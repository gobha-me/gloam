// SPEC 15 (M0), gloam#8 — the game core: key mapping, both pumps, the recording.
//
// AGENTS.md: "Test how code fails, not just that it produces the right output."
// The failures this file owns are the ones a DRIVER can produce and the sim
// cannot see: an action committed twice in one tick, a creep step that costs
// one tick instead of two, a session file that fails its own load gate, a
// mid-session pump toggle that loses the recording, and a replay of the log
// disagreeing with the session that wrote it. The last is the load-bearing
// property of the whole module: gloam_replay must reproduce a live session
// byte-for-byte or the M0 gate's bug reports arrive as artifacts that lie.

#include <catch2/catch_all.hpp>

#include <cstdint>
#include <vector>

#include "game.hpp"
#include "gloam/audio.hpp"
#include "gloam/replay.hpp"
#include "gloam/world.hpp"
#include "scene.hpp"

using namespace gloam;

namespace {

/// The RT script, one place for both the muted and voiced runs. Keys land at
/// fixed tick counts; keys inside a creep lockout are dropped BY DESIGN, which
/// is deterministic, so the two runs exercise identical pump behaviour.
auto scripted_rt_session(game::Core& core) -> void {
  for (std::uint32_t t = 0; t < 40; ++t) {
    switch (t) {
      case 1: core.handle_key(game::Key::LampUp); break;
      case 2: core.handle_key(game::Key::LampUp); break;
      case 3: core.handle_key(game::Key::LampUp); break;    // lamp 3: the corridor reads
      case 6: core.handle_key(game::Key::Forward); break;   // step toward the crossing
      case 9: core.handle_key(game::Key::CreepToggle); break;
      case 11: core.handle_key(game::Key::Back); break;     // a creep step, cost 2
      case 12: core.handle_key(game::Key::Forward); break;  // dropped: lockout
      case 16: core.handle_key(game::Key::TurnLeft); break;
      case 18: core.handle_key(game::Key::TurnRight); break;
      case 22: core.handle_key(game::Key::LampDown); break;
      case 24: core.handle_key(game::Key::LampDown); break;
      case 26: core.handle_key(game::Key::LampDown); break;  // doused again
      case 30: core.handle_key(game::Key::Wait); break;
      case 33: core.handle_key(game::Key::CreepToggle); break;
      default: break;
    }
    core.tick();
  }
}

/// The strongest check this file has: assemble the log into a real .gloam
/// image, pass it through the load gate a mailed-in bug report would face,
/// replay it into a fresh scene world, and demand the live session's hash.
auto replays_to_live_hash(const game::Core::Outcome& outcome,
                          const std::vector<replay::Record>& log) -> void {
  replay::Header header{};
  header.seed = outcome.seed;
  header.ruleset_hash = outcome.expect.ruleset_hash;
  header.pack_hash = outcome.expect.pack_hash;
  header.final_world_hash = outcome.world_hash;

  std::vector<std::byte> image(replay::image_bytes(static_cast<std::uint32_t>(log.size())));
  REQUIRE(replay::assemble(header, log, image));
  CHECK(replay::verify(image));

  replay::Header loaded{};
  std::vector<replay::Record> records(log.size());
  REQUIRE(replay::load(image, outcome.expect, loaded, records));
  CHECK(loaded.final_world_hash == outcome.world_hash);

  auto fresh = scene::m0_world(scene::kM0Seed);
  play(fresh, records, kDefaultTuning);
  CHECK(world_hash(fresh) == outcome.world_hash);
}

}  // namespace

TEST_CASE("map_key resolves relative intent against the four facings", "[gamecore][mapping]") {
  auto w = scene::m0_world();
  for (std::uint16_t f = 0; f < kDirCount; ++f) {
    w.facing = static_cast<Dir>(f);
    CHECK(game::map_key(game::Key::Forward, w) == game::Input{replay::Event::Step, f});
    CHECK(game::map_key(game::Key::Back, w) ==
          game::Input{replay::Event::Step, static_cast<std::uint16_t>((f + 2) % kDirCount)});
    CHECK(game::map_key(game::Key::TurnLeft, w) ==
          game::Input{replay::Event::Turn, static_cast<std::uint16_t>((f + 3) % kDirCount)});
    CHECK(game::map_key(game::Key::TurnRight, w) ==
          game::Input{replay::Event::Turn, static_cast<std::uint16_t>((f + 1) % kDirCount)});
  }
}

