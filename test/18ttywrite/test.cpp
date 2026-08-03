// SPEC §11 · BUDGETS.md "Per-frame emission" — what actually left the process.
//
// > The counters wrap the emit path, so they measure what actually left the
// > process, not what the compositor believed it produced.
//
// emit::ByteSink can only ever report the second quantity. This file tests the
// module that can report the first, and the case that justifies both of them is
// "the meter records what the kernel accepted, not what the buffer held" at the
// end of Part C — a 6000-byte frame into a 4096-byte pipe, where the two numbers
// are different and both are known.
//
// Failure matrix first, per AGENTS.md, and the split is deliberate: `classify`
// is pure precisely so that EINTR and a zero-return — neither of which can be
// provoked from a test with any reliability — are exhausted as ordinary function
// calls in Part A. Part B drives real file descriptors for the cases that need
// a kernel. The happy path is last.

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <catch2/catch_all.hpp>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "gloam/budgets.hpp"
#include "gloam/emit.hpp"
#include "gloam/meter.hpp"
#include "tty_writer.hpp"

using gloam::tty::Action;
using gloam::tty::classify;
using gloam::tty::drain_from;
using gloam::tty::WriteError;
using gloam::tty::write_all;

namespace {

/// A payload no prefix of which repeats, so a retry loop that fails to advance
/// its offset shows up as CONTENT corruption and not merely as a wrong length.
auto distinct_payload(std::size_t n) -> std::string {
  std::string s;
  s.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    s.push_back(static_cast<char>('a' + static_cast<int>(i % 26)));
  }
  return s;
}

/// A non-blocking pipe with a known, small buffer, so a write can be made to
/// stop part-way on demand. Linux-specific, as the header says.
struct SmallPipe {
  int read_fd{-1};
  int write_fd{-1};

  SmallPipe() {
    int fds[2]{-1, -1};
    if (::pipe(fds) != 0) return;
    read_fd = fds[0];
    write_fd = fds[1];
    ::fcntl(write_fd, F_SETFL, O_NONBLOCK);
    // The floor is one page; asking for less is silently rounded up.
    ::fcntl(write_fd, F_SETPIPE_SZ, 4096);
  }

  SmallPipe(const SmallPipe&) = delete;
  auto operator=(const SmallPipe&) -> SmallPipe& = delete;

  ~SmallPipe() {
    if (read_fd >= 0) ::close(read_fd);
    if (write_fd >= 0) ::close(write_fd);
  }

  [[nodiscard]] auto ok() const -> bool { return read_fd >= 0 && write_fd >= 0; }

  auto close_reader() -> void {
    if (read_fd >= 0) ::close(read_fd);
    read_fd = -1;
  }

  /// Read whatever is buffered, up to `n`.
  auto drain_reader(std::size_t n) -> std::string {
    std::string out;
    out.resize(n);
    const auto got = ::read(read_fd, out.data(), n);
    out.resize(got > 0 ? static_cast<std::size_t>(got) : 0);
    return out;
  }
};

/// A guard, because a write to a pipe whose reader has gone kills the process by
/// default and ctest reports that as a crash rather than a failure. main.cpp
/// carries the same line for the same reason.
struct IgnoreSigpipe {
  IgnoreSigpipe() { ::signal(SIGPIPE, SIG_IGN); }
};

const IgnoreSigpipe kIgnoreSigpipe;

}  // namespace

// ════════════════════════════════════════════════════════════════════════════
//  Part A — classify, exhausted. No file descriptors here on purpose.
// ════════════════════════════════════════════════════════════════════════════

TEST_CASE("a short write advances rather than failing", "[tty]") {
  CHECK(classify(1, 0, /*bytes_remain=*/true) == Action::Advance);
  CHECK(classify(4096, 0, true) == Action::Advance);
  CHECK(classify(4096, 0, false) == Action::Advance);

  // A short write is the NORMAL case for a pipe or a tty, not an error. Treating
  // it as one is the single most common way this loop gets written wrong.
  CHECK(classify(1, EAGAIN, true) == Action::Advance);  // errno is stale after success
}

