// SPEC §3 — geometry, frozen.
//
// Most of §3's invariants are static_asserts in the header, so they are already
// proven by the fact that this file compiles. What is left here is the part a
// static_assert cannot reach: the runtime reflow rules for a terminal whose
// cell is not the reference cell.

#include <catch2/catch_all.hpp>

#include "gloam/geometry.hpp"

using namespace gloam::geometry;

TEST_CASE("the depth ladder is frozen at the specified values", "[geometry]") {
  // These five pairs are quoted directly from §3.1's table. If a change to the
  // derivation rule ever moves one of them, that is a decision someone has to
  // make deliberately, not a refactor.
  CHECK(kDepths[0] == Ring{480, 360});
  CHECK(kDepths[1] == Ring{336, 252});
  CHECK(kDepths[2] == Ring{240, 180});
  CHECK(kDepths[3] == Ring{168, 126});
  CHECK(kDepths[4] == Ring{120, 90});
}

TEST_CASE("the 1/sqrt(2) ladder holds to the ratios §3.1 claims", "[geometry]") {
  // §3.1 states 0.700 and 0.714 alternating. Checked in integer arithmetic
  // scaled by 1000 — a float comparison here would be the only float in the
  // whole simulation-adjacent test suite, and §5.1 is worth honouring even in
  // a test.
  for (int d = 1; d < kDepthCount; ++d) {
    const int ratio = kDepths[d].width * 1000 / kDepths[d - 1].width;
    INFO("depth " << d << " ratio x1000 = " << ratio);
    CHECK(ratio >= 699);
    CHECK(ratio <= 715);
  }
}

TEST_CASE("cells_to_cover rounds up, so the viewport is never clipped", "[geometry]") {
  CHECK(cells_to_cover(480, 10) == 48);  // exact at the reference cell
  CHECK(cells_to_cover(480, 7) == 69);   // 68.57 -> 69, overhang painted as background
  CHECK(cells_to_cover(360, 20) == 18);
  CHECK(cells_to_cover(360, 16) == 23);

  // A degenerate cell size is a terminal that has not answered yet, not a
  // divide-by-zero.
  CHECK(cells_to_cover(480, 0) == 0);
  CHECK(cells_to_cover(480, -1) == 0);
}

TEST_CASE("the reference terminal is the universal floor", "[geometry]") {
  CHECK(fits(kReferenceCols, kReferenceRows, kReferenceCellWidthPx, kReferenceCellHeightPx));

  // §3.2's refusal condition is precise, and narrower than the reference
  // layout: "If the window cannot give 480 x 360 px to the viewport PLUS 6 cell
  // rows of chrome, GLOAM refuses to start."
  //
  // So the binding constraint is the VIEWPORT's 48 columns, not the reference
  // grid's 80. The rail (cols 50-80) reflows and is not part of the floor — a
  // 48-column terminal starts, with no room for a rail. Whether that should
  // also be a refusal is a §7 question, not a §3 one.
  CHECK(fits(kViewport.cols(), kReferenceRows, kReferenceCellWidthPx, kReferenceCellHeightPx));
  CHECK_FALSE(
      fits(kViewport.cols() - 1, kReferenceRows, kReferenceCellWidthPx, kReferenceCellHeightPx));

  // Rows are the tighter constraint at the reference cell: 18 for the viewport
  // plus 6 of chrome is exactly 24, with nothing spare.
  CHECK(cells_to_cover(kViewportHeightPx, kReferenceCellHeightPx) + kChromeRows == kReferenceRows);
  CHECK_FALSE(
      fits(kReferenceCols, kReferenceRows - 1, kReferenceCellWidthPx, kReferenceCellHeightPx));
}

TEST_CASE("a smaller cell needs more cells but the same pixels", "[geometry]") {
  // §3.2: "The viewport keeps its 480 x 360 px extent and simply covers a
  // different number of cells." A dense 6x12 cell needs 80 columns for the
  // viewport alone — which no longer fits an 80-column terminal once the rail
  // is accounted for, and must therefore refuse.
  CHECK(cells_to_cover(kViewportWidthPx, 6) == 80);
  CHECK(fits(80, 36, 6, 12));
  CHECK_FALSE(fits(79, 36, 6, 12));

  // A large cell needs fewer columns, and the floor is easy to clear.
  CHECK(cells_to_cover(kViewportWidthPx, 20) == 24);
  CHECK(fits(40, 24, 20, 20));
}

TEST_CASE("the chrome rows are the party strip plus the status line", "[geometry]") {
  CHECK(kPartyStrip.rows() == 5);
  CHECK(kStatusLine.rows() == 1);
  CHECK(kChromeRows == 6);
  // Four 20-column panels (§3.2).
  CHECK(kPartyStrip.cols() / 4 == 20);
}
