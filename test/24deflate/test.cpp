// SPEC §4.1, §11 — the compressor §11's cold-start budget is met with.
//
// A round-trip suite has a standing problem: an encoder verified only by its own
// decoder proves the two agree, not that either is right. Both halves here were
// written from RFC 1950 and RFC 1951 rather than from each other — the encoder in
// `src/lib/deflate.cpp`, the decoder in `test/include/gloam_test/inflate.hpp` —
// but that is an argument, not a check.
//
// THE CHECK IS THE ANCHOR CASES BELOW: three zlib streams produced by a real
// zlib, pasted in as bytes, that the decoder must decode. One stored, one fixed
// Huffman, one fixed Huffman carrying a back-reference. A bug shared by both of
// GLOAM's halves would have to also be a bug in zlib to survive them.
//
// What the encoder side is really guarding:
//
//   1. `bound` being a real bound. Incompressible input must cost its own size
//      plus a header, never more, or a caller's buffer overruns on data nobody
//      predicted. The fallback to stored blocks is what makes that true.
//   2. Determinism. §10 makes a digest over encoded bytes a build gate, so an
//      encoder whose output depends on what it compressed previously — through a
//      stale match table — is a red build on somebody else's machine and nowhere
//      else.
//   3. Termination. The hash chain is capped by a named constant, and the
//      light-field data is exactly the near-uniform input that makes an uncapped
//      chain quadratic. `24deflate-test` carries a ctest TIMEOUT for that reason.
//
// Failure matrix first, per AGENTS.md; the round trip is last.

#include <catch2/catch_all.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "gloam/deflate.hpp"
#include "gloam_test/inflate.hpp"

using gloam::deflate::bound;
using gloam::deflate::CompressResult;
using gloam::deflate::DeflateError;
using gloam::deflate::kWindowBytes;
using gloam::deflate::Scratch;
using gloam::deflate::zlib_compress;
using gloam_test::inflate_zlib;
using gloam_test::InflateError;

namespace {

/// One instance for the suite: a quarter of a megabyte, and re-using it is also
/// what exercises `zlib_compress`'s promise to clear it.
[[nodiscard]] auto matcher() -> Scratch& {
  static Scratch scratch;
  return scratch;
}

[[nodiscard]] auto bytes_of(std::string_view s) -> std::vector<std::byte> {
  std::vector<std::byte> out;
  out.reserve(s.size());
  for (const char c : s) out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
  return out;
}

template <std::size_t N>
[[nodiscard]] auto bytes_of(const std::array<std::uint8_t, N>& a) -> std::vector<std::byte> {
  std::vector<std::byte> out;
  out.reserve(N);
  for (const auto b : a) out.push_back(static_cast<std::byte>(b));
  return out;
}

struct Compressed {
  std::vector<std::byte> buffer;
  std::size_t size{0};

  [[nodiscard]] auto span() const -> std::span<const std::byte> {
    return std::span<const std::byte>{buffer}.first(size);
  }
};

[[nodiscard]] auto compress_ok(std::span<const std::byte> input) -> Compressed {
  Compressed out;
  out.buffer.assign(bound(input.size()), std::byte{0xA5});
  const auto r = zlib_compress(input, matcher(), out.buffer);
  REQUIRE(r.error == DeflateError::None);
  REQUIRE(r.bytes <= out.buffer.size());
  out.size = r.bytes;
  return out;
}

/// Compress, inflate, and require the original back.
auto round_trip(std::span<const std::byte> input) -> std::size_t {
  const auto compressed = compress_ok(input);
  const auto back = inflate_zlib(compressed.span());
  REQUIRE(back.error == InflateError::None);
  REQUIRE(back.bytes.size() == input.size());
  REQUIRE(std::equal(input.begin(), input.end(), back.bytes.begin()));
  return compressed.size;
}

/// A deterministic pseudo-random sequence. Incompressible by construction, and
/// the same on every platform — `std::mt19937` would be too, but a four-line LCG
/// is one fewer thing whose parameters have to be pinned in a comment.
[[nodiscard]] auto noise(std::size_t n, std::uint32_t seed = 1) -> std::vector<std::byte> {
  std::vector<std::byte> out;
  out.reserve(n);
  std::uint32_t s = seed;
  for (std::size_t i = 0; i < n; ++i) {
    s = s * 1'664'525U + 1'013'904'223U;
    out.push_back(static_cast<std::byte>((s >> 24) & 0xFF));
  }
  return out;
}

/// A screen door: the shape of the data this compressor actually exists for.
[[nodiscard]] auto periodic(std::size_t n, std::size_t period) -> std::vector<std::byte> {
  std::vector<std::byte> out;
  out.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    out.push_back(static_cast<std::byte>(i % period == 0 ? 0x11 : 0x00));
  }
  return out;
}

}  // namespace

