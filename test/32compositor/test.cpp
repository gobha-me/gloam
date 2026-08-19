// SPEC §4 — fixed slots, semantic plate selection and placement-list diff.
//
// Failure matrix first: malformed catalogs and lists must refuse before the
// happy-path scene checks exercise occlusion, facings and pose variants.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <vector>

#include "gloam/compositor.hpp"
#include "gloam/geometry.hpp"
#include "gloam/level.hpp"
#include "gloam/world.hpp"

namespace {

using namespace gloam;
using namespace gloam::compositor;

auto complete_records() -> std::vector<pack::Record> {
  std::vector<pack::Record> result;
  std::uint16_t id = 1;
  auto add = [&](pack::Role role, std::uint8_t depth, pack::Lateral lateral, std::uint8_t variant) {
    result.push_back(pack::Record{
        .plate_id = id++,
        .role = role,
        .depth = depth,
        .lateral = lateral,
        .variant = variant,
        .codec = pack::Codec::RawPlanes,
        .w = 24,
        .h = 24,
    });
  };

  for (std::uint8_t depth = 0; depth < 4; ++depth) {
    for (const auto lateral : {pack::Lateral::Left, pack::Lateral::Right}) {
      add(pack::Role::Wall, depth, lateral, 0);
      add(pack::Role::Wall, depth, lateral, 1);
    }
    add(pack::Role::Floor, depth, pack::Lateral::Centre, 0);
    add(pack::Role::Ceiling, depth, pack::Lateral::Centre, 0);
  }
  for (std::uint8_t depth = 1; depth <= 4; ++depth) {
    add(pack::Role::Wall, depth, pack::Lateral::Centre, 0);
    add(pack::Role::Wall, depth, pack::Lateral::Centre, 1);
  }
  for (std::uint8_t lamp = 0; lamp < kLampLevelCount; ++lamp) {
    add(pack::Role::LightField, pack::kDepthFullFrame, pack::Lateral::FullFrame, lamp);
  }
  for (std::uint8_t depth = 1; depth <= 3; ++depth) {
    for (const auto lateral : {pack::Lateral::Left, pack::Lateral::Centre, pack::Lateral::Right}) {
      for (std::uint8_t variant = 0; variant <= 2; ++variant) {
        add(pack::Role::Monster, depth, lateral, variant);
      }
    }
  }
  return result;
}

auto catalog() -> Catalog {
  const auto records = complete_records();
  auto built = Catalog::from_records(records);
  REQUIRE(built);
  return std::move(*built);
}

auto open_level(int side = 9) -> Level {
  Level level{side, side};
  for (int y = 0; y < side; ++y) level.carve({0, y}, Dir::East, side);
  for (int x = 0; x < side; ++x) level.carve({x, 0}, Dir::South, side);
  return level;
}

auto find_slot(const PlacementList& placements, SlotId slot) -> const Placement* {
  const auto found = std::ranges::find(placements, slot, &Placement::slot);
  return found == placements.end() ? nullptr : &*found;
}

}  // namespace

TEST_CASE("compositor catalog refuses malformed semantic records", "[compositor][failure]") {
  SECTION("zero extent") {
    auto records = complete_records();
    records.front().w = 0;
    const auto result = Catalog::from_records(records);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::InvalidPlate);
  }

  SECTION("invalid role variant") {
    auto records = complete_records();
    records.front().variant = 2;
    const auto result = Catalog::from_records(records);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::InvalidVariant);
  }

  SECTION("invalid monster lateral") {
    auto records = complete_records();
    const auto monster = std::ranges::find(records, pack::Role::Monster,
                                           &pack::Record::role);
    REQUIRE(monster != records.end());
    monster->lateral = static_cast<pack::Lateral>(99);
    const auto result = Catalog::from_records(records);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::InvalidVariant);
  }

  SECTION("duplicate semantic key") {
    auto records = complete_records();
    records.push_back(records.front());
    records.back().plate_id = 65000;
    const auto result = Catalog::from_records(records);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::DuplicatePlate);
  }
}