TEST_CASE("EINTR retries without advancing", "[tty]") {
  CHECK(classify(-1, EINTR, true) == Action::Retry);
  CHECK(classify(-1, EINTR, false) == Action::Retry);

  // This is the case that cannot be provoked from a test — a signal has to
  // arrive between the call and the copy — and it is why `classify` is a
  // separate pure function at all. `write_all`'s Retry arm is the one path that
  // must NOT touch the offset; advancing there duplicates a prefix on the wire.
}

TEST_CASE("EAGAIN and EWOULDBLOCK both stop, and both are spelled", "[tty]") {
  CHECK(classify(-1, EAGAIN, true) == Action::Stop);
  CHECK(classify(-1, EWOULDBLOCK, true) == Action::Stop);

  // They are the same value on Linux and are permitted to differ elsewhere, so
  // asserting both is not redundant on the platform where it matters. If someone
  // later writes this as two switch labels it is ill-formed exactly there.
  CHECK(gloam::tty::error_for(-1, EAGAIN, true) == WriteError::WouldBlock);
  CHECK(gloam::tty::error_for(-1, EWOULDBLOCK, true) == WriteError::WouldBlock);
}

TEST_CASE("a broken pipe is Closed, and closed is not fatal", "[tty]") {
  CHECK(classify(-1, EPIPE, true) == Action::Fail);
  CHECK(gloam::tty::error_for(-1, EPIPE, true) == WriteError::Closed);

  // §17 reserves fatality for a missing kitty protocol. A reader that went away
  // is reported and survived, the way §9.2 handles device loss.
}

TEST_CASE("every other errno fails rather than looping", "[tty]") {
  for (const int err : {EBADF, EINVAL, EIO, ENOSPC, EFBIG, EDESTADDRREQ}) {
    INFO("errno " << err);
    CHECK(classify(-1, err, true) == Action::Fail);
    CHECK(gloam::tty::error_for(-1, err, true) == WriteError::Failed);
  }
}

TEST_CASE("a zero-byte return with bytes remaining is a stall, not a retry", "[tty]") {
  CHECK(classify(0, 0, /*bytes_remain=*/true) == Action::Fail);
  CHECK(gloam::tty::error_for(0, 0, true) == WriteError::Stalled);

  // THE ANTI-HANG CASE. POSIX does not promise write() cannot return 0 with
  // bytes left; retrying is then an infinite loop that moves nothing, and a hung
  // runner is a worse outcome than a red one because nothing reports it. Same
  // argument test/CMakeLists.txt makes for 15voicering-test's TIMEOUT.
  CHECK(gloam::tty::error_for(0, 0, true) != WriteError::None);
}

TEST_CASE("a zero-byte return with nothing remaining is not a stall", "[tty]") {
  CHECK(classify(0, 0, /*bytes_remain=*/false) == Action::Advance);
  CHECK(gloam::tty::error_for(0, 0, false) == WriteError::None);
}

// ════════════════════════════════════════════════════════════════════════════
//  Part B — real file descriptors
// ════════════════════════════════════════════════════════════════════════════

TEST_CASE("a bad file descriptor reports EBADF and moves nothing", "[tty]") {
  const auto result = write_all(-1, "x");

  CHECK_FALSE(static_cast<bool>(result));
  CHECK(result.error == WriteError::Failed);
  CHECK(result.errno_value == EBADF);
  CHECK(result.bytes_written == 0);
}

TEST_CASE("an empty payload touches no file descriptor at all", "[tty]") {
  // Proven through an fd that would fail if it were touched. §11's idle row is
  // the common case — zero bytes, every tick — and paying a syscall for it would
  // make "nothing changed" cost something.
  const auto result = write_all(-1, "");

  CHECK(static_cast<bool>(result));
  CHECK(result.error == WriteError::None);
  CHECK(result.bytes_written == 0);
  CHECK(result.errno_value == 0);
}

