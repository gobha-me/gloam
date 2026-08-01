#pragma once

/// SPEC §4.4 — light as a full-frame field.
///
/// v1 proposed per-slot dither-density masks. They do not work: "a rectangular
/// mask cannot darken a trapezoidal side-wall plate without spilling over its
/// neighbours, and shaping one per slot costs 48 plates."
///
/// The fix is to notice that IN A FIXED-SLOT COMPOSITOR, SCREEN POSITION IS
/// DEPTH. The centre of the frame is far; the edges are near. So light falloff
/// is a single full-frame image: a 480x360 screen-door transparency field whose
/// hole density grows outward from the vanishing point. Six of them, one per
/// lamp level, composited above the geometry band and below the sprite and text
/// bands (§4.5's `layer::Band::Light`).
///
/// The band boundaries are DERIVED FROM `geometry::kDepths` and from nothing
/// else — see `knot_radius2`. That is what makes "screen position is depth" a
/// structural property rather than a claim: if the ladder moves, the fields move
/// with it, and `test/11lightfield/` fails if someone changes one without the
/// other.
///
/// Integers throughout. AGENTS.md rule 2 bans floating point in SIMULATION
/// state, and a baked field is not simulation state — but the pack hash is a
/// build gate (§10), and a float would make two compilers disagree about a byte
/// for reasons nobody could reproduce. The stricter rule is also the cheaper
/// one here: there is nothing in the bake a float would express better.
///
/// Excluded from the `gloam/gloam.hpp` umbrella — pipeline-side, not simulation.

#include <cstddef>
#include <cstdint>
#include <span>

#include "gloam/dither.hpp"
#include "gloam/geometry.hpp"
#include "gloam/plate.hpp"
#include "gloam/tuning.hpp"

