// SPEC §6.4 and §6.1 — patrol routes, the movement pump, and the two tells that
// are a head turn.
//
// AGENTS.md: "Test how code fails, not just that it produces the right output.
// Write the failure matrix first; the happy-path check is the last, least
// interesting test." §6.4 is three sentences and `SCHEMAS.md`'s `patrol` record
// cannot express a single one of this file's invariants, so almost everything
// here is data a loader could hand the pump and did not have to be well formed.
//
// THE ONE CLAIM EVERY REFUSAL BELOW SHARES: a malformed route makes a monster
// STAND STILL. Never a teleport, never a step through a wall, never an
// out-of-bounds read. `advance` does not call `valid_route` — `world.hpp` says
// why — so this file is what makes skipping it safe.

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "gloam/world.hpp"

using namespace gloam;

namespace {

constexpr std::uint64_t kSeed = 0x9E3779B97F4A7C15ULL;

/// A plus-shaped level: M0's corridor, four cells long with one intersection.
/// The side passage runs (3,0)..(3,4) through the intersection at (3,2), which
/// is the scene §6 opens with and #8's gate asks about.
[[nodiscard]] auto corridor_level() -> Level {
  Level level{7, 5};
  level.carve(Coord{1, 2}, Dir::East, 5);
  level.carve(Coord{3, 2}, Dir::North, 3);
  level.carve(Coord{3, 2}, Dir::South, 3);
  return level;
}

/// The side passage, north to south, as a well-formed route.
[[nodiscard]] auto crossing_route() -> std::vector<Coord> {
  return {Coord{3, 0}, Coord{3, 1}, Coord{3, 2}, Coord{3, 3}, Coord{3, 4}};
}

/// One monster on `route`, deaf and blind enough that nothing perturbs its
/// patrol: `Acuity::Dull` with the party parked out of sight and out of earshot.
/// Isolating the pump from §6.1 is the point — awareness has its own file.
[[nodiscard]] auto patrolling_world(std::vector<Coord> route, std::vector<std::uint8_t> dwell,
                                    Coord start) -> World {
  Monster m{};
  m.at = start;
  m.kind = MonsterKind{Acuity::Dull, false};
  m.patrol.route = std::move(route);
  m.patrol.dwell = std::move(dwell);

  auto w = make_world(kSeed, corridor_level(), {m});
  w.party = Coord{1, 2};
  w.lamp_level = 0;  // doused: §6.3's pillar keeps the monster UNAWARE
  return w;
}

/// Zero dwell of the right length, so a route is well formed by default.
[[nodiscard]] auto no_dwell(std::size_t n) -> std::vector<std::uint8_t> {
  return std::vector<std::uint8_t>(n, 0);
}

/// Tuning with §6.4's jitter switched off. `tuning.hpp` calls this the lever
/// every exact-sequence test in this file pulls: at zero the pump takes no draw
/// at all, so positions are a closed form rather than a distribution.
[[nodiscard]] auto no_jitter() -> Tuning {
  Tuning t = kDefaultTuning;
  t.patrol_idle_jitter_ticks = 0;
  return t;
}

/// Positions of monster 0 over `ticks` ticks, sampled after each tick.
[[nodiscard]] auto walk_for(World& w, const Tuning& t, int ticks) -> std::vector<Coord> {
  std::vector<Coord> seen{};
  seen.reserve(static_cast<std::size_t>(ticks));
  for (int i = 0; i < ticks; ++i) {
    advance(w, t);
    seen.push_back(w.monsters[0].at);
  }
  return seen;
}

/// True when the monster never left `start`.
[[nodiscard]] auto never_moved(World& w, const Tuning& t, int ticks) -> bool {
  const auto start = w.monsters[0].at;
  for (const auto c : walk_for(w, t, ticks)) {
    if (!(c == start)) return false;
  }
  return true;
}

}  // namespace

// ── The failure matrix ──────────────────────────────────────────────────────

