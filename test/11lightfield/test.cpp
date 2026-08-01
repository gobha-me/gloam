// SPEC §4.4 — the six full-frame light fields.
//
// §4.4's claim is structural, not decorative: "in a fixed-slot compositor,
// screen position IS depth." The tests that matter here are the ones that hold
// the arithmetic to that claim — that the band knots come from
// `geometry::kDepths` and nowhere else, that a brighter lamp never darkens a
// pixel, and that the field is exactly symmetric about the vanishing point.
//
// The last one is worth its own sentence. `radius2` works in DOUBLED pixel
// units so the vanishing point lands on a lattice point; with an even extent,
// naive integer halving puts it half a pixel off centre, and the resulting bias
// is invisible in a screenshot and permanent in the pack. A symmetry sweep is
// the only thing that catches it.
//
// Failure matrix first, per AGENTS.md.

#include <catch2/catch_all.hpp>

#include <climits>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "gloam/budgets.hpp"
#include "gloam/dither.hpp"
#include "gloam/geometry.hpp"
#include "gloam/lightfield.hpp"
#include "gloam/plate.hpp"
#include "gloam/tuning.hpp"

using namespace gloam;
using gloam::lightfield::BakeError;

namespace {

[[nodiscard]] auto fresh_blob() -> std::vector<std::byte> {
  return std::vector<std::byte>(
      plate::blob_bytes(lightfield::kWidthPx, lightfield::kHeightPx), std::byte{0});
}

[[nodiscard]] auto as_view(const std::vector<std::byte>& b) -> plate::PlateView {
  return {std::span<const std::byte>{b}, lightfield::kWidthPx, lightfield::kHeightPx};
}

}  // namespace

// ── The failure matrix ──────────────────────────────────────────────────────

TEST_CASE("a lamp level outside the table is refused", "[lightfield]") {
  auto blob = fresh_blob();
  for (const int level : {-1, kLampLevelMax + 1, INT_MIN, INT_MAX}) {
    INFO("level " << level);
    const auto res = lightfield::bake(level, blob);
    CHECK(res.error == BakeError::LampLevelOutOfRange);
    CHECK(res.bytes == 0);
    CHECK(res.opaque_pixels == 0);
  }

  // Both ends of the real range are inside it.
  CHECK(lightfield::bake(kLampLevelMin, blob));
  CHECK(lightfield::bake(kLampLevelMax, blob));
}

TEST_CASE("a blob one byte short is refused and left untouched", "[lightfield]") {
  auto blob = fresh_blob();
  blob.pop_back();
  const auto before = blob;

  const auto res = lightfield::bake(kLampLevelDefault, blob);
  CHECK(res.error == BakeError::BufferTooSmall);
  CHECK(res.bytes == 0);
  CHECK(blob == before);
}

TEST_CASE("a refused bake leaves an oversized blob untouched too", "[lightfield]") {
  // The level check runs before the buffer is touched, so an otherwise-valid
  // buffer must survive a bad level. A bake that zeroed first and validated
  // second would destroy a caller's plate on a typo'd argument.
  auto blob = fresh_blob();
  REQUIRE(lightfield::bake(kLampLevelMax, blob));
  const auto baked = blob;

  CHECK_FALSE(lightfield::bake(-3, blob));
  CHECK(blob == baked);
}

// ── L0: opaque by construction, not by a special case ───────────────────────

TEST_CASE("the doused field is totally opaque", "[lightfield]") {
  // §4.4's table: "0 — doused | 0 | L0 (opaque)". Asserted two ways, because a
  // partial bake could fake either one alone: every stencil byte is 0xFF, AND
  // the reported count is the whole frame.
  auto blob = fresh_blob();
  const auto res = lightfield::bake(kLampLevelMin, blob);
  REQUIRE(res);
  CHECK(res.opaque_pixels ==
        static_cast<std::size_t>(lightfield::kWidthPx) * lightfield::kHeightPx);

  const auto stencil_base = plate::index_plane_bytes(lightfield::kWidthPx, lightfield::kHeightPx);
  for (std::size_t i = stencil_base; i < blob.size(); ++i) {
    INFO("stencil byte " << (i - stencil_base));
    REQUIRE(blob[i] == std::byte{0xFF});
  }

  // 480 divides by 8, so there are no pad bits to be lenient about.
  CHECK(lightfield::kWidthPx % 8 == 0);
}

TEST_CASE("the index plane carries no colour, at any level", "[lightfield]") {
  // A light field that tinted would recolour the geometry beneath it. The
  // stencil is the whole mechanism; the index plane is uniformly the shadow
  // entry.
  const auto index_bytes =
      plate::index_plane_bytes(lightfield::kWidthPx, lightfield::kHeightPx);
  for (int level = kLampLevelMin; level <= kLampLevelMax; ++level) {
    auto blob = fresh_blob();
    REQUIRE(lightfield::bake(level, blob));
    for (std::size_t i = 0; i < index_bytes; ++i) {
      INFO("level " << level << " index byte " << i);
      REQUIRE(blob[i] == std::byte{0});
    }
  }
}

