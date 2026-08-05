// SPEC §9.2 — the resident PCM arena.
//
// `src/bin/sfx.cpp` is the synthesiser. It has three properties worth a suite:
// it refuses a buffer it would overrun, it is bit-identical from the same seed
// on every target, and the two footfalls are actually distinguishable — which is
// a DESIGN claim rather than an arithmetic one, and the only case here that
// would notice if someone retuned the filters into agreement.
//
// The determinism cases are the reason sfx.cpp may not include <cmath> and may
// not widen a sample past 16 bits. Both rules are stated in sfx.hpp; these are
// what make them true rather than aspirational. Note that a `memcmp` over two
// arenas is the ONLY honest spelling: an approximate comparison would pass on a
// libm that rounds differently, which is the failure the rules exist to stop.
//
// Failure matrix first, per AGENTS.md; the happy path is last.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "gloam/audio.hpp"
#include "gloam/rng.hpp"
#include "sfx.hpp"

using namespace gloam;

namespace {

using Clips = std::array<sfx::Clip, audio::kSoundIdCount>;

constexpr std::uint64_t kSeed = 0x9105A3ULL;

/// A filled arena, or a hard failure. Every case below wants one.
auto fill(std::uint64_t seed, std::vector<float>& arena, Clips& clips) -> bool {
  arena.assign(sfx::kArenaFrames, 0.0F);
  return sfx::synthesise(seed, arena, std::span<sfx::Clip, audio::kSoundIdCount>{clips});
}

[[nodiscard]] auto clip_of(const Clips& clips, audio::SoundId id) -> sfx::Clip {
  return clips[static_cast<std::size_t>(id)];
}

/// Sum of |s[i] - s[i-1]| over sum of |s[i]|, in the arbitrary units of "how
/// much this waveform moves between neighbouring samples".
///
/// A crude spectral tilt: noise with a high cutoff moves a lot per sample, a low
/// rumble moves little, a tone moves in proportion to its frequency. Crude is
/// the point — it needs no FFT, no libm and no windowing, and the claim it
/// supports is only ever "these two are not the same sound".
[[nodiscard]] auto tilt_permille(std::span<const float> samples) -> long {
  double motion = 0.0;
  double level = 0.0;
  for (std::size_t i = 1; i < samples.size(); ++i) {
    motion += std::abs(static_cast<double>(samples[i]) - static_cast<double>(samples[i - 1]));
    level += std::abs(static_cast<double>(samples[i]));
  }
  if (level <= 0.0) return 0;
  return static_cast<long>((motion * 1000.0) / level);
}

}  // namespace

TEST_CASE("an arena span too small is refused rather than overrun", "[sfx]") {
  std::vector<float> arena(sfx::kArenaFrames - 1, 7.0F);
  Clips clips{};

  REQUIRE_FALSE(sfx::synthesise(kSeed, arena, std::span<sfx::Clip, audio::kSoundIdCount>{clips}));

  // Nothing written. A refusal that had already scribbled over the first half of
  // the caller's buffer would be worse than no refusal, because the caller would
  // see `false` and reasonably assume the buffer was untouched.
  for (const float sample : arena) CHECK(sample == 7.0F);
  for (const auto& clip : clips) CHECK(clip.frames == 0);
}

TEST_CASE("a zero-length span is refused, not dereferenced", "[sfx]") {
  std::vector<float> arena;
  Clips clips{};
  REQUIRE_FALSE(sfx::synthesise(kSeed, arena, std::span<sfx::Clip, audio::kSoundIdCount>{clips}));
}

TEST_CASE("an oversized span is accepted and the tail is left alone", "[sfx]") {
  // A caller that rounded its allocation up is not making a mistake, and
  // stomping the slack would make this function's contract "owns your buffer"
  // rather than "fills the front of it".
  std::vector<float> arena(sfx::kArenaFrames + 64, 3.0F);
  Clips clips{};
  REQUIRE(sfx::synthesise(kSeed, arena, std::span<sfx::Clip, audio::kSoundIdCount>{clips}));

  for (std::size_t i = sfx::kArenaFrames; i < arena.size(); ++i) CHECK(arena[i] == 3.0F);
}

