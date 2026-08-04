// SPEC §4.1, §10, §11 — §10's plate as an `f=100` payload.
//
// Three things this file is really guarding, all of which fail INVISIBLY:
//
//   1. The five-state merge. A plate carries ink and opacity in separate planes;
//      PNG carries one index per pixel. Nothing checks that the merge is right
//      except this file, and a nibble written to the wrong half of its byte
//      swaps two neighbouring pixels — which on a screen-door light field is
//      indistinguishable from the dither it is supposed to be.
//   2. The chunk envelope. A CRC computed over the data but not the type
//      produces a file that libpng rejects and several decoders accept. The
//      terminal is one of the decoders, so the bug ships and surfaces on
//      somebody else's terminal.
//   3. The padding nibble. §10 makes a digest over bytes a build gate. An odd
//      width leaves half a byte unused, and a decoder is entitled to ignore what
//      is in it — a digest is not.
//
// The six light fields that exist today use ONE ink and the stencil, so real
// data exercises 2 of the 5 palette entries. The all-inks plate below is not
// optional coverage: without it, entries 2-4 and the high nibble are untested
// until authored art lands, which is after M0.
//
// Failure matrix first, per AGENTS.md; the decode and the digest are last.

#include <catch2/catch_all.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "gloam/deflate.hpp"
#include "gloam/lightfield.hpp"
#include "gloam/palette.hpp"
#include "gloam/plate.hpp"
#include "gloam/png.hpp"
#include "gloam/sha256.hpp"
#include "gloam_test/inflate.hpp"

using gloam::plate::Ink;
using gloam::plate::PlateSpan;
using gloam::plate::PlateView;
using gloam::png::EncodeResult;
using gloam::png::PngError;

namespace {

struct Encoded {
  std::vector<std::byte> bytes;
  std::size_t size{0};

  [[nodiscard]] auto span() const -> std::span<const std::byte> {
    return std::span<const std::byte>{bytes}.first(size);
  }
};

/// A plate whose every pixel is `(ink(x,y), opaque(x,y))`.
template <typename InkFn, typename OpaqueFn>
[[nodiscard]] auto make_plate(int w, int h, InkFn ink, OpaqueFn opaque) -> std::vector<std::byte> {
  std::vector<std::byte> blob(gloam::plate::blob_bytes(w, h), std::byte{0});
  const PlateSpan dst{std::span<std::byte>{blob}, w, h};
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      REQUIRE(gloam::plate::write(dst, x, y, ink(x, y), opaque(x, y)).error ==
              gloam::plate::PlateError::None);
    }
  }
  return blob;
}

/// The matcher's tables are a quarter of a megabyte, so one instance serves the
/// whole suite — which also exercises `zlib_compress`'s promise that it clears
/// them, since every case after the first sees a used one.
[[nodiscard]] auto matcher() -> gloam::deflate::Scratch& {
  static gloam::deflate::Scratch scratch;
  return scratch;
}

[[nodiscard]] auto encode_ok(const PlateView& src) -> Encoded {
  Encoded out;
  out.bytes.assign(gloam::png::bound(src.width, src.height), std::byte{0xA5});
  std::vector<std::byte> scratch(gloam::png::scratch_bytes(src.width, src.height), std::byte{0xA5});
  const auto r = gloam::png::encode(src, scratch, matcher(), out.bytes);
  REQUIRE(r.error == PngError::None);
  out.size = r.bytes;
  return out;
}

struct Chunk {
  std::string type;
  std::vector<std::byte> data;
  std::uint32_t stated_crc{0};
  std::uint32_t computed_crc{0};
};

[[nodiscard]] auto be32(std::span<const std::byte> s, std::size_t at) -> std::uint32_t {
  return (static_cast<std::uint32_t>(s[at]) << 24) | (static_cast<std::uint32_t>(s[at + 1]) << 16) |
         (static_cast<std::uint32_t>(s[at + 2]) << 8) | static_cast<std::uint32_t>(s[at + 3]);
}

