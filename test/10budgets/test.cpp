// SPEC §11 and §13.4 — every budget as an assertion.
//
// "Every row of §11 is an assertion in CI." Most of the rows constrain code
// that does not exist yet, and they are here anyway: §19 step 4's acceptance
// criterion is that the assertions EXIST "even where the numbers are trivially
// met", so that the first commit which can exceed one finds a test already
// waiting for it rather than a code review.
//
// Rows marked PENDING below assert their own constant and name the milestone
// that makes them measurable. That is deliberate: a row that quietly vanished
// would be indistinguishable from a row that was never written.

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "gloam/assets.hpp"
#include "gloam/audio.hpp"
#include "gloam/budgets.hpp"
#include "gloam/emit.hpp"
#include "gloam/layer.hpp"
#include "gloam/lightfield.hpp"
#include "gloam/noise.hpp"
#include "gloam/pack.hpp"
#include "gloam/perception.hpp"
#include "gloam/plate.hpp"
#include "gloam/tuning.hpp"
#include "gloam/world.hpp"

using namespace gloam;

TEST_CASE("§4.2's slot inventory fits §11's residency cap", "[budget]") {
  CHECK(budget::resident_images_m0() == 71);
  CHECK(budget::resident_images_full() == 246);
  CHECK(budget::resident_images_full() <= budget::kMaxResidentImages);

  // The row that makes the table add up. See the note in budgets.hpp — the
  // naive sum is 74 and 252, and both are wrong.
  const int naive_m0 = budget::resident_images_m0() + budget::kTransitionSequences.m0;
  const int naive_full = budget::resident_images_full() + budget::kTransitionSequences.full;
  CHECK(naive_m0 == 74);
  CHECK(naive_full == 252);

  // Counting transitions would not blow the cap — 252 still fits under 256 —
  // which is exactly why this is worth a test rather than trust. The mistake is
  // survivable and therefore silent: it would report totals that disagree with
  // §4.2's stated 71 and 246, and it would spend 6 of the 10 remaining slots on
  // things that are not plates. The next person to ask "how much monster
  // breadth can we afford?" would get the wrong answer (§16's cut lever).
  CHECK(naive_full <= budget::kMaxResidentImages);
  CHECK(naive_m0 - budget::resident_images_m0() == budget::kTransitionSequences.m0);
  CHECK(naive_full - budget::resident_images_full() == budget::kTransitionSequences.full);

  // The headroom the art plan actually has.
  CHECK(budget::kMaxResidentImages - budget::resident_images_full() == 10);
}

TEST_CASE("§4.4 ships exactly one light field per lamp level", "[budget]") {
  // Six of them — one per lamp level, L0 opaque through L5 bright. If the lamp
  // ladder ever grows a level, this is what notices the plate set did not.
  CHECK(budget::kLightFields.m0 == kLampLevelCount);
  CHECK(budget::kLightFields.full == kLampLevelCount);
}

TEST_CASE("§11 byte budgets are declared and ordered sanely", "[budget]") {
  // An idle frame costs ZERO bytes (§4.6). Asserted as an equality rather than
  // a bound, because "small" is exactly the failure mode this budget exists to
  // rule out.
  CHECK(budget::kIdleFrameBytes == 0);

  CHECK(budget::kMaxAnimationFrameBytes == 400);
  CHECK(budget::kMaxRecompositionBytes == 2048);
  CHECK(budget::kMaxSustainedBytesPerSecond == 8192);

  // The ordering is itself a claim: an animation-only frame must be cheaper
  // than a full recomposition, or emit-on-change is not buying anything.
  CHECK(budget::kIdleFrameBytes < budget::kMaxAnimationFrameBytes);
  CHECK(budget::kMaxAnimationFrameBytes < budget::kMaxRecompositionBytes);

  // The idle row is now measured, not merely declared — see the §4.6 case below,
  // which runs a frame through gloam::emit::ByteSink. §13.4 wants the counter to
  // wrap the emit path so it reports what actually left the process rather than
  // what the compositor believed it produced, and ByteSink is that counter.
  //
  // PENDING M0: the animation, recomposition and sustained-p95 rows need a
  // compositor producing real placement lists to measure against — G-6, and
  // through it the §4 compositor, which is still blocked upstream (UPSTREAM.md).
}

