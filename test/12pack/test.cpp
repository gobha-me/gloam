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
// A third subject has since joined the two: the audio run of gloam#23, tested
// the same way and hand-built from synthetic blobs — never sfx.cpp or the
// real bake, which would make a format unit depend on a synthesizer.
//
// Failure matrix first, per AGENTS.md. The round trip and the golden header
// prefix are last, and prove the least.

#include <catch2/catch_all.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
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

// ── A small mixed pack: two plates, then two sounds ──
//
// The audio failure matrix's somewhere to bite. The plate half is make_pack()
// byte-for-byte, so a plate failure means the same thing in both fixtures.

// The numbers, named once. Blob A is 300 frames × 2 channels × 2 bytes =
// 1200 B, a multiple of four; blob B is 441 × 1 × 2 = 882 B, which is not —
// the run keeps an inter-blob gap either way, but the asymmetric one is the
// interesting case. Sound id 1 sits behind plate id 1 ON PURPOSE: the audio
// run's id chain is its own, and overlapping the plate run's ids is exactly
// what must NOT be called out of order.
constexpr std::uint16_t kSoundA = 1;
constexpr std::uint16_t kSoundB = 9;
constexpr std::uint16_t kRateA = 22050;
constexpr std::uint8_t kChannelsA = 2;
constexpr std::uint32_t kFramesA = 300;
constexpr std::uint16_t kRateB = 8000;
constexpr std::uint8_t kChannelsB = 1;
constexpr std::uint32_t kFramesB = 441;

struct MixedFixture {
  std::vector<gloam::pack::Record> records;
  std::vector<std::byte> plate_bytes;
  std::vector<std::byte> audio_bytes;  ///< both sound blobs, concatenated
  std::vector<std::byte> image;
};

/// Deterministic arithmetic noise. The format under test cannot tell a sine
/// from a sawtooth, and a test that needed real synthesis would be testing
/// sfx.cpp at the wrong distance.
[[nodiscard]] auto make_audio_blob(std::uint32_t frames, std::uint8_t channels,
                                   std::uint8_t seed) -> std::vector<std::byte> {
  std::vector<std::byte> blob(static_cast<std::size_t>(gloam::pack::audio_blob_bytes(
      frames, channels, gloam::pack::SampleFormat::S16Le)));
  for (std::size_t i = 0; i < blob.size(); ++i) {
    blob[i] = static_cast<std::byte>((i * 37 + seed * 11 + 5) & 0xFF);
  }
  return blob;
}

/// The record→blob pairing, factored out so a test that has reordered records
/// can rebuild the spans to match.
[[nodiscard]] auto blobs_of(const MixedFixture& f) -> std::vector<std::span<const std::byte>> {
  const auto blob_bytes = plate::blob_bytes(kW, kH);
  const auto a_bytes = static_cast<std::size_t>(
      gloam::pack::audio_blob_bytes(kFramesA, kChannelsA, gloam::pack::SampleFormat::S16Le));
  return {std::span<const std::byte>{f.plate_bytes}.subspan(0, blob_bytes),
          std::span<const std::byte>{f.plate_bytes}.subspan(blob_bytes, blob_bytes),
          std::span<const std::byte>{f.audio_bytes}.subspan(0, a_bytes),
          std::span<const std::byte>{f.audio_bytes}.subspan(a_bytes)};
}

