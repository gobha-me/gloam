#include "audio_device.hpp"

#include <RtAudio.h>

// ── The version guard, and why it is not a find_package argument ────────────
//
// RtAudio's CMake package advertises its LIBTOOL ABI version rather than its
// release: 5.2.0 reports 6.0.2 and 6.0.1 reports 7.0.0, with COMPATIBILITY
// AnyNewerVersion. So `find_package(RtAudio 6)` ACCEPTS 5.2.0 — whose
// openStream() throws where this file checks a returned RtAudioErrorType, and
// whose whole error model is the opposite of the one §9.2 asks for.
//
// RTAUDIO_VERSION_MAJOR is the only reliable discriminator; 5.x does not define
// it at all. cmake/deps/rtaudio.cmake probes it too, so a pre-6 system copy
// falls through to the pinned fallback rather than reaching this line. This
// guard is the backstop for anyone who bypasses that recipe.
#if !defined(RTAUDIO_VERSION_MAJOR) || RTAUDIO_VERSION_MAJOR < 6
#error "GLOAM needs RtAudio >= 6: the pre-6 API throws where this file checks a return value."
#endif

#include <atomic>
#include <chrono>
#include <cstring>

namespace gloam::device {
namespace {

/// Copy a NUL-terminated literal into a caller buffer without allocating.
/// Truncates rather than overruns; returns what was written.
auto put(std::span<char> out, const char* text) noexcept -> std::size_t {
  if (out.empty()) return 0;
  const std::size_t length = std::strlen(text);
  const std::size_t count = length < out.size() ? length : out.size() - 1;
  std::memcpy(out.data(), text, count);
  out[count] = '\0';
  return count;
}

[[nodiscard]] auto now_ns() noexcept -> std::uint64_t {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now().time_since_epoch())
                                        .count());
}

}  // namespace

struct DeviceSink::Impl {
  Impl(std::span<const float> arena,
       std::span<const sfx::Clip, audio::kSoundIdCount> clips) noexcept
      : mixer{arena, clips} {}

  /// THE REAL-TIME CALLBACK.
  ///
  /// It does three things and nothing else: note an underflow, read the clock
  /// once, and call `mix::Mixer::render`. Everything expensive, allocating or
  /// lockable is upstream of here by construction — see voice_mixer.hpp.
  ///
  /// `steady_clock::now()` is a vDSO `clock_gettime` on Linux: no allocation, no
  /// lock and no syscall. Worth stating, because "the callback never allocates,
  /// never locks" invites the question and the answer is not obvious.
  ///
  /// A static member rather than a free function because `Impl` is private, and
  /// widening it to reach a file-local callback would put an implementation
  /// detail in the header for the convenience of one cast.
  static auto callback(void* output, void* input, unsigned int frames, double stream_time,
                       RtAudioStreamStatus status, void* user) -> int;

  mix::Mixer mixer;
  audio::Ring<> ring;
  std::unique_ptr<RtAudio> rt;

  detail::StateSignal state;
  std::atomic<int> last_error{0};
  std::atomic<std::uint64_t> underflows{0};

  std::uint32_t granted_frames{0};
  std::uint32_t latency_frames{0};
};

auto DeviceSink::Impl::callback(void* output, void* /*input*/, unsigned int frames,
                                double /*stream_time*/, RtAudioStreamStatus status,
                                void* user) -> int {
  auto* impl = static_cast<Impl*>(user);

  if ((status & RTAUDIO_OUTPUT_UNDERFLOW) != 0U) {
    impl->underflows.store(impl->underflows.load(std::memory_order_relaxed) + 1,
                           std::memory_order_relaxed);
  }

  auto* out = static_cast<float*>(output);
  impl->mixer.render(impl->ring, {out, std::size_t{frames} * 2U}, frames, now_ns());

  // Always zero. RtAudio reads 1 and 2 as "stop this stream", and a mixer that
  // could ask for that would be an audio -> sim channel with extra steps.
  return 0;
}

DeviceSink::DeviceSink(std::span<const float> arena,
                       std::span<const sfx::Clip, audio::kSoundIdCount> clips) noexcept
    : m_impl{std::make_unique<Impl>(arena, clips)} {}

DeviceSink::~DeviceSink() { close(); }

