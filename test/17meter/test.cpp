// SPEC §11 — the frame classes, and the p95 over their history.
//
// The meter is the layer between a byte counter and a budget: it says which of
// §11's three per-frame rows a frame is judged against, and it computes the
// sustained row, which is a percentile and therefore needs a history the sink
// does not keep.
//
// Failure matrix first, per AGENTS.md. Two of the cases below are the whole
// reason this file is worth more than the code it tests:
//
//   * an empty or out-of-range percentile yields nullopt and NOT 0, because a
//     zero p95 beats every budget there is and makes "I had no data"
//     indistinguishable from "I passed";
//   * the meter does not JUDGE. A 500-byte frame recorded as Idle is stored as a
//     500-byte idle frame. A meter that quietly corrected that would hide the
//     exact defect budget::kIdleFrameBytes == 0 exists to catch.
//
// The happy path is last and proves the least.

#include <algorithm>
#include <catch2/catch_all.hpp>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "gloam/budgets.hpp"
#include "gloam/emit.hpp"
#include "gloam/meter.hpp"

using gloam::meter::FrameClass;
using gloam::meter::FrameMeter;
using gloam::meter::percentile_nearest_rank;
using gloam::meter::sliding_window_sums;
using gloam::meter::sustained_p95;

namespace {

/// 1..n as sorted samples, so a rank can be read straight off the value.
auto ascending(std::size_t n) -> std::vector<std::uint64_t> {
  std::vector<std::uint64_t> v;
  v.reserve(n);
  for (std::size_t i = 1; i <= n; ++i) v.push_back(static_cast<std::uint64_t>(i));
  return v;
}

/// Feed a meter a run of frames, all of one class.
auto feed(FrameMeter& meter, FrameClass frame_class, const std::vector<std::size_t>& bytes)
    -> void {
  for (const auto b : bytes) meter.record(frame_class, b);
}

}  // namespace

// ════════════════════════════════════════════════════════════════════════════
//  Part A — the percentile, where an off-by-one is invisible
// ════════════════════════════════════════════════════════════════════════════

TEST_CASE("an empty history has no percentile, and pointedly does not have zero", "[meter]") {
  const std::vector<std::uint64_t> empty;

  const auto p = percentile_nearest_rank(empty, 95);
  CHECK_FALSE(p.has_value());

  // The failure mode being excluded, named rather than asserted — the thing
  // under test is that there is no value to compare at all. Every §11 byte
  // budget is strictly positive, so a p95 of 0 would satisfy all of them, and
  // "I measured nothing" would be indistinguishable from "I passed".
  static_assert(gloam::budget::kMaxSustainedBytesPerSecond > 0);
  static_assert(gloam::budget::kMaxRecompositionBytes > 0);
}

TEST_CASE("a percentile outside [1, 100] is refused rather than clamped", "[meter]") {
  const auto samples = ascending(10);

  CHECK_FALSE(percentile_nearest_rank(samples, 0).has_value());
  CHECK_FALSE(percentile_nearest_rank(samples, -1).has_value());
  CHECK_FALSE(percentile_nearest_rank(samples, 101).has_value());
  CHECK_FALSE(percentile_nearest_rank(samples, std::numeric_limits<int>::min()).has_value());
  CHECK_FALSE(percentile_nearest_rank(samples, std::numeric_limits<int>::max()).has_value());

  // Clamping 101 to 100 would be the "helpful" reading. It is refused because a
  // caller asking for a percentile that does not exist has a bug, and answering
  // a different question than the one asked is how that bug survives.
  CHECK(percentile_nearest_rank(samples, 100) == 10);
  CHECK(percentile_nearest_rank(samples, 1) == 1);
}

TEST_CASE("the rank is nearest-rank ceiling, asserted by literal", "[meter]") {
  // n = 20: ceil(95 * 20 / 100) = 19, so sorted[18], which is the value 19.
  // An off-by-one here reports p90 and NOTHING else in this suite would notice.
  CHECK(percentile_nearest_rank(ascending(20), 95) == 19);

  // n = 191 — the real shape: a 200-frame history with a 10-frame window gives
  // 191 sliding sums. ceil(95 * 191 / 100) = ceil(181.45) = 182, so sorted[181],
  // which is the value 182.
  CHECK(percentile_nearest_rank(ascending(191), 95) == 182);

  // The two neighbours, so the direction of the rounding is pinned and not just
  // the value. 190 -> ceil(180.5) = 181; 192 -> ceil(182.4) = 183.
  CHECK(percentile_nearest_rank(ascending(190), 95) == 181);
  CHECK(percentile_nearest_rank(ascending(192), 95) == 183);
}

