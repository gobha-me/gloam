/// SPEC §4.1, §11 — a zlib stream, written here rather than linked.
///
/// RFC 1950 (zlib) around RFC 1951 (DEFLATE). Fixed Huffman, greedy matching,
/// stored-block fallback. The reasons each of those is the shape it is are in
/// `include/gloam/deflate.hpp`; what follows is the mechanism.

#include "gloam/deflate.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace gloam::deflate {
namespace {

/// RFC 1950 §2.2. CM=8 (deflate), CINFO=7 (a 32 KiB window), and an FLG whose
/// low five bits make the two-byte header a multiple of 31.
///
/// 0x7800 % 31 is 30, so FLG must be congruent to 1. FLEVEL is informational and
/// zero here — "fastest" is an honest description of a greedy matcher.
constexpr std::uint8_t kCmf = 0x78;
constexpr std::uint8_t kFlg = 0x01;

/// RFC 1951 §3.2.4. A stored block's payload length field is 16 bits.
constexpr std::size_t kStoredBlockMax = 65'535;

/// RFC 1950 §9. The largest prime below 65,536, which is what makes the modulo
/// a real checksum rather than a truncation.
constexpr std::uint32_t kAdlerModulus = 65'521;

// ── The matcher's parameters ────────────────────────────────────────────────

/// RFC 1951 §3.2.5's shortest and longest back-reference.
constexpr std::size_t kMinMatch = 3;
constexpr std::size_t kMaxMatch = 258;

/// One bucket per 15-bit hash of the next three bytes.
constexpr std::size_t kHashSize = kWindowBytes;
constexpr std::uint32_t kHashMask = kHashSize - 1;

/// How far back along a hash chain to look before giving up.
///
/// LOAD-BEARING, not a tuning knob. An uncapped chain is O(n^2) on input whose
/// three-byte prefixes nearly all collide, and a light field's scanline is
/// exactly that: 86,760 bytes of near-uniform screen door. Removing this cap
/// turns a ten-millisecond encode into an unbounded one — which is why
/// `24deflate-test` carries a ctest TIMEOUT, the way `15voicering-test` does.
/// Sixteen is where the measured ratio stops improving on this data.
constexpr int kMaxChainLength = 16;

[[nodiscard]] auto adler32(std::span<const std::byte> data) -> std::uint32_t {
  std::uint32_t s1 = 1;
  std::uint32_t s2 = 0;
  // Reduced every byte rather than every 5,552 (zlib's deferred-modulo trick).
  // The cold-start upload runs once per process and this is not where its time
  // goes; a running sum that is always in range is a running sum that cannot
  // overflow on an input size nobody predicted.
  for (const auto b : data) {
    s1 = (s1 + static_cast<std::uint32_t>(b)) % kAdlerModulus;
    s2 = (s2 + s1) % kAdlerModulus;
  }
  return (s2 << 16) | s1;
}

/// Big-endian, per RFC 1950 — the one field in a zlib stream that is not little
/// endian, and the classic way to produce a stream every inflater rejects.
auto put_be32(std::span<std::byte> out, std::size_t at, std::uint32_t v) -> void {
  out[at] = static_cast<std::byte>((v >> 24) & 0xFF);
  out[at + 1] = static_cast<std::byte>((v >> 16) & 0xFF);
  out[at + 2] = static_cast<std::byte>((v >> 8) & 0xFF);
  out[at + 3] = static_cast<std::byte>(v & 0xFF);
}

/// Write `input` as a sequence of stored (BTYPE=00) blocks.
[[nodiscard]] auto put_stored(std::span<const std::byte> input, std::span<std::byte> out)
    -> std::size_t {
  std::size_t written = 0;
  std::size_t offset = 0;

  // `do` rather than `while`: empty input still owes the stream one final block,
  // and an inflater handed a bare header with no block at all does not stop, it
  // waits.
  do {
    const auto take =
        input.size() - offset < kStoredBlockMax ? input.size() - offset : kStoredBlockMax;
    const bool final_block = offset + take >= input.size();

    // BFINAL in bit 0, BTYPE=00 in bits 1-2. A stored block is byte-aligned by
    // definition, and every block in this function is, so there is no partial
    // byte to flush before LEN.
    out[written++] = static_cast<std::byte>(final_block ? 0x01 : 0x00);

    const auto len = static_cast<std::uint16_t>(take);
    const auto nlen = static_cast<std::uint16_t>(~len);
    out[written++] = static_cast<std::byte>(len & 0xFF);
    out[written++] = static_cast<std::byte>((len >> 8) & 0xFF);
    out[written++] = static_cast<std::byte>(nlen & 0xFF);
    out[written++] = static_cast<std::byte>((nlen >> 8) & 0xFF);

    for (std::size_t i = 0; i < take; ++i) out[written++] = input[offset + i];
    offset += take;
  } while (offset < input.size());

  return written;
}

/// A bit sink over a caller-owned span, LSB-first within each byte.
///
/// Refuses to write past its span and remembers that it did, rather than
/// growing: the fallback to stored blocks is the caller's answer to a stream
/// that did not fit, and a writer that can overrun would make `bound` a lie.
class BitWriter {
 public:
  explicit BitWriter(std::span<std::byte> out) : m_out(out) {}

