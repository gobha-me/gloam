// SPEC §6.1, §5.2 — the distance field every translating tell walks down.
//
// AGENTS.md: "Test how code fails, not just that it produces the right output.
// Write the failure matrix first; the happy-path check is the last, least
// interesting test." This primitive is queried with a monster's memory and with
// a route out of a level file, so almost everything below is a target no caller
// promised was well formed.
//
// THE ONE CLAIM EVERY REFUSAL SHARES: an ill-formed query answers `kUnreachable`
// and `nullopt`. Never an out-of-bounds read, never a step, never signed
// overflow on a coordinate that arrived from data.

#include <catch2/catch_all.hpp>

#include <cstdint>
#include <limits>
#include <vector>

#include "gloam/noise.hpp"
#include "gloam/path.hpp"

using namespace gloam;

namespace {

auto open_room(int w, int h) -> Level {
  Level level{w, h};
  for (int y = 0; y < h; ++y) level.carve(Coord{0, y}, Dir::East, w);
  for (int x = 0; x < w; ++x) level.carve(Coord{x, 0}, Dir::South, h);
  return level;
}

/// Two rooms with one doorway between them, so the edge under test is the ONLY
/// way across. `kind`/`state` decide what that way is worth.
auto two_rooms(EdgeKind kind, EdgeState state) -> Level {
  Level level{7, 3};
  level.carve(Coord{0, 1}, Dir::East, 3);  // (0,1)..(2,1)
  level.carve(Coord{4, 1}, Dir::East, 3);  // (4,1)..(6,1)

  // The bridging cell, carved by hand so the two runs stay disconnected except
  // through the edge this level exists to vary.
  if (auto* cell = level.at_mut(Coord{3, 1})) cell->kind = CellKind::Floor;
  level.link(Coord{2, 1}, Dir::East, Edge{EdgeKind::Open, EdgeState::Open, 0, 0});
  level.link(Coord{3, 1}, Dir::East, Edge{kind, state, 0, 0});
  return level;
}

/// Every navigable cell of a level, for the property cases.
auto navigable_cells(const Level& level) -> std::vector<Coord> {
  std::vector<Coord> cells;
  for (std::size_t i = 0; i < level.cell_count(); ++i) {
    const auto c = level.coord_of(i);
    if (level.navigable(c)) cells.push_back(c);
  }
  return cells;
}

}  // namespace

// ── The failure matrix ──────────────────────────────────────────────────────

TEST_CASE("a level with no cells answers nothing and steps nowhere", "[path]") {
  // A default-constructed Level, and one whose extents were negative — the
  // constructor clamps those to zero rather than allocating a negative extent,
  // so both arrive here as the same empty graph.
  const Level empty{};
  const Level clamped{-4, -7};

  for (const Level* level : {&empty, &clamped}) {
    const auto field = propagate_distance(*level, Coord{0, 0});
    CHECK(field.raw().empty());
    CHECK(field.at(*level, Coord{0, 0}) == kUnreachable);
    CHECK_FALSE(field.reached(*level, Coord{0, 0}));
    CHECK_FALSE(step_down(field, *level, Coord{0, 0}, Dir::North).has_value());
  }
}

TEST_CASE("a source outside the level seeds nothing", "[path]") {
  const auto level = open_room(5, 5);
  const auto outside = GENERATE(Coord{-1, 0}, Coord{0, -1}, Coord{5, 0}, Coord{0, 5},
                                Coord{std::numeric_limits<std::int32_t>::min(),
                                      std::numeric_limits<std::int32_t>::min()},
                                Coord{std::numeric_limits<std::int32_t>::max(),
                                      std::numeric_limits<std::int32_t>::max()});

  // The INT32_MIN row is why this case is worth its line: a coordinate that
  // reaches `Coord::step` unguarded is signed overflow, and the ubsan leg of the
  // matrix is what turns that from a silent wrong answer into a failure.
  const auto field = propagate_distance(level, outside);
  INFO("source (" << outside.x << "," << outside.y << ")");
  for (const auto c : navigable_cells(level)) CHECK_FALSE(field.reached(level, c));
}

TEST_CASE("a source in solid rock seeds nothing, even beside floor", "[path]") {
  // `Level::walk` checks `navigable` on the DESTINATION only, so a body can walk
  // out of rock and never into it. A field rooted in rock would therefore reach
  // cells that cannot reach it back, and every descent against it would be a
  // lie — which is exactly the "party standing in rock" case.
  Level level{5, 3};
  level.carve(Coord{0, 1}, Dir::East, 5);
  REQUIRE_FALSE(level.navigable(Coord{2, 0}));  // rock, directly north of floor

  const auto field = propagate_distance(level, Coord{2, 0});
  for (const auto c : navigable_cells(level)) CHECK_FALSE(field.reached(level, c));
  CHECK_FALSE(field.reached(level, Coord{2, 0}));
}

