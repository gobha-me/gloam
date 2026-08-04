// SPEC §6.1 — the three tells that translate a monster toward a target.
//
// `test/19patrol/` owns the route pump; this file owns what a monster does when
// it has stopped following one. The split is deliberate: they are different
// behaviours reached from different states, and mixing them would bury both.
//
// THE ONE CLAIM EVERY REFUSAL BELOW SHARES, and it is `test/19patrol/`'s claim
// one state higher: a target it cannot reach makes a monster STAND STILL. Never
// a drift toward the wall between you, which is the greedy-stepper failure mode
// gloam#32 rejected by name and §16 rates as the project's top risk.

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

#include "gloam/path.hpp"
#include "gloam/world.hpp"

using namespace gloam;

namespace {

constexpr std::uint64_t kSeed = 0x9E3779B97F4A7C15ULL;

auto open_room(int w, int h) -> Level {
  Level level{w, h};
  for (int y = 0; y < h; ++y) level.carve(Coord{0, y}, Dir::East, w);
  for (int x = 0; x < w; ++x) level.carve(Coord{x, 0}, Dir::South, h);
  return level;
}

/// A straight corridor, which is where §6's opening sentence happens.
auto corridor(int length) -> Level {
  Level level{length, 3};
  level.carve(Coord{0, 1}, Dir::East, length);
  return level;
}

/// A mind that has already escalated and remembers where you were.
///
/// Set directly rather than driven there through `step`, for `test/10budgets/`'s
/// reason: the state under test is the destination, and walking to it through
/// §6.1's timers would make every case below a test of the timers as well.
auto mind_at(Awareness state, Coord last_known) -> Perception {
  Perception p{};
  p.state = state;
  p.has_last_known = true;
  p.last_known = last_known;
  return p;
}

auto world_with(Level level, Monster m, Coord party) -> World {
  auto w = make_world(kSeed, std::move(level), {m});
  w.party = party;
  w.lamp_level = 0;  // doused unless a case says otherwise
  return w;
}

/// Positions after each of `ticks` ticks.
[[nodiscard]] auto walk_for(World& w, const Tuning& t, int ticks) -> std::vector<Coord> {
  std::vector<Coord> seen;
  for (int i = 0; i < ticks; ++i) {
    advance(w, t);
    seen.push_back(w.monsters[0].at);
  }
  return seen;
}

[[nodiscard]] auto never_moved(World& w, const Tuning& t, int ticks) -> bool {
  const auto start = w.monsters[0].at;
  for (const auto at : walk_for(w, t, ticks)) {
    if (at != start) return false;
  }
  return true;
}

/// §6.4's jitter off, so every sequence below is a closed form.
[[nodiscard]] auto no_jitter() -> Tuning {
  Tuning t = kDefaultTuning;
  t.patrol_idle_jitter_ticks = 0;
  return t;
}

}  // namespace

// ── The failure matrix ──────────────────────────────────────────────────────

TEST_CASE("a SEARCHING monster with nothing remembered does not move", "[pursuit]") {
  // SEARCHING is reached by two perception hits, each of which writes
  // `last_known`, so this is data that should not exist — which is exactly why
  // it is the first case. A loader, a save file or a test can produce it.
  const auto t = no_jitter();
  Monster m{};
  m.at = Coord{6, 1};
  m.kind = MonsterKind{Acuity::Dull, false};
  m.mind.state = Awareness::Searching;  // and has_last_known stays false
  auto w = world_with(corridor(10), m, Coord{1, 1});
  CHECK(never_moved(w, t, 20));
}

