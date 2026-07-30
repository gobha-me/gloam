#pragma once

/// SPEC §12 — the cell graph.
///
/// One graph, two readers. §12 puts attenuation on the EDGE rather than the
/// cell "so that a closed door and its doorway are one object, and so the noise
/// graph and the render graph are the same graph." That is the whole design of
/// this header: there is no separate sound map, no separate visibility map, and
/// no way for them to drift apart.

#include <cstdint>
#include <optional>
#include <vector>

#include "gloam/tuning.hpp"

namespace gloam {

/// Grid directions. Values are wire format (`level.gloam` stores `edges[4]`),
/// so the order is fixed: north, east, south, west, clockwise from north.
enum class Dir : std::uint8_t { North = 0, East = 1, South = 2, West = 3 };

inline constexpr int kDirCount = 4;

/// The direction you came from.
[[nodiscard]] constexpr auto opposite(Dir d) -> Dir {
  return static_cast<Dir>((static_cast<std::uint8_t>(d) + 2) % kDirCount);
}

enum class EdgeKind : std::uint8_t {
  Open = 0,     ///< nothing between the two cells
  Wall = 1,     ///< solid; blocks movement, sight and sound
  Door = 2,     ///< a door in a frame; its state decides what it blocks
  Doorway = 3,  ///< a framed opening with no door
};

enum class EdgeState : std::uint8_t { Open = 0, Closed = 1, Locked = 2 };

enum class CellKind : std::uint8_t {
  Void = 0,   ///< solid rock — not navigable, not renderable as a space
  Floor = 1,  ///< a navigable cell
};

/// One side of the boundary between two cells.
struct Edge {
  EdgeKind kind{EdgeKind::Wall};
  EdgeState state{EdgeState::Open};
  std::uint16_t key_id{0};

  /// Authored override for this edge's noise loss, as a positive magnitude.
  /// Zero means "derive from kind and state", which is what every generated
  /// level uses; an authored test level can pin an unusual value.
  std::uint8_t attenuation_override{0};

  [[nodiscard]] auto operator==(const Edge&) const -> bool = default;

  /// Can a body pass through?
  [[nodiscard]] constexpr auto passable() const -> bool {
    switch (kind) {
      case EdgeKind::Open:
      case EdgeKind::Doorway: return true;
      case EdgeKind::Door: return state == EdgeState::Open;
      case EdgeKind::Wall: return false;
    }
    return false;
  }

  /// Can sight pass through? A closed door is opaque; an open one is not.
  [[nodiscard]] constexpr auto transparent() const -> bool { return passable(); }

  /// Can sound pass through at all? Everything except solid rock leaks —
  /// a closed door attenuates by 40 (§6.2), it does not silence.
  [[nodiscard]] constexpr auto conducts_sound() const -> bool { return kind != EdgeKind::Wall; }
};

/// The noise lost crossing this edge, as a positive magnitude (§6.2).
///
/// A `Wall` returns a value large enough to extinguish any emitter in the
/// table; callers should gate on `conducts_sound()` first and treat this as
/// belt-and-braces rather than the primary check.
[[nodiscard]] constexpr auto edge_attenuation(const Edge& e, const Tuning& t) -> std::int32_t {
  if (e.attenuation_override != 0) return e.attenuation_override;
  switch (e.kind) {
    case EdgeKind::Open: return t.atten_open_cell;
    case EdgeKind::Doorway: return t.atten_open_doorway;
    case EdgeKind::Door:
      return e.state == EdgeState::Open ? t.atten_open_doorway : t.atten_closed_door;
    case EdgeKind::Wall: break;
  }
  return t.noise_fall + 1;  // louder than the loudest emitter: nothing survives a wall
}

/// A cell coordinate. Signed so that stepping off the grid is representable and
/// can be rejected, rather than wrapping to a huge unsigned index.
struct Coord {
  std::int32_t x{};
  std::int32_t y{};

  [[nodiscard]] constexpr auto operator==(const Coord&) const -> bool = default;

