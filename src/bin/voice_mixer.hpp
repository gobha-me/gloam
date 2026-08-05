#pragma once

/// SPEC §9.2 — what the real-time callback actually does.
///
/// > The callback never allocates, never locks, never touches sim state. Any of
/// > the three is a dropout, and a dropout in a stealth game is a lie about the
/// > world.
///
/// This file is the callback's whole body, and it contains no RtAudio and no
/// clock. That is not tidiness — it is the only way those three prohibitions
/// become testable. `render` takes the ring, the output buffer, the frame count
/// and THE CURRENT TIME AS A PARAMETER, owns no thread, and can therefore be
/// called on the main thread by `test/28voicemix/` on a machine with no sound
/// card, under every sanitizer leg, with the clock chosen by the test.
///
/// `src/bin/audio_device.cpp` is a dozen lines of glue around it. That asymmetry
/// is deliberate: the part that cannot be observed on this project's hardware is
/// kept as small as it can be made, and everything else is put where the matrix
/// can see it.
///
/// WHY IT IS NOT IN gloam::lib
///
/// Floats (AGENTS.md rule 2), and `test/28voicemix/` replaces global `operator
/// new` to prove `render` allocates nothing — which is only a statement about
/// this code if RtAudio is not linked into the same binary counting the
/// allocations.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

#include "gloam/audio.hpp"
#include "sfx.hpp"

namespace gloam::mix {

/// Simultaneous voices.
///
/// Sixteen, against a ring capacity of 64. §11's reference scale is sixteen
/// monsters, so one tick's worst burst is a footfall plus a sting each and the
/// ring absorbs two such ticks before it drops anything. Sixteen sounding AT
/// ONCE is already past what two channels can resolve, so a larger number would
/// buy mud rather than information.
inline constexpr std::size_t kVoiceSlots = 16;

/// `audio.hpp`'s conversion, verbatim and for its stated reason: 1024 is a power
/// of two, so this is exact in binary floating point on every IEEE-754 target.
/// `test/28voicemix/` asserts it with `==` rather than a tolerance, which is the
/// only way that claim means anything.
[[nodiscard]] constexpr auto gain_to_float(audio::Gain g) noexcept -> float {
  return static_cast<float>(g) * (1.0F / 1024.0F);
}

struct Lr {
  float left{0.0F};
  float right{0.0F};
};

/// Pan on `audio.hpp`'s +/-kPanFull scale to a linear pair summing to one.
///
/// LINEAR, NOT CONSTANT POWER, and this is `gain_from_loudness`'s argument
/// rather than a mixing-desk convention. §6.2's attenuation is subtractive and
/// linear; a -3 dB pan law here would put a curve in what the player hears that
/// the model the monsters are tested against does not have, and §9.3's whole
/// point is that those two stay the same thing. The cost is a centre dip that a
/// constant-power law would not have — a decision, recorded in UPSTREAM.md, not
/// an oversight.
///
/// CLAMPED FIRST. `pan_from_bearing` cannot produce an out-of-range `Pan`, but a
/// `Command` is a POD read off a ring and `Pan` is a plain `std::int16_t`; the
/// mixer trusts the wire, not the producer.
[[nodiscard]] constexpr auto pan_to_lr(audio::Pan p) noexcept -> Lr {
  const audio::Pan clamped =
      p < -audio::kPanFull ? -audio::kPanFull : (p > audio::kPanFull ? audio::kPanFull : p);
  // 512 is a power of two and the numerator is a small exact integer, so both
  // halves are exact: hard left is {1, 0}, centre is {0.5, 0.5}.
  const float right = (static_cast<float>(clamped) + 256.0F) * (1.0F / 512.0F);
  return Lr{1.0F - right, right};
}

/// The mixer. Constructed on the simulation thread, driven by the audio thread.
class Mixer {
 public:
  Mixer(std::span<const float> arena,
        std::span<const sfx::Clip, audio::kSoundIdCount> clips) noexcept;

  Mixer(const Mixer&) = delete;
  auto operator=(const Mixer&) -> Mixer& = delete;

  /// AUDIO THREAD ONLY. Drains `ring`, starts voices, renders `frames` of
  /// interleaved stereo into `out` (which must hold `2 * frames` floats).
  ///
  /// Allocates nothing, locks nothing, reads no clock and names no simulation
  /// type in its signature — §9.2's three prohibitions, in a function a test can
  /// call with no device attached.
  ///
  /// `now_ns` is INJECTED rather than read. That is what keeps <chrono> out of
  /// this file, and it is what makes the latency figure exactly assertable over
  /// synthetic stamps instead of approximately assertable over a real clock.
  auto render(audio::Ring<>& ring, std::span<float> out, std::uint32_t frames,
              std::uint64_t now_ns) noexcept -> void;

  // ── Instruments ───────────────────────────────────────────────────────────
  //
  // Written by the audio thread, read by the simulation thread, all relaxed:
  // they order nothing and must not put a fence on the callback's path. Every
  // one saturates rather than wraps, for `Ring::dropped`'s reason — a wrapped
  // counter reads as a passing budget.

  [[nodiscard]] auto started() const noexcept -> std::uint64_t;
  /// Voices displaced because all `kVoiceSlots` were busy. NOT a dropped
  /// command: the command was delivered and a quieter voice made way for it.
  [[nodiscard]] auto stolen() const noexcept -> std::uint64_t;

  /// Commands that were delivered but never sounded, because every slot was
  /// busy with something at least as loud.
  ///
  /// THE THIRD OUTCOME, and it exists because a test went looking for the first
  /// two and could not make them add up. `started + refused` is every command
  /// this mixer popped; without this counter a burst of seventeen equally loud
  /// footfalls loses one silently, and neither `Ring::dropped` (the command
  /// arrived) nor `stolen` (nothing was displaced) says a word about it.
  ///
  /// It is deliberately NOT folded into `Ring::dropped`, which is §11's budget
  /// row and must keep meaning "the ring was full" — see the case in
  /// `test/28voicemix/` that pins them apart.
  [[nodiscard]] auto refused() const noexcept -> std::uint64_t;
  [[nodiscard]] auto peak_voices() const noexcept -> std::uint32_t;
  /// Output samples that hit the limiter. Sixteen unity-gain centred voices sum
  /// to 16.0, so this is what stands between the mix and the DAC.
  [[nodiscard]] auto clipped() const noexcept -> std::uint64_t;
  /// The largest `now_ns - Command::stamp` seen at the moment a voice started.
  /// GLOAM's half of §11's tick-to-first-sample row; the device's own latency is
  /// the other half and needs hardware.
  [[nodiscard]] auto worst_latency_ns() const noexcept -> std::uint64_t;

  /// Start a measurement window. Never called implicitly, for
  /// `Ring::reset_counters`'s reason: a window that resets itself always passes.
  auto reset_counters() noexcept -> void;

 private:
  struct Voice {
    std::uint32_t offset{0};
    std::uint32_t frames{0};  ///< zero means the slot is free
    std::uint32_t cursor{0};
    float left{0.0F};
    float right{0.0F};
  };

  std::span<const float> m_arena;
  std::array<sfx::Clip, audio::kSoundIdCount> m_clips{};
  std::array<Voice, kVoiceSlots> m_voices{};

  std::atomic<std::uint64_t> m_started{0};
  std::atomic<std::uint64_t> m_stolen{0};
  std::atomic<std::uint64_t> m_refused{0};
  std::atomic<std::uint64_t> m_clipped{0};
  std::atomic<std::uint64_t> m_worst_latency_ns{0};
  std::atomic<std::uint32_t> m_peak_voices{0};
};

}  // namespace gloam::mix
