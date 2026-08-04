#pragma once

// A DEFLATE decompressor, for tests only. SPEC §4.1, §11.
//
// `include/gloam/deflate.hpp` states that GLOAM ships no decompressor and why:
// nothing in the game reads PNG, and an inflater in the library would be a
// hundred-odd lines of untested-in-production code carried for the benefit of a
// test suite. So it lives here, in `test/include/` — the hook
// `test/CMakeLists.txt:166-170` reserves for exactly this.
//
// IT IS DELIBERATELY NOT THE INVERSE OF THE ENCODER. Round-tripping through a
// decoder that shares the encoder's assumptions proves those assumptions
// self-consistent, not correct. This one is written from RFC 1951 and RFC 1950
// directly, with its own tables, and `test/24deflate/` anchors it on
// hand-assembled streams whose bytes were derived by hand BEFORE the encoder
// existed — so a bug shared by both would have to be a bug in the RFC reading of
// two independently written files.
//
// It rejects dynamic-Huffman blocks (BTYPE=10) rather than implementing them.
// That is not a gap: the encoder never emits one, and an inflater able to accept
// output the encoder cannot produce would let a future dynamic-block bug through
// the round-trip test that exists to catch it.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace gloam_test {

enum class InflateError : std::uint8_t {
  None = 0,
  TruncatedStream,     ///< ran out of bits mid-symbol
  BadZlibHeader,       ///< CM != 8, FCHECK wrong, or a preset dictionary
  BadStoredLength,     ///< LEN and NLEN are not complements
  DynamicBlock,        ///< BTYPE=10 — refused on purpose, see above
  ReservedBlockType,   ///< BTYPE=11
  BadSymbol,           ///< a code no fixed table defines
  DistanceTooFar,      ///< a back-reference before the start of the output
  AdlerMismatch,       ///< the stream decoded, but not to what was compressed
};

struct InflateResult {
  InflateError error{InflateError::None};
  std::vector<std::byte> bytes;

  [[nodiscard]] explicit operator bool() const { return error == InflateError::None; }
};