TEST_CASE("a route that does not describe a walk makes a monster stand still", "[patrol]") {
  const auto t = no_jitter();
  const auto route = crossing_route();

  SECTION("empty — this is how 'does not patrol' is spelled") {
    auto w = patrolling_world({}, {}, Coord{3, 0});
    CHECK(never_moved(w, t, 40));
  }

  SECTION("one cell — malformed, NOT a synonym for empty") {
    // `world.hpp` makes empty the one representation of standing still. A
    // one-cell route is data that meant to say something else, and guessing
    // which is worse than refusing.
    auto w = patrolling_world({Coord{3, 0}}, {0}, Coord{3, 0});
    CHECK(never_moved(w, t, 40));
    CHECK_FALSE(valid_route(w.level, w.monsters[0].patrol.route, w.monsters[0].patrol.dwell));
  }

  SECTION("dwell shorter than route") {
    auto w = patrolling_world(route, {0, 0}, Coord{3, 0});
    // The pump is total over this: a missing dwell reads as an absent pause, so
    // the monster still walks. It is `valid_route` that refuses it, which is
    // the division of labour `world.hpp` describes.
    CHECK_FALSE(valid_route(w.level, w.monsters[0].patrol.route, w.monsters[0].patrol.dwell));
    CHECK(w.monsters[0].at == Coord{3, 0});
    static_cast<void>(walk_for(w, t, 40));
    CHECK(w.level.navigable(w.monsters[0].at));
  }

  SECTION("dwell longer than route") {
    auto w = patrolling_world(route, no_dwell(9), Coord{3, 0});
    CHECK_FALSE(valid_route(w.level, w.monsters[0].patrol.route, w.monsters[0].patrol.dwell));
    static_cast<void>(walk_for(w, t, 40));
    CHECK(w.level.navigable(w.monsters[0].at));
  }

  SECTION("a cell out of bounds") {
    auto w = patrolling_world({Coord{3, 0}, Coord{3, -1}}, {0, 0}, Coord{3, 0});
    CHECK_FALSE(valid_route(w.level, w.monsters[0].patrol.route, w.monsters[0].patrol.dwell));
    CHECK(never_moved(w, t, 40));
  }

  SECTION("a cell in solid rock") {
    auto w = patrolling_world({Coord{3, 0}, Coord{0, 0}}, {0, 0}, Coord{3, 0});
    CHECK_FALSE(valid_route(w.level, w.monsters[0].patrol.route, w.monsters[0].patrol.dwell));
    CHECK(never_moved(w, t, 40));
  }

  SECTION("two consecutive cells that are not adjacent — a teleport in the data") {
    auto w = patrolling_world({Coord{3, 0}, Coord{3, 3}}, {0, 0}, Coord{3, 0});
    CHECK_FALSE(valid_route(w.level, w.monsters[0].patrol.route, w.monsters[0].patrol.dwell));
    // THE IMPORTANT HALF. `Level::walk` alone would have stepped one cell south
    // toward (3,3) and drifted off the route; requiring the destination to BE
    // the waypoint is what turns a data error into a stationary monster.
    CHECK(never_moved(w, t, 40));
  }

  SECTION("a cell repeated back to back") {
    auto w = patrolling_world({Coord{3, 0}, Coord{3, 0}, Coord{3, 1}}, {0, 0, 0}, Coord{3, 0});
    CHECK_FALSE(valid_route(w.level, w.monsters[0].patrol.route, w.monsters[0].patrol.dwell));
  }
}