TEST_CASE("an empty source list reaches nothing", "[path]") {
  const auto level = open_room(4, 4);
  const auto field = propagate_distance(level, std::span<const Coord>{});
  for (const auto c : navigable_cells(level)) CHECK_FALSE(field.reached(level, c));
}

TEST_CASE("invalid sources are dropped, not fatal, and the rest still seed", "[path]") {
  // A route out of a level file is data. Half of it being rock must degrade to
  // a field over the half that is floor, not to a refusal — the same reading
  // `dwell_at` takes of a `dwell` array that does not match its route.
  const auto level = open_room(5, 5);
  const std::vector<Coord> mixed{Coord{-3, 2}, Coord{0, 0}, Coord{99, 99}};
  const std::vector<Coord> valid{Coord{0, 0}};

  const auto from_mixed = propagate_distance(level, mixed);
  const auto from_valid = propagate_distance(level, valid);
  CHECK(from_mixed.raw() == from_valid.raw());
}

TEST_CASE("a repeated source is the same field as a single one", "[path]") {
  const auto level = open_room(5, 5);
  const std::vector<Coord> twice{Coord{1, 1}, Coord{1, 1}, Coord{1, 1}};
  CHECK(propagate_distance(level, twice).raw() == propagate_distance(level, Coord{1, 1}).raw());
}

TEST_CASE("a field read against a different level never reads out of bounds", "[path]") {
  // Fields outlive the expression that built them, and nothing in the type
  // system pairs one with its level. Both directions: a bigger level asks about
  // cells the field does not have, a smaller one indexes differently.
  const auto built_for = open_room(4, 4);
  const auto field = propagate_distance(built_for, Coord{0, 0});

  const auto bigger = open_room(9, 9);
  const auto smaller = open_room(2, 2);

  CHECK(field.at(bigger, Coord{8, 8}) == kUnreachable);
  CHECK(field.at(bigger, Coord{-1, -1}) == kUnreachable);
  CHECK_FALSE(step_down(field, bigger, Coord{8, 8}, Dir::North).has_value());
  // Not asserted for a VALUE against `smaller` — the indices genuinely overlap
  // and the answer is meaningless rather than wrong. What is asserted is that
  // asking is survivable.
  CHECK_NOTHROW(field.at(smaller, Coord{1, 1}));
}

TEST_CASE("a wall makes a room unreachable rather than far away", "[path]") {
  Level level{7, 3};
  level.carve(Coord{0, 1}, Dir::East, 3);
  level.carve(Coord{4, 1}, Dir::East, 3);
  const auto field = propagate_distance(level, Coord{0, 1});

  CHECK(field.at(level, Coord{2, 1}) == 2);
  for (int x = 4; x < 7; ++x) {
    INFO("cell (" << x << ",1)");
    CHECK_FALSE(field.reached(level, Coord{x, 1}));
  }
}

TEST_CASE("a closed door stops a body and does not stop a sound", "[path]") {
  // THE CASE THIS PRIMITIVE EXISTS SEPARATELY FOR. §6.2 attenuates a closed door
  // by 40; it does not silence it. §12's "one graph, two readings" means the
  // very same edge is impassable and audible at once, so a pathfinder that
  // reused `propagate_noise` would walk a monster through a shut door.
  const auto closed = two_rooms(EdgeKind::Door, EdgeState::Closed);
  const auto open = two_rooms(EdgeKind::Door, EdgeState::Open);

  const auto blocked = propagate_distance(closed, Coord{0, 1});
  const auto through = propagate_distance(open, Coord{0, 1});

  CHECK_FALSE(blocked.reached(closed, Coord{6, 1}));
  CHECK(through.at(open, Coord{6, 1}) == 6);

  // And the same edge, read as sound, AT §6.2'S OWN NUMBERS rather than at
  // tuning chosen to make the point: a sting of 90 crosses five open cells (−2
  // each) and the shut door (−40) and still arrives at 40. The body cannot get
  // there and the sound can, through one `Edge`, in one `Level`.
  const auto heard = propagate_noise(closed, Coord{0, 1}, 90, kDefaultTuning);
  CHECK(heard.at(closed, Coord{6, 1}) == 40);
}

TEST_CASE("a locked door is a closed door to a body", "[path]") {
  const auto locked = two_rooms(EdgeKind::Door, EdgeState::Locked);
  const auto field = propagate_distance(locked, Coord{0, 1});
  CHECK_FALSE(field.reached(locked, Coord{6, 1}));
}

