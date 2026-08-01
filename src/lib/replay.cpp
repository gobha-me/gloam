#include "gloam/replay.hpp"

#include "bytes.hpp"

namespace gloam::replay {
namespace {

// Little-endian, one byte at a time — see src/lib/bytes.hpp for why.
using namespace le;  // NOLINT(google-build-using-namespace)

/// Field offsets, named once. A layout typo in two places is a layout typo that
/// round-trips cleanly and corrupts nothing until someone else's reader tries.
namespace hdr {
constexpr std::size_t kMagic = 0;
constexpr std::size_t kVersion = 4;
constexpr std::size_t kTickHz = 6;
constexpr std::size_t kDigest = kDigestOffset;
constexpr std::size_t kSeed = kDigestCoverageStart;
constexpr std::size_t kRulesetHash = 48;
constexpr std::size_t kPackHash = 56;
constexpr std::size_t kFinalWorldHash = 64;
constexpr std::size_t kRecordCount = 96;
constexpr std::size_t kTotalBytes = 100;
}  // namespace hdr

namespace rec {
constexpr std::size_t kTick = 0;
constexpr std::size_t kEvent = 4;
constexpr std::size_t kPayload = 5;
}  // namespace rec

static_assert(hdr::kTotalBytes + 4 == kHeaderBytes, "the header layout must fill its 104 bytes");
static_assert(rec::kPayload + 2 == kRecordBytes, "the record layout must fill its 7 bytes");
static_assert(hdr::kDigest + 32 == hdr::kSeed, "the digest must end where its coverage begins");

/// Computed in 64 bits so that a hostile `record_count` cannot wrap the
/// multiplication into a plausible-looking total on a 32-bit `size_t`.
[[nodiscard]] auto claimed_bytes(std::uint32_t record_count) -> std::uint64_t {
  return static_cast<std::uint64_t>(kHeaderBytes) +
         static_cast<std::uint64_t>(kRecordBytes) * static_cast<std::uint64_t>(record_count);
}

/// Everything about the header that can be judged without seeing the rest of
/// the file. Shared by `read_header` and `verify` so the two cannot disagree.
[[nodiscard]] auto read_header_fields(std::span<const std::byte> in, Header& out) -> ReplayResult {
  if (in.size() < kHeaderBytes) return {ReplayError::Truncated, 0, 0, false};

  for (std::size_t i = 0; i < kMagic.size(); ++i) {
    if (get_u8(in, hdr::kMagic + i) != static_cast<std::uint8_t>(kMagic[i])) {
      return {ReplayError::BadMagic, 0, 0, false};
    }
  }

  Header h{};
  h.version = get_u16(in, hdr::kVersion);
  if (h.version != kVersion) return {ReplayError::UnsupportedVersion, 0, 0, false};

  h.tick_hz = get_u16(in, hdr::kTickHz);
  if (h.tick_hz == 0) return {ReplayError::ZeroTickHz, 0, 0, false};

  h.file_sha256 = get_digest(in, hdr::kDigest);
  h.seed = get_u64(in, hdr::kSeed);
  h.ruleset_hash = get_u64(in, hdr::kRulesetHash);
  h.pack_hash = get_u64(in, hdr::kPackHash);
  h.final_world_hash = get_digest(in, hdr::kFinalWorldHash);
  h.record_count = get_u32(in, hdr::kRecordCount);
  h.total_bytes = get_u32(in, hdr::kTotalBytes);

  if (h.record_count == 0) return {ReplayError::ZeroRecords, 0, 0, false};
  if (claimed_bytes(h.record_count) != static_cast<std::uint64_t>(h.total_bytes)) {
    return {ReplayError::RecordCountMismatch, 0, 0, false};
  }
  // AGAINST THE BYTES ACTUALLY PRESENT, and this is the load-bearing one. The
  // check above only proves the two header fields agree with EACH OTHER, and
  // both come from the file — a 104-byte image claiming 613,566,741 records and
  // 4,294,967,291 bytes satisfies it exactly. Callers size allocations from
  // `record_count`, so letting that number out of here unchecked against the
  // real length is a 4.9 GB `std::vector` and a SIGABRT.
  if (static_cast<std::uint64_t>(h.total_bytes) != static_cast<std::uint64_t>(in.size())) {
    return {ReplayError::TotalBytesMismatch, 0, 0, false};
  }

  out = h;
  return {ReplayError::None, kHeaderBytes, 0, false};
}

/// The whole structural pass, in ONE place: header, every record, tick order,
/// then the digest.
///
/// `verify` and `load` both call this rather than each walking the file
/// themselves. When they had a loop each, the only thing keeping them agreeing
/// about what a legal replay is was that the two loops happened to be identical
/// — so `gloam_replay verify` and `gloam_replay play` were one edit away from
/// disagreeing about the same file, and `load`'s copy was dead code that would
/// spring to life the moment they diverged.
///
/// When `fill` is set, `sink` receives the records as they are read; it is
/// filled progressively and is meaningful only if this returns success. `fill`
/// is an explicit flag rather than `!sink.empty()` so that a caller handing in
/// a zero-length span gets `BufferTooSmall` instead of a cheerful success and
/// no records.
[[nodiscard]] auto scan(std::span<const std::byte> image, Header& out, std::span<Record> sink,
                        bool fill) -> ReplayResult {
  Header header{};
  if (const auto res = read_header_fields(image, header); !res) return res;

  if (fill && sink.size() < header.record_count) {
    return {ReplayError::BufferTooSmall, 0, 0, false};
  }

  // Structure before the digest, `pack::verify`'s order and its reason: the
  // error should name the smallest thing that is actually wrong. "record 17
  // carries event 200" is a diagnosis; "the file is corrupt" is a symptom.
  std::uint32_t previous_tick = 0;
  for (std::uint32_t i = 0; i < header.record_count; ++i) {
    const auto at = kHeaderBytes + kRecordBytes * static_cast<std::size_t>(i);
    Record record{};
    if (auto res = read_record(image.subspan(at, kRecordBytes), record); !res) {
      res.record_index = i;
      return res;
    }
    // §3: "ordered by tick". EQUAL IS LEGAL — two inputs can land in one tick,
    // and they are then applied in file order. Only going backwards is refused.
    if (record.tick < previous_tick) return {ReplayError::TicksOutOfOrder, 0, i, false};
    if (record.tick > kMaxTick) return {ReplayError::TickOutOfRange, 0, i, false};
    previous_tick = record.tick;
    if (fill) sink[i] = record;
  }

  const auto digest =
      hash::sha256(image.subspan(kDigestCoverageStart, image.size() - kDigestCoverageStart));
  if (digest != header.file_sha256) return {ReplayError::FileDigestMismatch, 0, 0, false};

  out = header;
  return {ReplayError::None, image.size(), 0, false};
}

}  // namespace

auto describe(ReplayError error) -> const char* {
  switch (error) {
    case ReplayError::None: return "no error";
    case ReplayError::BadMagic: return "not a replay: the magic is not \"GLRP\"";
    case ReplayError::UnsupportedVersion: return "a replay version this build cannot read";
    case ReplayError::Truncated: return "fewer bytes present than the header claims";
    case ReplayError::BufferTooSmall: return "the output buffer is too small";
    case ReplayError::ZeroTickHz: return "tick_hz is zero";
    case ReplayError::ZeroRecords: return "the input log is empty";
    case ReplayError::ReservedEvent: return "event 0 is reserved and never valid";
    case ReplayError::UnknownEvent: return "an event value this build has never heard of";
    case ReplayError::PayloadOutOfRange: return "a payload that event cannot mean";
    case ReplayError::TicksOutOfOrder: return "records are not ordered by tick";
    case ReplayError::TickOutOfRange: return "a tick past kMaxTick: the replay would never finish";
    case ReplayError::RecordCountMismatch: return "record_count disagrees with total_bytes";
    case ReplayError::TotalBytesMismatch: return "total_bytes disagrees with the file size";
    case ReplayError::FileDigestMismatch: return "file_sha256 does not match the bytes";
    case ReplayError::RulesetMismatch:
      return "recorded against different tuning (SPEC §12: rejected, never mis-played)";
    case ReplayError::WorldHashMismatch: return "the replay did not reproduce final_world_hash";
  }
  return "unknown error";
}

auto write_header(std::span<std::byte> out, const Header& header) -> ReplayResult {
  if (out.size() < kHeaderBytes) return {ReplayError::BufferTooSmall, 0, 0, false};

  for (std::size_t i = 0; i < kMagic.size(); ++i) {
    put_u8(out, hdr::kMagic + i, static_cast<std::uint8_t>(kMagic[i]));
  }
  put_u16(out, hdr::kVersion, header.version);
  put_u16(out, hdr::kTickHz, header.tick_hz);
  put_digest(out, hdr::kDigest, header.file_sha256);
  put_u64(out, hdr::kSeed, header.seed);
  put_u64(out, hdr::kRulesetHash, header.ruleset_hash);
  put_u64(out, hdr::kPackHash, header.pack_hash);
  put_digest(out, hdr::kFinalWorldHash, header.final_world_hash);
  put_u32(out, hdr::kRecordCount, header.record_count);
  put_u32(out, hdr::kTotalBytes, header.total_bytes);

  return {ReplayError::None, kHeaderBytes, 0, false};
}

auto write_record(std::span<std::byte> out, const Record& record) -> ReplayResult {
  if (out.size() < kRecordBytes) return {ReplayError::BufferTooSmall, 0, 0, false};

  // Refused here rather than only at load, for `pack::assemble`'s reason: a
  // writer that can emit a record its own reader will reject produces files
  // whose corruption is indistinguishable from a bad disk.
  if (record.event == Event::None) return {ReplayError::ReservedEvent, 0, 0, false};
  if (static_cast<std::uint8_t>(record.event) > kEventMax) {
    return {ReplayError::UnknownEvent, 0, 0, false};
  }
  if (!payload_valid(record.event, record.payload)) {
    return {ReplayError::PayloadOutOfRange, 0, 0, false};
  }

  put_u32(out, rec::kTick, record.tick);
  put_u8(out, rec::kEvent, static_cast<std::uint8_t>(record.event));
  put_u16(out, rec::kPayload, record.payload);

  return {ReplayError::None, kRecordBytes, 0, false};
}

auto read_header(std::span<const std::byte> in, Header& out) -> ReplayResult {
  return read_header_fields(in, out);
}

auto read_record(std::span<const std::byte> in, Record& out) -> ReplayResult {
  if (in.size() < kRecordBytes) return {ReplayError::Truncated, 0, 0, false};

  Record r{};
  r.tick = get_u32(in, rec::kTick);

  const auto event = get_u8(in, rec::kEvent);
  if (event == static_cast<std::uint8_t>(Event::None)) {
    return {ReplayError::ReservedEvent, 0, 0, false};
  }
  if (event > kEventMax) return {ReplayError::UnknownEvent, 0, 0, false};
  r.event = static_cast<Event>(event);

  r.payload = get_u16(in, rec::kPayload);
  if (!payload_valid(r.event, r.payload)) {
    return {ReplayError::PayloadOutOfRange, 0, 0, false};
  }

  out = r;
  return {ReplayError::None, kRecordBytes, 0, false};
}

auto assemble(Header& header, std::span<const Record> records, std::span<std::byte> out)
    -> ReplayResult {
  if (records.empty()) return {ReplayError::ZeroRecords, 0, 0, false};

  // BOUND THE TOTAL, NOT THE COUNT. `record_count` fitting in u32 is not the
  // constraint — `total_bytes` is also u32, and it has to hold 104 + 7*count,
  // so the real ceiling is 613,566,741 records. Bounding the count alone let
  // `assemble` truncate the total on the way into the header and return
  // success, emitting a file whose very next `verify` refuses it with
  // `RecordCountMismatch`: a writer producing files its own reader rejects,
  // which is the failure `write_record`'s validation exists to prevent.
  constexpr std::uint64_t kMaxRecords =
      (static_cast<std::uint64_t>(UINT32_MAX) - kHeaderBytes) / kRecordBytes;
  if (static_cast<std::uint64_t>(records.size()) > kMaxRecords) {
    return {ReplayError::RecordCountMismatch, 0, 0, false};
  }

  const auto count = static_cast<std::uint32_t>(records.size());
  const auto total = image_bytes(count);
  if (out.size() < total) return {ReplayError::BufferTooSmall, 0, 0, false};

  // Derived, never taken from the caller: the same division `pack::assemble`
  // draws, so nothing a caller can get subtly wrong reaches the file.
  header.record_count = count;
  header.total_bytes = static_cast<std::uint32_t>(total);
  header.file_sha256 = hash::Digest{};

  if (const auto res = write_header(out, header); !res) return res;

  std::uint32_t previous_tick = 0;
  for (std::uint32_t i = 0; i < count; ++i) {
    const auto& record = records[i];
    if (record.tick < previous_tick) return {ReplayError::TicksOutOfOrder, 0, i, false};
    if (record.tick > kMaxTick) return {ReplayError::TickOutOfRange, 0, i, false};
    previous_tick = record.tick;

    const auto at = kHeaderBytes + kRecordBytes * static_cast<std::size_t>(i);
    if (auto res = write_record(out.subspan(at, kRecordBytes), record); !res) {
      res.record_index = i;
      return res;
    }
  }

  const auto digest = hash::sha256(out.subspan(kDigestCoverageStart, total - kDigestCoverageStart));
  put_digest(out, hdr::kDigest, digest);
  header.file_sha256 = digest;

  return {ReplayError::None, total, 0, false};
}

auto verify(std::span<const std::byte> image) -> ReplayResult {
  Header header{};
  return scan(image, header, {}, false);
}

auto load(std::span<const std::byte> image, Expect expect, Header& header,
          std::span<Record> records) -> ReplayResult {
  Header parsed{};
  if (const auto res = scan(image, parsed, records, true); !res) return res;

  // §12, and the reason it is not a warning: "a replay recorded against
  // different tuning is rejected at load rather than silently mis-played —
  // otherwise a golden test starts passing for the wrong reason."
  if (parsed.ruleset_hash != expect.ruleset_hash) {
    return {ReplayError::RulesetMismatch, 0, 0, false};
  }

  // SCHEMAS.md §3: "art changes do not affect simulation, so a mismatch warns
  // rather than rejects". Suppressed when the FILE claims no pack — a replay
  // that recorded nothing to disagree with cannot disagree.
  const bool pack_mismatch =
      parsed.pack_hash != kNoPackHash && parsed.pack_hash != expect.pack_hash;

  header = parsed;
  return {ReplayError::None, image.size(), 0, pack_mismatch};
}

}  // namespace gloam::replay
