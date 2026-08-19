#pragma once

/// SPEC §4 — the deterministic half of the fixed-slot compositor.
///
/// This module turns a World into a sorted placement list and diffs two lists.
/// It knows pixels, semantic plate keys and logical slots; it does not know a
/// terminal, a clock, termforge or a file descriptor. The binary-side adapter
/// converts these pixel placements to terminal cells and is the only layer that
/// emits them.
///
/// Excluded from `gloam/gloam.hpp`: render-side, not simulation. It is compiled
/// into `gloam::lib` for the same reason as kitty.cpp and pack.cpp — producing
/// a caller-owned value is not I/O and preserves §5.1's replay boundary.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <vector>

#include "gloam/layer.hpp"
#include "gloam/meter.hpp"
#include "gloam/pack.hpp"
#include "gloam/world.hpp"

namespace gloam::compositor {

using SlotId = std::uint16_t;

/// The role-independent identity of an authored plate.
struct PlateKey {
  pack::Role role{pack::Role::Wall};
  std::uint8_t depth{pack::kDepthFullFrame};
  pack::Lateral lateral{pack::Lateral::FullFrame};
  std::uint8_t variant{0};

  [[nodiscard]] auto operator==(const PlateKey&) const -> bool = default;
};

struct Binding {
  PlateKey key{};
  std::uint16_t plate_id{0};
  std::uint16_t width{0};
  std::uint16_t height{0};

  [[nodiscard]] auto operator==(const Binding&) const -> bool = default;
};

enum class ErrorCode : std::uint8_t {
  InvalidPlate = 0,
  InvalidVariant = 1,
  DuplicatePlate = 2,
  MissingPlate = 3,
  DuplicateSlot = 4,
  TooManyMonsters = 5,
};

struct Error {
  ErrorCode code{ErrorCode::InvalidPlate};
  PlateKey key{};
  SlotId slot{0};
};

/// A validated semantic view of a pack manifest. Payloads remain owned by the
/// pack/uploader; this is only the small metadata needed during composition.
class Catalog {
 public:
  [[nodiscard]] static auto from_records(std::span<const pack::Record> records)
      -> std::expected<Catalog, Error>;

  [[nodiscard]] auto find(PlateKey key) const noexcept -> std::optional<Binding>;
  [[nodiscard]] auto size() const noexcept -> std::size_t { return bindings_.size(); }

 private:
  explicit Catalog(std::vector<Binding> bindings) : bindings_(std::move(bindings)) {}

  std::vector<Binding> bindings_;
};

struct PixelRect {
  int x{0};
  int y{0};
  int w{0};
  int h{0};

  [[nodiscard]] auto operator==(const PixelRect&) const -> bool = default;
};

struct Placement {
  SlotId slot{0};
  PlateKey key{};
  std::uint16_t plate_id{0};
  PixelRect pixels{};
  layer::Band band{layer::Band::Geometry};
  int band_rank{0};

  [[nodiscard]] auto operator==(const Placement&) const -> bool = default;
};

using PlacementList = std::vector<Placement>;

/// Stable logical slots. Wall inventory is exactly §4.2's twelve; floor and
/// ceiling are four each. Monster identity is its stable index in World.
inline constexpr SlotId kLeftWallSlot = 1;
inline constexpr SlotId kRightWallSlot = 5;
inline constexpr SlotId kFrontWallSlot = 9;
inline constexpr SlotId kFloorSlot = 13;
inline constexpr SlotId kCeilingSlot = 17;
inline constexpr SlotId kLightSlot = 21;
inline constexpr SlotId kFirstMonsterSlot = 32;
inline constexpr std::size_t kMaxMonsterSlots = 16;

/// Snapshot only the state whose delta changes a rendered placement.
struct FrameState {
  Coord party{};
  Dir facing{Dir::North};
  std::int32_t lamp_level{0};
  std::vector<Awareness> minds{};
  std::vector<Coord> positions{};

  [[nodiscard]] auto operator==(const FrameState&) const -> bool = default;
};

[[nodiscard]] auto snapshot(const World& world) -> FrameState;
[[nodiscard]] auto classify(const FrameState& before, const FrameState& after) -> meter::FrameClass;

/// Build the visible scene in deterministic slot order. Opaque front edges stop
/// the walk; monsters are admitted only at depths 1..3, laterals L/C/R, and
/// through the same Level graph used by perception.
[[nodiscard]] auto compose(const World& world, const Catalog& catalog)
    -> std::expected<PlacementList, Error>;

enum class EditKind : std::uint8_t { Retain = 0, Draw = 1, Retire = 2 };

struct Edit {
  EditKind kind{EditKind::Retain};
  Placement placement{};

  [[nodiscard]] auto operator==(const Edit&) const -> bool = default;
};

/// Diff two slot-sorted lists. Retirement is represented explicitly for tests
/// and accounting; terminal adapters retire by omitting that placement from the
/// frame, as termforge's frame contract requires.
[[nodiscard]] auto diff(std::span<const Placement> previous, std::span<const Placement> next)
    -> std::expected<std::vector<Edit>, Error>;

}  // namespace gloam::compositor