TEST_CASE("a target it cannot reach makes a monster stand still", "[pursuit]") {
  // Every shape of "no path", against both translating states. A monster that
  // edged toward an unreachable target would be the greedy stepper gloam#32
  // rejected, and it would read to a player as broken AI rather than as a wall.
  const auto t = no_jitter();
  const auto state = GENERATE(Awareness::Searching, Awareness::Hunting);

  SECTION("a target outside the level") {
    const auto target = GENERATE(Coord{-1, 1}, Coord{99, 1},
                                 Coord{std::numeric_limits<std::int32_t>::min(), 1},
                                 Coord{std::numeric_limits<std::int32_t>::max(), 1});
    Monster m{};
    m.at = Coord{6, 1};
    m.mind = mind_at(state, target);
    auto w = world_with(corridor(10), m, Coord{1, 1});
    CHECK(never_moved(w, t, 20));
  }

  SECTION("a target inside solid rock") {
    Monster m{};
    m.at = Coord{6, 1};
    m.mind = mind_at(state, Coord{6, 0});  // the corridor is row 1; row 0 is rock
    auto w = world_with(corridor(10), m, Coord{1, 1});
    REQUIRE_FALSE(w.level.navigable(Coord{6, 0}));
    CHECK(never_moved(w, t, 20));
  }

  SECTION("a target behind a closed door") {
    auto level = corridor(10);
    level.link(Coord{4, 1}, Dir::East, Edge{EdgeKind::Door, EdgeState::Closed, 0, 0});
    Monster m{};
    m.at = Coord{6, 1};
    m.mind = mind_at(state, Coord{1, 1});
    auto w = world_with(std::move(level), m, Coord{1, 1});
    CHECK(never_moved(w, t, 20));
  }

  SECTION("a target behind a locked door") {
    auto level = corridor(10);
    level.link(Coord{4, 1}, Dir::East, Edge{EdgeKind::Door, EdgeState::Locked, 0, 0});
    Monster m{};
    m.at = Coord{6, 1};
    m.mind = mind_at(state, Coord{1, 1});
    auto w = world_with(std::move(level), m, Coord{1, 1});
    CHECK(never_moved(w, t, 20));
  }

  SECTION("a monster itself standing in rock") {
    // `Level::walk` lets a body OUT of rock and never into it, so this monster
    // could legally take a step. It does not: the field is rooted at the target
    // and a cell no path reaches reads unreachable from either end.
    Monster m{};
    m.at = Coord{6, 0};
    m.mind = mind_at(state, Coord{1, 1});
    auto w = world_with(corridor(10), m, Coord{1, 1});
    CHECK(never_moved(w, t, 20));
  }
}

TEST_CASE("a monster off a route it cannot reach does not walk home", "[pursuit]") {
  const auto t = no_jitter();
  auto level = corridor(10);
  level.link(Coord{4, 1}, Dir::East, Edge{EdgeKind::Door, EdgeState::Closed, 0, 0});

  Monster m{};
  m.at = Coord{6, 1};
  m.kind = MonsterKind{Acuity::Dull, false};
  m.patrol.route = {Coord{1, 1}, Coord{2, 1}};  // on the far side of the door
  m.patrol.dwell = {0, 0};
  auto w = world_with(std::move(level), m, Coord{9, 1});
  CHECK(never_moved(w, t, 20));
}

TEST_CASE("a route of fewer than two cells is nothing to walk home to", "[pursuit]") {
  // Empty is "does not patrol" and one cell is malformed — `world.hpp` calls
  // those the ONE representation of standing still and a mistake respectively,
  // and neither is a destination.
  const auto t = no_jitter();
  const auto route = GENERATE(std::vector<Coord>{}, std::vector<Coord>{Coord{1, 1}});

  Monster m{};
  m.at = Coord{6, 1};
  m.kind = MonsterKind{Acuity::Dull, false};
  m.patrol.route = route;
  m.patrol.dwell = std::vector<std::uint8_t>(route.size(), 0);
  auto w = world_with(corridor(10), m, Coord{9, 1});
  CHECK(never_moved(w, t, 20));
}