TEST_CASE("a route across an edge a body cannot cross makes a monster stand still", "[patrol]") {
  const auto t = no_jitter();

  SECTION("a wall between two floor cells") {
    auto w = patrolling_world({Coord{3, 1}, Coord{3, 2}}, {0, 0}, Coord{3, 1});
    w.level.link(Coord{3, 1}, Dir::South, Edge{EdgeKind::Wall, EdgeState::Open, 0, 0});
    CHECK_FALSE(valid_route(w.level, w.monsters[0].patrol.route, w.monsters[0].patrol.dwell));
    CHECK(never_moved(w, t, 40));
  }

  SECTION("a CLOSED door — refused, and the same route with it OPEN is walked") {
    // The predicate that gates the party gates the monster. That is the whole
    // reason `apply` was moved onto `Level::walk` in the same commit as this
    // file: two copies of "can a body pass" is a bug nobody thinks to test.
    auto closed = patrolling_world({Coord{3, 1}, Coord{3, 2}}, {0, 0}, Coord{3, 1});
    closed.level.link(Coord{3, 1}, Dir::South, Edge{EdgeKind::Door, EdgeState::Closed, 0, 0});
    CHECK_FALSE(
        valid_route(closed.level, closed.monsters[0].patrol.route, closed.monsters[0].patrol.dwell));
    CHECK(never_moved(closed, t, 40));

    auto open = patrolling_world({Coord{3, 1}, Coord{3, 2}}, {0, 0}, Coord{3, 1});
    open.level.link(Coord{3, 1}, Dir::South, Edge{EdgeKind::Door, EdgeState::Open, 0, 0});
    CHECK(valid_route(open.level, open.monsters[0].patrol.route, open.monsters[0].patrol.dwell));
    CHECK_FALSE(never_moved(open, t, 40));
  }

  SECTION("a LOCKED door is a closed door to a body") {
    auto w = patrolling_world({Coord{3, 1}, Coord{3, 2}}, {0, 0}, Coord{3, 1});
    w.level.link(Coord{3, 1}, Dir::South, Edge{EdgeKind::Door, EdgeState::Locked, 0, 0});
    CHECK_FALSE(valid_route(w.level, w.monsters[0].patrol.route, w.monsters[0].patrol.dwell));
    CHECK(never_moved(w, t, 40));
  }
}

TEST_CASE("a cursor that indexes nothing makes a monster stand still", "[patrol]") {
  const auto t = no_jitter();
  const auto route = crossing_route();

  // These arrive from a `level.gloam` that disagrees with itself. A stationary
  // monster is a reportable symptom; an out-of-bounds read is a crash somewhere
  // else entirely, which is the outcome this refuses to have.
  const auto cursor = GENERATE(std::int32_t{999}, std::int32_t{-1}, INT32_MIN, INT32_MAX,
                               std::int32_t{5});
  INFO("waypoint " << cursor);
  auto w = patrolling_world(route, no_dwell(5), Coord{3, 0});
  w.monsters[0].patrol.waypoint = cursor;
  CHECK(never_moved(w, t, 40));
}

TEST_CASE("a monster standing off its own route walks back to it", "[patrol]") {
  // gloam#32's gap, CASHED. This case used to read "a monster standing off its
  // own route never moves", and said: "When #32 lands this case goes red, which
  // is the intended way to find it." It landed on 2026-08-04, and it did.
  //
  // ONE CORRECTION TO HOW IT WENT RED, worth recording because it nearly did
  // not. The old case started the monster at (1,2) — which is the cell
  // `patrolling_world` parks the PARTY on. Under the pathfinder that monster
  // escalates to HUNTING within three ticks and then holds, because the arrival
  // rule stops it where it already stands. It would have gone on passing, for a
  // reason with nothing to do with its name. The start below is off the route
  // AND off the party, which is what the case always meant.
  const auto t = no_jitter();
  auto w = patrolling_world(crossing_route(), no_dwell(5), Coord{5, 2});

  // Two steps west along the main corridor to reach (3,2), which is `route[2]`
  // — the nearest cell of its own route, and the one the multi-source field
  // finds without anybody looping over candidates. One step per
  // `monster_move_ticks`, so the walk is four ticks long and three ticks of it
  // are visible here.
  const auto home = walk_for(w, t, 3);
  CHECK(home == std::vector<Coord>{Coord{4, 2}, Coord{4, 2}, Coord{3, 2}});

  const auto& m = w.monsters[0];
  REQUIRE(m.mind.state == Awareness::Unaware);  // it walked home, it did not chase
  CHECK(m.at == Coord{3, 2});
  CHECK(m.patrol.waypoint == 2);  // the index of the cell it arrived on

  // And it resumes the ping-pong from there rather than standing on it: §6.1's
  // tell is "walks back to the patrol route AND RESUMES". South down the side
  // passage, which is where `route[2] -> route[3]` goes.
  const auto after = walk_for(w, t, 4);
  CHECK(after == std::vector<Coord>{Coord{3, 2}, Coord{3, 3}, Coord{3, 3}, Coord{3, 4}});
}

