#pragma once

/// SPEC §9.2, §10 — the pack's audio arena, loaded rather than synthesised
/// (gloam#23, the runtime half).
///
/// > All PCM is decoded at startup into the pack's resident audio arena. No
/// > decoding on the callback thread; the audio pack shares §10's manifest hash.
///
/// This is the half of that quote `sfx.hpp` long deferred: the arena now comes
/// OUT of the pack, where the bake put it, and every sample is covered by
/// `pack_sha256`. What "decode" means at runtime is exactly one thing — the
/// s16le blob of each audio record, expanded to float by a multiply with
/// `1.0f / 32768.0f`. That expansion is EXACT (every `std::int16_t` is
/// representable in `float`; the divisor is a power of two), so the loaded
/// arena is bit-identical to the synthesis the bake ran — `test/27sfxarena/`
/// pins that by memcmp — and no `sinf`, no clock and no seed reaches this
/// file at all.
///
/// WHY THIS IS IN src/bin/ AND NOT IN gloam::lib
///
/// `sfx.hpp`'s two reasons, unchanged. It produces `float`, and AGENTS.md rule
/// 2 keeps floating point out of the simulation. And it owns no storage — the
/// arena is the caller's, the way `deflate::Scratch` is the caller's. It
/// includes no RtAudio and reads no clock, so `test/37packaudio/` compiles it
/// directly, the way `test/27sfxarena/` compiles `sfx.cpp` — the load path
/// sits under all eight sanitizer legs on machines with no sound card, which
/// is every machine this project builds on.
///
/// THE TWO CALLS, AND WHY THE GATE IS NOT THE LOAD
///
/// `check` is a records-only pass: it never touches a float and never needs
/// the arena, so §9.2's launch gate can run it ALWAYS — muted or not — before
/// any terminal entry. §9.2's degradation clause covers a missing audio
/// DEVICE, never missing audio CONTENT: a pack without its sounds refuses to
/// launch, and `--mute` mutes output, not integrity. `load` is `check` plus
/// the expansion, and runs only when something will read the arena.
///
/// THE LAYOUT THE LOAD PRODUCES
///
/// Clips land at cumulative offsets in RECORD order — which, `verify` having
/// passed, is id order: party @ 0, sting @ 4320, monster @ 33120. That is NOT
/// the synthesis layout (party, monster, sting — generation order, a
/// historical accident `sfx.hpp` names). Nothing outside the clips table can
/// observe the difference: the table is the only map anyone reads, and it is
/// filled here from the records themselves. An end-to-end comparison against
/// the synthesised arena must therefore go per-clip, never per-arena-byte.
///
/// WHAT THE RECORD DECIDES, AND WHAT IT DOES NOT
///
/// `frame_count` is read from the RECORD, never pinned against
/// `assets::kAudioInventory`: the pack is self-describing, a retuned sting is
/// a re-bake rather than a rebuild, and the record's own number is the truth
/// about what the bytes hold. What the record may NOT redescribe is the
/// stream's shape — the arena the mixer reads is mono float at
/// `audio::kSampleRateHz`, so a record claiming another rate, another channel
/// count or a zero frame count is refused (`UnexpectedFormat`), not
/// reinterpreted.
///
/// THE VERIFY ASSUMPTION, STATED
///
/// Every caller runs this AFTER `pack::verify` — m0 reaches it through
/// `resident::PlateSet::from_pack`, whose first act is the verify. So
/// structural horrors (a blob out of range, a digest that disagrees) are
/// already excluded, and what remains here is the SEMANTIC half verify cannot
/// know: that the audio run covers the SoundIds this binary was built with.
/// The reads still go through `read_header`/`read_record`'s own checks and
/// every subspan is bounded first — a gate that trusts its predecessor
/// blindly is one refactor away from being a hole.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "gloam/audio.hpp"
#include "gloam/pack.hpp"
#include "sfx.hpp"

namespace gloam::pack_audio {

/// Why an image's audio run was refused. `None` is success.
enum class LoadError : std::uint8_t {
  None = 0,
  MissingAudio = 1,   ///< no audio run, or a SoundId in [1, kSoundIdCount) with no record
  UnknownSound = 2,   ///< an audio record whose id is not a SoundId this binary knows
  DuplicateSound = 3, ///< two records for one SoundId — verify's ordering refuses this first
  UnexpectedFormat = 4,  ///< not mono s16le at kSampleRateHz, or zero frames
  ArenaTooSmall = 5,  ///< the caller's arena holds fewer frames than the records carry
  Malformed = 6,      ///< the header or a record refused its own read — see `pack_error`
};

/// One refusal, or one success. Mirrors `pack::PackResult`'s shape: the error
/// is the verdict, the rest names the smallest thing that is actually wrong.
struct LoadResult {
  LoadError error{LoadError::None};
  std::uint16_t sound_id{0};  ///< the record the failure names; 0 implicates none
  std::size_t frames{0};      ///< the audio run's total frames — the live arena extent
  pack::PackError pack_error{pack::PackError::None};  ///< `Malformed`: the parser's own word

  [[nodiscard]] constexpr explicit operator bool() const { return error == LoadError::None; }
};

/// The enumerator's own spelling, for stderr lines a player can quote. A
/// gate message that paraphrases the error drifts from it; this cannot.
[[nodiscard]] auto name(LoadError error) -> std::string_view;

/// §9.2's launch gate for CONTENT: presence and coverage, records only.
///
/// Answers the one question m0 asks before entering the terminal — does this
/// pack carry exactly one usable record for every `SoundId` in
/// [1, `audio::kSoundIdCount`)? — without needing the float arena to exist.
/// On success `frames` is the arena extent the records describe, which for the
/// shipped pack equals `assets::kAudioArenaFrames`.
[[nodiscard]] auto check(std::span<const std::byte> image) -> LoadResult;

/// The gate plus the load: `check`, then the s16le blobs expanded into
/// `arena` and the clips table filled, at cumulative offsets in record (id)
/// order — see the header's layout note.
///
/// `clips[SoundId::None]` is reset to `{0, 0}` explicitly: `None` gets no
/// record (silence has no samples), and a caller's stale entry there would
/// otherwise survive the load.
///
/// Refuses `ArenaTooSmall` — having written nothing — when `arena` is shorter
/// than the records' total frames. Refused rather than clamped, for
/// `sfx::synthesise`'s reason: a short arena means the binary and the pack
/// disagree about a constant, and truncating the sting to be helpful would
/// turn that into a sound bug nobody traces back here.
[[nodiscard]] auto load(std::span<const std::byte> image, std::span<float> arena,
                        std::span<sfx::Clip, audio::kSoundIdCount> clips) -> LoadResult;

}  // namespace gloam::pack_audio
