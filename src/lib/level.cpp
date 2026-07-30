#include "gloam/level.hpp"

namespace gloam {

auto Level::void_cell() -> const Cell& {
  // Solid rock, sealed on all four sides. Reads off the edge of the map behave
  // exactly like reads into rock, so callers walking out of bounds need no
  // special case.
  static const Cell kVoid = [] {
    Cell c{};
    c.kind = CellKind::Void;
    for (auto& e : c.edges) e = Edge{EdgeKind::Wall, EdgeState::Open, 0, 0};
    return c;
  }();
  return kVoid;
}

void Level::carve(Coord from, Dir d, int cells) {
  if (cells <= 0) return;
  if (auto* start = at_mut(from)) start->kind = CellKind::Floor;

  Coord cursor = from;
  for (int i = 1; i < cells; ++i) {
    const Coord next = cursor.step(d);
    if (!in_bounds(next)) return;
    if (auto* c = at_mut(next)) c->kind = CellKind::Floor;
    // Two-sided, so the corridor conducts sound and sight equally in both
    // directions. See the note on `link`.
    link(cursor, d, Edge{EdgeKind::Open, EdgeState::Open, 0, 0});
    cursor = next;
  }
}

auto Level::symmetric() const -> bool {
  for (std::int32_t y = 0; y < m_height; ++y) {
    for (std::int32_t x = 0; x < m_width; ++x) {
      const Coord here{x, y};
      for (int d = 0; d < kDirCount; ++d) {
        const auto dir = static_cast<Dir>(d);
        const Coord there = here.step(dir);
        if (!in_bounds(there)) continue;  // the void's edges are not ours to match
        if (!(edge(here, dir) == edge(there, opposite(dir)))) return false;
      }
    }
  }
  return true;
}

auto Level::walk(Coord c, Dir d) const -> std::optional<Coord> {
  if (!edge(c, d).passable()) return std::nullopt;
  const Coord next = c.step(d);
  if (!navigable(next)) return std::nullopt;
  return next;
}

}  // namespace gloam