TEST_CASE("degenerate tunables do not break the pump", "[patrol]") {
  const auto route = crossing_route();

  SECTION("monster_move_ticks at or below zero is clamped to every tick") {
    for (const std::int32_t period : {std::int32_t{0}, std::int32_t{-1}, INT32_MIN}) {
      INFO("monster_move_ticks " << period);
      Tuning t = no_jitter();
      t.monster_move_ticks = period;
      auto w = patrolling_world(route, no_dwell(5), Coord{3, 0});
      const auto seen = walk_for(w, t, 4);
      // Clamped to 1, so it steps every tick and never faster than the route.
      CHECK(seen == std::vector<Coord>{Coord{3, 1}, Coord{3, 2}, Coord{3, 3}, Coord{3, 4}});
    }
  }

  SECTION("monster_move_ticks at INT32_MAX means it never steps again") {
    Tuning t = no_jitter();
    t.monster_move_ticks = INT32_MAX;
    auto w = patrolling_world(route, no_dwell(5), Coord{3, 0});
    advance(w, t);
    CHECK(w.monsters[0].at == Coord{3, 1});  // the first step is free
    for (int i = 0; i < 100; ++i) advance(w, t);
    CHECK(w.monsters[0].at == Coord{3, 1});
  }

  SECTION("patrol_idle_jitter_ticks at INT32_MAX does not overflow dwell_left") {
    Tuning t = kDefaultTuning;
    t.patrol_idle_jitter_ticks = INT32_MAX;
    auto w = patrolling_world(route, {1, 1, 1, 1, 1}, Coord{3, 0});
    for (int i = 0; i < 50; ++i) advance(w, t);
    CHECK(w.monsters[0].patrol.dwell_left >= 0);
    CHECK(w.level.navigable(w.monsters[0].at));
  }

  SECTION("a negative jitter takes no draw and adds nothing") {
    Tuning t = kDefaultTuning;
    t.patrol_idle_jitter_ticks = -5;
    auto w = patrolling_world(route, {3, 3, 3, 3, 3}, Coord{3, 0});
    const auto before = w.rng_state;
    for (int i = 0; i < 20; ++i) advance(w, t);
    CHECK(w.monsters[0].patrol.dwell_left >= 0);
    CHECK(w.rng_state == before);  // `Rng::range` returns `lo` when `hi < lo`
  }
}

TEST_CASE("a route that revisits a cell follows the index, not the cell", "[patrol]") {
  // The route goes south to the intersection, west, then back east — so (3,2)
  // appears twice. A pump that searched for its current cell in the route would
  // find the first match and loop forever between two waypoints.
  Tuning t = no_jitter();
  t.monster_move_ticks = 1;  // one step per tick, so the sequence IS the route
  const std::vector<Coord> route{Coord{3, 0}, Coord{3, 1}, Coord{3, 2},
                                 Coord{3, 3}, Coord{3, 2}, Coord{3, 1}};
  auto w = patrolling_world(route, no_dwell(6), Coord{3, 0});

  const auto seen = walk_for(w, t, 5);
  CHECK(seen == std::vector<Coord>{Coord{3, 1}, Coord{3, 2}, Coord{3, 3}, Coord{3, 2},
                                   Coord{3, 1}});
  CHECK(w.monsters[0].patrol.waypoint == 5);
}

TEST_CASE("valid_route accepts a well-formed route and refuses an empty pair asymmetrically",
          "[patrol]") {
  const auto level = corridor_level();
  CHECK(valid_route(level, crossing_route(), no_dwell(5)));

  // Empty route with a non-empty dwell is not "does not patrol", it is a
  // truncated record.
  const std::vector<std::uint8_t> some_dwell{1};
  CHECK(valid_route(level, {}, {}));
  CHECK_FALSE(valid_route(level, {}, some_dwell));
}

// ── The properties ──────────────────────────────────────────────────────────
//
// TEST-PLAN.md §3 has six property rows and NONE of them are about movement,
// because nothing moved when it was written. These are the rows §6.4 needs.

