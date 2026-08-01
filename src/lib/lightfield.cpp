#include "gloam/lightfield.hpp"

#include <algorithm>

namespace gloam::lightfield {

auto bake(int lamp_level, std::span<std::byte> blob) -> BakeResult {
  if (lamp_level < kLampLevelMin || lamp_level > kLampLevelMax) {
    return {BakeError::LampLevelOutOfRange, 0, 0};
  }
  if (plate::validate(kWidthPx, kHeightPx, blob.size()) != plate::PlateError::None) {
    return {BakeError::BufferTooSmall, 0, 0};
  }

  const auto total = plate::blob_bytes(kWidthPx, kHeightPx);

  // Zero is both planes' correct starting state: Ink0 in the index plane (the
  // shadow colour, §4.4's "L0 opaque"), and transparent in the stencil.
  std::fill_n(blob.begin(), total, std::byte{0});

  const auto stencil_base = plate::index_plane_bytes(kWidthPx, kHeightPx);
  const auto row_bytes = plate::stencil_row_bytes(kWidthPx);
  std::size_t opaque_pixels = 0;

  // Packed a byte at a time rather than through `plate::write`, for speed —
  // but every piece of the layout comes from `plate.hpp`: the plane offsets
  // from `index_plane_bytes`/`stencil_row_bytes`, and the BIT ORDER from
  // `stencil_mask`. MSB-first is a format commitment (it is PNG's order), so
  // re-deriving `1 << (7 - x % 8)` here would put it in two files and let a
  // writer silently disagree with every reader.
  //
  // `test/11lightfield/` also reads every pixel back through `plate::read` and
  // compares it against `dither::opaque_at(coverage_at(...))`, which keeps the
  // fast path honest against the slow one.
  for (int y = 0; y < kHeightPx; ++y) {
    const auto row = stencil_base + row_bytes * static_cast<std::size_t>(y);
    for (int x = 0; x < kWidthPx; ++x) {
      if (!dither::opaque_at(coverage_at(lamp_level, x, y), x, y)) continue;
      const auto byte = row + static_cast<std::size_t>(x / 8);
      blob[byte] = static_cast<std::byte>(static_cast<std::uint8_t>(blob[byte]) |
                                          plate::stencil_mask(x));
      ++opaque_pixels;
    }
  }

  return {BakeError::None, total, opaque_pixels};
}

}  // namespace gloam::lightfield
