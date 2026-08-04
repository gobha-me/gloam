#pragma once

/// SPEC §4.1, §11 — a zlib stream, because §11's cold-start budget needs one.
///
/// This is the whole reason the cold-start payload row is met rather than
/// recorded as blown. The arithmetic that put it at 4.6x the budget (gloam#17) was
/// `f=32` RGBA: 480x360x4 bytes per plate, times six light fields, times base64's
/// four-thirds. §10's plate is 3 bits per pixel in the pack and PNG can carry it
/// at 4 — but even 4 bits per pixel is 520,560 B of scanline for the six fields,
/// and base64 would make that 694 KB against a 1.2 MB budget with 6 of M0's 71
/// plates existing. A budget met that way is a budget that fails on the first
/// authored wall ring. So the payload is compressed, and this is the compressor.
///
///
/// WHY GLOAM HAS ITS OWN
///
/// `${PROJECT_NAME}_DEPS` carries catch2 and nothing else, and §5.1's determinism
/// story is the reason: a compressor is a pure integer function of its input, and
/// linking one would put the wire bytes at the mercy of whatever zlib the build
/// host happened to have. `test/25png/` pins a digest over an encoded plate. That
/// digest is only meaningful if the encoder is the one in this tree.
///
/// It is also, deliberately, not a general-purpose compressor:
///
///   * Fixed Huffman only, never dynamic. A dynamic block needs a code-length
///     encoder and a tree builder, which is where cross-compiler determinism
///     bugs live, and it is worth a fraction of a budget already met with an
///     order of magnitude to spare.
///   * Greedy matching, never lazy or optimal. This note used to say "measured
///     at a few percent", which was not measured and was optimistic. What IS
///     measured, on lamp level 3's filtered scanline: this encoder produces a
///     2,220 B IDAT where a real zlib at the SAME block type (level 9,
///     `Z_FIXED`) produces 1,832 — so the greedy path costs about 21%. That
///     figure is lazy matching and a deeper chain together and cannot be split
///     without implementing one of them. It is 0.03% of a budget already met
///     with two orders of magnitude to spare, which is the actual argument.
///   * No decompressor. Nothing in GLOAM reads PNG.
///     `test/include/gloam_test/inflate.hpp` carries one for round-trip
///     verification, shared by `test/24deflate/` and `test/25png/`, and it stays
///     in the test tree.
///
/// Every one of those is a size/complexity trade taken in the direction of
/// "verifiable", because the thing being protected is a build gate, not a
/// download.
///
/// Nothing here allocates, holds a clock or touches a file descriptor: bytes in,
/// bytes out, into a span the caller sized with `bound`.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace gloam::deflate {

/// Why a compress was refused. `None` is success.
///
/// A discriminated enum rather than `std::expected`, for the reason spelled out
/// in `kitty.hpp`: these headers are installed, and Clang 18 cannot compile
/// `<expected>` against libstdc++.
enum class DeflateError : std::uint8_t {
  None = 0,
  /// The output span is smaller than `bound(input.size())`.
  OutputTooSmall = 1,
};

struct CompressResult {
  DeflateError error{DeflateError::None};
  std::size_t bytes{0};  ///< bytes written; 0 on refusal

  [[nodiscard]] constexpr explicit operator bool() const {
    return error == DeflateError::None;
  }
};

/// The largest zlib stream `zlib_compress` can produce for `raw_bytes` of input.
///
/// This is the STORED-BLOCK worst case, not the expected case, and it is what a
/// caller must size a buffer to: a compressor that can be beaten by its input
/// still has to be able to say so. A stored block carries at most 65,535 bytes
/// behind a five-byte header, plus the two-byte zlib header and the four-byte
/// Adler-32 trailer. The `+ 1` block covers both the final empty block and the
/// remainder, which is why this is deliberately one block looser than tight.
[[nodiscard]] constexpr auto bound(std::size_t raw_bytes) -> std::size_t {
  constexpr std::size_t kStoredBlockMax = 65'535;
  constexpr std::size_t kStoredHeaderBytes = 5;
  constexpr std::size_t kWrapperBytes = 2 + 4;
  const auto blocks = raw_bytes / kStoredBlockMax + 1;
  return raw_bytes + blocks * kStoredHeaderBytes + kWrapperBytes;
}

/// RFC 1951's maximum back-reference distance, and the size of the match window.
inline constexpr std::size_t kWindowBytes = 32'768;

/// The matcher's tables: one head per hash bucket, one link per window position.
///
/// A caller-owned object rather than a member, a `static` or a `std::vector`.
/// This module allocates nothing — the same discipline `plate.hpp` and `png.hpp`
/// keep — and a `static` table would make the one function in the tree that runs
/// on a startup path quietly non-reentrant.
///
/// A struct rather than a `std::span<std::byte>` because these are 32-bit
/// positions: carving them out of a byte span would mean a `reinterpret_cast`
/// onto memory whose alignment nothing guarantees, which is undefined behaviour
/// the UBSan leg is entitled to notice. A quarter of a megabyte is a lot to put
/// on a stack — allocate it once, near whoever owns the upload.
///
/// `zlib_compress` clears it on entry, so one instance can serve every plate and
/// the output cannot depend on what was compressed before it.
struct Scratch {
  std::array<std::uint32_t, kWindowBytes> head{};
  std::array<std::uint32_t, kWindowBytes> prev{};
};

/// Compress `input` into a complete zlib stream (RFC 1950 around RFC 1951).
///
/// Deterministic: the same input produces the same bytes on every compiler and
/// every run. That is a property `test/24deflate/` asserts rather than assumes,
/// because §10 makes a digest over encoded bytes a build gate.
///
/// Empty input is a success — it produces a valid zlib stream containing one
/// empty final block, which is what a zero-pixel image would need and what an
/// inflater expects to see.
///
/// Falls back to stored blocks whenever they are smaller, which is what makes
/// `bound` a real bound rather than an optimistic one: incompressible input
/// costs its own size plus a header, never more.
[[nodiscard]] auto zlib_compress(std::span<const std::byte> input, Scratch& scratch,
                                 std::span<std::byte> output) -> CompressResult;

}  // namespace gloam::deflate