// ── The knots, derived from the ladder ──────────────────────────────────────

TEST_CASE("band knots are the ladder's ring heights, squared", "[lightfield]") {
  // This is what makes "screen position is depth" a fact about the code rather
  // than a claim in a comment. Change geometry::kDepths without changing the
  // fields and this test is what says so.
  for (int d = 0; d < geometry::kDepthCount; ++d) {
    const std::int64_t h = geometry::kDepths[static_cast<std::size_t>(d)].height;
    INFO("depth " << d);
    CHECK(lightfield::knot_radius2(d) == h * h);
  }
  CHECK(lightfield::knot_radius2(geometry::kDepthCount) == 0);

  // Strictly receding, like the ladder it comes from.
  for (int d = 1; d <= geometry::kDepthCount; ++d) {
    INFO("depth " << d);
    CHECK(lightfield::knot_radius2(d) < lightfield::knot_radius2(d - 1));
  }
}

TEST_CASE("band 0 is reachable only left and right of centre", "[lightfield]") {
  // §4.4: "The centre of the frame is far; the edges are near." The near band
  // starts beyond the largest vertical radius in the frame, so the only way to
  // reach depth 0 is horizontally — which is the claim, falling out of the
  // arithmetic rather than being imposed on it.
  const auto max_dy2 = lightfield::radius2(lightfield::kWidthPx / 2, 0);
  CHECK(max_dy2 < lightfield::knot_radius2(0));
  CHECK(lightfield::radius2(0, lightfield::kHeightPx / 2) > lightfield::knot_radius2(0));
}

TEST_CASE("knot coverage is monotone in depth and saturates at both ends", "[lightfield]") {
  for (int level = kLampLevelMin; level <= kLampLevelMax; ++level) {
    for (int d = 1; d <= geometry::kDepthCount; ++d) {
      INFO("level " << level << " depth " << d);
      CHECK(lightfield::knot_coverage(level, d) >= lightfield::knot_coverage(level, d - 1));
    }
    for (int d = 0; d <= geometry::kDepthCount; ++d) {
      INFO("level " << level << " depth " << d);
      CHECK(lightfield::knot_coverage(level, d) >= 0);
      CHECK(lightfield::knot_coverage(level, d) <= dither::kLevels);
    }
  }

  // L0 is opaque at every knot, which is why the bake needs no doused branch.
  for (int d = 0; d <= geometry::kDepthCount; ++d) {
    CHECK(lightfield::knot_coverage(kLampLevelMin, d) == dither::kLevels);
  }
  // The brightest lamp is fully clear at the near band.
  CHECK(lightfield::knot_coverage(kLampLevelMax, 0) == 0);
}

TEST_CASE("exactly one cell of reach spare is the half-dithered band", "[lightfield]") {
  // §4.4's falloff band, pinned. `kFalloffPerCell` is dither::kLevels / 2
  // precisely so that this lands on the midpoint — the screen door at its most
  // visible, which is what makes the edge of the light read as an edge.
  //
  // Without this, kFalloffPerCell could be changed to 21 or 48, the whole ramp
  // would rescale, the half-dithered band would disappear, and every other test
  // in this file would still pass. The only other signal is that pack_sha256
  // moved, which is indistinguishable from an intentional art change.
  CHECK(lightfield::kFalloffPerCell == dither::kLevels / 2);

  for (int level = kLampLevelMin; level <= kLampLevelMax; ++level) {
    for (int d = 0; d < geometry::kDepthCount; ++d) {
      const int spare = level - d;
      const auto coverage = lightfield::knot_coverage(level, d);
      INFO("L" << level << " at depth " << d << " has " << spare << " cells spare");
      if (spare == 1) {
        REQUIRE(coverage == dither::kLevels / 2);
      } else if (spare >= 2) {
        REQUIRE(coverage == 0);
      } else {
        REQUIRE(coverage == dither::kLevels);
      }
    }
  }
}

TEST_CASE("the far cap is a flat plateau, not a ramp to nowhere", "[lightfield]") {
  // geometry.hpp: depth 4 "is a cap plate, not a navigable ring". There is no
  // depth 5 to ramp toward, so everything inside the depth-4 knot is uniformly
  // as far as anything gets — a disc of roughly 45 px around the vanishing
  // point. Asserted so that the flatness is a decision rather than an accident
  // of knot_coverage's clamp.
  constexpr int kCap = geometry::kDepthCount - 1;
  for (int level = kLampLevelMin; level <= kLampLevelMax; ++level) {
    const auto want = lightfield::knot_coverage(level, kCap);
    for (int y = 0; y < lightfield::kHeightPx; ++y) {
      for (int x = 0; x < lightfield::kWidthPx; ++x) {
        if (lightfield::radius2(x, y) >= lightfield::knot_radius2(kCap)) continue;
        if (lightfield::coverage_at(level, x, y) != want) {
          INFO("level " << level << " at (" << x << ", " << y << ")");
          FAIL();
        }
      }
    }
  }
  SUCCEED();
}

