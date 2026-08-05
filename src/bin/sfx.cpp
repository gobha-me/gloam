#include "sfx.hpp"

#include <array>
#include <cstdint>

#include "gloam/rng.hpp"

namespace gloam::sfx {
namespace {

// ── The sine table, and why it is computed rather than written down ─────────
//
// A tonal voice needs a sine and this file may not include <cmath> (see
// sfx.hpp). The two ways out are 256 magic numbers in the source or a rational
// approximation evaluated at compile time; this is the second, because a table
// of literals is a table nobody can check.
//
// Bhaskara I's seventh-century approximation, in the form that never leaves the
// integers. For x = pi*i/N over a half cycle:
//
//     sin(x) ~= 16 x (pi - x) / (5 pi^2 - 4 x (pi - x))
//             = 16 i (N - i) / (5 N^2 - 4 i (N - i))
//
// The pi^2 cancels exactly, which is the whole reason this approximation is the
// one to pick here rather than a Taylor series. Worst-case error is about
// 0.0016 of full scale — some 55 dB down, and this is a sting rather than a
// tuning fork.
//
// constexpr integer arithmetic has exactly one answer on every target, so the
// table is part of the determinism guarantee rather than a threat to it.
inline constexpr std::size_t kSineTableSize = 256;
inline constexpr std::size_t kSineHalf = kSineTableSize / 2;

consteval auto build_sine_table() -> std::array<std::int16_t, kSineTableSize> {
  std::array<std::int16_t, kSineTableSize> table{};
  for (std::size_t i = 0; i < kSineHalf; ++i) {
    const auto n = static_cast<std::int64_t>(kSineHalf);
    const auto x = static_cast<std::int64_t>(i);
    const std::int64_t p = x * (n - x);
    const std::int64_t numerator = 16 * p * 32'767;
    const std::int64_t denominator = 5 * n * n - 4 * p;
    const auto value = static_cast<std::int16_t>(numerator / denominator);
    table[i] = value;
    table[i + kSineHalf] = static_cast<std::int16_t>(-value);
  }
  return table;
}

inline constexpr auto kSine = build_sine_table();

static_assert(kSine[0] == 0, "the table must start at zero, or every voice clicks on");
static_assert(kSine[kSineHalf / 2] > 32'000, "quarter turn should be near full scale");
static_assert(kSine[kSineHalf] == 0, "the half cycle must cross zero");
static_assert(kSine[kSineHalf + kSineHalf / 2] < -32'000, "the second half is the first, negated");

/// Phase increment per sample for `millihertz`, in Q32.
///
/// Exact integer division of 2^32 by the sample rate, computed in 64 bits so
/// nothing overflows and no float ever appears.
[[nodiscard]] constexpr auto phase_step(std::uint64_t millihertz) noexcept -> std::uint32_t {
  constexpr std::uint64_t kTwoPow32 = 1ULL << 32U;
  return static_cast<std::uint32_t>((millihertz * kTwoPow32) /
                                    (static_cast<std::uint64_t>(kSampleRateHz) * 1000ULL));
}

[[nodiscard]] constexpr auto sine_at(std::uint32_t phase) noexcept -> std::int32_t {
  return kSine[(phase >> 24U) & (kSineTableSize - 1U)];
}

/// A percussive envelope in Q16, exact at both ends.
///
/// `(remaining/total)^3` for the body, times a linear ramp over the first
/// `kAttackFrames`. Cubic rather than exponential because an exponential needs
/// `pow`, and because this reaches EXACTLY zero on the last frame — an envelope
/// that merely gets small leaves a step discontinuity at the end of the clip,
/// which is a click, which is the one artefact a stealth game cannot afford to
/// put on every footstep.
inline constexpr std::uint32_t kAttackFrames = 48;  // 1 ms

[[nodiscard]] constexpr auto envelope_q16(std::uint32_t frame, std::uint32_t total,
                                          std::uint32_t power) noexcept -> std::int64_t {
  if (frame >= total) return 0;
  const auto remaining = static_cast<std::int64_t>(total - frame);
  const auto span = static_cast<std::int64_t>(total);

  std::int64_t env = 65'536;
  for (std::uint32_t p = 0; p < power; ++p) env = (env * remaining) / span;

  if (frame < kAttackFrames) {
    env = (env * static_cast<std::int64_t>(frame)) / static_cast<std::int64_t>(kAttackFrames);
  }
  return env;
}

/// One-pole low pass, Q16 coefficient, integer state.
///
/// This is the only thing separating a party footfall from a monster's: same
/// noise source, same envelope shape, different cutoff and different tail. See
/// sfx.hpp on why gain cannot be the discriminator.
[[nodiscard]] constexpr auto low_pass(std::int32_t state, std::int32_t input,
                                      std::int32_t coefficient_q16) noexcept -> std::int32_t {
  return state + static_cast<std::int32_t>((static_cast<std::int64_t>(input - state) *
                                            coefficient_q16) >>
                                           16U);
}

[[nodiscard]] constexpr auto to_float(std::int32_t sample) noexcept -> float {
  // The exactness rule from sfx.hpp, in one line. Clamp first: a filter can
  // overshoot its input range, and a sample outside [-1, 1] would be the mixer's
  // limiter's problem instead of this file's.
  const std::int32_t clamped = sample > 32'767 ? 32'767 : (sample < -32'768 ? -32'768 : sample);
  return static_cast<float>(static_cast<std::int16_t>(clamped)) * (1.0F / 32'768.0F);
}

/// A filtered noise burst — both footfalls, and the sting's attack.
///
/// `headroom_q16` scales the finished sample. It exists because `to_float`
/// CLAMPS, and a clip that reaches the clamp is flat-topped at the source —
/// distortion baked into the arena, which no amount of care in the mixer can
/// undo. The mixer's limiter is for the SUM of up to sixteen voices; a single
/// clip arriving at full scale has already lost information. `test/27sfxarena/`
/// asserts every clip peaks strictly below 1.0 for exactly this reason.
auto render_thud(Rng& noise, std::span<float> out, std::int32_t cutoff_q16,
                 std::uint32_t envelope_power, std::uint32_t body_millihertz,
                 std::int32_t body_weight_q16, std::int32_t headroom_q16) -> void {
  const auto total = static_cast<std::uint32_t>(out.size());
  std::int32_t filter = 0;
  std::uint32_t phase = 0;
  const std::uint32_t step = phase_step(body_millihertz);

  for (std::uint32_t i = 0; i < total; ++i) {
    // One draw per sample, which is what makes kNoiseDraws assertable.
    const auto raw = static_cast<std::int32_t>(static_cast<std::int16_t>(noise.next() >> 48U));
    filter = low_pass(filter, raw, cutoff_q16);

    // The low body under the transient. A footfall is a weight landing, and
    // without it the noise burst reads as a hiss rather than a step.
    phase += step;
    const std::int64_t body = (static_cast<std::int64_t>(sine_at(phase)) * body_weight_q16) >> 16U;

    const std::int64_t env = envelope_q16(i, total, envelope_power);
    const std::int64_t voiced =
        ((static_cast<std::int64_t>(filter) + body) * headroom_q16) >> 16U;
    out[i] = to_float(static_cast<std::int32_t>((voiced * env) >> 16U));
  }
}

/// §6.1 guarantees line of sight when this plays, and `audio.hpp` puts its
/// emission at melee-hit loudness (90). It is the loudest and longest thing in
/// the arena, and the only tonal one.
auto render_sting(Rng& noise, std::span<float> out) -> void {
  const auto total = static_cast<std::uint32_t>(out.size());

  // Two partials a little over a fifth apart, both falling ~15% across the
  // clip. Falling rather than steady because a steady pair reads as an alarm
  // and §6.1's sting is a thing NOTICING you, which is a shorter, dropping
  // gesture.
  constexpr std::uint64_t kLowStartMilliHz = 392'000;   // ~G4
  constexpr std::uint64_t kHighStartMilliHz = 587'000;  // ~D5
  constexpr std::uint32_t kFallPercent = 15;

  constexpr std::uint32_t kAttackNoiseFrames = 480;  // 10 ms of air, then tone
  constexpr std::int32_t kAttackCutoffQ16 = 22'000;

  std::uint32_t low_phase = 0;
  std::uint32_t high_phase = 0;
  std::int32_t filter = 0;

  for (std::uint32_t i = 0; i < total; ++i) {
    // Linear pitch fall, in integers: f = f0 * (100 - 15*i/total) / 100.
    const std::uint64_t progress = (static_cast<std::uint64_t>(kFallPercent) * i) / total;
    const std::uint64_t scale = 100ULL - progress;
    low_phase += phase_step((kLowStartMilliHz * scale) / 100ULL);
    high_phase += phase_step((kHighStartMilliHz * scale) / 100ULL);

    // The upper partial sits under the lower one so the pair reads as one voice
    // with a colour rather than as two notes.
    const std::int64_t tone =
        sine_at(low_phase) + ((static_cast<std::int64_t>(sine_at(high_phase)) * 26'000) >> 16U);

    std::int64_t air = 0;
    if (i < kAttackNoiseFrames) {
      const auto raw = static_cast<std::int32_t>(static_cast<std::int16_t>(noise.next() >> 48U));
      filter = low_pass(filter, raw, kAttackCutoffQ16);
      const std::int64_t air_env = envelope_q16(i, kAttackNoiseFrames, 2);
      air = (static_cast<std::int64_t>(filter) * air_env) >> 16U;
    }

    const std::int64_t env = envelope_q16(i, total, 2);
    // Two thirds of full scale: this is the loudest clip in the arena and it is
    // mixed with up to fifteen others. Leaving headroom here costs less than
    // the limiter costs in test/28voicemix/.
    const std::int64_t voiced = ((tone * 21'800) >> 16U) + (air >> 1U);
    out[i] = to_float(static_cast<std::int32_t>((voiced * env) >> 16U));
  }
}

}  // namespace

auto synthesise(std::uint64_t seed, std::span<float> arena,
                std::span<Clip, audio::kSoundIdCount> clips) -> bool {
  if (arena.size() < kArenaFrames) return false;

  // `SoundId::None` is index 0 and stays {0, 0}. That is what lets the mixer
  // index this table with a raw cast and no branch — `audio.hpp`'s
  // "A mixer sizes its arena by this", made concrete.
  for (auto& clip : clips) clip = Clip{};

  auto noise = rng(seed, Stream::Ambience);

  std::uint32_t cursor = 0;
  const auto place = [&](audio::SoundId id, std::uint32_t frames) -> std::span<float> {
    clips[static_cast<std::size_t>(id)] = Clip{cursor, frames};
    auto slice = arena.subspan(cursor, frames);
    cursor += frames;
    return slice;
  };

  // Order matters only in that it fixes the draw sequence, and therefore the
  // bytes. Do not reorder without expecting test/27sfxarena/'s determinism
  // digest to move.
  //
  // Party first. Brighter cutoff and a sharper envelope than the monster's:
  // your own step is close, dry and high, which is also what makes it
  // uninformative — you always know where you are.
  render_thud(noise, place(audio::SoundId::PartyFootfall, kPartyFootfallFrames),
              /*cutoff_q16=*/18'000, /*envelope_power=*/3,
              /*body_millihertz=*/78'000, /*body_weight_q16=*/30'000,
              /*headroom_q16=*/52'000);

  // The monster's, at the same emission (14) and therefore the same loudness at
  // the same distance. Everything separating the two is here: half the cutoff,
  // a gentler envelope power and a lower body. Duller, heavier, longer.
  render_thud(noise, place(audio::SoundId::MonsterFootfall, kMonsterFootfallFrames),
              /*cutoff_q16=*/7'000, /*envelope_power=*/2,
              /*body_millihertz=*/52'000, /*body_weight_q16=*/44'000,
              /*headroom_q16=*/36'000);

  render_sting(noise, place(audio::SoundId::HuntingSting, kHuntingStingFrames));

  return cursor == kArenaFrames;
}

}  // namespace gloam::sfx
