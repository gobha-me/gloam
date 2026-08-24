// gloam#8 — the placeholder plate set: what the rules guarantee, and what the
// compositor can count on.
//
// assets.hpp's header says what these plates ARE (deterministic stand-ins
// pending authored art). This file pins the properties that make them safe to
// compose: the inventory covers every PlateKey compose() can ask for, the
// variants it derives from world state (door from the edge, pose from
// awareness) resolve to real plates that differ from each other, the monster
// strips are transparent where the corridor should show through, and the one
// dithered class is dither-aligned (SPEC 4.3). The golden DIGEST of the whole
// pack lives in test/12pack/ — this file is about structure, not bytes.

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "gloam/assets.hpp"
#include "gloam/compositor.hpp"
#include "gloam/geometry.hpp"
#include "gloam/world.hpp"

using namespace gloam;

namespace {

/// The baked inventory: records and blobs, built once per case. This is
/// `gloam_bake`'s own pipeline minus the file — the same path test/12pack/'s
/// golden covers.
struct Baked {
  std::vector<std::byte> pixels{assets::pixel_bytes()};
  std::vector<pack::Record> records{static_cast<std::size_t>(assets::kPlateCount)};
  std::vector<std::span<const std::byte>> blobs{static_cast<std::size_t>(assets::kPlateCount)};

  Baked() { REQUIRE(assets::bake_all(pixels, records, blobs)); }

  [[nodiscard]] auto catalog() const {
    auto built = compositor::Catalog::from_records(records);
    REQUIRE(built.has_value());
    return std::move(*built);
  }

  /// One plate's blob, found by its semantic key.
  [[nodiscard]] auto blob_of(pack::Role role, std::uint8_t depth, pack::Lateral lateral,
                             std::uint8_t variant) const -> std::span<const std::byte> {
    for (std::size_t i = 0; i < records.size(); ++i) {
      if (records[i].role == role && records[i].depth == depth &&
          records[i].lateral == lateral && records[i].variant == variant) {
        return blobs[i];
      }
    }
    FAIL("no plate for the requested key");
    return {};
  }
};

/// The M0 corridor, library-only: the plus shape test/19patrol/ authors, one
/// monster standing on the corridor two cells ahead of the party.
[[nodiscard]] auto corridor_world() -> World {
  Level level{7, 5};
  level.carve(Coord{1, 2}, Dir::East, 5);
  level.carve(Coord{3, 2}, Dir::North, 3);
  level.carve(Coord{3, 2}, Dir::South, 3);

  Monster m{};
  m.at = Coord{4, 2};
  m.kind = MonsterKind{Acuity::Normal, false};

  auto w = make_world(0xA11CE, std::move(level), {m});
  w.party = Coord{1, 2};
  w.facing = Dir::East;
  w.lamp_level = 3;
  return w;
}

}  // namespace

TEST_CASE("the placeholder inventory is deterministic, byte for byte", "[sceneplates]") {
  const Baked first{};
  const Baked second{};
  CHECK(first.pixels == second.pixels);
}

TEST_CASE("every key the M0 corridor resolves exists in the pack", "[sceneplates]") {
  const Baked baked{};
  const auto catalog = baked.catalog();

  // All four facings, so every lateral/depth combination the plus-shaped
  // corridor can put on screen is exercised.
  for (std::uint16_t f = 0; f < kDirCount; ++f) {
    auto w = corridor_world();
    w.facing = static_cast<Dir>(f);
    const auto placements = compositor::compose(w, catalog);
    REQUIRE(placements.has_value());
    CHECK_FALSE(placements->empty());
  }
}

TEST_CASE("the door variant is real, distinct, and reached from an edge kind",
          "[sceneplates]") {
  const Baked baked{};
  const auto catalog = baked.catalog();

  const auto plain = baked.blob_of(pack::Role::Wall, 1, pack::Lateral::Centre, 0);
  const auto door = baked.blob_of(pack::Role::Wall, 1, pack::Lateral::Centre, 1);
  CHECK(plain.size() == door.size());
  CHECK_FALSE(std::equal(plain.begin(), plain.end(), door.begin()));

  // A CLOSED door on the corridor, inside the sight cap: the walk stops at it
  // and compose() must derive the door variant from the edge kind. (An open
  // door is transparent — the walk passes through and no wall is drawn at all,
  // which is why this pins Closed rather than the default-constructed Open.)
  auto w = corridor_world();
  Edge door_edge{};
  door_edge.kind = EdgeKind::Door;
  door_edge.state = EdgeState::Closed;
  w.level.link(Coord{4, 2}, Dir::East, door_edge);
  const auto placements = compositor::compose(w, catalog);
  REQUIRE(placements.has_value());
  const auto found =
      std::find_if(placements->begin(), placements->end(), [](const compositor::Placement& p) {
        return p.key.role == pack::Role::Wall && p.key.variant == 1;
      });
  CHECK(found != placements->end());
}

