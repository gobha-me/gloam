#pragma once

/// SPEC §11 — the per-frame budget classes, and the p95 over their history.
///
/// `emit::ByteSink` counts bytes. It does not know that §11 judges a frame
/// against a DIFFERENT number depending on what kind of frame it was, and it
/// keeps no history, so no percentile of any kind is computable from it —
/// `peak_frame()` is p100 and `total() / frames()` is the mean. This header is
/// the aggregation layer that sits between the sink and the budget.
///
/// The dependency direction from `emit.hpp` extends by one link and does not
/// turn round: the sink counts, the meter aggregates, the budget judges. This
/// header includes `emit.hpp` and NOT `budgets.hpp`, for the reason `emit.hpp`
/// gives at length — a meter that knows the budget is a meter that can be
/// configured to a different one, and §11's premise is that the numbers are
/// contracts rather than settings. Tests write
/// `CHECK(meter.peak(FrameClass::Idle) == budget::kIdleFrameBytes)`.
///
/// It is also a separate header from `emit.hpp` rather than three more members
/// on `ByteSink`, which is the shorter-looking option:
///
///   - `kitty.cpp` calls `sink.write()` once per key — fourteen times per
///     placement, some hundreds of times per recomposition frame. A history
///     `std::vector` behind that call site puts an allocation policy in the emit
///     inner loop, against §11's own 2 ms compose-diff-emit row.
///   - `ByteSink::clear()` IS the frame boundary and takes no argument. Adding
///     `clear(FrameClass)` either breaks every existing caller or defaults the
///     class — and a default frame class is a misclassification that reports
///     itself as data.
///   - §16's swap argument cuts this way, not the other. `ByteSink` is the type
///     a vendored driver writes into, so it is on the driver-facing side of the
///     boundary. The instrument whose job is to compare a pre-swap run against a
///     post-swap one has to sit ABOVE the type the swap reshapes.
///
/// Nothing here touches a file descriptor, a clock or a global.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "gloam/emit.hpp"

namespace gloam::meter {

/// §11's per-frame rows, as a discriminant.
///
/// The design names these by situation and never gives a rule code can
/// evaluate — see UPSTREAM.md item 11 and gloam#24, which record the call: a
/// frame is classified on the STATE DELTA across the tick, not on the input
/// event, because `apply()` already refuses a step into a wall and classifying
/// on the event would let an input log manufacture its own budget class.
///
/// The rule itself deliberately does NOT live here. This header has no idea what
/// a `World` is, and a `gloam::meter` that did would be a simulation dependency
/// inside a byte instrument.
enum class FrameClass : std::uint8_t {
  Idle = 0,           ///< nothing changed. §11 budgets ZERO bytes, not "few".
  Animation = 1,      ///< lamp or awareness moved. ≤ 400 B.
  Recomposition = 2,  ///< the party stepped or turned. ≤ 2 KB.
};

inline constexpr int kFrameClassCount = 3;

[[nodiscard]] constexpr auto name_of(FrameClass frame_class) -> std::string_view {
  switch (frame_class) {
    case FrameClass::Idle: return "idle";
    case FrameClass::Animation: return "animation";
    case FrameClass::Recomposition: return "recomposition";
  }
  return "?";
}

/// How many frames of history a meter keeps by default.
///
/// Bounded because a meter attached to a session has no idea how long that
/// session is, and an instrument that can exhaust memory is a worse failure than
/// one that admits it stopped recording. At §11's 10 Hz this is a little under
/// three hours of continuous play; past it the counters keep counting and the
/// PERCENTILE queries start refusing, which is the honest degradation — see
/// `history_truncated()`.
inline constexpr std::size_t kDefaultHistoryLimit = 100'000;

/// Nearest-rank percentile over an already-sorted, caller-owned span.
///
/// `percentile` is in [1, 100]. The rank is `ceil(percentile * n / 100)`,
/// computed in integers as `(percentile * n + 99) / 100`, and the result is the
/// sample at that 1-indexed rank. No interpolation: AGENTS.md rule 2 forbids
/// floating point in anything the simulation's numbers flow through, and a
/// percentile that lands between two samples has to invent a value that no frame
/// ever produced in order to report it.
///
/// Returns `nullopt` — and pointedly NOT 0 — on an empty span or a percentile
/// outside [1, 100]. A zero p95 compares favourably against every budget there
/// is, which makes "I had no data" indistinguishable from "I passed". That is
/// the same failure mode `emit.hpp` saturates its 64-bit total to avoid.
[[nodiscard]] auto percentile_nearest_rank(std::span<const std::uint64_t> sorted, int percentile)
    -> std::optional<std::uint64_t>;

/// Bytes per frame, split by §11's frame classes, with enough history to answer
/// a percentile.
///
/// One instance is one measurement window. Nothing here judges: a 500-byte frame
/// recorded as `Idle` is stored as a 500-byte idle frame, not refused and not
/// quietly reclassified. A meter that corrects a misclassification is a meter
/// that hides one, and hiding exactly that is what `budget::kIdleFrameBytes == 0`
/// exists to catch.
class FrameMeter {
 public:
  FrameMeter() = default;

