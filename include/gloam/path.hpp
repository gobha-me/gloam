#pragma once

/// SPEC §6.1, §5.2 — the one way a monster paths.
///
/// §5.2 says both play modes share "same AI, same patrols, same perception, same
/// pathfinding". That is the only occurrence of the word in the whole design
/// document, and until this header existed it asserted that two modes share a
/// thing that does not exist. Three of §6.1's five tells need it: "leaves the
/// patrol route, walks to the last known position", "direct pursuit", and "walks
/// back to the patrol route and resumes".
///
///
/// A BFS OVER `Level::walk`, AND DELIBERATELY NOT `propagate_noise`
///
/// `noise.hpp` is the obvious model and the wrong one to reuse. Noise is a
/// uniform-cost search over `Edge::conducts_sound()`, which is TRUE for a closed
/// door — §6.2 attenuates it by 40 rather than silencing it. Movement is a
/// breadth-first search over `Level::walk`, which is FALSE for that same door.
/// One graph, two readings (§12), and a monster that could path through a closed
/// door because sound can is precisely the bug a shared function would produce.
/// `test/21path/` pins the pair: the same level, refused closed and walked open.
///
///
/// ROOTED AT THE TARGET, WHICH IS THE WHOLE SHARED-FIELD DISCIPLINE
///
/// A field rooted at the monster answers one monster's question. A field rooted
/// at the DESTINATION answers every monster's, and `advance` reads it once per
/// monster exactly as it already reads one noise field per tick — the trick
/// `world.cpp` records paying 13.6 ms against a 4 ms budget to learn.
///
/// It is licensed by a theorem rather than by hope: `Level::walk` is symmetric
/// between two navigable cells, so descending a target-rooted field really is a
/// shortest path from the monster. `test/21path/` asserts `d(a,b) == d(b,a)`
/// over every navigable pair, the way §13.3 asserts line-of-sight symmetry, for
/// the same reason — a property the code depends on is asserted, not assumed.
///
/// THE ONE ASYMMETRY, and the reason `propagate_distance` refuses a source it
/// cannot stand on: `Level::walk` checks `navigable` on the DESTINATION only, so
/// a body can walk out of solid rock but never into it. A field rooted in rock
/// would therefore reach cells that cannot reach it back, and every descent
/// against it would be a lie. Refusing the source closes that, and it is also
/// the answer to "the party is standing in rock".
///
///
/// INTEGERS, NO TUNING, NO RNG
///
/// Every step costs one. There is no tunable here and no draw, so pursuit is
/// deterministic without needing a stream (§5.1), and adding a movement cost
/// later would be a `Tuning` decision made in the open rather than a constant
/// smuggled in here.

#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

#include "gloam/level.hpp"

namespace gloam {

/// No path. `NoiseField` can use 0 for "never reached" because zero loudness IS
/// inaudible; zero distance is a real answer — it is the source — so this needs
/// a sentinel of its own.
///
/// `INT32_MAX` rather than a negative value on purpose: it keeps the storage a
/// flat vector with no per-cell discriminant, and it makes "unreachable is
/// farther than anywhere" true by arithmetic, so a caller comparing distances
/// needs no special case. It is not producible — a reached distance never
/// exceeds `cell_count() - 1`, and a level with two billion cells is not
/// constructible — which `test/21path/` asserts rather than assumes.
inline constexpr std::int32_t kUnreachable = std::numeric_limits<std::int32_t>::max();

/// Steps to the nearest source, at every cell of a level.
///
/// Cells no source can reach read `kUnreachable`, as do cells outside the level
/// and reads against a level the field was not built for — the same "out of
/// bounds is survivable, not undefined" idiom as `Level::at`'s void cell.
class DistanceField {
 public:
  DistanceField() = default;
  explicit DistanceField(const Level& level)
      : m_width{level.width()}, m_distance(level.cell_count(), kUnreachable) {}

  [[nodiscard]] auto at(const Level& level, Coord c) const -> std::int32_t {
    // THE WIDTH CHECK IS WHAT MAKES THE SENTENCE ABOVE TRUE. A size check alone
    // lets a field built for a 10x10 answer a 4x4's question with an in-range
    // number: `index_of` is `y * width + x`, so a narrower level maps the same
    // coordinate to a different cell and the answer is confidently wrong rather
    // than refused. Measured: a field built for 10x10 reported distance 2 at
    // (3,2) against a 4x4 level, where its own level says 5.
    if (level.width() != m_width) return kUnreachable;
    if (!level.in_bounds(c)) return kUnreachable;
    const auto i = level.index_of(c);
    return i < m_distance.size() ? m_distance[i] : kUnreachable;
  }

  /// Prefer this to comparing against `kUnreachable` at the call site. A caller
  /// that open-codes the comparison is a caller that will get it wrong once.
  [[nodiscard]] auto reached(const Level& level, Coord c) const -> bool {
    return at(level, c) != kUnreachable;
  }

  [[nodiscard]] auto raw() const noexcept -> const std::vector<std::int32_t>& { return m_distance; }

  friend auto propagate_distance(const Level&, std::span<const Coord>) -> DistanceField;

 private:
  std::int32_t m_width{};
  std::vector<std::int32_t> m_distance{};
};

/// Breadth-first from every source at once, outward through `Level::walk`.
///
/// MULTI-SOURCE IS THE GENERAL FORM, and it is what makes §6.1's "walks back to
/// the patrol route" one search instead of one per route cell: seed the field
/// with the whole route and descend, and "the nearest cell of the route" falls
/// out of the search rather than out of a loop over candidate targets.
///
/// Sources that are out of bounds or not navigable are DROPPED, not rejected —
/// a roster is data, and the caller that hands over a route half of which is
/// rock gets a field over the half that is floor. An empty result (no source
/// survived) is a field that reaches nothing, which every caller already has to
/// handle.
///
/// Deterministic: the frontier is a FIFO seeded in span order and expanded in
/// `Dir` wire order, so the result depends on no container's iteration order —
/// §5.1's third rule.
[[nodiscard]] auto propagate_distance(const Level& level, std::span<const Coord> sources)
    -> DistanceField;

/// The single-source form: a party, or a remembered position.
[[nodiscard]] auto propagate_distance(const Level& level, Coord source) -> DistanceField;

/// One step down the field, or `nullopt` at a source, off the field, or where no
/// path exists.
///
/// `prefer` is tried before `Dir` wire order. Callers pass the monster's current
/// facing, so a monster continues STRAIGHT through a tie — which is literally
/// §6.1's "direct pursuit", and costs one parameter. Without it every tied
/// monster in the game prefers north, which is a visible artefact on an open
/// floor rather than a neutral default.
///
/// `from` outside the level answers `nullopt` through `at()`'s own bounds check
/// rather than through a guard of its own — see the note in `path.cpp`.
///
/// The tie-break is free to be anything BECAUSE every step strictly decreases
/// the distance: no rule over a strictly decreasing sequence can produce a
/// cycle, so "which of two equally good steps" cannot turn into "walks in a
/// circle forever". That is the property that makes the choice a matter of
/// taste, and it is why it is stated here rather than defended at each caller.
[[nodiscard]] auto step_down(const DistanceField& field, const Level& level, Coord from, Dir prefer)
    -> std::optional<Dir>;

}  // namespace gloam