namespace detail {

/// RFC 1951 §3.2.5, the length and distance tables, transcribed from the RFC.
inline constexpr std::array<std::uint16_t, 29> kLengthBase{
    3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
    31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
inline constexpr std::array<std::uint8_t, 29> kLengthExtra{
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
inline constexpr std::array<std::uint16_t, 30> kDistanceBase{
    1,    2,    3,    4,    5,    7,     9,     13,    17,    25,
    33,   49,   65,   97,   129,  193,   257,   385,   513,   769,
    1025, 1537, 2049, 3073, 4097, 6145,  8193,  12'289, 16'385, 24'577};
inline constexpr std::array<std::uint8_t, 30> kDistanceExtra{
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4,  4,  5,  5,  6,
    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

/// A bit reader over the byte stream, LSB-first within each byte (RFC 1951 §3.1).
class BitReader {
 public:
  explicit BitReader(std::span<const std::byte> data) : m_data(data) {}

  /// `n` bits as an integer, least significant bit first — how every length,
  /// distance and header field is stored.
  [[nodiscard]] auto bits(int n) -> std::uint32_t {
    std::uint32_t v = 0;
    for (int i = 0; i < n; ++i) {
      const auto b = next_bit();
      v |= static_cast<std::uint32_t>(b) << i;
    }
    return v;
  }

  /// One bit, for walking a Huffman code. Huffman codes are packed with their
  /// MOST significant bit first, which is why they cannot be read with `bits`.
  [[nodiscard]] auto next_bit() -> int {
    if (m_byte >= m_data.size()) {
      m_overrun = true;
      return 0;
    }
    const auto v = (static_cast<std::uint32_t>(m_data[m_byte]) >> m_bit) & 1U;
    if (++m_bit == 8) {
      m_bit = 0;
      ++m_byte;
    }
    return static_cast<int>(v);
  }

  auto align() -> void {
    if (m_bit != 0) {
      m_bit = 0;
      ++m_byte;
    }
  }

  [[nodiscard]] auto byte_position() const -> std::size_t { return m_byte; }
  [[nodiscard]] auto overrun() const -> bool { return m_overrun; }
  auto skip_bytes(std::size_t n) -> void { m_byte += n; }

  [[nodiscard]] auto byte_at(std::size_t i) -> std::uint8_t {
    if (i >= m_data.size()) {
      m_overrun = true;
      return 0;
    }
    return static_cast<std::uint8_t>(m_data[i]);
  }

 private:
  std::span<const std::byte> m_data;
  std::size_t m_byte{0};
  int m_bit{0};
  bool m_overrun{false};
};

/// Decode one literal/length symbol from the fixed table (RFC 1951 §3.2.6).
///
/// The ranges, written out rather than built into a tree: 7-bit codes 0x00-0x17
/// are symbols 256-279, 8-bit 0x30-0xBF are 0-143, 8-bit 0xC0-0xC7 are 280-287,
/// and 9-bit 0x190-0x1FF are 144-255.
[[nodiscard]] inline auto fixed_literal(BitReader& in, bool& bad) -> int {
  std::uint32_t code = 0;
  for (int len = 1; len <= 9; ++len) {
    code = (code << 1) | static_cast<std::uint32_t>(in.next_bit());
    if (len == 7 && code <= 0x17) return static_cast<int>(code) + 256;
    if (len == 8 && code >= 0x30 && code <= 0xBF) return static_cast<int>(code - 0x30);
    if (len == 8 && code >= 0xC0 && code <= 0xC7) return static_cast<int>(code - 0xC0) + 280;
    if (len == 9 && code >= 0x190 && code <= 0x1FF) return static_cast<int>(code - 0x190) + 144;
  }
  bad = true;
  return -1;
}

[[nodiscard]] inline auto adler32(std::span<const std::byte> data) -> std::uint32_t {
  std::uint32_t s1 = 1;
  std::uint32_t s2 = 0;
  for (const auto b : data) {
    s1 = (s1 + static_cast<std::uint32_t>(b)) % 65'521U;
    s2 = (s2 + s1) % 65'521U;
  }
  return (s2 << 16) | s1;
}

}  // namespace detail

/// Decompress a raw DEFLATE stream (no zlib wrapper).
[[nodiscard]] inline auto inflate_raw(std::span<const std::byte> in) -> InflateResult {
  InflateResult out;
  detail::BitReader r{in};

  for (;;) {
    const auto final_block = r.bits(1);
    const auto type = r.bits(2);

    if (type == 0) {
      r.align();
      const auto pos = r.byte_position();
      const auto len = static_cast<std::uint32_t>(r.byte_at(pos)) |
                       (static_cast<std::uint32_t>(r.byte_at(pos + 1)) << 8);
      const auto nlen = static_cast<std::uint32_t>(r.byte_at(pos + 2)) |
                        (static_cast<std::uint32_t>(r.byte_at(pos + 3)) << 8);
      if ((len ^ 0xFFFFU) != nlen) return {InflateError::BadStoredLength, {}};
      r.skip_bytes(4);
      for (std::uint32_t i = 0; i < len; ++i) {
        out.bytes.push_back(static_cast<std::byte>(r.byte_at(r.byte_position() + i)));
      }
      r.skip_bytes(len);
      if (r.overrun()) return {InflateError::TruncatedStream, {}};
    } else if (type == 1) {
      for (;;) {
        bool bad = false;
        const auto sym = detail::fixed_literal(r, bad);
        if (bad) return {InflateError::BadSymbol, {}};
        if (r.overrun()) return {InflateError::TruncatedStream, {}};

        if (sym < 256) {
          out.bytes.push_back(static_cast<std::byte>(sym));
          continue;
        }
        if (sym == 256) break;  // end of block
        if (sym > 285) return {InflateError::BadSymbol, {}};

        const auto li = static_cast<std::size_t>(sym - 257);
        const auto length = static_cast<std::size_t>(detail::kLengthBase[li]) +
                            r.bits(detail::kLengthExtra[li]);

        // Distance codes are a 5-bit fixed-width code, MSB first.
        std::uint32_t dist_code = 0;
        for (int i = 0; i < 5; ++i) {
          dist_code = (dist_code << 1) | static_cast<std::uint32_t>(r.next_bit());
        }
        if (dist_code >= detail::kDistanceBase.size()) return {InflateError::BadSymbol, {}};
        const auto distance = static_cast<std::size_t>(detail::kDistanceBase[dist_code]) +
                              r.bits(detail::kDistanceExtra[dist_code]);
        if (r.overrun()) return {InflateError::TruncatedStream, {}};
        if (distance == 0 || distance > out.bytes.size()) {
          return {InflateError::DistanceTooFar, {}};
        }

        // Byte at a time, on purpose: an overlapping copy (distance < length) is
        // legal and is how a run is encoded, so a memmove here would be wrong.
        const auto from = out.bytes.size() - distance;
        for (std::size_t i = 0; i < length; ++i) out.bytes.push_back(out.bytes[from + i]);
      }
    } else if (type == 2) {
      return {InflateError::DynamicBlock, {}};
    } else {
      return {InflateError::ReservedBlockType, {}};
    }

    if (r.overrun()) return {InflateError::TruncatedStream, {}};
    if (final_block != 0) break;
  }

  return out;
}

/// Decompress a zlib stream (RFC 1950), checking the header and the Adler-32.
[[nodiscard]] inline auto inflate_zlib(std::span<const std::byte> in) -> InflateResult {
  if (in.size() < 6) return {InflateError::TruncatedStream, {}};

  const auto cmf = static_cast<std::uint32_t>(in[0]);
  const auto flg = static_cast<std::uint32_t>(in[1]);
  if ((cmf & 0x0FU) != 8) return {InflateError::BadZlibHeader, {}};
  if ((cmf * 256 + flg) % 31 != 0) return {InflateError::BadZlibHeader, {}};
  if ((flg & 0x20U) != 0) return {InflateError::BadZlibHeader, {}};  // FDICT

  auto result = inflate_raw(in.subspan(2, in.size() - 6));
  if (!result) return result;

  const auto at = in.size() - 4;
  const auto stated = (static_cast<std::uint32_t>(in[at]) << 24) |
                      (static_cast<std::uint32_t>(in[at + 1]) << 16) |
                      (static_cast<std::uint32_t>(in[at + 2]) << 8) |
                      static_cast<std::uint32_t>(in[at + 3]);
  if (stated != detail::adler32(result.bytes)) return {InflateError::AdlerMismatch, {}};

  return result;
}

}  // namespace gloam_test
