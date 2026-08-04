/// SPEC §9.2, §19 step 9 — the wall between the simulation and the sink.
///
/// Step 9's acceptance criterion is "`--mute` and unmuted runs produce identical
/// replays", and this file is where that is asserted in process. `cmake/
/// check_audio_mute.cmake` asserts it across two processes; the two are not
/// redundant for the reason the pack and replay gates already give — a shared
/// allocator and address space can hide a difference that a fresh process would
/// expose.
///
/// THE WAY THIS FILE ROTS, AND THE GUARD AGAINST IT.
///
/// "The two runs are identical" passes trivially if audio never does anything.
/// A sink that is silently never called, a footfall condition that is always
/// false, a sting keyed on a `Tell` that never fires — each makes this file
/// green and the subsystem dead. So every identity case here also REQUIREs that
/// the unmuted run actually pushed commands. An identity proved over silence is
/// not evidence.

#include <cstdint>
#include <string>
#include <vector>

#include <catch2/catch_all.hpp>

#include "gloam/audio.hpp"
#include "gloam/gloam.hpp"
#include "gloam/world.hpp"

using namespace gloam;

namespace {

/// A sink that answers "what did the simulation say, and in what order".
class Log final : public audio::Sink {
 public:
  auto play(audio::SoundId sound, audio::Gain gain, audio::Pan pan) -> void override {
    entries.push_back(Entry{tick(), sound, gain, pan});
  }

  struct Entry {
    std::uint32_t tick{};
    audio::SoundId sound{};
    audio::Gain gain{};
    audio::Pan pan{};
  };

  [[nodiscard]] auto count(audio::SoundId sound) const -> std::size_t {
    std::size_t n = 0;
    for (const auto& e : entries) {
      if (e.sound == sound) ++n;
    }
    return n;
  }

  std::vector<Entry> entries{};
};

/// A sink that does everything a sink must never be able to affect the
/// simulation by doing.
///
/// If §9.2's "audio -> sim is nothing" held only by convention, THIS is what
/// would break it: it allocates on every call, grows unboundedly, and does real
/// work. The world hash must not notice.
class Pathological final : public audio::Sink {
 public:
  auto play(audio::SoundId sound, audio::Gain gain, audio::Pan pan) -> void override {
    ballast.emplace_back(static_cast<std::size_t>(gain % 64) + 1, 'x');
    scratch.resize(scratch.size() + static_cast<std::size_t>(pan < 0 ? -pan : pan) + 1, 0);
    churn += static_cast<std::uint64_t>(sound) * scratch.size();
    calls += 1;
  }

  std::vector<std::string> ballast{};
  std::vector<std::uint8_t> scratch{};
  std::uint64_t churn{0};
  std::uint64_t calls{0};
};

/// M0's corridor with an alcove and two monsters — the same shape
/// `src/bin/replay.cpp` and `test/13replay/` build, and duplicated here for the
/// reason those two already state: a scenario read from one shared definition
/// can drift into agreeing with itself about the wrong thing.
[[nodiscard]] auto corridor_world() -> World {
  Level level{9, 3};
  level.carve(Coord{0, 1}, Dir::East, 9);
  level.carve(Coord{4, 1}, Dir::North, 2);

  std::vector<Monster> monsters{
      Monster{Coord{7, 1}, MonsterKind{Acuity::Normal, false}, {}},
      Monster{Coord{4, 0}, MonsterKind{Acuity::Keen, false}, {}},
  };

  auto w = make_world(0xDEADBEEF12345678ULL, std::move(level), std::move(monsters));
  w.party = Coord{1, 1};
  w.armour = Armour::Leather;
  return w;
}

[[nodiscard]] auto scripted_session() -> std::vector<replay::Record> {
  using replay::Event;
  return {
      {0, Event::Lamp, 3},  {1, Event::Step, 1}, {2, Event::Step, 1}, {3, Event::Creep, 1},
      {3, Event::Turn, 2},  {5, Event::Wait, 0}, {8, Event::Lamp, 0}, {9, Event::Step, 3},
  };
}

/// A lit party stepping toward a monster with clear line of sight — the setup
/// §6.1 requires before SEARCHING -> HUNTING can fire at all.
[[nodiscard]] auto sighting_world() -> World {
  Level level{10, 3};
  level.carve(Coord{0, 1}, Dir::East, 10);
  std::vector<Monster> monsters{
      Monster{Coord{6, 1}, MonsterKind{Acuity::Keen, false}, {}},
  };
  auto w = make_world(0x5EEDULL, std::move(level), std::move(monsters));
  w.party = Coord{2, 1};
  w.lamp_level = kLampLevelMax;
  w.armour = Armour::Plate;  // loud, so the monster hears as well as sees
  return w;
}

}  // namespace

