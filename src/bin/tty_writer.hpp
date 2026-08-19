#pragma once

/// SPEC §11 · BUDGETS.md "Per-frame emission" — the one write syscall.
///
/// > The counters wrap the emit path, so they measure what actually left the
/// > process, not what the compositor believed it produced.
///
/// `gloam::emit::ByteSink` counts bytes appended to a `std::string`. That is the
/// second quantity, not the first, and no amount of work inside `gloam::lib` can
/// turn it into the first — the library may not reach a file descriptor (§5.1,
/// AGENTS.md rule 1). This header is the other half, and it is why it lives in
/// `src/bin/`: it is the only place in GLOAM allowed to call `write`.
///
/// AGENTS.md already promised this file existed — "the `write` that puts those
/// bytes on a terminal stays in `src/bin/`, and that one line is the whole
/// terminal-facing surface". Until now it did not. Writing it before termforge
/// arrives means the boundary is a fact when the driver lands rather than a
/// negotiation, which is §16's argument for build-order step 2 applied one layer
/// out.
///
/// WHAT THIS DELIBERATELY DOES NOT DO
///
/// No backpressure policy: no `poll`, no timeout, no retry budget, no spinning
/// on `EAGAIN`. Those are decisions for a frame loop, and GLOAM's termforge-backed
/// frame loop has not landed yet (gloam#5, gloam#7). A policy chosen now would be
/// chosen against an imaginary caller and would be load
/// bearing by the time a real one appeared. `write_all` returns what got through
/// and the caller decides.
///
/// PORTABILITY
///
/// POSIX, and the pipe sizing in its test is Linux. This is the first
/// non-portable file in the tree, which is the correct place for it to be: the
/// deterministic core is portable by construction and the terminal is not.
///
/// This file is inside `cmake/check_kitty_boundary.cmake`'s glob. It must never
/// grow a hard-coded escape sequence — it moves bytes, it does not author them.

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "gloam/emit.hpp"

namespace gloam::tty {

enum class WriteError : std::uint8_t {
  None = 0,
  /// The fd is non-blocking and would have blocked. `bytes_written` is real
  /// progress, not a failure — the caller resumes by ADVANCING its offset by
  /// that many bytes. See the note on `WriteResult::bytes_written`.
  WouldBlock = 1,
  /// `EPIPE` — the reader went away. Not fatal to GLOAM; §17 reserves fatality
  /// for a missing kitty protocol.
  Closed = 2,
  /// `write` returned 0 with bytes still to send. Refused rather than retried:
  /// the retry is an infinite loop, and a hung CI runner is worse than a red
  /// one — the same argument test/CMakeLists.txt makes for its timeouts.
  Stalled = 3,
  Failed = 4,
};

struct WriteResult {
  WriteError error{WriteError::None};
  /// What the syscall REPORTED accepting, over THIS CALL only.
  ///
  /// Relative to the bytes handed in, never to any buffer they came from. For
  /// `drain_from` that means it counts from `offset`, not from the start of the
  /// sink — so a caller resumes with `offset += bytes_written`, and a caller
  /// that reads it as an absolute position re-sends a prefix and corrupts the
  /// stream mid-sequence. The regression test for that resumes from a NONZERO
  /// offset, where the two readings differ.
  ///
  /// This is the number §11's budgets are about, and the number to hand to
  /// `meter::FrameMeter::record`.
  std::size_t bytes_written{0};
  int errno_value{0};

  [[nodiscard]] constexpr explicit operator bool() const { return error == WriteError::None; }
};

/// What to do about one `write` return.
///
/// Pure, and separate from the loop, because that is the only way most of it is
/// testable: `EINTR` and a zero-with-bytes-remaining return cannot be provoked
/// from a test with any reliability, so the DECISION is lifted out where it can
/// be exhausted case by case and the loop over it becomes trivial. `kitty.cpp`
/// splits `validate()` from `emit_placement` for the same reason.
enum class Action : std::uint8_t {
  Advance,  ///< bytes moved; advance the offset
  Retry,    ///< interrupted; call again with the same offset
  Stop,     ///< would block; return what got through
  Fail,     ///< nothing further will succeed
};

[[nodiscard]] auto classify(std::ptrdiff_t written, int err, bool bytes_remain) -> Action;

/// Map the `errno` that produced an `Action::Fail` or `Action::Stop` onto a
/// `WriteError`. Separate for the same reason `classify` is.
[[nodiscard]] auto error_for(std::ptrdiff_t written, int err, bool bytes_remain) -> WriteError;

/// Write every byte of `bytes` to `fd`, looping over short writes and `EINTR`.
///
/// Returns early on `EAGAIN`/`EWOULDBLOCK` with `WriteError::WouldBlock` and the
/// bytes that did get through. An empty payload touches no file descriptor.
[[nodiscard]] auto write_all(int fd, std::string_view bytes) -> WriteResult;

/// Drain a sink to `fd`, starting at `offset`.
///
/// Does NOT clear the sink. `emit::ByteSink::clear()` ends a frame, and a
/// partial write has not ended one — clearing here would let a meter record a
/// complete frame that was half written, which is precisely the lie this module
/// exists to prevent. The caller holds the offset and clears when it reaches
/// `sink.size()`.
///
/// An `offset` past the end is refused rather than clamped: a caller that lost
/// track of its own offset has a bug, and reading past a buffer to be helpful is
/// how that bug becomes a crash somewhere else.
///
/// The returned `bytes_written` is RELATIVE to `offset`. Resume with
/// `offset += result.bytes_written`, never `offset = result.bytes_written`.
[[nodiscard]] auto drain_from(int fd, const emit::ByteSink& sink, std::size_t offset)
    -> WriteResult;

}  // namespace gloam::tty