/// An independent CRC-32, written from the PNG specification rather than shared
/// with the encoder — a checksum verified with its own implementation checks
/// nothing at all.
[[nodiscard]] auto png_crc(std::span<const std::byte> data) -> std::uint32_t {
  std::uint32_t c = 0xFFFFFFFFU;
  for (const auto b : data) {
    c ^= static_cast<std::uint32_t>(b);
    for (int k = 0; k < 8; ++k) c = (c & 1U) != 0U ? 0xEDB88320U ^ (c >> 1) : c >> 1;
  }
  return c ^ 0xFFFFFFFFU;
}

/// Walk the file into its chunks, checking nothing — the assertions live in the
/// test cases so a structural surprise reports as a failed CHECK rather than as
/// a parser crash.
[[nodiscard]] auto parse(std::span<const std::byte> file) -> std::vector<Chunk> {
  std::vector<Chunk> out;
  std::size_t at = 8;  // past the signature
  while (at + 12 <= file.size()) {
    Chunk c;
    const auto len = be32(file, at);
    for (std::size_t i = 0; i < 4; ++i) {
      c.type += static_cast<char>(static_cast<unsigned char>(file[at + 4 + i]));
    }
    const auto data_at = at + 8;
    if (data_at + len + 4 > file.size()) break;
    c.data.assign(file.begin() + static_cast<std::ptrdiff_t>(data_at),
                  file.begin() + static_cast<std::ptrdiff_t>(data_at + len));
    c.stated_crc = be32(file, data_at + len);
    c.computed_crc = png_crc(file.subspan(at + 4, 4 + len));
    out.push_back(std::move(c));
    at = data_at + len + 4;
  }
  return out;
}

[[nodiscard]] auto idat_of(const std::vector<Chunk>& chunks) -> std::span<const std::byte> {
  for (const auto& c : chunks) {
    if (c.type == "IDAT") return c.data;
  }
  FAIL("no IDAT chunk");
  return {};
}

constexpr auto kAllInks = [](int x, int y) { return static_cast<Ink>((x + y) % 4); };
constexpr auto kCheckers = [](int x, int y) { return (x + y) % 2 == 0; };

}  // namespace

// ── The failure matrix ──────────────────────────────────────────────────────

TEST_CASE("a non-positive extent is refused, and the spans are untouched", "[png]") {
  const auto w = GENERATE(0, -1, -480);
  CAPTURE(w);

  std::vector<std::byte> blob(64, std::byte{0});
  std::vector<std::byte> scratch(64, std::byte{0xA5});
  std::vector<std::byte> out(64, std::byte{0xA5});

  const auto r = gloam::png::encode(PlateView{blob, w, 4}, scratch, matcher(), out);
  CHECK(r.error == PngError::NonPositiveExtent);
  CHECK(r.bytes == 0);
  CHECK_FALSE(static_cast<bool>(r));
  for (const auto b : out) CHECK(b == std::byte{0xA5});
  for (const auto b : scratch) CHECK(b == std::byte{0xA5});
}

TEST_CASE("a blob shorter than the plate it claims is refused", "[png]") {
  auto blob = make_plate(8, 8, kAllInks, kCheckers);
  blob.pop_back();

  std::vector<std::byte> scratch(gloam::png::scratch_bytes(8, 8), std::byte{0});
  std::vector<std::byte> out(gloam::png::bound(8, 8), std::byte{0xA5});

  const auto r = gloam::png::encode(PlateView{blob, 8, 8}, scratch, matcher(), out);
  CHECK(r.error == PngError::BlobTooSmall);
  CHECK(r.bytes == 0);
  for (const auto b : out) CHECK(b == std::byte{0xA5});
}

TEST_CASE("a scratch span one byte short is refused before anything is written",
          "[png]") {
  const auto blob = make_plate(8, 8, kAllInks, kCheckers);
  std::vector<std::byte> scratch(gloam::png::scratch_bytes(8, 8) - 1, std::byte{0});
  std::vector<std::byte> out(gloam::png::bound(8, 8), std::byte{0xA5});

  const auto r = gloam::png::encode(PlateView{blob, 8, 8}, scratch, matcher(), out);
  CHECK(r.error == PngError::ScratchTooSmall);
  CHECK(r.bytes == 0);
  for (const auto b : out) CHECK(b == std::byte{0xA5});
}

