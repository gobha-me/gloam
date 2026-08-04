#pragma once

/// SPEC §4.1, §10, §11 — §10's plate as an `f=100` payload.
///
/// Kitty's `f=100` takes a PNG and reads the image geometry from the datastream,
/// which is the format upstream #163 added a verbatim transmit path for and the
/// one §11's cold-start budget survives. This module is the whole of the
/// conversion: a `plate::PlateView` in, a complete PNG file out, into a span the
/// caller sized.
///
///
/// WHAT THE PACK DOES NOT DO, AND WHY
///
/// `pack::Codec::Png` exists as a versioned door and `validate_record` still
/// refuses it. That is deliberate and this header is where the reason belongs:
/// the pack is a PIXEL SOURCE and its hash is a build gate (§10, "two pipeline
/// runs must produce byte-identical packs"). Baking PNG into it would put the
/// compressor, the palette and the filter choice inside `pack_sha256`, so
/// improving the encoder or touching one grey value would invalidate every
/// baked pack and re-open a build gate that has nothing to do with either.
/// Encoding at transmit keeps the two hashes measuring different things: the
/// pack hash asks "are these the same pixels", this side asks "are these the
/// same bytes on the wire".
///
///
/// THE PIXEL MAPPING, WHICH IS THE ONE INTERESTING PART
///
/// A plate is five states — four inks opaque, plus transparent — carried as two
/// planes, 2 bits of ink and 1 bit of stencil. PNG has bit depths 1, 2, 4 and 8
/// and no 3, so the encoded form is COLOUR TYPE 3 AT 4 BITS PER PIXEL over a
/// five-entry `PLTE`, with a one-byte `tRNS` making entry 0 transparent
/// (`palette.hpp` owns the entries).
///
/// That makes this a re-pack rather than a re-container, and `plate.hpp`'s note
/// has been corrected to say so. What survives from that note is the half that
/// was actually load-bearing: both formats are MSB-first with byte-aligned rows,
/// so the packing is a merge of two planes and never a bit-order conversion.
///
/// Filter 0 (None) on every scanline, never adaptive. Adaptive filtering is the
/// largest single source of encoder-version-dependent output, and this repo
/// gates on digests; a filter heuristic is also worth very little on 4-bit
/// indexed data, where the byte to the left is two pixels away.
///
/// Nothing here allocates. `bound` and `scratch_bytes` say how large the two
/// caller-owned spans have to be — the same shape `plate.hpp` and `pack.hpp`
/// take, for the same reason.

#include <cstddef>
#include <cstdint>
#include <span>

#include "gloam/deflate.hpp"
#include "gloam/plate.hpp"

namespace gloam::png {

/// Why an encode was refused. `None` is success.
///
/// A discriminated enum rather than `std::expected`, for the reason spelled out
/// in `kitty.hpp`: these headers are installed, and Clang 18 cannot compile
/// `<expected>` against libstdc++.
enum class PngError : std::uint8_t {
  None = 0,
  NonPositiveExtent = 1,  ///< a zero-width plate is a bug, not an empty image
  ExtentOutOfRange = 2,   ///< beyond the u16 a §12 manifest record can carry
  BlobTooSmall = 3,       ///< smaller than plate::blob_bytes(width, height)
  ScratchTooSmall = 4,    ///< smaller than scratch_bytes(width, height)
  OutputTooSmall = 5,     ///< smaller than bound(width, height)
};

struct EncodeResult {
  PngError error{PngError::None};
  std::size_t bytes{0};  ///< bytes written; 0 on every error

  [[nodiscard]] constexpr explicit operator bool() const { return error == PngError::None; }
};

/// Bytes in one filtered scanline: the filter byte, then two pixels per byte.
///
/// An odd width leaves the final byte's low nibble unused. PNG requires those
/// padding bits to be zero, which is a real constraint rather than a courtesy —
/// a decoder is entitled to ignore them, and a digest is not.
[[nodiscard]] constexpr auto scanline_bytes(int width) -> std::size_t {
  return 1 + static_cast<std::size_t>((width + 1) / 2);
}

/// The scratch span `encode` needs: every scanline, filtered, before compression.
///
/// Explicit rather than internal, because this module allocates nothing and the
/// alternative is a hidden `std::vector` in the one code path §11 measures. The
/// compressor's own tables are the second scratch argument, `deflate::Scratch` —
/// separate because they are 32-bit positions rather than bytes, and shared
/// across plates because clearing them is cheaper than owning them twice.
[[nodiscard]] constexpr auto scratch_bytes(int width, int height) -> std::size_t {
  return scanline_bytes(width) * static_cast<std::size_t>(height);
}

/// The largest PNG `encode` can produce for this extent.
///
/// Signature, IHDR, PLTE, tRNS, IDAT and IEND, with the IDAT payload at
/// `deflate::bound` — the stored-block worst case, because a compressor that can
/// be beaten by its input still has to be able to say so.
[[nodiscard]] constexpr auto bound(int width, int height) -> std::size_t {
  constexpr std::size_t kSignature = 8;
  constexpr std::size_t kChunkOverhead = 12;  // length, type, CRC
  constexpr std::size_t kIhdrBytes = 13;
  constexpr std::size_t kPlteBytes = 15;      // five entries, three bytes each
  constexpr std::size_t kTrnsBytes = 1;
  return kSignature + (kChunkOverhead + kIhdrBytes) + (kChunkOverhead + kPlteBytes) +
         (kChunkOverhead + kTrnsBytes) +
         (kChunkOverhead + deflate::bound(scratch_bytes(width, height))) + kChunkOverhead;
}

/// Encode `src` as a complete PNG file into `output`.
///
/// Validates completely before writing anything: on any refusal both spans are
/// left untouched, so a caller cannot transmit a half-encoded image.
///
/// Deterministic. Two calls with the same plate produce the same bytes on every
/// compiler — `test/25png/` pins a digest, because §10's build-gate discipline
/// is only worth as much as the reproducibility underneath it.
[[nodiscard]] auto encode(const plate::PlateView& src, std::span<std::byte> scratch,
                          deflate::Scratch& matcher, std::span<std::byte> output) -> EncodeResult;

}  // namespace gloam::png