// ── The anchors: streams a real zlib produced ───────────────────────────────

TEST_CASE("the decoder decodes a stored stream a real zlib produced", "[deflate]") {
  // zlib.compress(b"hello, gloam", 0) — BTYPE=00.
  const std::array<std::uint8_t, 23> stream{0x78, 0x01, 0x01, 0x0C, 0x00, 0xF3, 0xFF, 0x68,
                                            0x65, 0x6C, 0x6C, 0x6F, 0x2C, 0x20, 0x67, 0x6C,
                                            0x6F, 0x61, 0x6D, 0x1C, 0xE2, 0x04, 0x71};
  const auto out = inflate_zlib(bytes_of(stream));
  REQUIRE(out.error == InflateError::None);
  CHECK(out.bytes == bytes_of("hello, gloam"));
}

TEST_CASE("the decoder decodes a fixed-Huffman stream a real zlib produced", "[deflate]") {
  // zlib.compress(b"A", 9) — BTYPE=01, one literal.
  const std::array<std::uint8_t, 9> one{0x78, 0xDA, 0x73, 0x04, 0x00, 0x00, 0x42, 0x00, 0x42};
  const auto a = inflate_zlib(bytes_of(one));
  REQUIRE(a.error == InflateError::None);
  CHECK(a.bytes == bytes_of("A"));

  // zlib.compress(b"abcabcabcabc", 9) — BTYPE=01 with a back-reference, which is
  // the half a literals-only stream would leave untested.
  const std::array<std::uint8_t, 13> repeated{0x78, 0xDA, 0x4B, 0x4C, 0x4A, 0x4E, 0x84,
                                              0x21, 0x00, 0x1D, 0xE0, 0x04, 0x99};
  const auto r = inflate_zlib(bytes_of(repeated));
  REQUIRE(r.error == InflateError::None);
  CHECK(r.bytes == bytes_of("abcabcabcabc"));
}

// ── The failure matrix ──────────────────────────────────────────────────────

TEST_CASE("an output span one byte short is refused, not truncated", "[deflate]") {
  const auto input = bytes_of("the quick brown fox");
  std::vector<std::byte> out(bound(input.size()) - 1, std::byte{0xA5});

  const auto r = zlib_compress(input, matcher(), out);
  CHECK(r.error == DeflateError::OutputTooSmall);
  CHECK(r.bytes == 0);
  CHECK_FALSE(static_cast<bool>(r));

  // A truncated zlib stream is not a corrupt file an inflater rejects early — it
  // is a stream that decodes to a prefix and then reports a checksum failure,
  // which for a plate means half an image and a message nobody reads.
  for (const auto b : out) CHECK(b == std::byte{0xA5});
}

TEST_CASE("an empty output span is refused for empty input too", "[deflate]") {
  // `bound(0)` is not zero: an empty stream still owes a header, a final block
  // and a checksum.
  CHECK(bound(0) == 11);
  const auto r = zlib_compress(std::span<const std::byte>{}, matcher(), std::span<std::byte>{});
  CHECK(r.error == DeflateError::OutputTooSmall);
  CHECK(r.bytes == 0);
}

TEST_CASE("empty input compresses to a stream that decodes to nothing", "[deflate]") {
  const auto compressed = compress_ok(std::span<const std::byte>{});
  CHECK(compressed.size > 0);

  const auto back = inflate_zlib(compressed.span());
  REQUIRE(back.error == InflateError::None);
  CHECK(back.bytes.empty());
}