// ── the acceptance criterion ────────────────────────────────────────────────

TEST_CASE("a muted and an unmuted run reach the same world hash", "[audio][determinism]") {
  const Tuning& t = kDefaultTuning;
  const auto records = scripted_session();

  auto muted = corridor_world();
  play(muted, records, t, nullptr);

  auto unmuted = corridor_world();
  audio::RecordingSink<> sink;
  play(unmuted, records, t, &sink);

  // THE GUARD AGAINST A VACUOUS PASS. Without this line the case below is
  // satisfied by a sink that was never called.
  REQUIRE(sink.ring().pushed() > 0);

  CHECK(world_hash(muted) == world_hash(unmuted));
  CHECK(muted.tick == unmuted.tick);
  CHECK(muted.party == unmuted.party);
  CHECK(muted.monsters == unmuted.monsters);
}

TEST_CASE("a sink that allocates and thrashes still cannot move the world hash",
          "[audio][determinism]") {
  // The by-construction argument in world.hpp says `play` returns void and takes
  // scalars by value, so nothing can be read back. This is that argument checked
  // empirically against a sink built to violate it if it were violable.
  const Tuning& t = kDefaultTuning;
  const auto records = scripted_session();

  auto quiet = corridor_world();
  play(quiet, records, t, nullptr);

  auto noisy = corridor_world();
  Pathological hostile;
  play(noisy, records, t, &hostile);

  REQUIRE(hostile.calls > 0);
  CHECK(world_hash(quiet) == world_hash(noisy));
}

TEST_CASE("the identity holds over a full budget window, with monsters transitioning",
          "[audio][determinism]") {
  // A divergence that only appears after an awareness transition would slip past
  // a short session. This runs the §11 window with a party that is seen, heard
  // and then doused.
  const Tuning& t = kDefaultTuning;
  constexpr std::uint32_t kTicks = 1'000;

  auto muted = sighting_world();
  auto unmuted = sighting_world();
  audio::RecordingSink<> sink;

  for (std::uint32_t i = 0; i < kTicks; ++i) {
    // Step back and forth so `pending_noise` is set on roughly half the ticks.
    const auto dir = (i % 2 == 0) ? Dir::East : Dir::West;
    apply(muted, replay::Event::Step, static_cast<std::uint16_t>(dir), t);
    apply(unmuted, replay::Event::Step, static_cast<std::uint16_t>(dir), t);
    advance(muted, t, nullptr);
    advance(unmuted, t, &sink);
  }

  REQUIRE(sink.ring().pushed() > 0);
  CHECK(world_hash(muted) == world_hash(unmuted));
}

// ── the sting: §6.1's reserved tell ─────────────────────────────────────────

TEST_CASE("SEARCHING -> HUNTING emits exactly one sting", "[audio][sting]") {
  const Tuning& t = kDefaultTuning;
  auto w = sighting_world();
  Log log;

  // Walk toward the monster until it hunts. §6.1 escalates at most one state per
  // tick, so this needs a handful of ticks.
  for (int i = 0; i < 40 && w.monsters[0].mind.state != Awareness::Hunting; ++i) {
    apply(w, replay::Event::Step, static_cast<std::uint16_t>(Dir::East), t);
    advance(w, t, &log);
  }

  REQUIRE(w.monsters[0].mind.state == Awareness::Hunting);
  CHECK(log.count(audio::SoundId::HuntingSting) == 1);

  // AND IT MUST BE AUDIBLE. Counting the voice is not enough: `advance` seeds
  // the shared sting field with `kStingEmission`, and a field seeded too quietly
  // still produces exactly one command — at zero gain. That bug would present as
  // "the sting sometimes does not play", and every count-based assertion in this
  // file would stay green through it.
  //
  // §6.1 only reaches this transition with clear line of sight at close range,
  // so an inaudible sting here is always wrong.
  for (const auto& e : log.entries) {
    if (e.sound != audio::SoundId::HuntingSting) continue;
    INFO("sting gain " << e.gain << ", pan " << e.pan);
    CHECK(e.gain > audio::kGainSilent);
  }
}

