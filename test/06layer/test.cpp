// SPEC §4.5 — the compositing bands, and the z-index policy over them.
//
// Most of §4.5's ordering invariants are static_asserts in layer.hpp, so they are
// already proven by the fact that this file compiles. What is left here is the
// part a static_assert is a bad shape for: the total-function behaviour of
// image_z under inputs no correct caller would pass, and the cross-header claim
// that the band is actually wide enough for §3.1's ladder.
//
// Failure matrix first, per AGENTS.md. The §4.5 table itself is checked last,
// because a table that reads back correctly under well-formed input proves the
// least of anything in this file.

#include <catch2/catch_all.hpp>

#include <cstdint>
#include <limits>

#include "gloam/geometry.hpp"
#include "gloam/layer.hpp"

using namespace gloam;
using gloam::layer::Band;

// ── The two bands that carry no image ───────────────────────────────────────

TEST_CASE("§4.5's text and cell-background rows have no image z at all", "[layer]") {
  // These two rows are not layers GLOAM chose — they are the dividers between
  // kitty's three z regimes. A caller that got a number back would place an
  // image where §4.5 says only terminal cells go, and it would look plausible.
  CHECK_FALSE(layer::carries_image(Band::Text));
  CHECK_FALSE(layer::carries_image(Band::CellBackground));

  CHECK_FALSE(layer::image_z(Band::Text, 0).has_value());
  CHECK_FALSE(layer::image_z(Band::CellBackground, 0).has_value());

  // Not just at rank 0 — nothing in the space works.
  for (int rank = -2; rank <= layer::kBandStride + 1; ++rank) {
    INFO("rank " << rank);
    CHECK_FALSE(layer::image_z(Band::Text, rank).has_value());
    CHECK_FALSE(layer::image_z(Band::CellBackground, rank).has_value());
  }

  // Zero ranks available, so a caller sizing a loop off rank_limit never asks.
  CHECK(layer::rank_limit(Band::Text) == 0);
  CHECK(layer::rank_limit(Band::CellBackground) == 0);
}

TEST_CASE("carries_image and the band list cannot drift apart", "[layer]") {
  int carrying = 0;
  for (int i = 0; i < layer::kBandCount; ++i) {
    const auto b = static_cast<Band>(i);
    INFO("band " << i);
    // The predicate and image_z must agree in both directions, or one of them
    // is lying about a band and the other is the only thing that notices.
    CHECK(layer::carries_image(b) == layer::image_z(b, 0).has_value());
    CHECK(layer::rank_limit(b) == (layer::carries_image(b) ? layer::kBandStride : 0));
    if (layer::carries_image(b)) ++carrying;
  }
  CHECK(carrying == 5);
  CHECK(layer::kBandCount == 7);
}

// ── Ranks outside the band ──────────────────────────────────────────────────

TEST_CASE("a rank outside its band yields nothing, at both ends", "[layer]") {
  // One past the end.
  CHECK_FALSE(layer::image_z(Band::Geometry, layer::kBandStride).has_value());
  // ...and the last one that is inside it.
  CHECK(layer::image_z(Band::Geometry, layer::kBandStride - 1).has_value());

  // A negative rank is the dangerous one and it is not a crash. The formulas
  // are linear in rank, so rank -1 would push a plate one step FORWARD, out of
  // its band and into the back of the band in front — a silent z-collision with
  // whatever lives there, which is the worst kind of render bug to chase.
  CHECK_FALSE(layer::image_z(Band::Geometry, -1).has_value());
  CHECK(layer::image_z(Band::Geometry, 0).has_value());

  for (int i = 0; i < layer::kBandCount; ++i) {
    const auto b = static_cast<Band>(i);
    INFO("band " << i);
    CHECK_FALSE(layer::image_z(b, -1).has_value());
    CHECK_FALSE(layer::image_z(b, layer::kBandStride).has_value());
  }
}

TEST_CASE("the rank bounds check does not itself overflow", "[layer]") {
  // A bounds check written as `rank * stride` rather than a comparison would be
  // UB here, and UB that only fires on an input nobody passes is UB that ships.
  constexpr int kMin = std::numeric_limits<int>::min();
  constexpr int kMax = std::numeric_limits<int>::max();

  for (int i = 0; i < layer::kBandCount; ++i) {
    const auto b = static_cast<Band>(i);
    INFO("band " << i);
    CHECK_FALSE(layer::image_z(b, kMin).has_value());
    CHECK_FALSE(layer::image_z(b, kMax).has_value());
  }
}

