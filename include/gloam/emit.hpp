#pragma once

/// SPEC §11 — the emit path, and the counter wrapped around it.
///
/// `BUDGETS.md`'s "Per-frame emission" is specific about where the byte budgets
/// are measured: "The counters wrap the EMIT PATH, so they measure what actually
/// left the process, not what the compositor believed it produced." `ByteSink`
/// is the near half of that wrapper — it counts bytes PRODUCED, which is a
/// necessary condition and not the row itself. What actually left the process is
/// what a write syscall reported, and that lives in `src/bin/tty_writer.hpp`,
/// on the other side of the boundary this library may not cross. `meter.hpp`
/// takes a byte count rather than reading a sink for exactly that reason.
///
/// Every byte GLOAM sends to a terminal goes through one of these, and the
/// counter is not optional bookkeeping bolted on afterwards — it is the reason
/// the type exists rather than a bare `std::string`.
///
/// This header is deliberately separate from `kitty.hpp`. §16's mitigation is
/// that "a vendored driver is a swap and not a rewrite"; that swap replaces the
/// kitty grammar, and it must not drag the budget instrument with it. A byte
/// counter that changes shape because the wire format changed cannot compare a
/// measurement taken before the swap to one taken after.
///
/// It also does not include `budgets.hpp`, on purpose. A sink that knows the
/// budget is a sink that can be CONFIGURED to a different budget, and §11's whole
/// premise is that the numbers are contracts rather than settings. The sink
/// reports; the budget judges; exactly one file can relax a budget. Tests write
/// `CHECK(sink.size() <= budget::kIdleFrameBytes)` — the dependency runs one way.
///
/// Nothing here touches a file descriptor, a clock or a global. Producing bytes
/// is not the same as writing them: the write syscall lives in `src/bin/`, and
/// that is what keeps this side of the boundary inside `gloam::lib` without
/// violating AGENTS.md rule 1.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace gloam::emit {

/// Saturating add, for the cumulative counter.
///
/// Free rather than a private member so the saturation boundary is directly
/// testable without adding a test-only seam to shipped code — you cannot
/// realistically fill a 64-bit counter, and an untested overflow path in a
/// budget instrument is exactly the kind of thing that is discovered wrong.
///
/// Both parameters are `std::uint64_t` rather than `(counter, std::size_t)`.
/// A `std::size_t` addend widens to this harmlessly on every platform, and
/// taking the narrower type is what made `meter.cpp` reach for a local
/// 64-bit-on-both-sides copy — a THIRD copy of a function AGENTS.md permits
/// exactly two of. One signature that serves both call shapes is the fix that
/// rule asks for.
[[nodiscard]] constexpr auto saturating_add(std::uint64_t a, std::uint64_t b) -> std::uint64_t {
  if (a > UINT64_MAX - b) return UINT64_MAX;
  return a + b;
}

/// A byte buffer that remembers how much has gone through it.
///
/// One instance is one output stream. `clear()` ends a frame — the buffer empties
/// and the counters do not, because §11's budgets are measured across a run and a
/// counter that reset every frame would report a passing budget forever.
class ByteSink {
 public:
  ByteSink() = default;

  explicit ByteSink(std::size_t capacity_hint) { m_buf.reserve(capacity_hint); }

  /// Append bytes. NUL-transparent: `bytes` carries its own length, so a binary
  /// payload with an embedded zero is stored whole rather than truncated.
  auto write(std::string_view bytes) -> void {
    m_buf.append(bytes);
    m_total = saturating_add(m_total, bytes.size());
  }

  auto write(char byte) -> void {
    m_buf.push_back(byte);
    m_total = saturating_add(m_total, 1);
  }

  /// This frame's bytes, ready to hand to a write syscall.
  [[nodiscard]] auto view() const noexcept -> std::string_view { return m_buf; }

  /// This frame's byte count. Compare against §11's per-frame rows.
  [[nodiscard]] auto size() const noexcept -> std::size_t { return m_buf.size(); }

  /// Bytes written since the last `reset_totals()`, across all frames.
  ///
  /// 64-bit and saturating, both deliberately. At §11's sustained 8 KB/s a
  /// 32-bit counter wraps in about six days — and a wrapped counter reads as a
  /// PASSING budget, which is the one failure mode a budget instrument must not
  /// have. Saturation turns an impossible overflow into a permanent failure
  /// instead of a silent success.
  [[nodiscard]] auto total() const noexcept -> std::uint64_t { return m_total; }

  /// Frames ended since the last `reset_totals()`. Counts empty frames too:
  /// §11's zero-byte idle row is unmeasurable if idle frames are not frames.
  [[nodiscard]] auto frames() const noexcept -> std::uint64_t { return m_frames; }

  /// The largest single frame since the last `reset_totals()`.
  ///
  /// §11's 400 B and 2 KB rows are per-frame caps, so a run is judged on its
  /// worst frame rather than its last one. Three bytes of state, and impossible
  /// to reconstruct after the fact.
  [[nodiscard]] auto peak_frame() const noexcept -> std::size_t { return m_peak_frame; }

  /// End a frame: the buffer empties, the counters do not.
  auto clear() noexcept -> void {
    if (m_buf.size() > m_peak_frame) m_peak_frame = m_buf.size();
    m_buf.clear();
    ++m_frames;
  }

  /// Start a measurement window. Never called implicitly — a window that resets
  /// itself always passes.
  auto reset_totals() noexcept -> void {
    m_total = 0;
    m_frames = 0;
    m_peak_frame = 0;
  }

 private:
  std::string m_buf;
  std::uint64_t m_total{0};
  std::uint64_t m_frames{0};
  std::size_t m_peak_frame{0};
};

}  // namespace gloam::emit
