/// SPEC §4.1, §10, §11 — §10's plate as an `f=100` payload.

#include "gloam/png.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "gloam/deflate.hpp"
#include "gloam/palette.hpp"
#include "gloam/plate.hpp"

namespace gloam::png {
namespace {

/// PNG specification §5.2. The last six bytes are a line-ending canary: a
/// transfer that mangles CRLF or strips the high bit corrupts them and nothing
/// else, which is what makes them worth carrying through a base64 pipe that
/// cannot mangle anything.
constexpr std::array<std::uint8_t, 8> kSignature{0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};

constexpr std::uint8_t kColourTypeIndexed = 3;
constexpr std::uint8_t kBitDepth = 4;      ///< five palette entries need three bits; PNG has no 3
constexpr std::uint8_t kFilterNone = 0;

/// The CRC-32 the PNG specification §5.5 mandates: reflected, polynomial
/// 0xEDB88320, initialised and finalised with all ones.
///
/// The table is built once, at first use, from the polynomial rather than
/// written out as 256 constants — a table you can derive is a table that cannot
/// be transcribed wrong, and this is the one place in the tree where a single
/// wrong entry would produce a file every decoder rejects for no visible reason.
[[nodiscard]] auto crc_table() -> const std::array<std::uint32_t, 256>& {
  static const std::array<std::uint32_t, 256> table = [] {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t n = 0; n < 256; ++n) {
      std::uint32_t c = n;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1U) != 0U ? 0xEDB88320U ^ (c >> 1) : c >> 1;
      }
      t[n] = c;
    }
    return t;
  }();
  return table;
}

[[nodiscard]] auto crc32(std::span<const std::byte> data) -> std::uint32_t {
  const auto& table = crc_table();
  std::uint32_t c = 0xFFFFFFFFU;
  for (const auto b : data) {
    c = table[(c ^ static_cast<std::uint32_t>(b)) & 0xFFU] ^ (c >> 8);
  }
  return c ^ 0xFFFFFFFFU;
}

/// Every multi-byte integer in a PNG is big-endian (specification §7.1).
auto put_be32(std::span<std::byte> out, std::size_t at, std::uint32_t v) -> void {
  out[at] = static_cast<std::byte>((v >> 24) & 0xFF);
  out[at + 1] = static_cast<std::byte>((v >> 16) & 0xFF);
  out[at + 2] = static_cast<std::byte>((v >> 8) & 0xFF);
  out[at + 3] = static_cast<std::byte>(v & 0xFF);
}

/// Write one chunk: length, type, data, CRC over type AND data.
///
/// The CRC covering the type as well as the data is the detail everyone gets
/// wrong once. A CRC over the data alone produces a file that libpng rejects and
/// several decoders accept, which is the worst possible way to be wrong.
[[nodiscard]] auto put_chunk(std::span<std::byte> out, std::size_t at, std::string_view type,
                             std::span<const std::byte> data) -> std::size_t {
  put_be32(out, at, static_cast<std::uint32_t>(data.size()));
  std::size_t w = at + 4;

  const auto type_start = w;
  for (const char c : type) out[w++] = static_cast<std::byte>(static_cast<unsigned char>(c));
  for (const auto b : data) out[w++] = b;

  put_be32(out, w, crc32(out.subspan(type_start, w - type_start)));
  w += 4;
  return w - at;
}

/// Pack the plate into `scratch` as PNG scanlines: a filter byte, then two
/// pixels per byte, high nibble first.
///
/// High nibble first is PNG's order and it is also `plate.hpp`'s, which is the
/// half of that header's "re-container" note that survived: no bit-order
/// conversion happens anywhere in this file, only a merge of the two planes.
auto pack_scanlines(const plate::PlateView& src, std::span<std::byte> scratch) -> void {
  const auto row_bytes = scanline_bytes(src.width);

  for (int y = 0; y < src.height; ++y) {
    const auto row = static_cast<std::size_t>(y) * row_bytes;
    scratch[row] = static_cast<std::byte>(kFilterNone);

    // Zeroed first, so an odd width leaves the final byte's low nibble at zero.
    // PNG requires those padding bits to be zero and a decoder may ignore them —
    // a digest may not, and this encoder is pinned by one.
    for (std::size_t i = 1; i < row_bytes; ++i) scratch[row + i] = std::byte{0};

    for (int x = 0; x < src.width; ++x) {
      const auto pixel = plate::read(src, x, y);
      // Unreachable: the extent was validated before this function was called,
      // so every coordinate below is in range. Read through the accessor anyway
      // rather than re-deriving the shifts here — `plate.hpp` says in as many
      // words that a second copy of the bit order is how the writer and the
      // readers silently desynchronise.
      if (pixel.error != plate::PlateError::None) continue;

      const auto entry = pixel.opaque ? palette::entry_of(pixel.ink) : palette::kTransparentEntry;
      const auto at = row + 1 + static_cast<std::size_t>(x / 2);
      const auto shift = (x % 2 == 0) ? 4 : 0;
      scratch[at] |= static_cast<std::byte>(static_cast<unsigned>(entry) << shift);
    }
  }
}

