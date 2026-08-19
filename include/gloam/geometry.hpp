#pragma once

/// SPEC §3 — geometry. Frozen.
///
/// Everything downstream depends on these numbers and they are expensive to
/// change later, so they are decided here rather than in M0. The rule that
/// makes the art volume solo-sized: each depth is 1/sqrt(2) of the one before
/// it, so every second depth is exactly half. The offline pipeline (§10)
/// authors the depth-0 and depth-1 rings and derives 2-4 by exact 2:1
/// downsample.
///
/// Every relationship the prose asserts is a static_assert at the bottom of
/// this file. A ladder that stops being self-consistent stops compiling.

#include <array>
#include <cstdint>

namespace gloam::geometry {

/// A plate ring: the front-face extent of one depth, in pixels.
struct Ring {
  int width{};
  int height{};

  [[nodiscard]] constexpr auto operator==(const Ring&) const -> bool = default;
};

/// Sight distance in cells. Depth 4 is a cap plate, not a navigable ring —
/// you can see it, you can never stand in it.
inline constexpr int kSightDistance = 4;

/// Depths 0 (own cell) through 4 (far cap).
inline constexpr int kDepthCount = 5;

/// §3.1 — the depth ladder.
inline constexpr std::array<Ring, kDepthCount> kDepths{{
    {480, 360},  // 0 — own cell     · authored
    {336, 252},  // 1                · authored
    {240, 180},  // 2                · depth 0 / 2
    {168, 126},  // 3                · depth 1 / 2
    {120, 90},   // 4 — far cap      · depth 2 / 2
}};

/// The dither cell. §4.3 requires that quantization happen offline and that
/// nothing is ever resampled at runtime; a plate width that is not a whole
/// number of dither cells would land the pattern on a fractional boundary and
/// reintroduce exactly the crawl the pre-baking exists to avoid.
inline constexpr int kDitherCell = 8;
inline constexpr int kDitherBlock = 24;

// ── §3.2 Screen layout ──────────────────────────────────────────────────────
//
// The viewport is defined in PIXELS and never resampled. Everything else is
// defined in CELLS and reflows. The reference terminal is an 80x24 grid at a
// 10x20 px cell — an 800x480 px window, which is the universal floor.

/// The viewport's pixel extent. Identical to the depth-0 ring by construction:
/// the near plate fills the frame.
inline constexpr int kViewportWidthPx = 480;
inline constexpr int kViewportHeightPx = 360;

/// The reference cell. Real terminals differ; §3.2 handles that by keeping the
/// viewport's pixel extent fixed and covering a different number of cells,
/// never by scaling the plates.
inline constexpr int kReferenceCellWidthPx = 10;
inline constexpr int kReferenceCellHeightPx = 20;

/// The reference grid, and the floor GLOAM refuses to start below (§14, GL-D2 /
/// upstream #91, landed in termforge v0.30.0).
inline constexpr int kReferenceCols = 80;
inline constexpr int kReferenceRows = 24;

/// Rows of chrome below the viewport: the party strip (§7.2) plus the status
/// line. The viewport needs its 480x360 px on top of these or there is no game.
inline constexpr int kChromeRows = 6;

/// A screen region, in 1-based cell coordinates inclusive of both bounds, as
/// §3.2's table states them.
struct CellRegion {
  int first_col{};
  int last_col{};
  int first_row{};
  int last_row{};

