#pragma once

/// SPEC §9.2 — the output stream, and the wall between it and the simulation.
///
/// > One output stream. 48 kHz, float32, 256-frame buffer (~5.3 ms).
/// > Device loss is a degradation, not a crash: sink goes silent, game
/// > continues, message line reports it.
///
/// THIS HEADER DOES NOT INCLUDE RtAudio, and that is a requirement rather than a
/// courtesy. `cmake/check_rtaudio_boundary.cmake` asserts the include appears in
/// exactly ONE file in the shipped tree; a header that named RtAudio would make
/// the single-module claim a two-module claim, and every consumer of this
/// interface would start dragging a third-party header behind it. The stream is
/// therefore held through a pointer to an incomplete type, and the destructor is
/// declared here and defined where `RtAudio` is complete.
///
/// WHAT CANNOT BE OBSERVED FROM HERE
///
/// Neither this project's dev box nor a GitHub runner has `/dev/snd`. Every path
/// below is compiled and sanitized on both; only `DeviceState::NoDevice` is ever
/// reached. That is a real gate rather than a gap — the no-device path IS
/// §9.2's degradation row, and `audio-no-device-degrades` asserts it end to end
/// — but the reader should know which half is which. `test/10budgets/` keeps the
/// `PENDING M2` marker for the rest, and gloam#4 lists what still needs
/// hardware.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "device_state.hpp"
#include "gloam/audio.hpp"
#include "sfx.hpp"
#include "voice_mixer.hpp"

class RtAudio;  // opaque here on purpose — see the header note

namespace gloam::device {

/// §9.3's sink, backed by a real stream.
///
/// The simulation holds this as an `audio::Sink*` and can therefore do exactly
/// one thing with it: call `play`, which returns void. That is §9.2's "Audio ->
/// sim is nothing. Ever." closed by the type system rather than by care.
class DeviceSink final : public audio::Sink {
 public:
  /// CONSTRUCTION TOUCHES NO DEVICE AND CANNOT FAIL.
  ///
  /// A constructor that opened a stream would have to report failure by
  /// throwing, and §9.2 says device loss is a degradation. Opening is a separate
  /// call that returns a bool nobody is obliged to check.
  ///
  /// `arena` and `clips` must outlive this object; it copies neither.
  DeviceSink(std::span<const float> arena,
             std::span<const sfx::Clip, audio::kSoundIdCount> clips) noexcept;
  ~DeviceSink() override;

  DeviceSink(const DeviceSink&) = delete;
  auto operator=(const DeviceSink&) -> DeviceSink& = delete;

  /// Attempt to open and start the stream. Returns whether it is now `Running`.
  ///
  /// Never throws, never exits, never prints. A `false` return is a supported
  /// outcome of a correct program, not an error path.
  auto open() noexcept -> bool;

  /// Stop and close. Idempotent, and safe to call from the simulation thread at
  /// a tick boundary — which is the only place it should be called from.
  auto close() noexcept -> void;

  auto play(audio::SoundId sound, audio::Gain gain, audio::Pan pan) -> void override;

  [[nodiscard]] auto state() const noexcept -> DeviceState;

  /// True when at least one state transition happened since the previous call.
  ///
  /// Several transitions before the simulation thread polls are coalesced, with
  /// `state()` carrying the latest one. Re-publishing the same state is not a
  /// transition and does not re-arm this flag. §9.2 asks for "message line
  /// reports it", singular; without this the report is either printed once at
  /// exit — too late to be a message line — or every tick, which is a log flood
  /// rather than a message.
  [[nodiscard]] auto take_state_change() noexcept -> bool;

  /// Render one line describing the current state into a CALLER-OWNED buffer,
  /// returning the length written (never more than `out.size()`).
  ///
  /// Takes a span rather than returning a `std::string` because this is also
  /// reachable while a stream thread is running, and §9.2's second rule is that
  /// nothing on that path allocates.
  [[nodiscard]] auto describe(std::span<char> out) const noexcept -> std::size_t;

  /// The buffer size the driver actually GRANTED.
  ///
  /// `openStream`'s `bufferFrames` is an in/out parameter: §9.2 names 256 and a
  /// driver is free to answer with something else, which moves §11's latency
  /// arithmetic. Reporting the granted figure rather than the requested one is
  /// the difference between an instrument and a restatement of a constant.
  [[nodiscard]] auto granted_buffer_frames() const noexcept -> std::uint32_t;

  /// The driver's own contribution to §11's tick-to-first-sample row, in frames.
  /// Zero when no stream is open — which is everywhere GLOAM currently builds.
  [[nodiscard]] auto stream_latency_frames() const noexcept -> std::uint32_t;

  /// Buffers the driver reported as underflowed. §9.2's "a dropout in a stealth
  /// game is a lie about the world", counted.
  [[nodiscard]] auto underflows() const noexcept -> std::uint64_t;

  /// §11's zero-drop row, readable without handing out a mutable ring.
  ///
  /// `Ring::dropped()` is const, but `ring()` is not — and an instrument printer
  /// that has to `const_cast` its way to a counter is one refactor away from
  /// pushing a command by accident, on the wrong thread.
  [[nodiscard]] auto dropped_voices() const noexcept -> std::uint64_t;

  [[nodiscard]] auto ring() noexcept -> audio::Ring<>&;
  [[nodiscard]] auto mixer() const noexcept -> const mix::Mixer&;

 private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

}  // namespace gloam::device
