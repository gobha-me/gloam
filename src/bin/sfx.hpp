#pragma once

/// SPEC §9.2 — the resident PCM arena.
///
/// > All PCM is decoded at startup into the pack's resident audio arena. No
/// > decoding on the callback thread; the audio pack shares §10's manifest hash.
///
/// Half of that is met here and half is not, deliberately. The arena is
/// resident, filled once before the stream opens, and never touched by the
/// callback thread except to read. It does NOT live in the pack and is not
/// covered by `pack_sha256`: `SCHEMAS.md` §1 has no role and no record shape for
/// audio — `role` is `wall|floor|ceiling|light_field|monster|item|ui|rune` and
/// the record is plate-shaped, with nowhere to put a sample rate, a channel
/// count or a frame count. That gap is gloam#23, and UPSTREAM.md item 10 named
/// synthesis-at-startup as the interim position before this file existed.
///
/// WHY THIS IS IN src/bin/ AND NOT IN gloam::lib
///
/// Two independent reasons, either of which would be enough. It produces
/// `float`, and AGENTS.md rule 2 keeps floating point out of the simulation. And
/// the arena is 153,600 B of storage; `gloam::lib` owns no buffers — the
/// precedent is `deflate::Scratch`, "a quarter of a megabyte of match tables and
/// it belongs to whoever calls the compressor, which is `src/bin/`".
///
/// It includes no RtAudio and reads no clock, which is what lets
/// `test/27sfxarena/` compile it directly under all eight sanitizer legs on a
/// machine with no sound card — which is every machine this project builds on.
///
/// DETERMINISM, AND THE TWO RULES THAT BUY IT
///
/// The arena must be bit-identical from the same seed on every target, or the
/// determinism case in `test/27sfxarena/` is a cross-machine flake rather than a
/// test. Two rules get that, and both are the same argument `audio.hpp` makes
/// for `kGainUnity` being a power of two:
///
///  1. Samples are built as 16-bit integers and scaled by `1.0f / 32768.0f`.
///     Every `std::int16_t` is exactly representable in `float` and the divisor
///     is a power of two, so the conversion is exact on every IEEE-754 target.
///     Widening to 32 bits would round above 2^24 and make the result depend on
///     the rounding mode.
///  2. NOTHING HERE INCLUDES <cmath>. `sinf`, `expf` and `powf` are not required
///     to be correctly rounded and do vary between libm versions, so an arena
///     built on them would differ between a GCC and a Clang leg for reasons that
///     have nothing to do with GLOAM. Envelopes are integer, and the oscillator
///     reads a `constexpr` integer sine table built by a rational approximation.
///
/// WHAT §9 DOES NOT SAY, AND THIS FILE THEREFORE DECIDES
///
/// §9 names sounds — "Monster footfalls, door noise and distance attenuation" —
/// and DESCRIBES none of them. It never enumerates the three `SoundId`s this
/// file fills, and it never mentions a party footfall at all. §10's asset table
/// gives a pipeline — "Normalise, trim, tag with an attenuation class, decode to
/// raw PCM into the pack" — and no content. So the durations below, the choice
/// of noise for footfalls and tone for the sting, and the fact that the
/// monster's footfall differs from yours by TIMBRE are decisions taken here
/// rather than read. See UPSTREAM.md's "Corrections to the design document" and
/// gloam#40.
///
/// The timbre point is the load-bearing one. `audio.hpp` gives a monster's
/// footfall `kMonsterFootfallEmission = 14`, the same leather-step loudness as
/// your own, deliberately — so at equal distance the two are equally loud and
/// GAIN CANNOT TELL THEM APART. If they shared a waveform, §9's promise that you
/// can hear the corridor would be a promise you could not act on.

#include <cstddef>
#include <cstdint>
#include <span>

#include "gloam/audio.hpp"

namespace gloam::sfx {

/// §9.2's stream format. The device may be granted something else — see
/// `audio_device.hpp` — but the arena is synthesised at this rate and these are
/// the numbers §11's latency arithmetic is written against.
inline constexpr int kSampleRateHz = 48'000;
inline constexpr int kBufferFrames = 256;
inline constexpr int kChannels = 2;

/// One sound's extent in the arena.
///
/// MONO. The mixer pans on the way out, because pan is a per-command value read
/// off the ring (`audio::Command::pan`) and a stereo clip would have to be
/// re-panned per voice anyway — at twice the resident cost and with the source
/// material already committed to a position it does not know.
struct Clip {
  std::uint32_t offset{0};
  std::uint32_t frames{0};  ///< 0 means "no sound", which is `SoundId::None`
};

/// Durations. Decided here, not read from §9 — see the header note.
///
/// The two footfalls differ in length as well as timbre, but length is NOT what
/// distinguishes them: a listener hears the difference in the tail, and the
/// spectral-tilt case in `test/27sfxarena/` is what keeps that true if someone
/// retunes the filters.
inline constexpr std::uint32_t kPartyFootfallFrames = 4'320;    ///< 90 ms
inline constexpr std::uint32_t kMonsterFootfallFrames = 5'280;  ///< 110 ms
inline constexpr std::uint32_t kHuntingStingFrames = 28'800;    ///< 600 ms

inline constexpr std::size_t kArenaFrames =
    std::size_t{kPartyFootfallFrames} + kMonsterFootfallFrames + kHuntingStingFrames;

/// 153,600 B resident, allocated once by `main` and never again.
inline constexpr std::size_t kArenaBytes = kArenaFrames * sizeof(float);

/// How many `Rng::next()` draws `synthesise` takes, in total.
///
/// Pinned as a constant so `test/27sfxarena/` can assert that the Ambience
/// stream advanced by exactly this much and no other stream advanced at all.
/// One draw per noise sample; the sting's tonal body takes none.
inline constexpr std::uint32_t kNoiseDraws =
    kPartyFootfallFrames + kMonsterFootfallFrames + 480U;

/// Fill a CALLER-OWNED arena and the clip table.
///
/// Deterministic in `seed` and in nothing else — no clock, no global state, no
/// allocation, no libm. Draws exclusively from `Stream::Ambience`, which
/// `world.cpp` excludes from `world_hash`, so retuning what a sting sounds like
/// can never surface as a determinism regression.
///
/// Returns false, having written nothing, when `arena` is shorter than
/// `kArenaFrames`. Refused rather than clamped: a short arena means the caller
/// disagrees with this header about a compile-time constant, and truncating the
/// sting to be helpful would turn that into a sound bug nobody traces back here.
[[nodiscard]] auto synthesise(std::uint64_t seed, std::span<float> arena,
                              std::span<Clip, audio::kSoundIdCount> clips) -> bool;

}  // namespace gloam::sfx
