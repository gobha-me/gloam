// SPEC §4.1, §11 — the encoding the cold-start upload pays for.
//
// Base64 looks too simple to be worth a failure matrix, which is exactly why it
// gets one. The three things that actually go wrong here all go wrong SILENTLY:
//
//   1. Sign extension. A plate's bytes routinely have the high bit set, and
//      `char` is signed on x86 — an implementation that reaches for `char`
//      instead of `std::uint32_t` corrupts every byte above 0x7F and nothing
//      else. The image arrives, it is just wrong.
//   2. The tail. One and two leftover bytes carry real bits in the digit that
//      straddles the group boundary; dropping that digit and writing padding
//      instead loses the last pixels of a scanline rather than erroring.
//   3. The size formula. `encoded_size` is budget arithmetic (§11 caps the
//      upload at 1.2 MB), so a formula that is right for multiples of three and
//      wrong for the tail understates the wire cost of every real payload.
//
// Failure matrix first, per AGENTS.md; RFC 4648's own vectors are last, because
// a golden that agrees with a buggy implementation of the same misreading proves
// nothing until the refusals are pinned.

#include <catch2/catch_all.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "gloam/base64.hpp"

using gloam::base64::Base64Error;
using gloam::base64::encode;
using gloam::base64::encoded_size;

namespace {

[[nodiscard]] auto bytes_of(std::string_view s) -> std::vector<std::byte> {
  std::vector<std::byte> out;
  out.reserve(s.size());
  for (const char c : s) out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
  return out;
}

/// Encode into a span sized by the formula, and hand back what was written.
[[nodiscard]] auto encoded(std::span<const std::byte> in) -> std::string {
  std::vector<char> out(encoded_size(in.size()) + 8, '\0');
  const auto r = encode(in, std::span<char>{out});
  REQUIRE(r.error == Base64Error::None);
  return std::string{out.data(), r.bytes};
}

}  // namespace

// ── The failure matrix ──────────────────────────────────────────────────────

TEST_CASE("an output span one character short is refused, not truncated", "[base64]") {
  const auto in = bytes_of("abcd");
  REQUIRE(encoded_size(in.size()) == 8);

  std::vector<char> out(7, 'Z');
  const auto r = encode(in, std::span<char>{out});

  CHECK(r.error == Base64Error::OutputTooSmall);
  CHECK(r.bytes == 0);
  CHECK_FALSE(static_cast<bool>(r));

  // And it wrote nothing: a partial encode of a chunked upload is a payload the
  // terminal decodes to garbage rather than rejects.
  for (const char c : out) CHECK(c == 'Z');
}

TEST_CASE("an empty output span is refused for any non-empty input", "[base64]") {
  const auto in = bytes_of("a");
  const auto r = encode(in, std::span<char>{});
  CHECK(r.error == Base64Error::OutputTooSmall);
  CHECK(r.bytes == 0);
}

TEST_CASE("empty input is a success that writes nothing", "[base64]") {
  // Distinguished from a refusal by the error, not by the count — both are zero.
  std::vector<char> out(4, 'Z');
  const auto r = encode(std::span<const std::byte>{}, std::span<char>{out});

  CHECK(r.error == Base64Error::None);
  CHECK(r.bytes == 0);
  CHECK(static_cast<bool>(r));
  CHECK(out[0] == 'Z');

  // Empty input against an EMPTY output is also a success: zero fits in zero.
  const auto empty = encode(std::span<const std::byte>{}, std::span<char>{});
  CHECK(empty.error == Base64Error::None);
  CHECK(empty.bytes == 0);
}