TEST_CASE("composition refuses state outside the authored slot ladders",
          "[compositor][failure]") {
  SECTION("lamp level") {
    auto world = make_world(1, open_level(), {});
    world.party = {4, 4};
    world.lamp_level = kLampLevelMax + 1;
    const auto result = compose(world, catalog());
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::InvalidVariant);
    CHECK(result.error().slot == kLightSlot);
  }

  SECTION("monster slots") {
    auto world = make_world(1, open_level(), std::vector<Monster>(kMaxMonsterSlots + 1));
    world.party = {4, 4};
    const auto result = compose(world, catalog());
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::TooManyMonsters);
  }
}

TEST_CASE("composition reports the first missing required plate", "[compositor][failure]") {
  auto records = complete_records();
  std::erase_if(records, [](const pack::Record& record) {
    return record.role == pack::Role::Floor && record.depth == 0;
  });
  const auto incomplete = Catalog::from_records(records);
  REQUIRE(incomplete);

  auto world = make_world(1, open_level(), {});
  world.party = {4, 4};
  const auto result = compose(world, *incomplete);
  REQUIRE_FALSE(result);
  CHECK(result.error().code == ErrorCode::MissingPlate);
  CHECK(result.error().key.role == pack::Role::Floor);
  CHECK(result.error().slot == kFloorSlot);
}

TEST_CASE("diff refuses duplicate or unsorted logical slots", "[compositor][diff][failure]") {
  const Placement a{.slot = 2};
  const Placement b{.slot = 2};
  const std::array bad{a, b};
  const auto duplicate = diff({}, bad);
  REQUIRE_FALSE(duplicate);
  CHECK(duplicate.error().code == ErrorCode::DuplicateSlot);

  const std::array reversed{Placement{.slot = 3}, Placement{.slot = 1}};
  const auto unsorted = diff(reversed, {});
  REQUIRE_FALSE(unsorted);
  CHECK(unsorted.error().code == ErrorCode::DuplicateSlot);
}

TEST_CASE("opaque front door stops deeper geometry and selects door variant",
          "[compositor][occlusion][door]") {
  Level level{7, 3};
  level.carve({0, 1}, Dir::East, 7);
  level.link({2, 1}, Dir::East, Edge{.kind = EdgeKind::Door, .state = EdgeState::Closed});
  auto world = make_world(2, std::move(level), {});
  world.party = {2, 1};
  world.facing = Dir::East;

  const auto placements = compose(world, catalog());
  REQUIRE(placements);
  const auto* front = find_slot(*placements, kFrontWallSlot);
  REQUIRE(front != nullptr);
  CHECK(front->key.depth == 1);
  CHECK(front->key.variant == 1);
  CHECK(find_slot(*placements, static_cast<SlotId>(kFloorSlot + 1)) == nullptr);
  CHECK(find_slot(*placements, static_cast<SlotId>(kFrontWallSlot + 1)) == nullptr);
}

TEST_CASE("the twelve wall slots stay fixed across every facing", "[compositor][slots][facing]") {
  const auto plates = catalog();
  for (const auto facing : {Dir::North, Dir::East, Dir::South, Dir::West}) {
    INFO("facing " << static_cast<int>(facing));
    auto world = make_world(3, open_level(), {});
    world.party = {4, 4};
    world.facing = facing;
    const auto placements = compose(world, plates);
    REQUIRE(placements);

    // An open four-cell view reaches the far cap, always in front slot depth 4.
    const auto* cap = find_slot(*placements, static_cast<SlotId>(kFrontWallSlot + 3));
    REQUIRE(cap != nullptr);
    CHECK(cap->key.depth == 4);
    CHECK(cap->key.lateral == pack::Lateral::Centre);
    for (int depth = 0; depth < 4; ++depth) {
      CHECK(find_slot(*placements, static_cast<SlotId>(kFloorSlot + depth)) != nullptr);
      CHECK(find_slot(*placements, static_cast<SlotId>(kCeilingSlot + depth)) != nullptr);
    }
    CHECK(std::ranges::is_sorted(*placements, {}, &Placement::slot));
  }
}