TEST_CASE("a band value outside the enumeration is refused, not fallen off", "[layer]") {
  // Band is a uint8_t enum, so a cast can produce a value no enumerator names.
  // Every switch in layer.hpp has to have somewhere to land; a switch that runs
  // off the end returns an indeterminate value, which here would be a z-index.
  const auto bogus = static_cast<Band>(200);
  CHECK_FALSE(layer::carries_image(bogus));
  CHECK(layer::rank_limit(bogus) == 0);
  CHECK_FALSE(layer::image_z(bogus, 0).has_value());
  CHECK(layer::spec_row(bogus) == 0);

  const auto edge = static_cast<Band>(layer::kBandCount);
  CHECK_FALSE(layer::image_z(edge, 0).has_value());
}

// ── The z regimes, at their boundaries ──────────────────────────────────────

TEST_CASE("§4.5 row 1 stays at z >= 0 even at the worst rank", "[layer]") {
  // The typical case proves nothing; kBandStride - 1 is the rank a change to
  // the stride breaks first.
  for (int rank = 0; rank < layer::kBandStride; ++rank) {
    const auto z = layer::image_z(Band::Overlay, rank);
    REQUIRE(z.has_value());
    INFO("rank " << rank << " -> z " << *z);
    CHECK(*z >= 0);
  }
}

TEST_CASE("§4.5's z<0 bands stay below text and above the cell background", "[layer]") {
  // The failure this prevents is total and silent: if the back edge of the last
  // negative band ever falls through kitty's below-background boundary, every
  // wall plate is drawn behind the ambient tint and the viewport goes blank.
  // Nothing errors; the screen is just empty.
  constexpr std::int32_t kBoundary = -(std::int32_t{1} << 30);

  for (const auto b : {Band::Light, Band::Sprites, Band::Geometry}) {
    for (int rank = 0; rank < layer::kBandStride; ++rank) {
      const auto z = layer::image_z(b, rank);
      REQUIRE(z.has_value());
      INFO("band " << static_cast<int>(b) << " rank " << rank << " -> z " << *z);
      CHECK(*z < 0);
      CHECK(*z > kBoundary);
      CHECK(*z > layer::kBelowBackgroundZ);
    }
  }
}

TEST_CASE("the below-background threshold is exactly §4.5's number, and one below kitty's",
          "[layer]") {
  // This is the value assertion cmake/check_layer_z.cmake is the grep half of.
  // §19 step 2's acceptance criterion is that the literal appears exactly once
  // in the SHIPPED tree; the checker scopes itself to include/ and src/ so this
  // file may name it, and naming it here is what stops the constant and the
  // checker drifting apart.
  CHECK(layer::kBelowBackgroundZ == -1073741825);

  constexpr std::int32_t kBoundary = -(std::int32_t{1} << 30);
  CHECK(layer::kBelowBackgroundZ == kBoundary - 1);

  // The off-by-one, stated from the other side: the boundary itself is NOT
  // below background. A threshold that is one out puts the static frame
  // furniture in front of the cell backgrounds instead of behind them.
  CHECK(layer::kBelowBackgroundZ + 1 == kBoundary);
  CHECK_FALSE(layer::kBelowBackgroundZ + 1 < kBoundary);

  CHECK(layer::image_z(Band::BelowBackground, 0) == layer::kBelowBackgroundZ);
}