TEST_CASE("a garbage cursor still does not move a monster standing on its route",
          "[pursuit]") {
  // `test/19patrol/` asserts that a cursor indexing nothing makes a monster
  // stand still. The re-join rule must not quietly repair it: a monster standing
  // ON its route is home, whatever its cursor says, and silently rewriting the
  // cursor would turn malformed data into behaviour nobody authored.
  const auto t = no_jitter();
  const auto cursor = GENERATE(999, -1, std::numeric_limits<std::int32_t>::min(),
                               std::numeric_limits<std::int32_t>::max());

  Monster m{};
  m.at = Coord{2, 1};
  m.kind = MonsterKind{Acuity::Dull, false};
  m.patrol.route = {Coord{1, 1}, Coord{2, 1}, Coord{3, 1}};
  m.patrol.dwell = {0, 0, 0};
  m.patrol.waypoint = cursor;
  auto w = world_with(corridor(10), m, Coord{9, 1});
  INFO("cursor " << cursor);
  CHECK(never_moved(w, t, 20));
}

TEST_CASE("pursuit survives degenerate movement rates", "[pursuit]") {
  const auto rate = GENERATE(0, -1, 1, std::numeric_limits<std::int32_t>::min(),
                             std::numeric_limits<std::int32_t>::max());
  auto t = no_jitter();
  t.monster_move_ticks = rate;

  Monster m{};
  m.at = Coord{6, 1};
  m.mind = mind_at(Awareness::Searching, Coord{1, 1});
  auto w = world_with(corridor(10), m, Coord{1, 1});

  const auto walk = walk_for(w, t, 20);
  INFO("monster_move_ticks " << rate);
  for (const auto at : walk) {
    REQUIRE(w.level.navigable(at));  // P1 still holds at any rate
  }
  if (rate == std::numeric_limits<std::int32_t>::max()) {
    // Charged once and never ready again. Not a special case in the code — it
    // falls out of the clamp — but worth pinning, because "never moves again"
    // is a behaviour an author could reach from a data file.
    CHECK(walk.back() == Coord{5, 1});
  } else {
    // Below 1 is clamped to 1 at the point of use, so the fastest legal rate is
    // one cell per tick and the monster is at the party's edge well inside 20.
    CHECK(walk.back() == Coord{2, 1});
  }
}

// ── The arrival rule ────────────────────────────────────────────────────────

TEST_CASE("P9c: a HUNTING monster stops adjacent and never enters the party's cell",
          "[pursuit][property]") {
  // gloam#32's design decision, asserted. There is no combat, no death and no
  // cell-occupancy rule at M0, so a monster that walked onto you would be a more
  // visible dead end than one that stops at the edge of arm's reach.
  // LIT, AND THE LAMP IS LOAD-BEARING. A doused party cannot be perceived
  // beyond an adjacent cell (§6.3), so a hunter chasing one runs out
  // `hunting_lost_ticks` and gives up four cells short — correct behaviour, and
  // a scenario in which the arrival rule is never reached. Carrying a lamp is
  // what makes the thing keep coming, which is also the M0 picture #8 asks about.
  const auto t = no_jitter();
  Monster m{};
  m.at = Coord{6, 1};
  m.mind = mind_at(Awareness::Hunting, Coord{1, 1});
  auto w = world_with(corridor(12), m, Coord{1, 1});
  w.lamp_level = kLampLevelMax;

  int adjacent_ticks = 0;
  for (int i = 0; i < 40; ++i) {
    advance(w, t);
    const auto at = w.monsters[0].at;
    REQUIRE(at != w.party);  // the property
    if (range_between(at, w.party) == 1) ++adjacent_ticks;
  }
  // Non-vacuity: it really did close the distance, so "never entered" is a
  // restraint rather than a monster that never arrived.
  CHECK(adjacent_ticks > 0);
  CHECK(w.monsters[0].at == Coord{2, 1});
  // And it turned to face what it came for, which is the halted-hunter pose.
  CHECK(w.monsters[0].facing == Dir::West);
}

