#include "gloam/plate.hpp"

#include <array>
#include <limits>

namespace gloam::plate {
namespace {

/// §12's manifest record carries w and h as u16, so an extent that cannot round
/// trip through a record is refused here rather than truncated there.
constexpr int kMaxExtent = std::numeric_limits<std::uint16_t>::max();

/// Byte offsets. The BIT offsets come from `plate.hpp`'s `index_shift` /
/// `stencil_shift` — MSB-first is a format commitment (it is PNG's order), so
/// it is stated once, in the header, where `lightfield.cpp` can reach it.
[[nodiscard]] constexpr auto index_byte(int width, int x, int y) -> std::size_t {
  return index_row_bytes(width) * static_cast<std::size_t>(y) + static_cast<std::size_t>(x / 4);
}

[[nodiscard]] constexpr auto stencil_byte(int width, int height, int x, int y) -> std::size_t {
  return index_plane_bytes(width, height) +
         stencil_row_bytes(width) * static_cast<std::size_t>(y) + static_cast<std::size_t>(x / 8);
}

[[nodiscard]] constexpr auto in_bounds(int width, int height, int x, int y) -> bool {
  return x >= 0 && y >= 0 && x < width && y < height;
}

/// Unchecked accessors. Every caller below has already run `validate` on the
/// extent and bounds-checked the coordinate; these exist so `downsample_2to1`
/// does not re-validate five times per output pixel, which on the real ladder
/// was 375,840 redundant validations to derive three depths.
[[nodiscard]] auto read_raw(const PlateView& src, int x, int y) -> Pixel {
  const auto ib = static_cast<std::uint8_t>(src.blob[index_byte(src.width, x, y)]);
  const auto sb = static_cast<std::uint8_t>(src.blob[stencil_byte(src.width, src.height, x, y)]);
  return {PlateError::None, static_cast<Ink>((ib >> index_shift(x)) & 0x03U),
          (sb & stencil_mask(x)) != 0U};
}

auto write_raw(const PlateSpan& dst, int x, int y, Ink ink, bool opaque) -> void {
  const auto ib = index_byte(dst.width, x, y);
  auto index = static_cast<std::uint8_t>(dst.blob[ib]);
  index = static_cast<std::uint8_t>(index & ~(0x03U << index_shift(x)));
  index = static_cast<std::uint8_t>(index |
                                    ((static_cast<std::uint8_t>(ink) & 0x03U) << index_shift(x)));
  dst.blob[ib] = static_cast<std::byte>(index);

  const auto sb = stencil_byte(dst.width, dst.height, x, y);
  auto stencil = static_cast<std::uint8_t>(dst.blob[sb]);
  stencil = static_cast<std::uint8_t>(stencil & ~stencil_mask(x));
  if (opaque) stencil = static_cast<std::uint8_t>(stencil | stencil_mask(x));
  dst.blob[sb] = static_cast<std::byte>(stencil);
}

}  // namespace

auto validate(int width, int height, std::size_t blob_size) -> PlateError {
  if (width <= 0 || height <= 0) return PlateError::NonPositiveExtent;
  if (width > kMaxExtent || height > kMaxExtent) return PlateError::ExtentOutOfRange;
  if (blob_size < blob_bytes(width, height)) return PlateError::BufferTooSmall;
  return PlateError::None;
}

auto read(const PlateView& src, int x, int y) -> Pixel {
  if (const auto err = validate(src.width, src.height, src.blob.size());
      err != PlateError::None) {
    return {err, Ink::Ink0, false};
  }
  if (!in_bounds(src.width, src.height, x, y)) {
    return {PlateError::CoordinateOutOfRange, Ink::Ink0, false};
  }
  return read_raw(src, x, y);
}

auto write(const PlateSpan& dst, int x, int y, Ink ink, bool opaque) -> PlateResult {
  if (const auto err = validate(dst.width, dst.height, dst.blob.size());
      err != PlateError::None) {
    return {err, 0};
  }
  if (!in_bounds(dst.width, dst.height, x, y)) {
    return {PlateError::CoordinateOutOfRange, 0};
  }
  write_raw(dst, x, y, ink, opaque);

  // Two bytes even when both land in the same one: `bytes` is what the call
  // touched, and the two planes are always distinct byte ranges.
  return {PlateError::None, 2};
}

auto downsample_2to1(const PlateView& src, const PlateSpan& dst) -> PlateResult {
  if (const auto err = validate(src.width, src.height, src.blob.size());
      err != PlateError::None) {
    return {err, 0};
  }
  if (src.width % 2 != 0 || src.height % 2 != 0) return {PlateError::OddSourceExtent, 0};
  if (dst.width != src.width / 2 || dst.height != src.height / 2) {
    return {PlateError::DestinationExtentMismatch, 0};
  }
  if (const auto err = validate(dst.width, dst.height, dst.blob.size());
      err != PlateError::None) {
    return {err, 0};
  }

  // Both extents are validated and every coordinate below is in range by
  // construction, so the loop uses the unchecked accessors. There is no
  // unreachable error branch threaded through the pipeline's only pixel loop.
  for (int y = 0; y < dst.height; ++y) {
    for (int x = 0; x < dst.width; ++x) {
      std::array<int, kInkCount> votes{};
      int opaque_sources = 0;

      for (int dy = 0; dy < 2; ++dy) {
        for (int dx = 0; dx < 2; ++dx) {
          const auto px = read_raw(src, x * 2 + dx, y * 2 + dy);
          if (!px.opaque) continue;
          ++opaque_sources;
          ++votes[static_cast<std::size_t>(px.ink)];
        }
      }

      // Ties to the lowest Ink: the scan is strictly-greater, so the first
      // index holding the maximum wins and the result does not depend on
      // iteration order. With no opaque sources every tally is zero and this
      // falls out as Ink0, which is also the rule the doc comment states — no
      // separate branch, so the two cannot drift apart.
      auto ink = Ink::Ink0;
      int best = -1;
      for (int i = 0; i < kInkCount; ++i) {
        if (votes[static_cast<std::size_t>(i)] > best) {
          best = votes[static_cast<std::size_t>(i)];
          ink = static_cast<Ink>(i);
        }
      }

      write_raw(dst, x, y, ink, opaque_sources >= 2);
    }
  }

  return {PlateError::None, blob_bytes(dst.width, dst.height)};
}

}  // namespace gloam::plate