TEST_CASE("a full pipe stops part-way and reports real progress", "[tty]") {
  SmallPipe pipe;
  REQUIRE(pipe.ok());

  const auto payload = distinct_payload(6000);
  const auto first = write_all(pipe.write_fd, payload);

  REQUIRE(first.error == WriteError::WouldBlock);
  CHECK(first.errno_value == EAGAIN);

  // Not zero (it made progress) and not everything (the pipe is smaller than the
  // payload). The exact figure is the kernel's business.
  CHECK(first.bytes_written > 0);
  CHECK(first.bytes_written < payload.size());

  // Now the reconstruction, which is the assertion that matters. A loop that
  // retried without advancing its offset would have written a duplicated prefix;
  // the byte count alone cannot see that, and this can.
  std::string received;
  received += pipe.drain_reader(first.bytes_written);

  std::size_t offset = first.bytes_written;
  while (offset < payload.size()) {
    const auto next = write_all(pipe.write_fd, std::string_view{payload}.substr(offset));
    REQUIRE((next.error == WriteError::None || next.error == WriteError::WouldBlock));
    REQUIRE(next.bytes_written > 0);
    offset += next.bytes_written;
    received += pipe.drain_reader(next.bytes_written);
  }

  CHECK(offset == payload.size());
  CHECK(received.size() == payload.size());
  CHECK(received == payload);
}

TEST_CASE("a closed reader is reported and survived", "[tty]") {
  SmallPipe pipe;
  REQUIRE(pipe.ok());
  pipe.close_reader();

  const auto result = write_all(pipe.write_fd, "some bytes");

  CHECK(result.error == WriteError::Closed);
  CHECK(result.errno_value == EPIPE);

  // The process is still here. Without SIGPIPE ignored the default disposition
  // would have killed it and ctest would report a crash rather than this.
  CHECK(true);
}

TEST_CASE("drain_from refuses an offset past the end", "[tty]") {
  gloam::emit::ByteSink sink;
  sink.write("abcd");

  // Through a bad fd, so a refusal that leaked into a syscall would report EBADF
  // instead of EINVAL and the two would be distinguishable.
  const auto result = drain_from(-1, sink, 5);

  CHECK(result.error == WriteError::Failed);
  CHECK(result.errno_value == EINVAL);
  CHECK(result.bytes_written == 0);

  // The sink is untouched — drain_from never ends a frame.
  CHECK(sink.size() == 4);
  CHECK(sink.frames() == 0);
}

TEST_CASE("drain_from at the end of the sink is a no-op", "[tty]") {
  gloam::emit::ByteSink sink;
  sink.write("abcd");

  const auto result = drain_from(-1, sink, 4);

  CHECK(static_cast<bool>(result));
  CHECK(result.bytes_written == 0);
  CHECK(sink.size() == 4);
}

TEST_CASE("drain_from does not end the frame it partially wrote", "[tty]") {
  SmallPipe pipe;
  REQUIRE(pipe.ok());

  gloam::emit::ByteSink sink;
  const auto payload = distinct_payload(6000);
  sink.write(payload);

  const auto result = drain_from(pipe.write_fd, sink, 0);
  REQUIRE(result.error == WriteError::WouldBlock);

  // The sink still holds the whole frame, including the part that got out. A
  // clear() here would let a meter record a complete frame that was half
  // written — the exact lie this module exists to prevent.
  CHECK(sink.size() == payload.size());
  CHECK(sink.frames() == 0);

  // And the caller can resume from where the kernel stopped.
  pipe.drain_reader(result.bytes_written);
  const auto rest = drain_from(pipe.write_fd, sink, result.bytes_written);
  CHECK(rest.bytes_written > 0);
}

