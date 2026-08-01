#pragma once

/// SPEC §12, §5.1 — `replay.gloam`, the container a determinism regression
/// arrives in.
///
/// §12 states the load-bearing rule: "`replay.gloam` carries a `ruleset_hash`
/// covering every tunable integer in §6 and §8, and a replay recorded against
/// different tuning is **rejected at load** rather than silently mis-played —
/// otherwise a golden test starts passing for the wrong reason." §5.1 gives the
/// shape: "Replay format: seed plus ordered `(tick, key, event_type)`. Bug
/// reports arrive as playable artifacts, and the replay log doubles as the
/// regression test format."
///
///
/// WHERE THIS DEVIATES FROM `design/SCHEMAS.md` §3, AND WHY
///
/// `design/` is a snapshot of the design project that owns it, so it is not
/// edited here (`design/README.md`). Each item below is recorded in UPSTREAM.md's
/// "Corrections to the design document" as item 8 and mirrored on GLOAM #19 —
/// item 1 on #16, where the question was raised — per AGENTS.md, so the call
/// gets made rather than absorbed:
///
///   1. `pack_hash:u64` IS the first eight bytes of the manifest's
///      `pack_sha256:[32]u8`. SCHEMAS.md carries both widths and never says how
///      one becomes the other. See `pack_hash_from` for the argument.
///   2. Endianness and packing were unstated, as they were for the pack.
///      Little-endian, fields serialized one at a time. `src/lib/bytes.hpp`.
///   3. FOUR FIELDS ARE ADDED to §3's six: `file_sha256`, `final_world_hash`,
///      `record_count` and `total_bytes`. The first, third and fourth are the
///      pack's own header fields doing the pack's own job, and need no separate
///      defence. `final_world_hash` is the one that changes what the artifact
///      IS — see below.
///   4. A record is SEVEN bytes, not eight. §3 says "packed" and this format
///      writes fields one at a time, so there is no struct to align and no
///      padding that could reach the digest. Honouring the word costs nothing.
///
///
/// WHY THE FILE CARRIES THE ANSWER IT IS SUPPOSED TO REPRODUCE
///
/// `TEST-PLAN.md` §2 defines a golden replay as "`seed + input log -> world
/// hash at tick N`", and then says "keep at least one replay per bug ever
/// filed". Those two sentences only compose if the file holds all three parts.
/// A `.gloam` file that carries just the seed and the inputs is HALF a
/// regression test: the other half lives in whatever test source happened to
/// record it, and a bug report mailed in from outside arrives with no way to
/// say what it was supposed to do. `final_world_hash` is what makes the
/// artifact self-checking, and `play()` comparing against it is the whole
/// harness.
///
///
/// WHAT IS REFUSED, AND THE ONE THING THAT IS NOT
///
/// Everything structural is a refusal, and so is `ruleset_hash` — §12 is
/// explicit that it is not a warning. `pack_hash` is the single exception:
/// SCHEMAS.md §3 says "art changes do not affect simulation, so a mismatch
/// warns rather than rejects". A warning is not an error, so it is reported as
/// a flag on a SUCCESSFUL result and never as a `ReplayError`. Squeezing it
/// into the error enum would force every caller to decide which errors are
/// really errors, which is how an advisory becomes a rejection by accident.
///
/// Nothing here opens a file. Every entry point is `(caller-owned span,
/// integers) -> ReplayResult`; `src/bin/replay.cpp` owns the buffers and holds
/// the only `read` and `write`.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "gloam/level.hpp"
#include "gloam/sha256.hpp"
#include "gloam/tuning.hpp"