TEST_CASE("the standoff is path distance, not Chebyshev range", "[pursuit]") {
  // The two metrics disagree on the diagonal: a monster one cell away
  // diagonally is at `range_between` 1 and at path distance 2, so it takes one
  // more step. Stating the rule in the pathfinder's own metric is what makes it
  // predictable; mixing them would make "adjacent" mean two things.
  const auto t = no_jitter();
  Monster m{};
  m.at = Coord{2, 2};
  m.mind = mind_at(Awareness::Hunting, Coord{1, 1});
  auto w = world_with(open_room(5, 5), m, Coord{1, 1});
  w.lamp_level = kLampLevelMax;  // seen every tick, so it never loses the trail

  REQUIRE(range_between(m.at, w.party) == 1);  // already Chebyshev-adjacent

  const auto walk = walk_for(w, t, 12);
  const auto arrived = walk.back();
  CHECK(arrived != w.party);
  CHECK(range_between(arrived, w.party) == 1);
  // It moved, which is the whole distinction: a Chebyshev reading would have
  // called this arrived and held it at (2,2).
  CHECK(arrived != Coord{2, 2});

  const auto field = propagate_distance(w.level, w.party);
  CHECK(field.at(w.level, arrived) == 1);
}

TEST_CASE("P9b: a SEARCHING monster walks ONTO the last known position and stops",
          "[pursuit][property]") {
  // §6.1: "leaves the patrol route, walks to the last known position." Onto it,
  // not up to it — there is nothing standing there to keep it out, and the
  // picture the tell describes is a monster searching the spot itself.
  const auto t = no_jitter();
  Monster m{};
  m.at = Coord{8, 1};
  m.kind = MonsterKind{Acuity::Dull, false};  // deaf, so nothing re-triggers it
  m.mind = mind_at(Awareness::Searching, Coord{4, 1});
  auto w = world_with(corridor(12), m, Coord{11, 1});

  const auto walk = walk_for(w, t, 10);
  CHECK(walk.back() == Coord{4, 1});
  CHECK(std::count(walk.begin(), walk.end(), Coord{4, 1}) > 1);  // it stopped there
}

TEST_CASE("monsters do not collide with each other", "[pursuit]") {
  // A PINNED DECISION, not an accident. There is no occupancy model at M0, and
  // inventing one to serve pathing would be deciding §7's entity question from
  // inside `world.cpp`. Two monsters converging on one party converge on one
  // cell, and that is allowed.
  const auto t = no_jitter();
  auto level = corridor(12);
  std::vector<Monster> monsters;
  for (const auto start : {Coord{8, 1}, Coord{9, 1}}) {
    Monster m{};
    m.at = start;
    m.kind = MonsterKind{Acuity::Dull, false};
    m.mind = mind_at(Awareness::Searching, Coord{4, 1});
    monsters.push_back(m);
  }
  auto w = make_world(kSeed, std::move(level), std::move(monsters));
  w.party = Coord{11, 1};
  w.lamp_level = 0;

  for (int i = 0; i < 20; ++i) advance(w, t);
  CHECK(w.monsters[0].at == Coord{4, 1});
  CHECK(w.monsters[1].at == Coord{4, 1});  // the same cell, and that is fine
}

// ── The rate contract, extended to the new mover ────────────────────────────

TEST_CASE("P12: pursuit never steps more often than monster_move_ticks allows",
          "[pursuit][property]") {
  // P3 for the other mover. The clock is shared — `ready_to_move` and
  // `charge_step` are the same pair the patrol pump uses — so this is really an
  // assertion that nobody gave pursuit a clock of its own.
  const auto t = no_jitter();
  Monster m{};
  m.at = Coord{11, 1};
  m.kind = MonsterKind{Acuity::Dull, false};
  m.mind = mind_at(Awareness::Searching, Coord{1, 1});
  auto w = world_with(corridor(14), m, Coord{13, 1});

  constexpr int kTicks = 40;
  auto previous = w.monsters[0].at;
  int moves = 0;
  for (int i = 0; i < kTicks; ++i) {
    advance(w, t);
    if (w.monsters[0].at != previous) ++moves;
    previous = w.monsters[0].at;
  }
  CHECK(moves > 0);
  CHECK(moves <= kTicks / t.monster_move_ticks);
}

