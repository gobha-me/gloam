#pragma once

/// SPEC §4.8, §11 — termforge's frame boundary through GLOAM's one writer.
///
/// TermForge assembles a complete frame and lends it to ByteSink. This adapter
/// routes that span through `tty::write_all`, so GLOAM still has exactly one
/// source file that calls write(2), and counts bytes the kernel accepted rather
/// than bytes the compositor intended to emit.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

#include <termforge/core/byte_sink.hpp>

namespace gloam::tty {

class TerminalSink final : public termforge::ByteSink {
 public:
  explicit TerminalSink(int fd) noexcept : fd_(fd) {}

  [[nodiscard]] auto write(std::span<const char> bytes)
      -> std::expected<void, termforge::ErrorEvent> override;

  [[nodiscard]] auto bytes_accepted() const noexcept -> std::uint64_t {
    return bytes_accepted_;
  }
  [[nodiscard]] auto last_frame_bytes() const noexcept -> std::size_t {
    return last_frame_bytes_;
  }

 private:
  int fd_{-1};
  std::uint64_t bytes_accepted_{0};
  std::size_t last_frame_bytes_{0};
};

}  // namespace gloam::tty
