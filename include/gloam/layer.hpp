#pragma once

/// SPEC §4.5 — the compositing bands, and the only route to a z-index.
///
/// §4.5's table, verbatim:
///
///   | Band                   | z            | Content                          |
///   | ---------------------- | ------------ | -------------------------------- |
///   | Overlay images         | z >= 0       | party panel overlays, hand cursor |
///   | Text cells             | -            | readouts, item labels, rune entry |
///   | Light + sprites, upper | z < 0        | light field, then monster sprites |
///   | Geometry, lower        | z < 0        | wall / floor / ceiling plates     |
///   | Cell backgrounds       | -            | ambient tint                      |
///   | Below background       | -1073741825  | static frame furniture            |
///
/// Read against the kitty graphics protocol, that table is not a set of layers
/// GLOAM invented — it is kitty's own stacking rules written out. Kitty splits
/// the order three ways: z >= 0 draws the image above text, z < 0 draws it below
/// text, and z < -(2^30) draws it below the cell background as well. So "Text
/// cells" and "Cell backgrounds" are not bands anyone chose. They are the
/// dividers BETWEEN kitty's regimes, and that is why they have no z of their own.
///
/// §4.5: "That threshold integer must appear EXACTLY ONCE in the codebase,
/// inside a named layer API." This header is that named layer API, and
/// `cmake/check_layer_z.cmake` greps the shipped tree to enforce the once
/// (registered as the `layer-z-single-definition` ctest case). No application
/// code should ever hand-write the literal.
///
/// §16 names this module the mitigation for the project's highest-severity risk:
/// "Keep all kitty calls behind GLOAM's own layer API from day one, so a vendored
/// driver is a swap and not a rewrite." That is why the band vocabulary lives
/// here and not in `kitty.hpp` — a driver swap changes the wire format, not which
/// band a monster sprite belongs to. This header must survive that swap untouched.
///
/// termforge now exposes semantic `ImageLayer` placement through GL-B2 / #114.
/// GLOAM keeps this game-specific band vocabulary and maps it into that driver
/// surface when the terminal layer lands — see UPSTREAM.md.

#include <cstdint>
#include <optional>

