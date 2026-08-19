#include "terminal_sink.hpp"

#include <limits>
#include <string>
#include <string_view>

#include "tty_writer.hpp"

namespace gloam::tty {

auto TerminalSink::write(std::span<const char> bytes)
    -> std::expected<void, termforge::ErrorEvent> {
  if (bytes.empty()) {
    last_frame_bytes_ = 0;
    return {};
  }
  const auto result = write_all(fd_, std::string_view{bytes.data(), bytes.size()});
  last_frame_bytes_ = result.bytes_written;

  const auto room = std::numeric_limits<std::uint64_t>::max() - bytes_accepted_;
  bytes_accepted_ += result.bytes_written > room ? room : result.bytes_written;

  if (result) return {};

  return std::unexpected{termforge::ErrorEvent{
      termforge::Severity::Warning, "terminal",
      "frame write refused after " + std::to_string(result.bytes_written) +
          " bytes (write error " + std::to_string(static_cast<int>(result.error)) +
          ", errno " + std::to_string(result.errno_value) + ")"}};
}

}  // namespace gloam::tty