TEST_CASE("drain_from's byte count is relative to its offset, not absolute", "[tty]") {
  // THE CASE THAT DISTINGUISHES THE TWO READINGS. Every resume above starts from
  // offset 0, where "bytes accepted" and "the offset to resume at" are the same
  // number by coincidence. They are not the same number on the second resume,
  // and a caller that assigns rather than adds re-sends a prefix — thousands of
  // bytes duplicated in the middle of an APC sequence, which the byte totals
  // cannot see and the reader can.
  SmallPipe pipe;
  REQUIRE(pipe.ok());

  gloam::emit::ByteSink sink;
  const auto payload = distinct_payload(6000);
  sink.write(payload);

  const auto first = drain_from(pipe.write_fd, sink, 0);
  REQUIRE(first.error == WriteError::WouldBlock);
  REQUIRE(first.bytes_written > 0);

  std::string received = pipe.drain_reader(first.bytes_written);

  // The reader has emptied the pipe, so this second call resumes from a NONZERO
  // offset — the one place the two readings differ. If `bytes_written` were
  // absolute it would come back as the whole 6000, and the bound below would
  // catch it; a caller that assigned rather than added would rewind into bytes
  // already on the wire.
  const std::size_t offset = first.bytes_written;
  const auto second = drain_from(pipe.write_fd, sink, offset);
  REQUIRE(second.bytes_written > 0);

  // Relative: what this call accepted, starting at `offset`. NOT the absolute
  // position in the sink.
  CHECK(second.bytes_written <= sink.size() - offset);
  CHECK(offset + second.bytes_written <= sink.size());

  received += pipe.drain_reader(second.bytes_written);

  std::size_t done = offset + second.bytes_written;
  while (done < sink.size()) {
    const auto next = drain_from(pipe.write_fd, sink, done);
    REQUIRE(next.bytes_written > 0);
    done += next.bytes_written;
    received += pipe.drain_reader(next.bytes_written);
  }

  CHECK(done == payload.size());
  CHECK(received == payload);
}

// ════════════════════════════════════════════════════════════════════════════
//  Part C — the reason both this module and meter.hpp exist
// ════════════════════════════════════════════════════════════════════════════

TEST_CASE("the meter records what the kernel accepted, not what the buffer held", "[tty][meter]") {
  SmallPipe pipe;
  REQUIRE(pipe.ok());

  gloam::emit::ByteSink sink;
  gloam::meter::FrameMeter meter;

  const auto payload = distinct_payload(6000);
  sink.write(payload);
  REQUIRE(sink.size() == 6000);

  const auto result = drain_from(pipe.write_fd, sink, 0);
  REQUIRE(result.error == WriteError::WouldBlock);
  REQUIRE(result.bytes_written < sink.size());

  // THE LINE THIS WHOLE FILE IS FOR. BUDGETS.md wants what left the process, and
  // `record` takes a count rather than a sink precisely so this number — the
  // kernel's, not the buffer's — is the one that reaches the budget.
  meter.record(gloam::meter::FrameClass::Recomposition, result.bytes_written);

  CHECK(meter.total() == result.bytes_written);
  CHECK(meter.total() < sink.size());
  CHECK(meter.total() != 6000);

  // Both numbers are known and they disagree. Had the meter read the sink it
  // would have reported 6000 bytes of traffic that never reached a terminal, and
  // the recomposition row would have been judged against a fiction.
  CHECK(sink.size() == 6000);
}

// ════════════════════════════════════════════════════════════════════════════
//  Part D — the smoke check
// ════════════════════════════════════════════════════════════════════════════

TEST_CASE("bytes written to a blocking fd all arrive, in order", "[tty]") {
  int fds[2]{-1, -1};
  REQUIRE(::pipe(fds) == 0);

  // A payload comfortably inside a default 64 KiB pipe buffer, so no reader is
  // needed and the write completes in one go.
  //
  // That "comfortably" is a real dependency on a budget constant this test does
  // not own, and it is the difference between a red run and a HUNG one: past a
  // pipe buffer, `write_all` blocks in write(2) forever on a fd with no reader.
  // Made a compile error rather than left to be discovered by a stalled CI job —
  // and note gloam#26 pushes in exactly the direction that would raise it.
  static_assert(gloam::budget::kMaxRecompositionBytes < 65536,
                "this case writes a recomposition-sized payload into an undrained pipe; "
                "past the pipe buffer it would block rather than fail");
  const auto payload = distinct_payload(gloam::budget::kMaxRecompositionBytes);
  const auto result = write_all(fds[1], payload);

  CHECK(static_cast<bool>(result));
  CHECK(result.error == WriteError::None);
  CHECK(result.bytes_written == payload.size());

  std::string received;
  received.resize(payload.size());
  const auto got = ::read(fds[0], received.data(), received.size());
  REQUIRE(got == static_cast<std::ptrdiff_t>(payload.size()));
  CHECK(received == payload);

  ::close(fds[0]);
  ::close(fds[1]);
}
