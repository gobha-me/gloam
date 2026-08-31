#pragma once

/// SPEC §10, §12 — `pack.manifest`, and the bytes it describes.
///
/// §12 gives the shape: "Header, then one fixed-size record per plate." §10
/// gives the reason it is hashed: "Because plates are transmitted once at
/// startup, pack integrity is a hard startup requirement: a mismatched hash
/// refuses to launch rather than half-uploading a corrupt plate set."
///
///
/// WHERE THIS DEVIATES FROM `design/SCHEMAS.md`, AND WHY
///
/// `design/` is a snapshot of the design project that owns it, so it is not
/// edited here (`design/README.md`). Each deviation below is recorded in
/// UPSTREAM.md's "Corrections to the design document" and mirrored as a GLOAM
/// issue, per AGENTS.md, so the call gets made rather than absorbed:
///
///   1. The record gains `codec:u8`. termforge #163 landed a verbatim transmit
///      path that takes pre-encoded bytes, which is how the §11 cold-start
///      budget eventually gets met — but nothing encodes PNG today. A one-byte
///      discriminant now means that lands as a new codec value rather than a
///      format version bump, and `Codec::Png` already PARSES and is REFUSED, so
///      the forward door is a door and not a hole.
///   2. Endianness and packing were unstated. Little-endian, fields serialized
///      one at a time, never a `memcpy` of a compiler struct. See below.
///   3. Manifest and pixels are ONE file. SCHEMAS.md's `offset`/`length` imply a
///      blob region but never say where. Two files means the manifest can be
///      fresher than the pixels, and §10's "a mismatched hash refuses to launch"
///      needs one atomic object to hash.
///   4. A plate's pixels are two planes (`plate.hpp`), which is how "four
///      colours plus transparent" — five states — fits in two bits.
///   5. A second record KIND: audio. §9.2 requires a resident audio arena in
///      the pack sharing §10's manifest hash, but the snapshot's `role` enum
///      stops at `rune` and its record is shaped for plates — `w`, `h` and a
///      palette codec, with nowhere to put a sample rate, a channel count or
///      a frame count. `Role::Audio` reads the same 52 bytes as a PCM
///      descriptor (see `Record`), and the header's reserved1 is CLAIMED as
///      `audio_count`: a pack baked before this kind existed reads back with
///      a zero count, unchanged, while an old reader handed a pack WITH
///      sounds refuses it as ReservedNotZero — which is the fail-closed
///      direction. Recorded in UPSTREAM.md item 10, mirrored as gloam#23.
///
///
/// LITTLE-ENDIAN, ALWAYS
///
/// Every target GLOAM runs on is little-endian, so an LE writer has no
/// byte-swap branch at all. An untested swap path in the one artifact whose hash
/// is a build gate is precisely the code that turns out to be wrong, years
/// later, on the one machine nobody has. Fields are read and written a byte at a
/// time, so ABI padding and struct alignment cannot leak into the digest either.
/// (`sha256.cpp`'s message schedule is big-endian because FIPS 180-4 says so;
/// that is the hash's business and unrelated to this.)
///
///
/// THE BUDGET IS NOT ENFORCED HERE, ON PURPOSE
///
/// `emit.hpp` states the rule this header follows: "The sink reports; the budget
/// judges; exactly one file can relax a budget." So `read_header` does not
/// compare `plate_count` against `budget::kMaxResidentImages`, and this header
/// does not include `budgets.hpp`. It rejects what is MALFORMED — a blob past
/// the end of the file, a length that disagrees with its extent — and leaves
/// what is merely OVER BUDGET to `test/10budgets/` and to the baker. A parser
/// that knows the budget is a parser that can be configured to a different one.
///
/// Nothing here opens a file. Every entry point is `(caller-owned span,
/// integers) -> bytes written into that span`; `src/bin/bake.cpp` owns the
/// buffers and holds the only `write`. Excluded from the `gloam/gloam.hpp`
/// umbrella — pipeline-side, not simulation.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "gloam/plate.hpp"
#include "gloam/sha256.hpp"