  /// `n` bits of `value`, least significant first — headers, lengths, distances.
  auto bits(std::uint32_t value, int n) -> void {
    for (int i = 0; i < n; ++i) put_bit((value >> i) & 1U);
  }

  /// A Huffman code: `n` bits of `code`, MOST significant first. RFC 1951 §3.1.1
  /// packs codes the other way round from everything else in the format, and
  /// this pair of functions is the only place that difference exists.
  auto code(std::uint32_t value, int n) -> void {
    for (int i = n - 1; i >= 0; --i) put_bit((value >> i) & 1U);
  }

  auto flush() -> void {
    if (m_bit_count > 0) put_bit_padding();
  }

  [[nodiscard]] auto bytes() const -> std::size_t { return m_at; }
  [[nodiscard]] auto overrun() const -> bool { return m_overrun; }

 private:
  auto put_bit(std::uint32_t bit) -> void {
    m_accumulator |= bit << m_bit_count;
    if (++m_bit_count == 8) {
      emit_byte();
    }
  }

  auto put_bit_padding() -> void {
    // The tail of the last byte is zero-filled. RFC 1951 says nothing about what
    // is in it, but a digest does, so it has to be something stated.
    emit_byte();
  }

  auto emit_byte() -> void {
    if (m_at < m_out.size()) {
      m_out[m_at] = static_cast<std::byte>(m_accumulator & 0xFF);
    } else {
      m_overrun = true;
    }
    ++m_at;
    m_accumulator = 0;
    m_bit_count = 0;
  }