namespace gloam::replay {

/// §3: `magic:"GLRP"`.
inline constexpr std::array<char, 4> kMagic{'G', 'L', 'R', 'P'};
inline constexpr std::uint16_t kVersion = 1;

/// §5.2's real-time row is `set_tick_hz(10)`, and `budgets.hpp`'s simulation
/// budget is "≤4 ms at 10 Hz". The field exists so a replay recorded at another
/// rate is self-describing rather than silently mis-timed; nothing writes
/// anything else today.
inline constexpr std::uint16_t kTickHz = 10;

inline constexpr std::size_t kHeaderBytes = 104;

/// §3: "`record { tick:u32, event:u8, payload:u16 }` // packed, ordered by
/// tick". Seven bytes, and the word "packed" is taken at face value.
inline constexpr std::size_t kRecordBytes = 7;

/// Where `file_sha256` sits, and what it covers: `[kDigestCoverageStart, EOF)`.
///
/// The placement is `pack.hpp`'s and so is the reason: magic and version sit
/// OUTSIDE the digest deliberately, because you have to read them to know
/// whether the rest of the file has the shape you are about to hash, and a
/// digest you cannot check without first trusting the file is not a check.
inline constexpr std::size_t kDigestOffset = 8;
inline constexpr std::size_t kDigestCoverageStart = 40;

/// The largest tick a record may carry.
///
/// A BOUND ON WORK, NOT ON TASTE. `play` reaches a record's tick by advancing
/// the simulation one tick at a time, so `tick` is the field that decides how
/// much CPU a file costs — and it is a `u32`, so without a ceiling a
/// well-formed 111-byte replay whose one record sits at 0xFFFFFFFF costs 4.29
/// billion full-level ticks. Measured at roughly 3.3M ticks/s on this hardware,
/// that is about twenty minutes of a pinned core for a file that passes every
/// other check in this header, including the digest. The harness is documented
/// to accept files "mailed in from outside"; one that hangs CI rather than
/// failing it is worse than one that is rejected.
///
/// Ten million ticks is ~11.5 days of game time at 10 Hz, which is far past any
/// session anyone will record and still bounds a hostile file to a few seconds.
inline constexpr std::uint32_t kMaxTick = 10'000'000;

/// "No pack was loaded when this was recorded" — which is every replay until
/// the compositor exists (#7), because there is nothing to upload plates to.
/// Suppresses the advisory rather than warning on every sim-only replay, which
/// is how a warning becomes noise and then becomes ignored.
///
/// A real digest whose first eight bytes are all zero collides with this at
/// 2^-64. That is documented rather than defended: the field only ever warns.
inline constexpr std::uint64_t kNoPackHash = 0;

/// The manifest's `pack_sha256` truncated to §3's `pack_hash:u64`.
///
/// THE FIRST EIGHT BYTES, in the order they appear on disk. SCHEMAS.md carries
/// both widths and never says how one becomes the other, so this is the answer:
/// the eight bytes this returns, written little-endian by `write_header`, are
/// byte-for-byte the eight bytes at `pack.gloam` offset 8. You can hold the two
/// files up against each other in a hex dump and see it.
///
/// That property is the entire argument. A fold, a re-hash, or the LAST eight
/// bytes would all be equally uniform, and none of them would let someone
/// looking at a warning they did not expect confirm by eye which of the two
/// files is the surprising one. For a field that only ever warns, being
/// checkable by hand beats every other consideration.
[[nodiscard]] constexpr auto pack_hash_from(const hash::Digest& pack_sha256) -> std::uint64_t {
  std::uint64_t v = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    v |= static_cast<std::uint64_t>(pack_sha256[i]) << (8 * i);
  }
  return v;
}

/// What the player did. §5.1's "event_type".
///
/// APPEND-ONLY, AND NEVER RENUMBERED — the same contract `rng::Stream` carries
/// and for the same reason. These values are written into files that are kept
/// "per bug ever filed"; renumbering one silently re-interprets every replay
/// recorded before the change, and the symptom is a golden hash that moved for
/// reasons nobody can reconstruct.
enum class Event : std::uint8_t {
  /// Reserved, and REFUSED at load. A run of zero bytes is the most likely
  /// shape of a corrupt or truncated file, and it must not parse as a valid
  /// sequence of no-ops.
  None = 0,
  Step = 1,   ///< payload: a `Dir`, 0-3
  Turn = 2,   ///< payload: the `Dir` now faced, 0-3
  Lamp = 3,   ///< payload: the new lamp level, 0-5 (§4.4, `kLampLevelMax`)
  Creep = 4,  ///< payload: 0 walking, 1 creeping (§6.2)
  Wait = 5,   ///< payload: must be 0
};
inline constexpr std::uint8_t kEventMax = 5;