namespace gloam::pack {

inline constexpr std::array<char, 4> kMagic{'G', 'L', 'P', 'K'};
inline constexpr std::uint16_t kVersion = 1;

inline constexpr std::size_t kHeaderBytes = 48;
inline constexpr std::size_t kRecordBytes = 52;

/// Where `pack_sha256` sits, and what it covers: `[kDigestCoverageStart, EOF)`.
///
/// Magic and version sit OUTSIDE the digest deliberately — you have to read them
/// to know whether the rest of the file has the shape you are about to hash, and
/// a digest you cannot check without first trusting the file is not a check.
inline constexpr std::size_t kDigestOffset = 8;
inline constexpr std::size_t kDigestCoverageStart = 40;

/// Blobs start on a 4-byte boundary. Gap bytes are written zero AND hashed, so
/// padding is not a place to hide a byte.
inline constexpr std::uint32_t kBlobAlignment = 4;

/// §12: "`depth` — 0-4, or 255 for full-frame plates."
inline constexpr std::uint8_t kDepthFullFrame = 255;

/// §12: "wall | floor | ceiling | light_field | monster | item | ui | rune"
///
/// `Audio` is NOT in the snapshot's list — the snapshot's record is shaped for
/// plates and has nowhere to put a sample rate, which is UPSTREAM.md item 10,
/// mirrored as gloam#23 and recorded as deviation 5 above. It is appended,
/// never renumbered, and it does not name a plate kind: it selects the
/// record's SECOND reading, so the middle of the record means something else
/// under it (see `Record`).
enum class Role : std::uint8_t {
  Wall = 0,
  Floor = 1,
  Ceiling = 2,
  LightField = 3,
  Monster = 4,
  Item = 5,
  Ui = 6,
  Rune = 7,
  Audio = 8,
};
inline constexpr std::uint8_t kRoleMax = 8;

/// §12: "left | centre | right | full_frame"
enum class Lateral : std::uint8_t { Left = 0, Centre = 1, Right = 2, FullFrame = 3 };
inline constexpr std::uint8_t kLateralMax = 3;

/// How a plate's blob is encoded.
///
/// `RawPlanes` is the only thing written or accepted today: `plate.hpp`'s index
/// plane followed by its stencil plane, `plate::blob_bytes(w, h)` long. `Png` is
/// the slot termforge #163's `f=100` path lands in — it parses, and `verify`
/// refuses it, which is what makes this a versioned door rather than an
/// unchecked one.
///
/// THE `f=100` PATH HAS SINCE LANDED (`png.hpp`) AND THIS STILL REFUSES `Png`.
/// That is a decision, not an omission. The pack is a pixel source and its hash
/// is a build gate — §10: "two pipeline runs must produce byte-identical packs".
/// A PNG in the pack would put the compressor, the palette and the filter choice
/// inside `pack_sha256`, so improving the encoder or moving one grey value would
/// invalidate every baked pack for a reason that has nothing to do with the
/// pixels. Encoding at transmit keeps the two hashes measuring different things:
/// this one asks "are these the same pixels", `test/25png/`'s digest asks "are
/// these the same bytes on the wire". The door stays shut until something needs
/// a plate whose SOURCE is compressed — authored art from an external tool, say
/// — rather than one GLOAM compresses on its way out. See gloam#16.
enum class Codec : std::uint8_t { RawPlanes = 0, Png = 1 };
inline constexpr std::uint8_t kCodecMax = 1;

/// How an audio record's blob is encoded — the audio-side occupant of the
/// record's +6 discriminant slot, where a plate keeps its `Codec`.
///
/// `S16Le` is the only thing written or accepted today: signed 16-bit
/// little-endian PCM, channels interleaved. A one-value enum is not degenerate
/// here, it is the same door `Codec` keeps — a format this version has never
/// heard of is refused by value (`UnknownSampleFormat`), so a second encoding
/// lands as a new enumerator rather than a format version bump.
enum class SampleFormat : std::uint8_t { S16Le = 0 };
inline constexpr std::uint8_t kSampleFormatMax = 0;

/// Bytes per sample under each format. `S16Le` is two.
[[nodiscard]] constexpr auto bytes_per_sample(SampleFormat format) -> std::uint32_t {
  return format == SampleFormat::S16Le ? 2U : 0U;
}

/// A sound's blob length from its extent, the way `plate::blob_bytes` is a
/// plate's — and what a manifest record's `length` must equal under it.
///
/// The product is u64 because frame_count × channels × bytes_per_sample can
/// exceed the u32 a record's `length` field carries, and `verify` needs the
/// honest product to refuse exactly that record rather than a wrapped one.
[[nodiscard]] constexpr auto audio_blob_bytes(std::uint32_t frame_count, std::uint8_t channels,
                                              SampleFormat format) -> std::uint64_t {
  return static_cast<std::uint64_t>(frame_count) * static_cast<std::uint64_t>(channels) *
         bytes_per_sample(format);
}

/// One plate, or one sound. Fifty-two bytes on disk either way, in this field
/// order; `role` decides which reading the descriptive middle of the record
/// gets.
///
/// There is NO KITTY IMAGE ID HERE, and that absence is the design. An image id
/// is a property of a live terminal session — kitty reads `i=0` as unset, and
/// termforge deliberately keeps recycled protocol ids behind generation-qualified
/// `PinnedImage` handles (upstream #109/#110). Binding one at bake time would
/// burn a runtime residency policy into a build artifact and change `pack_sha256`
/// whenever that policy changed. The `plate_id -> PinnedImage` map belongs to the
/// uploader, and the uploader is a binary.
///
/// THE TWO READINGS, byte by byte (deviation 5, gloam#23):
///
///   +0-1    plate_id — which IS the sound id under `Audio`: one field, two
///           names, each kind's ids strictly increasing within its own run
///   +2      role — the discriminant the whole reading turns on
///   +3-4    plate: depth:u8, lateral:u8   sound: sample_rate:u16
///   +5      plate: variant:u8             sound: channels:u8
///   +6      plate: codec:u8               sound: sample_format:u8
///   +7      reserved, zero, under both readings
///   +8-11   plate: w:u16, h:u16           sound: frame_count:u32
///   +12-51  offset, length, sha256 — identical semantics under both readings
///
/// There is deliberately NO UNION: the struct carries both field sets flat and
/// `write_record`/`read_record` branch on `role`. The fields of the OTHER
/// reading are never serialized — a plate record's `sample_rate` never reaches
/// the file — so leave them defaulted: `operator==` compares them, and a
/// set-but-unserialized field reads back as a round-trip loss that never
/// happened on the wire.
struct Record {
  std::uint16_t plate_id{0};  ///< the sound id under Role::Audio
  Role role{Role::Wall};
  // ── the plate reading (role ≤ Rune) ──
  std::uint8_t depth{kDepthFullFrame};
  Lateral lateral{Lateral::FullFrame};
  /// Role-specific visual variant. Wall uses 0=plain, 1=door at M0; monster
  /// uses 0=calm, 1=alert, 2=hunting. The byte was named `wall_type` in the
  /// design snapshot, but every role shares this fixed record and §4.2 budgets
  /// variant plates outside walls too. Renaming the C++ field changes no v1
  /// wire byte, offset or record size.
  std::uint8_t variant{0};
  Codec codec{Codec::RawPlanes};
  std::uint16_t w{0};
  std::uint16_t h{0};
  // ── the audio reading (role == Audio); same bytes as depth…h above ──
  std::uint16_t sample_rate{0};                    ///< frames per second, > 0
  std::uint8_t channels{0};                        ///< ≥ 1
  SampleFormat sample_format{SampleFormat::S16Le};
  std::uint32_t frame_count{0};                    ///< frames per channel, > 0
  // ── both readings ──
  std::uint32_t offset{0};  ///< absolute from file start; filled by assemble()
  std::uint32_t length{0};  ///< filled by assemble()
  hash::Digest sha256{};    ///< over [offset, offset + length); filled by assemble()