TEST_CASE("P1/P2: a monster is always on a navigable cell and never crosses an "
          "impassable edge",
          "[patrol][property]") {
  // Over EVERY route in the failure matrix, valid or not. The claim is about
  // the pump being total, so restricting it to well-formed data would test the
  // wrong thing.
  const auto t = kDefaultTuning;
  const std::vector<std::vector<Coord>> routes{
      {},
      {Coord{3, 0}},
      crossing_route(),
      {Coord{3, 0}, Coord{3, 3}},
      {Coord{3, 0}, Coord{0, 0}},
      {Coord{3, 0}, Coord{3, -1}},
      {Coord{1, 2}, Coord{2, 2}, Coord{3, 2}, Coord{4, 2}, Coord{5, 2}},
  };

  for (const auto& route : routes) {
    auto w = patrolling_world(route, no_dwell(route.size()), Coord{3, 0});
    auto previous = w.monsters[0].at;
    for (int i = 0; i < 200; ++i) {
      advance(w, t);
      const auto now = w.monsters[0].at;
      REQUIRE(w.level.navigable(now));  // P1

      if (now == previous) continue;
      bool reachable = false;  // P2
      for (int d = 0; d < kDirCount && !reachable; ++d) {
        const auto next = w.level.walk(previous, static_cast<Dir>(d));
        reachable = next && *next == now;
      }
      REQUIRE(reachable);
      previous = now;
    }
  }
}

TEST_CASE("P3: a monster never steps more often than monster_move_ticks allows",
          "[patrol][property]") {
  Tuning t = no_jitter();
  t.monster_move_ticks = 3;
  auto w = patrolling_world(crossing_route(), no_dwell(5), Coord{3, 0});

  constexpr int kTicks = 90;
  auto previous = w.monsters[0].at;
  int moves = 0;
  for (int i = 0; i < kTicks; ++i) {
    advance(w, t);
    if (!(w.monsters[0].at == previous)) ++moves;
    previous = w.monsters[0].at;
  }
  CHECK(moves > 0);  // a bound nothing satisfies is not a bound
  CHECK(moves <= kTicks / t.monster_move_ticks);
}

TEST_CASE("P4: a monster that never leaves UNAWARE stays on its own route",
          "[patrol][property]") {
  // RE-SCOPED BY gloam#32, AND THE INNER REQUIRE IS THE POINT. The old name was
  // "a monster's position is always a cell of its own route", which stopped
  // being true the moment a monster could leave the route to search — and this
  // case would have gone on PASSING, because its monster is Dull under a doused
  // lamp and never escalates. A property that silently narrows to its fixture is
  // worse than one that breaks: it keeps its name, keeps its green tick, and
  // stops covering the thing it is named after.
  //
  // So the precondition is now asserted rather than assumed. If a future
  // perception change makes this monster notice the party, this case goes red on
  // the state line instead of quietly measuring nothing. The companion claim —
  // that a monster which DID leave comes back — is P13 in `test/22pursuit/`.
  const auto t = kDefaultTuning;
  const auto route = crossing_route();
  auto w = patrolling_world(route, {2, 0, 0, 0, 2}, Coord{3, 0});

  for (int i = 0; i < 200; ++i) {
    advance(w, t);
    REQUIRE(w.monsters[0].mind.state == Awareness::Unaware);
    const auto at = w.monsters[0].at;
    REQUIRE(std::find(route.begin(), route.end(), at) != route.end());
  }
}

TEST_CASE("P5: the same seed patrols identically, a different one dwells differently",
          "[patrol][property]") {
  const auto t = kDefaultTuning;  // jitter ON: this is the case that needs it

  auto a = patrolling_world(crossing_route(), {4, 0, 0, 0, 4}, Coord{3, 0});
  auto b = patrolling_world(crossing_route(), {4, 0, 0, 0, 4}, Coord{3, 0});
  CHECK(walk_for(a, t, 200) == walk_for(b, t, 200));
  CHECK(world_hash(a) == world_hash(b));

  // A different seed reaches a different schedule. Asserted on the whole
  // sequence rather than on one sample, because a single tick can agree by
  // luck and 200 of them cannot.
  auto c = patrolling_world(crossing_route(), {4, 0, 0, 0, 4}, Coord{3, 0});
  c.seed ^= 0xFFFFULL;
  for (std::size_t i = 0; i < kStreamCount; ++i) {
    c.rng_state[i] = rng(c.seed, static_cast<Stream>(i + 1)).state();
  }
  CHECK(walk_for(c, t, 200) != walk_for(b, t, 200));
}

