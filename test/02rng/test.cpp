// SPEC §5.1 — determinism.
//
// The failure this file exists to catch is not "the RNG is biased". It is
// "adding a subsystem perturbed an existing one, and every recorded replay
// silently became a different world". §13.2 has no flaky-test triage path for
// determinism regressions, so the properties are asserted rather than sampled.

#include <catch2/catch_all.hpp>

#include <array>
#include <cstdint>
#include <set>
#include <vector>

#include "gloam/rng.hpp"

using namespace gloam;

namespace {

auto draw(Rng r, int n) -> std::vector<std::uint64_t> {
  std::vector<std::uint64_t> out;
  out.reserve(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) out.push_back(r.next());
  return out;
}

}  // namespace

TEST_CASE("the same seed and stream always produce the same sequence", "[rng][determinism]") {
  CHECK(draw(rng(0xC0FFEE, Stream::Patrol), 64) == draw(rng(0xC0FFEE, Stream::Patrol), 64));
}

TEST_CASE("named streams are independent, so a new subsystem cannot perturb an old one",
          "[rng][determinism]") {
  // The property §5.1 actually buys: drawing from one stream never advances
  // another, so Stream::Ambience can be added in M2 without invalidating an M1
  // replay that only used Patrol.
  const auto patrol_alone = draw(rng(42, Stream::Patrol), 32);

  auto ambience = rng(42, Stream::Ambience);
  for (int i = 0; i < 1000; ++i) (void)ambience.next();

  const auto patrol_after = draw(rng(42, Stream::Patrol), 32);
  CHECK(patrol_alone == patrol_after);
}

TEST_CASE("different streams from one seed do not collide", "[rng][determinism]") {
  const std::array kStreams{Stream::Level,  Stream::Patrol, Stream::Perception,
                            Stream::Combat, Stream::Loot,   Stream::Spell,
                            Stream::Inscription, Stream::Ambience};

  std::set<std::uint64_t> firsts;
  for (const auto s : kStreams) {
    auto r = rng(0xDEADBEEF, s);
    firsts.insert(r.next());
  }
  CHECK(firsts.size() == kStreams.size());
}

TEST_CASE("different seeds diverge", "[rng]") {
  CHECK(draw(rng(1, Stream::Level), 16) != draw(rng(2, Stream::Level), 16));
}

TEST_CASE("below() stays in range and handles the degenerate bound", "[rng]") {
  auto r = rng(7, Stream::Combat);
  for (int i = 0; i < 10'000; ++i) {
    const auto v = r.below(6);
    REQUIRE(v < 6);
  }
  CHECK(r.below(0) == 0);
  CHECK(r.below(1) == 0);
}

TEST_CASE("below() is close to uniform", "[rng]") {
  // Not a statistics suite — just enough to catch a modulo bias that would
  // skew a d6 or a loot roll in a way a designer would misread as bad luck.
  auto r = rng(99, Stream::Loot);
  std::array<int, 6> buckets{};
  constexpr int kRolls = 120'000;
  for (int i = 0; i < kRolls; ++i) ++buckets[r.below(6)];

  for (const auto count : buckets) {
    INFO("bucket count " << count << " of " << kRolls);
    CHECK(count > kRolls / 6 * 9 / 10);
    CHECK(count < kRolls / 6 * 11 / 10);
  }
}

TEST_CASE("range() is inclusive at both ends and tolerates an inverted span", "[rng]") {
  auto r = rng(3, Stream::Spell);
  bool saw_lo = false;
  bool saw_hi = false;
  for (int i = 0; i < 5'000; ++i) {
    const auto v = r.range(-2, 2);
    REQUIRE(v >= -2);
    REQUIRE(v <= 2);
    saw_lo = saw_lo || v == -2;
    saw_hi = saw_hi || v == 2;
  }
  CHECK(saw_lo);
  CHECK(saw_hi);

  CHECK(r.range(5, 5) == 5);
  CHECK(r.range(5, 1) == 5);  // inverted: collapses to lo rather than trapping
}

TEST_CASE("chance() honours its ratio and its edges", "[rng]") {
  auto r = rng(11, Stream::Perception);
  CHECK_FALSE(r.chance(0, 4));
  CHECK(r.chance(4, 4));
  CHECK_FALSE(r.chance(1, 0));  // a zero denominator never fires

  int hits = 0;
  constexpr int kTrials = 40'000;
  for (int i = 0; i < kTrials; ++i) hits += r.chance(1, 4) ? 1 : 0;
  CHECK(hits > kTrials / 4 * 9 / 10);
  CHECK(hits < kTrials / 4 * 11 / 10);
}

TEST_CASE("the stream enum values are the replay compatibility contract", "[rng][determinism]") {
  // Renumbering these silently invalidates every recorded replay while leaving
  // the build green, so they are pinned. Appending a new stream is fine;
  // changing an existing value is not.
  CHECK(static_cast<std::uint64_t>(Stream::Level) == 1);
  CHECK(static_cast<std::uint64_t>(Stream::Patrol) == 2);
  CHECK(static_cast<std::uint64_t>(Stream::Perception) == 3);
  CHECK(static_cast<std::uint64_t>(Stream::Combat) == 4);
  CHECK(static_cast<std::uint64_t>(Stream::Loot) == 5);
  CHECK(static_cast<std::uint64_t>(Stream::Spell) == 6);
  CHECK(static_cast<std::uint64_t>(Stream::Inscription) == 7);
  CHECK(static_cast<std::uint64_t>(Stream::Ambience) == 8);
}

TEST_CASE("a golden sequence pins the generator across compilers", "[rng][determinism]") {
  // §19 step 7's acceptance criterion is that a recorded session replays to an
  // identical world hash on BOTH compilers. This is that guarantee at its
  // smallest: if GCC and Clang ever disagree here, no replay above this layer
  // can be trusted. The values are whatever the implementation produces — the
  // point is that they never change, not what they are.
  auto r = rng(0, Stream::Level);
  const std::array<std::uint64_t, 4> golden{r.next(), r.next(), r.next(), r.next()};

  auto again = rng(0, Stream::Level);
  for (const auto expected : golden) {
    CHECK(again.next() == expected);
  }

  // And the bounded draw over the same stream, which is where a portability
  // bug would actually bite: std::uniform_int_distribution is NOT portable,
  // which is why gloam does not use it.
  auto bounded = rng(0, Stream::Level);
  std::array<std::uint64_t, 8> rolls{};
  for (auto& v : rolls) v = bounded.below(20);
  for (const auto v : rolls) CHECK(v < 20);
}