  [[nodiscard]] auto operator==(const Record&) const -> bool = default;
};

struct Header {
  std::uint16_t version{kVersion};
  hash::Digest pack_sha256{};
  std::uint16_t plate_count{0};
  /// Claims the snapshot's reserved1 (bytes +42-43) — deviation 5, gloam#23. A
  /// pack baked before the audio record kind reads back 0 here, which is why
  /// old packs still read; an old READER refuses a nonzero value as
  /// ReservedNotZero, which is the fail-closed direction.
  std::uint16_t audio_count{0};
  std::uint32_t total_bytes{0};
};

/// Why a pack was refused. `None` is success.
enum class PackError : std::uint8_t {
  None = 0,
  BadMagic = 1,
  UnsupportedVersion = 2,
  Truncated = 3,      ///< fewer bytes present than the header or records claim
  BufferTooSmall = 4, ///< an output span too small to hold what was asked for
  ZeroPlates = 5,     ///< an empty pack is a build failure, not an empty level
  ReservedNotZero = 6,        ///< reserved bytes are hashed, so they must be written zero
  UnknownRole = 7,
  UnknownLateral = 8,
  DepthOutOfRange = 9,        ///< not in [0, geometry::kDepthCount) and not kDepthFullFrame
  UnknownCodec = 10,          ///< a codec value this version has never heard of
  UnsupportedCodec = 11,      ///< a codec this version parses but cannot decode
  RecordsOutOfOrder = 12,     ///< ids must strictly increase within each kind's run
  BlobMisaligned = 13,
  BlobOutOfRange = 14,        ///< offset + length past total_bytes, or overflowing
  BlobsOverlap = 15,          ///< blobs appear in record order and may not overlap
  BlobInsideManifest = 16,    ///< a blob starting inside the header or the record table
  BlobLengthWrongForExtent = 17,  ///< length != plate::blob_bytes(w, h) under RawPlanes
  ExtentInvalid = 18,             ///< whatever plate::validate() would refuse
  TotalBytesMismatch = 19,        ///< total_bytes disagrees with the bytes actually present
  PaddingNotZero = 20,            ///< an inter-blob alignment gap holding something
  PlateDigestMismatch = 21,
  PackDigestMismatch = 22,
  BlobCountMismatch = 23,  ///< assemble(): records.size() != blobs.size()
  TooManyPlates = 24,      ///< assemble(): more records of a kind than its u16 count can express
  UnknownSampleFormat = 25,       ///< a sample_format value this version has never heard of
  ZeroChannels = 26,              ///< a sound with no channels carries no samples
  ZeroFrames = 27,                ///< a sound with no frames is not silence, it is a bug
  ZeroSampleRate = 28,            ///< a sound with no sample rate has no pitch to be
  BlobLengthWrongForFrames = 29,  ///< length != frame_count * channels * bytes_per_sample(format)
  AudioBeforePlates = 30,         ///< the plate run ends where the audio run begins
  AudioCountMismatch = 31,        ///< the header's audio_count disagrees with the records present
};

struct PackResult {
  PackError error{PackError::None};
  std::size_t bytes{0};         ///< bytes written or consumed; 0 on every error
  std::uint16_t plate_index{0}; ///< which record failed, for the per-record errors