TEST_CASE("an output span one byte short is refused, not truncated", "[png]") {
  // A truncated PNG is not a corrupt file a decoder rejects — the chunks it does
  // have are well-formed, so it is a file that renders half an image.
  const auto blob = make_plate(8, 8, kAllInks, kCheckers);
  std::vector<std::byte> scratch(gloam::png::scratch_bytes(8, 8), std::byte{0});
  std::vector<std::byte> out(gloam::png::bound(8, 8) - 1, std::byte{0xA5});

  const auto r = gloam::png::encode(PlateView{blob, 8, 8}, scratch, matcher(), out);
  CHECK(r.error == PngError::OutputTooSmall);
  CHECK(r.bytes == 0);
  for (const auto b : out) CHECK(b == std::byte{0xA5});
}

TEST_CASE("the sizing helpers are exact, including the odd-width nibble", "[png]") {
  CHECK(gloam::png::scanline_bytes(1) == 2);   // filter byte + one nibble, padded
  CHECK(gloam::png::scanline_bytes(2) == 2);
  CHECK(gloam::png::scanline_bytes(3) == 3);
  CHECK(gloam::png::scanline_bytes(480) == 241);
  CHECK(gloam::png::scratch_bytes(480, 360) == 86'760);

  // The number §11's cold-start row is computed from: six of these, before
  // compression, is 520,560 B of scanline.
  CHECK(gloam::png::scratch_bytes(480, 360) * 6 == 520'560);
}

// ── The envelope ────────────────────────────────────────────────────────────

TEST_CASE("the signature, chunk order and CRCs are what the specification says",
          "[png]") {
  const auto blob = make_plate(16, 4, kAllInks, kCheckers);
  const auto file = encode_ok(PlateView{blob, 16, 4});
  const auto bytes = file.span();

  const std::array<std::uint8_t, 8> signature{0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
  for (std::size_t i = 0; i < signature.size(); ++i) {
    CAPTURE(i);
    CHECK(static_cast<std::uint8_t>(bytes[i]) == signature[i]);
  }

  const auto chunks = parse(bytes);
  REQUIRE(chunks.size() == 5);
  CHECK(chunks[0].type == "IHDR");
  CHECK(chunks[1].type == "PLTE");  // must precede IDAT, per the specification
  CHECK(chunks[2].type == "tRNS");  // must follow PLTE and precede IDAT
  CHECK(chunks[3].type == "IDAT");
  CHECK(chunks[4].type == "IEND");
  CHECK(chunks[4].data.empty());

  // The CRC covers the type as well as the data. Checked against an independent
  // implementation, because a checksum verified with its own code is decoration.
  for (const auto& c : chunks) {
    CAPTURE(c.type);
    CHECK(c.stated_crc == c.computed_crc);
  }

  // And nothing follows IEND: a trailing byte is a chunk header a strict decoder
  // will try to read.
  std::size_t consumed = 8;
  for (const auto& c : chunks) consumed += 12 + c.data.size();
  CHECK(consumed == file.size);
}

TEST_CASE("IHDR describes a 4-bit indexed image with no interlacing", "[png]") {
  const auto blob = make_plate(480, 3, kAllInks, kCheckers);
  const auto file = encode_ok(PlateView{blob, 480, 3});
  const auto chunks = parse(file.span());
  REQUIRE(chunks[0].type == "IHDR");
  const auto& d = chunks[0].data;
  REQUIRE(d.size() == 13);

  CHECK(be32(d, 0) == 480);
  CHECK(be32(d, 4) == 3);
  CHECK(static_cast<int>(d[8]) == 4);   // bit depth: five entries need three bits
  CHECK(static_cast<int>(d[9]) == 3);   // colour type: indexed
  CHECK(static_cast<int>(d[10]) == 0);  // compression method: deflate, the only one
  CHECK(static_cast<int>(d[11]) == 0);  // filter method: the only one
  CHECK(static_cast<int>(d[12]) == 0);  // interlace: seven passes of nothing useful
}

TEST_CASE("PLTE is the five entries palette.hpp names, and tRNS is one byte",
          "[png]") {
  const auto blob = make_plate(4, 2, kAllInks, kCheckers);
  const auto file = encode_ok(PlateView{blob, 4, 2});
  const auto chunks = parse(file.span());

  REQUIRE(chunks[1].data.size() == 15);
  for (int i = 0; i < gloam::palette::kEntryCount; ++i) {
    CAPTURE(i);
    const auto rgb = gloam::palette::entry(i);
    CHECK(static_cast<std::uint8_t>(chunks[1].data[static_cast<std::size_t>(i) * 3]) == rgb.r);
    CHECK(static_cast<std::uint8_t>(chunks[1].data[static_cast<std::size_t>(i) * 3 + 1]) == rgb.g);
    CHECK(static_cast<std::uint8_t>(chunks[1].data[static_cast<std::size_t>(i) * 3 + 2]) == rgb.b);
  }

  // The loop above is pinned against the same function the encoder reads, so it
  // catches a wrong ENTRY. Pinned as literals here as well, because the two are
  // different claims and the second one is what a look decision would break.
  const std::array<std::uint8_t, 15> expected{0, 0, 0, 0, 0, 0, 85, 85, 85, 170, 170, 170, 255, 255, 255};
  for (std::size_t i = 0; i < expected.size(); ++i) {
    CAPTURE(i);
    CHECK(static_cast<std::uint8_t>(chunks[1].data[i]) == expected[i]);
  }

  // AND A GAP, STATED RATHER THAN PAPERED OVER. Neither check above can see the
  // red and blue channels transposed in `png.cpp`'s packing loop — mutation
  // testing confirmed a swap leaves this whole suite green — because §10's ramp
  // is grey and a grey triple is symmetric. Nothing in the tree can distinguish
  // channel order while that is true. The first non-grey palette entry is what
  // makes it testable, and gloam#37 (which owns the palette values) carries the
  // note; whoever lands it must add an asymmetric entry to this case in the same
  // commit, or the ordering ships unverified.

  // tRNS is a PREFIX of the palette, not a sparse map — which is the entire
  // reason palette.hpp puts the transparent entry first.
  REQUIRE(chunks[2].data.size() == 1);
  CHECK(static_cast<int>(chunks[2].data[0]) == 0);

  // The ramp itself, pinned: derived values, but derived once and then fixed.
  CHECK(gloam::palette::grey_of(Ink::Ink0) == 0);
  CHECK(gloam::palette::grey_of(Ink::Ink1) == 85);
  CHECK(gloam::palette::grey_of(Ink::Ink2) == 170);
  CHECK(gloam::palette::grey_of(Ink::Ink3) == 255);
  CHECK(gloam::palette::entry_of(Ink::Ink0) == 1);
  CHECK(gloam::palette::entry_of(Ink::Ink3) == 4);
}

// ── The pixels ──────────────────────────────────────────────────────────────

TEST_CASE("every pixel survives the merge, all four inks and both opacities",
          "[png]") {
  // The case the six real light fields cannot provide: they use Ink0 and the
  // stencil, so without this the high nibble and entries 2-4 are dead code.
  const int w = 9;  // odd, so the padding nibble is exercised too
  const int h = 5;
  const auto blob = make_plate(w, h, kAllInks, kCheckers);
  const PlateView src{blob, w, h};

  const auto file = encode_ok(src);
  const auto chunks = parse(file.span());
  const auto raw = gloam_test::inflate_zlib(idat_of(chunks));
  REQUIRE(raw.error == gloam_test::InflateError::None);
  REQUIRE(raw.bytes.size() == gloam::png::scratch_bytes(w, h));

  const auto row_bytes = gloam::png::scanline_bytes(w);
  for (int y = 0; y < h; ++y) {
    const auto row = static_cast<std::size_t>(y) * row_bytes;
    CAPTURE(y);
    CHECK(static_cast<int>(raw.bytes[row]) == 0);  // filter None on every scanline

    for (int x = 0; x < w; ++x) {
      CAPTURE(x);
      const auto pixel = gloam::plate::read(src, x, y);
      REQUIRE(pixel.error == gloam::plate::PlateError::None);

      const auto byte = static_cast<unsigned>(raw.bytes[row + 1 + static_cast<std::size_t>(x / 2)]);
      const auto nibble = (x % 2 == 0) ? (byte >> 4) & 0xFU : byte & 0xFU;
      const auto expected =
          pixel.opaque ? gloam::palette::entry_of(pixel.ink) : gloam::palette::kTransparentEntry;
      CHECK(nibble == expected);
    }

    // The odd width's final low nibble must be zero: a decoder may ignore it,
    // and the digest below may not.
    CHECK((static_cast<unsigned>(raw.bytes[row + row_bytes - 1]) & 0xFU) == 0U);
  }
}

TEST_CASE("a fully transparent plate encodes as entry zero everywhere", "[png]") {
  // The doused-lamp end of §4.4's light-field range, and the case where a
  // stencil bug reads as a black frame rather than as a missing one.
  const auto blob = make_plate(8, 2, [](int, int) { return Ink::Ink3; },
                               [](int, int) { return false; });
  const auto file = encode_ok(PlateView{blob, 8, 2});
  const auto raw = gloam_test::inflate_zlib(idat_of(parse(file.span())));
  REQUIRE(raw.error == gloam_test::InflateError::None);

  for (std::size_t i = 0; i < raw.bytes.size(); ++i) {
    CAPTURE(i);
    CHECK(static_cast<int>(raw.bytes[i]) == 0);  // filter bytes and pixels alike
  }
}

TEST_CASE("a 1x1 plate is a legal PNG", "[png]") {
  const auto blob = make_plate(1, 1, [](int, int) { return Ink::Ink2; },
                               [](int, int) { return true; });
  const auto file = encode_ok(PlateView{blob, 1, 1});
  const auto chunks = parse(file.span());
  CHECK(chunks.size() == 5);

  const auto raw = gloam_test::inflate_zlib(idat_of(chunks));
  REQUIRE(raw.error == gloam_test::InflateError::None);
  REQUIRE(raw.bytes.size() == 2);
  CHECK(static_cast<int>(raw.bytes[0]) == 0);
  CHECK(static_cast<int>(raw.bytes[1]) == 0x30);  // entry 3 in the high nibble
}

// ── Determinism, last ───────────────────────────────────────────────────────

TEST_CASE("a real light field encodes to one pinned digest", "[png]") {
  // The golden. §10 makes a digest over encoded bytes a build gate, and this is
  // the one that covers everything the pack hash cannot: the palette values, the
  // filter choice, the nibble packing, the compressor's tie-breaks and the zlib
  // header. Two compilers producing two digests here is a bug in one of them.
  //
  // It is deliberately over a REAL light field rather than a synthetic plate.
  // The synthetic cases above cover the states the light fields do not have; this
  // one covers the thing that actually goes on the wire.
  std::vector<std::byte> blob(
      gloam::plate::blob_bytes(gloam::lightfield::kWidthPx, gloam::lightfield::kHeightPx));
  REQUIRE(gloam::lightfield::bake(3, blob));

  const auto file =
      encode_ok(PlateView{blob, gloam::lightfield::kWidthPx, gloam::lightfield::kHeightPx});
  const auto digest = gloam::hash::sha256(file.span());
  const auto hex = gloam::hash::to_hex(digest);
  const std::string_view hex_view{hex.data(), hex.size()};

  INFO("lamp level 3 encodes to " << file.size << " B, sha256 " << hex_view);
  CHECK(hex_view ==
        "8472d860f652c2f4630d7fc738b736d91da0e3385d7b0453bb27016557b4e9bb");
  CHECK(file.size == 2'317);
}

TEST_CASE("the same plate encodes to the same bytes, every time", "[png]") {
  const auto blob = make_plate(32, 8, kAllInks, kCheckers);
  const PlateView src{blob, 32, 8};

  const auto first = encode_ok(src);
  for (int i = 0; i < 4; ++i) {
    CAPTURE(i);
    const auto again = encode_ok(src);
    REQUIRE(again.size == first.size);
    CHECK(std::equal(first.span().begin(), first.span().end(), again.span().begin()));
  }
}