[[nodiscard]] auto to_png_error(plate::PlateError e) -> PngError {
  switch (e) {
    case plate::PlateError::NonPositiveExtent: return PngError::NonPositiveExtent;
    case plate::PlateError::ExtentOutOfRange: return PngError::ExtentOutOfRange;
    case plate::PlateError::BufferTooSmall: return PngError::BlobTooSmall;
    default: return PngError::None;
  }
}

}  // namespace

auto encode(const plate::PlateView& src, std::span<std::byte> scratch, deflate::Scratch& matcher,
            std::span<std::byte> output) -> EncodeResult {
  // Validate everything before a byte is written, the way `kitty.cpp` does: a
  // half-encoded image is a payload the terminal decodes rather than refuses.
  if (const auto e = plate::validate(src.width, src.height, src.blob.size());
      e != plate::PlateError::None) {
    return EncodeResult{to_png_error(e), 0};
  }
  if (scratch.size() < scratch_bytes(src.width, src.height)) {
    return EncodeResult{PngError::ScratchTooSmall, 0};
  }
  if (output.size() < bound(src.width, src.height)) {
    return EncodeResult{PngError::OutputTooSmall, 0};
  }

  const auto raw_bytes = scratch_bytes(src.width, src.height);
  pack_scanlines(src, scratch);

  std::size_t w = 0;
  for (const auto b : kSignature) output[w++] = static_cast<std::byte>(b);

  // IHDR. Compression method 0 and filter method 0 are the only values the
  // specification defines; interlace 0 is what a terminal wants, since an
  // interlaced image is seven passes of nothing useful.
  std::array<std::byte, 13> ihdr{};
  put_be32(ihdr, 0, static_cast<std::uint32_t>(src.width));
  put_be32(ihdr, 4, static_cast<std::uint32_t>(src.height));
  ihdr[8] = static_cast<std::byte>(kBitDepth);
  ihdr[9] = static_cast<std::byte>(kColourTypeIndexed);
  ihdr[10] = std::byte{0};
  ihdr[11] = std::byte{0};
  ihdr[12] = std::byte{0};
  w += put_chunk(output, w, "IHDR", ihdr);

  // PLTE, then tRNS: `palette.hpp` puts the transparent entry first precisely so
  // that tRNS — which is a PREFIX of the palette, not a sparse map — is one byte.
  std::array<std::byte, static_cast<std::size_t>(palette::kEntryCount) * 3> plte{};
  for (int i = 0; i < palette::kEntryCount; ++i) {
    const auto rgb = palette::entry(i);
    plte[static_cast<std::size_t>(i) * 3] = static_cast<std::byte>(rgb.r);
    plte[static_cast<std::size_t>(i) * 3 + 1] = static_cast<std::byte>(rgb.g);
    plte[static_cast<std::size_t>(i) * 3 + 2] = static_cast<std::byte>(rgb.b);
  }
  w += put_chunk(output, w, "PLTE", plte);

  const std::array<std::byte, 1> trns{std::byte{0}};
  w += put_chunk(output, w, "tRNS", trns);

  // IDAT. The zlib stream is written into the output span in place, behind a
  // provisional length, and the chunk is then closed over what it actually took
  // — the alternative is a second scratch buffer the size of the payload.
  const auto idat_at = w;
  const auto idat_data_at = idat_at + 8;  // length and type
  const auto compressed =
      deflate::zlib_compress(scratch.first(raw_bytes), matcher, output.subspan(idat_data_at));
  // Unreachable: `bound` sizes the output at `deflate::bound` of exactly this
  // input, and it was checked above.
  if (!compressed) return EncodeResult{PngError::OutputTooSmall, 0};

  put_be32(output, idat_at, static_cast<std::uint32_t>(compressed.bytes));
  const auto type_at = idat_at + 4;
  const std::string_view idat_type = "IDAT";
  std::size_t t = type_at;
  for (const char c : idat_type) output[t++] = static_cast<std::byte>(static_cast<unsigned char>(c));
  const auto crc_at = idat_data_at + compressed.bytes;
  put_be32(output, crc_at, crc32(output.subspan(type_at, 4 + compressed.bytes)));
  w = crc_at + 4;

  w += put_chunk(output, w, "IEND", std::span<const std::byte>{});

  return EncodeResult{PngError::None, w};
}

}  // namespace gloam::png