TEST_CASE("a doorway with no door is walked like open floor", "[path]") {
  const auto doorway = two_rooms(EdgeKind::Doorway, EdgeState::Open);
  const auto field = propagate_distance(doorway, Coord{0, 1});
  CHECK(field.at(doorway, Coord{6, 1}) == 6);
}

TEST_CASE("step_down refuses a source, an unreachable cell and a cell in rock", "[path]") {
  Level level{7, 3};
  level.carve(Coord{0, 1}, Dir::East, 3);
  level.carve(Coord{4, 1}, Dir::East, 3);
  const auto field = propagate_distance(level, Coord{0, 1});

  CHECK_FALSE(step_down(field, level, Coord{0, 1}, Dir::East).has_value());   // the source
  CHECK_FALSE(step_down(field, level, Coord{5, 1}, Dir::East).has_value());   // other room
  CHECK_FALSE(step_down(field, level, Coord{2, 0}, Dir::East).has_value());   // rock
  CHECK_FALSE(step_down(field, level, Coord{-1, 1}, Dir::East).has_value());  // off the grid
  CHECK_FALSE(step_down(field, level, Coord{std::numeric_limits<std::int32_t>::min(), 1},
                        Dir::East)
                  .has_value());
}

TEST_CASE("a single navigable cell is a field of one and a step of none", "[path]") {
  Level level{1, 1};
  if (auto* cell = level.at_mut(Coord{0, 0})) cell->kind = CellKind::Floor;

  const auto field = propagate_distance(level, Coord{0, 0});
  CHECK(field.at(level, Coord{0, 0}) == 0);
  // Four neighbour reads, every one of them off the grid: this is the case that
  // would catch an expansion that indexed before it bounds-checked.
  CHECK_FALSE(step_down(field, level, Coord{0, 0}, Dir::North).has_value());
}

// ── The properties ──────────────────────────────────────────────────────────

TEST_CASE("the tie-break prefers the given direction, then wire order", "[path][property]") {
  // An open room, and a target diagonally opposite: two descents are always
  // equally good, so the choice is entirely the tie-break's. Without `prefer`
  // every tied monster in the game would step north.
  const auto level = open_room(5, 5);
  const auto field = propagate_distance(level, Coord{0, 0});
  const Coord from{2, 2};
  REQUIRE(field.at(level, from) == 4);

  CHECK(step_down(field, level, from, Dir::North) == Dir::North);
  CHECK(step_down(field, level, from, Dir::West) == Dir::West);

  // South and East both lead AWAY, so neither can be honoured: the result falls
  // back to wire order, and North is the first direction that descends. This is
  // the half that proves `prefer` is a preference and not an instruction.
  CHECK(step_down(field, level, from, Dir::South) == Dir::North);
  CHECK(step_down(field, level, from, Dir::East) == Dir::North);
}

TEST_CASE("distance is symmetric over every navigable pair", "[path][property]") {
  // THE THEOREM THE WHOLE DESIGN RESTS ON. A field is rooted at the TARGET and
  // descended from the monster, which is a shortest path only if `walk` is
  // symmetric between two navigable cells. §13.3 asserts line-of-sight symmetry
  // for the same reason and in the same shape: a property the code depends on is
  // asserted, not assumed.
  auto level = open_room(6, 6);
  level.link(Coord{2, 1}, Dir::South, Edge{EdgeKind::Wall, EdgeState::Open, 0, 0});
  level.link(Coord{3, 3}, Dir::East, Edge{EdgeKind::Door, EdgeState::Closed, 0, 0});
  level.link(Coord{4, 4}, Dir::North, Edge{EdgeKind::Wall, EdgeState::Open, 0, 0});
  REQUIRE(level.symmetric());

  const auto cells = navigable_cells(level);
  int reachable_pairs = 0;
  for (const auto a : cells) {
    const auto from_a = propagate_distance(level, a);
    for (const auto b : cells) {
      const auto from_b = propagate_distance(level, b);
      INFO("(" << a.x << "," << a.y << ") -> (" << b.x << "," << b.y << ")");
      REQUIRE(from_a.at(level, b) == from_b.at(level, a));
      if (from_a.reached(level, b)) ++reachable_pairs;
    }
  }
  // Non-vacuity: a field that reached nothing would satisfy the symmetry above
  // perfectly.
  CHECK(reachable_pairs > 0);
}