TEST_CASE("the size formula is exact at every residue, including zero", "[base64]") {
  CHECK(encoded_size(0) == 0);
  CHECK(encoded_size(1) == 4);
  CHECK(encoded_size(2) == 4);
  CHECK(encoded_size(3) == 4);
  CHECK(encoded_size(4) == 8);
  CHECK(encoded_size(5) == 8);
  CHECK(encoded_size(6) == 8);

  // §11's arithmetic: the tax is 4 bytes per 3, so a payload the size of the
  // budget itself does not fit inside the budget once encoded.
  CHECK(encoded_size(1'200'000) == 1'600'000);

  // The chunk size `src/lib/kitty.cpp` transmits on, both directions.
  CHECK(encoded_size(3'072) == 4'096);
  CHECK(encoded_size(3'071) == 4'096);
  CHECK(encoded_size(3'069) == 4'092);

  // And the written count always agrees with the promise.
  for (std::size_t n = 0; n <= 16; ++n) {
    const std::vector<std::byte> in(n, std::byte{0x42});
    CAPTURE(n);
    CHECK(encoded(in).size() == encoded_size(n));
  }
}

TEST_CASE("the three residues pad the way RFC 4648 says, and only at the end", "[base64]") {
  // One leftover byte: two digits then "==". The second digit is not padding —
  // it carries the low two bits of the input byte.
  CHECK(encoded(bytes_of("f")) == "Zg==");
  // Two leftover bytes: three digits then "=".
  CHECK(encoded(bytes_of("fo")) == "Zm8=");
  // No leftover: no padding at all.
  CHECK(encoded(bytes_of("foo")) == "Zm9v");

  // The straddling digit really is load-bearing: two inputs differing only in
  // the bits it carries must not encode identically.
  const std::array<std::byte, 1> a{std::byte{0x00}};
  const std::array<std::byte, 1> b{std::byte{0x03}};
  CHECK(encoded(a) != encoded(b));
  CHECK(encoded(a) == "AA==");
  CHECK(encoded(b) == "Aw==");
}

TEST_CASE("a high-bit payload survives, which is where sign extension shows", "[base64]") {
  const std::array<std::byte, 3> high{std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}};
  CHECK(encoded(high) == "////");

  const std::array<std::byte, 3> mixed{std::byte{0x80}, std::byte{0x7F}, std::byte{0xFF}};
  CHECK(encoded(mixed) == "gH//");

  // Every single byte value, encoded on its own: a sign-extension bug shows up
  // as a repeated or out-of-alphabet digit somewhere above 0x7F.
  std::string seen;
  for (int v = 0; v < 256; ++v) {
    const std::array<std::byte, 1> one{static_cast<std::byte>(v)};
    const auto s = encoded(one);
    REQUIRE(s.size() == 4);
    CHECK(s[2] == '=');
    CHECK(s[3] == '=');
    seen += s[0];
  }
  // The first digit is the top six bits, so it takes each of the 64 values in
  // the alphabet exactly four times across 0..255.
  for (const char c : seen) {
    CHECK(std::string_view{"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"}
              .find(c) != std::string_view::npos);
  }
}

TEST_CASE("an embedded NUL is encoded, not treated as a terminator", "[base64]") {
  // The sink this feeds is NUL-transparent (`emit.hpp`) and a PNG payload's
  // second byte is 0x50 after an 0x89 — but an IDAT is full of zeros, and a
  // C-string reading of the input would truncate the upload at the first one.
  const std::array<std::byte, 6> nul{std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
                                     std::byte{0x00}, std::byte{0x00}, std::byte{0x01}};
  const auto s = encoded(nul);
  CHECK(s.size() == 8);
  CHECK(s == "AAAAAAAB");
}

// ── The golden, last ────────────────────────────────────────────────────────

TEST_CASE("RFC 4648 section 10's test vectors, verbatim", "[base64]") {
  CHECK(encoded(bytes_of("")) == "");
  CHECK(encoded(bytes_of("f")) == "Zg==");
  CHECK(encoded(bytes_of("fo")) == "Zm8=");
  CHECK(encoded(bytes_of("foo")) == "Zm9v");
  CHECK(encoded(bytes_of("foob")) == "Zm9vYg==");
  CHECK(encoded(bytes_of("fooba")) == "Zm9vYmE=");
  CHECK(encoded(bytes_of("foobar")) == "Zm9vYmFy");
}