[[nodiscard]] auto make_mixed_pack() -> MixedFixture {
  MixedFixture f;
  const auto plates = make_pack();  // the plate run, byte-for-byte the plate fixture's
  f.plate_bytes = plates.plate_bytes;
  f.records = plates.records;

  gloam::pack::Record a{};
  a.plate_id = kSoundA;  // the plate_id field doubles as the sound id
  a.role = gloam::pack::Role::Audio;
  a.sample_rate = kRateA;
  a.channels = kChannelsA;
  a.sample_format = gloam::pack::SampleFormat::S16Le;
  a.frame_count = kFramesA;

  gloam::pack::Record b{};
  b.plate_id = kSoundB;
  b.role = gloam::pack::Role::Audio;
  b.sample_rate = kRateB;
  b.channels = kChannelsB;
  b.sample_format = gloam::pack::SampleFormat::S16Le;
  b.frame_count = kFramesB;

  const auto blob_a = make_audio_blob(kFramesA, kChannelsA, 1);
  const auto blob_b = make_audio_blob(kFramesB, kChannelsB, 2);
  f.audio_bytes.assign(blob_a.begin(), blob_a.end());
  f.audio_bytes.insert(f.audio_bytes.end(), blob_b.begin(), blob_b.end());

  f.records.push_back(a);
  f.records.push_back(b);

  f.image.assign(gloam::pack::image_bytes(f.records), std::byte{0});
  const auto res = gloam::pack::assemble(f.records, blobs_of(f), f.image);
  REQUIRE(res);
  REQUIRE(res.bytes == f.image.size());
  return f;
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
  // Header bytes +42-43 are no longer such a byte — the audio record kind
  // claimed them as audio_count (gloam#23), and that field has its own matrix
  // below. reserved0 and the record's +7 stay reserved, +7 under BOTH readings.
  auto a = make_pack();
  poke_u16(a.image, 6, 1);
  gloam::pack::Header h{};
  CHECK(gloam::pack::read_header(a.image, h).error == PackError::ReservedNotZero);

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
  f.image[record_at(0) + 5] = std::byte{0x42};  // variant: structurally legal
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
  // field. Swept over every legal enumerator combination of the PLATE reading.
  // Role::Audio rides exactly once: it is a different reading of the same
  // bytes — its sample_rate occupies the depth/lateral slots, sweeping those
  // would be sweeping another field's bytes, and an audio-only pack is
  // ZeroPlates — so one legal sound whose extent matches the fixture blob
  // (3 frames × 1 channel × 2 bytes) stands in for it here, and the
  // byte-exact pin for the reading lives in the audio section below.
  static_assert(static_cast<std::uint8_t>(gloam::pack::Role::Audio) ==
                    static_cast<std::uint8_t>(gloam::pack::Role::Rune) + 1,
                "Audio must stay the first non-plate role, or this sweep silently drifts");
  const auto blob_bytes = plate::blob_bytes(kW, kH);
  auto f = make_pack();

  for (std::uint8_t role = 0; role <= gloam::pack::kRoleMax; ++role) {
    const auto is_audio = static_cast<gloam::pack::Role>(role) == gloam::pack::Role::Audio;
    for (std::uint8_t lateral = 0; lateral <= gloam::pack::kLateralMax; ++lateral) {
      for (const std::uint8_t depth :
           {std::uint8_t{0}, static_cast<std::uint8_t>(geometry::kDepthCount - 1),
            gloam::pack::kDepthFullFrame}) {
        if (is_audio && (lateral != 0 || depth != 0)) continue;  // not fields under this reading
        auto records = f.records;
        if (is_audio) {
          auto& a = records[1];  // records[0] stays a plate: the run needs one
          a.role = gloam::pack::Role::Audio;
          // The plate-side fields are never serialized under Audio, so they
          // must sit at their defaults for the round trip to compare equal —
          // see the Record doc in pack.hpp.
          a.w = 0;
          a.h = 0;
          a.sample_rate = 8000;
          a.channels = 1;
          a.sample_format = gloam::pack::SampleFormat::S16Le;
          a.frame_count = 3;
        } else {
          for (auto& r : records) {
            r.role = static_cast<gloam::pack::Role>(role);
            r.lateral = static_cast<gloam::pack::Lateral>(lateral);
            r.depth = depth;
            r.variant = 3;
          }
        }
        std::vector<std::span<const std::byte>> blobs{
            std::span<const std::byte>{f.plate_bytes}.subspan(0, blob_bytes),
            std::span<const std::byte>{f.plate_bytes}.subspan(blob_bytes, blob_bytes)};
        std::vector<std::byte> out(f.image.size());

        INFO("role " << int{role} << " lateral " << int{lateral} << " depth " << int{depth});
        REQUIRE(gloam::pack::assemble(records, blobs, out));
        REQUIRE(gloam::pack::verify(out));

        for (const std::uint16_t index : {0, 1}) {
          gloam::pack::Record back{};
          REQUIRE(gloam::pack::read_record(
              std::span<const std::byte>{out}.subspan(record_at(index),
                                                      gloam::pack::kRecordBytes),
              back));
          REQUIRE(back == records[index]);
        }
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

// ── The audio run (gloam#23, UPSTREAM.md item 10) ──
//
// The record table's second kind: the same 52-byte envelope, the same blob
// rules and the same digest gate, with a different reading of the descriptive
// middle and a two-run ordering rule. Failure matrix first, as everywhere
// else in this file.

TEST_CASE("an unknown sample format is refused rather than cast", "[pack][audio]") {
  // The +6 slot's audio-side enumerator, under the same discipline as codec:
  // a value this version has never heard of is refused by value, so a future
  // format lands as a new enumerator rather than a silent misread.
  auto f = make_mixed_pack();
  f.image[record_at(2) + 6] = std::byte{gloam::pack::kSampleFormatMax + 1};

  gloam::pack::Record r{};
  CHECK(gloam::pack::read_record(
            std::span<const std::byte>{f.image}.subspan(record_at(2), gloam::pack::kRecordBytes),
            r)
            .error == PackError::UnknownSampleFormat);

  const auto res = gloam::pack::verify(f.image);
  CHECK(res.error == PackError::UnknownSampleFormat);
  CHECK(res.plate_index == 2);
}

TEST_CASE("a sound with no rate, no channels or no frames is refused", "[pack][audio]") {
  // Each zero trips its own name. The mutations are minimal — one field,
  // zeroed — so the error that fires is the check that field owns, not a
  // downstream consequence of it (a zero channel count would ALSO make the
  // blob length wrong; the channels check runs first and names it).
  auto rate = make_mixed_pack();
  poke_u16(rate.image, record_at(2) + 3, 0);
  const auto rate_res = gloam::pack::verify(rate.image);
  CHECK(rate_res.error == PackError::ZeroSampleRate);
  CHECK(rate_res.plate_index == 2);

  auto channels = make_mixed_pack();
  channels.image[record_at(2) + 5] = std::byte{0};
  const auto channels_res = gloam::pack::verify(channels.image);
  CHECK(channels_res.error == PackError::ZeroChannels);
  CHECK(channels_res.plate_index == 2);

  auto frames = make_mixed_pack();
  poke_u32(frames.image, record_at(2) + 8, 0);
  const auto frames_res = gloam::pack::verify(frames.image);
  CHECK(frames_res.error == PackError::ZeroFrames);
  CHECK(frames_res.plate_index == 2);
}

TEST_CASE("an audio blob whose length disagrees with its frame count is refused",
          "[pack][audio]") {
  // The audio reading of BlobLengthWrongForExtent: under S16Le the length is a
  // pure function of frames × channels, so a mismatch is a record describing a
  // different sound than the one stored.
  auto f = make_mixed_pack();
  gloam::pack::Record r{};
  REQUIRE(gloam::pack::read_record(
      std::span<const std::byte>{f.image}.subspan(record_at(2), gloam::pack::kRecordBytes), r));
  REQUIRE(r.length == 1200);
  poke_u32(f.image, record_at(2) + 16, r.length - 1);
  const auto res = gloam::pack::verify(f.image);
  CHECK(res.error == PackError::BlobLengthWrongForFrames);
  CHECK(res.plate_index == 2);
}

TEST_CASE("an audio record ahead of the plate run is refused", "[pack][audio]") {
  // The two-run rule: every plate record precedes every audio record. The
  // mutation rewrites the table as [plate, SOUND, plate, sound] using the
  // library's own writer — the moved sound takes over the second plate's blob
  // slot (6 bytes = 3 frames × 1 channel × 2, digest and all), so every check
  // that is not the run order passes, and the error names the run order.
  auto f = make_mixed_pack();
  gloam::pack::Record plate1{};
  REQUIRE(gloam::pack::read_record(
      std::span<const std::byte>{f.image}.subspan(record_at(1), gloam::pack::kRecordBytes),
      plate1));

  gloam::pack::Record moved{};
  moved.plate_id = 7;  // a sound id, on the audio run's own chain
  moved.role = gloam::pack::Role::Audio;
  moved.sample_rate = 8000;
  moved.channels = 1;
  moved.sample_format = gloam::pack::SampleFormat::S16Le;
  moved.frame_count = 3;
  moved.offset = plate1.offset;
  moved.length = plate1.length;
  moved.sha256 = plate1.sha256;  // the digest of the bytes actually there

  REQUIRE(gloam::pack::write_record(
      std::span<std::byte>{f.image}.subspan(record_at(1), gloam::pack::kRecordBytes), moved));
  REQUIRE(gloam::pack::write_record(
      std::span<std::byte>{f.image}.subspan(record_at(2), gloam::pack::kRecordBytes), plate1));

  const auto res = gloam::pack::verify(f.image);
  CHECK(res.error == PackError::AudioBeforePlates);
  CHECK(res.plate_index == 2);
}

TEST_CASE("assemble refuses an audio record ahead of the plate run", "[pack][audio]") {
  // The producer/consumer agreement, two-run edition: what verify refuses,
  // assemble must refuse first — a baker that emitted this order would move a
  // build failure to the player.
  auto f = make_mixed_pack();
  auto records = f.records;             // [plate, plate, sound, sound]
  std::swap(records[1], records[2]);    // [plate, sound, plate, sound]
  auto blobs = blobs_of(f);
  std::swap(blobs[1], blobs[2]);        // each record keeps its own blob
  std::vector<std::byte> out(f.image.size());
  const auto res = gloam::pack::assemble(records, blobs, out);
  CHECK(res.error == PackError::AudioBeforePlates);
  CHECK(res.plate_index == 2);
}

TEST_CASE("sound ids must strictly increase within the audio run", "[pack][audio]") {
  // The plate run's rule, applied to the audio run's own chain. That the chain
  // IS its own is pinned by the fixture itself: sound id 1 sits behind plate
  // id 1 in a pack that verifies.
  auto f = make_mixed_pack();
  poke_u16(f.image, record_at(3) + 0, kSoundA);  // the same sound id twice
  const auto res = gloam::pack::verify(f.image);
  CHECK(res.error == PackError::RecordsOutOfOrder);
  CHECK(res.plate_index == 3);
}

TEST_CASE("a nonzero reserved byte in an audio record is refused", "[pack][audio]") {
  // +7 is reserved under BOTH readings — the one descriptive byte the two
  // readings share — and it stays a place nothing may be smuggled through.
  auto f = make_mixed_pack();
  f.image[record_at(2) + 7] = std::byte{1};

  gloam::pack::Record r{};
  CHECK(gloam::pack::read_record(
            std::span<const std::byte>{f.image}.subspan(record_at(2), gloam::pack::kRecordBytes),
            r)
            .error == PackError::ReservedNotZero);

  const auto res = gloam::pack::verify(f.image);
  CHECK(res.error == PackError::ReservedNotZero);
  CHECK(res.plate_index == 2);
}

TEST_CASE("the header's audio_count must agree with the records present", "[pack][audio]") {
  // The claim against the table. The pokes keep plate_count + audio_count at
  // four, so the table still parses end to end — what breaks is that the roles
  // in it no longer tally with the header's claim. Both directions, because a
  // claim can drift either way.
  auto fewer = make_mixed_pack();
  poke_u16(fewer.image, 40, 3);  // claims three plates…
  poke_u16(fewer.image, 42, 1);  // …and one sound, where two of each sit
  CHECK(gloam::pack::verify(fewer.image).error == PackError::AudioCountMismatch);

  auto more = make_mixed_pack();
  poke_u16(more.image, 40, 1);
  poke_u16(more.image, 42, 3);
  CHECK(gloam::pack::verify(more.image).error == PackError::AudioCountMismatch);
}

TEST_CASE("the plate run's rules still bind in a pack that has an audio run", "[pack][audio]") {
  // Regression guard: growing a second reading must not have softened the
  // first. The same minimal mutations as the plate-only matrix above, applied
  // to the plate run of a mixed pack — plus one flipped audio blob, because
  // the blob checks never knew what a plate was.
  {
    auto f = make_mixed_pack();
    poke_u16(f.image, record_at(0) + 8, 0);  // width 0
    CHECK(gloam::pack::verify(f.image).error == PackError::ExtentInvalid);
  }
  {
    auto f = make_mixed_pack();
    gloam::pack::Record r{};
    REQUIRE(gloam::pack::read_record(
        std::span<const std::byte>{f.image}.subspan(record_at(0), gloam::pack::kRecordBytes), r));
    poke_u32(f.image, record_at(0) + 16, r.length - 1);
    CHECK(gloam::pack::verify(f.image).error == PackError::BlobLengthWrongForExtent);
  }
  {
    auto f = make_mixed_pack();
    poke_u16(f.image, record_at(1) + 0, 0);  // a plate id seen before
    CHECK(gloam::pack::verify(f.image).error == PackError::RecordsOutOfOrder);
  }
  {
    auto f = make_mixed_pack();
    f.image[record_at(0) + 4] = std::byte{gloam::pack::kLateralMax + 1};
    CHECK(gloam::pack::verify(f.image).error == PackError::UnknownLateral);
  }
  {
    auto f = make_mixed_pack();
    gloam::pack::Record r{};
    REQUIRE(gloam::pack::read_record(
        std::span<const std::byte>{f.image}.subspan(record_at(3), gloam::pack::kRecordBytes), r));
    flip(f.image, r.offset + 100);
    const auto res = gloam::pack::verify(f.image);
    CHECK(res.error == PackError::PlateDigestMismatch);
    CHECK(res.plate_index == 3);
  }
}

TEST_CASE("assemble refuses every audio reading verify would refuse", "[pack][audio]") {
  // The producer/consumer agreement extended to the second kind. These
  // mutations are in-memory, so read_record's refusals never get a turn —
  // which is exactly the gap the agreement exists to close.
  struct Case {
    const char* what;
    PackError want;
    void (*spoil)(gloam::pack::Record&);
  };
  const std::array<Case, 4> cases{{
      {"sample rate", PackError::ZeroSampleRate,
       [](gloam::pack::Record& r) { r.sample_rate = 0; }},
      {"channels", PackError::ZeroChannels, [](gloam::pack::Record& r) { r.channels = 0; }},
      {"frame count", PackError::ZeroFrames, [](gloam::pack::Record& r) { r.frame_count = 0; }},
      {"sample format", PackError::UnknownSampleFormat,
       [](gloam::pack::Record& r) {
         r.sample_format =
             static_cast<gloam::pack::SampleFormat>(gloam::pack::kSampleFormatMax + 1);
       }},
  }};

  for (const auto& c : cases) {
    auto f = make_mixed_pack();
    auto records = f.records;
    c.spoil(records[2]);
    std::vector<std::byte> out(f.image.size());
    INFO("spoiled field: " << c.what);
    const auto res = gloam::pack::assemble(records, blobs_of(f), out);
    CHECK(res.error == c.want);
    CHECK(res.plate_index == 2);
  }
}

TEST_CASE("assemble refuses a blob whose size disagrees with an audio record", "[pack][audio]") {
  auto f = make_mixed_pack();
  auto blobs = blobs_of(f);
  blobs[2] = blobs[2].first(blobs[2].size() - 1);
  std::vector<std::byte> out(f.image.size());
  const auto res = gloam::pack::assemble(f.records, blobs, out);
  CHECK(res.error == PackError::BlobLengthWrongForFrames);
  CHECK(res.plate_index == 2);
}

TEST_CASE("an audio record round trips, every field pinned to its own bytes", "[pack][audio]") {
  // The inverse-map check for the second reading. Every byte is distinct (and
  // nonzero wherever the format allows a value at all — S16Le is 0, and +7 is
  // reserved), so a serializer that swapped two fields would move a literal
  // this test is watching.
  gloam::pack::Record in{};
  in.plate_id = 0x0102;  // the sound id under the audio reading
  in.role = gloam::pack::Role::Audio;
  in.sample_rate = 0x0304;
  in.channels = 0x05;
  in.sample_format = gloam::pack::SampleFormat::S16Le;
  in.frame_count = 0x1718191A;
  in.offset = 0x1D1E1F20;
  in.length = 0x21222324;
  for (std::size_t i = 0; i < in.sha256.size(); ++i) {
    in.sha256[i] = static_cast<std::uint8_t>(0x80 + i);
  }

  std::array<std::byte, gloam::pack::kRecordBytes> buf{};
  REQUIRE(gloam::pack::write_record(buf, in));

  CHECK(buf[0] == std::byte{0x02});  // the sound id, little-endian
  CHECK(buf[1] == std::byte{0x01});
  CHECK(buf[2] == std::byte{0x08});  // Role::Audio
  CHECK(buf[3] == std::byte{0x04});  // sample_rate, little-endian
  CHECK(buf[4] == std::byte{0x03});
  CHECK(buf[5] == std::byte{0x05});  // channels
  CHECK(buf[6] == std::byte{0x00});  // S16Le
  CHECK(buf[7] == std::byte{0x00});  // reserved
  CHECK(buf[8] == std::byte{0x1A});  // frame_count, little-endian
  CHECK(buf[9] == std::byte{0x19});
  CHECK(buf[10] == std::byte{0x18});
  CHECK(buf[11] == std::byte{0x17});
  CHECK(buf[12] == std::byte{0x20});  // offset, little-endian
  CHECK(buf[13] == std::byte{0x1F});
  CHECK(buf[14] == std::byte{0x1E});
  CHECK(buf[15] == std::byte{0x1D});
  CHECK(buf[16] == std::byte{0x24});  // length, little-endian
  CHECK(buf[17] == std::byte{0x23});
  CHECK(buf[18] == std::byte{0x22});
  CHECK(buf[19] == std::byte{0x21});
  for (std::size_t i = 0; i < 32; ++i) {
    INFO("digest byte " << i);
    CHECK(buf[20 + i] == static_cast<std::byte>(0x80 + i));
  }

  gloam::pack::Record out{};
  REQUIRE(gloam::pack::read_record(buf, out));
  CHECK(out == in);
}

TEST_CASE("a pack of plates and sounds assembles, verifies and reads back", "[pack][audio]") {
  auto f = make_mixed_pack();
  REQUIRE(gloam::pack::verify(f.image));

  gloam::pack::Header h{};
  REQUIRE(gloam::pack::read_header(f.image, h));
  CHECK(h.version == gloam::pack::kVersion);
  CHECK(h.plate_count == 2);
  CHECK(h.audio_count == 2);
  CHECK(h.total_bytes == f.image.size());

  // The layout, predicted by hand: 48 + 4 × 52 = 256 of manifest, then blobs
  // in record order — 6 + 2 pad + 6 + 2 pad + 1200 + 882 = 2354.
  CHECK(f.image.size() == 2354);
  CHECK(f.image.size() == gloam::pack::image_bytes(f.records));

  const std::array<std::uint32_t, 4> want_offsets{{256, 264, 272, 1472}};
  for (std::uint32_t i = 0; i < 4; ++i) {
    gloam::pack::Record r{};
    REQUIRE(gloam::pack::read_record(
        std::span<const std::byte>{f.image}.subspan(record_at(static_cast<std::uint16_t>(i)),
                                                    gloam::pack::kRecordBytes),
        r));
    INFO("record " << i);
    CHECK(r == f.records[i]);
    CHECK(r.offset == want_offsets[i]);
  }

  const auto a_bytes = static_cast<std::size_t>(
      gloam::pack::audio_blob_bytes(kFramesA, kChannelsA, gloam::pack::SampleFormat::S16Le));
  const auto stored_a = std::span<const std::byte>{f.image}.subspan(272, a_bytes);
  CHECK(std::equal(stored_a.begin(), stored_a.end(), f.audio_bytes.begin()));
  const auto stored_b = std::span<const std::byte>{f.image}.subspan(1472, 882);
  CHECK(std::equal(stored_b.begin(), stored_b.end(), f.audio_bytes.begin() + a_bytes));
}

TEST_CASE("a pack with no audio still reads — every pack baked before this format",
          "[pack][audio]") {
  // The backward-read half of claiming reserved1: audio_count comes back zero,
  // the bytes ARE zero, and nothing else about the pack's shape moved. (The
  // golden digest above pins the same fact against the REAL pack.)
  auto f = make_pack();
  CHECK(f.image[42] == std::byte{0});
  CHECK(f.image[43] == std::byte{0});

  gloam::pack::Header h{};
  REQUIRE(gloam::pack::read_header(f.image, h));
  CHECK(h.audio_count == 0);
  CHECK(h.plate_count == 2);
  CHECK(gloam::pack::verify(f.image));

  // The other direction fails CLOSED and is not testable here: an old reader —
  // any binary still checking reserved1 — refuses a pack with sounds as
  // ReservedNotZero rather than half-loading it. That break is deliberate, and
  // is the reason audio_count was given a claim instead of a version bump.
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
  // +42-43 are audio_count, not padding — the audio section pins them.
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
  CHECK(gloam::pack::first_blob_offset(4) == 256);  // the mixed fixture's manifest end
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

  // The size the format arithmetic predicts: 48 + 65 * 52 + the inventory's
  // blob bytes. gloam#8 grew this from six light fields to the whole M0 plate
  // inventory; the number is pinned against `assets`' own arithmetic rather
  // than a hand-copied constant, and the digest below is the golden.
  CHECK(first.size() == assets::image_bytes());

  // THE GOLDEN DIGEST. Two runs agreeing with each other only proves this
  // machine is consistent with itself; §19 step 5's real requirement is that a
  // pack is the same artifact everywhere. Verified identical under GCC 13
  // (CI's floor), GCC 14 and Clang 20 before being written down here.
  //
  // If this changes, something changed the ART. That is allowed — the falloff
  // band width in lightfield.hpp is explicitly a look decision — but it has to
  // be a deliberate line in a diff rather than a number that drifted.
  CHECK(hex_of(first) == "d9560201da4fa92c5e575a790c232e64f6b4003248fa0a4904feea3651f83051");

  // §11's residency cap. pack.hpp deliberately does not know about budgets —
  // emit.hpp's rule, "the sink reports, the budget judges" — so the comparison
  // is made here rather than inside the parser.
  gloam::pack::Header h{};
  REQUIRE(gloam::pack::read_header(first, h));
  CHECK(h.plate_count == static_cast<std::uint16_t>(assets::kPlateCount));
  CHECK(h.plate_count <= budget::kMaxResidentImages);
  // Every record round-trips to exactly its inventory entry — the manifest is
  // the single source the baker, the compositor and this test all read.
  const auto specs = assets::inventory();
  for (std::uint16_t index = 0; index < h.plate_count; ++index) {
    pack::Record record{};
    REQUIRE(pack::read_record(
        std::span<const std::byte>{first}.subspan(
            pack::kHeaderBytes + pack::kRecordBytes * index, pack::kRecordBytes),
        record));
    CHECK(record.plate_id == index);
    CHECK(record.role == specs[index].role);
    CHECK(record.depth == specs[index].depth);
    CHECK(record.lateral == specs[index].lateral);
    CHECK(record.variant == specs[index].variant);
    CHECK(record.w == static_cast<std::uint16_t>(specs[index].width));
    CHECK(record.h == static_cast<std::uint16_t>(specs[index].height));
  }

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