/// Is `payload` meaningful for this event?
///
/// An out-of-range payload is REFUSED, never clamped. A clamped replay plays a
/// session nobody recorded, reaches a world hash nobody predicted, and reports
/// it as a determinism regression.
[[nodiscard]] constexpr auto payload_valid(Event event, std::uint16_t payload) -> bool {
  switch (event) {
    case Event::None: return false;
    case Event::Step:
    case Event::Turn: return payload < kDirCount;
    case Event::Lamp: return payload <= static_cast<std::uint16_t>(kLampLevelMax);
    case Event::Creep: return payload <= 1;
    case Event::Wait: return payload == 0;
  }
  return false;
}

/// One input. Seven bytes on disk, in this field order.
struct Record {
  std::uint32_t tick{0};
  Event event{Event::None};
  std::uint16_t payload{0};

  [[nodiscard]] auto operator==(const Record&) const -> bool = default;
};

struct Header {
  std::uint16_t version{kVersion};
  std::uint16_t tick_hz{kTickHz};
  hash::Digest file_sha256{};
  std::uint64_t seed{0};
  std::uint64_t ruleset_hash{0};
  std::uint64_t pack_hash{kNoPackHash};
  hash::Digest final_world_hash{};
  std::uint32_t record_count{0};
  std::uint32_t total_bytes{0};
};

/// What a replay must agree with before it is allowed to run.
///
/// Passed by value into `load`, which is what makes the §12 rejection
/// unskippable: there is no way to reach the records without stating what you
/// expected them to have been recorded against.
struct Expect {
  /// `gloam::ruleset_hash(tuning)`. A mismatch is a REFUSAL (§12).
  std::uint64_t ruleset_hash{0};
  /// `pack_hash_from(manifest.pack_sha256)`, or `kNoPackHash` when no pack is
  /// loaded. A mismatch is ADVISORY (SCHEMAS.md §3).
  std::uint64_t pack_hash{kNoPackHash};
};

/// Why a replay was refused. `None` is success.
enum class ReplayError : std::uint8_t {
  None = 0,
  BadMagic = 1,
  UnsupportedVersion = 2,
  Truncated = 3,           ///< fewer bytes present than the header claims
  BufferTooSmall = 4,      ///< an output span too small to hold what was asked for
  ZeroTickHz = 5,          ///< a replay that advances no ticks per second plays forever
  ZeroRecords = 6,         ///< an empty input log records nothing; say so rather than replay it
  ReservedEvent = 7,       ///< `Event::None` — see its doc comment
  UnknownEvent = 8,        ///< an event value this version has never heard of
  PayloadOutOfRange = 9,   ///< valid event, payload it cannot mean
  TicksOutOfOrder = 10,    ///< §3: "ordered by tick"; equal is fine, decreasing is not
  TickOutOfRange = 16,     ///< past `kMaxTick` — see the bound-on-work note there
  RecordCountMismatch = 11,   ///< record_count disagrees with the bytes present
  TotalBytesMismatch = 12,    ///< total_bytes disagrees with the bytes present
  FileDigestMismatch = 13,    ///< `file_sha256` over [40, EOF) does not match
  RulesetMismatch = 14,       ///< §12's refusal — the one that must never be a warning
  /// The replay ran and did not reproduce `final_world_hash`. The only value
  /// here the codec never returns: it belongs to whoever ran the simulation,
  /// which is `src/bin/replay.cpp` and the harness. It lives in this enum
  /// anyway so that a replay failure has one vocabulary rather than two.
  WorldHashMismatch = 15,
};

