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
///
///
/// LAZY, WHICH IS A MEMO AND NOT A SECOND PATHFINDER
///
/// `propagate_distance` used to expand the whole reachable component before
/// returning. Measured, because this file only moves on measurements: sixteen
/// monsters searching sixteen distinct stale targets on a 32x32 open level
/// cost 2,695 us on a GCC 14 Debug dev box and 5,462 us on a CI runner — 137%
/// of §11's 4 ms tick (gloam#36) — and every production reader then looked at
/// the monster's own cell and its four neighbours and threw the rest of the
/// search away.
///
/// So seeding and expanding are now different events. Construction seeds the
/// surviving sources and stops; `at` runs the same FIFO in the same `Dir`
/// wire order, assign-on-push, until the queried cell is settled or the
/// frontier runs dry. THE OBJECT IS THE COMPLETE TRUE FIELD AND THE STORAGE
/// IS A MEMO: a distance is final the moment it is pushed (every edge costs
/// one, so the first visit is the shortest one), expansion ORDER is a function
/// of the level and the sources and never of the query pattern, and a query
/// decides only how much of that fixed order has run. Partiality is therefore
/// not observable — a reachable cell is never READ as `kUnreachable`, because
/// the read itself finishes the proof — and that is the whole of what the
/// design leans on. The symmetry theorem `test/21path/` asserts over every
/// navigable pair keeps holding, because both sides of the comparison settle
/// to the same values however the pair was asked. And the SEARCHING exit in
/// `world.cpp` keys "the trail cannot be walked" on `kUnreachable`, which is
/// a BEHAVIOURAL reading: it must mean the map, never the memo, and it does —
/// `kUnreachable` is produced only by a frontier that ran dry, which is
/// exactly the eager field's answer.
///
/// None of this is a second pathfinder. There is one search, one predicate
/// (`Level::walk`), one wire order, one sentinel; what changed is WHEN the
/// work happens, not what the work is. A lazy field that answered any query
/// differently from the eager one would simply be a bug, and `test/21path/`
/// pins the equivalence by driving one field by queries alone and another to
/// completion and comparing every answer.

#include <cstddef>
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
///
/// The object logically IS the complete field; the storage is filled on
/// demand, by the queries themselves (the header essay gives the argument).
/// `at` stays `const` and the distance vector, frontier, read cursor and
/// exhaustion flag are `mutable` because this is memoization, not mutation:
/// two fields built from the same sources answer every identical query
/// identically, however different the query patterns that drove them. The
/// type stays copyable and movable, and a copy of a half-settled field is
/// safe — it carries its frontier, cursor and flag, and the two then expand
/// independently and answer identically.
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
    //
    // The checks never touch the search, which is what keeps a rejected read
    // free AND consequence-free: it expands nothing, so it cannot drain the
    // frontier for the queries that follow.
    if (level.width() != m_width) return kUnreachable;
    if (!level.in_bounds(c)) return kUnreachable;
    const auto i = level.index_of(c);
    if (i >= m_distance.size()) return kUnreachable;

    // THE MEMO LINE. A settled cell is a load. Anything else runs the eager
    // loop's own iteration until the cell is assigned — its distance is final
    // the moment it is pushed — or the frontier runs dry, which is the only
    // way an in-bounds cell of the field's own level reads `kUnreachable`.
    // Once dry, the flag answers later unassigned queries in O(1): there is
    // nothing left that could settle them.
    while (m_distance[i] == kUnreachable && !m_exhausted) expand_one(level);
    return m_distance[i];
  }

  /// Prefer this to comparing against `kUnreachable` at the call site. A caller
  /// that open-codes the comparison is a caller that will get it wrong once.
  [[nodiscard]] auto reached(const Level& level, Coord c) const -> bool {
    return at(level, c) != kUnreachable;
  }

  /// The complete field, completing it first if no query already did.
  ///
  /// `at` settles only what it is asked about, so a caller that wants the
  /// whole vector — the determinism tests; no library code is one — pays the
  /// rest of the search here, driven through `level`, which must be the
  /// field's own level exactly as it must be for `at`. That the signature
  /// grew the parameter is the laziness showing through: the eager field
  /// could answer this from storage, and the memo cannot. It also lost
  /// `noexcept` honestly: a COPY's frontier is not reserved to the cell count,
  /// so a drain can reallocate it.
  [[nodiscard]] auto raw(const Level& level) const -> const std::vector<std::int32_t>& {
    while (!m_exhausted) expand_one(level);
    return m_distance;
  }

  friend auto propagate_distance(const Level&, std::span<const Coord>) -> DistanceField;

 private:
  /// One iteration of the loop `propagate_distance` used to run to completion,
  /// now run on demand.
  ///
  /// `level` is the QUERIER'S, as `at`'s contract says — a field driven by a
  /// level it was not built for gets meaningless-but-safe answers, and the
  /// index guard below is what keeps them safe. The eager loop could never
  /// meet that case (expansion finished before any level could be
  /// substituted); the lazy one expands under whatever level it is handed, and
  /// a same-width but TALLER level walks to cells this field has no storage
  /// for. Without the guard that is a write past the vector, which the
  /// AddressSanitizer leg demonstrates and `test/21path/` drives.
  void expand_one(const Level& level) const {
    if (m_read == m_frontier.size()) {
      // Every pushed cell has been expanded, so every reachable cell is
      // assigned: the field is complete, and the flag is what makes a later
      // unassigned query O(1) instead of a walk over an empty frontier.
      m_exhausted = true;
      return;
    }

    const auto index = m_frontier[m_read++];
    const Coord here = level.coord_of(index);
    const std::int32_t next = m_distance[index] + 1;

    for (int d = 0; d < kDirCount; ++d) {
      // THE ONE MOVEMENT PREDICATE. `apply` refuses a party's step through it
      // and `patrol_step` walks a route through it; nothing here is allowed a
      // second opinion about whether a body fits through an edge.
      const auto destination = level.walk(here, static_cast<Dir>(d));
      if (!destination) continue;

      const auto neighbour = level.index_of(*destination);
      if (neighbour >= m_distance.size()) continue;  // a taller level's cell, not mine
      // Every edge costs one, so the first visit is the shortest one and there
      // is nothing to relax. That is the whole difference from
      // `propagate_noise` and it is why this needs no priority queue.
      if (m_distance[neighbour] != kUnreachable) continue;

      m_distance[neighbour] = next;
      m_frontier.push_back(neighbour);
    }
  }

  std::int32_t m_width{};
  // Everything the search writes is `mutable`, because `at` is a const query
  // that learns: filling storage on demand changes nothing a reader can
  // observe. That is the whole design, and the header essay defends it.
  mutable std::vector<std::int32_t> m_distance{};
  mutable std::vector<std::size_t> m_frontier{};
  mutable std::size_t m_read{0};
  mutable bool m_exhausted{true};
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
/// `Dir` wire order WHENEVER A QUERY DRIVES IT, so the result depends on no
/// container's iteration order and on no query pattern — §5.1's third rule,
/// kept under the laziness the header essay describes.
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