  std::span<std::byte> m_out;
  std::size_t m_at{0};
  std::uint32_t m_accumulator{0};
  int m_bit_count{0};
  bool m_overrun{false};
};

/// RFC 1951 §3.2.6's fixed literal/length code, by symbol.
///
///   0-143    8 bits, 0x30-0xBF
///   144-255  9 bits, 0x190-0x1FF
///   256-279  7 bits, 0x00-0x17
///   280-287  8 bits, 0xC0-0xC7
auto put_symbol(BitWriter& w, unsigned symbol) -> void {
  if (symbol < 144) {
    w.code(0x30 + symbol, 8);
  } else if (symbol < 256) {
    w.code(0x190 + (symbol - 144), 9);
  } else if (symbol < 280) {
    w.code(symbol - 256, 7);
  } else {
    w.code(0xC0 + (symbol - 280), 8);
  }
}

/// RFC 1951 §3.2.5, transcribed. Index i is symbol 257 + i.
constexpr std::array<std::uint16_t, 29> kLengthBase{
    3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
    31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::array<std::uint8_t, 29> kLengthExtra{
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::array<std::uint16_t, 30> kDistanceBase{
    1,    2,    3,    4,    5,     7,     9,      13,     17,     25,
    33,   49,   65,   97,   129,   193,   257,    385,    513,    769,
    1025, 1537, 2049, 3073, 4097,  6145,  8193,   12'289, 16'385, 24'577};
constexpr std::array<std::uint8_t, 30> kDistanceExtra{
    0, 0, 0, 0, 1, 1, 2,  2,  3,  3,  4,  4,  5,  5,  6,
    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

/// The largest table index whose base is `<= value`. A linear scan over 29 or 30
/// entries, once per match: a binary search would be the same number of
/// comparisons at this size and one more place to be off by one.
template <std::size_t N>
[[nodiscard]] auto code_index(const std::array<std::uint16_t, N>& base, std::size_t value)
    -> std::size_t {
  std::size_t i = 0;
  while (i + 1 < N && base[i + 1] <= value) ++i;
  return i;
}

auto put_length(BitWriter& w, std::size_t length) -> void {
  const auto i = code_index(kLengthBase, length);
  put_symbol(w, static_cast<unsigned>(257 + i));
  w.bits(static_cast<std::uint32_t>(length - kLengthBase[i]), kLengthExtra[i]);
}

auto put_distance(BitWriter& w, std::size_t distance) -> void {
  const auto i = code_index(kDistanceBase, distance);
  // Distance codes are a fixed-width 5-bit code, not a Huffman code — but they
  // are packed MSB-first like one, which is why this is `code` and not `bits`.
  w.code(static_cast<std::uint32_t>(i), 5);
  w.bits(static_cast<std::uint32_t>(distance - kDistanceBase[i]), kDistanceExtra[i]);
}

/// A 15-bit hash of three bytes. Multiplicative, so that the low bits of the
/// result depend on all three inputs — a shift-and-xor hash buckets a screen
/// door's scanlines into a handful of chains and turns the cap below into the
/// only thing standing between this and a quadratic encode.
[[nodiscard]] auto hash3(std::span<const std::byte> in, std::size_t at) -> std::uint32_t {
  const auto a = static_cast<std::uint32_t>(in[at]);
  const auto b = static_cast<std::uint32_t>(in[at + 1]);
  const auto c = static_cast<std::uint32_t>(in[at + 2]);
  return ((a << 16 | b << 8 | c) * 2'654'435'761U >> 17) & kHashMask;
}

/// Emit `input` as one fixed-Huffman block. Returns the byte count, or 0 if the
/// stream did not fit — which the caller reads as "use stored blocks instead".
[[nodiscard]] auto put_fixed(std::span<const std::byte> input, Scratch& scratch,
                             std::span<std::byte> out) -> std::size_t {
  // 0 means "no position recorded"; positions are stored one-based so that the
  // table needs no separate emptiness flag and no fill with a sentinel.
  //
  // BOTH tables are cleared, and the second one is not obviously necessary —
  // every chain starts from a `head` written during this call. It is necessary
  // anyway: a chain's FIRST link is read out of `prev`, so a stale link from a
  // previous call is reachable, and a compressor whose output depends on what it
  // compressed an hour ago is not the deterministic function §10's build gate
  // needs it to be. Two hundred and fifty-six kilobytes of memset, six times per
  // process.
  auto& head = scratch.head;
  auto& prev = scratch.prev;
  head.fill(0);
  prev.fill(0);

  BitWriter w{out};
  w.bits(1, 1);  // BFINAL
  w.bits(1, 2);  // BTYPE=01, fixed Huffman

  std::size_t at = 0;
  while (at < input.size()) {
    std::size_t best_length = 0;
    std::size_t best_distance = 0;

    if (at + kMinMatch <= input.size()) {
      const auto h = hash3(input, at);
      auto candidate = head[h];

      for (int depth = 0; depth < kMaxChainLength && candidate != 0; ++depth) {
        const auto pos = static_cast<std::size_t>(candidate - 1);
        const auto distance = at - pos;
        if (distance > kWindowBytes) break;

        std::size_t length = 0;
        const auto limit = input.size() - at < kMaxMatch ? input.size() - at : kMaxMatch;
        while (length < limit && input[pos + length] == input[at + length]) ++length;

        // Strictly greater, so the SHORTEST distance wins a tie. Chains are
        // walked newest-first, so this is also the cheapest encoding of the same
        // match — and, more to the point, it is a rule rather than an accident,
        // which is what makes two compilers agree byte for byte.
        if (length > best_length) {
          best_length = length;
          best_distance = distance;
          if (length == kMaxMatch) break;
        }

        candidate = prev[pos & (kWindowBytes - 1)];
        // A chain link must point strictly backwards. Anything else is a stale
        // entry from a previous call, and following it would loop forever.
        if (candidate == 0 || static_cast<std::size_t>(candidate - 1) >= pos) break;
      }
    }

    if (best_length >= kMinMatch) {
      put_length(w, best_length);
      put_distance(w, best_distance);
    } else {
      put_symbol(w, static_cast<unsigned>(input[at]));
      best_length = 1;
    }

    // Insert every position the match covered, not just its first byte: a
    // position that is never inserted is a match nobody can ever find, and on
    // periodic data that is most of them.
    for (std::size_t i = 0; i < best_length; ++i) {
      const auto pos = at + i;
      if (pos + kMinMatch > input.size()) break;
      const auto h = hash3(input, pos);
      prev[pos & (kWindowBytes - 1)] = head[h];
      head[h] = static_cast<std::uint32_t>(pos + 1);
    }
    at += best_length;

    // Bail out as soon as the block has outgrown what a stored block would cost.
    // Without this, incompressible input walks the whole matcher to produce
    // something the caller then throws away.
    if (w.overrun()) return 0;
  }

  put_symbol(w, 256);  // end of block
  w.flush();

  return w.overrun() ? 0 : w.bytes();
}

}  // namespace

auto zlib_compress(std::span<const std::byte> input, Scratch& scratch, std::span<std::byte> output)
    -> CompressResult {
  if (output.size() < bound(input.size())) {
    return CompressResult{DeflateError::OutputTooSmall, 0};
  }

  std::size_t written = 0;
  output[written++] = static_cast<std::byte>(kCmf);
  output[written++] = static_cast<std::byte>(kFlg);

  // The compressed block is written straight into the output span, capped at
  // what a stored block would have cost — so "did it help?" is answered by
  // whether it fit, and a stream that did not is simply not there to undo.
  const auto stored_size = bound(input.size()) - 6;
  const auto compressed = put_fixed(input, scratch, output.subspan(written, stored_size));

  if (compressed > 0) {
    written += compressed;
  } else {
    written += put_stored(input, output.subspan(written));
  }

  put_be32(output, written, adler32(input));
  written += 4;

  return CompressResult{DeflateError::None, written};
}

}  // namespace gloam::deflate