TEST_CASE("nearest-rank is not linear interpolation, and the difference is asserted", "[meter]") {
  // Four samples: 10, 20, 30, 40. Linear interpolation (the "exclusive"/R-7
  // definition most libraries default to) puts p95 at 38.5 — a value no frame
  // ever produced. Nearest rank is ceil(95 * 4 / 100) = 4, so 40.
  const std::vector<std::uint64_t> samples{10, 20, 30, 40};

  const auto p95 = percentile_nearest_rank(samples, 95);
  REQUIRE(p95.has_value());
  CHECK(*p95 == 40);
  CHECK(*p95 != 38);  // the floor of the interpolated answer
  CHECK(*p95 != 39);

  // Same shape at p50, where interpolation would invent 25 and nearest-rank
  // returns an observed sample.
  CHECK(percentile_nearest_rank(samples, 50) == 20);

  // Every percentile this function returns is a value some frame actually
  // emitted. AGENTS.md rule 2 forbids the float that interpolation needs, and
  // this case is what stops someone reintroducing it as a "fix".
  for (int p = 1; p <= 100; ++p) {
    const auto v = percentile_nearest_rank(samples, p);
    REQUIRE(v.has_value());
    CHECK(std::ranges::find(samples, *v) != samples.end());
  }
}

TEST_CASE("a single sample answers every percentile", "[meter]") {
  const std::vector<std::uint64_t> one{7};
  for (int p = 1; p <= 100; ++p) CHECK(percentile_nearest_rank(one, p) == 7);
}

TEST_CASE("an all-equal history reports that value, not a neighbour", "[meter]") {
  const std::vector<std::uint64_t> flat(37, 819);
  CHECK(percentile_nearest_rank(flat, 95) == 819);
  CHECK(percentile_nearest_rank(flat, 1) == 819);
  CHECK(percentile_nearest_rank(flat, 100) == 819);
}

TEST_CASE("appending a sample at or above the maximum never lowers the p95", "[meter][property]") {
  auto samples = ascending(40);
  auto previous = percentile_nearest_rank(samples, 95);
  REQUIRE(previous.has_value());

  for (int i = 0; i < 30; ++i) {
    samples.push_back(samples.back() + static_cast<std::uint64_t>(i));
    std::ranges::sort(samples);
    const auto now = percentile_nearest_rank(samples, 95);
    REQUIRE(now.has_value());
    CHECK(*now >= *previous);
    previous = now;
  }
}

// ════════════════════════════════════════════════════════════════════════════
//  Part B — the windows, where the units go wrong
// ════════════════════════════════════════════════════════════════════════════

TEST_CASE("a window longer than the history has no answer, not a shorter one", "[meter]") {
  FrameMeter meter;
  feed(meter, FrameClass::Idle, {1, 2, 3});

  CHECK_FALSE(sliding_window_sums(meter, 4).has_value());
  CHECK_FALSE(sustained_p95(meter, 4).has_value());

  // The rejected fallback: "use the whole history when it is shorter than one
  // window". A three-tick second is not a second, and a bytes-per-second budget
  // compared against three tenths of one passes for the wrong reason.
  CHECK_FALSE(sustained_p95(meter, 10).has_value());
}

TEST_CASE("a zero-frame window is refused", "[meter]") {
  FrameMeter meter;
  feed(meter, FrameClass::Idle, {1, 2, 3});

  CHECK_FALSE(sliding_window_sums(meter, 0).has_value());
  CHECK_FALSE(sustained_p95(meter, 0).has_value());
}

TEST_CASE("a window exactly the length of the history yields one sum", "[meter]") {
  FrameMeter meter;
  feed(meter, FrameClass::Recomposition, {100, 200, 300, 400});

  const auto sums = sliding_window_sums(meter, 4);
  REQUIRE(sums.has_value());
  REQUIRE(sums->size() == 1);
  CHECK(sums->front() == 1000);
  CHECK(sustained_p95(meter, 4) == 1000);
}

TEST_CASE("windows slide by one frame and overlap", "[meter]") {
  FrameMeter meter;
  feed(meter, FrameClass::Animation, {1, 2, 3, 4, 5});

  const auto sums = sliding_window_sums(meter, 2);
  REQUIRE(sums.has_value());
  // 5 frames, window 2 -> 4 windows: (1+2), (2+3), (3+4), (4+5).
  REQUIRE(sums->size() == 4);
  CHECK((*sums)[0] == 3);
  CHECK((*sums)[1] == 5);
  CHECK((*sums)[2] == 7);
  CHECK((*sums)[3] == 9);
}