namespace gloam::lightfield {

/// One field per lamp level. §4.4's table has six rows and `tuning.hpp` already
/// counts them; a second count here would be a second thing to keep in step.
inline constexpr int kFieldCount = kLampLevelCount;

/// Full-frame, by definition: the field covers the viewport exactly (§4.4), and
/// the viewport is the depth-0 ring (`geometry.hpp` static_asserts as much).
inline constexpr int kWidthPx = geometry::kViewportWidthPx;
inline constexpr int kHeightPx = geometry::kViewportHeightPx;

/// Squared distance from the vanishing point, in DOUBLED pixel units.
///
/// `dx = 2x + 1 - width` rather than `x - width/2`: with an even extent the
/// centre falls between two pixels, so integer halving would bias the field half
/// a pixel to one side and break the four-fold symmetry the bake is checked on.
/// Doubling puts the vanishing point on an exact lattice point instead.
///
/// Range at the corners is 479^2 + 359^2 = 358322, which is why this is i64:
/// the intermediate products are small, but the interpolation in `coverage_at`
/// multiplies a difference of radii by a coverage delta and that is not.
[[nodiscard]] constexpr auto radius2(int x, int y) -> std::int64_t {
  const std::int64_t dx = 2LL * x + 1 - kWidthPx;
  const std::int64_t dy = 2LL * y + 1 - kHeightPx;
  return dx * dx + dy * dy;
}

/// The squared radius at which depth `d` gives way to depth `d + 1`.
///
/// Screen position is depth, so the boundary is the circle inscribed in ring
/// `d`'s rectangle — its half-height, which in the doubled units `radius2` uses
/// is exactly `kDepths[d].height`. Anything at or past `kDepthCount` is 0, which
/// closes the sequence at the vanishing point; `coverage_at` does not ramp into
/// it, because depth 4 is a cap rather than a ring and there is nothing beyond
/// it to ramp toward.
///
/// Note what this yields for depth 0: `kDepths[0].height` is 360, so the knot is
/// at 129600, while the largest `dy^2` anywhere in the frame is 359^2 = 128881.
/// Band 0 is therefore reachable only to the left and right of centre — §4.4's
/// "the centre of the frame is far; the edges are near", falling out of the
/// arithmetic rather than being asserted by it.
[[nodiscard]] constexpr auto knot_radius2(int depth) -> std::int64_t {
  if (depth >= geometry::kDepthCount) return 0;
  const std::int64_t h = geometry::kDepths[static_cast<std::size_t>(depth)].height;
  return h * h;
}

/// How much opacity one cell of remaining lamp reach buys, on the dither's
/// scale. Half the range, so that "exactly one cell spare" lands on exactly the
/// half-dithered falloff band §4.4 describes — the screen door at its most
/// visible, which is what makes the edge of the light read as an edge.
///
/// DERIVED, not chosen: at `kLevels / 2` the falloff band is the midpoint by
/// construction, and `test/11lightfield/` pins that. A bare 32 here would be the
/// only unexplained number in a header whose whole thesis is that its geometry
/// comes from `geometry::kDepths` and nothing else.
///
/// Not a `tuning.hpp` tunable, and deliberately not covered by `ruleset_hash()`:
/// AGENTS.md scopes that to "every tunable integer in §6 and §8", and this is a
/// §4.4 bake constant that cannot alter a single simulation step. Hashing it
/// would invalidate every recorded replay for a change no replay can observe.
/// The gate that DOES cover it is `pack_sha256` — change this and the pack hash
/// moves, which is exactly right.
inline constexpr int kFalloffPerCell = dither::kLevels / 2;

/// Opacity at band knot `depth`, on [0, dither::kLevels].
///
/// `lamp_level - depth` is the lamp's remaining reach at that depth (§4.4's "lit
/// radius in cells"): two or more cells spare is clear, exactly one is the
/// half-dithered falloff band, none is black.
///
/// L0 is fully opaque at every knot, so a doused lamp needs no special case
/// anywhere in the bake — the ramp and the dither both degenerate on their own.
/// That matters more than it looks: §4.4's whole point is that the same integer
/// drives the render and the perception model, so "doused" must not be a branch
/// that could be forgotten on one side.
[[nodiscard]] constexpr auto knot_coverage(int lamp_level, int depth) -> int {
  const int d = depth >= geometry::kDepthCount ? geometry::kDepthCount - 1 : depth;
  const int fade = kFalloffPerCell * (lamp_level - d);
  const int clamped = fade < 0 ? 0 : (fade > dither::kLevels ? dither::kLevels : fade);
  return dither::kLevels - clamped;
}

/// Opacity at a pixel, on [0, dither::kLevels], interpolated in r^2 between
/// knots.
///
/// Three cases, and all three are reachable — there is no fallback branch:
///
///   - OUTSIDE the depth-0 knot: the near band, flat at its knot value. Reached
///     only left and right of centre, per `knot_radius2`'s note.
///   - INSIDE the depth-4 knot: the FAR CAP, flat at its knot value. Flat is
///     correct rather than a shortcut — `geometry.hpp` says depth 4 "is a cap
///     plate, not a navigable ring", so there is no depth 5 to ramp toward and
///     everything past sight distance is equally far. A disc of roughly 45 px
///     around the vanishing point.
///   - BETWEEN two knots: interpolated. `knot_coverage` is monotone
///     non-decreasing in `depth` for every level, so the numerator is never
///     negative and plain truncating division has exactly one answer on every
///     compiler. The band is found by a linear scan over five knots — no
///     logarithm, no lookup table, no float.
[[nodiscard]] constexpr auto coverage_at(int lamp_level, int x, int y) -> int {
  const auto r2 = radius2(x, y);
  if (r2 >= knot_radius2(0)) return knot_coverage(lamp_level, 0);

  constexpr int kCap = geometry::kDepthCount - 1;
  if (r2 < knot_radius2(kCap)) return knot_coverage(lamp_level, kCap);

  for (int d = 0; d < kCap; ++d) {
    const auto r2_outer = knot_radius2(d);
    const auto r2_inner = knot_radius2(d + 1);
    if (r2 < r2_inner) continue;

    const auto a_outer = static_cast<std::int64_t>(knot_coverage(lamp_level, d));
    const auto a_inner = static_cast<std::int64_t>(knot_coverage(lamp_level, d + 1));
    return static_cast<int>(a_outer +
                            ((a_inner - a_outer) * (r2_outer - r2)) / (r2_outer - r2_inner));
  }

  // Unreachable: the two guards above bracket r2 into [knot_radius2(kCap),
  // knot_radius2(0)), and the loop covers that interval exactly. Present
  // because a constexpr function must not fall off its end.
  return knot_coverage(lamp_level, kCap);
}

enum class BakeError : std::uint8_t {
  None = 0,
  LampLevelOutOfRange = 1,  ///< outside [kLampLevelMin, kLampLevelMax]
  BufferTooSmall = 2,       ///< smaller than plate::blob_bytes(kWidthPx, kHeightPx)
};

struct BakeResult {
  BakeError error{BakeError::None};
  std::size_t bytes{0};          ///< bytes written; 0 on every error
  std::size_t opaque_pixels{0};  ///< makes "L0 is total" and the level sweep exact

  [[nodiscard]] constexpr explicit operator bool() const { return error == BakeError::None; }
};

/// Bake one field into `blob`, which must be `plate::blob_bytes(kWidthPx,
/// kHeightPx)` bytes and is left untouched on any error.
///
/// The index plane is filled with `Ink0` — the shadow colour — and the STENCIL
/// PLANE IS THE SCREEN DOOR. This is screen-door transparency, exactly what
/// period engines did without an alpha channel: period-correct and cheapest
/// simultaneously.
///
/// Allocates nothing.
[[nodiscard]] auto bake(int lamp_level, std::span<std::byte> blob) -> BakeResult;

}  // namespace gloam::lightfield