TEST_CASE("monster poses resolve from awareness and differ pairwise", "[sceneplates]") {
  const Baked baked{};
  const auto catalog = baked.catalog();

  const auto calm = baked.blob_of(pack::Role::Monster, 1, pack::Lateral::Centre, 0);
  const auto alert = baked.blob_of(pack::Role::Monster, 1, pack::Lateral::Centre, 1);
  const auto hunting = baked.blob_of(pack::Role::Monster, 1, pack::Lateral::Centre, 2);
  CHECK_FALSE(std::equal(calm.begin(), calm.end(), alert.begin()));
  CHECK_FALSE(std::equal(calm.begin(), calm.end(), hunting.begin()));
  CHECK_FALSE(std::equal(alert.begin(), alert.end(), hunting.begin()));

  // The mapping the painter documents: Unaware is pose 0, Hunting pose 2, and
  // everything between is alert. compose() derives it from the monster's mind.
  for (const auto awareness :
       {Awareness::Unaware, Awareness::Suspicious, Awareness::Searching, Awareness::Hunting}) {
    auto w = corridor_world();
    w.monsters[0].mind.state = awareness;
    const auto placements = compositor::compose(w, catalog);
    REQUIRE(placements.has_value());
    const auto monster =
        std::find_if(placements->begin(), placements->end(), [](const compositor::Placement& p) {
          return p.key.role == pack::Role::Monster;
        });
    REQUIRE(monster != placements->end());
    const auto want = awareness == Awareness::Unaware ? 0 : awareness == Awareness::Hunting ? 2 : 1;
    CHECK(monster->key.variant == want);
  }
}

TEST_CASE("monster plates are transparent where the corridor shows through",
          "[sceneplates]") {
  const Baked baked{};

  bool saw_transparent = false;
  bool saw_opaque = false;
  for (std::uint8_t depth = 1; depth <= 3; ++depth) {
    const auto blob = baked.blob_of(pack::Role::Monster, depth, pack::Lateral::Centre, 0);
    const auto spec_h = geometry::kDepths[depth].height;
    const auto spec_w = geometry::kDepths[depth].width / 4;
    for (int y = 0; y < spec_h; ++y) {
      for (int x = 0; x < spec_w; ++x) {
        const auto px = plate::read(plate::PlateView{blob, spec_w, spec_h}, x, y);
        REQUIRE(px.error == plate::PlateError::None);
        saw_transparent = saw_transparent || !px.opaque;
        saw_opaque = saw_opaque || px.opaque;
      }
    }
  }
  // Both, in every plate walked: a figure with no transparency would be a
  // brick of pale wall, and one with nothing opaque would be no monster at all.
  CHECK(saw_transparent);
  CHECK(saw_opaque);
}

TEST_CASE("geometry plates are fully opaque, and the dithered class is aligned",
          "[sceneplates]") {
  const Baked baked{};

  // Walls, floors and ceilings occlude: a hole in one would show the terminal's
  // cell background where the corridor should be, and §4.5's band order is not
  // a coverage model.
  for (const auto& record : baked.records) {
    if (record.role == pack::Role::Monster || record.role == pack::Role::LightField) {
      continue;
    }
    const auto& blob = baked.blobs[&record - baked.records.data()];
    const auto w = static_cast<int>(record.w);
    const auto h = static_cast<int>(record.h);
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        const auto px = plate::read(plate::PlateView{blob, w, h}, x, y);
        REQUIRE(px.error == plate::PlateError::None);
        REQUIRE(px.opaque);
      }
    }
  }

  // §4.3: the floor bands are the one class the painters dither, and their
  // widths are the ring widths — whole dither cells, no fractional boundary.
  for (const auto& spec : assets::inventory()) {
    if (spec.role == pack::Role::Floor) {
      CHECK(plate::dither_aligned(spec.width));
    }
  }
}
