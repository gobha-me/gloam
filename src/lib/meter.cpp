#include "gloam/meter.hpp"

#include <algorithm>

namespace gloam::meter {

// Every counter below saturates through `emit::saturating_add`, and there is no
// local copy of it.
//
// AGENTS.md permits exactly two spellings of this function — `emit`'s and
// `audio`'s — and says that "if a third copy ever appears, that is the moment to
// give them a shared home rather than now". A local one here would have been the
// third. `meter.hpp` already includes `emit.hpp`, because the meter is
// downstream of the sink by design, so the shared home already existed; the only
// thing in the way was `emit::saturating_add` taking a narrower second
// parameter, and widening it was the whole fix.

auto percentile_nearest_rank(std::span<const std::uint64_t> sorted, int percentile)
    -> std::optional<std::uint64_t> {
  if (sorted.empty()) return std::nullopt;
  if (percentile < 1 || percentile > 100) return std::nullopt;

  // ceil(percentile * n / 100), in integers. `percentile` is bounded by 100 and
  // `n` by the history limit, so the product cannot overflow the 64-bit rank.
  const auto n = static_cast<std::uint64_t>(sorted.size());
  const auto p = static_cast<std::uint64_t>(percentile);
  const std::uint64_t rank = (p * n + 99) / 100;

  // `rank >= 1` for every percentile >= 1 over a non-empty span, and `rank <= n`
  // because `p <= 100`. Both bounds carry the indexing below, and both are
  // asserted by literal in test/17meter/ rather than assumed here.
  return sorted[static_cast<std::size_t>(rank - 1)];
}

auto FrameMeter::record(FrameClass frame_class, std::size_t bytes) -> void {
  const auto i = index_of(frame_class);

  m_frames[i] = emit::saturating_add(m_frames[i], 1);
  m_total[i] = emit::saturating_add(m_total[i], bytes);
  if (bytes > m_peak[i]) m_peak[i] = bytes;

  // The counters above are unconditional; only the history is bounded. A meter
  // that stopped counting at the limit would report a passing total for a run
  // that outlived its own instrument.
  if (m_bytes.size() < m_limit) {
    m_bytes.push_back(bytes);
  } else {
    m_truncated = true;
  }
}

auto FrameMeter::end_frame(emit::ByteSink& sink, FrameClass frame_class) -> void {
  record(frame_class, sink.size());
  sink.clear();
}

auto FrameMeter::frames() const noexcept -> std::uint64_t {
  std::uint64_t sum = 0;
  for (const auto count : m_frames) sum = emit::saturating_add(sum, count);
  return sum;
}

auto FrameMeter::frames(FrameClass frame_class) const noexcept -> std::uint64_t {
  return m_frames[index_of(frame_class)];
}

auto FrameMeter::total() const noexcept -> std::uint64_t {
  std::uint64_t sum = 0;
  for (const auto bytes : m_total) sum = emit::saturating_add(sum, bytes);
  return sum;
}

auto FrameMeter::total(FrameClass frame_class) const noexcept -> std::uint64_t {
  return m_total[index_of(frame_class)];
}

auto FrameMeter::peak(FrameClass frame_class) const noexcept -> std::size_t {
  return m_peak[index_of(frame_class)];
}

auto FrameMeter::reset() noexcept -> void {
  m_bytes.clear();
  m_frames.fill(0);
  m_total.fill(0);
  m_peak.fill(0);
  m_truncated = false;
}

auto sliding_window_sums(const FrameMeter& meter, std::size_t window_frames)
    -> std::optional<std::vector<std::uint64_t>> {
  if (window_frames == 0) return std::nullopt;
  if (meter.history_truncated()) return std::nullopt;

  const auto history = meter.history();
  if (history.size() < window_frames) return std::nullopt;

  const std::size_t count = history.size() - window_frames + 1;

  std::vector<std::uint64_t> sums;
  sums.reserve(count);

  // Each window summed from scratch: nothing can be correctly subtracted from a
  // saturated total, so add-one-drop-one would let one pinned window poison
  // every later one. Called once at the end of a run, never per frame.
  for (std::size_t i = 0; i < count; ++i) {
    std::uint64_t sum = 0;
    for (std::size_t j = i; j < i + window_frames; ++j) sum = emit::saturating_add(sum, history[j]);
    sums.push_back(sum);
  }

  return sums;
}

auto sustained_p95(const FrameMeter& meter, std::size_t window_frames)
    -> std::optional<std::uint64_t> {
  auto sums = sliding_window_sums(meter, window_frames);
  if (!sums) return std::nullopt;

  std::ranges::sort(*sums);
  return percentile_nearest_rank(*sums, 95);
}

}  // namespace gloam::meter
