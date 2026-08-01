#pragma once

/// SPEC §4.3, §10 — the fixed ordered dither, and the one comparison in the
/// whole pipeline.
///
/// §10: "The dither kernel is a fixed ordered matrix with a fixed seed — the
/// pipeline run twice on the same input must produce byte-identical plates,
/// because the pack hash is a build gate." An ordered matrix IS the fixed seed:
/// there is no PRNG here to seed, which is why the reproducibility requirement
/// costs nothing to meet.
///
/// §4.3 is the reason the matrix lives offline and only offline: "all
/// quantization happens in the offline pipeline. Nothing is dithered, scaled or
/// resampled at runtime, ever." The pattern is burned into the plate, so
/// stepping forward cannot make it swim — the pixels are literally the same
/// pixels.
///
/// Header-only and `constexpr` throughout. There is no `dither.cpp` and there
/// should not be: everything here is a table lookup and a comparison, and a
/// caller that can fold it at compile time gets a better light-field bake.
/// Excluded from the `gloam/gloam.hpp` umbrella — pipeline-side, not simulation.

#include <array>
#include <cstdint>

#include "gloam/geometry.hpp"

namespace gloam::dither {

/// The matrix is `geometry::kDitherCell` on a side, and that is not a
/// coincidence to be maintained by hand — `geometry.hpp` already static_asserts
/// that every ring width divides by both `kDitherCell` and `kDitherBlock`, so
/// the pattern never lands on a fractional boundary on any plate in the ladder.
inline constexpr int kMatrixSize = geometry::kDitherCell;

/// The coverage denominator: how many distinct opacities the matrix can express.
///
/// 64 rather than 16 or 255 because it is exactly the number of cells, which is
/// what makes both endpoints exact — see `opaque_at`.
inline constexpr int kLevels = kMatrixSize * kMatrixSize;

/// The canonical Bayer 8x8 threshold matrix: a permutation of 0..63, recursively
/// constructed, chosen because its thresholds are maximally spread — the visual
/// property that makes a screen door read as a screen door rather than as
/// clumps. Asserted to be a permutation below, and swept again at runtime in
/// `test/09dither/` so a bad edit fails loudly rather than in one translation
/// unit that nobody rebuilds.
inline constexpr std::array<std::array<std::uint8_t, kMatrixSize>, kMatrixSize> kBayer8{{
    {{0, 32, 8, 40, 2, 34, 10, 42}},
    {{48, 16, 56, 24, 50, 18, 58, 26}},
    {{12, 44, 4, 36, 14, 46, 6, 38}},
    {{60, 28, 52, 20, 62, 30, 54, 22}},
    {{3, 35, 11, 43, 1, 33, 9, 41}},
    {{51, 19, 59, 27, 49, 17, 57, 25}},
    {{15, 47, 7, 39, 13, 45, 5, 37}},
    {{63, 31, 55, 23, 61, 29, 53, 21}},
}};

/// The matrix phase of a coordinate. Always in [0, kMatrixSize).
///
/// C++'s `%` truncates toward zero, so `-1 % 8` is -1, not 7. Casting that to
/// `std::size_t` to index the matrix would read four billion elements past the
/// end. The pattern is genuinely periodic, so a negative coordinate has a
/// perfectly well-defined phase — this computes it rather than making the
/// caller promise not to ask.
///
/// TOTAL BY CONSTRUCTION, AND DELIBERATELY NOT AN ERROR ENUM. Every other entry
/// point in the pipeline (`plate::read`, `lightfield::bake`, `pack::verify`)
/// refuses bad input with a discriminated error, because for those there is no
/// right answer. Here there is one, and this function is inlined into the
/// innermost loop of every bake — a fallible signature would put a branch in the
/// hot path to reject a question that has an answer.
[[nodiscard]] constexpr auto phase(int coordinate) -> int {
  const int wrapped = coordinate % kMatrixSize;
  return wrapped < 0 ? wrapped + kMatrixSize : wrapped;
}

/// Is the pixel at plate-local `(x, y)` opaque, at this `coverage`?
///
/// `coverage` is an opacity on [0, kLevels]. `x` and `y` are PLATE-LOCAL — the
/// phase is a property of the plate, not of where the plate lands on screen,
/// which is what §4.3's "the pixels are literally the same pixels" requires. A
/// screen-relative phase would reintroduce crawl through the back door. Passing
/// a screen coordinate is a correctness bug this cannot detect; passing a
/// negative one is merely a phase, and is handled.
///
/// The comparison is strictly `>`, and that is load-bearing at both ends:
///
///   - `coverage == 0` yields ZERO opaque pixels. With `>=`, the matrix's single
///     0 cell would leak one pixel in 64, and §4.4's fully-doused field would be
///     very slightly not-opaque's mirror image.
///   - `coverage == kLevels` is opaque everywhere, because the matrix maxes at
///     kLevels - 1.
///
/// Both endpoints exact is the whole reason the denominator is the cell count.
/// Coverage outside [0, kLevels] is clamped by the same two facts rather than
/// refused: below 0 nothing lights, above kLevels everything does.
[[nodiscard]] constexpr auto opaque_at(int coverage, int x, int y) -> bool {
  const auto row = static_cast<std::size_t>(phase(y));
  const auto col = static_cast<std::size_t>(phase(x));
  return coverage > static_cast<int>(kBayer8[row][col]);
}

static_assert(kMatrixSize == geometry::kDitherCell,
              "the matrix and the ladder's dither cell are the same number");
static_assert(geometry::kDitherBlock % kMatrixSize == 0,
              "the dither block must be a whole number of matrix tiles");

static_assert([] {
  std::array<bool, kLevels> seen{};
  for (const auto& row : kBayer8) {
    for (const auto v : row) {
      if (v >= kLevels) return false;
      if (seen[v]) return false;
      seen[v] = true;
    }
  }
  return true;
}(), "kBayer8 must be a permutation of 0..kLevels-1, or coverage is not linear");

}  // namespace gloam::dither