TEST_CASE("every reached cell has a neighbour exactly one step closer", "[path][property]") {
  // THE PROPERTY THAT KILLS THE REJECTED SHORTCUT. gloam#32 rejects greedy "step
  // to the neighbour that reduces Chebyshev range" because it sticks in local
  // minima on a walled grid. This asserts the opposite for a BFS field: from
  // anywhere reachable, a descent EXISTS — so `step_down` never returns nullopt
  // where a path exists, and a monster cannot get stuck in a pocket.
  auto level = open_room(8, 8);
  // A U-shaped pocket: the greedy step from inside the U walks into the closed
  // end, which is exactly the arrangement that reads to a player as broken AI.
  for (int y = 1; y <= 5; ++y) {
    level.link(Coord{2, y}, Dir::East, Edge{EdgeKind::Wall, EdgeState::Open, 0, 0});
    level.link(Coord{5, y}, Dir::West, Edge{EdgeKind::Wall, EdgeState::Open, 0, 0});
  }
  level.link(Coord{3, 1}, Dir::North, Edge{EdgeKind::Wall, EdgeState::Open, 0, 0});
  level.link(Coord{4, 1}, Dir::North, Edge{EdgeKind::Wall, EdgeState::Open, 0, 0});

  const Coord target{0, 0};
  const auto field = propagate_distance(level, target);

  int descents = 0;
  for (const auto c : navigable_cells(level)) {
    const auto d = field.at(level, c);
    if (d == kUnreachable || d == 0) continue;
    const auto dir = step_down(field, level, c, Dir::North);
    INFO("cell (" << c.x << "," << c.y << ") at distance " << d);
    REQUIRE(dir.has_value());
    const auto next = level.walk(c, *dir);
    REQUIRE(next.has_value());
    REQUIRE(field.at(level, *next) == d - 1);
    ++descents;
  }
  CHECK(descents > 0);

  // And the walk terminates: from the deepest cell of the pocket, descending
  // reaches the target in exactly its distance. A greedy stepper would loop.
  const Coord deep{4, 5};
  auto cursor = deep;
  auto facing = Dir::North;
  int steps = 0;
  const auto expected = field.at(level, deep);
  REQUIRE(expected != kUnreachable);
  while (cursor != target) {
    const auto dir = step_down(field, level, cursor, facing);
    REQUIRE(dir.has_value());
    cursor = *level.walk(cursor, *dir);
    facing = *dir;
    REQUIRE(++steps <= expected);
  }
  CHECK(steps == expected);
}

TEST_CASE("kUnreachable is a sentinel, never a computed distance", "[path][property]") {
  // The sentinel is INT32_MAX rather than a negative number so that "unreachable
  // is farther than anywhere" holds by arithmetic. That only stays true while it
  // cannot be produced — a reached distance is bounded by the cell count.
  constexpr int kSide = 32;
  auto level = open_room(kSide, kSide);
  const auto field = propagate_distance(level, Coord{0, 0});

  const auto cells = static_cast<std::int32_t>(level.cell_count());
  for (const auto d : field.raw()) {
    if (d == kUnreachable) continue;
    REQUIRE(d >= 0);
    REQUIRE(d < cells);
  }
  CHECK(field.at(level, Coord{kSide - 1, kSide - 1}) == 2 * (kSide - 1));
}

TEST_CASE("the same query answers identically every time", "[path][property]") {
  // §5.1 forbids order-dependent iteration. The frontier is a FIFO expanded in
  // Dir wire order, so this is a statement about the implementation staying that
  // way rather than a hope about the container.
  auto level = open_room(6, 6);
  level.link(Coord{3, 2}, Dir::East, Edge{EdgeKind::Wall, EdgeState::Open, 0, 0});
  const std::vector<Coord> sources{Coord{0, 0}, Coord{5, 5}};

  const auto first = propagate_distance(level, sources);
  for (int i = 0; i < 8; ++i) CHECK(propagate_distance(level, sources).raw() == first.raw());
}

// ── The golden, last ────────────────────────────────────────────────────────

TEST_CASE("a multi-source field is the distance to the NEAREST source", "[path]") {
  // The form §6.1's "walks back to the patrol route" uses: seed the whole route
  // and descend, and "nearest cell of the route" falls out of the search rather
  // than out of a loop over candidates.
  const auto level = open_room(9, 1);
  const std::vector<Coord> route{Coord{0, 0}, Coord{8, 0}};
  const auto field = propagate_distance(level, route);

  const std::vector<std::int32_t> expected{0, 1, 2, 3, 4, 3, 2, 1, 0};
  CHECK(field.raw() == expected);

  // And a monster in the middle walks to whichever end its facing prefers,
  // because from (4,0) both are four steps away.
  CHECK(step_down(field, level, Coord{4, 0}, Dir::East) == Dir::East);
  CHECK(step_down(field, level, Coord{4, 0}, Dir::West) == Dir::West);
}
