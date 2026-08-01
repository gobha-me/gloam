// SPEC §4.3, §10 — the ordered dither matrix and its one comparison.
//
// Six lines of shipped code, and they decide every byte of every plate. §10
// makes reproducibility a build gate ("the pipeline run twice on the same input
// must produce byte-identical plates"), so the properties below are not
// stylistic preferences about how a dither should behave — they are what the
// pack hash rests on.
//
// The matrix's own invariant is a static_assert in the header, and it is
// re-checked here at runtime on purpose: a static_assert only fires in a
// translation unit that includes the header AND gets rebuilt, and `dither.hpp`
// is header-only. A bad edit landing green in an incremental build is exactly
// the failure this duplicate exists to prevent.
//
// Failure matrix first, per AGENTS.md.

#include <catch2/catch_all.hpp>

#include <array>
#include <cstdint>

#include "gloam/dither.hpp"
#include "gloam/geometry.hpp"

using namespace gloam;

TEST_CASE("a negative coordinate is a phase, not an out-of-bounds read", "[dither]") {
  // THE failure case this file was missing. C++'s `%` truncates toward zero, so
  // `-1 % 8` is -1; casting that to size_t to index kBayer8 reads four billion
  // elements past the end. `phase` computes the periodic answer instead, which
  // is well-defined because the pattern genuinely repeats.
  //
  // Not hypothetical: any caller deriving a plate-local coordinate by
  // subtracting an origin (screen_x - plate_left) goes negative the first time a
  // plate is partially off the left or top edge.
  for (int i = -3 * dither::kMatrixSize; i < 3 * dither::kMatrixSize; ++i) {
    INFO("coordinate " << i);
    const auto p = dither::phase(i);
    REQUIRE(p >= 0);
    REQUIRE(p < dither::kMatrixSize);
    // Periodic: a shift by a whole matrix is a no-op in both directions.
    REQUIRE(p == dither::phase(i + dither::kMatrixSize));
    REQUIRE(p == dither::phase(i - dither::kMatrixSize));
  }

  CHECK(dither::phase(0) == 0);
  CHECK(dither::phase(-1) == dither::kMatrixSize - 1);
  CHECK(dither::phase(-dither::kMatrixSize) == 0);

  // And opaque_at agrees with itself across the origin rather than reading wild.
  for (int c = 0; c <= dither::kLevels; c += 13) {
    for (int i = -16; i < 16; ++i) {
      INFO("coverage " << c << " at " << i);
      CHECK(dither::opaque_at(c, i, i) == dither::opaque_at(c, i + 8, i + 8));
    }
  }
}

TEST_CASE("coverage outside the range saturates rather than misbehaving", "[dither]") {
  // Both facts fall out of the strict `>` and the matrix's range, so neither
  // needs a branch — but a caller that clamps badly should get black or opaque,
  // never an inverted pattern.
  for (int y = 0; y < dither::kMatrixSize; ++y) {
    for (int x = 0; x < dither::kMatrixSize; ++x) {
      INFO("phase (" << x << ", " << y << ")");
      CHECK_FALSE(dither::opaque_at(-1, x, y));
      CHECK_FALSE(dither::opaque_at(-1000, x, y));
      CHECK(dither::opaque_at(dither::kLevels + 1, x, y));
      CHECK(dither::opaque_at(1'000'000, x, y));
    }
  }
}

TEST_CASE("kBayer8 is a permutation of 0..kLevels-1", "[dither]") {
  std::array<int, dither::kLevels> seen{};
  for (const auto& row : dither::kBayer8) {
    for (const auto v : row) {
      REQUIRE(v < dither::kLevels);
      ++seen[v];
    }
  }
  for (int i = 0; i < dither::kLevels; ++i) {
    INFO("threshold " << i);
    CHECK(seen[static_cast<std::size_t>(i)] == 1);
  }

  // If it is not a permutation, coverage is not linear: some opacities become
  // unreachable and others get two levels' worth of pixels.
  CHECK(dither::kLevels == dither::kMatrixSize * dither::kMatrixSize);
}

TEST_CASE("zero coverage lights nothing, at every phase", "[dither]") {
  // The strict `>` in opaque_at is what makes this true. With `>=`, the single
  // 0 cell would leak one pixel in 64 — and §4.4's doused field would be
  // opaque's imperfect mirror rather than its exact one.
  for (int y = 0; y < dither::kMatrixSize; ++y) {
    for (int x = 0; x < dither::kMatrixSize; ++x) {
      INFO("phase (" << x << ", " << y << ")");
      CHECK_FALSE(dither::opaque_at(0, x, y));
    }
  }
}

