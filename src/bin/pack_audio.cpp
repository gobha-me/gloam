#include "pack_audio.hpp"

#include <array>

namespace gloam::pack_audio {
namespace {

/// The audio run's records, walked once for their DESCRIPTIVE fields: which
/// SoundIds are covered, what each claims to be, and how many frames the run
/// carries in total. This is the whole of `check`, and the first half of
/// `load` — the load re-walks the run to expand, so a refusal below leaves
/// the caller's arena exactly as it found it.
///
/// `verify` has already passed by the time anyone calls this (the header
/// says why that is the assumption), so every refusal here is either the
/// semantic half verify cannot know — a SoundId this binary has never heard
/// of, a hole in the coverage — or a defensive branch over the parser's own
/// checks, kept because a gate that trusts its predecessor blindly is one
/// refactor away from being a hole.
[[nodiscard]] auto walk_records(std::span<const std::byte> image) -> LoadResult {
  pack::Header header{};
  if (const auto res = pack::read_header(image, header); !res) {
    return {LoadError::Malformed, 0, 0, res.error};
  }

  // The one failure a structurally valid pack can still present: no audio
  // run at all. A pack baked before the record kind existed reads back with
  // a zero count (pack.hpp's deviation 5) and lands HERE — which is §9.2's
  // launch gate doing its job, not a parser error.
  if (header.audio_count == 0) return {LoadError::MissingAudio, 0, 0, pack::PackError::None};

  std::array<bool, audio::kSoundIdCount> seen{};
  std::uint64_t frames = 0;

  for (std::uint16_t i = 0; i < header.audio_count; ++i) {
    // The audio run follows the plate run (pack.hpp's two-run ordering), so
    // its records sit at plate_count + i. The bounds check precedes the
    // subspan: forming a span past the image would be UB before read_record
    // ever got to refuse it.
    const auto at = pack::kHeaderBytes +
                    pack::kRecordBytes * static_cast<std::size_t>(header.plate_count + i);
    if (at + pack::kRecordBytes > image.size()) {
      return {LoadError::Malformed, 0, 0, pack::PackError::Truncated};
    }
    pack::Record record{};
    if (const auto res = pack::read_record(image.subspan(at, pack::kRecordBytes), record);
        !res) {
      return {LoadError::Malformed, 0, 0, res.error};
    }
    // A plate record sitting in the audio run means the header's counts
    // disagree with the table — verify's AudioCountMismatch, arrived at from
    // the other direction.
    if (record.role != pack::Role::Audio) {
      return {LoadError::Malformed, 0, 0, pack::PackError::AudioCountMismatch};
    }

    const std::uint16_t id = record.plate_id;  // the sound id under Role::Audio
    if (id == 0 || static_cast<std::size_t>(id) >= audio::kSoundIdCount) {
      // The pack describes a sound this binary was not built with. Not a
      // coverage hole — every SoundId might still be present beside it — so
      // it gets its own name rather than MissingAudio's.
      return {LoadError::UnknownSound, id, 0, pack::PackError::None};
    }
    if (seen[id]) {
      // verify's strictly-increasing-ids rule refuses this first; the branch
      // exists so the loader's own invariant — one record per SoundId —
      // never depends on that ordering.
      return {LoadError::DuplicateSound, id, 0, pack::PackError::None};
    }
    seen[id] = true;

    // The stream's shape is NOT the record's to redescribe (the header says
    // why): mono s16le at the one rate is what the arena is. frame_count is
    // the exception — the record's own number is the truth about the bytes.
    if (record.sample_rate != static_cast<std::uint16_t>(audio::kSampleRateHz) ||
        record.channels != 1 || record.sample_format != pack::SampleFormat::S16Le ||
        record.frame_count == 0) {
      return {LoadError::UnexpectedFormat, id, 0, pack::PackError::None};
    }
    // The length's agreement with the extent is verify's check
    // (BlobLengthWrongForFrames); it is repeated here because `load` indexes
    // the blob BY frame_count, and a gate's own reads should be safe on the
    // gate's own checks rather than its predecessor's.
    if (record.length != pack::audio_blob_bytes(record.frame_count, record.channels,
                                                record.sample_format)) {
      return {LoadError::Malformed, id, 0, pack::PackError::BlobLengthWrongForFrames};
    }
    frames += record.frame_count;
  }

  // Coverage, rather than count: audio_count records could be the right
  // number and still leave a SoundId unrecorded.
  for (std::size_t id = 1; id < audio::kSoundIdCount; ++id) {
    if (!seen[id]) {
      return {LoadError::MissingAudio, static_cast<std::uint16_t>(id), 0,
              pack::PackError::None};
    }
  }

  return {LoadError::None, 0, static_cast<std::size_t>(frames), pack::PackError::None};
}

}  // namespace

auto name(LoadError error) -> std::string_view {
  switch (error) {
    case LoadError::None: return "None";
    case LoadError::MissingAudio: return "MissingAudio";
    case LoadError::UnknownSound: return "UnknownSound";
    case LoadError::DuplicateSound: return "DuplicateSound";
    case LoadError::UnexpectedFormat: return "UnexpectedFormat";
    case LoadError::ArenaTooSmall: return "ArenaTooSmall";
    case LoadError::Malformed: return "Malformed";
  }
  return "UnknownLoadError";
}

auto check(std::span<const std::byte> image) -> LoadResult { return walk_records(image); }

auto load(std::span<const std::byte> image, std::span<float> arena,
          std::span<sfx::Clip, audio::kSoundIdCount> clips) -> LoadResult {
  const auto checked = walk_records(image);
  if (!checked) return checked;

  // Capacity before a single sample is written: sfx.hpp's refusal rule, kept
  // verbatim — having written nothing, the caller can still tell the player
  // exactly which constant the pack and the binary disagree about.
  if (arena.size() < checked.frames) {
    return {LoadError::ArenaTooSmall, 0, checked.frames, pack::PackError::None};
  }

  // `None` gets no record — silence has no samples — and the entry is reset
  // rather than trusted to arrive zeroed: the clips table is the only map
  // anyone reads, so nothing stale may survive in it.
  clips[0] = sfx::Clip{};

  pack::Header header{};
  static_cast<void>(pack::read_header(image, header));  // walk_records just read it

  std::size_t cursor = 0;
  for (std::uint16_t i = 0; i < header.audio_count; ++i) {
    const auto at = pack::kHeaderBytes +
                    pack::kRecordBytes * static_cast<std::size_t>(header.plate_count + i);
    pack::Record record{};
    static_cast<void>(pack::read_record(image.subspan(at, pack::kRecordBytes), record));

    // Bounds first, as in the walk: verify has already ranged every blob,
    // and the check before the subspan is what keeps that a description of
    // the code rather than a hope about its caller.
    const auto end = static_cast<std::uint64_t>(record.offset) + record.length;
    if (end > image.size()) {
      return {LoadError::Malformed, record.plate_id, 0, pack::PackError::BlobOutOfRange};
    }
    const auto blob = image.subspan(record.offset, record.length);

    // Two bytes at a time, little-endian, assembled explicitly — pack.hpp's
    // LE law reaches blob payloads, and never a reinterpret_cast: the blob
    // has no alignment promise a `std::int16_t*` dereference could lean on,
    // and on a big-endian host the cast would silently swap every sample.
    // The multiply is the expansion sfx.hpp pins: exact on every IEEE-754
    // target, so the loaded arena is the synthesised one, bit for bit.
    for (std::uint32_t j = 0; j < record.frame_count; ++j) {
      const auto bits = static_cast<std::uint16_t>(
          static_cast<std::uint16_t>(blob[2 * j]) |
          static_cast<std::uint16_t>(static_cast<std::uint16_t>(blob[2 * j + 1]) << 8U));
      arena[cursor + j] = static_cast<float>(static_cast<std::int16_t>(bits)) *
                          (1.0f / 32768.0f);
    }

    clips[record.plate_id] =
        sfx::Clip{static_cast<std::uint32_t>(cursor), record.frame_count};
    cursor += record.frame_count;
  }

  return {LoadError::None, 0, cursor, pack::PackError::None};
}

}  // namespace gloam::pack_audio