TEST_CASE("the sting's gain is exactly what the propagation says it is", "[audio][sting]") {
  // PINS THE VALUE, NOT ONLY ITS SIGN.
  //
  // `advance` builds ONE field per tick, rooted at the party and seeded with
  // `kStingEmission`, and reads every stinging monster out of it. Seed that
  // field too quietly and the field truncates: the sting still fires, still
  // counts as one voice, and arrives quieter than it should — or silent, at
  // range. Every count-based assertion in this file survives that, and so does
  // "gain > silent" whenever the monster happens to be close.
  //
  // Measured: seeding at kStingEmission/8 left this file green until this case
  // existed.
  //
  // The expected value is computed through `mix_for`, which propagates from the
  // MONSTER and reads at the PARTY — the opposite ends from what `advance` does.
  // So this pins the reciprocity in the simulation as well as the magnitude.
  const Tuning& t = kDefaultTuning;

  // Far enough apart that a truncated field is visibly wrong, close enough that
  // §6.1's sighting condition still holds: Keen sight reaches 8 cells.
  constexpr std::int32_t kSeparation = 7;
  const Coord party{1, 1};
  const Coord monster{party.x + kSeparation, 1};

  Level level{16, 3};
  level.carve(Coord{0, 1}, Dir::East, 16);

  Perception searching{};
  searching.state = Awareness::Searching;
  searching.has_last_known = true;
  searching.last_known = party;
  std::vector<Monster> monsters{Monster{monster, MonsterKind{Acuity::Keen, false}, searching}};

  auto w = make_world(0xACE0ULL, std::move(level), std::move(monsters));
  w.party = party;
  w.facing = Dir::East;
  w.lamp_level = kLampLevelMax;

  Log log;
  advance(w, t, &log);

  REQUIRE(w.monsters[0].mind.state == Awareness::Hunting);
  REQUIRE(log.count(audio::SoundId::HuntingSting) == 1);

  // READ AT THE CELL IT ARRIVED ON, NOT THE ONE IT STARTED FROM. Since gloam#32
  // the tick that carries the sting also carries a step — §6.1's "gait changes,
  // DIRECT PURSUIT" is a translation, and `advance` emits from the cell the
  // monster reached, which is the cell a listener would place the sound at.
  // Recomputed rather than relaxed to an inequality: the exactness is the point
  // of the case, and an inequality here would pass for a mix computed from the
  // wrong cell entirely.
  const auto arrived = w.monsters[0].at;
  CHECK(arrived != monster);  // the pursuit happened, so the distinction is real
  const auto expected =
      audio::mix_for(w.level, party, Dir::East, arrived, audio::kStingEmission, t);
  REQUIRE(expected.gain > audio::kGainSilent);  // the case is not vacuous

  for (const auto& e : log.entries) {
    if (e.sound != audio::SoundId::HuntingSting) continue;
    CHECK(e.gain == expected.gain);
    CHECK(e.pan == expected.pan);
  }
}

TEST_CASE("a monster that moves sounds once; one that does not is silent",
          "[audio][footfall]") {
  // §9 names monster footfalls FIRST among the things the player must hear, and
  // until §6.4's patrols existed there was nothing to sound.
  const auto t = kDefaultTuning;

  Level level{9, 3};
  level.carve(Coord{0, 1}, Dir::East, 9);

  Monster walker{};
  walker.at = Coord{5, 1};
  walker.kind = MonsterKind{Acuity::Dull, false};
  walker.patrol.route = {Coord{5, 1}, Coord{6, 1}, Coord{7, 1}};
  walker.patrol.dwell = {0, 0, 0};

  Monster sitter{};
  sitter.at = Coord{3, 1};
  sitter.kind = MonsterKind{Acuity::Dull, false};  // no route at all

  auto w = make_world(0xF007ULL, std::move(level), {walker, sitter});
  w.party = Coord{1, 1};
  w.lamp_level = 0;

  Log log;
  for (int i = 0; i < 6; ++i) advance(w, t, &log);

  // The walker covered the route; the sitter never sounded. If footfalls were
  // keyed on anything but "this monster changed cell" — a tick counter, an
  // awareness state, the mere presence of a route — the sitter would show up.
  CHECK(log.count(audio::SoundId::MonsterFootfall) == 3);
  CHECK(w.monsters[1].at == Coord{3, 1});
}