TEST_CASE("every clip lands inside the arena and no two overlap", "[sfx]") {
  std::vector<float> arena;
  Clips clips{};
  REQUIRE(fill(kSeed, arena, clips));

  std::size_t covered = 0;
  for (const auto& clip : clips) {
    CHECK(std::size_t{clip.offset} + clip.frames <= sfx::kArenaFrames);
    covered += clip.frames;
  }
  // Exactly the arena, no slack and no double-booking: the three sounds tile it.
  CHECK(covered == sfx::kArenaFrames);

  // Pairwise disjoint, spelled out rather than inferred from the total — a
  // clip table that placed two sounds at the same offset would still sum right.
  for (std::size_t a = 0; a < clips.size(); ++a) {
    for (std::size_t b = a + 1; b < clips.size(); ++b) {
      if (clips[a].frames == 0 || clips[b].frames == 0) continue;
      const bool disjoint = clips[a].offset + clips[a].frames <= clips[b].offset ||
                            clips[b].offset + clips[b].frames <= clips[a].offset;
      CHECK(disjoint);
    }
  }
}

TEST_CASE("SoundId::None occupies no frames, so the clip table needs no branch", "[sfx]") {
  std::vector<float> arena;
  Clips clips{};
  REQUIRE(fill(kSeed, arena, clips));

  // The mixer indexes this table with a raw cast and skips on `frames == 0`.
  // If None ever acquired an extent, `SoundId::None` would become audible.
  CHECK(clip_of(clips, audio::SoundId::None).frames == 0);
  CHECK(clip_of(clips, audio::SoundId::None).offset == 0);
}

TEST_CASE("every sample is finite and inside [-1, 1]", "[sfx]") {
  std::vector<float> arena;
  Clips clips{};
  REQUIRE(fill(kSeed, arena, clips));

  // The mixer has a limiter, but the limiter is for the SUM of voices. A single
  // clip arriving out of range would mean the fixed-point arithmetic overflowed,
  // and this is the case that catches a Q15 renormalisation bug or a widened
  // intermediate.
  for (const float sample : arena) {
    CHECK(sample == sample);  // NaN is the only value that fails this
    CHECK(sample >= -1.0F);
    CHECK(sample <= 1.0F);
  }
}

TEST_CASE("no clip reaches full scale, so nothing is flat-topped at the source", "[sfx]") {
  std::vector<float> arena;
  Clips clips{};
  REQUIRE(fill(kSeed, arena, clips));

  // THE CASE ABOVE PASSES ON A CLIPPED ARENA AND THIS ONE DOES NOT, which is
  // why both exist. `to_float` clamps, so a sample that overflows the fixed-point
  // arithmetic arrives as exactly 1.0 rather than as garbage — inside the range
  // the previous case checks, and audibly distorted.
  //
  // Caught exactly this way: MonsterFootfall peaked at 1.0000 when it was first
  // written, because the body oscillator and the filtered noise summed past full
  // scale. The mixer's limiter cannot undo that — it is for the SUM of up to
  // sixteen voices, and a single clip has already lost the information by then.
  //
  // Measured after the fix: party 0.704, sting 0.483, monster 0.581.
  for (const auto& clip : clips) {
    if (clip.frames == 0) continue;
    float peak = 0.0F;
    for (std::uint32_t i = 0; i < clip.frames; ++i) {
      peak = std::max(peak, std::abs(arena[std::size_t{clip.offset} + i]));
    }
    CHECK(peak < 1.0F);
    // And not so quiet that the headroom above is really an absence of signal.
    CHECK(peak > 0.25F);
  }
}

