#include "gloam/path.hpp"

#include <array>
#include <cstddef>

namespace gloam {

namespace {

/// The four directions in wire order, with `prefer` first when it is one of
/// them. Written as an explicit array rather than as a rotation because a
/// rotation would make the ORDER OF THE OTHER THREE depend on `prefer`, and the
/// point of the wire order is that it is the same every time.
[[nodiscard]] auto search_order(Dir prefer) -> std::array<Dir, kDirCount> {
  std::array<Dir, kDirCount> order{};
  std::size_t n = 0;

  // AN ENUM CLASS DOES NOT ENFORCE ITS OWN RANGE, and `prefer` is `Monster::
  // facing` — hashed state that can arrive from a save file or a test. Out of
  // range, the loop below matches none of the four and appends all of them
  // AFTER the preferred one, writing a fifth entry into a four-element array.
  // Measured with AddressSanitizer, through `advance`, with `facing = Dir(200)`:
  // "stack-buffer-overflow ... WRITE of size 1".
  //
  // Same defect and same fix as `world.cpp`'s `stream_index`, which was written
  // against exactly this: a scoped enum is a promise about spelling, not about
  // values. An unknown facing degrades to plain wire order, which is a monster
  // that steps somewhere sensible rather than a corrupted stack.
  if (static_cast<std::uint8_t>(prefer) < kDirCount) order[n++] = prefer;

  for (int d = 0; d < kDirCount && n < order.size(); ++d) {
    const auto dir = static_cast<Dir>(d);
    if (dir != prefer) order[n++] = dir;
  }
  return order;
}

}  // namespace

auto propagate_distance(const Level& level, std::span<const Coord> sources) -> DistanceField {
  DistanceField field(level);
  if (field.m_distance.empty()) return field;

  auto& distance = field.m_distance;

  // A vector plus a read cursor, not a std::queue over a deque: every cell is
  // pushed at most once, so the whole frontier is bounded by `cell_count()` and
  // one reservation covers the search. `std::queue`'s default container would
  // allocate a block per few hundred cells for no benefit.
  std::vector<std::size_t> frontier;
  frontier.reserve(distance.size());

  // SOURCES ARE DROPPED, NOT REJECTED, and a source in rock is dropped for a
  // reason the header states: `Level::walk` only checks that the DESTINATION is
  // navigable, so a field rooted in rock would reach cells that could not reach
  // it back, and the symmetry the target-rooted trick rests on would be false.
  for (const Coord source : sources) {
    if (!level.in_bounds(source)) continue;
    if (!level.navigable(source)) continue;
    const auto index = level.index_of(source);
    if (distance[index] == 0) continue;  // a duplicate source, already seeded
    distance[index] = 0;
    frontier.push_back(index);
  }

  for (std::size_t read = 0; read < frontier.size(); ++read) {
    const auto index = frontier[read];
    const Coord here = level.coord_of(index);
    const std::int32_t next = distance[index] + 1;

    for (int d = 0; d < kDirCount; ++d) {
      // THE ONE MOVEMENT PREDICATE. `apply` refuses a party's step through it
      // and `patrol_step` walks a route through it; nothing here is allowed a
      // second opinion about whether a body fits through an edge.
      const auto destination = level.walk(here, static_cast<Dir>(d));
      if (!destination) continue;

      const auto neighbour = level.index_of(*destination);
      // Every edge costs one, so the first visit is the shortest one and there
      // is nothing to relax. That is the whole difference from `propagate_noise`
      // and it is why this needs no priority queue.
      if (distance[neighbour] != kUnreachable) continue;

      distance[neighbour] = next;
      frontier.push_back(neighbour);
    }
  }

  return field;
}

auto propagate_distance(const Level& level, Coord source) -> DistanceField {
  return propagate_distance(level, std::span<const Coord>{&source, 1});
}

auto step_down(const DistanceField& field, const Level& level, Coord from, Dir prefer)
    -> std::optional<Dir> {
  // BELT AND BRACES, AND HONESTLY LABELLED AS SUCH. `Coord::step` on INT32_MIN
  // is signed overflow, and a `from` is data — it can arrive from a
  // `level.gloam` that disagrees with itself. But `DistanceField::at` below
  // bounds-checks too and answers `kUnreachable`, which returns before any
  // `walk` is attempted, so NO INPUT REACHES THIS LINE FIRST.
  //
  // It is kept rather than deleted because it makes the precondition local to
  // the function that has it, the way `edge_attenuation` keeps a wall case it
  // documents as unreachable behind `conducts_sound()`. What it is NOT is a
  // guard the ubsan leg is exercising — an earlier version of this comment
  // claimed that, and a mutation removing the line left every suite green.
  if (!level.in_bounds(from)) return std::nullopt;

  const auto here = field.at(level, from);
  if (here == kUnreachable) return std::nullopt;
  if (here == 0) return std::nullopt;  // already there; a source is not a step

  for (const Dir dir : search_order(prefer)) {
    const auto destination = level.walk(from, dir);
    if (!destination) continue;
    if (field.at(level, *destination) == here - 1) return dir;
  }

  // Unreachable in the mathematical sense rather than the map sense: a reached
  // cell at distance d always has a neighbour at d - 1, because that is how it
  // was reached. `test/21path/` asserts it as a property, so arriving here means
  // the field and the level disagree — a field read against a level it was not
  // built for. Standing still is the right answer to that, as it is everywhere
  // else in the pump.
  return std::nullopt;
}

}  // namespace gloam