  [[nodiscard]] constexpr explicit operator bool() const { return error == PackError::None; }
};

/// Round up to the next blob boundary.
///
/// Saturates to the largest representable ALIGNED value rather than to
/// UINT32_MAX, which is 3 mod 4. A saturation that broke the function's one
/// postcondition would hand a caller a misaligned offset in exactly the case it
/// was meant to make safe, and `verify` would then report `BlobMisaligned` —
/// pointing the diagnosis at the offset instead of at the overflow.
[[nodiscard]] constexpr auto align_up(std::uint32_t offset) -> std::uint32_t {
  constexpr std::uint32_t kMaxAligned = UINT32_MAX - (UINT32_MAX % kBlobAlignment);
  const auto slack = offset % kBlobAlignment;
  if (slack == 0) return offset;
  const auto pad = kBlobAlignment - slack;
  if (offset > kMaxAligned - pad) return kMaxAligned;
  return offset + pad;
}

/// Where the first blob starts: past the header and every record, plate and
/// audio alike. The count is a u32 because it is a SUM — plate_count +
/// audio_count does not fit in either of the u16 fields it adds.
///
/// Saturates the way `align_up` does: an unrepresentable answer comes back as
/// the largest ALIGNED u32, so the function's one postcondition survives the
/// overflow it is reporting.
[[nodiscard]] constexpr auto first_blob_offset(std::uint32_t record_count) -> std::uint32_t {
  constexpr std::uint32_t kMaxAligned = UINT32_MAX - (UINT32_MAX % kBlobAlignment);
  const auto past = static_cast<std::uint64_t>(kHeaderBytes) +
                    static_cast<std::uint64_t>(kRecordBytes) * record_count;
  if (past >= kMaxAligned) return kMaxAligned;
  return align_up(static_cast<std::uint32_t>(past));
}

/// The size `assemble` will need, computed from extents alone — a plate's w × h
/// via `plate::blob_bytes`, a sound's frames × channels × format via
/// `audio_blob_bytes` — so a caller can size its output buffer before any
/// offset has been filled in.
[[nodiscard]] auto image_bytes(std::span<const Record> records) -> std::size_t;

// ── Field-at-a-time codecs ─────────────────────────────────────────────────

[[nodiscard]] auto write_header(std::span<std::byte> out, const Header& header) -> PackResult;
[[nodiscard]] auto write_record(std::span<std::byte> out, const Record& record) -> PackResult;
[[nodiscard]] auto read_header(std::span<const std::byte> in, Header& out) -> PackResult;
[[nodiscard]] auto read_record(std::span<const std::byte> in, Record& out) -> PackResult;

/// Build a whole pack image into `out`.
///
/// The caller supplies only the DESCRIPTIVE fields of each record — id, role,
/// depth, lateral, variant, codec, extent. `assemble` fills `offset`, `length`
/// and `sha256`, overwriting whatever was there: a caller-supplied stale digest
/// must not be able to survive into a pack. That division is what makes two runs
/// byte-identical rather than merely equivalent, because nothing a caller can
/// get subtly wrong reaches the file.
///
/// Blobs land in record order, 4-byte aligned, with zeroed padding between them
/// and none after the last.
[[nodiscard]] auto assemble(std::span<Record> records,
                            std::span<const std::span<const std::byte>> blobs,
                            std::span<std::byte> out) -> PackResult;

/// The startup gate (§10). Structure first, then every per-plate digest, then
/// `pack_sha256` — in that order, so the error names the smallest thing that is
/// actually wrong.
[[nodiscard]] auto verify(std::span<const std::byte> image) -> PackResult;

}  // namespace gloam::pack