namespace gloam::layer {

// ── §4.5 The bands ──────────────────────────────────────────────────────────
//
// Seven enumerators for six table rows. Row 3 names two depths in its own prose
// — "light field, THEN monster sprites beneath it" — and one enumerator mapping
// to two z values is not a function. Splitting Light from Sprites honours that
// row; it does not contradict it, and `spec_row` below carries the mapping back
// so the arithmetic stays checkable.
//
// The enumerator value IS the paint rank: 0 is frontmost, kBandCount - 1 is
// backmost. Ordering the enum this way makes "z decreases as the band goes back"
// a property a static_assert can walk, rather than a convention.

enum class Band : std::uint8_t {
  Overlay = 0,         ///< §4.5 row 1 — party panel overlays, hand cursor, inscriptions
  Text = 1,            ///< §4.5 row 2 — terminal text. NO image z; see carries_image
  Light = 2,           ///< §4.5 row 3, upper — the §4.4 full-frame light field
  Sprites = 3,         ///< §4.5 row 3, lower — "monster sprites beneath it"
  Geometry = 4,        ///< §4.5 row 4 — wall / floor / ceiling slot plates
  CellBackground = 5,  ///< §4.5 row 5 — ambient tint. NO image z
  BelowBackground = 6, ///< §4.5 row 6 — static frame furniture
};

inline constexpr int kBandCount = 7;

/// Ranks available within one band.
///
/// The one number in this header that is a judgement call rather than a quote
/// from §4.5, so it is worth saying where it comes from. A band needs internal
/// ordering because §3.1's ladder is five depths deep and each depth carries a
/// floor, a wall and a ceiling — fifteen geometry slots that must paint
/// far-to-near. §4.2 budgets 27 monster poses at M0. 16 sits exactly on the
/// geometry edge with no headroom; 32 is one doubling of slack.
///
/// `test/06layer/` cross-checks this against `geometry::kDepthCount` rather than
/// a static_assert here, because keeping this header a leaf that includes only
/// <cstdint> and <optional> is worth more than an in-header assertion — a driver
/// swap must be able to lift this file on its own.
inline constexpr int kBandStride = 32;

/// §4.5 — the below-background z threshold.
///
/// One below kitty's own below-background boundary of -(2^30). The decimal form
/// is spelled out rather than computed because `cmake/check_layer_z.cmake`
/// searches for it as a literal string; the static_assert immediately below
/// attaches the meaning to the magic number so the two cannot drift.
inline constexpr std::int32_t kBelowBackgroundZ = -1073741825;

/// The §4.5 table row (1..6) a band belongs to. Row 3 is shared by Light and
/// Sprites; every other row has exactly one band.
[[nodiscard]] constexpr auto spec_row(Band b) -> int {
  switch (b) {
    case Band::Overlay: return 1;
    case Band::Text: return 2;
    case Band::Light: return 3;
    case Band::Sprites: return 3;
    case Band::Geometry: return 4;
    case Band::CellBackground: return 5;
    case Band::BelowBackground: return 6;
  }
  return 0;
}

/// Whether images can be placed in this band at all.
///
/// False for Text and CellBackground. Those two rows are the terminal's own text
/// and background cells — the dividers between kitty's z regimes — so there is no
/// z that means "in" them. A caller that got one back would place an image where
/// §4.5 says only terminal cells go.
[[nodiscard]] constexpr auto carries_image(Band b) -> bool {
  switch (b) {
    case Band::Overlay:
    case Band::Light:
    case Band::Sprites:
    case Band::Geometry:
    case Band::BelowBackground:
      return true;
    case Band::Text:
    case Band::CellBackground:
      return false;
  }
  return false;
}

/// How many ranks this band accepts. Zero for the two bands that carry no image.
[[nodiscard]] constexpr auto rank_limit(Band b) -> int {
  return carries_image(b) ? kBandStride : 0;
}

/// The kitty z-index for a placement in `b` at `rank`, or nullopt.
///
/// Rank 0 is frontmost WITHIN the band, uniformly, in every band. Nullopt means
/// either the band carries no image or the rank is outside [0, rank_limit(b)) —
/// `carries_image` and `rank_limit` tell the two apart, so the single nullopt is
/// never ambiguous to a caller who cares.
///
/// The bands tile the z-space without gaps:
///
///   Overlay          [0, 31]                       (row 1: z >= 0)
///   Light            [-32, -1]                     (row 3, upper)
///   Sprites          [-64, -33]                    (row 3, lower)
///   Geometry         [-96, -65]                    (row 4)
///   BelowBackground  [-1073741856, -1073741825]    (row 6)
///
/// `rank` is deliberately a parameter rather than a single z per band. Without
/// it the compositor writes `kGeometryZ - depth` at the call site, and §4.5's
/// "inside a named layer API" is dead in practice: raw z arithmetic is back
/// outside this module, which is the thing the whole file exists to prevent.
[[nodiscard]] constexpr auto image_z(Band b, int rank) -> std::optional<std::int32_t> {
  if (!carries_image(b)) return std::nullopt;
  if (rank < 0 || rank >= rank_limit(b)) return std::nullopt;

  switch (b) {
    case Band::Overlay: return kBandStride - 1 - rank;
    case Band::Light: return -(1 + (0 * kBandStride) + rank);
    case Band::Sprites: return -(1 + (1 * kBandStride) + rank);
    case Band::Geometry: return -(1 + (2 * kBandStride) + rank);
    case Band::BelowBackground: return kBelowBackgroundZ - rank;
    case Band::Text:
    case Band::CellBackground:
      return std::nullopt;
  }
  return std::nullopt;
}

// ── §4.5's invariants, as compile-time tests ────────────────────────────────

/// Kitty's below-background boundary. Local to the asserts so no other file has
/// a reason to spell it.
inline constexpr std::int32_t kKittyBelowBackgroundBoundary = -(std::int32_t{1} << 30);

static_assert(kBelowBackgroundZ == kKittyBelowBackgroundBoundary - 1,
              "§4.5's threshold is one below kitty's below-background boundary");
static_assert(kBelowBackgroundZ + 1 == kKittyBelowBackgroundBoundary,
              "the boundary itself is NOT below background — off-by-one here is the "
              "failure this constant exists to prevent");

static_assert(static_cast<int>(Band::BelowBackground) == kBandCount - 1,
              "the backmost band is the last enumerator, or the ladder walks below is wrong");

// Row 1's constraint, checked at the WORST rank rather than the typical one:
// that is the boundary a change to kBandStride breaks.
static_assert(image_z(Band::Overlay, kBandStride - 1) >= 0, "§4.5 row 1: overlay images are z >= 0");
static_assert(image_z(Band::Overlay, 0) >= 0);

// The highest-value assertion in this file. If the back edge of the last
// negative band ever falls through kitty's cell-background boundary, every wall
// plate is drawn behind the ambient tint and the viewport goes blank — a total
// render failure produced by an arithmetic change nowhere near the renderer.
static_assert([] {
  for (const auto b : {Band::Light, Band::Sprites, Band::Geometry}) {
    for (int rank = 0; rank < kBandStride; ++rank) {
      const auto z = image_z(b, rank);
      if (!z) return false;
      if (*z >= 0) return false;                             // §4.5: these rows are z < 0
      if (*z <= kKittyBelowBackgroundBoundary) return false;  // ...but not below background
    }
  }
  return true;
}(), "§4.5's z<0 bands must stay below text and above the cell background");

// Strictly decreasing across the image-carrying bands, in paint order.
static_assert([] {
  std::optional<std::int32_t> previous;
  for (int i = 0; i < kBandCount; ++i) {
    const auto b = static_cast<Band>(i);
    if (!carries_image(b)) continue;
    const auto front = image_z(b, 0);
    if (!front) return false;
    if (previous && *front >= *previous) return false;
    previous = image_z(b, kBandStride - 1);
    if (!previous) return false;
  }
  return true;
}(), "a band further back must sit entirely below the band in front of it");

// Strictly decreasing in rank, within every band. Rank 0 is frontmost.
static_assert([] {
  for (int i = 0; i < kBandCount; ++i) {
    const auto b = static_cast<Band>(i);
    if (!carries_image(b)) continue;
    for (int rank = 1; rank < kBandStride; ++rank) {
      const auto lo = image_z(b, rank - 1);
      const auto hi = image_z(b, rank);
      if (!lo || !hi || *hi >= *lo) return false;
    }
  }
  return true;
}(), "rank 0 is frontmost within every band");

// §4.5 row 3's internal ordering: "light field, THEN monster sprites beneath
// it". The table only states this in prose, so it is asserted separately.
static_assert(image_z(Band::Light, kBandStride - 1) > image_z(Band::Sprites, 0),
              "§4.5 row 3: the whole light field sits above every sprite");

// No two distinct (band, rank) pairs may resolve to the same z. That property is
// already IMPLIED by the two asserts above — z decreases strictly within a band,
// and each band's front sits strictly below the previous band's back, so the
// ranges cannot overlap — which is why it is not restated here as a pairwise
// sweep. The O(n^2) form costs ~50k constexpr steps, blows Clang's default step
// limit, and would charge that to every translation unit that includes this
// header. `test/06layer/` runs the exhaustive pairwise check at runtime instead,
// where it is free and serves as an independent witness rather than a restatement.

// INT32_MIN from <cstdint> rather than a shift: `-1 << 31` is not a constant
// expression (left-shifting a negative value), which GCC accepted here and Clang
// correctly refused. Same class of break AGENTS.md records for fmt-under-clang-20
// — build on both, always.
static_assert(image_z(Band::BelowBackground, kBandStride - 1) > INT32_MIN + 1,
              "the back of the below-background band must not approach INT32_MIN");

// §4.5's table has six rows; this enum has seven bands. Exactly one row is
// shared, and it is row 3. Same discipline budgets.hpp applies to §4.2's
// transition-sequence row: pin the arithmetic so the table cannot stop adding up.
static_assert([] {
  int per_row[7]{};
  for (int i = 0; i < kBandCount; ++i) {
    const int row = spec_row(static_cast<Band>(i));
    if (row < 1 || row > 6) return false;
    ++per_row[row];
  }
  int shared = 0;
  for (int row = 1; row <= 6; ++row) {
    if (per_row[row] == 0) return false;
    if (per_row[row] > 2) return false;
    if (per_row[row] == 2) ++shared;
  }
  return shared == 1 && per_row[3] == 2;
}(), "§4.5's six rows are covered by seven bands, sharing row 3 and nothing else");

}  // namespace gloam::layer