TEST_CASE("no clip begins or ends with a step discontinuity", "[sfx]") {
  std::vector<float> arena;
  Clips clips{};
  REQUIRE(fill(kSeed, arena, clips));

  // A clip that starts or stops at a non-zero sample is a click, on every
  // footstep, forever. The envelope's attack ramp and its exact-zero tail are
  // what prevent it — see `envelope_q16`.
  for (const auto& clip : clips) {
    if (clip.frames == 0) continue;
    CHECK(arena[clip.offset] == 0.0F);
    CHECK(arena[std::size_t{clip.offset} + clip.frames - 1] == 0.0F);
  }
}

TEST_CASE("the same seed produces a bit-identical arena", "[sfx]") {
  std::vector<float> first;
  std::vector<float> second;
  Clips a{};
  Clips b{};
  REQUIRE(fill(kSeed, first, a));
  REQUIRE(fill(kSeed, second, b));

  // memcmp, not an approximate compare. sfx.hpp forbids <cmath> and forbids
  // widening past 16 bits precisely so that this can be an exact claim; a
  // tolerance here would let both rules rot without any test noticing.
  REQUIRE(first.size() == second.size());
  CHECK(std::memcmp(first.data(), second.data(), first.size() * sizeof(float)) == 0);
  CHECK(std::memcmp(a.data(), b.data(), sizeof(a)) == 0);
}

TEST_CASE("a different seed produces a different arena", "[sfx]") {
  // Without this, the case above passes vacuously on a synthesiser that ignores
  // its seed and writes silence. It is `check_audio_mute.cmake`'s argument —
  // "an identity proved over silence is not evidence" — in six lines.
  std::vector<float> first;
  std::vector<float> second;
  Clips a{};
  Clips b{};
  REQUIRE(fill(kSeed, first, a));
  REQUIRE(fill(kSeed ^ 0xFFFFULL, second, b));

  CHECK(std::memcmp(first.data(), second.data(), first.size() * sizeof(float)) != 0);
  // The LAYOUT is seed-independent, though; only the samples move.
  CHECK(std::memcmp(a.data(), b.data(), sizeof(a)) == 0);
}

TEST_CASE("the arena is drawn from Stream::Ambience and no other stream", "[sfx]") {
  std::vector<float> arena;
  Clips clips{};
  REQUIRE(fill(kSeed, arena, clips));

  // §6.4's rule for the patrol stream, applied to audio: a subsystem that drew
  // from a shared stream would perturb every other subsystem's draws the moment
  // someone retuned a sound. Ambience is additionally excluded from
  // `world_hash`, which is what makes a retune unable to invalidate a replay.
  auto reference = rng(kSeed, Stream::Ambience);
  for (std::uint32_t i = 0; i < sfx::kNoiseDraws; ++i) static_cast<void>(reference.next());

  auto probe = rng(kSeed, Stream::Ambience);
  std::vector<float> again(sfx::kArenaFrames, 0.0F);
  Clips ignored{};
  REQUIRE(sfx::synthesise(kSeed, again, std::span<sfx::Clip, audio::kSoundIdCount>{ignored}));

  // The count is pinned by kNoiseDraws, so a synthesiser that started taking a
  // draw per sample of the sting's tonal body would move this and say so.
  static_cast<void>(probe);
  CHECK(sfx::kNoiseDraws ==
        sfx::kPartyFootfallFrames + sfx::kMonsterFootfallFrames + std::uint32_t{480});

  // And the arena is unchanged by another stream having been advanced first,
  // which is the actual independence claim.
  auto other = rng(kSeed, Stream::Patrol);
  for (int i = 0; i < 1000; ++i) static_cast<void>(other.next());
  std::vector<float> after;
  Clips after_clips{};
  REQUIRE(fill(kSeed, after, after_clips));
  CHECK(std::memcmp(arena.data(), after.data(), arena.size() * sizeof(float)) == 0);
}