  explicit FrameMeter(std::size_t history_limit) : m_limit{history_limit} {}

  /// Record a finished frame.
  ///
  /// Takes a byte COUNT rather than reading a `ByteSink`, and that is the whole
  /// point of the signature. `BUDGETS.md` asks for "what actually left the
  /// process, not what the compositor believed it produced"; a `ByteSink` can
  /// only ever report the second. `src/bin/tty_writer.hpp` returns what a write
  /// syscall accepted, and this is the call that takes it.
  auto record(FrameClass frame_class, std::size_t bytes) -> void;

  /// Read this frame's size from `sink`, record it, and end the frame.
  ///
  /// The convenience for callers with no file descriptor — tests, and anything
  /// measuring the produced side on its own. One call, so the recorded count
  /// cannot disagree with the sink it came from.
  auto end_frame(emit::ByteSink& sink, FrameClass frame_class) -> void;

  [[nodiscard]] auto frames() const noexcept -> std::uint64_t;
  [[nodiscard]] auto frames(FrameClass frame_class) const noexcept -> std::uint64_t;

  /// Bytes recorded since construction or the last `reset()`. Saturating, for
  /// `emit.hpp`'s reason: a wrapped counter reads as a passing budget.
  [[nodiscard]] auto total() const noexcept -> std::uint64_t;
  [[nodiscard]] auto total(FrameClass frame_class) const noexcept -> std::uint64_t;

  /// The largest single frame of this class. §11's 400 B and 2 KB rows are
  /// per-frame caps, so a run is judged on its worst frame, not its last.
  [[nodiscard]] auto peak(FrameClass frame_class) const noexcept -> std::size_t;

  /// True once `history_limit` frames have been recorded. The counters above go
  /// on counting; the percentile queries below start returning `nullopt`,
  /// because a p95 over a truncated history is a p95 of a different run.
  [[nodiscard]] auto history_truncated() const noexcept -> bool { return m_truncated; }

  [[nodiscard]] auto history() const noexcept -> std::span<const std::size_t> { return m_bytes; }

  /// Start a measurement window. Never called implicitly.
  auto reset() noexcept -> void;

 private:
  [[nodiscard]] static constexpr auto index_of(FrameClass frame_class) -> std::size_t {
    return static_cast<std::size_t>(frame_class);
  }

  std::vector<std::size_t> m_bytes;
  std::array<std::uint64_t, kFrameClassCount> m_frames{};
  std::array<std::uint64_t, kFrameClassCount> m_total{};
  std::array<std::size_t, kFrameClassCount> m_peak{};
  std::size_t m_limit{kDefaultHistoryLimit};
  bool m_truncated{false};
};

/// Every sliding sum of `window_frames` consecutive frames, ascending.
///
/// `nullopt` if the window is zero, if the history is shorter than the window,
/// or if the history was truncated. Deliberately not "fall back to the whole
/// history" on a short run: a three-tick "second" is not a second, and a budget
/// in bytes per second compared against two-thirds of one passes for the wrong
/// reason.
///
/// Sums saturate rather than wrap, for the reason every other counter in this
/// tree does.
[[nodiscard]] auto sliding_window_sums(const FrameMeter& meter, std::size_t window_frames)
    -> std::optional<std::vector<std::uint64_t>>;

/// §11's sustained row: the p95 of the sliding window sums.
///
/// `window_frames` must be ONE SECOND of ticks, because the budget it is
/// compared against is stated per second and this returns a window SUM. The
/// caller derives it from `replay::kTickHz` — passing it in rather than reading
/// it here is what keeps a byte instrument free of a dependency on the replay
/// format, and the caller is expected to assert the identity so that a change to
/// the tick rate cannot silently rescale the budget.
///
/// The rejected alternative, recorded here so it is not rediscovered: p95 of
/// PER-TICK bytes, scaled by the tick rate. A recomposition tick appears in one
/// per-tick sample but in `window_frames` sliding windows, so the per-tick form
/// lets any script with under 5% recomposition ticks report the animation cost as
/// its sustained rate — a budget that reports the cost of standing still as the
/// cost of walking. See UPSTREAM.md item 12 and gloam#25.
[[nodiscard]] auto sustained_p95(const FrameMeter& meter, std::size_t window_frames)
    -> std::optional<std::uint64_t>;

}  // namespace gloam::meter
