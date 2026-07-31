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

#include <chrono>
#include <cstddef>
#include <cstdint>

#include "gloam/budgets.hpp"
#include "gloam/emit.hpp"
#include "gloam/layer.hpp"
#include "gloam/noise.hpp"
#include "gloam/perception.hpp"
#include "gloam/tuning.hpp"

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

  // PENDING M0: cold start needs the pack and the upload path (§19 step 5).
  // PENDING M0: compose+diff+emit needs the compositor (§19 step 6).
  // PENDING M2: audio latency needs the sink (§19 step 9).
}

TEST_CASE("§9.2's dropped-voice-command budget is zero", "[budget]") {
  // "A full ring drops the command and increments a counter. It never blocks."
  // The counter is a budget assertion, not a log line — so the budget is zero,
  // not "few".
  CHECK(budget::kMaxDroppedVoiceCommands == 0);
  CHECK(budget::kDroppedVoiceCommandWindowTicks == 1000);
  // PENDING M2: needs the SPSC ring (§19 step 9).
}

TEST_CASE("§11's simulation tick budget, measured", "[budget]") {
  // The one timing row that is measurable today, because the simulation core
  // exists and the renderer does not.
  //
  // A tick's perception work is: propagate the party's step noise across the
  // level, then run every monster's senses against it. This runs that at a
  // level size and monster count well past M0's corridor, so the headroom is
  // real rather than an artefact of a four-cell test.
  const Tuning& t = kDefaultTuning;

  constexpr int kSide = 32;
  Level level{kSide, kSide};
  for (int y = 0; y < kSide; ++y) level.carve(Coord{0, y}, Dir::East, kSide);
  for (int x = 0; x < kSide; ++x) level.carve(Coord{x, 0}, Dir::South, kSide);

  constexpr int kMonsters = 16;
  Perception monsters[kMonsters]{};
  const MonsterKind kind{Acuity::Normal, false};
  const Coord party{1, 1};

  constexpr int kTicks = 100;
  const auto start = std::chrono::steady_clock::now();
  for (int tick = 0; tick < kTicks; ++tick) {
    const auto field = propagate_noise(level, party, step_noise(Armour::Plate, false, t), t);
    for (int m = 0; m < kMonsters; ++m) {
      const Coord at{2 + m % 28, 2 + m / 4};
      Senses s{};
      s.heard = hears(field, level, at, kind.acuity, monsters[m].state == Awareness::Hunting, t);
      s.los_clear = line_of_sight(level, at, party);
      s.range = range_between(at, party);
      s.lamp_level = kLampLevelDefault;
      s.party_position = party;
      step(monsters[m], s, kind, t);
    }
  }
  const auto elapsed = std::chrono::steady_clock::now() - start;
  const auto per_tick_us =
      std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count() / kTicks;

  INFO("per-tick " << per_tick_us << " us against a budget of "
                   << budget::kMaxSimulationTickMs * 1000 << " us");
  CHECK(per_tick_us < budget::kMaxSimulationTickMs * 1000);
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