TEST_CASE("the bound is honoured on input the compressor cannot help", "[deflate]") {
  // The stored-block fallback, which is what makes `bound` a bound rather than a
  // hope. Noise has no matches to find, so a fixed-Huffman block would be LARGER
  // than the input — 9 bits for every byte above 0x7F.
  const auto sizes = GENERATE(std::size_t{1}, std::size_t{1'000}, std::size_t{65'534},
                              std::size_t{65'535}, std::size_t{65'536}, std::size_t{70'000});
  CAPTURE(sizes);

  const auto input = noise(sizes);
  const auto size = round_trip(input);

  CHECK(size <= bound(input.size()));
  // And it really did give up rather than inflate the payload: within a few
  // bytes per 64 KiB block of the input's own size.
  CHECK(size <= input.size() + 16 * (input.size() / 65'535 + 1));
}

TEST_CASE("a stored fallback spanning several blocks round-trips", "[deflate]") {
  // 65,535 is a stored block's maximum payload, so anything above it is at least
  // two blocks and only the LAST may set BFINAL. Getting that wrong produces a
  // stream that decodes to the first 64 KiB and stops.
  const auto input = noise(200'000, 7);
  const auto size = round_trip(input);
  CHECK(size <= bound(input.size()));
}

// ── The matcher ─────────────────────────────────────────────────────────────

TEST_CASE("a run longer than the maximum match length is encoded in pieces",
          "[deflate]") {
  // 258 is the longest single match RFC 1951 can express. A 10,000-byte run has
  // to become a chain of them, and an encoder that silently emitted a longer
  // length would produce a stream that decodes to too many bytes.
  std::vector<std::byte> input(10'000, std::byte{0x5A});
  const auto size = round_trip(input);

  // Roughly one match per 258 bytes, a few bits each — under 1% either way.
  CHECK(size < input.size() / 50);
}

TEST_CASE("a match at the window edge is found, and one past it is not",
          "[deflate]") {
  // The distance limit is 32,768. A back-reference one byte further is ILLEGAL —
  // its extra bits overflow the 13-bit field — so an encoder that computed the
  // distance off by one emits a stream that decodes to the wrong bytes.
  //
  // THIS CASE USED TO ASSERT NEITHER HALF OF ITS OWN NAME, and mutation testing
  // is what caught it: halving the window, raising it by one and raising it by
  // 4,096 all left the suite green, and two of those three make the encoder emit
  // an illegal stream. The reason was structural — the filler was `noise`, so the
  // whole input was incompressible and went to a STORED block, where the
  // matcher's distance limit is never on the emitted path at all.
  //
  // The filler is therefore compressible now, and the sizes are compared rather
  // than merely round-tripped: at the edge the marker is found and costs a
  // back-reference; one past it, the marker has to be spelled out again.
  const std::string_view marker = "GLOAM-MARKER-32768";

  const auto encode_with_gap = [&](std::size_t gap) {
    std::vector<std::byte> input;
    for (const char c : marker) input.push_back(static_cast<std::byte>(c));
    for (std::size_t i = 0; i < gap; ++i) {
      input.push_back(static_cast<std::byte>(i % 64 == 0 ? 0x22 : 0x00));
    }
    for (const char c : marker) input.push_back(static_cast<std::byte>(c));
    return round_trip(input);  // round_trip REQUIREs the bytes back, illegal or not
  };

  const auto at_edge = encode_with_gap(kWindowBytes - marker.size() - 1);
  const auto past_edge = encode_with_gap(kWindowBytes + 4'096);

  INFO("at the window edge " << at_edge << " B, past it " << past_edge << " B");
  CHECK(at_edge < past_edge);
}

TEST_CASE("a compressible payload containing every byte value round-trips",
          "[deflate]") {
  // RFC 1951's fixed literal table is two ranges: symbols 0-143 are 8 bits and
  // 144-255 are NINE. The second branch had no coverage at all until this case,
  // and mutation testing is how that was found — widening the branch by one
  // symbol left every one of the 37 tests green while producing a stream no
  // inflater accepts.
  //
  // Nothing else here can reach it. Every other fixed-Huffman case in this file
  // draws from a handful of low bytes, and the one input that does use high
  // bytes — `noise` — is incompressible, so it goes to a stored block where
  // `put_symbol` is never called at all.
  std::vector<std::byte> input;
  for (int rep = 0; rep < 8; ++rep) {
    for (int v = 0; v < 256; ++v) input.push_back(static_cast<std::byte>(v));
  }

  const auto size = round_trip(input);
  INFO("2,048 B of every byte value compressed to " << size << " B");
  // It really did take the fixed-Huffman path: a stored fallback could not be
  // smaller than the input.
  CHECK(size < input.size());
}

TEST_CASE("the matcher's constants are pinned by an encoded size, not a ratio",
          "[deflate]") {
  // The two determinism cases above compare the encoder against ITSELF, so a
  // constant that shifts every output equally is invisible to them, and the
  // ratio bands elsewhere in this file (`< size/20`, `< size/100`) are orders of
  // magnitude too loose to notice. Mutation testing found exactly that hole:
  // `kMinMatch` 3 -> 4 changes the wire bytes and left the whole suite green.
  //
  // A pinned size closes it. The encoder is deterministic across compilers —
  // `test/25png/`'s digest asserts the same thing about a whole file — so this
  // is a byte-level pin that costs one line and goes red the day a matcher
  // constant moves.
  CHECK(compress_ok(periodic(50'000, 5)).size == 351);
  CHECK(compress_ok(bytes_of("gloam gloam gloam gloam gloam gloam")).size == 16);
}

TEST_CASE("periodic data — what a light field actually is — compresses hard",
          "[deflate]") {
  const auto input = periodic(86'760, 8);  // one 480x360 plate's worth of scanline
  const auto size = round_trip(input);

  INFO("periodic input " << input.size() << " B compressed to " << size << " B");
  CHECK(size < input.size() / 20);
}

TEST_CASE("compression is bounded in time on near-uniform input", "[deflate]") {
  // Not a timing assertion — those are machine-dependent and this file has no
  // budget to police. It is a TERMINATION assertion: the hash chain cap is what
  // stops this input walking every previous position of the same bucket, and
  // without it this case does not run slowly, it effectively does not finish.
  // The ctest TIMEOUT on this suite is the other half.
  std::vector<std::byte> input(300'000, std::byte{0});
  const auto size = round_trip(input);
  CHECK(size < input.size() / 100);
}

// ── Determinism, last ───────────────────────────────────────────────────────

TEST_CASE("the same input compresses to the same bytes, every time", "[deflate]") {
  const auto input = periodic(50'000, 5);
  const auto first = compress_ok(input);

  for (int i = 0; i < 3; ++i) {
    CAPTURE(i);
    const auto again = compress_ok(input);
    REQUIRE(again.size == first.size);
    CHECK(std::equal(first.span().begin(), first.span().end(), again.span().begin()));
  }
}

TEST_CASE("a used match table produces the same bytes as a fresh one", "[deflate]") {
  // THE PROPERTY §10's BUILD GATE RESTS ON. The tables are caller-owned and
  // shared across plates, so if `zlib_compress` did not clear them, a plate's
  // encoding would depend on which plate was encoded before it — and the first
  // symptom would be two developers hashing the same pack to different values.
  const auto warm_up = periodic(40'000, 3);
  const auto subject = periodic(40'000, 11);

  Scratch fresh;
  std::vector<std::byte> from_fresh(bound(subject.size()));
  const auto a = zlib_compress(subject, fresh, from_fresh);
  REQUIRE(a.error == DeflateError::None);

  Scratch used;
  std::vector<std::byte> scribble(bound(warm_up.size()));
  REQUIRE(zlib_compress(warm_up, used, scribble).error == DeflateError::None);
  std::vector<std::byte> from_used(bound(subject.size()));
  const auto b = zlib_compress(subject, used, from_used);
  REQUIRE(b.error == DeflateError::None);

  REQUIRE(a.bytes == b.bytes);
  CHECK(std::equal(from_fresh.begin(), from_fresh.begin() + static_cast<std::ptrdiff_t>(a.bytes),
                   from_used.begin()));
}

TEST_CASE("the stream is a valid zlib stream by its own header rules", "[deflate]") {
  const auto compressed = compress_ok(bytes_of("gloam"));
  const auto s = compressed.span();
  REQUIRE(s.size() >= 6);

  const auto cmf = static_cast<std::uint32_t>(s[0]);
  const auto flg = static_cast<std::uint32_t>(s[1]);
  CHECK((cmf & 0x0FU) == 8);            // CM: deflate
  CHECK(((cmf >> 4) & 0x0FU) == 7);     // CINFO: a 32 KiB window
  CHECK((cmf * 256 + flg) % 31 == 0);   // the header's own check value
  CHECK((flg & 0x20U) == 0);            // no preset dictionary
}