TEST_CASE("P6: the patrol stream is the ONLY stream a patrol touches", "[patrol][property]") {
  // §6.4: "drawn from the `patrol` RNG stream, NEVER the shared one." That
  // sentence is the whole of what the design settles about patrols, so it is
  // worth an assertion rather than a convention.
  const auto t = kDefaultTuning;
  auto w = patrolling_world(crossing_route(), {3, 1, 1, 1, 3}, Coord{3, 0});
  const auto before = w.rng_state;

  for (int i = 0; i < 1000; ++i) advance(w, t);

  constexpr auto kPatrol = static_cast<std::size_t>(Stream::Patrol) - 1;
  CHECK(w.rng_state[kPatrol] != before[kPatrol]);  // it drew, so the check below means something
  for (std::size_t i = 0; i < kStreamCount; ++i) {
    if (i == kPatrol) continue;
    INFO("stream index " << i);
    CHECK(w.rng_state[i] == before[i]);
  }
}

TEST_CASE("P7: a metronome returns to its start in closed form", "[patrol][property]") {
  // Legibility as a theorem. With no jitter and no authored dwell, an n-cell
  // ping-pong is 2(n-1) steps long, so the monster is back at route[0] after
  // exactly 2(n-1) * monster_move_ticks ticks — and the walk it takes getting
  // there repeats forever. If §6.4's "idle variation" ever leaks onto a
  // pauseless leg, this is what notices.
  Tuning t = no_jitter();
  t.monster_move_ticks = 2;

  const auto route = crossing_route();
  const auto n = static_cast<int>(route.size());
  const int period = 2 * (n - 1) * t.monster_move_ticks;

  auto w = patrolling_world(route, no_dwell(route.size()), route.front());
  const auto first = walk_for(w, t, period);
  CHECK(w.monsters[0].at == route.front());
  CHECK(w.monsters[0].patrol.waypoint == 0);

  // The second lap is the first lap. Asserted on the SEQUENCE rather than on
  // `reversed`, deliberately: `reversed` is still true here, because the flag
  // flips on the step that leaves waypoint 0 rather than on the one that
  // arrives. Pinning that in a test would pin an encoding — what a metronome
  // actually promises is that the walk repeats.
  CHECK(walk_for(w, t, period) == first);
}

TEST_CASE("P10: an authored dwell costs ticks ON TOP OF the step, not concurrently with it",
          "[patrol][property]") {
  // REGRESSION. The pump first ticked `dwell_left` and `move_cooldown` in
  // parallel, which makes the effective pause `max(dwell, monster_move_ticks)`
  // rather than the sum — so at the default period of 2, an authored dwell of 1
  // or 2 was a COMPLETE NO-OP. Measured at the time: dwell 0, 1 and 2 all gave
  // 20 moves in 40 ticks.
  //
  // Nothing caught it. Every other case in this file either sets dwell to zero
  // or asserts something a swallowed dwell still satisfies, and `world.hpp`'s
  // "ticks owed on arriving at that waypoint" is prose. The failure mode is the
  // quiet one: an author writes a pause, gets none, and has no diagnostic.
  Tuning t = no_jitter();
  t.monster_move_ticks = 2;

  constexpr int kTicks = 60;
  const auto route = crossing_route();

  int previous_moves = -1;
  for (std::uint8_t dwell = 0; dwell <= 4; ++dwell) {
    INFO("dwell " << static_cast<int>(dwell));
    auto w = patrolling_world(route, std::vector<std::uint8_t>(route.size(), dwell), Coord{3, 0});

    int moves = 0;
    auto at = w.monsters[0].at;
    for (int i = 0; i < kTicks; ++i) {
      advance(w, t);
      if (!(w.monsters[0].at == at)) ++moves;
      at = w.monsters[0].at;
    }

    // The gap between steps is period + dwell, so the step count follows.
    const int gap = t.monster_move_ticks + dwell;
    CHECK(moves == kTicks / gap);

    // And every increase in dwell must MOVE the number. This is the assertion
    // the original bug walked straight through: three consecutive dwell values
    // producing identical walks is exactly what "the pause is a no-op" looks
    // like from outside.
    if (previous_moves >= 0) CHECK(moves < previous_moves);
    previous_moves = moves;
  }
}