TEST_CASE("map_key clamps the lamp, flips creep, and refuses the unbound", "[gamecore][mapping]") {
  auto w = scene::m0_world();

  w.lamp_level = kLampLevelMin;
  CHECK(game::map_key(game::Key::LampDown, w) ==
        game::Input{replay::Event::Lamp, static_cast<std::uint16_t>(kLampLevelMin)});
  w.lamp_level = kLampLevelMax;
  CHECK(game::map_key(game::Key::LampUp, w) ==
        game::Input{replay::Event::Lamp, static_cast<std::uint16_t>(kLampLevelMax)});
  w.lamp_level = 3;
  CHECK(game::map_key(game::Key::LampUp, w) == game::Input{replay::Event::Lamp, 4});
  CHECK(game::map_key(game::Key::LampDown, w) == game::Input{replay::Event::Lamp, 2});

  w.creeping = false;
  CHECK(game::map_key(game::Key::CreepToggle, w) == game::Input{replay::Event::Creep, 1});
  w.creeping = true;
  CHECK(game::map_key(game::Key::CreepToggle, w) == game::Input{replay::Event::Creep, 0});

  CHECK(game::map_key(game::Key::Wait, w) == game::Input{replay::Event::Wait, 0});
  CHECK_FALSE(game::map_key(game::Key::Unbound, w).has_value());
}

TEST_CASE("the real-time pump commits at most one action per tick, latest-wins",
          "[gamecore][realtime]") {
  game::Core core{scene::m0_world(), game::PumpMode::RealTime};

  REQUIRE(core.handle_key(game::Key::Forward));
  REQUIRE(core.handle_key(game::Key::TurnLeft));  // overwrites: intent is replaceable
  core.tick();

  REQUIRE(core.log().size() == 1);
  CHECK(core.log().back().event == replay::Event::Turn);
  CHECK(core.log().back().tick == 0);  // recorded against the tick it landed on
  CHECK(core.world().tick == 1);
}

TEST_CASE("a creep step locks the real-time pump for exactly its cost", "[gamecore][realtime]") {
  game::Core core{scene::m0_world(), game::PumpMode::RealTime};

  core.handle_key(game::Key::CreepToggle);
  core.tick();  // Creep commits, cost 1: tick 1
  core.handle_key(game::Key::Forward);
  core.tick();  // the creep step commits, cost 2: tick 2 is its first

  CHECK(core.handle_key(game::Key::Wait) == false);  // lockout: dropped, not banked
  core.tick();                                       // tick 3: still occupied, world walks on
  CHECK(core.world().tick == 3);

  CHECK(core.handle_key(game::Key::Wait) == true);  // the lockout has lifted
  core.tick();                                      // tick 4: the Wait commits

  REQUIRE(core.log().size() == 3);
  CHECK(core.log()[0].event == replay::Event::Creep);
  CHECK(core.log()[1].event == replay::Event::Step);
  CHECK(core.log()[2].event == replay::Event::Wait);
  CHECK(core.log()[2].tick == 3);
  // Nothing recorded for the dropped key: the log is what the player DID.
  CHECK(std::count_if(core.log().begin(), core.log().end(), [](const replay::Record& r) {
    return r.event == replay::Event::Wait;
  }) == 1);
}

TEST_CASE("the step-timed pump pays an action's cost immediately, and tick() is a no-op",
          "[gamecore][steptimed]") {
  game::Core core{scene::m0_world(), game::PumpMode::StepTimed};

  REQUIRE(core.handle_key(game::Key::Wait));
  CHECK(core.world().tick == 1);

  REQUIRE(core.handle_key(game::Key::CreepToggle));
  CHECK(core.world().tick == 2);

  REQUIRE(core.handle_key(game::Key::Forward));  // a creep step: two ticks
  CHECK(core.world().tick == 4);

  core.tick();  // "no action, no ticks" — SPEC 5.2's step-timed row
  CHECK(core.world().tick == 4);

  REQUIRE(core.log().size() == 3);
  CHECK(core.log()[2].tick == 2);  // recorded at commit, before its two advances
}

