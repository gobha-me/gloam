#include "gloam/pack.hpp"

#include <algorithm>
#include <limits>

#include "bytes.hpp"
#include "gloam/geometry.hpp"

namespace gloam::pack {
namespace {

// Little-endian, one byte at a time — see src/lib/bytes.hpp for why. These were
// this file's own until `replay.cpp` needed the same six plus a u64 version;
// two copies of a format writer's primitives is one copy too many.
using namespace le;  // NOLINT(google-build-using-namespace)

/// Field offsets, named once. A layout typo in two places is a layout typo that
/// round-trips cleanly and corrupts nothing until someone else's reader tries.
namespace hdr {
constexpr std::size_t kMagic = 0;
constexpr std::size_t kVersion = 4;
constexpr std::size_t kReserved0 = 6;
constexpr std::size_t kDigest = kDigestOffset;
constexpr std::size_t kPlateCount = kDigestCoverageStart;
constexpr std::size_t kAudioCount = 42;  // the snapshot's reserved1, claimed (gloam#23)
constexpr std::size_t kTotalBytes = 44;
}  // namespace hdr

namespace rec {
constexpr std::size_t kPlateId = 0;  // doubles as the sound id under Role::Audio
constexpr std::size_t kRole = 2;
constexpr std::size_t kDepth = 3;
constexpr std::size_t kLateral = 4;
constexpr std::size_t kVariant = 5;
constexpr std::size_t kCodec = 6;
constexpr std::size_t kReserved = 7;
constexpr std::size_t kWidth = 8;
constexpr std::size_t kHeight = 10;
constexpr std::size_t kOffset = 12;
constexpr std::size_t kLength = 16;
constexpr std::size_t kDigest = 20;

// The audio reading of the same 52 bytes (pack.hpp's `Record` doc has the full
// byte map): the descriptive middle means something else, the rest is shared.
constexpr std::size_t kSampleRate = kDepth;       // u16, spanning depth + lateral
constexpr std::size_t kChannels = kVariant;       // u8
constexpr std::size_t kSampleFormat = kCodec;     // u8, the audio-side discriminant slot
constexpr std::size_t kFrameCount = kWidth;       // u32, spanning w + h
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
  if (r.role == Role::Audio) {
    // NONE of the plate rules below apply to a sound: its bytes at +3..+11 are
    // a sample rate, a channel count and a frame count, not a depth, a
    // lateral and an extent. The audio rules are the same shape, though —
    // descriptive fields first, the length's agreement with them last.
    if (r.sample_rate == 0) return PackError::ZeroSampleRate;
    if (r.channels == 0) return PackError::ZeroChannels;
    if (static_cast<std::uint8_t>(r.sample_format) > kSampleFormatMax) {
      return PackError::UnknownSampleFormat;
    }
    if (r.frame_count == 0) return PackError::ZeroFrames;
    if (blob_length != audio_blob_bytes(r.frame_count, r.channels, r.sample_format)) {
      return PackError::BlobLengthWrongForFrames;
    }
    return PackError::None;
  }
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

/// The blob length a record's own extent predicts, whichever reading it has.
[[nodiscard]] auto extent_bytes(const Record& r) -> std::uint64_t {
  if (r.role == Role::Audio) return audio_blob_bytes(r.frame_count, r.channels, r.sample_format);
  return plate::blob_bytes(r.w, r.h);
}

}  // namespace

auto image_bytes(std::span<const Record> records) -> std::size_t {
  // Zero for anything a pack cannot express, rather than a truncated count: a
  // caller sizing a buffer from this must get an obviously-wrong answer, not a
  // plausible one that is short by 65536 records' worth of blobs. What the
  // header expresses is one u16 per KIND, so each kind is counted separately.
  if (records.empty()) return 0;
  const auto max = std::numeric_limits<std::uint16_t>::max();
  std::uint64_t plates = 0;
  std::uint64_t sounds = 0;
  for (const auto& r : records) {
    (r.role == Role::Audio ? sounds : plates) += 1;
  }
  if (plates > max || sounds > max) return 0;
  auto cursor = static_cast<std::uint64_t>(
      first_blob_offset(static_cast<std::uint32_t>(records.size())));
  std::uint64_t end = cursor;
  for (const auto& r : records) {
    cursor = (cursor + kBlobAlignment - 1) / kBlobAlignment * kBlobAlignment;
    end = cursor + extent_bytes(r);
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
  put_digest(out, hdr::kDigest, header.pack_sha256);
  put_u16(out, hdr::kPlateCount, header.plate_count);
  put_u16(out, hdr::kAudioCount, header.audio_count);
  put_u32(out, hdr::kTotalBytes, header.total_bytes);

  return {PackError::None, kHeaderBytes, 0};
}

auto write_record(std::span<std::byte> out, const Record& record) -> PackResult {
  if (out.size() < kRecordBytes) return {PackError::BufferTooSmall, 0, 0};

  put_u16(out, rec::kPlateId, record.plate_id);  // the sound id under Role::Audio
  put_u8(out, rec::kRole, static_cast<std::uint8_t>(record.role));
  if (record.role == Role::Audio) {
    // The audio reading. The plate-side fields are NOT written zero here —
    // they are not written at all, because the bytes they would occupy ARE the
    // audio fields. Only the other reading's field set is ever serialized.
    put_u16(out, rec::kSampleRate, record.sample_rate);
    put_u8(out, rec::kChannels, record.channels);
    put_u8(out, rec::kSampleFormat, static_cast<std::uint8_t>(record.sample_format));
    put_u8(out, rec::kReserved, 0);
    put_u32(out, rec::kFrameCount, record.frame_count);
  } else {
    put_u8(out, rec::kDepth, record.depth);
    put_u8(out, rec::kLateral, static_cast<std::uint8_t>(record.lateral));
    put_u8(out, rec::kVariant, record.variant);
    put_u8(out, rec::kCodec, static_cast<std::uint8_t>(record.codec));
    put_u8(out, rec::kReserved, 0);
    put_u16(out, rec::kWidth, record.w);
    put_u16(out, rec::kHeight, record.h);
  }
  put_u32(out, rec::kOffset, record.offset);
  put_u32(out, rec::kLength, record.length);
  put_digest(out, rec::kDigest, record.sha256);

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

  h.pack_sha256 = get_digest(in, hdr::kDigest);
  h.plate_count = get_u16(in, hdr::kPlateCount);
  if (h.plate_count == 0) return {PackError::ZeroPlates, 0, 0};
  h.audio_count = get_u16(in, hdr::kAudioCount);
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

  if (r.role == Role::Audio) {
    // The audio reading. None of the plate checks apply — depth range,
    // lateral, codec all describe bytes that here hold a sample rate, a
    // channel count and a frame count. The enumerator check on the +6 slot is
    // the one plate habit this reading keeps, against its own enum.
    r.sample_rate = get_u16(in, rec::kSampleRate);
    r.channels = get_u8(in, rec::kChannels);

    const auto format = get_u8(in, rec::kSampleFormat);
    if (format > kSampleFormatMax) return {PackError::UnknownSampleFormat, 0, 0};
    r.sample_format = static_cast<SampleFormat>(format);

    if (get_u8(in, rec::kReserved) != 0) return {PackError::ReservedNotZero, 0, 0};

    r.frame_count = get_u32(in, rec::kFrameCount);
    r.offset = get_u32(in, rec::kOffset);
    r.length = get_u32(in, rec::kLength);
    r.sha256 = get_digest(in, rec::kDigest);

    out = r;
    return {PackError::None, kRecordBytes, 0};
  }

  r.depth = get_u8(in, rec::kDepth);
  if (r.depth != kDepthFullFrame && r.depth >= geometry::kDepthCount) {
    return {PackError::DepthOutOfRange, 0, 0};
  }

  const auto lateral = get_u8(in, rec::kLateral);
  if (lateral > kLateralMax) return {PackError::UnknownLateral, 0, 0};
  r.lateral = static_cast<Lateral>(lateral);

  r.variant = get_u8(in, rec::kVariant);

  const auto codec = get_u8(in, rec::kCodec);
  if (codec > kCodecMax) return {PackError::UnknownCodec, 0, 0};
  r.codec = static_cast<Codec>(codec);

  if (get_u8(in, rec::kReserved) != 0) return {PackError::ReservedNotZero, 0, 0};

  r.w = get_u16(in, rec::kWidth);
  r.h = get_u16(in, rec::kHeight);
  r.offset = get_u32(in, rec::kOffset);
  r.length = get_u32(in, rec::kLength);
  r.sha256 = get_digest(in, rec::kDigest);

  out = r;
  return {PackError::None, kRecordBytes, 0};
}

auto assemble(std::span<Record> records, std::span<const std::span<const std::byte>> blobs,
              std::span<std::byte> out) -> PackResult {
  if (records.size() != blobs.size()) return {PackError::BlobCountMismatch, 0, 0};
  if (records.empty()) return {PackError::ZeroPlates, 0, 0};

  // The header counts are derived from the records' roles, never supplied —
  // the caller cannot state them wrong because the caller never states them.
  // An audio-only pack is refused the way read_header refuses one: plate_count
  // is still the pack's reason to exist, and 0 there is still ZeroPlates.
  std::uint32_t plate_count = 0;
  std::uint32_t audio_count = 0;
  for (const auto& r : records) {
    (r.role == Role::Audio ? audio_count : plate_count) += 1;
  }
  if (plate_count == 0) return {PackError::ZeroPlates, 0, 0};
  if (plate_count > std::numeric_limits<std::uint16_t>::max() ||
      audio_count > std::numeric_limits<std::uint16_t>::max()) {
    return {PackError::TooManyPlates, 0, 0};
  }

  const auto count = static_cast<std::uint32_t>(records.size());

  // Structure and extents first, so a refusal leaves `records` exactly as the
  // caller passed it. Half-filled offsets on a rejected call would be the sort
  // of thing a caller writes to disk anyway.
  //
  // THE TWO-RUN ORDERING: every plate record precedes every audio record, and
  // ids strictly increase WITHIN each kind's run — the audio run's id chain is
  // its own and may restart where the plate run's ended. verify() enforces the
  // same rule, and the two must agree, or the baker emits packs it would
  // itself reject.
  bool audio_run = false;
  bool run_open = false;  // the current run holds at least one record
  std::uint16_t previous_id = 0;
  for (std::uint32_t i = 0; i < count; ++i) {
    const auto& r = records[i];
    if (const auto err = validate_record(r, blobs[i].size()); err != PackError::None) {
      return {err, 0, static_cast<std::uint16_t>(i)};
    }
    if (r.role == Role::Audio) {
      if (!audio_run) {
        audio_run = true;
        run_open = false;  // the audio id chain starts here
      }
    } else if (audio_run) {
      return {PackError::AudioBeforePlates, 0, static_cast<std::uint16_t>(i)};
    }
    if (run_open && r.plate_id <= previous_id) {
      return {PackError::RecordsOutOfOrder, 0, static_cast<std::uint16_t>(i)};
    }
    previous_id = r.plate_id;
    run_open = true;
  }

  const auto total = image_bytes(records);
  if (total > std::numeric_limits<std::uint32_t>::max()) return {PackError::BlobOutOfRange, 0, 0};
  if (out.size() < total) return {PackError::BufferTooSmall, 0, 0};

  // Zero everything first: the inter-blob padding is hashed, so it has to be a
  // known value rather than whatever the caller's buffer happened to hold.
  std::fill_n(out.begin(), total, std::byte{0});

  auto cursor = first_blob_offset(count);
  for (std::uint32_t i = 0; i < count; ++i) {
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
  header.plate_count = static_cast<std::uint16_t>(plate_count);
  header.audio_count = static_cast<std::uint16_t>(audio_count);
  header.total_bytes = static_cast<std::uint32_t>(total);
  if (const auto res = write_header(out, header); !res) return res;

  for (std::uint32_t i = 0; i < count; ++i) {
    const auto at = kHeaderBytes + kRecordBytes * static_cast<std::size_t>(i);
    if (const auto res = write_record(out.subspan(at, kRecordBytes), records[i]); !res) {
      return {res.error, 0, static_cast<std::uint16_t>(i)};
    }
  }

  // Last, over everything the digest covers — which now includes the records
  // just written and every padding byte between blobs.
  const auto digest = hash::sha256(out.subspan(kDigestCoverageStart, total - kDigestCoverageStart));
  put_digest(out, kDigestOffset, digest);

  return {PackError::None, total, 0};
}

auto verify(std::span<const std::byte> image) -> PackResult {
  Header header{};
  if (const auto res = read_header(image, header); !res) return res;

  if (header.total_bytes != image.size()) return {PackError::TotalBytesMismatch, 0, 0};

  const auto record_count = static_cast<std::uint32_t>(header.plate_count) +
                            static_cast<std::uint32_t>(header.audio_count);
  const auto records_end =
      static_cast<std::uint64_t>(kHeaderBytes) +
      static_cast<std::uint64_t>(kRecordBytes) * static_cast<std::uint64_t>(record_count);
  if (records_end > image.size()) return {PackError::Truncated, 0, 0};

  const auto blobs_begin = first_blob_offset(record_count);
  std::uint64_t previous_end = blobs_begin;

  // THE TWO-RUN ORDERING, the same rule assemble() enforces: every plate
  // record precedes every audio record, and ids strictly increase WITHIN each
  // kind's run — the audio run's id chain is its own and may restart where the
  // plate run's ended. The blob checks below (alignment, range, overlap,
  // padding, digest) know nothing of kinds and apply to both unchanged.
  bool audio_run = false;
  bool run_open = false;  // the current run holds at least one record
  std::uint16_t previous_id = 0;
  std::uint32_t audio_seen = 0;

  for (std::uint32_t i = 0; i < record_count; ++i) {
    const auto at = kHeaderBytes + kRecordBytes * static_cast<std::size_t>(i);
    Record r{};
    if (const auto res = read_record(image.subspan(at, kRecordBytes), r); !res) {
      return {res.error, 0, static_cast<std::uint16_t>(i)};
    }

    if (r.role == Role::Audio) {
      if (!audio_run) {
        audio_run = true;
        run_open = false;  // the audio id chain starts here
      }
      audio_seen += 1;
    } else if (audio_run) {
      return {PackError::AudioBeforePlates, 0, static_cast<std::uint16_t>(i)};
    }
    if (run_open && r.plate_id <= previous_id) {
      return {PackError::RecordsOutOfOrder, 0, static_cast<std::uint16_t>(i)};
    }
    previous_id = r.plate_id;
    run_open = true;

    if (const auto err = validate_record(r, r.length); err != PackError::None) {
      return {err, 0, static_cast<std::uint16_t>(i)};
    }

    if (r.offset % kBlobAlignment != 0) {
      return {PackError::BlobMisaligned, 0, static_cast<std::uint16_t>(i)};
    }
    const auto end = static_cast<std::uint64_t>(r.offset) + static_cast<std::uint64_t>(r.length);
    if (end > header.total_bytes) {
      return {PackError::BlobOutOfRange, 0, static_cast<std::uint16_t>(i)};
    }

    // Two distinct failures, because they mean different things to whoever is
    // holding a pack that will not load: a blob pointing into the manifest is a
    // writer that got the record table's size wrong, while a blob pointing into
    // its predecessor is a layout that overlaps. Reporting the first as an
    // overlap sends the reader hunting for a second plate that is not involved.
    if (r.offset < blobs_begin) {
      return {PackError::BlobInsideManifest, 0, static_cast<std::uint16_t>(i)};
    }
    if (r.offset < previous_end) {
      return {PackError::BlobsOverlap, 0, static_cast<std::uint16_t>(i)};
    }

    // The alignment gap ahead of this blob. pack.hpp claims padding "is not a
    // place to hide a byte"; hashing alone only makes that true against
    // corruption, since anything that rewrites the file can recompute a digest
    // that sits outside its own coverage. Checking it is what makes the claim
    // true against a rewrite as well.
    for (auto gap = previous_end; gap < r.offset; ++gap) {
      if (image[gap] != std::byte{0}) {
        return {PackError::PaddingNotZero, 0, static_cast<std::uint16_t>(i)};
      }
    }
    previous_end = end;

    const auto blob = image.subspan(r.offset, r.length);
    if (hash::sha256(blob) != r.sha256) {
      return {PackError::PlateDigestMismatch, 0, static_cast<std::uint16_t>(i)};
    }
  }

  // The counts the header CLAIMS against the records actually present. The
  // table holds plate_count + audio_count slots, so one kind's tally being
  // wrong implies the other's is too — a single comparison catches both.
  if (audio_seen != header.audio_count) return {PackError::AudioCountMismatch, 0, 0};

  const auto digest =
      hash::sha256(image.subspan(kDigestCoverageStart, image.size() - kDigestCoverageStart));
  if (digest != header.pack_sha256) return {PackError::PackDigestMismatch, 0, 0};

  return {PackError::None, image.size(), 0};
}

}  // namespace gloam::pack