TEST_CASE("the interpolator agrees with its own endpoints", "[lightfield]") {
  // A ramp that does not meet the knots it ramps between produces a seam at
  // every band boundary — visible, and exactly the kind of artefact that gets
  // blamed on the dither.
  for (int level = kLampLevelMin; level <= kLampLevelMax; ++level) {
    for (int d = 0; d < geometry::kDepthCount; ++d) {
      // Walk the vertical axis, where r2 is a clean function of y, and find the
      // pixel nearest each knot radius.
      const auto target = lightfield::knot_radius2(d);
      if (target > lightfield::radius2(lightfield::kWidthPx / 2, 0)) continue;
      int best_y = 0;
      std::int64_t best_gap = INT64_MAX;
      for (int y = 0; y < lightfield::kHeightPx; ++y) {
        const auto gap = std::abs(lightfield::radius2(lightfield::kWidthPx / 2, y) - target);
        if (gap < best_gap) {
          best_gap = gap;
          best_y = y;
        }
      }
      const auto got = lightfield::coverage_at(level, lightfield::kWidthPx / 2, best_y);
      const auto want = lightfield::knot_coverage(level, d);
      INFO("level " << level << " depth " << d << " at y=" << best_y << " gap=" << best_gap);
      // Within one coverage step of the knot: the nearest pixel is not exactly
      // on the boundary, so this is a proximity check, not an equality one.
      CHECK(std::abs(got - want) <= 1);
    }
  }
}

// ── The field's shape, over every pixel ─────────────────────────────────────

TEST_CASE("coverage stays inside the dither's range everywhere", "[lightfield][property]") {
  for (int level = kLampLevelMin; level <= kLampLevelMax; ++level) {
    for (int y = 0; y < lightfield::kHeightPx; ++y) {
      for (int x = 0; x < lightfield::kWidthPx; ++x) {
        const auto c = lightfield::coverage_at(level, x, y);
        if (c < 0 || c > dither::kLevels) {
          INFO("level " << level << " at (" << x << ", " << y << ") gave " << c);
          FAIL();
        }
      }
    }
  }
  SUCCEED();
}

TEST_CASE("a brighter lamp never darkens a pixel", "[lightfield][property]") {
  // The whole model inverted, if it fails. §4.4 ties the field plate and the
  // perception model to the same integer, so a non-monotone field would mean
  // raising the lamp made the party harder to see.
  for (int level = kLampLevelMin; level < kLampLevelMax; ++level) {
    for (int y = 0; y < lightfield::kHeightPx; ++y) {
      for (int x = 0; x < lightfield::kWidthPx; ++x) {
        if (lightfield::coverage_at(level + 1, x, y) > lightfield::coverage_at(level, x, y)) {
          INFO("(" << x << ", " << y << ") got brighter-but-darker from L" << level << " to L"
                   << (level + 1));
          FAIL();
        }
      }
    }
  }
  SUCCEED();
}

TEST_CASE("opacity never rises with distance from the vanishing point",
          "[lightfield][property]") {
  // Hole density grows OUTWARD (§4.4). Checked along both axes from the centre,
  // where r2 is monotone by construction.
  for (int level = kLampLevelMin; level <= kLampLevelMax; ++level) {
    const int cy = lightfield::kHeightPx / 2;
    for (int x = lightfield::kWidthPx / 2; x + 1 < lightfield::kWidthPx; ++x) {
      INFO("level " << level << " along y=" << cy << " at x=" << x);
      REQUIRE(lightfield::coverage_at(level, x + 1, cy) <= lightfield::coverage_at(level, x, cy));
    }
    const int cx = lightfield::kWidthPx / 2;
    for (int y = lightfield::kHeightPx / 2; y + 1 < lightfield::kHeightPx; ++y) {
      INFO("level " << level << " along x=" << cx << " at y=" << y);
      REQUIRE(lightfield::coverage_at(level, cx, y + 1) <= lightfield::coverage_at(level, cx, y));
    }
  }
}