/// The result of every entry point in this header.
///
/// `pack_hash_mismatch` is deliberately NOT a `ReplayError`. See the header
/// comment: an advisory that lives in the error enum is an advisory one
/// `if (!result)` away from being a rejection.
struct ReplayResult {
  ReplayError error{ReplayError::None};
  std::size_t bytes{0};          ///< bytes written or consumed; 0 on every error
  std::uint32_t record_index{0}; ///< which record failed, for the per-record errors
  bool pack_hash_mismatch{false};

  [[nodiscard]] constexpr explicit operator bool() const { return error == ReplayError::None; }
};

/// A human-readable cause, for `src/bin/replay.cpp`'s diagnostics. Never null.
[[nodiscard]] auto describe(ReplayError error) -> const char*;

/// The size `assemble` will need for `record_count` records.
[[nodiscard]] constexpr auto image_bytes(std::uint32_t record_count) -> std::size_t {
  return kHeaderBytes + kRecordBytes * static_cast<std::size_t>(record_count);
}

// ── Field-at-a-time codecs ─────────────────────────────────────────────────

[[nodiscard]] auto write_header(std::span<std::byte> out, const Header& header) -> ReplayResult;
[[nodiscard]] auto write_record(std::span<std::byte> out, const Record& record) -> ReplayResult;

/// Parse the header of a WHOLE replay image.
///
/// `in` MUST be the entire file, not just its first 104 bytes, because this
/// checks `total_bytes` against `in.size()` — and that check is the only thing
/// standing between `record_count` and a caller who is about to size an
/// allocation with it. `record_count` and `total_bytes` are both read from the
/// file and only agree with each other; a 104-byte image can claim 613 million
/// records and satisfy that identity perfectly. Sizing a buffer from the result
/// of a call that has not seen the whole file is how this parser turns a
/// truncated download into a 4.9 GB allocation and a SIGABRT.
[[nodiscard]] auto read_header(std::span<const std::byte> in, Header& out) -> ReplayResult;
[[nodiscard]] auto read_record(std::span<const std::byte> in, Record& out) -> ReplayResult;

/// Build a whole replay image into `out`.
///
/// The caller supplies the descriptive header fields — seed, ruleset hash, pack
/// hash, tick rate, final world hash — and `assemble` fills `record_count`,
/// `total_bytes` and `file_sha256`, overwriting whatever was there. Same
/// division as `pack::assemble`, and for the same reason: nothing a caller can
/// get subtly wrong is allowed to reach the file.
[[nodiscard]] auto assemble(Header& header, std::span<const Record> records,
                            std::span<std::byte> out) -> ReplayResult;

/// Everything intrinsic to the file: magic, version, tick rate, counts, the
/// digest, every event and payload, and tick ordering.
///
/// THIS IS NOT THE LOAD GATE. It cannot be — it has nothing to compare
/// `ruleset_hash` against, and §12's whole point is that the comparison is not
/// optional. `load` is the entry point; this exists for `gloam_replay verify`,
/// which answers "is this file intact" and deliberately not "may I run it".
[[nodiscard]] auto verify(std::span<const std::byte> image) -> ReplayResult;

/// The load gate (§12). The structure, then the expectations, then the records.
///
/// `records` must hold at least `header.record_count` entries — read the header
/// first to size it, which is safe because `read_header` has seen the whole
/// file by then.
///
/// `header` is written only on success. `records` IS FILLED PROGRESSIVELY and
/// is meaningful ONLY when the call succeeds — a rejected load may have written
/// some prefix of it. Said plainly here rather than promised and left to an
/// argument about which checks run in which order: the guarantee callers
/// actually get is the one worth writing down.
[[nodiscard]] auto load(std::span<const std::byte> image, Expect expect, Header& header,
                        std::span<Record> records) -> ReplayResult;

}  // namespace gloam::replay