TEST_CASE("leaving the route discards the pause it owed", "[pursuit]") {
  // A monster part-way through a four-tick authored dwell that notices you must
  // not stand through the rest of it first. Measured before this rule existed:
  // it did, and a long authored pause read as a monster ignoring the noise it
  // had just reacted to.
  const auto t = no_jitter();
  Monster m{};
  m.at = Coord{6, 1};
  m.kind = MonsterKind{Acuity::Dull, false};
  m.patrol.route = {Coord{6, 1}, Coord{7, 1}};
  m.patrol.dwell = {9, 0};
  m.patrol.dwell_left = 9;  // mid-pause
  m.mind = mind_at(Awareness::Searching, Coord{1, 1});
  auto w = world_with(corridor(10), m, Coord{9, 1});

  // Two ticks is one step at the default rate. If the dwell were paid first this
  // monster would still be standing on (6,1) nine ticks from now.
  const auto walk = walk_for(w, t, 2);
  CHECK(walk.back() == Coord{5, 1});
  CHECK(w.monsters[0].patrol.dwell_left == 0);
}

// ── The round trip, which is what gloam#32 is actually about ────────────────

TEST_CASE("P13: a monster that loses you walks home and resumes its patrol",
          "[pursuit][property]") {
  // §6.1's cycle, performed end to end for the first time:
  //   UNAWARE -> SUSPICIOUS -> SEARCHING -> (trail cold) LOST_TRACK -> UNAWARE
  // and then a walk back to the route. Before this slice the monster froze at
  // SEARCHING and the cycle had no way round; before gloam#32's SEARCHING exit
  // it would have stood on a stale cell for the rest of the session.
  const auto t = no_jitter();
  const auto route = std::vector<Coord>{Coord{2, 1}, Coord{3, 1}, Coord{4, 1}};

  Monster m{};
  m.at = Coord{2, 1};
  m.kind = MonsterKind{Acuity::Keen, false};
  m.patrol.route = route;
  m.patrol.dwell = {0, 0, 0};
  // A LONG corridor, and the party WALKS AWAY down it. That is what makes the
  // trail go cold: `last_known` is written on every perception hit, so a party
  // that stays put is a party the monster finds. A plate step is 44 and an open
  // cell costs 2, so a Keen monster (threshold 20) stops hearing this party once
  // it is twelve cells off — it is out of earshot long before it is out of
  // corridor, and the monster is left walking to a cell nobody is standing on.
  auto w = world_with(corridor(40), m, Coord{9, 1});
  w.armour = Armour::Plate;

  for (int i = 0; i < 20; ++i) {
    apply(w, replay::Event::Step, static_cast<std::uint16_t>(Dir::East), t);
    advance(w, t);
  }
  REQUIRE(w.monsters[0].mind.state == Awareness::Searching);
  REQUIRE(w.party.x >= 25);  // genuinely gone

  // Then silence. It walks to where it last heard you, finds nothing, gives up,
  // and goes home.
  bool left_the_route = false;
  bool cast_about = false;
  for (int i = 0; i < 200; ++i) {
    advance(w, t);
    const auto& mon = w.monsters[0];
    if (std::find(route.begin(), route.end(), mon.at) == route.end()) left_the_route = true;
    if (mon.mind.state == Awareness::LostTrack) cast_about = true;
  }

  const auto& mon = w.monsters[0];
  CHECK(left_the_route);  // it really did leave, so coming back means something
  CHECK(cast_about);      // through LOST_TRACK, not straight from SEARCHING
  CHECK(mon.mind.state == Awareness::Unaware);
  CHECK(std::find(route.begin(), route.end(), mon.at) != route.end());
  CHECK(mon.patrol.waypoint >= 0);
  CHECK(mon.patrol.waypoint < static_cast<std::int32_t>(route.size()));
  CHECK(route[static_cast<std::size_t>(mon.patrol.waypoint)] == mon.at);
}

// ── Determinism ─────────────────────────────────────────────────────────────