TEST_CASE("a window sum saturates rather than wrapping", "[meter]") {
  FrameMeter meter;
  const auto huge = std::numeric_limits<std::size_t>::max();
  feed(meter, FrameClass::Recomposition, {huge, huge, huge});

  const auto sums = sliding_window_sums(meter, 2);
  REQUIRE(sums.has_value());
  for (const auto sum : *sums) {
    CHECK(sum == std::numeric_limits<std::uint64_t>::max());
    // The failure being excluded: 2 * SIZE_MAX wraps to SIZE_MAX - 1, which is
    // still enormous — but three of them wrap to something small, and a small
    // number reads as a passing budget. Saturation turns an impossible overflow
    // into a permanent failure instead of a silent success.
    CHECK(sum > gloam::budget::kMaxSustainedBytesPerSecond);
  }

  CHECK(meter.total(FrameClass::Recomposition) == std::numeric_limits<std::uint64_t>::max());
}

TEST_CASE("a truncated history refuses to report a percentile, and keeps counting", "[meter]") {
  FrameMeter meter{4};
  feed(meter, FrameClass::Animation, {10, 20, 30, 40, 50, 60});

  REQUIRE(meter.history_truncated());
  CHECK(meter.history().size() == 4);

  // The percentile is refused: a p95 over the first four frames of a six-frame
  // run is a p95 of a different run.
  CHECK_FALSE(sliding_window_sums(meter, 2).has_value());
  CHECK_FALSE(sustained_p95(meter, 2).has_value());

  // The counters are not. An instrument that stopped counting at its own limit
  // would report a passing total for a run that outlived it.
  CHECK(meter.frames() == 6);
  CHECK(meter.frames(FrameClass::Animation) == 6);
  CHECK(meter.total() == 210);
  CHECK(meter.peak(FrameClass::Animation) == 60);
}

// ════════════════════════════════════════════════════════════════════════════
//  Part C — the meter reports; the budget judges
// ════════════════════════════════════════════════════════════════════════════

TEST_CASE("the meter does not judge, correct or reclassify", "[meter]") {
  FrameMeter meter;

  // An idle frame that cost 500 bytes is a BUG — §11 budgets zero. The meter's
  // job is to make it visible, not to fix it.
  meter.record(FrameClass::Idle, 500);

  CHECK(meter.frames(FrameClass::Idle) == 1);
  CHECK(meter.total(FrameClass::Idle) == 500);
  CHECK(meter.peak(FrameClass::Idle) == 500);

  // Not silently moved to a class it would fit.
  CHECK(meter.frames(FrameClass::Animation) == 0);
  CHECK(meter.frames(FrameClass::Recomposition) == 0);

  // And the judgement, made where it belongs — this is the shape every budget
  // assertion takes, and it must be able to FAIL.
  CHECK(meter.peak(FrameClass::Idle) > gloam::budget::kIdleFrameBytes);
}

TEST_CASE("classes are counted apart, not pooled", "[meter]") {
  FrameMeter meter;
  meter.record(FrameClass::Idle, 0);
  meter.record(FrameClass::Animation, 134);
  meter.record(FrameClass::Recomposition, 1608);
  meter.record(FrameClass::Animation, 67);

  CHECK(meter.frames() == 4);
  CHECK(meter.frames(FrameClass::Idle) == 1);
  CHECK(meter.frames(FrameClass::Animation) == 2);
  CHECK(meter.frames(FrameClass::Recomposition) == 1);

  CHECK(meter.total() == 1809);
  CHECK(meter.total(FrameClass::Animation) == 201);

  // Peak is per class: the run is judged on its worst frame OF THAT KIND, so a
  // 1608-byte recomposition does not raise the animation row's peak past 400.
  CHECK(meter.peak(FrameClass::Animation) == 134);
  CHECK(meter.peak(FrameClass::Recomposition) == 1608);
  CHECK(meter.peak(FrameClass::Idle) == gloam::budget::kIdleFrameBytes);
}

TEST_CASE("reset starts a window and is never implicit", "[meter]") {
  FrameMeter meter;
  feed(meter, FrameClass::Recomposition, {100, 200});
  REQUIRE(meter.frames() == 2);

  meter.reset();

  CHECK(meter.frames() == 0);
  CHECK(meter.total() == 0);
  CHECK(meter.peak(FrameClass::Recomposition) == 0);
  CHECK(meter.history().empty());
  CHECK_FALSE(meter.history_truncated());
  CHECK_FALSE(sustained_p95(meter, 10).has_value());
}