TEST_CASE("visible monsters select depth lateral and awareness pose", "[compositor][monster]") {
  std::vector<Monster> monsters(3);
  monsters[0].at = {3, 2};  // left, depth 2
  monsters[0].mind.state = Awareness::Unaware;
  monsters[1].at = {4, 1};  // centre, depth 3
  monsters[1].mind.state = Awareness::Searching;
  monsters[2].at = {5, 3};  // right, depth 1
  monsters[2].mind.state = Awareness::Hunting;

  auto world = make_world(4, open_level(), std::move(monsters));
  world.party = {4, 4};
  world.facing = Dir::North;
  const auto placements = compose(world, catalog());
  REQUIRE(placements);

  const auto* calm = find_slot(*placements, kFirstMonsterSlot);
  const auto* alert = find_slot(*placements, static_cast<SlotId>(kFirstMonsterSlot + 1));
  const auto* hunting = find_slot(*placements, static_cast<SlotId>(kFirstMonsterSlot + 2));
  REQUIRE(calm != nullptr);
  REQUIRE(alert != nullptr);
  REQUIRE(hunting != nullptr);
  CHECK(calm->key == (PlateKey{pack::Role::Monster, 2, pack::Lateral::Left, 0}));
  CHECK(alert->key == (PlateKey{pack::Role::Monster, 3, pack::Lateral::Centre, 1}));
  CHECK(hunting->key == (PlateKey{pack::Role::Monster, 1, pack::Lateral::Right, 2}));
}

TEST_CASE("monster clipping and overlap preserve stable identity slots",
          "[compositor][monster][slots]") {
  std::vector<Monster> monsters(4);
  monsters[0].at = {4, 2};  // visible centre depth 2
  monsters[1].at = {4, 2};  // exact overlap remains a second logical monster
  monsters[2].at = {4, 0};  // depth 4: outside the sprite ladder
  monsters[3].at = {6, 2};  // lateral 2: outside L/C/R

  auto world = make_world(6, open_level(), std::move(monsters));
  world.party = {4, 4};
  world.facing = Dir::North;
  const auto placements = compose(world, catalog());
  REQUIRE(placements);

  const auto* first = find_slot(*placements, kFirstMonsterSlot);
  const auto* second = find_slot(*placements, static_cast<SlotId>(kFirstMonsterSlot + 1));
  REQUIRE(first != nullptr);
  REQUIRE(second != nullptr);
  CHECK(first->key == second->key);
  CHECK(first->pixels == second->pixels);
  CHECK(find_slot(*placements, static_cast<SlotId>(kFirstMonsterSlot + 2)) == nullptr);
  CHECK(find_slot(*placements, static_cast<SlotId>(kFirstMonsterSlot + 3)) == nullptr);
}

TEST_CASE("placement diff retains identity, draws changes and retires omissions",
          "[compositor][diff]") {
  const Placement same{.slot = 1, .plate_id = 10, .pixels = {0, 0, 10, 10}};
  const Placement changed_before{.slot = 2, .plate_id = 11, .pixels = {0, 0, 10, 10}};
  const Placement changed_after{.slot = 2, .plate_id = 12, .pixels = {0, 0, 10, 10}};
  const Placement removed{.slot = 3, .plate_id = 13, .pixels = {0, 0, 10, 10}};
  const Placement added{.slot = 4, .plate_id = 14, .pixels = {0, 0, 10, 10}};
  const std::array before{same, changed_before, removed};
  const std::array after{same, changed_after, added};

  const auto edits = diff(before, after);
  REQUIRE(edits);
  REQUIRE(edits->size() == 4);
  CHECK((*edits)[0].kind == EditKind::Retain);
  CHECK((*edits)[1].kind == EditKind::Draw);
  CHECK((*edits)[2].kind == EditKind::Retire);
  CHECK((*edits)[3].kind == EditKind::Draw);
}

TEST_CASE("frame classification follows rendered state delta", "[compositor][classification]") {
  auto world = make_world(5, open_level(), {Monster{.at = {4, 2}}});
  world.party = {4, 4};
  auto before = snapshot(world);
  CHECK(classify(before, snapshot(world)) == meter::FrameClass::Idle);

  world.monsters[0].mind.state = Awareness::Suspicious;
  CHECK(classify(before, snapshot(world)) == meter::FrameClass::Animation);

  world.party = {4, 3};
  CHECK(classify(before, snapshot(world)) == meter::FrameClass::Recomposition);
}