TEST_CASE("the monster footfall's gain is exactly what the propagation says it is",
          "[audio][footfall]") {
  // THE CASE THAT CATCHES REUSING THE STING FIELD, and nothing else does.
  //
  // `advance` builds a SECOND lazy field per tick, seeded at
  // `kMonsterFootfallEmission` (14) rather than `kStingEmission` (90). Reading
  // the 90-seeded field here would still emit exactly one footfall, still count
  // as one voice, and still be louder than silence — every count-based
  // assertion in this file survives the mistake. What it would NOT survive is
  // the number: `gain_from_loudness` clamps to unity once the arriving loudness
  // meets the emission, so a 90-seeded read reports FULL VOLUME for a monster
  // seven cells away that should be most of the way to inaudible.
  //
  // As with the sting, the expected value is computed by `mix_for`, which
  // propagates from the MONSTER and reads at the PARTY — the opposite ends from
  // what `advance` does — so this pins reciprocity as well as magnitude.
  const auto t = kDefaultTuning;

  constexpr std::int32_t kSeparation = 6;
  const Coord party{1, 1};
  const Coord from{party.x + kSeparation, 1};
  const Coord to{from.x - 1, 1};  // it steps TOWARD the party

  Level level{16, 3};
  level.carve(Coord{0, 1}, Dir::East, 16);

  Monster walker{};
  walker.at = from;
  walker.kind = MonsterKind{Acuity::Dull, false};
  walker.patrol.route = {from, to};
  walker.patrol.dwell = {0, 0};
  walker.patrol.reversed = false;

  auto w = make_world(0xB007ULL, std::move(level), {walker});
  w.party = party;
  w.facing = Dir::East;
  w.lamp_level = 0;

  Log log;
  advance(w, t, &log);

  REQUIRE(w.monsters[0].at == to);
  REQUIRE(log.count(audio::SoundId::MonsterFootfall) == 1);

  // Read at the cell it ARRIVED at, which is where the foot landed.
  const auto expected =
      audio::mix_for(w.level, party, Dir::East, to, audio::kMonsterFootfallEmission, t);
  REQUIRE(expected.gain > audio::kGainSilent);   // not vacuous...
  REQUIRE(expected.gain < audio::kGainUnity);    // ...and not clamped, which is the whole point

  for (const auto& e : log.entries) {
    if (e.sound != audio::SoundId::MonsterFootfall) continue;
    CHECK(e.gain == expected.gain);
    CHECK(e.pan == expected.pan);
  }
}

TEST_CASE("the party sounds first, then monsters in roster order", "[audio][order]") {
  // Emission order within a tick is a RULE, not an accident, and a mixer that
  // ever needs to resolve two voices deterministically will depend on it.
  //
  // WHAT THIS CASE CANNOT REACH, stated so the gap is on the record rather than
  // discovered: `advance` also orders a monster's own footfall BEFORE its own
  // sting, and no world can currently exercise that. `Tell::GaitChanges` only
  // arrives from SEARCHING, and a monster at SEARCHING holds position
  // (gloam#32) — so within one tick a monster either steps or stings, never
  // both. When #32 lands, that pairing becomes constructible and belongs here.
  const auto t = kDefaultTuning;

  Level level{14, 3};
  level.carve(Coord{0, 1}, Dir::East, 14);

  Monster near{};
  near.at = Coord{5, 1};
  near.kind = MonsterKind{Acuity::Dull, false};
  near.patrol.route = {Coord{5, 1}, Coord{6, 1}};
  near.patrol.dwell = {0, 0};

  Monster far{};
  far.at = Coord{10, 1};
  far.kind = MonsterKind{Acuity::Dull, false};
  far.patrol.route = {Coord{10, 1}, Coord{11, 1}};
  far.patrol.dwell = {0, 0};

  auto w = make_world(0x0DDULL, std::move(level), {near, far});
  w.party = Coord{1, 1};
  w.facing = Dir::East;
  w.lamp_level = 0;
  w.armour = Armour::Plate;

  Log log;
  apply(w, replay::Event::Step, static_cast<std::uint16_t>(Dir::East), t);
  advance(w, t, &log);

  REQUIRE(log.entries.size() == 3);
  CHECK(log.entries[0].sound == audio::SoundId::PartyFootfall);
  CHECK(log.entries[1].sound == audio::SoundId::MonsterFootfall);
  CHECK(log.entries[2].sound == audio::SoundId::MonsterFootfall);

  // Roster order, and it is the GAINS that prove it rather than the ids: the
  // near monster is five cells closer, so a reversed roster would swap two
  // otherwise identical-looking entries.
  CHECK(log.entries[1].gain > log.entries[2].gain);
}

