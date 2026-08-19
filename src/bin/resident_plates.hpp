#pragma once

/// SPEC §3.2, §4.5, §4.8 — ownership of the terminal-resident plate set.
///
/// This is intentionally a binary-private header. It owns allocations, PNG
/// payloads and termforge handles, all of which belong on the terminal side of
/// AGENTS.md rule 1. The pack, encoder and layer arithmetic remain pure
/// `gloam::lib` functions; this class composes them at the I/O boundary.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <termforge/drivers/terminal_driver.hpp>

#include "gloam/layer.hpp"
#include "gloam/pack.hpp"
#include "gloam/png.hpp"

namespace gloam::resident {

enum class ErrorCode : std::uint8_t {
  InvalidPack,
  EncodeFailed,
  AlreadyPinned,
  InsufficientCapacity,
  PinFailed,
  RollbackFailed,
  HandlesStillValid,
  NotPinned,
  UnknownPlate,
  InvalidLayer,
  StaleHandle,
  PlacementFailed,
};

struct Error {
  ErrorCode code{ErrorCode::InvalidPack};
  std::uint16_t plate_id{0};
  pack::PackError pack_error{pack::PackError::None};
  png::PngError png_error{png::PngError::None};
  std::optional<termforge::ErrorEvent> terminal;
};

struct Placement {
  std::uint16_t plate_id{0};
  termforge::Rect cells{};
  layer::Band band{layer::Band::Geometry};
  int band_rank{0};
  termforge::PixelPoint pixel_offset{};
  std::optional<termforge::PixelRect> source{};
};

class PlateSet {
 public:
  [[nodiscard]] static auto from_pack(std::span<const std::byte> image)
      -> std::expected<PlateSet, Error>;

  /// Atomically acquire the whole set. A short capacity check emits nothing;
  /// a mid-batch refusal releases every handle already acquired.
  [[nodiscard]] auto pin_all(termforge::TerminalDriver& driver)
      -> std::expected<void, Error>;

  /// Called only after App has invalidated its driver and before the next draw.
  /// Old handles must already report stale; otherwise no state is discarded.
  [[nodiscard]] auto repin_after_invalidation(termforge::TerminalDriver& driver)
      -> std::expected<void, Error>;

  /// End the borrowed session without emitting per-image deletes. Termforge's
  /// driver shutdown owns the one delete-all for normal alt-screen teardown.
  auto forget_session() noexcept -> void { handles_.clear(); }

  [[nodiscard]] auto draw(termforge::TerminalDriver& driver, const Placement& placement)
      -> std::expected<void, Error>;

  [[nodiscard]] auto handle(std::uint16_t plate_id) const noexcept
      -> std::optional<termforge::PinnedImage>;
  [[nodiscard]] auto size() const noexcept -> std::size_t { return uploads_.size(); }
  [[nodiscard]] auto pinned() const noexcept -> bool {
    return !uploads_.empty() && handles_.size() == uploads_.size();
  }

 private:
  struct Upload {
    std::uint16_t plate_id{0};
    termforge::Extent pixels{};
    std::vector<std::byte> png;
  };

  explicit PlateSet(std::vector<Upload> uploads) : uploads_(std::move(uploads)) {}

  std::vector<Upload> uploads_;
  std::vector<termforge::PinnedImage> handles_;
};

/// The cell footprint that covers the frozen 480x360 viewport at native pixel
/// resolution. Resize changes this rect; it never changes or re-pins payloads.
[[nodiscard]] auto viewport_cells(termforge::Extent cell_pixels)
    -> std::optional<termforge::Rect>;

}  // namespace gloam::resident