TEST_CASE("the M0 scene's route is valid and the first crossing lands on schedule",
          "[gamecore][scene]") {
  auto w = scene::m0_world();
  REQUIRE(w.monsters.size() == 1);
  CHECK(valid_route(w.level, w.monsters[0].patrol.route, w.monsters[0].patrol.dwell));

  // scene.hpp derives this: monster_move_ticks = 2, no dwell paid on the
  // outbound leg, so the second step — onto the intersection — is tick 3, and
  // no jitter draw happens before it. Seed-independent by construction.
  for (std::uint32_t t = 1; t < scene::kM0FirstCrossingTick; ++t) {
    advance(w, kDefaultTuning);
    CHECK(w.monsters[0].at != scene::kM0Intersection);
  }
  advance(w, kDefaultTuning);
  CHECK(w.tick == scene::kM0FirstCrossingTick);
  CHECK(w.monsters[0].at == scene::kM0Intersection);

  // And it does not linger: the crossing is a glimpse, not a posting.
  for (std::uint32_t t = 0; t < static_cast<std::uint32_t>(kDefaultTuning.monster_move_ticks);
       ++t) {
    advance(w, kDefaultTuning);
  }
  CHECK(w.monsters[0].at != scene::kM0Intersection);
}

TEST_CASE("a recorded session replays to the live hash, muted or voiced", "[gamecore][replay]") {
  game::Core muted{scene::m0_world(), game::PumpMode::RealTime};
  scripted_rt_session(muted);
  const auto muted_outcome = muted.finish();

  audio::RecordingSink<> sink;
  game::Core voiced{scene::m0_world(), game::PumpMode::RealTime, &sink};
  scripted_rt_session(voiced);
  const auto voiced_outcome = voiced.finish();

  // SPEC 19 step 9's identity, at the driver level: the sink changes what is
  // heard, never what is hashed or recorded.
  CHECK(muted.log() == voiced.log());
  CHECK(muted_outcome == voiced_outcome);

  replays_to_live_hash(muted_outcome, muted.log());
}

TEST_CASE("a step-timed session replays to the live hash", "[gamecore][replay]") {
  game::Core core{scene::m0_world(), game::PumpMode::StepTimed};
  for (const auto key :
       {game::Key::LampUp, game::Key::LampUp, game::Key::LampUp, game::Key::Forward,
        game::Key::CreepToggle, game::Key::Back, game::Key::TurnLeft, game::Key::Wait,
        game::Key::TurnRight, game::Key::CreepToggle, game::Key::LampDown}) {
    REQUIRE(core.handle_key(key));
  }
  const auto outcome = core.finish();
  replays_to_live_hash(outcome, core.log());
}

TEST_CASE("set_mode mid-session discards only the uncommitted and keeps the log valid",
          "[gamecore][toggle]") {
  game::Core core{scene::m0_world(), game::PumpMode::RealTime};

  core.handle_key(game::Key::LampUp);
  core.tick();  // one committed record

  core.handle_key(game::Key::Forward);  // pending, never committed
  core.set_mode(game::PumpMode::StepTimed);
  CHECK(core.mode() == game::PumpMode::StepTimed);
  CHECK(core.log().size() == 1);  // the pending action did not reach the file

  REQUIRE(core.handle_key(game::Key::Wait));  // commits immediately, step-timed
  CHECK(core.world().tick == 2);

  core.set_mode(game::PumpMode::RealTime);
  core.tick();  // no pending resurfaced across the boundary
  CHECK(core.world().tick == 3);
  CHECK(core.log().size() == 2);

  const auto outcome = core.finish();
  replays_to_live_hash(outcome, core.log());
}

TEST_CASE("finish() seals the session exactly once, even an empty one", "[gamecore][seal]") {
  game::Core empty{scene::m0_world(), game::PumpMode::RealTime};
  const auto outcome = empty.finish();

  // An untouched session finishes as exactly one Wait — replay::ZeroRecords is
  // refused at load by design, so the seal is what makes "did nothing" a file.
  REQUIRE(empty.log().size() == 1);
  CHECK(empty.log()[0].event == replay::Event::Wait);
  replays_to_live_hash(outcome, empty.log());

  CHECK_FALSE(empty.handle_key(game::Key::Wait));
  const auto tick_at_seal = empty.world().tick;
  empty.tick();
  CHECK(empty.world().tick == tick_at_seal);
  CHECK(empty.finish() == outcome);  // idempotent

  // A session ending mid-lockout seals cleanly too: the Wait lands on the
  // current tick whatever the pump still owes.
  game::Core mid_creep{scene::m0_world(), game::PumpMode::RealTime};
  mid_creep.handle_key(game::Key::CreepToggle);
  mid_creep.tick();
  mid_creep.handle_key(game::Key::Forward);
  mid_creep.tick();  // the creep step commits; its second tick is still owed
  const auto creep_outcome = mid_creep.finish();
  replays_to_live_hash(creep_outcome, mid_creep.log());
}