TEST_CASE("patrols run identically muted and unmuted", "[audio][determinism]") {
  // §19 step 9's criterion, now that something in the tick draws from an RNG.
  // The draw in `patrol_step` is deliberately NOT behind `voices != nullptr`;
  // if it ever moves there, the two worlds below diverge within a few ticks and
  // this is what says so.
  const auto t = kDefaultTuning;

  auto build = [] {
    Level level{9, 5};
    level.carve(Coord{1, 2}, Dir::East, 5);
    level.carve(Coord{3, 2}, Dir::North, 3);
    level.carve(Coord{3, 2}, Dir::South, 3);

    Monster m{};
    m.at = Coord{3, 0};
    m.kind = MonsterKind{Acuity::Normal, false};
    m.patrol.route = {Coord{3, 0}, Coord{3, 1}, Coord{3, 2}, Coord{3, 3}, Coord{3, 4}};
    m.patrol.dwell = {2, 0, 1, 0, 2};  // authored pauses, so §6.4's jitter draws

    auto w = make_world(0x9A7401ULL, std::move(level), {m});
    w.party = Coord{1, 2};
    w.armour = Armour::Plate;
    return w;
  };

  auto muted = build();
  auto voiced = build();

  Log log;
  for (int i = 0; i < 200; ++i) {
    advance(muted, t, nullptr);
    advance(voiced, t, &log);
  }

  REQUIRE(log.count(audio::SoundId::MonsterFootfall) > 0);  // not proved over silence
  CHECK(world_hash(muted) == world_hash(voiced));
  CHECK(muted.rng_state == voiced.rng_state);
}

TEST_CASE("LOST_TRACK -> HUNTING is SILENT, and that silence is the tell",
          "[audio][sting]") {
  // GLOAM ISSUE #12's DECISION, MADE AUDIBLE.
  //
  // `perception.hpp` reserves the sting for a FIRST sighting: it means "found
  // you", while its absence on SnapsBack means "never lost you" — the honest
  // report from a monster that was already searching the right place.
  //
  // Implementing this as `if (m.mind.state == Awareness::Hunting)` passes every
  // other test in the entire suite and fails here. That is the whole reason this
  // case exists.
  const Tuning& t = kDefaultTuning;
  Perception mind{};
  mind.state = Awareness::LostTrack;
  mind.has_last_known = true;
  mind.last_known = Coord{3, 1};

  Level level{10, 3};
  level.carve(Coord{0, 1}, Dir::East, 10);
  std::vector<Monster> monsters{Monster{Coord{6, 1}, MonsterKind{Acuity::Keen, false}, mind}};
  auto w = make_world(0x1234ULL, std::move(level), std::move(monsters));
  w.party = Coord{5, 1};
  w.lamp_level = kLampLevelMax;

  Log log;
  advance(w, t, &log);

  REQUIRE(w.monsters[0].mind.state == Awareness::Hunting);  // it did snap back
  CHECK(log.count(audio::SoundId::HuntingSting) == 0);      // and said nothing
}

