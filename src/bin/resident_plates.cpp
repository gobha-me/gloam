#include "resident_plates.hpp"

#include <algorithm>
#include <memory>
#include <utility>

#include "gloam/deflate.hpp"
#include "gloam/geometry.hpp"
#include "gloam/plate.hpp"

namespace gloam::resident {
namespace {

[[nodiscard]] auto capacity_left(const termforge::TerminalDriver& driver) -> std::size_t {
  const auto maximum = driver.max_pinned_images();
  const auto used = driver.residency().pinned_images;
  return used >= maximum ? 0 : maximum - used;
}

}  // namespace

auto PlateSet::from_pack(std::span<const std::byte> image)
    -> std::expected<PlateSet, Error> {
  const auto verified = pack::verify(image);
  if (!verified) {
    return std::unexpected{Error{
        .code = ErrorCode::InvalidPack,
        .pack_error = verified.error,
        .compositor = std::nullopt,
        .terminal = std::nullopt,
    }};
  }

  pack::Header header{};
  const auto header_result = pack::read_header(image, header);
  if (!header_result) {
    return std::unexpected{Error{
        .code = ErrorCode::InvalidPack,
        .pack_error = header_result.error,
        .compositor = std::nullopt,
        .terminal = std::nullopt,
    }};
  }

  std::vector<Upload> uploads;
  uploads.reserve(header.plate_count);
  std::vector<pack::Record> records;
  records.reserve(header.plate_count);
  auto matcher = std::make_unique<deflate::Scratch>();

  for (std::uint16_t index = 0; index < header.plate_count; ++index) {
    pack::Record record{};
    const auto at = pack::kHeaderBytes + pack::kRecordBytes * static_cast<std::size_t>(index);
    const auto record_result =
        pack::read_record(image.subspan(at, pack::kRecordBytes), record);
    if (!record_result) {
      return std::unexpected{Error{
          .code = ErrorCode::InvalidPack,
          .plate_id = record.plate_id,
          .pack_error = record_result.error,
          .compositor = std::nullopt,
          .terminal = std::nullopt,
      }};
    }

    std::vector<std::byte> scratch(png::scratch_bytes(record.w, record.h));
    std::vector<std::byte> payload(png::bound(record.w, record.h));
    const auto result = png::encode(
        plate::PlateView{image.subspan(record.offset, record.length), record.w, record.h},
        scratch, *matcher, payload);
    if (!result) {
      return std::unexpected{Error{
          .code = ErrorCode::EncodeFailed,
          .plate_id = record.plate_id,
          .png_error = result.error,
          .compositor = std::nullopt,
          .terminal = std::nullopt,
      }};
    }
    payload.resize(result.bytes);
    records.push_back(record);
    uploads.push_back(Upload{
        .plate_id = record.plate_id,
        .pixels = termforge::Extent{record.w, record.h},
        .png = std::move(payload),
    });
  }

  auto catalog = compositor::Catalog::from_records(records);
  if (!catalog) {
    return std::unexpected{Error{
        .code = ErrorCode::InvalidCatalog,
        .compositor = catalog.error(),
        .terminal = std::nullopt,
    }};
  }
  return PlateSet{std::move(uploads), std::move(*catalog)};
}

auto PlateSet::pin_all(termforge::TerminalDriver& driver)
    -> std::expected<void, Error> {
  if (!handles_.empty()) {
    return std::unexpected{Error{
        .code = ErrorCode::AlreadyPinned,
        .compositor = std::nullopt,
        .terminal = std::nullopt,
    }};
  }
  if (capacity_left(driver) < uploads_.size()) {
    return std::unexpected{Error{
        .code = ErrorCode::InsufficientCapacity,
        .compositor = std::nullopt,
        .terminal = std::nullopt,
    }};
  }

  std::vector<termforge::PinnedImage> pending;
  pending.reserve(uploads_.size());
  for (const auto& upload : uploads_) {
    const termforge::EncodedImage image{
        .format = termforge::ImageFormat::Png,
        .bytes = upload.png,
        .pixels = upload.pixels,
    };
    auto handle = driver.pin_image(image);
    if (handle) {
      pending.push_back(*handle);
      continue;
    }

    bool rollback_failed = false;
    std::optional<termforge::ErrorEvent> rollback_error;
    for (auto it = pending.rbegin(); it != pending.rend(); ++it) {
      auto released = driver.unpin_image(*it);
      if (!released && !rollback_failed) {
        rollback_failed = true;
        rollback_error = released.error();
      }
    }
    return std::unexpected{Error{
        .code = rollback_failed ? ErrorCode::RollbackFailed : ErrorCode::PinFailed,
        .plate_id = upload.plate_id,
        .compositor = std::nullopt,
        .terminal = rollback_failed ? std::move(rollback_error)
                                    : std::optional<termforge::ErrorEvent>{handle.error()},
    }};
  }

  handles_ = std::move(pending);
  return {};
}

auto PlateSet::repin_after_invalidation(termforge::TerminalDriver& driver)
    -> std::expected<void, Error> {
  for (std::size_t i = 0; i < handles_.size(); ++i) {
    if (driver.pinned_image_status(handles_[i]).valid) {
      return std::unexpected{Error{
          .code = ErrorCode::HandlesStillValid,
          .plate_id = uploads_[i].plate_id,
          .compositor = std::nullopt,
          .terminal = std::nullopt,
      }};
    }
  }
  handles_.clear();
  return pin_all(driver);
}

auto PlateSet::handle(std::uint16_t plate_id) const noexcept
    -> std::optional<termforge::PinnedImage> {
  const auto found = std::lower_bound(
      uploads_.begin(), uploads_.end(), plate_id,
      [](const Upload& upload, std::uint16_t id) { return upload.plate_id < id; });
  if (found == uploads_.end() || found->plate_id != plate_id) return std::nullopt;
  const auto index = static_cast<std::size_t>(found - uploads_.begin());
  if (index >= handles_.size()) return std::nullopt;
  return handles_[index];
}

auto PlateSet::place(termforge::TerminalDriver& driver, const Placement& placement,
                     bool retain) -> std::expected<void, Error> {
  if (handles_.size() != uploads_.size()) {
    return std::unexpected{Error{
        .code = ErrorCode::NotPinned,
        .plate_id = placement.plate_id,
        .compositor = std::nullopt,
        .terminal = std::nullopt,
    }};
  }
  const auto image = handle(placement.plate_id);
  if (!image) {
    return std::unexpected{Error{
        .code = ErrorCode::UnknownPlate,
        .plate_id = placement.plate_id,
        .compositor = std::nullopt,
        .terminal = std::nullopt,
    }};
  }
  if (!driver.pinned_image_status(*image).valid) {
    return std::unexpected{Error{
        .code = ErrorCode::StaleHandle,
        .plate_id = placement.plate_id,
        .compositor = std::nullopt,
        .terminal = std::nullopt,
    }};
  }
  const auto z = layer::image_z(placement.band, placement.band_rank);
  if (!z) {
    return std::unexpected{Error{
        .code = ErrorCode::InvalidLayer,
        .plate_id = placement.plate_id,
        .compositor = std::nullopt,
        .terminal = std::nullopt,
    }};
  }

  termforge::ImagePlacementOptions options{
      .fit = termforge::PlacementFit::Exact,
      .layer = termforge::ImageLayer::raw(*z),
      .pixel_offset = placement.pixel_offset,
      .source = placement.source,
  };
  auto drawn = retain ? driver.retain_pinned(placement.cells, *image, options)
                      : driver.draw_pinned(placement.cells, *image, options);
  if (!drawn) {
    return std::unexpected{Error{
        .code = ErrorCode::PlacementFailed,
        .plate_id = placement.plate_id,
        .compositor = std::nullopt,
        .terminal = drawn.error(),
    }};
  }
  return {};
}

auto PlateSet::draw(termforge::TerminalDriver& driver, const Placement& placement)
    -> std::expected<void, Error> {
  return place(driver, placement, false);
}

auto PlateSet::retain(termforge::TerminalDriver& driver, const Placement& placement)
    -> std::expected<void, Error> {
  return place(driver, placement, true);
}

auto viewport_cells(termforge::Extent cell_pixels) -> std::optional<termforge::Rect> {
  if (cell_pixels.empty()) return std::nullopt;
  return termforge::Rect{
      0,
      0,
      geometry::cells_to_cover(geometry::kViewportWidthPx, cell_pixels.w),
      geometry::cells_to_cover(geometry::kViewportHeightPx, cell_pixels.h),
  };
}

}  // namespace gloam::resident