auto DeviceSink::open() noexcept -> bool {
  auto* impl = m_impl.get();

  // A caller may retry after NoDevice or Failed. Finish any previous attempt
  // before returning the state signal to its opening state; after `close()` no
  // old callback remains that could publish into the new attempt.
  if (impl->rt) close();
  impl->state.begin_open();

  // The error callback may be invoked FROM THE STREAM THREAD.
  //
  // It captures only `impl`, records the numeric type, and publishes Lost
  // through the lock-free `StateSignal`. The message text is DELIBERATELY
  // IGNORED: copying a std::string allocates, and §9.2's second rule covers
  // anything reachable from that thread. `describe()` renders a fixed line from
  // the type, on the simulation thread, where allocating would be legal and
  // still is not done.
  //
  // It does NOT call stopStream(). Tearing down a stream from inside a callback
  // the stream invoked is a re-entrancy hazard; the flag is set here and `main`
  // closes at a tick boundary.
  impl->rt = std::make_unique<RtAudio>(
      RtAudio::UNSPECIFIED, [impl](RtAudioErrorType type, const std::string&) {
        impl->last_error.store(static_cast<int>(type), std::memory_order_relaxed);
        if (type == RTAUDIO_DEVICE_DISCONNECT) {
          impl->state.mark_lost();
        }
      });

  const auto fail = [&](DeviceState why) {
    // A disconnect reported from the stream thread wins this race. Failure is
    // still the return value; `StateSignal` preserves the more precise reason.
    static_cast<void>(impl->state.mark_failed(why));
    return false;
  };

  // This is the branch every machine GLOAM currently builds on takes.
  if (impl->rt->getDeviceIds().empty()) return fail(DeviceState::NoDevice);

  const unsigned int out_id = impl->rt->getDefaultOutputDevice();
  if (out_id == 0) return fail(DeviceState::NoDevice);

  RtAudio::StreamParameters params{};
  params.deviceId = out_id;
  params.nChannels = static_cast<unsigned int>(sfx::kChannels);
  params.firstChannel = 0;

  // IN/OUT. What comes back is what the driver granted — see
  // granted_buffer_frames() in the header.
  auto buffer_frames = static_cast<unsigned int>(sfx::kBufferFrames);

  const RtAudioErrorType opened =
      impl->rt->openStream(&params, nullptr, RTAUDIO_FLOAT32,
                           static_cast<unsigned int>(sfx::kSampleRateHz), &buffer_frames,
                           &Impl::callback, impl, nullptr);

  if (opened == RTAUDIO_NO_DEVICES_FOUND) return fail(DeviceState::NoDevice);
  if (opened != RTAUDIO_NO_ERROR) return fail(DeviceState::Failed);

  impl->granted_frames = buffer_frames;

  if (impl->rt->startStream() != RTAUDIO_NO_ERROR) {
    impl->rt->closeStream();
    return fail(DeviceState::Failed);
  }

  const long latency = impl->rt->getStreamLatency();
  impl->latency_frames = latency > 0 ? static_cast<std::uint32_t>(latency) : 0U;

  // `startStream()` launches the callback thread before it returns. Publishing
  // Running with an unconditional store here used to erase a disconnect that
  // thread reported during startup. If Lost won, close from this simulation
  // thread and leave the reason sticky.
  if (!impl->state.mark_running()) {
    close();
    return false;
  }
  return true;
}

auto DeviceSink::close() noexcept -> void {
  auto* impl = m_impl.get();
  if (impl == nullptr || !impl->rt) return;

  if (impl->rt->isStreamRunning()) impl->rt->stopStream();
  if (impl->rt->isStreamOpen()) impl->rt->closeStream();
  impl->rt.reset();

  // Running returns to Muted; Lost stays Lost. The compare/exchange also makes
  // a disconnect racing this close win in either interleaving.
  impl->state.mark_closed();
}

auto DeviceSink::play(audio::SoundId sound, audio::Gain gain, audio::Pan pan) -> void {
  auto* impl = m_impl.get();

  // NOT RUNNING MEANS NOT QUEUEING, and this is `audio.hpp`'s argument about
  // NullSink rather than an optimisation:
  //
  //   > IT COUNTS NOTHING, on purpose. A null sink owning a ring nobody drains
  //   > would report drops on any machine without a sound card, and §11's drop
  //   > budget would then be measuring the absence of a device rather than the
  //   > correctness of the ring.
  //
  // Every CI runner and this project's dev box ARE that machine. Without this
  // line `budget::kMaxDroppedVoiceCommands == 0` fails everywhere GLOAM builds,
  // and it fails for a reason that has nothing to do with the ring.
  if (impl->state.state() != DeviceState::Running) return;

  audio::Command command{};
  // THE ONLY CLOCK IN THE AUDIO PATH THAT THE SIMULATION SIDE READS, and it is
  // here rather than in gloam::lib because AGENTS.md rule 1 says so.
  // `audio.hpp` on `Command::stamp`: "Carrying eight bytes needs no clock; THIS
  // is what keeps the latency instrument out of gloam::lib without giving up on
  // measuring it."
  command.stamp = now_ns();
  command.tick = tick();
  command.sound = sound;
  command.gain = gain;
  command.pan = pan;

  // Discarded, and that is `Sink::play` returning void: there is no expression
  // in `advance` that can read the ring's state back.
  static_cast<void>(impl->ring.try_push(command));
}

auto DeviceSink::state() const noexcept -> DeviceState {
  return m_impl->state.state();
}

auto DeviceSink::take_state_change() noexcept -> bool {
  return m_impl->state.take_change();
}

auto DeviceSink::describe(std::span<char> out) const noexcept -> std::size_t {
  switch (m_impl->state.state()) {
    case DeviceState::Muted:
      return put(out, "muted (--audio opens a device; --mute is the default)");
    case DeviceState::NoDevice:
      return put(out, "no output device (SPEC 9.2: a degradation, not a crash - the game ran)");
    case DeviceState::Failed:
      return put(out, "open refused by the driver (SPEC 9.2: a degradation, not a crash)");
    case DeviceState::Running:
      return put(out, "running");
    case DeviceState::Lost:
      return put(out, "disconnected mid-run; the sink went silent and the game continued");
  }
  return put(out, "unknown");
}

auto DeviceSink::granted_buffer_frames() const noexcept -> std::uint32_t {
  return m_impl->granted_frames;
}

auto DeviceSink::stream_latency_frames() const noexcept -> std::uint32_t {
  return m_impl->latency_frames;
}

auto DeviceSink::underflows() const noexcept -> std::uint64_t {
  return m_impl->underflows.load(std::memory_order_relaxed);
}

auto DeviceSink::dropped_voices() const noexcept -> std::uint64_t {
  return m_impl->ring.dropped();
}

auto DeviceSink::ring() noexcept -> audio::Ring<>& { return m_impl->ring; }

auto DeviceSink::mixer() const noexcept -> const mix::Mixer& { return m_impl->mixer; }

}  // namespace gloam::device