TEST_CASE("no other tell makes a sound", "[audio][sting]") {
  // A `default:` that fired on every transition would make the sting meaningless
  // — which is exactly what test/04perception guards against on the perception
  // side, phrased there as "otherwise the audio sting stops meaning anything".
  const Tuning& t = kDefaultTuning;

  // UNLIT AND FAR IS NO LONGER ENOUGH, and the reason is gloam#32. A SEARCHING
  // monster used to hold position, so "escalates on noise alone" was a state it
  // could not walk out of. Now it walks to the last known position — and §6.3
  // makes even a doused party visible at an ADJACENT cell, so an unlit party at
  // the far end of an open corridor is a party the monster eventually finds.
  // The old arrangement stopped testing "no other tell makes a sound" and
  // started testing "the pursuit is slower than twenty ticks".
  //
  // So: audible and genuinely UNREACHABLE. A closed door is impassable to a body
  // and still conducts sound (§6.2 attenuates it, §12 keeps one graph with two
  // readings), and `Edge::attenuation_override` is exactly the authored value
  // SCHEMAS.md provides for "an authored test level can pin an unusual value".
  // The monster hears you through the door, escalates, walks up to it, and can
  // never see you.
  Level level{10, 3};
  level.carve(Coord{0, 1}, Dir::East, 10);
  level.link(Coord{4, 1}, Dir::East,
             Edge{EdgeKind::Door, EdgeState::Closed, 0, /*attenuation_override=*/4});

  std::vector<Monster> monsters{Monster{Coord{6, 1}, MonsterKind{Acuity::Keen, false}, {}}};
  auto w = make_world(0x5EEDULL, std::move(level), std::move(monsters));
  w.party = Coord{2, 1};
  w.lamp_level = 0;
  w.armour = Armour::Plate;  // loud, so the door is the only thing stopping it
  Log log;

  for (int i = 0; i < 10; ++i) {
    apply(w, replay::Event::Step, static_cast<std::uint16_t>(Dir::East), t);
    advance(w, t, &log);
    apply(w, replay::Event::Step, static_cast<std::uint16_t>(Dir::West), t);
    advance(w, t, &log);
  }

  // It noticed something, and it never claimed to have found anyone.
  CHECK(w.monsters[0].mind.state != Awareness::Unaware);
  CHECK(log.count(audio::SoundId::HuntingSting) == 0);
  // And it never got through: an unreachable target is a monster that HOLDS, not
  // one that drifts toward the wall between you. That is the failure mode
  // gloam#32 rejected greedy stepping to avoid, asserted where it would show.
  CHECK(w.monsters[0].at == Coord{6, 1});
}

// ── the footfall: inheriting apply()'s rules rather than restating them ─────

TEST_CASE("a silent tick emits nothing at all", "[audio][footfall]") {
  const Tuning& t = kDefaultTuning;
  auto w = corridor_world();

  // DOUSED, which this case did not have to be until gloam#32. `corridor_world`
  // starts at the default lamp level, and the monster at (7,1) has clear line of
  // sight to a lit party six cells away — inside a Normal monster's sight. It
  // has ALWAYS been escalating here; it simply could not act on it, so the tick
  // looked silent. Now it walks toward you and §9's first-named sound is its
  // footfall, which is the subsystem working.
  //
  // Dousing restores what the case is actually about: no input, no perception,
  // no sound. §6.3's pillar is what makes that reachable in one line.
  w.lamp_level = 0;

  Log log;
  advance(w, t, &log);
  advance(w, t, &log);
  CHECK(log.entries.empty());
  // The guard that keeps the line above meaning something: nothing perceived
  // anything, so silence is the absence of a cause rather than a lost voice.
  for (const auto& m : w.monsters) CHECK(m.mind.state == Awareness::Unaware);
}

TEST_CASE("a step into an impassable edge is not a footfall", "[audio][footfall]") {
  // `apply` refuses the move AND emits no noise — "you did not take a step".
  // Deriving the sound from `pending_noise` rather than from the Step event is
  // what makes the sink inherit that rule instead of restating it somewhere it
  // could drift.
  const Tuning& t = kDefaultTuning;
  auto w = corridor_world();
  const auto before = w.party;
  Log log;

  apply(w, replay::Event::Step, static_cast<std::uint16_t>(Dir::North), t);  // into rock
  advance(w, t, &log);

  CHECK(w.party == before);
  CHECK(log.count(audio::SoundId::PartyFootfall) == 0);
}

