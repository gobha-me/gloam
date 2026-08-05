#include "voice_mixer.hpp"

#include <algorithm>
#include <cstddef>

namespace gloam::mix {
namespace {

/// `emit::saturating_add` and `audio::saturating_add`'s third cousin, for their
/// reason: a wrapped instrument reads as a passing budget.
///
/// AGENTS.md says of the existing pair — "Do not 'fix' it by including
/// emit.hpp... If a third copy ever appears, that is the moment to give them a
/// shared home." This is that third copy, and it is deliberately NOT that
/// moment: the other two are in `gloam::lib`, this one is in `src/bin/`, and
/// giving them a shared home would mean either a header in the library that
/// exists for the binary's benefit or an include edge from the binary into the
/// library's internals. Both are worse than four lines. Revisit if a fourth
/// appears IN src/bin/.
[[nodiscard]] constexpr auto saturating_add(std::uint64_t value, std::uint64_t addend) noexcept
    -> std::uint64_t {
  constexpr std::uint64_t kMax = ~std::uint64_t{0};
  return value > kMax - addend ? kMax : value + addend;
}

}  // namespace

Mixer::Mixer(std::span<const float> arena,
             std::span<const sfx::Clip, audio::kSoundIdCount> clips) noexcept
    : m_arena{arena} {
  std::copy(clips.begin(), clips.end(), m_clips.begin());
}

auto Mixer::render(audio::Ring<>& ring, std::span<float> out, std::uint32_t frames,
                   std::uint64_t now_ns) noexcept -> void {
  const std::size_t samples = std::size_t{frames} * 2U;
  if (out.size() < samples) return;

  std::fill(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(samples), 0.0F);

  // ── Drain ────────────────────────────────────────────────────────────────
  //
  // BOUNDED BY THE RING'S CAPACITY, and the bound is not decoration. An
  // unbounded `while (try_pop(...))` in a real-time callback is one runaway
  // producer away from a callback that never returns, which is an xrun — and
  // §9.2's argument for the ring is precisely that the callback's cost is
  // bounded. The ring cannot hold more than `capacity()` commands, so this loop
  // drains a full ring exactly and refuses to be held open by a producer racing
  // it.
  std::uint64_t started = 0;
  std::uint64_t stolen = 0;
  std::uint64_t refused = 0;
  std::uint64_t worst_latency = 0;

  audio::Command command{};
  for (std::size_t drained = 0; drained < audio::Ring<>::capacity(); ++drained) {
    if (!ring.try_pop(command)) break;

    const auto index = static_cast<std::size_t>(command.sound);
    if (index >= m_clips.size()) continue;  // a byte off a ring is not a promise
    const sfx::Clip clip = m_clips[index];
    // Zero frames is `SoundId::None`, which is why the clip table is indexable
    // with no branch on the id itself. Neither this nor the bounds check below
    // counts as `refused`: refused means "there was a sound and no room for it",
    // and a command naming nothing was never going to be audible.
    if (clip.frames == 0) continue;
    if (std::size_t{clip.offset} + clip.frames > m_arena.size()) continue;

    const Lr pan = pan_to_lr(command.pan);
    const float gain = gain_to_float(command.gain);
    const float left = pan.left * gain;
    const float right = pan.right * gain;

    // ── Slot allocation: steal the QUIETEST, never the oldest ──────────────
    //
    // The ring drops the NEWEST when it is full, because an overwriting ring can
    // rewrite a slot the consumer is reading — a correctness argument that does
    // not apply here, since a voice slot has one owner. So the game argument
    // governs instead: a sting one cell away must never be lost to sixteen
    // distant footfalls. Erring the other way would hand the player less
    // information than the monster has, which is the bias `gain_from_loudness`
    // refuses by name.
    std::size_t chosen = kVoiceSlots;
    for (std::size_t i = 0; i < kVoiceSlots; ++i) {
      if (m_voices[i].frames == 0) {
        chosen = i;
        break;
      }
    }

    if (chosen == kVoiceSlots) {
      std::size_t quietest = 0;
      float quietest_level = m_voices[0].left + m_voices[0].right;
      for (std::size_t i = 1; i < kVoiceSlots; ++i) {
        const float level = m_voices[i].left + m_voices[i].right;
        if (level < quietest_level) {
          quietest_level = level;
          quietest = i;
        }
      }
      // A new voice no louder than everything already sounding is not worth
      // displacing anything for — refusing it is the same call the previous
      // paragraph makes, pointed the other way. Note `<=` rather than `<`: an
      // equally loud newcomer does not displace, which is what stops sixteen
      // identical footfalls thrashing every slot on every buffer.
      //
      // COUNTED, because the command was delivered and did not sound. That
      // outcome is invisible to `Ring::dropped` and to `stolen` alike.
      if (left + right <= quietest_level) {
        refused += 1;
        continue;
      }
      chosen = quietest;
      stolen += 1;
    }

    m_voices[chosen] = Voice{clip.offset, clip.frames, 0, left, right};
    started += 1;

    // §11's tick-to-first-sample, GLOAM's half.
    //
    // A zero stamp is skipped rather than measured: `Command{}` and anything a
    // `RecordingSink` produced carry no clock reading, and treating zero as a
    // timestamp would report a fifty-year latency the first time a test built a
    // command by hand.
    if (command.stamp != 0 && now_ns > command.stamp) {
      worst_latency = std::max(worst_latency, now_ns - command.stamp);
    }
  }

  // ── Render ───────────────────────────────────────────────────────────────
  std::uint32_t active = 0;
  for (auto& voice : m_voices) {
    if (voice.frames == 0) continue;
    ++active;

    const std::uint32_t remaining = voice.frames - voice.cursor;
    const std::uint32_t count = std::min(frames, remaining);
    const std::size_t base = std::size_t{voice.offset} + voice.cursor;

    for (std::uint32_t i = 0; i < count; ++i) {
      const float sample = m_arena[base + i];
      out[std::size_t{i} * 2U] += sample * voice.left;
      out[(std::size_t{i} * 2U) + 1U] += sample * voice.right;
    }

    voice.cursor += count;
    if (voice.cursor >= voice.frames) voice = Voice{};
  }

  // ── Limit ────────────────────────────────────────────────────────────────
  //
  // Sixteen unity-gain centred voices sum to 16.0. Two comparisons rather than
  // std::clamp on floats, and counted rather than silent: a mix that clips is
  // information about the tuning, and an instrument that hides it is how a
  // distorted mix ships.
  std::uint64_t clipped = 0;
  for (std::size_t i = 0; i < samples; ++i) {
    if (out[i] > 1.0F) {
      out[i] = 1.0F;
      clipped += 1;
    } else if (out[i] < -1.0F) {
      out[i] = -1.0F;
      clipped += 1;
    }
  }

  // Counters last, in one batch: the render loop above must not pay for an
  // atomic per sample.
  if (started != 0) {
    m_started.store(saturating_add(m_started.load(std::memory_order_relaxed), started),
                    std::memory_order_relaxed);
  }
  if (stolen != 0) {
    m_stolen.store(saturating_add(m_stolen.load(std::memory_order_relaxed), stolen),
                   std::memory_order_relaxed);
  }
  if (refused != 0) {
    m_refused.store(saturating_add(m_refused.load(std::memory_order_relaxed), refused),
                    std::memory_order_relaxed);
  }
  if (clipped != 0) {
    m_clipped.store(saturating_add(m_clipped.load(std::memory_order_relaxed), clipped),
                    std::memory_order_relaxed);
  }
  if (worst_latency > m_worst_latency_ns.load(std::memory_order_relaxed)) {
    m_worst_latency_ns.store(worst_latency, std::memory_order_relaxed);
  }
  if (active > m_peak_voices.load(std::memory_order_relaxed)) {
    m_peak_voices.store(active, std::memory_order_relaxed);
  }
}

auto Mixer::started() const noexcept -> std::uint64_t {
  return m_started.load(std::memory_order_relaxed);
}

auto Mixer::stolen() const noexcept -> std::uint64_t {
  return m_stolen.load(std::memory_order_relaxed);
}

auto Mixer::refused() const noexcept -> std::uint64_t {
  return m_refused.load(std::memory_order_relaxed);
}

auto Mixer::peak_voices() const noexcept -> std::uint32_t {
  return m_peak_voices.load(std::memory_order_relaxed);
}

auto Mixer::clipped() const noexcept -> std::uint64_t {
  return m_clipped.load(std::memory_order_relaxed);
}

auto Mixer::worst_latency_ns() const noexcept -> std::uint64_t {
  return m_worst_latency_ns.load(std::memory_order_relaxed);
}

auto Mixer::reset_counters() noexcept -> void {
  m_started.store(0, std::memory_order_relaxed);
  m_stolen.store(0, std::memory_order_relaxed);
  m_refused.store(0, std::memory_order_relaxed);
  m_clipped.store(0, std::memory_order_relaxed);
  m_worst_latency_ns.store(0, std::memory_order_relaxed);
  m_peak_voices.store(0, std::memory_order_relaxed);
}

}  // namespace gloam::mix