  [[nodiscard]] constexpr auto cols() const -> int { return last_col - first_col + 1; }
  [[nodiscard]] constexpr auto rows() const -> int { return last_row - first_row + 1; }
  [[nodiscard]] constexpr auto operator==(const CellRegion&) const -> bool = default;
};

/// The compositor's region. Plates only — no text ever lands here.
inline constexpr CellRegion kViewport{1, 48, 1, 18};
/// Messages, item labels, rune entry.
inline constexpr CellRegion kRail{50, 80, 1, 18};
/// Four 20-column panels, persistent and never modal (§7.2).
inline constexpr CellRegion kPartyStrip{1, 80, 19, 23};
/// Hand cursor contents, fuel, weight.
inline constexpr CellRegion kStatusLine{1, 80, 24, 24};

/// Cells needed to cover `px` pixels at a cell pitch of `cell_px`, rounded up.
/// The overhang is painted as background and sub-cell alignment uses kitty's
/// X=/Y= pixel offsets — never c=/r= cell scaling, which resamples (§3.2).
[[nodiscard]] constexpr auto cells_to_cover(int px, int cell_px) -> int {
  return cell_px <= 0 ? 0 : (px + cell_px - 1) / cell_px;
}

/// Whether a terminal of this cell geometry can host GLOAM at all: the viewport
/// needs its full pixel extent AND `kChromeRows` rows beneath it. Below this,
/// GLOAM refuses to start rather than degrading — §17, and the whole point of
/// upstream #91, landed in termforge v0.30.0.
[[nodiscard]] constexpr auto fits(int cols, int rows, int cell_w_px, int cell_h_px) -> bool {
  if (cell_w_px <= 0 || cell_h_px <= 0) return false;
  const int need_cols = cells_to_cover(kViewportWidthPx, cell_w_px);
  const int need_rows = cells_to_cover(kViewportHeightPx, cell_h_px) + kChromeRows;
  return cols >= need_cols && rows >= need_rows;
}

// ── The ladder's invariants, as compile-time tests ──────────────────────────

static_assert(kDepths[0] == Ring{kViewportWidthPx, kViewportHeightPx},
              "the depth-0 plate is the viewport: the near ring fills the frame");

// "each depth is 1/sqrt(2) of the one before it, so every second depth is
// exactly half" — the half-steps are exact integer division, which is what lets
// the pipeline derive rings 2-4 instead of authoring them.
static_assert(kDepths[2].width == kDepths[0].width / 2 && kDepths[2].height == kDepths[0].height / 2,
              "depth 2 is depth 0 downsampled 2:1");
static_assert(kDepths[3].width == kDepths[1].width / 2 && kDepths[3].height == kDepths[1].height / 2,
              "depth 3 is depth 1 downsampled 2:1");
static_assert(kDepths[4].width == kDepths[2].width / 2 && kDepths[4].height == kDepths[2].height / 2,
              "depth 4 is depth 2 downsampled 2:1");

// A 2:1 downsample is only exact if the parent is even on both axes.
static_assert(kDepths[0].width % 2 == 0 && kDepths[0].height % 2 == 0);
static_assert(kDepths[1].width % 2 == 0 && kDepths[1].height % 2 == 0);
static_assert(kDepths[2].width % 2 == 0 && kDepths[2].height % 2 == 0);

// "Every width is divisible by 8 and by 24, so the dither cell never lands on a
// fractional boundary."
static_assert([] {
  for (const auto& r : kDepths) {
    if (r.width % kDitherCell != 0) return false;
    if (r.width % kDitherBlock != 0) return false;
  }
  return true;
}(), "a plate width that is not a whole number of dither cells reintroduces dither crawl");

// Every ring is 4:3. A ring that drifted would make the derived depths
// non-square-pixel against the authored ones.
static_assert([] {
  for (const auto& r : kDepths) {
    if (r.width * 3 != r.height * 4) return false;
  }
  return true;
}(), "every ring is 4:3");

// Strictly receding: a nearer depth is always larger than a farther one.
static_assert([] {
  for (int d = 1; d < kDepthCount; ++d) {
    if (kDepths[d].width >= kDepths[d - 1].width) return false;
    if (kDepths[d].height >= kDepths[d - 1].height) return false;
  }
  return true;
}(), "the ladder recedes monotonically");

// The regions tile the reference grid without overlapping, and the viewport's
// cell extent at the reference cell is exactly its pixel extent.
static_assert(kViewport.cols() * kReferenceCellWidthPx == kViewportWidthPx);
static_assert(kViewport.rows() * kReferenceCellHeightPx == kViewportHeightPx);
static_assert(kRail.first_col > kViewport.last_col, "the rail clears the viewport");
static_assert(kPartyStrip.first_row > kViewport.last_row, "the party strip clears the viewport");
static_assert(kPartyStrip.cols() % 4 == 0, "the party strip is four equal panels");
static_assert(kStatusLine.first_row == kPartyStrip.last_row + 1);
static_assert(kStatusLine.last_row == kReferenceRows, "the status line is the last row");
static_assert(kPartyStrip.rows() + kStatusLine.rows() == kChromeRows,
              "chrome is the party strip plus the status line");
static_assert(fits(kReferenceCols, kReferenceRows, kReferenceCellWidthPx, kReferenceCellHeightPx),
              "the reference terminal is the universal floor, so it must fit");

}  // namespace gloam::geometry