// ════════════════════════════════════════════════════════════════════════════
//  Part D — the seam onto emit::ByteSink
// ════════════════════════════════════════════════════════════════════════════

TEST_CASE("end_frame records what the sink held and then ends the frame", "[meter][emit]") {
  gloam::emit::ByteSink sink;
  FrameMeter meter;

  sink.write("0123456789");
  REQUIRE(sink.size() == 10);

  meter.end_frame(sink, FrameClass::Animation);

  CHECK(sink.size() == 0);
  CHECK(sink.frames() == 1);  // ended exactly once, not twice
  CHECK(meter.frames() == 1);
  CHECK(meter.total(FrameClass::Animation) == 10);
}

TEST_CASE("two counters that must agree, over a run", "[meter][emit]") {
  gloam::emit::ByteSink sink;
  FrameMeter meter;

  for (int i = 0; i < 10; ++i) {
    for (int b = 0; b < i; ++b) sink.write('x');
    meter.end_frame(sink, i % 3 == 0 ? FrameClass::Recomposition : FrameClass::Animation);
  }

  // 0+1+...+9 = 45.
  CHECK(sink.total() == 45);
  CHECK(meter.total() == 45);
  CHECK(sink.frames() == 10);
  CHECK(meter.frames() == 10);

  // If these two ever disagree the harness is wrong, not the code under test —
  // which is exactly why both are asserted rather than one derived from the
  // other.
  CHECK(meter.total() == sink.total());
  CHECK(meter.frames() == sink.frames());
  CHECK(meter.peak(FrameClass::Recomposition) <= sink.peak_frame());
}

TEST_CASE("resetting the meter does not reach into the sink", "[meter][emit]") {
  gloam::emit::ByteSink sink;
  FrameMeter meter;

  sink.write("abcd");
  meter.end_frame(sink, FrameClass::Idle);
  REQUIRE(sink.total() == 4);

  meter.reset();

  // The dependency runs one way: the meter observes the sink, it does not own
  // it. A reset that cleared both would make the two counters unable to
  // disagree, and the case above depends on their being able to.
  CHECK(sink.total() == 4);
  CHECK(sink.frames() == 1);
  CHECK(meter.frames() == 0);
}

TEST_CASE("record takes a count, so it can hold what a syscall reported", "[meter]") {
  gloam::emit::ByteSink sink;
  FrameMeter meter;

  for (int i = 0; i < 6000; ++i) sink.write('x');
  REQUIRE(sink.size() == 6000);

  // The whole reason `record` takes a std::size_t rather than a ByteSink&:
  // BUDGETS.md wants what LEFT THE PROCESS, and only a write syscall knows that.
  // Here 4096 of the 6000 produced bytes are what a full pipe accepted.
  const std::size_t accepted_by_the_kernel = 4096;
  meter.record(FrameClass::Recomposition, accepted_by_the_kernel);
  sink.clear();

  CHECK(meter.total() == 4096);
  CHECK(meter.total() != sink.total());  // produced != emitted, and both are known
  CHECK(sink.total() == 6000);
}

// ════════════════════════════════════════════════════════════════════════════
//  Part E — the smoke check
// ════════════════════════════════════════════════════════════════════════════

TEST_CASE("a plausible session reads back", "[meter]") {
  FrameMeter meter;

  // 20 frames: a step every fourth, otherwise animation and idle.
  for (int tick = 0; tick < 20; ++tick) {
    if (tick % 4 == 0) {
      meter.record(FrameClass::Recomposition, 1608);
    } else if (tick % 2 == 0) {
      meter.record(FrameClass::Animation, 67);
    } else {
      meter.record(FrameClass::Idle, 0);
    }
  }

  CHECK(meter.frames() == 20);
  CHECK(meter.frames(FrameClass::Recomposition) == 5);
  CHECK(meter.peak(FrameClass::Idle) == gloam::budget::kIdleFrameBytes);
  CHECK(meter.peak(FrameClass::Animation) <= gloam::budget::kMaxAnimationFrameBytes);
  CHECK(meter.peak(FrameClass::Recomposition) <= gloam::budget::kMaxRecompositionBytes);

  const auto p95 = sustained_p95(meter, 10);
  REQUIRE(p95.has_value());
  CHECK(*p95 > 0);

  CHECK(gloam::meter::name_of(FrameClass::Idle) == "idle");
  CHECK(gloam::meter::name_of(FrameClass::Animation) == "animation");
  CHECK(gloam::meter::name_of(FrameClass::Recomposition) == "recomposition");
}