TEST_CASE("P8: the SUSPICIOUS tell halts for exactly one tick and turns the head",
          "[patrol][property]") {
  // §6.1: "patrol rhythm breaks — halt for one tick, head turns toward the
  // source." One tick, not a freeze — SEARCHING is where holding position
  // starts (P9).
  Tuning t = no_jitter();
  t.monster_move_ticks = 1;  // so a halt is visible as a missing step, not as a cooldown

  auto w = patrolling_world(crossing_route(), no_dwell(5), Coord{3, 0});
  w.monsters[0].kind = MonsterKind{Acuity::Keen, false};
  w.party = Coord{3, 4};  // in the passage, below the monster
  w.armour = Armour::Plate;
  w.monsters[0].facing = Dir::North;

  // Make a noise loud enough to be heard, and tick once.
  apply(w, replay::Event::Step, static_cast<std::uint16_t>(Dir::North), t);
  const auto before = w.monsters[0].at;
  advance(w, t);

  REQUIRE(w.monsters[0].mind.state == Awareness::Suspicious);
  CHECK(w.monsters[0].at == before);          // halted
  CHECK(w.monsters[0].facing == Dir::South);  // turned toward the source, which is south of it

  // ...and it is patrolling again on the very next tick.
  advance(w, t);
  CHECK_FALSE(w.monsters[0].at == before);
}

TEST_CASE("P9a: LOST_TRACK pins the position, and is now the only state that does",
          "[patrol][property]") {
  // WHAT SURVIVES OF P9. It used to read "awareness at SEARCHING or above pins
  // the position" and covered all three states — gloam#32's boundary, asserted
  // so that removing it would be a deliberate red test. #32 landed, and
  // SEARCHING and HUNTING now translate; their replacements are P9b and P9c in
  // `test/22pursuit/`.
  //
  // LOST_TRACK still holds, and that is a decision rather than a leftover:
  // §6.1's tell for it is "casts about at the last position, TURNING IN PLACE",
  // so a monster that walked while casting about would be performing a
  // different tell from the one the design authored. Its timers stop with it,
  // for the reason `world.cpp` gives — a monster that has stopped moving is not
  // quietly accruing the right to a step it will take the instant it calms down.
  //
  // NOTE WHAT THIS FIXTURE DOES NOT GIVE THE MONSTER: a `last_known`. That makes
  // the case narrower than it looks, so P9b/P9c carry the version with a memory
  // and this one is only about the state.
  const auto t = no_jitter();
  auto w = patrolling_world(crossing_route(), no_dwell(5), Coord{3, 0});
  w.monsters[0].mind.state = Awareness::LostTrack;
  w.monsters[0].mind.has_last_known = true;
  w.monsters[0].mind.last_known = Coord{3, 4};  // somewhere it could walk to, and does not
  // Held for long enough that §6.1's timers cannot carry it back to UNAWARE.
  CHECK(never_moved(w, t, 20));
}

// ── Goldens, last ───────────────────────────────────────────────────────────

TEST_CASE("the M0 corridor patrol crosses the intersection, and comes back", "[patrol]") {
  // §6's opening sentence, and #8's gate: "you glimpse a monster crossing a
  // distant intersection". The intersection is (3,2). This is the least
  // interesting test in the file and it is the one the milestone is about.
  Tuning t = no_jitter();
  t.monster_move_ticks = 2;

  auto w = patrolling_world(crossing_route(), no_dwell(5), Coord{3, 0});
  const auto seen = walk_for(w, t, 17);

  // North end to south end and back, one cell per two ticks. The monster is
  // over the intersection at ticks 3-4 on the way down and 11-12 on the way up,
  // which is the glimpse the gate is about.
  const std::vector<Coord> expected{
      Coord{3, 1}, Coord{3, 1}, Coord{3, 2}, Coord{3, 2}, Coord{3, 3}, Coord{3, 3},
      Coord{3, 4}, Coord{3, 4}, Coord{3, 3}, Coord{3, 3}, Coord{3, 2}, Coord{3, 2},
      Coord{3, 1}, Coord{3, 1}, Coord{3, 0}, Coord{3, 0}, Coord{3, 1},
  };
  CHECK(seen == expected);
}