TEST_CASE("the field is exactly symmetric about the vanishing point",
          "[lightfield][property]") {
  // The doubled-coordinate check. A half-pixel bias fails here and nowhere
  // else — it is far too small to see and it is baked into every plate.
  for (int level = kLampLevelMin; level <= kLampLevelMax; ++level) {
    for (int y = 0; y < lightfield::kHeightPx; ++y) {
      for (int x = 0; x < lightfield::kWidthPx; ++x) {
        const auto c = lightfield::coverage_at(level, x, y);
        const auto mirror_x = lightfield::coverage_at(level, lightfield::kWidthPx - 1 - x, y);
        const auto mirror_y = lightfield::coverage_at(level, x, lightfield::kHeightPx - 1 - y);
        if (c != mirror_x || c != mirror_y) {
          INFO("level " << level << " at (" << x << ", " << y << "): " << c << " vs " << mirror_x
                        << " / " << mirror_y);
          FAIL();
        }
      }
    }
  }
  SUCCEED();
}

TEST_CASE("radius2 is exact at the corners and does not overflow", "[lightfield]") {
  CHECK(lightfield::radius2(0, 0) == 479LL * 479 + 359LL * 359);
  CHECK(lightfield::radius2(0, 0) == 358322);
  CHECK(lightfield::radius2(lightfield::kWidthPx - 1, lightfield::kHeightPx - 1) == 358322);
  CHECK(lightfield::radius2(lightfield::kWidthPx - 1, 0) == 358322);

  // The two centre pixels either side of the vanishing point are equidistant —
  // the property doubling the units exists to give.
  CHECK(lightfield::radius2(239, 179) == lightfield::radius2(240, 180));
  CHECK(lightfield::radius2(239, 179) == 2);
}

// ── The bake itself ─────────────────────────────────────────────────────────

TEST_CASE("the packed bake agrees with the pixel-at-a-time model", "[lightfield]") {
  // `bake` packs stencil bytes directly for speed. This is what keeps the fast
  // path honest: every pixel read back through plate::read must equal what
  // dither::opaque_at(coverage_at(...)) says it should be.
  for (int level = kLampLevelMin; level <= kLampLevelMax; ++level) {
    auto blob = fresh_blob();
    const auto res = lightfield::bake(level, blob);
    REQUIRE(res);

    std::size_t counted = 0;
    for (int y = 0; y < lightfield::kHeightPx; ++y) {
      for (int x = 0; x < lightfield::kWidthPx; ++x) {
        const auto px = plate::read(as_view(blob), x, y);
        const bool want = dither::opaque_at(lightfield::coverage_at(level, x, y), x, y);
        if (px.error != plate::PlateError::None || px.opaque != want ||
            px.ink != plate::Ink::Ink0) {
          INFO("level " << level << " at (" << x << ", " << y << ")");
          FAIL();
        }
        if (px.opaque) ++counted;
      }
    }
    INFO("level " << level);
    CHECK(counted == res.opaque_pixels);
  }
}

TEST_CASE("a brighter lamp leaves fewer opaque pixels", "[lightfield]") {
  // The per-pixel monotonicity above implies this, but the aggregate is what a
  // human reads when the numbers change, and it is the one line of output the
  // baker prints per plate.
  std::size_t previous = SIZE_MAX;
  for (int level = kLampLevelMin; level <= kLampLevelMax; ++level) {
    auto blob = fresh_blob();
    const auto res = lightfield::bake(level, blob);
    REQUIRE(res);
    INFO("level " << level << " has " << res.opaque_pixels << " opaque");
    CHECK(res.opaque_pixels < previous);
    previous = res.opaque_pixels;
  }
  // Even the brightest lamp leaves the vanishing point dark: there is always
  // somewhere further than the light reaches.
  CHECK(previous > 0);
}

TEST_CASE("two bakes of the same level are byte-identical", "[lightfield]") {
  // §10 in miniature. The whole-pack version lives in test/12pack/ and in the
  // `pack-reproducible` ctest case; this is the unit that has to hold first.
  for (int level = kLampLevelMin; level <= kLampLevelMax; ++level) {
    auto a = fresh_blob();
    auto b = fresh_blob();
    REQUIRE(lightfield::bake(level, a));
    REQUIRE(lightfield::bake(level, b));
    INFO("level " << level);
    CHECK(a == b);
  }
}

TEST_CASE("the six fields match §4.2's light-field row and §11's payload", "[lightfield]") {
  CHECK(lightfield::kFieldCount == kLampLevelCount);
  CHECK(lightfield::kFieldCount == budget::kLightFields.m0);
  CHECK(lightfield::kFieldCount == budget::kLightFields.full);
  CHECK(lightfield::kWidthPx == geometry::kViewportWidthPx);
  CHECK(lightfield::kHeightPx == geometry::kViewportHeightPx);

  const auto one = plate::blob_bytes(lightfield::kWidthPx, lightfield::kHeightPx);
  CHECK(one == 64'800);
  CHECK(one * lightfield::kFieldCount == 388'800);
}
