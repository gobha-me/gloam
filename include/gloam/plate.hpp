#pragma once

/// SPEC §3.1, §4.3, §10 — what a plate IS, in bytes.
///
/// §10's palette is "four colours plus transparent, 1-bit dithered". That is five
/// states, and two bits hold four. The resolution here is TWO PLANES in one blob:
///
///   index plane    2 bits per pixel, colour only
///   stencil plane  1 bit per pixel, opacity only
///
/// Stealing a palette entry for transparency would cost 25% of a palette that IS
/// the entire art direction. The stencil costs 50% more bytes on a payload
/// already an eighth the size of RGBA, and is exactly what a PNG alpha or `tRNS`
/// channel would carry — so `pack::Codec::Png` stays a re-container rather than a
/// re-pack. Both planes are row-major with byte-aligned rows and MSB-first bit
/// order, which is PNG's order, for the same reason.
///
///
/// NOTHING HERE OWNS A PLATE
///
/// `include/gloam/kitty.hpp`'s transmit note warns that a pixel source "would
/// drag image OWNERSHIP — a buffer of plate data — into `gloam::lib`". It does
/// not. Every entry point below is `(caller-owned span, integers) -> bytes
/// written into that span`, with `blob_bytes` telling the caller how large to
/// make it. The library gains the pack's grammar and its arithmetic; it never
/// allocates, holds or frees a plate. `src/bin/bake.cpp` owns the buffers and is
/// the only file in the pipeline that opens a file descriptor.
///
/// Excluded from the `gloam/gloam.hpp` umbrella — pipeline-side, not simulation.

#include <cstddef>
#include <cstdint>
#include <span>

#include "gloam/geometry.hpp"

namespace gloam::plate {

/// The four palette entries. Transparency is NOT one of them — see the stencil.
///
/// Deliberately unnamed by colour. §10 fixes the count, not the values; the
/// concrete RGBA the uploader expands these into is a look decision that belongs
/// with the transmit path, and putting it here would make a palette tweak change
/// `pack_sha256`.
enum class Ink : std::uint8_t { Ink0 = 0, Ink1 = 1, Ink2 = 2, Ink3 = 3 };

inline constexpr int kInkCount = 4;

/// Rows are byte-aligned in both planes, so a row's tail bits are padding.
[[nodiscard]] constexpr auto index_row_bytes(int width) -> std::size_t {
  return static_cast<std::size_t>((width + 3) / 4);
}

[[nodiscard]] constexpr auto stencil_row_bytes(int width) -> std::size_t {
  return static_cast<std::size_t>((width + 7) / 8);
}

[[nodiscard]] constexpr auto index_plane_bytes(int width, int height) -> std::size_t {
  return index_row_bytes(width) * static_cast<std::size_t>(height);
}

[[nodiscard]] constexpr auto stencil_plane_bytes(int width, int height) -> std::size_t {
  return stencil_row_bytes(width) * static_cast<std::size_t>(height);
}

/// Total blob size: the index plane, then the stencil plane, contiguous.
///
/// This is what a caller allocates and what a manifest record's `length` must
/// equal under `pack::Codec::RawPlanes`.
[[nodiscard]] constexpr auto blob_bytes(int width, int height) -> std::size_t {
  return index_plane_bytes(width, height) + stencil_plane_bytes(width, height);
}

// ── The bit order, stated once ──────────────────────────────────────────────
//
// MSB-first is a FORMAT COMMITMENT, not an implementation detail: it is PNG's
// order, which is what keeps `pack::Codec::Png` a re-container rather than a
// re-pack. So the shift lives here, in the header, rather than in whichever
// .cpp happened to need it first — `lightfield.cpp` packs stencil bytes
// directly for speed and must not re-derive the convention, or flipping it
// would silently desynchronise the writer from every reader.

/// The bit within its byte holding pixel `x` of a stencil row. 7 is pixel 0.
[[nodiscard]] constexpr auto stencil_shift(int x) -> int { return 7 - (x % 8); }

/// The mask selecting that bit.
[[nodiscard]] constexpr auto stencil_mask(int x) -> std::uint8_t {
  return static_cast<std::uint8_t>(1U << stencil_shift(x));
}

/// The low bit of the two holding pixel `x` of an index row. 6 is pixel 0.
[[nodiscard]] constexpr auto index_shift(int x) -> int { return 6 - 2 * (x % 4); }

/// Why an operation was refused. `None` is success.
///
/// A discriminated enum rather than `std::expected`, for the reason spelled out
/// in `kitty.hpp`: these headers are installed, and Clang 18 cannot compile
/// `<expected>` against libstdc++.
enum class PlateError : std::uint8_t {
  None = 0,
  NonPositiveExtent = 1,          ///< a zero-width plate is a bug, not an empty plate
  ExtentOutOfRange = 2,           ///< beyond the u16 a §12 manifest record can carry
  BufferTooSmall = 3,             ///< smaller than blob_bytes(width, height)
  CoordinateOutOfRange = 4,
  OddSourceExtent = 5,            ///< a 2:1 downsample of an odd extent is not exact
  DestinationExtentMismatch = 6,  ///< dst is not exactly src/2 on both axes
};

struct PlateResult {
  PlateError error{PlateError::None};
  std::size_t bytes{0};  ///< bytes touched; 0 on every error

