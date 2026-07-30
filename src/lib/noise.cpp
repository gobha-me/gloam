#include "gloam/noise.hpp"

#include <queue>
#include <vector>

namespace gloam {

namespace {

/// Frontier entry. Ordered loudest-first, ties broken by ascending cell index.
///
/// Dijkstra's result is unique for positive edge weights, so the tie-break is
/// not needed for correctness — it is here so that a future change which DOES
/// make the result order-sensitive (a cap, an early-out, a per-cell side
/// effect) fails reproducibly instead of intermittently. §5.1 forbids
/// order-dependent iteration; this is that rule applied to a work queue.
struct Node {
  std::int32_t loudness{};
  std::size_t index{};
};

struct Quieter {
  auto operator()(const Node& a, const Node& b) const -> bool {
    if (a.loudness != b.loudness) return a.loudness < b.loudness;
    return a.index > b.index;
  }
};

}  // namespace

auto propagate_noise(const Level& level, Coord source, std::int32_t emission,
                     const Tuning& tuning) -> NoiseField {
  NoiseField field(level);
  if (emission <= 0) return field;
  if (!level.in_bounds(source)) return field;
  if (field.m_loudness.empty()) return field;

  auto& loudness = field.m_loudness;
  std::priority_queue<Node, std::vector<Node>, Quieter> frontier;

  const auto origin = level.index_of(source);
  loudness[origin] = emission;
  frontier.push({emission, origin});

  while (!frontier.empty()) {
    const Node node = frontier.top();
    frontier.pop();
    // A stale entry: this cell has since been reached more loudly.
    if (node.loudness < loudness[node.index]) continue;

    const Coord here = level.coord_of(node.index);
    for (int d = 0; d < kDirCount; ++d) {
      const auto dir = static_cast<Dir>(d);
      const Edge& crossing = level.edge(here, dir);
      // Solid rock does not leak. Everything else does — a closed door
      // attenuates by 40, it does not silence.
      if (!crossing.conducts_sound()) continue;

      const Coord there = here.step(dir);
      if (!level.in_bounds(there)) continue;

      const std::int32_t arriving = node.loudness - edge_attenuation(crossing, tuning);
      if (arriving <= 0) continue;  // inaudible is the same as never arrived

      const auto neighbour = level.index_of(there);
      if (arriving > loudness[neighbour]) {
        loudness[neighbour] = arriving;
        frontier.push({arriving, neighbour});
      }
    }
  }

  return field;
}

auto hears(const NoiseField& field, const Level& level, Coord listener, Acuity acuity, bool hunting,
           const Tuning& tuning) -> bool {
  std::int32_t threshold = tuning.hearing_threshold(acuity);
  // §18 Q5 — a HUNTING monster listens harder.
  if (hunting) threshold -= tuning.hear_hunting_bonus;

  // §6.2: "A monster hears if attenuated noise at its own position EXCEEDS its
  // threshold." Strictly greater, not greater-or-equal — a step at exactly the
  // threshold is the quietest inaudible sound, and the boundary is tuned
  // against that reading.
  return field.at(level, listener) > threshold;
}

}  // namespace gloam
