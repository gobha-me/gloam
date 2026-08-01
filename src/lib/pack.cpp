#include "gloam/pack.hpp"

#include <algorithm>
#include <limits>

#include "gloam/geometry.hpp"

namespace gloam::pack {
namespace {

// ── Little-endian, one byte at a time ──────────────────────────────────────
//
// Never a memcpy of a compiler struct: ABI padding would land in the digest,
// and the digest is a build gate.

auto put_u8(std::span<std::byte> out, std::size_t at, std::uint8_t v) -> void {
  out[at] = static_cast<std::byte>(v);
}

auto put_u16(std::span<std::byte> out, std::size_t at, std::uint16_t v) -> void {
  out[at + 0] = static_cast<std::byte>(v & 0xFFU);
  out[at + 1] = static_cast<std::byte>((v >> 8) & 0xFFU);
}

auto put_u32(std::span<std::byte> out, std::size_t at, std::uint32_t v) -> void {
  out[at + 0] = static_cast<std::byte>(v & 0xFFU);
  out[at + 1] = static_cast<std::byte>((v >> 8) & 0xFFU);
  out[at + 2] = static_cast<std::byte>((v >> 16) & 0xFFU);
  out[at + 3] = static_cast<std::byte>((v >> 24) & 0xFFU);
}

[[nodiscard]] auto get_u8(std::span<const std::byte> in, std::size_t at) -> std::uint8_t {
  return static_cast<std::uint8_t>(in[at]);
}

[[nodiscard]] auto get_u16(std::span<const std::byte> in, std::size_t at) -> std::uint16_t {
  return static_cast<std::uint16_t>(static_cast<std::uint16_t>(get_u8(in, at)) |
                                    static_cast<std::uint16_t>(get_u8(in, at + 1) << 8));
}

[[nodiscard]] auto get_u32(std::span<const std::byte> in, std::size_t at) -> std::uint32_t {
  return static_cast<std::uint32_t>(get_u8(in, at)) |
         (static_cast<std::uint32_t>(get_u8(in, at + 1)) << 8) |
         (static_cast<std::uint32_t>(get_u8(in, at + 2)) << 16) |
         (static_cast<std::uint32_t>(get_u8(in, at + 3)) << 24);
}

/// Field offsets, named once. A layout typo in two places is a layout typo that
/// round-trips cleanly and corrupts nothing until someone else's reader tries.
namespace hdr {
constexpr std::size_t kMagic = 0;
constexpr std::size_t kVersion = 4;
constexpr std::size_t kReserved0 = 6;
constexpr std::size_t kDigest = kDigestOffset;
constexpr std::size_t kPlateCount = kDigestCoverageStart;
constexpr std::size_t kReserved1 = 42;
constexpr std::size_t kTotalBytes = 44;
}  // namespace hdr

namespace rec {
constexpr std::size_t kPlateId = 0;
constexpr std::size_t kRole = 2;
constexpr std::size_t kDepth = 3;
constexpr std::size_t kLateral = 4;
constexpr std::size_t kWallType = 5;
constexpr std::size_t kCodec = 6;
constexpr std::size_t kReserved = 7;
constexpr std::size_t kWidth = 8;
constexpr std::size_t kHeight = 10;
constexpr std::size_t kOffset = 12;
constexpr std::size_t kLength = 16;
constexpr std::size_t kDigest = 20;
}  // namespace rec

static_assert(rec::kDigest + 32 == kRecordBytes, "the record layout must fill its 52 bytes");
static_assert(hdr::kTotalBytes + 4 == kHeaderBytes, "the header layout must fill its 48 bytes");

/// Every structural rule a record must satisfy on its own, before anything is
/// known about where its blob lands.
///
/// ONE COPY, CALLED BY BOTH `assemble` AND `verify`. They have to agree about
/// what a legal record is, or the baker emits a pack it would itself reject —
/// and it would do so silently, because a producer and a consumer that disagree
/// only diverge on the inputs neither was tested with. `blob_length` is the
/// caller's because `assemble` knows it from the blob and `verify` reads it from
/// the record; everything else is identical.
///
/// The enumerator checks mirror `read_record`'s, which is what makes
/// `write_record` and `read_record` inverses over every value `assemble` can
/// produce.
[[nodiscard]] auto validate_record(const Record& r, std::size_t blob_length) -> PackError {
  if (static_cast<std::uint8_t>(r.role) > kRoleMax) return PackError::UnknownRole;
  if (static_cast<std::uint8_t>(r.lateral) > kLateralMax) return PackError::UnknownLateral;
  if (r.depth != kDepthFullFrame && r.depth >= geometry::kDepthCount) {
    return PackError::DepthOutOfRange;
  }
  if (static_cast<std::uint8_t>(r.codec) > kCodecMax) return PackError::UnknownCodec;
  if (r.codec != Codec::RawPlanes) return PackError::UnsupportedCodec;
  if (plate::validate(r.w, r.h, plate::blob_bytes(r.w, r.h)) != plate::PlateError::None) {
    return PackError::ExtentInvalid;
  }
  if (blob_length != plate::blob_bytes(r.w, r.h)) return PackError::BlobLengthWrongForExtent;
  return PackError::None;
}

}  // namespace

auto image_bytes(std::span<const Record> records) -> std::size_t {
  // Zero for anything a pack cannot express, rather than a truncated count: a
  // caller sizing a buffer from this must get an obviously-wrong answer, not a
  // plausible one that is short by 65536 records' worth of blobs.
  if (records.empty()) return 0;
  if (records.size() > std::numeric_limits<std::uint16_t>::max()) return 0;
  auto cursor = static_cast<std::uint64_t>(
      first_blob_offset(static_cast<std::uint16_t>(records.size())));
  std::uint64_t end = cursor;
  for (const auto& r : records) {
    cursor = (cursor + kBlobAlignment - 1) / kBlobAlignment * kBlobAlignment;
    end = cursor + plate::blob_bytes(r.w, r.h);
    cursor = end;
  }
  return static_cast<std::size_t>(end);
}

auto write_header(std::span<std::byte> out, const Header& header) -> PackResult {
  if (out.size() < kHeaderBytes) return {PackError::BufferTooSmall, 0, 0};
  if (header.plate_count == 0) return {PackError::ZeroPlates, 0, 0};

  for (std::size_t i = 0; i < kMagic.size(); ++i) {
    put_u8(out, hdr::kMagic + i, static_cast<std::uint8_t>(kMagic[i]));
  }
  put_u16(out, hdr::kVersion, header.version);
  put_u16(out, hdr::kReserved0, 0);
  for (std::size_t i = 0; i < header.pack_sha256.size(); ++i) {
    put_u8(out, hdr::kDigest + i, header.pack_sha256[i]);
  }
  put_u16(out, hdr::kPlateCount, header.plate_count);
  put_u16(out, hdr::kReserved1, 0);
  put_u32(out, hdr::kTotalBytes, header.total_bytes);

  return {PackError::None, kHeaderBytes, 0};
}

auto write_record(std::span<std::byte> out, const Record& record) -> PackResult {
  if (out.size() < kRecordBytes) return {PackError::BufferTooSmall, 0, 0};

  put_u16(out, rec::kPlateId, record.plate_id);
  put_u8(out, rec::kRole, static_cast<std::uint8_t>(record.role));
  put_u8(out, rec::kDepth, record.depth);
  put_u8(out, rec::kLateral, static_cast<std::uint8_t>(record.lateral));
  put_u8(out, rec::kWallType, record.wall_type);
  put_u8(out, rec::kCodec, static_cast<std::uint8_t>(record.codec));
  put_u8(out, rec::kReserved, 0);
  put_u16(out, rec::kWidth, record.w);
  put_u16(out, rec::kHeight, record.h);
  put_u32(out, rec::kOffset, record.offset);
  put_u32(out, rec::kLength, record.length);
  for (std::size_t i = 0; i < record.sha256.size(); ++i) {
    put_u8(out, rec::kDigest + i, record.sha256[i]);
  }

  return {PackError::None, kRecordBytes, 0};
}

auto read_header(std::span<const std::byte> in, Header& out) -> PackResult {
  if (in.size() < kHeaderBytes) return {PackError::Truncated, 0, 0};

  for (std::size_t i = 0; i < kMagic.size(); ++i) {
    if (get_u8(in, hdr::kMagic + i) != static_cast<std::uint8_t>(kMagic[i])) {
      return {PackError::BadMagic, 0, 0};
    }
  }

  Header h{};
  h.version = get_u16(in, hdr::kVersion);
  if (h.version != kVersion) return {PackError::UnsupportedVersion, 0, 0};
  if (get_u16(in, hdr::kReserved0) != 0) return {PackError::ReservedNotZero, 0, 0};
  if (get_u16(in, hdr::kReserved1) != 0) return {PackError::ReservedNotZero, 0, 0};

  for (std::size_t i = 0; i < h.pack_sha256.size(); ++i) {
    h.pack_sha256[i] = get_u8(in, hdr::kDigest + i);
  }
  h.plate_count = get_u16(in, hdr::kPlateCount);
  if (h.plate_count == 0) return {PackError::ZeroPlates, 0, 0};
  h.total_bytes = get_u32(in, hdr::kTotalBytes);

  out = h;
  return {PackError::None, kHeaderBytes, 0};
}

auto read_record(std::span<const std::byte> in, Record& out) -> PackResult {
  if (in.size() < kRecordBytes) return {PackError::Truncated, 0, 0};

  Record r{};
  r.plate_id = get_u16(in, rec::kPlateId);

  const auto role = get_u8(in, rec::kRole);
  if (role > kRoleMax) return {PackError::UnknownRole, 0, 0};
  r.role = static_cast<Role>(role);

  r.depth = get_u8(in, rec::kDepth);
  if (r.depth != kDepthFullFrame && r.depth >= geometry::kDepthCount) {
    return {PackError::DepthOutOfRange, 0, 0};
  }

  const auto lateral = get_u8(in, rec::kLateral);
  if (lateral > kLateralMax) return {PackError::UnknownLateral, 0, 0};
  r.lateral = static_cast<Lateral>(lateral);

  r.wall_type = get_u8(in, rec::kWallType);

  const auto codec = get_u8(in, rec::kCodec);
  if (codec > kCodecMax) return {PackError::UnknownCodec, 0, 0};
  r.codec = static_cast<Codec>(codec);

  if (get_u8(in, rec::kReserved) != 0) return {PackError::ReservedNotZero, 0, 0};

  r.w = get_u16(in, rec::kWidth);
  r.h = get_u16(in, rec::kHeight);
  r.offset = get_u32(in, rec::kOffset);
  r.length = get_u32(in, rec::kLength);
  for (std::size_t i = 0; i < r.sha256.size(); ++i) {
    r.sha256[i] = get_u8(in, rec::kDigest + i);
  }

  out = r;
  return {PackError::None, kRecordBytes, 0};
}

auto assemble(std::span<Record> records, std::span<const std::span<const std::byte>> blobs,
              std::span<std::byte> out) -> PackResult {
  if (records.size() != blobs.size()) return {PackError::BlobCountMismatch, 0, 0};
  if (records.empty()) return {PackError::ZeroPlates, 0, 0};
  if (records.size() > std::numeric_limits<std::uint16_t>::max()) {
    return {PackError::TooManyPlates, 0, 0};
  }

  const auto count = static_cast<std::uint16_t>(records.size());

  // Structure and extents first, so a refusal leaves `records` exactly as the
  // caller passed it. Half-filled offsets on a rejected call would be the sort
  // of thing a caller writes to disk anyway.
  for (std::uint16_t i = 0; i < count; ++i) {
    const auto& r = records[i];
    if (const auto err = validate_record(r, blobs[i].size()); err != PackError::None) {
      return {err, 0, i};
    }
    if (i > 0 && r.plate_id <= records[i - 1].plate_id) {
      return {PackError::RecordsOutOfOrder, 0, i};
    }
  }

  const auto total = image_bytes(records);
  if (total > std::numeric_limits<std::uint32_t>::max()) return {PackError::BlobOutOfRange, 0, 0};
  if (out.size() < total) return {PackError::BufferTooSmall, 0, 0};

  // Zero everything first: the inter-blob padding is hashed, so it has to be a
  // known value rather than whatever the caller's buffer happened to hold.
  std::fill_n(out.begin(), total, std::byte{0});

  auto cursor = first_blob_offset(count);
  for (std::uint16_t i = 0; i < count; ++i) {
    auto& r = records[i];
    cursor = align_up(cursor);
    r.offset = cursor;
    r.length = static_cast<std::uint32_t>(blobs[i].size());
    r.sha256 = hash::sha256(blobs[i]);
    std::copy(blobs[i].begin(), blobs[i].end(), out.begin() + static_cast<std::ptrdiff_t>(cursor));
    cursor += r.length;
  }

  Header header{};
  header.version = kVersion;
  header.plate_count = count;
  header.total_bytes = static_cast<std::uint32_t>(total);
  if (const auto res = write_header(out, header); !res) return res;

  for (std::uint16_t i = 0; i < count; ++i) {
    const auto at = kHeaderBytes + kRecordBytes * static_cast<std::size_t>(i);
    if (const auto res = write_record(out.subspan(at, kRecordBytes), records[i]); !res) {
      return {res.error, 0, i};
    }
  }

  // Last, over everything the digest covers — which now includes the records
  // just written and every padding byte between blobs.
  const auto digest = hash::sha256(out.subspan(kDigestCoverageStart, total - kDigestCoverageStart));
  for (std::size_t i = 0; i < digest.size(); ++i) {
    put_u8(out, kDigestOffset + i, digest[i]);
  }

  return {PackError::None, total, 0};
}

auto verify(std::span<const std::byte> image) -> PackResult {
  Header header{};
  if (const auto res = read_header(image, header); !res) return res;

  if (header.total_bytes != image.size()) return {PackError::TotalBytesMismatch, 0, 0};

  const auto records_end =
      static_cast<std::uint64_t>(kHeaderBytes) +
      static_cast<std::uint64_t>(kRecordBytes) * static_cast<std::uint64_t>(header.plate_count);
  if (records_end > image.size()) return {PackError::Truncated, 0, 0};

  const auto blobs_begin = first_blob_offset(header.plate_count);
  std::uint64_t previous_end = blobs_begin;
  std::uint32_t previous_id = 0;

  for (std::uint16_t i = 0; i < header.plate_count; ++i) {
    const auto at = kHeaderBytes + kRecordBytes * static_cast<std::size_t>(i);
    Record r{};
    if (const auto res = read_record(image.subspan(at, kRecordBytes), r); !res) {
      return {res.error, 0, i};
    }

    if (i > 0 && r.plate_id <= previous_id) return {PackError::RecordsOutOfOrder, 0, i};
    previous_id = r.plate_id;

    if (const auto err = validate_record(r, r.length); err != PackError::None) {
      return {err, 0, i};
    }

    if (r.offset % kBlobAlignment != 0) return {PackError::BlobMisaligned, 0, i};
    const auto end = static_cast<std::uint64_t>(r.offset) + static_cast<std::uint64_t>(r.length);
    if (end > header.total_bytes) return {PackError::BlobOutOfRange, 0, i};

    // Two distinct failures, because they mean different things to whoever is
    // holding a pack that will not load: a blob pointing into the manifest is a
    // writer that got the record table's size wrong, while a blob pointing into
    // its predecessor is a layout that overlaps. Reporting the first as an
    // overlap sends the reader hunting for a second plate that is not involved.
    if (r.offset < blobs_begin) return {PackError::BlobInsideManifest, 0, i};
    if (r.offset < previous_end) return {PackError::BlobsOverlap, 0, i};

    // The alignment gap ahead of this blob. pack.hpp claims padding "is not a
    // place to hide a byte"; hashing alone only makes that true against
    // corruption, since anything that rewrites the file can recompute a digest
    // that sits outside its own coverage. Checking it is what makes the claim
    // true against a rewrite as well.
    for (auto gap = previous_end; gap < r.offset; ++gap) {
      if (image[gap] != std::byte{0}) return {PackError::PaddingNotZero, 0, i};
    }
    previous_end = end;

    const auto blob = image.subspan(r.offset, r.length);
    if (hash::sha256(blob) != r.sha256) return {PackError::PlateDigestMismatch, 0, i};
  }

  const auto digest =
      hash::sha256(image.subspan(kDigestCoverageStart, image.size() - kDigestCoverageStart));
  if (digest != header.pack_sha256) return {PackError::PackDigestMismatch, 0, 0};

  return {PackError::None, image.size(), 0};
}

}  // namespace gloam::pack
