#include "tty_writer.hpp"

#include <unistd.h>

namespace gloam::tty {

auto classify(std::ptrdiff_t written, int err, bool bytes_remain) -> Action {
  if (written > 0) return Action::Advance;

  if (written == 0) {
    // A zero return with nothing left to send is the empty write, which is fine.
    // A zero return with bytes REMAINING is the classic hang: retry forever,
    // move nothing. POSIX does not promise this cannot happen, so it is refused
    // rather than trusted.
    return bytes_remain ? Action::Fail : Action::Advance;
  }

  if (err == EINTR) return Action::Retry;

  // EAGAIN and EWOULDBLOCK are the same value on Linux and are permitted to
  // differ elsewhere. Both are spelled out so the intent survives a platform
  // where they differ — and as an `if` rather than two `case` labels, which is
  // ill-formed on every platform where they do not.
  if (err == EAGAIN || err == EWOULDBLOCK) return Action::Stop;

  return Action::Fail;
}

auto error_for(std::ptrdiff_t written, int err, bool bytes_remain) -> WriteError {
  switch (classify(written, err, bytes_remain)) {
    case Action::Advance:
    case Action::Retry: return WriteError::None;
    case Action::Stop: return WriteError::WouldBlock;
    case Action::Fail: break;
  }

  if (written == 0) return WriteError::Stalled;
  if (err == EPIPE) return WriteError::Closed;
  return WriteError::Failed;
}

auto write_all(int fd, std::string_view bytes) -> WriteResult {
  WriteResult result{};

  // An empty payload is not a write. Checked before the loop so that "flush a
  // frame that emitted nothing" — §11's idle row, the common case — costs no
  // syscall at all rather than one that returns 0.
  if (bytes.empty()) return result;

  std::size_t offset = 0;
  while (offset < bytes.size()) {
    // Cleared first so that `errno_value` is never a leftover from stdio or
    // locale initialisation. `write` returning 0 does not set errno, and the
    // Stalled path reports it — pointing whoever debugs a wedged terminal at a
    // stale ENOENT is worse than reporting nothing.
    errno = 0;
    const std::ptrdiff_t n = ::write(fd, bytes.data() + offset, bytes.size() - offset);
    const int err = errno;
    const bool bytes_remain = offset < bytes.size();

    switch (classify(n, err, bytes_remain)) {
      case Action::Advance:
        offset += static_cast<std::size_t>(n);
        result.bytes_written = offset;
        break;
      case Action::Retry:
        // Same offset, no accounting change. The one path that must not touch
        // `offset` — advancing on EINTR duplicates a prefix on the wire, and a
        // length check alone would never see it.
        break;
      case Action::Stop:
      case Action::Fail:
        result.error = error_for(n, err, bytes_remain);
        result.errno_value = err;
        return result;
    }
  }

  return result;
}

auto drain_from(int fd, const emit::ByteSink& sink, std::size_t offset) -> WriteResult {
  const auto view = sink.view();

  if (offset > view.size()) {
    WriteResult refused{};
    refused.error = WriteError::Failed;
    refused.errno_value = EINVAL;
    return refused;
  }

  return write_all(fd, view.substr(offset));
}

}  // namespace gloam::tty