TEST_CASE("§11 timing budgets are declared", "[budget]") {
  CHECK(budget::kMaxColdStartLocalMs == 800);
  CHECK(budget::kMaxColdStartThrottledMs == 12'000);
  CHECK(budget::kMaxComposeDiffEmitMs == 2);
  CHECK(budget::kMaxAudioLatencyMs == 20);
  CHECK(budget::kMaxColdStartPayloadBytes == 1'200'000);

  // PENDING M0: cold start needs the UPLOAD path — see the pack case below,
  //             which measures what exists (§19 step 5) and says plainly why
  //             that is not this row.
  // PENDING M0: compose+diff+emit needs the compositor (§19 step 6).
  // PENDING M2: the END-TO-END audio row needs a DEVICE. §19 step 9 landed the
  //             ring, not the RtAudio stream, and "tick -> first sample" cannot
  //             be measured without something producing samples. The part that
  //             is decidable without a device — whether §9.2's chosen buffer can
  //             fit inside the budget at all — is asserted in the case below
  //             rather than left as prose. Narrowing this marker instead of
  //             deleting it is the point: the row is not met yet.
}

TEST_CASE("§9.2's audio buffer choice fits §11's latency budget", "[budget]") {
  // Not the end-to-end row — that needs a device — but the half of it that is
  // fixed by §9.2's own numbers and would silently break the budget if either
  // moved. "One output stream. 48 kHz, float32, 256-frame buffer (~5.3 ms)."
  //
  // Triple-buffered worst case: a command that just missed the current period
  // waits one period to be picked up, one to be mixed and one to reach the
  // device. 3 x 5.33 ms = 16 ms, against a 20 ms budget — so the choice leaves
  // about 4 ms for everything else in the chain, and there is no room to grow
  // the buffer to 512 without blowing the row.
  constexpr int kSampleRateHz = 48'000;
  constexpr int kBufferFrames = 256;
  constexpr int kWorstCasePeriods = 3;

  const int period_us = kBufferFrames * 1'000'000 / kSampleRateHz;
  INFO("one period is " << period_us << " us");
  CHECK(period_us == 5'333);

  const int worst_case_ms = kWorstCasePeriods * period_us / 1000;
  INFO("worst case " << worst_case_ms << " ms against a budget of "
                     << budget::kMaxAudioLatencyMs << " ms");
  CHECK(worst_case_ms <= budget::kMaxAudioLatencyMs);

  // And the headroom is genuinely thin. Pinned so that doubling the buffer is a
  // decision taken here, against this row, rather than a tweak in src/bin/.
  CHECK(kWorstCasePeriods * (kBufferFrames * 2) * 1000 / kSampleRateHz >
        budget::kMaxAudioLatencyMs);
}

TEST_CASE("§11's residency cap, measured against a real manifest", "[budget]") {
  // §19 step 5 landed the pack, so the plate count is no longer a declaration:
  // it is read back out of a manifest that was actually assembled.
  //
  // The comparison happens HERE and not in pack.hpp, deliberately. emit.hpp
  // states the rule: "The sink reports; the budget judges; exactly one file can
  // relax a budget." A parser that knows the cap is a parser that can be
  // configured past it.
  const auto field_bytes = plate::blob_bytes(lightfield::kWidthPx, lightfield::kHeightPx);
  std::vector<std::byte> pixels(field_bytes * lightfield::kFieldCount);
  std::vector<pack::Record> records;
  std::vector<std::span<const std::byte>> blobs;

  for (int level = kLampLevelMin; level <= kLampLevelMax; ++level) {
    const auto slot = static_cast<std::size_t>(level - kLampLevelMin);
    const auto blob = std::span{pixels}.subspan(slot * field_bytes, field_bytes);
    REQUIRE(lightfield::bake(level, blob));

    pack::Record r{};
    r.plate_id = static_cast<std::uint16_t>(level);
    r.role = pack::Role::LightField;
    r.depth = pack::kDepthFullFrame;
    r.lateral = pack::Lateral::FullFrame;
    r.codec = pack::Codec::RawPlanes;
    r.w = static_cast<std::uint16_t>(lightfield::kWidthPx);
    r.h = static_cast<std::uint16_t>(lightfield::kHeightPx);
    records.push_back(r);
    blobs.push_back(blob);
  }

  std::vector<std::byte> image(pack::image_bytes(records));
  REQUIRE(pack::assemble(records, blobs, image));
  REQUIRE(pack::verify(image));

  pack::Header header{};
  REQUIRE(pack::read_header(image, header));
  CHECK(header.plate_count <= budget::kMaxResidentImages);
  CHECK(header.plate_count == budget::kLightFields.m0);
  CHECK(header.total_bytes == image.size());

  // A necessary condition, not the budget: the pack cannot be under 1.2 MB on
  // the wire if it is already over 1.2 MB at rest.
  CHECK(image.size() <= budget::kMaxColdStartPayloadBytes);
}

TEST_CASE("§11's cold-start payload row is CURRENTLY BLOWN, and this says so", "[budget]") {
  // §11 and BUDGETS.md budget 1.2 MB of BASE64 — the TRANSMIT payload, not the
  // pack. BUDGETS.md names a "manifest test" as the enforcer, and a manifest
  // test can only measure the pack, so the row as written cannot be checked by
  // the thing it names. Those are wildly different numbers: kitty is handed
  // PIXELS, so a RawPlanes plate expands from 2 bits per pixel to 32, and then
  // base64 adds another third.
  //
  // THIS ASSERTION IS DELIBERATELY INVERTED. The projected wire cost is
  // computable from constants already in scope, so rather than record the
  // overrun in four comment blocks and assert nothing — which is how a budget
  // becomes prose — it is asserted in the direction that is true today. When
  // the transmit path lands with Codec::Png, `o=z` or shared memory, this test
  // goes RED and whoever fixed it has to come here and flip it into the real
  // row. A budget nobody can accidentally leave broken.
  //
  // See UPSTREAM.md's correction 7 and gloam#17.
  // Derived from the PIXEL COUNT, not from a bits-per-pixel ratio against the
  // blob. A plate's blob is 3 bits per pixel, not 2 — the 2-bit index plane
  // plus the 1-bit stencil — and quoting the palette depth alone overstates the
  // expansion by half as much again.
  constexpr std::size_t kBytesPerWirePixel = 4;  // kitty f=32 RGBA

  const auto pixels = static_cast<std::size_t>(lightfield::kWidthPx) *
                      static_cast<std::size_t>(lightfield::kHeightPx) *
                      static_cast<std::size_t>(assets::kPlateCount);
  const auto pack_bytes = assets::image_bytes();
  const auto wire_bytes = pixels * kBytesPerWirePixel;
  const auto base64_bytes = wire_bytes * 4 / 3;

  // The blob really is 3 bits per pixel, and that is what makes the pack small.
  CHECK(assets::pixel_bytes() * 8 == pixels * 3);

  INFO("pack " << pack_bytes << " B, f=32 payload " << wire_bytes << " B, base64 "
               << base64_bytes << " B, budget " << budget::kMaxColdStartPayloadBytes << " B");

  CHECK(pack_bytes <= budget::kMaxColdStartPayloadBytes);
  CHECK(base64_bytes > budget::kMaxColdStartPayloadBytes);

  // Roughly 4.6x over, from the light fields alone, before a single wall plate
  // exists. Pinned so the ratio cannot drift unremarked in either direction.
  CHECK(base64_bytes / budget::kMaxColdStartPayloadBytes >= 4);
}

TEST_CASE("§9.2's dropped-voice-command budget is zero", "[budget]") {
  // "A full ring drops the command and increments a counter. It never blocks."
  // The counter is a budget assertion, not a log line — so the budget is zero,
  // not "few".
  CHECK(budget::kMaxDroppedVoiceCommands == 0);
  CHECK(budget::kDroppedVoiceCommandWindowTicks == 1000);
}

TEST_CASE("§9.2's dropped-voice-command budget, measured over its own window", "[budget]") {
  // §19 step 9 landed the ring, so this row stops declaring its constant and
  // starts measuring against it.
  //
  // THE DRAIN CADENCE IS THE WORST CASE, DELIBERATELY. §9.2's 256-frame buffer
  // at 48 kHz means the real consumer wakes roughly EIGHTEEN times per simulated
  // tick; this drains ONCE per tick. If the budget holds when the audio thread
  // gets a single look per tick, it holds comfortably at the cadence the device
  // actually runs at — and a test written at the real cadence would pass on a
  // ring far too small.
  const Tuning& t = kDefaultTuning;

  Level level{16, 3};
  level.carve(Coord{0, 1}, Dir::East, 16);
  std::vector<Monster> monsters;
  for (int m = 0; m < 8; ++m) {
    monsters.push_back(Monster{Coord{4 + m, 1}, MonsterKind{Acuity::Keen, false}, {}});
  }

  auto world = make_world(0xA0D10ULL, std::move(level), std::move(monsters));
  world.party = Coord{1, 1};
  world.armour = Armour::Plate;   // loudest emitter, so every tick has a voice
  world.lamp_level = kLampLevelMax;

  audio::RecordingSink<> sink;
  audio::Command drained{};

  for (int tick = 0; tick < budget::kDroppedVoiceCommandWindowTicks; ++tick) {
    world.pending_noise = step_noise(world.armour, world.creeping, t);
    advance(world, t, &sink);
    while (sink.ring().try_pop(drained)) {}  // the audio thread's one look
  }

  // The window must have carried real traffic. Without this the row below is
  // satisfied by a ring nothing was ever pushed to — the same vacuous pass
  // test/16audiosim/ guards against, and the reason `pushed()` exists.
  INFO("voices pushed over the window: " << sink.ring().pushed());
  CHECK(sink.ring().pushed() >= static_cast<std::uint64_t>(
                                   budget::kDroppedVoiceCommandWindowTicks));

  INFO("dropped " << sink.ring().dropped() << " of " << sink.ring().pushed());
  CHECK(sink.ring().dropped() ==
        static_cast<std::uint64_t>(budget::kMaxDroppedVoiceCommands));
}

TEST_CASE("the drop counter is not merely stuck at zero", "[budget]") {
  // The row above passes if `dropped()` can never increment for any reason —
  // a counter wired to nothing reports a perfect budget forever. This drives
  // the ring past capacity with no consumer at all and requires the number to
  // move, which is what makes the zero above mean something.
  audio::Ring<4> ring;
  for (int i = 0; i < 16; ++i) static_cast<void>(ring.try_push(audio::Command{}));
  CHECK(ring.dropped() == 12);
  CHECK(ring.dropped() > static_cast<std::uint64_t>(budget::kMaxDroppedVoiceCommands));
}

TEST_CASE("§11's simulation tick budget, measured", "[budget]") {
  // The one timing row that is measurable today, because the simulation core
  // exists and the renderer does not.
  //
  // A tick's perception work is: propagate the party's step noise across the
  // level, then run every monster's senses against it. This runs that at a
  // level size and monster count well past M0's corridor, so the headroom is
  // real rather than an artefact of a four-cell test.
  //
  // It calls `gloam::advance` rather than open-coding the loop. This case used
  // to BE the only tick in the tree, and `world.cpp` took it verbatim; running
  // the copy would leave the measured tick free to drift away from the replayed
  // one, and the budget would then be guarding something nothing executes.
  const Tuning& t = kDefaultTuning;

  constexpr int kSide = 32;
  Level level{kSide, kSide};
  for (int y = 0; y < kSide; ++y) level.carve(Coord{0, y}, Dir::East, kSide);
  for (int x = 0; x < kSide; ++x) level.carve(Coord{x, 0}, Dir::South, kSide);

  constexpr int kMonsters = 16;
  std::vector<Monster> monsters;
  for (int m = 0; m < kMonsters; ++m) {
    monsters.push_back(Monster{Coord{2 + m % 28, 2 + m / 4}, MonsterKind{Acuity::Normal, false}, {}});
  }

  auto world = make_world(0xB0DEB0DEULL, std::move(level), std::move(monsters));
  world.party = Coord{1, 1};
  world.armour = Armour::Plate;

  constexpr int kTicks = 100;
  const auto start = std::chrono::steady_clock::now();
  for (int tick = 0; tick < kTicks; ++tick) {
    // Set the emission directly rather than stepping: the party stays put, so
    // every tick is a LOUD tick over the full field. That is the worst case the
    // budget has to hold for, and a walking party would spend most of these
    // ticks propagating silence.
    world.pending_noise = step_noise(world.armour, world.creeping, t);
    advance(world, t);
  }
  const auto elapsed = std::chrono::steady_clock::now() - start;
  const auto per_tick_us =
      std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count() / kTicks;

  // The tick must have DONE something. Delegating to `gloam::advance` bought
  // drift-protection and gave up the property that the work being timed was
  // written here — so without these two lines, a plausible early-out added to
  // `advance` (skip a silent tick, skip a monster out of noise range) would
  // make this row pass faster than ever while measuring almost nothing.
  CHECK(world.tick == kTicks);
  const auto roused = std::count_if(world.monsters.begin(), world.monsters.end(),
                                    [](const Monster& m) {
                                      return m.mind.state != Awareness::Unaware;
                                    });
  INFO("monsters no longer unaware: " << roused << " of " << kMonsters);
  CHECK(roused > 0);

  INFO("per-tick " << per_tick_us << " us against a budget of "
                   << budget::kMaxSimulationTickMs * 1000 << " us");
  CHECK(per_tick_us < budget::kMaxSimulationTickMs * 1000);
}

TEST_CASE("§11's tick budget still holds with the audio sink attached", "[budget]") {
  // THE ROW MOST AT RISK FROM §19 STEP 9, AND THE REASON THIS CASE EXISTS.
  //
  // The party's footfall is free — it reads the field `advance` already
  // propagated. A monster's sting is NOT: it is sourced at the monster, so it
  // needs its own `propagate_noise` from the other end. That is the same
  // function `noise.hpp` insists on rather than a second path, but it is not
  // free, and a tick in which every monster transitions at once pays for one
  // propagation per monster.
  //
  // §6.1 makes that pathological — a sting is reserved for a FIRST sighting, so
  // in play it fires once per monster per encounter. But "rare" is not a budget
  // argument, and the honest way to hold this row is to measure the worst case
  // rather than to assume the common one. Same level size and monster count as
  // the case above, so the two numbers are directly comparable.
  const Tuning& t = kDefaultTuning;

  constexpr int kSide = 32;
  Level level{kSide, kSide};
  for (int y = 0; y < kSide; ++y) level.carve(Coord{0, y}, Dir::East, kSide);
  for (int x = 0; x < kSide; ++x) level.carve(Coord{x, 0}, Dir::South, kSide);

  constexpr int kMonsters = 16;
  std::vector<Monster> monsters;
  for (int m = 0; m < kMonsters; ++m) {
    monsters.push_back(
        Monster{Coord{2 + m % 28, 2 + m / 4}, MonsterKind{Acuity::Normal, false}, {}});
  }

  auto world = make_world(0xB0DEB0DEULL, std::move(level), std::move(monsters));
  world.party = Coord{1, 1};
  world.armour = Armour::Plate;
  world.lamp_level = kLampLevelMax;  // lit and in the open: every monster can escalate

  audio::RecordingSink<> sink;
  audio::Command drained{};

  constexpr int kTicks = 100;
  const auto start = std::chrono::steady_clock::now();
  for (int tick = 0; tick < kTicks; ++tick) {
    world.pending_noise = step_noise(world.armour, world.creeping, t);
    advance(world, t, &sink);
    while (sink.ring().try_pop(drained)) {}
  }
  const auto elapsed = std::chrono::steady_clock::now() - start;
  const auto per_tick_us =
      std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count() / kTicks;

  // The stings must actually have fired, or this is timing the footfall path
  // twice and calling it a worst case.
  INFO("voices over " << kTicks << " ticks: " << sink.ring().pushed());
  CHECK(sink.ring().pushed() > static_cast<std::uint64_t>(kTicks));

  INFO("per-tick with audio " << per_tick_us << " us against a budget of "
                              << budget::kMaxSimulationTickMs * 1000 << " us");
  CHECK(per_tick_us < budget::kMaxSimulationTickMs * 1000);
}

TEST_CASE("§11's tick budget holds when EVERY monster stings on one tick", "[budget]") {
  // THE ACTUAL WORST CASE, CONSTRUCTED RATHER THAN WAITED FOR.
  //
  // The case above is honest about cost but not about concurrency: escalation
  // takes several ticks and §6.1 advances at most one state per tick, so its
  // stings arrive a few at a time and no single tick pays for many
  // propagations. That measures the common case and calls it worst.
  //
  // This puts every monster one step below HUNTING and then satisfies §6.1's
  // sighting condition for all of them simultaneously, so ONE tick performs one
  // sting propagation per monster. If audio can blow the tick budget, it blows
  // it here and nowhere else.
  const Tuning& t = kDefaultTuning;

  constexpr int kSide = 32;
  Level level{kSide, kSide};
  for (int y = 0; y < kSide; ++y) level.carve(Coord{0, y}, Dir::East, kSide);
  for (int x = 0; x < kSide; ++x) level.carve(Coord{x, 0}, Dir::South, kSide);

  // Within sight distance of the party and in clear line of sight, so the
  // SEARCHING -> HUNTING row fires for each of them on the same tick.
  constexpr int kMonsters = 16;
  const Coord party{16, 16};
  std::vector<Monster> monsters;
  for (int m = 0; m < kMonsters; ++m) {
    Perception searching{};
    searching.state = Awareness::Searching;
    searching.has_last_known = true;
    searching.last_known = party;
    monsters.push_back(Monster{Coord{14 + m % 5, 14 + m / 5},
                               MonsterKind{Acuity::Normal, false}, searching});
  }

  auto world = make_world(0xC0FFEEULL, std::move(level), std::move(monsters));
  world.party = party;
  world.armour = Armour::Plate;
  world.lamp_level = kLampLevelMax;
  world.pending_noise = step_noise(world.armour, world.creeping, t);

  audio::RecordingSink<> sink;
  const auto start = std::chrono::steady_clock::now();
  advance(world, t, &sink);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  const auto tick_us = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();

  // Every monster really did sting on this one tick: 16 stings plus 1 footfall.
  const auto hunting = std::count_if(world.monsters.begin(), world.monsters.end(),
                                     [](const Monster& m) {
                                       return m.mind.state == Awareness::Hunting;
                                     });
  INFO("monsters that reached HUNTING on one tick: " << hunting);
  CHECK(hunting == kMonsters);
  INFO("voices on the worst tick: " << sink.ring().pushed());
  CHECK(sink.ring().pushed() == static_cast<std::uint64_t>(kMonsters) + 1);

  INFO("worst tick " << tick_us << " us against a budget of "
                     << budget::kMaxSimulationTickMs * 1000 << " us");
  CHECK(tick_us < budget::kMaxSimulationTickMs * 1000);
}

TEST_CASE("§4.5's below-background threshold is reachable only through the layer API", "[budget]") {
  // The threshold's value is asserted in test/06layer/, next to the constant it
  // pins. This case deliberately does NOT spell the literal: two tests asserting
  // the same number in two files is two places for it to drift.
  //
  // What §11 needs from §4.5 is weaker and worth stating on its own — that the
  // back of the compositor is reachable through the named band API, and that it
  // really is below kitty's cell background rather than merely negative.
  CHECK(layer::image_z(layer::Band::BelowBackground, 0) == layer::kBelowBackgroundZ);
  CHECK(layer::kBelowBackgroundZ < -(std::int32_t{1} << 30));
}

TEST_CASE("§4.6's idle frame costs zero bytes, measured at the sink", "[budget]") {
  // §11's headline row, and the first one that stops being a declaration and
  // becomes a measurement. A frame in which the compositor placed nothing must
  // put nothing on the wire — not "almost nothing", not "a cursor move".
  emit::ByteSink sink;
  sink.clear();

  CHECK(sink.size() == budget::kIdleFrameBytes);
  CHECK(sink.total() == budget::kIdleFrameBytes);

  // An idle frame is still a frame. If clear() did not count it, a run of idle
  // frames would be indistinguishable from no frames at all and this row would
  // be unmeasurable rather than merely trivially met.
  CHECK(sink.frames() == 1);

  // PENDING M0: the sustained p95 row (kMaxSustainedBytesPerSecond) needs a
  // scripted replay to measure against — TEST-PLAN.md §4, G-6.
}