  [[nodiscard]] constexpr auto step(Dir d) const -> Coord {
    switch (d) {
      case Dir::North: return {x, y - 1};
      case Dir::East: return {x + 1, y};
      case Dir::South: return {x, y + 1};
      case Dir::West: return {x - 1, y};
    }
    return *this;
  }
};

struct Cell {
  CellKind kind{CellKind::Void};
  Edge edges[kDirCount]{};
  /// Which rune inscription is carved here, if any (§8.3). 0 is none.
  std::uint16_t inscription_id{0};
  /// Item instance ids resting in this cell. A vector, not a set: iteration
  /// order is simulation-visible and §5.1 forbids order-dependent iteration
  /// over unordered containers.
  std::vector<std::uint16_t> contents{};
};

/// A dungeon level as a cell graph.
///
/// Authored test levels and M0's corridor load from `level.gloam`. Generated
/// levels never touch disk — the seed IS the level (§12).
class Level {
 public:
  Level() = default;

  Level(std::int32_t width, std::int32_t height)
      : m_width{width < 0 ? 0 : width},
        m_height{height < 0 ? 0 : height},
        m_cells(static_cast<std::size_t>(m_width) * static_cast<std::size_t>(m_height)) {}

  [[nodiscard]] auto width() const noexcept -> std::int32_t { return m_width; }
  [[nodiscard]] auto height() const noexcept -> std::int32_t { return m_height; }
  [[nodiscard]] auto cell_count() const noexcept -> std::size_t { return m_cells.size(); }

  [[nodiscard]] auto in_bounds(Coord c) const noexcept -> bool {
    return c.x >= 0 && c.y >= 0 && c.x < m_width && c.y < m_height;
  }

  [[nodiscard]] auto index_of(Coord c) const noexcept -> std::size_t {
    return static_cast<std::size_t>(c.y) * static_cast<std::size_t>(m_width) +
           static_cast<std::size_t>(c.x);
  }

  [[nodiscard]] auto coord_of(std::size_t index) const noexcept -> Coord {
    if (m_width <= 0) return {};
    return {static_cast<std::int32_t>(index % static_cast<std::size_t>(m_width)),
            static_cast<std::int32_t>(index / static_cast<std::size_t>(m_width))};
  }

  /// Out-of-bounds reads return a shared void cell rather than trapping, so
  /// callers walking off the edge of the map behave like callers walking into
  /// rock. Writes are bounds-checked separately by `at_mut`.
  [[nodiscard]] auto at(Coord c) const -> const Cell& {
    if (!in_bounds(c)) return void_cell();
    return m_cells[index_of(c)];
  }

  [[nodiscard]] auto at_mut(Coord c) -> Cell* {
    if (!in_bounds(c)) return nullptr;
    return &m_cells[index_of(c)];
  }

  [[nodiscard]] auto navigable(Coord c) const -> bool { return at(c).kind == CellKind::Floor; }

  /// The edge leaving `c` in direction `d`.
  [[nodiscard]] auto edge(Coord c, Dir d) const -> const Edge& {
    return at(c).edges[static_cast<std::size_t>(d)];
  }

  /// Sets BOTH sides of the boundary between `c` and its neighbour in `d`.
  ///
  /// The two-sided write is the point. An edge stored once per cell can
  /// disagree with its twin, and a disagreeing edge means sound crosses a wall
  /// in one direction only — a bug that reads as a haunted level rather than as
  /// a data error. `symmetric()` asserts the invariant this maintains.
  void link(Coord c, Dir d, const Edge& e) {
    if (auto* here = at_mut(c)) here->edges[static_cast<std::size_t>(d)] = e;
    if (auto* there = at_mut(c.step(d))) there->edges[static_cast<std::size_t>(opposite(d))] = e;
  }

  /// Carves a run of floor cells and opens the edges between them. The helper
  /// M0's corridor is built from.
  void carve(Coord from, Dir d, int cells);

  /// True when every edge equals its twin across the boundary.
  ///
  /// This is the precondition for §13.3's "line of sight is symmetric" property
  /// — LOS symmetry is a theorem about a symmetric graph, not a coincidence, so
  /// the test asserts this first and the property second.
  [[nodiscard]] auto symmetric() const -> bool;

  /// The neighbour reachable from `c` by walking through `d`, if the edge is
  /// passable and the destination is navigable.
  [[nodiscard]] auto walk(Coord c, Dir d) const -> std::optional<Coord>;

 private:
  [[nodiscard]] static auto void_cell() -> const Cell&;

  std::int32_t m_width{};
  std::int32_t m_height{};
  std::vector<Cell> m_cells{};
};

}  // namespace gloam