TEST_CASE("full coverage lights everything, at every phase", "[dither]") {
  for (int y = 0; y < dither::kMatrixSize; ++y) {
    for (int x = 0; x < dither::kMatrixSize; ++x) {
      INFO("phase (" << x << ", " << y << ")");
      CHECK(dither::opaque_at(dither::kLevels, x, y));
    }
  }
}

TEST_CASE("coverage c lights exactly c pixels per tile", "[dither][property]") {
  // The property that makes `coverage` mean something. Without it "half
  // opacity" is a word rather than a measurement, and §4.4's falloff bands
  // stop being comparable across lamp levels.
  for (int c = 0; c <= dither::kLevels; ++c) {
    int lit = 0;
    for (int y = 0; y < dither::kMatrixSize; ++y) {
      for (int x = 0; x < dither::kMatrixSize; ++x) {
        if (dither::opaque_at(c, x, y)) ++lit;
      }
    }
    INFO("coverage " << c);
    CHECK(lit == c);
  }
}

TEST_CASE("a pixel never un-lights as coverage rises", "[dither][property]") {
  // A dither that turns a pixel off when the field gets more opaque produces a
  // visible band at the transition, which is the artefact §4.3's whole
  // pre-baking strategy exists to avoid reintroducing by the back door.
  for (int y = 0; y < dither::kMatrixSize; ++y) {
    for (int x = 0; x < dither::kMatrixSize; ++x) {
      for (int lo = 0; lo <= dither::kLevels; ++lo) {
        if (!dither::opaque_at(lo, x, y)) continue;
        for (int hi = lo; hi <= dither::kLevels; ++hi) {
          INFO("phase (" << x << ", " << y << ") from " << lo << " to " << hi);
          REQUIRE(dither::opaque_at(hi, x, y));
        }
      }
    }
  }
}

TEST_CASE("the phase is periodic in the matrix and in the dither block", "[dither]") {
  // §4.3 needs the pattern to tile a plate whole. `geometry.hpp` static_asserts
  // that every ring width divides by kDitherBlock; this is the other half —
  // that a shift by a whole block is a no-op.
  for (int c = 0; c <= dither::kLevels; c += 7) {
    for (int y = 0; y < dither::kMatrixSize; ++y) {
      for (int x = 0; x < dither::kMatrixSize; ++x) {
        INFO("coverage " << c << " phase (" << x << ", " << y << ")");
        CHECK(dither::opaque_at(c, x, y) ==
              dither::opaque_at(c, x + dither::kMatrixSize, y + dither::kMatrixSize));
        CHECK(dither::opaque_at(c, x, y) ==
              dither::opaque_at(c, x + geometry::kDitherBlock, y + geometry::kDitherBlock));
      }
    }
  }
}

TEST_CASE("the matrix agrees with the ladder's dither constants", "[dither]") {
  // Asserted here as well as in the header, so a change to geometry.hpp that
  // nobody rebuilds dither.hpp against still fails a test rather than passing.
  CHECK(dither::kMatrixSize == geometry::kDitherCell);
  CHECK(geometry::kDitherBlock % dither::kMatrixSize == 0);

  for (const auto& ring : geometry::kDepths) {
    INFO("ring " << ring.width << "x" << ring.height);
    CHECK(ring.width % dither::kMatrixSize == 0);
  }
}

TEST_CASE("the matrix's spread is what makes it Bayer and not a ramp", "[dither]") {
  // Last, and the least interesting: a lock on the values themselves. It only
  // proves the table is the one that was reviewed — but the table IS the fixed
  // seed §10 requires, so a silent edit to it must be a test failure.
  CHECK(dither::kBayer8[0][0] == 0);
  CHECK(dither::kBayer8[0][1] == 32);
  CHECK(dither::kBayer8[7][0] == 63);
  CHECK(dither::kBayer8[4][4] == 1);

  // Adjacent thresholds are never adjacent cells — the property that makes the
  // screen door read as a texture instead of as diagonal stripes.
  for (int y = 0; y < dither::kMatrixSize; ++y) {
    for (int x = 0; x + 1 < dither::kMatrixSize; ++x) {
      const auto a = static_cast<int>(dither::kBayer8[static_cast<std::size_t>(y)]
                                                     [static_cast<std::size_t>(x)]);
      const auto b = static_cast<int>(dither::kBayer8[static_cast<std::size_t>(y)]
                                                     [static_cast<std::size_t>(x + 1)]);
      INFO("cells (" << x << ", " << y << ") and (" << x + 1 << ", " << y << ")");
      CHECK(a != b + 1);
      CHECK(b != a + 1);
    }
  }
}