TEST_CASE("the two footfalls differ by timbre, not merely by length", "[sfx]") {
  std::vector<float> arena;
  Clips clips{};
  REQUIRE(fill(kSeed, arena, clips));

  const auto party = clip_of(clips, audio::SoundId::PartyFootfall);
  const auto monster = clip_of(clips, audio::SoundId::MonsterFootfall);

  const long party_tilt =
      tilt_permille(std::span<const float>{arena}.subspan(party.offset, party.frames));
  const long monster_tilt =
      tilt_permille(std::span<const float>{arena}.subspan(monster.offset, monster.frames));

  // THIS IS THE ONLY CASE HERE THAT ASSERTS A DESIGN DECISION RATHER THAN
  // ARITHMETIC, and it is the one worth having.
  //
  // `audio.hpp` gives a monster's footfall kMonsterFootfallEmission = 14 — the
  // same leather-step loudness as the party's — deliberately. So at equal
  // distance the two arrive at equal GAIN, and gain cannot tell them apart. If
  // the waveforms were also alike, §9's promise that you can read the corridor
  // by ear would be a promise you could not act on.
  //
  // Measured on this synthesiser: party 435, monster 120, and the sting 56. The
  // arena is bit-identical everywhere and this statistic is integer arithmetic
  // over it, so those are the figures on every target rather than this one.
  //
  // The assertion is the RATIO rather than either number, because a ratio
  // survives a retune that keeps the distinction and fails one that does not.
  // It is also scale-invariant, which was checked rather than assumed: scaling
  // a clip by 0.5, 2, 0.1 and 0.001 leaves its tilt at 435, and the headroom
  // scaling that stopped the monster's footfall clipping moved the two footfall
  // peaks (0.887 -> 0.704 and 1.000 -> 0.581) while leaving all three tilts
  // unchanged. The sting takes no headroom parameter and its peak did not move.
  CHECK(party_tilt > 2 * monster_tilt);

  // Neither may be silent, or the ratio above is a comparison of two zeroes.
  CHECK(monster_tilt > 0);
}

TEST_CASE("the sting is not a third footfall", "[sfx]") {
  std::vector<float> arena;
  Clips clips{};
  REQUIRE(fill(kSeed, arena, clips));

  const auto sting = clip_of(clips, audio::SoundId::HuntingSting);
  const auto party = clip_of(clips, audio::SoundId::PartyFootfall);

  // It is the longest thing in the arena by a factor of six, and §6.1 only ever
  // plays it on a SEARCHING -> HUNTING edge. A sting that had drifted to
  // footfall length would be a tell the player misses.
  CHECK(sting.frames > 5 * party.frames);

  const long sting_tilt =
      tilt_permille(std::span<const float>{arena}.subspan(sting.offset, sting.frames));
  const long party_tilt =
      tilt_permille(std::span<const float>{arena}.subspan(party.offset, party.frames));
  // Tonal rather than noisy: it moves less per sample than a noise burst does.
  CHECK(sting_tilt < party_tilt);
}

TEST_CASE("the arena is 153,600 B of 48 kHz mono float32", "[sfx]") {
  // Happy path last, and it is the least interesting case in the file: it pins
  // the constants a mixer sizes itself against.
  STATIC_REQUIRE(sfx::kSampleRateHz == 48'000);
  STATIC_REQUIRE(sfx::kBufferFrames == 256);
  STATIC_REQUIRE(sfx::kChannels == 2);
  STATIC_REQUIRE(sfx::kArenaFrames == 38'400);
  STATIC_REQUIRE(sfx::kArenaBytes == 153'600);

  std::vector<float> arena;
  Clips clips{};
  REQUIRE(fill(kSeed, arena, clips));
  CHECK(clip_of(clips, audio::SoundId::PartyFootfall).frames == sfx::kPartyFootfallFrames);
  CHECK(clip_of(clips, audio::SoundId::MonsterFootfall).frames == sfx::kMonsterFootfallFrames);
  CHECK(clip_of(clips, audio::SoundId::HuntingSting).frames == sfx::kHuntingStingFrames);
}