TEST_CASE("two steps in one tick are ONE footfall, at the louder of the two",
          "[audio][footfall]") {
  // `apply` takes the MAX rather than the sum, because two footfalls 100 ms
  // apart are one sound event and not a louder one. The mix must not turn that
  // back into two voices.
  const Tuning& t = kDefaultTuning;
  auto w = corridor_world();
  Log log;

  apply(w, replay::Event::Step, static_cast<std::uint16_t>(Dir::East), t);
  apply(w, replay::Event::Step, static_cast<std::uint16_t>(Dir::East), t);
  advance(w, t, &log);

  CHECK(log.count(audio::SoundId::PartyFootfall) == 1);
}

TEST_CASE("the party's own footfall is unattenuated and centred", "[audio][footfall]") {
  const Tuning& t = kDefaultTuning;
  auto w = corridor_world();
  Log log;

  apply(w, replay::Event::Step, static_cast<std::uint16_t>(Dir::East), t);
  advance(w, t, &log);

  REQUIRE(log.entries.size() == 1);
  CHECK(log.entries[0].sound == audio::SoundId::PartyFootfall);
  CHECK(log.entries[0].gain == audio::kGainUnity);
  CHECK(log.entries[0].pan == audio::kPanCentre);
}

TEST_CASE("creeping is quieter than walking, but no quieter at the player's own ear",
          "[audio][footfall]") {
  // §6.2 halves the EMISSION when creeping, and gain is normalised against the
  // emission — so your own footsteps sound the same to you either way, while
  // the monster hears half as much. That is correct rather than a bug: the
  // player's feedback is "I stepped", the stealth consequence is elsewhere. It
  // is pinned because the opposite behaviour would look equally plausible.
  const Tuning& t = kDefaultTuning;

  auto walking = corridor_world();
  Log loud;
  apply(walking, replay::Event::Step, static_cast<std::uint16_t>(Dir::East), t);
  advance(walking, t, &loud);

  auto creeping = corridor_world();
  Log soft;
  apply(creeping, replay::Event::Creep, 1, t);
  apply(creeping, replay::Event::Step, static_cast<std::uint16_t>(Dir::East), t);
  advance(creeping, t, &soft);

  REQUIRE(loud.entries.size() == 1);
  REQUIRE(soft.entries.size() == 1);
  CHECK(soft.entries[0].gain == loud.entries[0].gain);
  // ...and the underlying emission really did halve.
  CHECK(step_noise(Armour::Leather, true, t) < step_noise(Armour::Leather, false, t));
}

// ── ordering, which is a drop priority ──────────────────────────────────────

TEST_CASE("the footfall is emitted before any sting", "[audio][order]") {
  // Under a saturated ring `try_push` refuses the NEWEST, so emission order
  // states a priority: the player's own action survives and the consequences are
  // what get dropped. Cause before consequence.
  const Tuning& t = kDefaultTuning;
  auto w = sighting_world();
  Log log;

  for (int i = 0; i < 40 && log.count(audio::SoundId::HuntingSting) == 0; ++i) {
    apply(w, replay::Event::Step, static_cast<std::uint16_t>(Dir::East), t);
    advance(w, t, &log);
  }

  REQUIRE(log.count(audio::SoundId::HuntingSting) == 1);
  std::size_t sting_index = 0;
  for (std::size_t i = 0; i < log.entries.size(); ++i) {
    if (log.entries[i].sound == audio::SoundId::HuntingSting) sting_index = i;
  }
  // The sting's own tick also carried a footfall, and that footfall came first.
  const auto sting_tick = log.entries[sting_index].tick;
  REQUIRE(sting_index > 0);
  CHECK(log.entries[sting_index - 1].tick == sting_tick);
  CHECK(log.entries[sting_index - 1].sound == audio::SoundId::PartyFootfall);
}

TEST_CASE("a command carries the tick being simulated, not the one after it",
          "[audio][order]") {
  // An off-by-one here would misattribute every latency measurement the device
  // takes, and would be invisible without an explicit check: `advance`
  // increments `w.tick` at its end, so reading the counter after the fact gives
  // the wrong answer.
  const Tuning& t = kDefaultTuning;
  auto w = corridor_world();
  Log log;

  CHECK(w.tick == 0);
  apply(w, replay::Event::Step, static_cast<std::uint16_t>(Dir::East), t);
  advance(w, t, &log);

  REQUIRE(log.entries.size() == 1);
  CHECK(log.entries[0].tick == 0);
  CHECK(w.tick == 1);
}