  [[nodiscard]] constexpr explicit operator bool() const { return error == PlateError::None; }
};

/// A read-only view over a caller-owned blob. Three fields, no invariants — the
/// checking lives in `validate`, which every entry point calls first.
struct PlateView {
  std::span<const std::byte> blob;
  int width{0};
  int height{0};
};

struct PlateSpan {
  std::span<std::byte> blob;
  int width{0};
  int height{0};
};

/// Does the dither pattern tile this width a whole number of times (§4.3)?
///
/// A PROPERTY OF AUTHORED RING SIZES, NOT OF EVERY PLATE — which is why it is a
/// predicate here rather than a rule inside `validate`. `geometry.hpp` already
/// static_asserts it for all five rings of the ladder, and that is where it
/// belongs: §4.3 asks that the pattern not land on a fractional boundary on the
/// plates GLOAM composes, not that no plate may ever have another size.
///
/// Enforcing it universally is a trap, and an expensive one. It would refuse a
/// 24x24 rune glyph — §4.3's own dither BLOCK, the natural authoring size — and
/// it would refuse to halve the ladder's own 168-wide depth-3 ring, because 84
/// is not a multiple of 8. The ladder currently survives that only because
/// depth 4 is derived from depth 2 rather than depth 3.
[[nodiscard]] constexpr auto dither_aligned(int width) -> bool {
  return width > 0 && width % geometry::kDitherCell == 0;
}

/// Is this extent representable, and is the blob big enough for it?
///
/// Deliberately does NOT check `dither_aligned` — see above.
[[nodiscard]] auto validate(int width, int height, std::size_t blob_size) -> PlateError;

/// One pixel, both planes. `error` is `None` on success and the other fields are
/// meaningless otherwise.
///
/// Both planes in one call because every real consumer — an RGBA expander, a PNG
/// encoder, the downsampler — wants both, and one call halves the surface that
/// can be bounds-checked wrong.
struct Pixel {
  PlateError error{PlateError::None};
  Ink ink{Ink::Ink0};
  bool opaque{false};
};

[[nodiscard]] auto read(const PlateView& src, int x, int y) -> Pixel;

/// Write one pixel. On any error the blob is left byte-for-byte untouched.
[[nodiscard]] auto write(const PlateSpan& dst, int x, int y, Ink ink, bool opaque) -> PlateResult;

/// Exact 2:1 box downsample — §3.1's derived depths 2, 3 and 4.
///
/// "Each depth is 1/sqrt(2) of the one before it, so every second depth is
/// exactly half." The pipeline authors the depth-0 and depth-1 rings and derives
/// the rest with this, which is §16's named mitigation for art volume exceeding
/// solo capacity.
///
/// Both reduction rules are TOTAL and ORDER-FREE, which is what makes two runs
/// byte-identical on two compilers rather than merely equivalent:
///
///   stencil  opaque iff at least 2 of the 4 source pixels are opaque
///   ink      majority over the OPAQUE sources only, ties to the lowest Ink
///
/// A transparent source's ink never votes: it is not a colour, it is an absence,
/// and letting it vote would drag the shadow entry into every silhouette edge.
/// If no source is opaque the destination is transparent with `Ink0`.
[[nodiscard]] auto downsample_2to1(const PlateView& src, const PlateSpan& dst) -> PlateResult;

}  // namespace gloam::plate
