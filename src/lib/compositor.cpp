#include "gloam/compositor.hpp"

#include <algorithm>
#include <tuple>

#include "gloam/geometry.hpp"
#include "gloam/tuning.hpp"

namespace gloam::compositor {
namespace {

[[nodiscard]] constexpr auto key_tuple(PlateKey key) {
  return std::tuple{static_cast<std::uint8_t>(key.role), key.depth,
                    static_cast<std::uint8_t>(key.lateral), key.variant};
}

[[nodiscard]] constexpr auto key_less(PlateKey lhs, PlateKey rhs) -> bool {
  return key_tuple(lhs) < key_tuple(rhs);
}

[[nodiscard]] constexpr auto valid_key(PlateKey key) -> bool {
  switch (key.role) {
    case pack::Role::Wall:
      if (key.variant > 1) return false;
      if (key.lateral == pack::Lateral::Centre) {
        return key.depth >= 1 && key.depth < geometry::kDepthCount;
      }
      return (key.lateral == pack::Lateral::Left || key.lateral == pack::Lateral::Right) &&
             key.depth < geometry::kDepthCount - 1;
    case pack::Role::Floor:
    case pack::Role::Ceiling:
      return key.variant == 0 && key.depth < geometry::kDepthCount - 1 &&
             key.lateral == pack::Lateral::Centre;
    case pack::Role::LightField:
      return key.variant < kLampLevelCount && key.depth == pack::kDepthFullFrame &&
             key.lateral == pack::Lateral::FullFrame;
    case pack::Role::Monster:
      return key.variant <= 2 && key.depth >= 1 && key.depth <= 3 &&
             (key.lateral == pack::Lateral::Left ||
              key.lateral == pack::Lateral::Centre ||
              key.lateral == pack::Lateral::Right);
    case pack::Role::Item:
    case pack::Role::Ui:
    case pack::Role::Rune:
      return true;
  }
  return false;
}

[[nodiscard]] constexpr auto right_of(Dir direction) -> Dir {
  return static_cast<Dir>((static_cast<std::uint8_t>(direction) + 1) % kDirCount);
}

[[nodiscard]] constexpr auto left_of(Dir direction) -> Dir {
  return static_cast<Dir>((static_cast<std::uint8_t>(direction) + 3) % kDirCount);
}

[[nodiscard]] constexpr auto wall_variant(const Edge& edge) -> std::uint8_t {
  return edge.kind == EdgeKind::Door ? 1 : 0;
}

[[nodiscard]] constexpr auto monster_variant(Awareness awareness) -> std::uint8_t {
  if (awareness == Awareness::Unaware) return 0;
  if (awareness == Awareness::Hunting) return 2;
  return 1;
}

[[nodiscard]] constexpr auto direction_vector(Dir direction) -> Coord {
  switch (direction) {
    case Dir::North:
      return {0, -1};
    case Dir::East:
      return {1, 0};
    case Dir::South:
      return {0, 1};
    case Dir::West:
      return {-1, 0};
  }
  return {};
}

[[nodiscard]] auto anchor(const Binding& binding) -> PixelRect {
  const auto depth =
      binding.key.depth == pack::kDepthFullFrame ? 0 : static_cast<int>(binding.key.depth);
  const auto ring = geometry::kDepths[static_cast<std::size_t>(depth)];
  const int left = (geometry::kViewportWidthPx - ring.width) / 2;
  const int top = (geometry::kViewportHeightPx - ring.height) / 2;
  int x = (geometry::kViewportWidthPx - static_cast<int>(binding.width)) / 2;
  int y = (geometry::kViewportHeightPx - static_cast<int>(binding.height)) / 2;

  if (binding.key.lateral == pack::Lateral::Left) x = left;
  if (binding.key.lateral == pack::Lateral::Right) {
    x = left + ring.width - static_cast<int>(binding.width);
  }
  if (binding.key.role == pack::Role::Ceiling) y = top;
  if (binding.key.role == pack::Role::Floor) {
    y = top + ring.height - static_cast<int>(binding.height);
  }
  if (binding.key.role == pack::Role::LightField) {
    x = 0;
    y = 0;
  }
  return {x, y, binding.width, binding.height};
}

auto append(PlacementList& out, const Catalog& catalog, SlotId slot, PlateKey key, layer::Band band,
            int rank) -> std::optional<Error> {
  const auto binding = catalog.find(key);
  if (!binding) return Error{ErrorCode::MissingPlate, key, slot};
  out.push_back(Placement{slot, key, binding->plate_id, anchor(*binding), band, rank});
  return std::nullopt;
}

[[nodiscard]] auto unique_slots(std::span<const Placement> placements) -> std::optional<SlotId> {
  for (std::size_t i = 1; i < placements.size(); ++i) {
    if (placements[i - 1].slot >= placements[i].slot) return placements[i].slot;
  }
  return std::nullopt;
}

}  // namespace

auto Catalog::from_records(std::span<const pack::Record> records) -> std::expected<Catalog, Error> {
  std::vector<Binding> bindings;
  bindings.reserve(records.size());
  for (const auto& record : records) {
    const PlateKey key{record.role, record.depth, record.lateral, record.variant};
    if (record.w == 0 || record.h == 0) {
      return std::unexpected{Error{ErrorCode::InvalidPlate, key, 0}};
    }
    if (!valid_key(key)) {
      return std::unexpected{Error{ErrorCode::InvalidVariant, key, 0}};
    }
    // Item/UI/rune plates are instance-addressed by their owning systems, not
    // by §4's scene slots. Their generic record fields need not be unique.
    if (record.role == pack::Role::Item || record.role == pack::Role::Ui ||
        record.role == pack::Role::Rune) {
      continue;
    }
    bindings.push_back(Binding{key, record.plate_id, record.w, record.h});
  }
  std::ranges::sort(
      bindings, [](const Binding& lhs, const Binding& rhs) { return key_less(lhs.key, rhs.key); });
  for (std::size_t i = 1; i < bindings.size(); ++i) {
    if (bindings[i - 1].key == bindings[i].key) {
      return std::unexpected{Error{ErrorCode::DuplicatePlate, bindings[i].key, 0}};
    }
  }
  return Catalog{std::move(bindings)};
}

auto Catalog::find(PlateKey key) const noexcept -> std::optional<Binding> {
  const auto found = std::lower_bound(
      bindings_.begin(), bindings_.end(), key,
      [](const Binding& binding, PlateKey wanted) { return key_less(binding.key, wanted); });
  if (found == bindings_.end() || found->key != key) return std::nullopt;
  return *found;
}

auto snapshot(const World& world) -> FrameState {
  FrameState state{world.party, world.facing, world.lamp_level, {}, {}};
  state.minds.reserve(world.monsters.size());
  state.positions.reserve(world.monsters.size());
  for (const auto& monster : world.monsters) {
    state.minds.push_back(monster.mind.state);
    state.positions.push_back(monster.at);
  }
  return state;
}

auto classify(const FrameState& before, const FrameState& after) -> meter::FrameClass {
  if (before.party != after.party || before.facing != after.facing) {
    return meter::FrameClass::Recomposition;
  }
  if (before.lamp_level != after.lamp_level || before.minds != after.minds ||
      before.positions != after.positions) {
    return meter::FrameClass::Animation;
  }
  return meter::FrameClass::Idle;
}

auto compose(const World& world, const Catalog& catalog) -> std::expected<PlacementList, Error> {
  if (world.monsters.size() > kMaxMonsterSlots) {
    return std::unexpected{Error{ErrorCode::TooManyMonsters, {}, 0}};
  }
  if (world.lamp_level < kLampLevelMin || world.lamp_level > kLampLevelMax) {
    return std::unexpected{
        Error{ErrorCode::InvalidVariant,
              PlateKey{pack::Role::LightField, pack::kDepthFullFrame, pack::Lateral::FullFrame,
                       static_cast<std::uint8_t>(world.lamp_level)},
              kLightSlot}};
  }

  PlacementList placements;
  placements.reserve(21 + world.monsters.size());
  Coord current = world.party;
  const Dir left = left_of(world.facing);
  const Dir right = right_of(world.facing);

  for (int depth = 0; depth < geometry::kDepthCount - 1; ++depth) {
    const auto depth_byte = static_cast<std::uint8_t>(depth);
    if (auto error = append(placements, catalog, static_cast<SlotId>(kFloorSlot + depth),
                            {pack::Role::Floor, depth_byte, pack::Lateral::Centre, 0},
                            layer::Band::Geometry, depth)) {
      return std::unexpected{*error};
    }
    if (auto error = append(placements, catalog, static_cast<SlotId>(kCeilingSlot + depth),
                            {pack::Role::Ceiling, depth_byte, pack::Lateral::Centre, 0},
                            layer::Band::Geometry, depth)) {
      return std::unexpected{*error};
    }

    for (const auto& [direction, lateral, base] :
         {std::tuple{left, pack::Lateral::Left, kLeftWallSlot},
          std::tuple{right, pack::Lateral::Right, kRightWallSlot}}) {
      const auto& edge = world.level.edge(current, direction);
      if (!edge.transparent()) {
        if (auto error = append(placements, catalog, static_cast<SlotId>(base + depth),
                                {pack::Role::Wall, depth_byte, lateral, wall_variant(edge)},
                                layer::Band::Geometry, depth)) {
          return std::unexpected{*error};
        }
      }
    }

    const auto& ahead = world.level.edge(current, world.facing);
    const auto next = world.level.walk(current, world.facing);
    const bool cap = depth == geometry::kDepthCount - 2;
    if (!ahead.transparent() || !next || cap) {
      const auto front_depth = static_cast<std::uint8_t>(depth + 1);
      const std::uint8_t variant = !ahead.transparent() ? wall_variant(ahead) : 0;
      if (auto error = append(placements, catalog, static_cast<SlotId>(kFrontWallSlot + depth),
                              {pack::Role::Wall, front_depth, pack::Lateral::Centre, variant},
                              layer::Band::Geometry, depth + 1)) {
        return std::unexpected{*error};
      }
      break;
    }
    current = *next;
  }

  if (auto error = append(placements, catalog, kLightSlot,
                          {pack::Role::LightField, pack::kDepthFullFrame, pack::Lateral::FullFrame,
                           static_cast<std::uint8_t>(world.lamp_level)},
                          layer::Band::Light, 0)) {
    return std::unexpected{*error};
  }

  const Coord forward = direction_vector(world.facing);
  const Coord side = direction_vector(right);
  for (std::size_t index = 0; index < world.monsters.size(); ++index) {
    const auto& monster = world.monsters[index];
    const auto dx = static_cast<std::int64_t>(monster.at.x) - world.party.x;
    const auto dy = static_cast<std::int64_t>(monster.at.y) - world.party.y;
    const auto forward_distance = dx * forward.x + dy * forward.y;
    const auto lateral_distance = dx * side.x + dy * side.y;
    if (forward_distance < 1 || forward_distance > 3 || lateral_distance < -1 ||
        lateral_distance > 1) {
      continue;
    }
    if (!line_of_sight(world.level, world.party, monster.at)) continue;

    const auto lateral = lateral_distance < 0   ? pack::Lateral::Left
                         : lateral_distance > 0 ? pack::Lateral::Right
                                                : pack::Lateral::Centre;
    const PlateKey key{pack::Role::Monster, static_cast<std::uint8_t>(forward_distance), lateral,
                       monster_variant(monster.mind.state)};
    if (auto error = append(placements, catalog, static_cast<SlotId>(kFirstMonsterSlot + index),
                            key, layer::Band::Sprites, static_cast<int>(forward_distance - 1))) {
      return std::unexpected{*error};
    }
  }

  std::ranges::sort(placements,
                    [](const Placement& lhs, const Placement& rhs) { return lhs.slot < rhs.slot; });
  return placements;
}

auto diff(std::span<const Placement> previous, std::span<const Placement> next)
    -> std::expected<std::vector<Edit>, Error> {
  if (const auto duplicate = unique_slots(previous)) {
    return std::unexpected{Error{ErrorCode::DuplicateSlot, {}, *duplicate}};
  }
  if (const auto duplicate = unique_slots(next)) {
    return std::unexpected{Error{ErrorCode::DuplicateSlot, {}, *duplicate}};
  }

  std::vector<Edit> edits;
  edits.reserve(previous.size() + next.size());
  std::size_t old_index = 0;
  std::size_t new_index = 0;
  while (old_index < previous.size() || new_index < next.size()) {
    if (new_index == next.size() ||
        (old_index < previous.size() && previous[old_index].slot < next[new_index].slot)) {
      edits.push_back({EditKind::Retire, previous[old_index++]});
      continue;
    }
    if (old_index == previous.size() || next[new_index].slot < previous[old_index].slot) {
      edits.push_back({EditKind::Draw, next[new_index++]});
      continue;
    }
    edits.push_back({previous[old_index] == next[new_index] ? EditKind::Retain : EditKind::Draw,
                     next[new_index]});
    ++old_index;
    ++new_index;
  }
  return edits;
}

}  // namespace gloam::compositor
