// SPEC §10, §12 — the pack container, and the hash that gates it.
//
// Two subjects in one file, hash first, following test/07emit/'s precedent:
// testing the pack apart from its digest would be testing a struct serializer,
// and §10's actual requirement — "a mismatched hash refuses to launch rather
// than half-uploading a corrupt plate set" — is a joint property of the two.
//
// The assertions that carry the most weight here are the corruption cases. A
// pack that parses cleanly and is wrong is the failure this format exists to
// make impossible, so every region of the file gets a byte flipped in it: a
// blob, a record, the header, and the PADDING BETWEEN BLOBS. That last one is
// the assertion which says the gaps are hashed rather than merely written.
//
// Failure matrix first, per AGENTS.md. The round trip and the golden header
// prefix are last, and prove the least.

#include <catch2/catch_all.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "gloam/assets.hpp"
#include "gloam/budgets.hpp"
#include "gloam/geometry.hpp"
#include "gloam/lightfield.hpp"
#include "gloam/pack.hpp"
#include "gloam/plate.hpp"
#include "gloam/sha256.hpp"

using namespace gloam;
using gloam::pack::PackError;

namespace {

[[nodiscard]] auto bytes_of(std::string_view s) -> std::span<const std::byte> {
  return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

[[nodiscard]] auto hex_of(std::span<const std::byte> in) -> std::string {
  const auto h = hash::to_hex(hash::sha256(in));
  return std::string(h.data(), h.size());
}

// ── A small synthetic pack, so corruption cases have somewhere to bite ──────

constexpr int kW = 8;  ///< dither-aligned, and small enough to reason about
constexpr int kH = 2;

struct Fixture {
  std::vector<gloam::pack::Record> records;
  std::vector<std::byte> plate_bytes;
  std::vector<std::byte> image;
};

/// Two plates whose blobs do NOT abut: 6 bytes each on a 4-byte alignment
/// leaves a two-byte gap, which is what the padding cases need.
[[nodiscard]] auto make_pack() -> Fixture {
  Fixture f;
  const auto blob_bytes = plate::blob_bytes(kW, kH);
  f.plate_bytes.assign(blob_bytes * 2, std::byte{0});

  for (int i = 0; i < 2; ++i) {
    const auto slot = std::span{f.plate_bytes}.subspan(
        static_cast<std::size_t>(i) * blob_bytes, blob_bytes);
    plate::PlateSpan ps{slot, kW, kH};
    for (int y = 0; y < kH; ++y) {
      for (int x = 0; x < kW; ++x) {
        (void)plate::write(ps, x, y, static_cast<plate::Ink>((x + y + i) % 4), (x + i) % 2 == 0);
      }
    }

    gloam::pack::Record r{};
    r.plate_id = static_cast<std::uint16_t>(i);
    r.role = gloam::pack::Role::LightField;
    r.depth = gloam::pack::kDepthFullFrame;
    r.lateral = gloam::pack::Lateral::FullFrame;
    r.codec = gloam::pack::Codec::RawPlanes;
    r.w = kW;
    r.h = kH;
    f.records.push_back(r);
  }

  std::vector<std::span<const std::byte>> blobs{
      std::span<const std::byte>{f.plate_bytes}.subspan(0, blob_bytes),
      std::span<const std::byte>{f.plate_bytes}.subspan(blob_bytes, blob_bytes)};

  f.image.assign(gloam::pack::image_bytes(f.records), std::byte{0});
  const auto res = gloam::pack::assemble(f.records, blobs, f.image);
  REQUIRE(res);
  REQUIRE(res.bytes == f.image.size());
  return f;
}

[[nodiscard]] constexpr auto record_at(std::uint16_t i) -> std::size_t {
  return gloam::pack::kHeaderBytes + gloam::pack::kRecordBytes * static_cast<std::size_t>(i);
}

auto flip(std::vector<std::byte>& image, std::size_t at) -> void {
  image[at] = static_cast<std::byte>(static_cast<std::uint8_t>(image[at]) ^ 0xFFU);
}

auto poke_u32(std::vector<std::byte>& image, std::size_t at, std::uint32_t v) -> void {
  for (int i = 0; i < 4; ++i) {
    image[at + static_cast<std::size_t>(i)] = static_cast<std::byte>((v >> (i * 8)) & 0xFFU);
  }
}

auto poke_u16(std::vector<std::byte>& image, std::size_t at, std::uint16_t v) -> void {
  image[at + 0] = static_cast<std::byte>(v & 0xFFU);
  image[at + 1] = static_cast<std::byte>((v >> 8) & 0xFFU);
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// Part A — SHA-256 (§10's build gate rests entirely on this)
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("the NIST vectors", "[pack][sha256]") {
  CHECK(hex_of(bytes_of("")) ==
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(hex_of(bytes_of("abc")) ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(hex_of(bytes_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  CHECK(hex_of(bytes_of("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"
                        "ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu")) ==
        "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");

  const std::string million(1'000'000, 'a');
  CHECK(hex_of(bytes_of(million)) ==
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("the digest does not depend on where the updates are split", "[pack][sha256]") {
  // The classic 64-byte-block bug, swept rather than sampled. A pack is hashed
  // in whatever chunks the caller happens to have, so a boundary bug here would
  // surface as a pack that verifies on the machine that baked it and nowhere
  // else.
  std::vector<std::byte> message(1000);
  for (std::size_t i = 0; i < message.size(); ++i) {
    message[i] = static_cast<std::byte>((i * 31 + 7) & 0xFF);
  }
  const auto want = hash::sha256(message);

  for (std::size_t split = 0; split <= message.size(); ++split) {
    hash::Sha256 h;
    h.update(std::span<const std::byte>{message}.subspan(0, split));
    h.update(std::span<const std::byte>{message}.subspan(split));
    INFO("split at " << split);
    REQUIRE(h.finish() == want);
  }
}

TEST_CASE("the padding boundaries", "[pack][sha256]") {
  // 55/56 and 119/120 are where the 64-bit length no longer fits in the final
  // block and the padding spills into one more.
  for (const std::size_t n : {std::size_t{0}, std::size_t{1}, std::size_t{54}, std::size_t{55},
                              std::size_t{56}, std::size_t{63}, std::size_t{64}, std::size_t{65},
                              std::size_t{119}, std::size_t{120}, std::size_t{128}}) {
    const std::string message(n, 'x');
    hash::Sha256 streamed;
    for (std::size_t i = 0; i < n; ++i) streamed.update(bytes_of(std::string_view{&message[i], 1}));
    INFO("length " << n);
    REQUIRE(streamed.finish() == hash::sha256(bytes_of(message)));
  }
}

TEST_CASE("an empty update is a no-op", "[pack][sha256]") {
  hash::Sha256 a;
  a.update(bytes_of("abc"));
  hash::Sha256 b;
  b.update({});
  b.update(bytes_of("ab"));
  b.update({});
  b.update(bytes_of("c"));
  b.update({});
  CHECK(a.finish() == b.finish());
}

TEST_CASE("finish() resets, so a hasher's output cannot depend on its history",
          "[pack][sha256]") {
  hash::Sha256 h;
  h.update(bytes_of("abc"));
  const auto first = h.finish();
  const auto second = h.finish();
  CHECK(first == hash::sha256(bytes_of("abc")));
  CHECK(second == hash::sha256({}));
  CHECK(first != second);
}

TEST_CASE("to_hex is lowercase, 64 characters, and carries no terminator", "[pack][sha256]") {
  const auto h = hash::to_hex(hash::sha256(bytes_of("abc")));
  CHECK(h.size() == 64);
  for (const auto c : h) {
    const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    INFO("character '" << c << "'");
    REQUIRE(ok);
  }
}

// ═══════════════════════════════════════════════════════════════════════════
// Part B — the pack
// ═══════════════════════════════════════════════════════════════════════════

// ── Header parsing ──────────────────────────────────────────────────────────

TEST_CASE("a header shorter than the header is refused", "[pack]") {
  gloam::pack::Header h{};
  CHECK(gloam::pack::read_header({}, h).error == PackError::Truncated);

  auto f = make_pack();
  CHECK(gloam::pack::read_header(std::span<const std::byte>{f.image}.subspan(
                                     0, gloam::pack::kHeaderBytes - 1),
                                 h)
            .error == PackError::Truncated);
}

TEST_CASE("the wrong magic is refused before anything else is trusted", "[pack]") {
  auto f = make_pack();
  f.image[3] = static_cast<std::byte>('X');  // "GLPX"
  gloam::pack::Header h{};
  CHECK(gloam::pack::read_header(f.image, h).error == PackError::BadMagic);
  CHECK(gloam::pack::verify(f.image).error == PackError::BadMagic);
}

TEST_CASE("an unsupported version is refused", "[pack]") {
  for (const std::uint16_t v : {std::uint16_t{0}, std::uint16_t{2}, std::uint16_t{0xFFFF}}) {
    auto f = make_pack();
    poke_u16(f.image, 4, v);
    gloam::pack::Header h{};
    INFO("version " << v);
    CHECK(gloam::pack::read_header(f.image, h).error == PackError::UnsupportedVersion);
  }
}

TEST_CASE("a nonzero reserved field is refused", "[pack]") {
  // Reserved bytes inside the digest's coverage are a place to smuggle a byte
  // past a reader that ignores them, so they are checked rather than skipped.
  auto a = make_pack();
  poke_u16(a.image, 6, 1);
  gloam::pack::Header h{};
  CHECK(gloam::pack::read_header(a.image, h).error == PackError::ReservedNotZero);

  auto b = make_pack();
  poke_u16(b.image, 42, 0x8000);
  CHECK(gloam::pack::read_header(b.image, h).error == PackError::ReservedNotZero);

  auto c = make_pack();
  c.image[record_at(0) + 7] = std::byte{1};
  gloam::pack::Record r{};
  CHECK(gloam::pack::read_record(
            std::span<const std::byte>{c.image}.subspan(record_at(0), gloam::pack::kRecordBytes),
            r)
            .error == PackError::ReservedNotZero);
}

TEST_CASE("an empty pack is a build failure, not an empty level", "[pack]") {
  auto f = make_pack();
  poke_u16(f.image, 40, 0);
  gloam::pack::Header h{};
  CHECK(gloam::pack::read_header(f.image, h).error == PackError::ZeroPlates);

  std::vector<gloam::pack::Record> none;
  std::vector<std::span<const std::byte>> no_blobs;
  std::vector<std::byte> out(64);
  CHECK(gloam::pack::assemble(none, no_blobs, out).error == PackError::ZeroPlates);
}

// ── Record parsing ──────────────────────────────────────────────────────────

TEST_CASE("an unknown enumerator is refused rather than cast", "[pack]") {
  const auto at = record_at(0);
  gloam::pack::Record r{};

  auto role = make_pack();
  role.image[at + 2] = std::byte{gloam::pack::kRoleMax + 1};
  CHECK(gloam::pack::read_record(std::span<const std::byte>{role.image}.subspan(
                                     at, gloam::pack::kRecordBytes),
                                 r)
            .error == PackError::UnknownRole);

  auto lateral = make_pack();
  lateral.image[at + 4] = std::byte{gloam::pack::kLateralMax + 1};
  CHECK(gloam::pack::read_record(std::span<const std::byte>{lateral.image}.subspan(
                                     at, gloam::pack::kRecordBytes),
                                 r)
            .error == PackError::UnknownLateral);

  auto depth = make_pack();
  depth.image[at + 3] = std::byte{geometry::kDepthCount};
  CHECK(gloam::pack::read_record(std::span<const std::byte>{depth.image}.subspan(
                                     at, gloam::pack::kRecordBytes),
                                 r)
            .error == PackError::DepthOutOfRange);

  // 255 is the full-frame sentinel and is legal; kDepthCount - 1 is the far cap.
  auto ok = make_pack();
  ok.image[at + 3] = std::byte{geometry::kDepthCount - 1};
  CHECK(gloam::pack::read_record(
      std::span<const std::byte>{ok.image}.subspan(at, gloam::pack::kRecordBytes), r));
}

TEST_CASE("Png parses and is then refused, which is what makes it a door", "[pack]") {
  // The forward compatibility hatch for termforge #163's f=100 path. A codec
  // this version has never heard of is UnknownCodec; a codec it can name but
  // cannot decode is UnsupportedCodec. Collapsing the two would mean a future
  // pack was indistinguishable from a corrupt one.
  const auto at = record_at(0);
  gloam::pack::Record r{};

  auto png = make_pack();
  png.image[at + 6] = static_cast<std::byte>(gloam::pack::Codec::Png);
  const auto parsed = gloam::pack::read_record(
      std::span<const std::byte>{png.image}.subspan(at, gloam::pack::kRecordBytes), r);
  CHECK(parsed);
  CHECK(r.codec == gloam::pack::Codec::Png);
  CHECK(gloam::pack::verify(png.image).error == PackError::UnsupportedCodec);

  auto future = make_pack();
  future.image[at + 6] = std::byte{gloam::pack::kCodecMax + 1};
  CHECK(gloam::pack::read_record(std::span<const std::byte>{future.image}.subspan(
                                     at, gloam::pack::kRecordBytes),
                                 r)
            .error == PackError::UnknownCodec);
}

// ── verify: the structural failure matrix ───────────────────────────────────

TEST_CASE("total_bytes must equal the bytes actually present", "[pack]") {
  auto f = make_pack();
  CHECK(gloam::pack::verify(std::span<const std::byte>{f.image}.subspan(0, f.image.size() - 1))
            .error == PackError::TotalBytesMismatch);

  auto grown = f.image;
  grown.push_back(std::byte{0});
  CHECK(gloam::pack::verify(grown).error == PackError::TotalBytesMismatch);

  // One byte off in the field rather than in the file.
  auto poked = make_pack();
  poke_u32(poked.image, 44, static_cast<std::uint32_t>(poked.image.size() - 1));
  CHECK(gloam::pack::verify(poked.image).error == PackError::TotalBytesMismatch);
}

TEST_CASE("a blob outside the file is refused", "[pack]") {
  // Aligned, so the alignment check does not fire first and mask this one.
  auto past = make_pack();
  const auto beyond = (static_cast<std::uint32_t>(past.image.size()) + 3U) / 4U * 4U;
  REQUIRE(beyond >= past.image.size());
  poke_u32(past.image, record_at(0) + 12, beyond);
  CHECK(gloam::pack::verify(past.image).error == PackError::BlobOutOfRange);

  // offset + length must not wrap. UINT32_MAX - 2 plus a 6-byte length would
  // land back near zero in 32-bit arithmetic and index a valid blob.
  auto wrap = make_pack();
  poke_u32(wrap.image, record_at(0) + 12, 0xFFFFFFFCU);
  CHECK(gloam::pack::verify(wrap.image).error == PackError::BlobOutOfRange);
}

TEST_CASE("a misaligned blob is refused", "[pack]") {
  auto f = make_pack();
  gloam::pack::Record r{};
  REQUIRE(gloam::pack::read_record(
      std::span<const std::byte>{f.image}.subspan(record_at(0), gloam::pack::kRecordBytes), r));
  poke_u32(f.image, record_at(0) + 12, r.offset + 1);
  CHECK(gloam::pack::verify(f.image).error == PackError::BlobMisaligned);
}

TEST_CASE("blobs may not overlap, by even one byte", "[pack]") {
  auto f = make_pack();
  gloam::pack::Record first{};
  REQUIRE(gloam::pack::read_record(
      std::span<const std::byte>{f.image}.subspan(record_at(0), gloam::pack::kRecordBytes),
      first));
  // Pull the second blob back onto the tail of the first, staying aligned.
  poke_u32(f.image, record_at(1) + 12, first.offset + 4);
  CHECK(gloam::pack::verify(f.image).error == PackError::BlobsOverlap);
}

TEST_CASE("a blob pointing into the manifest is not called an overlap", "[pack]") {
  // Two distinct failures that used to share one name. A blob inside the header
  // or the record table means the writer got the table's size wrong; a blob
  // inside its predecessor means the layout overlaps. Reporting the first as an
  // overlap sends whoever is holding an unloadable pack hunting for a second
  // plate that is not involved — and `verify`'s own contract is that the error
  // names the smallest thing actually wrong.
  auto f = make_pack();
  poke_u32(f.image, record_at(0) + 12, 4);  // aligned, but inside the header
  const auto res = gloam::pack::verify(f.image);
  CHECK(res.error == PackError::BlobInsideManifest);
  CHECK(res.plate_index == 0);

  // Just short of the first legal offset is still the manifest.
  auto edge = make_pack();
  poke_u32(edge.image, record_at(0) + 12, gloam::pack::first_blob_offset(2) - 4);
  CHECK(gloam::pack::verify(edge.image).error == PackError::BlobInsideManifest);
}

TEST_CASE("an alignment gap holding anything is refused", "[pack]") {
  // pack.hpp claims padding "is not a place to hide a byte". Hashing alone only
  // makes that true against CORRUPTION: pack_sha256 sits at offset 8, outside
  // its own coverage, so anything that rewrites the file can recompute a digest
  // over modified padding and the pack verifies clean. This is the check that
  // makes the claim true against a rewrite as well.
  auto f = make_pack();
  gloam::pack::Record first{};
  gloam::pack::Record second{};
  REQUIRE(gloam::pack::read_record(
      std::span<const std::byte>{f.image}.subspan(record_at(0), gloam::pack::kRecordBytes),
      first));
  REQUIRE(gloam::pack::read_record(
      std::span<const std::byte>{f.image}.subspan(record_at(1), gloam::pack::kRecordBytes),
      second));
  const auto gap = first.offset + first.length;
  REQUIRE(gap < second.offset);

  f.image[gap] = std::byte{0x01};
  // Recompute pack_sha256 over the tampered image, exactly as a rewriter would,
  // so the digest cannot be what catches this.
  const auto digest = hash::sha256(
      std::span<const std::byte>{f.image}.subspan(gloam::pack::kDigestCoverageStart));
  for (std::size_t i = 0; i < digest.size(); ++i) {
    f.image[gloam::pack::kDigestOffset + i] = static_cast<std::byte>(digest[i]);
  }

  const auto res = gloam::pack::verify(f.image);
  CHECK(res.error == PackError::PaddingNotZero);
  CHECK(res.plate_index == 1);
}

TEST_CASE("a length that disagrees with the extent is refused", "[pack]") {
  // Under RawPlanes the length is a pure function of w and h, so a mismatch is a
  // record describing a different plate than the one stored — the exact class of
  // silent corruption §10 refuses to launch on.
  auto f = make_pack();
  gloam::pack::Record r{};
  REQUIRE(gloam::pack::read_record(
      std::span<const std::byte>{f.image}.subspan(record_at(0), gloam::pack::kRecordBytes), r));
  poke_u32(f.image, record_at(0) + 16, r.length - 1);
  CHECK(gloam::pack::verify(f.image).error == PackError::BlobLengthWrongForExtent);
}

TEST_CASE("an extent no plate could have is refused", "[pack]") {
  auto zero = make_pack();
  poke_u16(zero.image, record_at(0) + 8, 0);
  CHECK(gloam::pack::verify(zero.image).error == PackError::ExtentInvalid);

  auto zero_h = make_pack();
  poke_u16(zero_h.image, record_at(0) + 10, 0);
  CHECK(gloam::pack::verify(zero_h.image).error == PackError::ExtentInvalid);

  // A width that is not dither-aligned is NOT invalid — see plate.hpp's
  // dither_aligned(). Enforcing §4.3's alignment as a validity rule refused a
  // 24x24 rune glyph and refused to halve the ladder's own 168-wide depth-3
  // ring. What still has to hold is that the length matches the extent, and
  // that is a separate error.
  auto narrow = make_pack();
  poke_u16(narrow.image, record_at(0) + 8, 4);
  CHECK(gloam::pack::verify(narrow.image).error == PackError::BlobLengthWrongForExtent);
}

TEST_CASE("plate ids must strictly increase", "[pack]") {
  auto dup = make_pack();
  poke_u16(dup.image, record_at(1) + 0, 0);
  CHECK(gloam::pack::verify(dup.image).error == PackError::RecordsOutOfOrder);

  auto backwards = make_pack();
  poke_u16(backwards.image, record_at(0) + 0, 9);
  CHECK(gloam::pack::verify(backwards.image).error == PackError::RecordsOutOfOrder);
}

// ── verify: the corruption matrix, region by region ─────────────────────────

TEST_CASE("a flipped byte inside a blob names that plate", "[pack]") {
  auto f = make_pack();
  gloam::pack::Record r{};
  REQUIRE(gloam::pack::read_record(
      std::span<const std::byte>{f.image}.subspan(record_at(1), gloam::pack::kRecordBytes), r));
  flip(f.image, r.offset + 2);

  const auto res = gloam::pack::verify(f.image);
  CHECK(res.error == PackError::PlateDigestMismatch);
  CHECK(res.plate_index == 1);
}

TEST_CASE("a flipped byte in a record trips the pack digest", "[pack]") {
  // A record's own bytes are covered by pack_sha256 and by nothing else, which
  // is why the header digest has to span the record table rather than only the
  // blobs.
  auto f = make_pack();
  f.image[record_at(0) + 5] = std::byte{0x42};  // wall_type: structurally legal
  CHECK(gloam::pack::verify(f.image).error == PackError::PackDigestMismatch);
}

TEST_CASE("a flipped byte in the padding between blobs is caught", "[pack]") {
  // Inter-blob padding is BOTH zero-checked and hashed. This case leaves the
  // digest stale, so either gate could fire; the structural one runs first and
  // names the smaller thing. The companion case above recomputes the digest,
  // which is what proves the zero-check is doing real work rather than riding
  // on the hash.
  auto f = make_pack();
  gloam::pack::Record first{};
  gloam::pack::Record second{};
  REQUIRE(gloam::pack::read_record(
      std::span<const std::byte>{f.image}.subspan(record_at(0), gloam::pack::kRecordBytes),
      first));
  REQUIRE(gloam::pack::read_record(
      std::span<const std::byte>{f.image}.subspan(record_at(1), gloam::pack::kRecordBytes),
      second));

  const auto gap_start = first.offset + first.length;
  REQUIRE(gap_start < second.offset);  // the fixture exists to produce this gap
  CHECK(f.image[gap_start] == std::byte{0});

  flip(f.image, gap_start);
  CHECK(gloam::pack::verify(f.image).error == PackError::PaddingNotZero);
}

TEST_CASE("a flipped byte in the stored digest itself is caught", "[pack]") {
  auto f = make_pack();
  flip(f.image, gloam::pack::kDigestOffset);
  CHECK(gloam::pack::verify(f.image).error == PackError::PackDigestMismatch);
}

// ── assemble ────────────────────────────────────────────────────────────────

TEST_CASE("assemble refuses a record and blob list that disagree", "[pack]") {
  auto f = make_pack();
  const auto blob_bytes = plate::blob_bytes(kW, kH);
  std::vector<std::span<const std::byte>> one{
      std::span<const std::byte>{f.plate_bytes}.subspan(0, blob_bytes)};
  std::vector<std::byte> out(f.image.size());
  CHECK(gloam::pack::assemble(f.records, one, out).error == PackError::BlobCountMismatch);
}

TEST_CASE("assemble refuses a blob whose size disagrees with its record", "[pack]") {
  auto f = make_pack();
  const auto blob_bytes = plate::blob_bytes(kW, kH);
  std::vector<std::span<const std::byte>> blobs{
      std::span<const std::byte>{f.plate_bytes}.subspan(0, blob_bytes - 1),
      std::span<const std::byte>{f.plate_bytes}.subspan(blob_bytes, blob_bytes)};
  std::vector<std::byte> out(f.image.size());
  const auto res = gloam::pack::assemble(f.records, blobs, out);
  CHECK(res.error == PackError::BlobLengthWrongForExtent);
  CHECK(res.plate_index == 0);
}

TEST_CASE("assemble refuses an output buffer one byte short", "[pack]") {
  auto f = make_pack();
  const auto blob_bytes = plate::blob_bytes(kW, kH);
  std::vector<std::span<const std::byte>> blobs{
      std::span<const std::byte>{f.plate_bytes}.subspan(0, blob_bytes),
      std::span<const std::byte>{f.plate_bytes}.subspan(blob_bytes, blob_bytes)};
  std::vector<std::byte> out(f.image.size() - 1);
  CHECK(gloam::pack::assemble(f.records, blobs, out).error == PackError::BufferTooSmall);
}

TEST_CASE("a refused assemble leaves the caller's records untouched", "[pack]") {
  // Half-filled offsets on a rejected call are the sort of thing a caller
  // writes to disk anyway, so every structural check runs before any mutation.
  auto f = make_pack();
  auto records = f.records;
  for (auto& r : records) {
    r.offset = 0;
    r.length = 0;
    r.sha256 = {};
  }
  const auto before = records;

  std::vector<std::span<const std::byte>> one{
      std::span<const std::byte>{f.plate_bytes}.subspan(0, plate::blob_bytes(kW, kH))};
  std::vector<std::byte> out(f.image.size());
  CHECK_FALSE(gloam::pack::assemble(records, one, out));
  CHECK(records == before);
}

TEST_CASE("assemble overwrites a stale caller-supplied digest", "[pack]") {
  // The division of labour that makes two runs byte-identical: the caller owns
  // the descriptive fields, assemble owns offset, length and the digest. A
  // caller-supplied digest that survived would put a lie in the manifest that
  // verify would then blame on the blob.
  auto f = make_pack();
  auto records = f.records;
  for (auto& r : records) {
    r.offset = 0xDEADBEEF;
    r.length = 12345;
    r.sha256.fill(0xAB);
  }
  const auto blob_bytes = plate::blob_bytes(kW, kH);
  std::vector<std::span<const std::byte>> blobs{
      std::span<const std::byte>{f.plate_bytes}.subspan(0, blob_bytes),
      std::span<const std::byte>{f.plate_bytes}.subspan(blob_bytes, blob_bytes)};
  std::vector<std::byte> out(f.image.size());
  REQUIRE(gloam::pack::assemble(records, blobs, out));

  CHECK(records == f.records);
  CHECK(out == f.image);
  CHECK(gloam::pack::verify(out));
}

TEST_CASE("assemble refuses everything verify would refuse", "[pack]") {
  // THE producer/consumer agreement. `assemble` used to check only codec and
  // extent while `read_record`/`verify` also enforced role, depth and lateral —
  // so 12% of randomly-generated records assembled cleanly into a pack that
  // then refused to load. A baker emitting a pack it would itself reject moves
  // a build failure to the player, and it does so silently, because a producer
  // and a consumer that disagree only diverge on inputs neither was tested with.
  const auto blob_bytes = plate::blob_bytes(kW, kH);

  struct Case {
    const char* what;
    PackError want;
    void (*spoil)(gloam::pack::Record&);
  };
  const std::array<Case, 4> cases{{
      {"role", PackError::UnknownRole,
       [](gloam::pack::Record& r) {
         r.role = static_cast<gloam::pack::Role>(gloam::pack::kRoleMax + 1);
       }},
      {"lateral", PackError::UnknownLateral,
       [](gloam::pack::Record& r) {
         r.lateral = static_cast<gloam::pack::Lateral>(gloam::pack::kLateralMax + 1);
       }},
      {"depth", PackError::DepthOutOfRange,
       [](gloam::pack::Record& r) { r.depth = geometry::kDepthCount; }},
      {"codec", PackError::UnknownCodec,
       [](gloam::pack::Record& r) {
         r.codec = static_cast<gloam::pack::Codec>(gloam::pack::kCodecMax + 1);
       }},
  }};

  for (const auto& c : cases) {
    auto f = make_pack();
    auto records = f.records;
    c.spoil(records[1]);
    std::vector<std::span<const std::byte>> blobs{
        std::span<const std::byte>{f.plate_bytes}.subspan(0, blob_bytes),
        std::span<const std::byte>{f.plate_bytes}.subspan(blob_bytes, blob_bytes)};
    std::vector<std::byte> out(f.image.size());

    INFO("spoiled field: " << c.what);
    const auto res = gloam::pack::assemble(records, blobs, out);
    REQUIRE(res.error == c.want);
    REQUIRE(res.plate_index == 1);
  }
}

TEST_CASE("write_record and read_record are inverses over everything assemble emits",
          "[pack]") {
  // The other half of the same property: if assemble only ever writes values
  // read_record accepts, the two are inverses, and a round trip cannot lose a
  // field. Swept over every legal enumerator combination.
  const auto blob_bytes = plate::blob_bytes(kW, kH);
  auto f = make_pack();

  for (std::uint8_t role = 0; role <= gloam::pack::kRoleMax; ++role) {
    for (std::uint8_t lateral = 0; lateral <= gloam::pack::kLateralMax; ++lateral) {
      for (const std::uint8_t depth :
           {std::uint8_t{0}, static_cast<std::uint8_t>(geometry::kDepthCount - 1),
            gloam::pack::kDepthFullFrame}) {
        auto records = f.records;
        for (auto& r : records) {
          r.role = static_cast<gloam::pack::Role>(role);
          r.lateral = static_cast<gloam::pack::Lateral>(lateral);
          r.depth = depth;
          r.wall_type = 3;
        }
        std::vector<std::span<const std::byte>> blobs{
            std::span<const std::byte>{f.plate_bytes}.subspan(0, blob_bytes),
            std::span<const std::byte>{f.plate_bytes}.subspan(blob_bytes, blob_bytes)};
        std::vector<std::byte> out(f.image.size());

        INFO("role " << int{role} << " lateral " << int{lateral} << " depth " << int{depth});
        REQUIRE(gloam::pack::assemble(records, blobs, out));
        REQUIRE(gloam::pack::verify(out));

        gloam::pack::Record back{};
        REQUIRE(gloam::pack::read_record(
            std::span<const std::byte>{out}.subspan(record_at(0), gloam::pack::kRecordBytes),
            back));
        REQUIRE(back == records[0]);
      }
    }
  }
}

TEST_CASE("assemble refuses records that are not in plate_id order", "[pack]") {
  auto f = make_pack();
  auto records = f.records;
  records[1].plate_id = records[0].plate_id;
  const auto blob_bytes = plate::blob_bytes(kW, kH);
  std::vector<std::span<const std::byte>> blobs{
      std::span<const std::byte>{f.plate_bytes}.subspan(0, blob_bytes),
      std::span<const std::byte>{f.plate_bytes}.subspan(blob_bytes, blob_bytes)};
  std::vector<std::byte> out(f.image.size());
  const auto res = gloam::pack::assemble(records, blobs, out);
  CHECK(res.error == PackError::RecordsOutOfOrder);
  CHECK(res.plate_index == 1);
}

// ── Layout: the on-disk contract ────────────────────────────────────────────

TEST_CASE("the format is little-endian, asserted by literal", "[pack]") {
  // SCHEMAS.md never stated an endianness. This is where it is stated, in the
  // only form that cannot drift from the implementation.
  auto f = make_pack();
  auto records = f.records;
  records[0].plate_id = 0x0102;
  records[1].plate_id = 0x0304;
  const auto blob_bytes = plate::blob_bytes(kW, kH);
  std::vector<std::span<const std::byte>> blobs{
      std::span<const std::byte>{f.plate_bytes}.subspan(0, blob_bytes),
      std::span<const std::byte>{f.plate_bytes}.subspan(blob_bytes, blob_bytes)};
  std::vector<std::byte> out(f.image.size());
  REQUIRE(gloam::pack::assemble(records, blobs, out));

  CHECK(out[record_at(0) + 0] == std::byte{0x02});
  CHECK(out[record_at(0) + 1] == std::byte{0x01});
  CHECK(out[record_at(1) + 0] == std::byte{0x04});
  CHECK(out[record_at(1) + 1] == std::byte{0x03});

  // total_bytes, four bytes, low byte first.
  const auto total = static_cast<std::uint32_t>(out.size());
  CHECK(out[44] == static_cast<std::byte>(total & 0xFF));
  CHECK(out[45] == static_cast<std::byte>((total >> 8) & 0xFF));
  CHECK(out[46] == static_cast<std::byte>((total >> 16) & 0xFF));
  CHECK(out[47] == static_cast<std::byte>((total >> 24) & 0xFF));
}

TEST_CASE("every padding byte is written zero", "[pack]") {
  auto f = make_pack();
  CHECK(f.image[6] == std::byte{0});
  CHECK(f.image[7] == std::byte{0});
  CHECK(f.image[42] == std::byte{0});
  CHECK(f.image[43] == std::byte{0});
  CHECK(f.image[record_at(0) + 7] == std::byte{0});
  CHECK(f.image[record_at(1) + 7] == std::byte{0});
}

TEST_CASE("the layout constants are what the records and header occupy", "[pack]") {
  CHECK(gloam::pack::kHeaderBytes == 48);
  CHECK(gloam::pack::kRecordBytes == 52);
  CHECK(gloam::pack::kDigestOffset == 8);
  CHECK(gloam::pack::kDigestCoverageStart == 40);
  CHECK(gloam::pack::first_blob_offset(1) == 100);
  CHECK(gloam::pack::first_blob_offset(2) == 152);
  CHECK(gloam::pack::first_blob_offset(6) == 360);

  CHECK(gloam::pack::align_up(0) == 0);
  CHECK(gloam::pack::align_up(1) == 4);
  CHECK(gloam::pack::align_up(4) == 4);
  CHECK(gloam::pack::align_up(5) == 8);

  // Saturation must not break the postcondition. Returning UINT32_MAX would be
  // 3 mod 4 — a misaligned offset handed back by the one function whose whole
  // job is alignment, which `verify` would then report as BlobMisaligned,
  // pointing the diagnosis at the offset instead of at the overflow.
  for (std::uint32_t v : {UINT32_MAX, UINT32_MAX - 1, UINT32_MAX - 2, UINT32_MAX - 3,
                          UINT32_MAX - 4}) {
    INFO("align_up(" << v << ")");
    const auto aligned = gloam::pack::align_up(v);
    CHECK(aligned % gloam::pack::kBlobAlignment == 0);
    CHECK(aligned >= v - gloam::pack::kBlobAlignment);
  }
}

// ── The real pack: reproducibility, §11, and the round trip ─────────────────

TEST_CASE("the light-field pack is byte-identical across two independent bakes", "[pack]") {
  // §10's acceptance criterion, in process. The out-of-process half — running
  // the real binary twice — is the `pack-reproducible` ctest case, and it
  // catches the uninitialised byte that a comparison sharing one allocator
  // will not.
  // Built through `gloam::assets`, which is the SAME code path `gloam_bake`
  // runs. That matters for the golden digest below: asserting it against a
  // private copy of the assembly would let the test and the binary drift, and
  // only `cmake/check_pack_repro.cmake` would be left — and that compares a run
  // against a run, never against the golden.
  const auto build = []() {
    std::vector<std::byte> pixels(assets::pixel_bytes());
    std::vector<pack::Record> records(assets::kPlateCount);
    std::vector<std::span<const std::byte>> blobs(assets::kPlateCount);
    std::vector<std::byte> image(assets::image_bytes());
    REQUIRE(assets::build_pack(pixels, records, blobs, image));
    return image;
  };

  const auto first = build();
  const auto second = build();
  CHECK(first == second);
  CHECK(hash::sha256(first) == hash::sha256(second));
  CHECK(gloam::pack::verify(first));

  // The size the format arithmetic predicts: 48 + 6 * 52 + 6 * 64800.
  CHECK(first.size() == 389'160);

  // THE GOLDEN DIGEST. Two runs agreeing with each other only proves this
  // machine is consistent with itself; §19 step 5's real requirement is that a
  // pack is the same artifact everywhere. Verified identical under GCC 13
  // (CI's floor), GCC 14 and Clang 20 before being written down here.
  //
  // If this changes, something changed the ART. That is allowed — the falloff
  // band width in lightfield.hpp is explicitly a look decision — but it has to
  // be a deliberate line in a diff rather than a number that drifted.
  CHECK(hex_of(first) == "1f448bbffc7e8274477cdc65b0155b9c5ba3cf54c4e3fd329bdd10cf0f325b1b");

  // §11's residency cap. pack.hpp deliberately does not know about budgets —
  // emit.hpp's rule, "the sink reports, the budget judges" — so the comparison
  // is made here rather than inside the parser.
  gloam::pack::Header h{};
  REQUIRE(gloam::pack::read_header(first, h));
  CHECK(h.plate_count == lightfield::kFieldCount);
  CHECK(h.plate_count <= budget::kMaxResidentImages);

  // A NECESSARY CONDITION, NOT §11's BUDGET. `kMaxColdStartPayloadBytes` is the
  // BASE64 TRANSMIT payload, and the pack is not that: kitty is handed pixels,
  // so a RawPlanes plate expands to RGBA before it goes on the wire. See the
  // note in test/10budgets/ — a pack inside the cap does not mean the cold
  // start is.
  CHECK(first.size() <= budget::kMaxColdStartPayloadBytes);
}

TEST_CASE("every record round trips, and every blob is where it says it is", "[pack]") {
  auto f = make_pack();
  REQUIRE(gloam::pack::verify(f.image));

  gloam::pack::Header h{};
  REQUIRE(gloam::pack::read_header(f.image, h));
  CHECK(h.version == gloam::pack::kVersion);
  CHECK(h.plate_count == f.records.size());
  CHECK(h.total_bytes == f.image.size());

  const auto blob_bytes = plate::blob_bytes(kW, kH);
  for (std::uint16_t i = 0; i < h.plate_count; ++i) {
    gloam::pack::Record r{};
    REQUIRE(gloam::pack::read_record(std::span<const std::byte>{f.image}.subspan(
                                         record_at(i), gloam::pack::kRecordBytes),
                                     r));
    INFO("record " << i);
    CHECK(r == f.records[i]);
    CHECK(r.length == blob_bytes);

    const auto stored = std::span<const std::byte>{f.image}.subspan(r.offset, r.length);
    const auto source =
        std::span<const std::byte>{f.plate_bytes}.subspan(static_cast<std::size_t>(i) * blob_bytes,
                                                          blob_bytes);
    CHECK(std::equal(stored.begin(), stored.end(), source.begin()));
  }
}

TEST_CASE("the golden header prefix", "[pack]") {
  // Last, and it proves the least: a lock on the first bytes any reader sees.
  auto f = make_pack();
  CHECK(f.image[0] == std::byte{'G'});
  CHECK(f.image[1] == std::byte{'L'});
  CHECK(f.image[2] == std::byte{'P'});
  CHECK(f.image[3] == std::byte{'K'});
  CHECK(f.image[4] == std::byte{0x01});
  CHECK(f.image[5] == std::byte{0x00});
}