TEST_CASE("the back of the below-background band does not approach INT32_MIN", "[layer]") {
  const auto back = layer::image_z(Band::BelowBackground, layer::kBandStride - 1);
  REQUIRE(back.has_value());
  INFO("back of band = " << *back);
  CHECK(*back > std::numeric_limits<std::int32_t>::min());
  // With ~1.07e9 of headroom the margin should be enormous, not marginal.
  CHECK(*back - std::numeric_limits<std::int32_t>::min() > 1'000'000'000);
}

// ── Ordering, which is the whole point of a band ────────────────────────────

TEST_CASE("z decreases strictly with rank, in every band", "[layer][property]") {
  for (int i = 0; i < layer::kBandCount; ++i) {
    const auto b = static_cast<Band>(i);
    if (!layer::carries_image(b)) continue;
    for (int rank = 1; rank < layer::kBandStride; ++rank) {
      const auto front = layer::image_z(b, rank - 1);
      const auto back = layer::image_z(b, rank);
      REQUIRE(front.has_value());
      REQUIRE(back.has_value());
      INFO("band " << i << " rank " << rank - 1 << " -> " << *front << ", rank " << rank << " -> "
                   << *back);
      CHECK(*back < *front);
    }
  }
}

TEST_CASE("every (band, rank) pair owns a distinct z", "[layer][property]") {
  // 7 x 32. A stride edit that silently overlapped two bands would reorder the
  // frame with no error anywhere — the plates would just start drawing in the
  // wrong order on some machines and not others.
  for (int i = 0; i < layer::kBandCount; ++i) {
    for (int ri = 0; ri < layer::kBandStride; ++ri) {
      const auto a = layer::image_z(static_cast<Band>(i), ri);
      if (!a) continue;
      for (int j = 0; j < layer::kBandCount; ++j) {
        for (int rj = 0; rj < layer::kBandStride; ++rj) {
          if (i == j && ri == rj) continue;
          const auto c = layer::image_z(static_cast<Band>(j), rj);
          if (!c) continue;
          INFO("(" << i << "," << ri << ") and (" << j << "," << rj << ") both -> " << *a);
          CHECK(*a != *c);
        }
      }
    }
  }
}

TEST_CASE("§4.5 row 3 puts the whole light field above every sprite", "[layer]") {
  // The table states this only in prose — "light field, then monster sprites
  // beneath it" — so it gets its own case. It is also the reason row 3 needs
  // two enumerators rather than one.
  const auto light_back = layer::image_z(Band::Light, layer::kBandStride - 1);
  const auto sprite_front = layer::image_z(Band::Sprites, 0);
  REQUIRE(light_back.has_value());
  REQUIRE(sprite_front.has_value());
  INFO("light back " << *light_back << " vs sprite front " << *sprite_front);
  CHECK(*light_back > *sprite_front);

  // And §4.4's consequence: the light field composites above the geometry band.
  const auto geometry_front = layer::image_z(Band::Geometry, 0);
  REQUIRE(geometry_front.has_value());
  CHECK(*light_back > *geometry_front);
}

// ── Cross-header: does the band actually fit the game? ──────────────────────

TEST_CASE("the geometry band holds §3.1's ladder times floor, wall and ceiling", "[layer]") {
  // kBandStride is the one number in layer.hpp that is a judgement call rather
  // than a quote from §4.5, so it is checked against the thing that sizes it.
  // §3.1 is five depths deep and each carries a floor, a wall and a ceiling,
  // all of which must paint far-to-near within one band.
  const int needed = geometry::kDepthCount * 3;
  INFO("geometry needs " << needed << " ranks, stride is " << layer::kBandStride);
  CHECK(layer::kBandStride >= needed);

  // §4.2 budgets 27 monster poses at M0, and they share the sprite band.
  CHECK(layer::kBandStride >= 27);

  // Every geometry slot the ladder implies must resolve.
  for (int slot = 0; slot < needed; ++slot) {
    INFO("geometry slot " << slot);
    CHECK(layer::image_z(Band::Geometry, slot).has_value());
  }
}

// ── §4.5's table, read back row by row. Last, and least interesting. ────────

TEST_CASE("§4.5's six rows are covered by seven bands, sharing row 3", "[layer]") {
  CHECK(layer::spec_row(Band::Overlay) == 1);
  CHECK(layer::spec_row(Band::Text) == 2);
  CHECK(layer::spec_row(Band::Light) == 3);
  CHECK(layer::spec_row(Band::Sprites) == 3);
  CHECK(layer::spec_row(Band::Geometry) == 4);
  CHECK(layer::spec_row(Band::CellBackground) == 5);
  CHECK(layer::spec_row(Band::BelowBackground) == 6);

  // Exactly one row is shared, and it is row 3. The same "the table adds up"
  // discipline budgets.hpp applies to §4.2's transition-sequence row.
  int per_row[7]{};
  for (int i = 0; i < layer::kBandCount; ++i) ++per_row[layer::spec_row(static_cast<Band>(i))];
  int shared = 0;
  for (int row = 1; row <= 6; ++row) {
    INFO("row " << row);
    CHECK(per_row[row] >= 1);
    if (per_row[row] == 2) ++shared;
  }
  CHECK(shared == 1);
  CHECK(per_row[3] == 2);
}

TEST_CASE("the band ladder reads back as §4.5's table", "[layer]") {
  CHECK(layer::image_z(Band::Overlay, 0) == 31);
  CHECK(layer::image_z(Band::Overlay, 31) == 0);
  CHECK(layer::image_z(Band::Light, 0) == -1);
  CHECK(layer::image_z(Band::Light, 31) == -32);
  CHECK(layer::image_z(Band::Sprites, 0) == -33);
  CHECK(layer::image_z(Band::Sprites, 31) == -64);
  CHECK(layer::image_z(Band::Geometry, 0) == -65);
  CHECK(layer::image_z(Band::Geometry, 31) == -96);
  CHECK(layer::image_z(Band::BelowBackground, 31) == layer::kBelowBackgroundZ - 31);
}
