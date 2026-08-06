#pragma once

/// SPEC §9.2 — the cross-thread half of the device lifecycle, without RtAudio.
///
/// The stream thread may publish only `Lost`; the simulation thread publishes
/// every other state and consumes the change flag.  Keeping those two atomics
/// here rather than open-coded in `audio_device.cpp` makes the startup race
/// testable on a machine with no device attached — which is every machine in
/// this project's sanitizer matrix.

#include <atomic>
#include <cstdint>

namespace gloam::device {

/// §9.2: "Device loss is a degradation, not a crash."
///
/// EVERY VALUE BELOW IS A STATE THE GAME KEEPS RUNNING IN. There is no fatal
/// one, and there must not be: §17 reserves fatality for a missing kitty
/// protocol, because a game you cannot see is not a game and a game you cannot
/// hear is a worse game. The distinction is the whole of this enum.
enum class DeviceState : std::uint8_t {
  /// `--mute`, or `--audio` was never passed. `open()` was not attempted.
  Muted = 0,
  /// No output device exists. Every machine this project builds on, today.
  NoDevice = 1,
  /// A device exists and refused the stream — format, rate or channel count.
  Failed = 2,
  Running = 3,
  /// Unplugged mid-run. Reported once, then silence; the game carries on.
  Lost = 4,
};

namespace detail {

/// Two atomics shared by the stream and simulation threads.
///
/// `Lost` is sticky against every simulation-thread transition. In particular,
/// `mark_running()` is a compare/exchange rather than a store because
/// `startStream()` launches the thread before it returns: that thread can report
/// a disconnect before `open()` gets to publish Running.
class StateSignal {
 public:
  StateSignal() = default;
  StateSignal(const StateSignal&) = delete;
  auto operator=(const StateSignal&) -> StateSignal& = delete;

  [[nodiscard]] auto state() const noexcept -> DeviceState {
    return m_state.load(std::memory_order_relaxed);
  }

  /// SIMULATION THREAD, after any old RtAudio object has been closed.
  auto begin_open() noexcept -> void {
    m_state.store(DeviceState::Muted, std::memory_order_relaxed);
    m_changed.store(false, std::memory_order_relaxed);
  }

  [[nodiscard]] auto mark_running() noexcept -> bool {
    DeviceState expected = DeviceState::Muted;
    if (!m_state.compare_exchange_strong(expected, DeviceState::Running,
                                         std::memory_order_relaxed,
                                         std::memory_order_relaxed)) {
      return false;
    }
    publish_change();
    return true;
  }

  /// Publish an opening failure, unless the stream thread got to `Lost` first.
  [[nodiscard]] auto mark_failed(DeviceState failure) noexcept -> bool {
    if (failure != DeviceState::NoDevice && failure != DeviceState::Failed) return false;

    DeviceState expected = DeviceState::Muted;
    if (!m_state.compare_exchange_strong(expected, failure, std::memory_order_relaxed,
                                         std::memory_order_relaxed)) {
      return false;
    }
    publish_change();
    return true;
  }

  /// STREAM THREAD. The one transition that can cross from RtAudio.
  auto mark_lost() noexcept -> void {
    auto current = m_state.load(std::memory_order_relaxed);
    while (current != DeviceState::Lost) {
      if (m_state.compare_exchange_weak(current, DeviceState::Lost,
                                        std::memory_order_relaxed,
                                        std::memory_order_relaxed)) {
        publish_change();
        return;
      }
    }
  }

  /// SIMULATION THREAD. Closing a lost stream must not erase why it closed.
  auto mark_closed() noexcept -> void {
    DeviceState expected = DeviceState::Running;
    if (m_state.compare_exchange_strong(expected, DeviceState::Muted,
                                        std::memory_order_relaxed,
                                        std::memory_order_relaxed)) {
      publish_change();
    }
  }

  [[nodiscard]] auto take_change() noexcept -> bool {
    // Acquire pairs with `publish_change` so a consumer that sees the flag also
    // sees the state store that preceded it. The old pair of relaxed atomics
    // had no such edge: it could consume `true` and still describe the old state.
    return m_changed.exchange(false, std::memory_order_acquire);
  }

 private:
  auto publish_change() noexcept -> void {
    m_changed.store(true, std::memory_order_release);
  }

  std::atomic<DeviceState> m_state{DeviceState::Muted};
  std::atomic<bool> m_changed{false};
};

static_assert(std::atomic<DeviceState>::is_always_lock_free,
              "the RtAudio callback may not acquire a hidden atomic lock");
static_assert(std::atomic<bool>::is_always_lock_free,
              "the RtAudio callback may not acquire a hidden atomic lock");

}  // namespace detail
}  // namespace gloam::device
