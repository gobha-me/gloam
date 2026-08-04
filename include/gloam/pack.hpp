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
enum class Role : std::uint8_t {
  Wall = 0,
  Floor = 1,
  Ceiling = 2,
  LightField = 3,
  Monster = 4,
  Item = 5,
  Ui = 6,
  Rune = 7,
};
inline constexpr std::uint8_t kRoleMax = 7;

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

/// One plate. Fifty-two bytes on disk, in this field order.
///
/// There is NO KITTY IMAGE ID HERE, and that absence is the design. An image id
/// is a property of a live terminal session — kitty reads `i=0` as unset,
/// termforge recycles ids and LRU-evicts them (upstream #109) — so binding one
/// at bake time would burn a runtime residency policy into a build artifact and
/// change `pack_sha256` whenever that policy changed. The `plate_id -> image_id`
/// map belongs to the uploader, and the uploader is a binary.
struct Record {
  std::uint16_t plate_id{0};
  Role role{Role::Wall};
  std::uint8_t depth{kDepthFullFrame};
  Lateral lateral{Lateral::FullFrame};
  std::uint8_t wall_type{0};
  Codec codec{Codec::RawPlanes};
  std::uint16_t w{0};
  std::uint16_t h{0};
  std::uint32_t offset{0};  ///< absolute from file start; filled by assemble()
  std::uint32_t length{0};  ///< filled by assemble()
  hash::Digest sha256{};    ///< over [offset, offset + length); filled by assemble()

  [[nodiscard]] auto operator==(const Record&) const -> bool = default;
};

struct Header {
  std::uint16_t version{kVersion};
  hash::Digest pack_sha256{};
  std::uint16_t plate_count{0};
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
  RecordsOutOfOrder = 12,     ///< plate_id must be strictly increasing
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
  TooManyPlates = 24,      ///< assemble(): more records than plate_count can express
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

/// Where the first blob starts: past the header and every record.
[[nodiscard]] constexpr auto first_blob_offset(std::uint16_t plate_count) -> std::uint32_t {
  return align_up(static_cast<std::uint32_t>(kHeaderBytes) +
                  static_cast<std::uint32_t>(kRecordBytes) *
                      static_cast<std::uint32_t>(plate_count));
}

/// The size `assemble` will need, computed from extents alone — so a caller can
/// size its output buffer before any offset has been filled in.
[[nodiscard]] auto image_bytes(std::span<const Record> records) -> std::size_t;

// ── Field-at-a-time codecs ─────────────────────────────────────────────────

[[nodiscard]] auto write_header(std::span<std::byte> out, const Header& header) -> PackResult;
[[nodiscard]] auto write_record(std::span<std::byte> out, const Record& record) -> PackResult;
[[nodiscard]] auto read_header(std::span<const std::byte> in, Header& out) -> PackResult;
[[nodiscard]] auto read_record(std::span<const std::byte> in, Record& out) -> PackResult;

/// Build a whole pack image into `out`.
///
/// The caller supplies only the DESCRIPTIVE fields of each record — id, role,
/// depth, lateral, wall type, codec, extent. `assemble` fills `offset`, `length`
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