TEST_CASE("pursuit is deterministic and takes no RNG draw", "[pursuit][property]") {
  // The pathfinder has no stream and needs none: every tie is broken by the
  // monster's own facing and then by `Dir` wire order. That is what lets §19
  // step 9's `--mute` identity survive a second subsystem writing `Monster::at`.
  const auto t = no_jitter();
  const auto make = [&] {
    Monster m{};
    m.at = Coord{9, 1};
    m.kind = MonsterKind{Acuity::Dull, false};
    m.mind = mind_at(Awareness::Searching, Coord{1, 1});
    return world_with(corridor(12), m, Coord{11, 1});
  };

  auto a = make();
  auto b = make();
  const auto reference = walk_for(b, t, 30);
  CHECK(walk_for(a, t, 30) == reference);
  CHECK(world_hash(a) == world_hash(b));

  // A different seed reaches the SAME walk, which is the half worth asserting:
  // if pursuit ever draws, this line goes red and names the reason. Compared
  // against the recorded sequence rather than against a second `walk_for(b, …)`
  // — `walk_for` ADVANCES the world it is given, so calling it twice on one
  // world compares the first thirty ticks with the second thirty.
  auto c = make();
  c.seed ^= 0xFFFFULL;
  for (std::size_t i = 0; i < kStreamCount; ++i) {
    c.rng_state[i] = rng(c.seed, static_cast<Stream>(i + 1)).state();
  }
  CHECK(walk_for(c, t, 30) == reference);

  auto quiet = make();
  const auto before = quiet.rng_state;
  for (int i = 0; i < 100; ++i) advance(quiet, t);
  CHECK(quiet.rng_state == before);  // not one stream moved
}

TEST_CASE("a rejoin arrival DOES draw, from the patrol stream and no other",
          "[pursuit][property]") {
  // The one draw in the neighbourhood: §6.4's idle variation, paid on arriving
  // at a waypoint — which a walk home is. Asserted rather than assumed, because
  // "never the shared one" is the whole of what §6.4 settles.
  auto t = kDefaultTuning;  // jitter ON, so an authored dwell draws
  Monster m{};
  m.at = Coord{6, 1};
  m.kind = MonsterKind{Acuity::Dull, false};
  m.patrol.route = {Coord{1, 1}, Coord{2, 1}};
  m.patrol.dwell = {5, 5};
  auto w = world_with(corridor(10), m, Coord{9, 1});

  const auto before = w.rng_state;
  for (int i = 0; i < 30; ++i) advance(w, t);

  constexpr auto kPatrol = static_cast<std::size_t>(Stream::Patrol) - 1;
  REQUIRE(w.monsters[0].at == Coord{2, 1});          // it got home
  CHECK(w.rng_state[kPatrol] != before[kPatrol]);    // and paid the dwell
  for (std::size_t i = 0; i < kStreamCount; ++i) {
    if (i == kPatrol) continue;
    INFO("stream index " << i);
    CHECK(w.rng_state[i] == before[i]);
  }
}

TEST_CASE("a halted hunter keeps facing you as you walk past it", "[pursuit]") {
  // The pose half of the arrival rule. "Stands adjacent, FACING you" is the
  // picture §4.2 will draw, and `facing` is hashed state precisely so a replay
  // reproduces it — a tell a replay cannot reproduce is not a tell.
  //
  // The party walks THROUGH the monster's cell, which it may: there is no
  // occupancy model and monsters block nobody. North of it, on it, south of it —
  // and the thing turns to follow without taking a step, because the arrival
  // rule holds it at arm's reach the whole way.
  const auto t = no_jitter();
  Monster m{};
  m.at = Coord{2, 2};
  m.mind = mind_at(Awareness::Hunting, Coord{2, 1});
  m.facing = Dir::North;
  auto w = world_with(open_room(5, 5), m, Coord{2, 1});
  w.lamp_level = kLampLevelMax;  // seen every tick, so it never loses the trail

  advance(w, t);
  REQUIRE(w.monsters[0].at == Coord{2, 2});
  REQUIRE(w.monsters[0].facing == Dir::North);

  for (int i = 0; i < 2; ++i) {
    apply(w, replay::Event::Step, static_cast<std::uint16_t>(Dir::South), t);
    advance(w, t);
  }

  CHECK(w.party == Coord{2, 3});
  CHECK(w.monsters[0].at == Coord{2, 2});      // it never stepped
  CHECK(w.monsters[0].facing == Dir::South);   // and it turned all the way round
  CHECK(w.monsters[0].mind.state == Awareness::Hunting);
}
