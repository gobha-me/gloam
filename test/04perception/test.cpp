// SPEC §6.1, §6.3 and §13.3 — awareness, light and line of sight.
//
// The pillar under test: "A doused party is never seen beyond an adjacent cell
// by a sees_unlit = false monster." §13.3 lists it as a property because it is
// the mechanic that makes the game, and a regression in it would not look like
// a bug — it would look like the monsters being good at their job.

#include <catch2/catch_all.hpp>

#include <limits>
#include <vector>

#include "gloam/perception.hpp"

using namespace gloam;

namespace {

auto open_room(int w, int h) -> Level {
  Level level{w, h};
  for (int y = 0; y < h; ++y) level.carve(Coord{0, y}, Dir::East, w);
  for (int x = 0; x < w; ++x) level.carve(Coord{x, 0}, Dir::South, h);
  return level;
}

/// Runs a monster through `ticks` of identical senses.
auto settle(Perception& p, const Senses& s, const MonsterKind& k, int ticks) -> std::vector<Tell> {
  std::vector<Tell> tells;
  for (int i = 0; i < ticks; ++i) tells.push_back(step(p, s, k, kDefaultTuning));
  return tells;
}

}  // namespace

// ── §13.3: line of sight is symmetric ───────────────────────────────────────

TEST_CASE("line of sight is symmetric over every pair in an irregular level",
          "[perception][property]") {
  auto level = open_room(7, 7);
  // Scatter some walls so the raycast has corners to disagree about. A fully
  // open room would pass this property trivially and prove nothing.
  level.link(Coord{2, 2}, Dir::East, Edge{EdgeKind::Wall, EdgeState::Open, 0, 0});
  level.link(Coord{3, 1}, Dir::South, Edge{EdgeKind::Wall, EdgeState::Open, 0, 0});
  level.link(Coord{4, 4}, Dir::North, Edge{EdgeKind::Wall, EdgeState::Open, 0, 0});
  level.link(Coord{1, 5}, Dir::East, Edge{EdgeKind::Door, EdgeState::Closed, 0, 0});
  level.link(Coord{5, 2}, Dir::West, Edge{EdgeKind::Doorway, EdgeState::Open, 0, 0});

  REQUIRE(level.symmetric());  // the precondition the property rests on

  int clear_pairs = 0;
  for (std::int32_t ay = 0; ay < 7; ++ay) {
    for (std::int32_t ax = 0; ax < 7; ++ax) {
      for (std::int32_t by = 0; by < 7; ++by) {
        for (std::int32_t bx = 0; bx < 7; ++bx) {
          const Coord a{ax, ay};
          const Coord b{bx, by};
          const bool ab = line_of_sight(level, a, b);
          INFO("(" << ax << "," << ay << ") <-> (" << bx << "," << by << ")");
          REQUIRE(ab == line_of_sight(level, b, a));
          clear_pairs += ab ? 1 : 0;
        }
      }
    }
  }
  // Guard against a vacuous pass: if every pair were blocked, symmetry would
  // hold and mean nothing.
  CHECK(clear_pairs > 0);
}

TEST_CASE("a wall blocks sight; an open doorway does not", "[perception]") {
  auto level = open_room(5, 1);
  CHECK(line_of_sight(level, Coord{0, 0}, Coord{4, 0}));

  level.link(Coord{2, 0}, Dir::East, Edge{EdgeKind::Wall, EdgeState::Open, 0, 0});
  CHECK_FALSE(line_of_sight(level, Coord{0, 0}, Coord{4, 0}));
  CHECK(line_of_sight(level, Coord{0, 0}, Coord{2, 0}));
}

TEST_CASE("a closed door is opaque, an open one is not", "[perception]") {
  auto level = open_room(5, 1);
  level.link(Coord{2, 0}, Dir::East, Edge{EdgeKind::Door, EdgeState::Closed, 0, 0});
  CHECK_FALSE(line_of_sight(level, Coord{0, 0}, Coord{4, 0}));

  level.link(Coord{2, 0}, Dir::East, Edge{EdgeKind::Door, EdgeState::Open, 0, 0});
  CHECK(line_of_sight(level, Coord{0, 0}, Coord{4, 0}));
}

// ── §13.3: a doused party is never seen beyond an adjacent cell ─────────────

TEST_CASE("a doused party is never seen beyond an adjacent cell", "[perception][property]") {
  // The pillar, asserted. Exhaustive over every range and every sight distance
  // in the tuning, because "never" is the claim.
  const Tuning& t = kDefaultTuning;
  const std::int32_t sights[] = {t.sight_short, t.sight_medium, t.sight_long};

  for (const auto sight : sights) {
    for (std::int32_t range = 0; range <= sight + 2; ++range) {
      const bool seen = party_visible(/*lamp_level=*/0, /*sees_unlit=*/false,
                                      /*los_clear=*/true, range, sight);
      INFO("sight " << sight << " range " << range);
      if (range > kAdjacentRange) {
        CHECK_FALSE(seen);
      } else {
        CHECK(seen);
      }
    }
  }
}

TEST_CASE("the sees_unlit monster is the reason dousing is a decision, not a dominant strategy",
          "[perception]") {
  const Tuning& t = kDefaultTuning;
  const auto sight = t.sight_medium;
  // Same darkness, same range — the only difference is the monster.
  CHECK_FALSE(party_visible(0, /*sees_unlit=*/false, true, 4, sight));
  CHECK(party_visible(0, /*sees_unlit=*/true, true, 4, sight));
}

TEST_CASE("sight needs all three conditions, and any one of them is enough to break it",
          "[perception]") {
  const auto sight = kDefaultTuning.sight_medium;
  // §6.3: "if AND ONLY IF line of sight is clear, and illumination is greater
  // than zero, and range is within its sight distance."
  CHECK(party_visible(3, false, /*los=*/true, 3, sight));
  CHECK_FALSE(party_visible(3, false, /*los=*/false, 3, sight));   // no LOS
  CHECK_FALSE(party_visible(0, false, true, 3, sight));            // unlit, beyond adjacent
  CHECK_FALSE(party_visible(3, false, true, sight + 1, sight));    // out of range
}

TEST_CASE("any lamp level above zero is enough to be seen", "[perception]") {
  // There is no second light model: illumination is the same integer that picks
  // the light-field plate, and it is a boolean test at the perception layer.
  const auto sight = kDefaultTuning.sight_medium;
  for (int lamp = 1; lamp <= kLampLevelMax; ++lamp) {
    CHECK(party_visible(lamp, false, true, sight, sight));
  }
  CHECK_FALSE(party_visible(0, false, true, sight, sight));
}

// ── §13.3: awareness never advances two states in one tick ─────────────────

TEST_CASE("awareness advances at most one state per tick", "[perception][property]") {
  const MonsterKind kind{Acuity::Keen, false};
  Perception p{};

  // The most provocative senses possible: heard AND seen, in range, fully lit.
  Senses loud{};
  loud.heard = true;
  loud.los_clear = true;
  loud.range = 1;
  loud.lamp_level = kLampLevelMax;

  Awareness previous = p.state;
  for (int tick = 0; tick < 50; ++tick) {
    step(p, loud, kind, kDefaultTuning);
    const int delta = static_cast<int>(p.state) - static_cast<int>(previous);
    INFO("tick " << tick << " " << static_cast<int>(previous) << " -> "
                 << static_cast<int>(p.state));
    CHECK(delta <= 1);
    previous = p.state;
  }
  // And it does climb all the way, so the test is not passing by standing still.
  CHECK(p.state == Awareness::Hunting);
}

TEST_CASE("the full §6.1 escalation, one transition at a time", "[perception]") {
  const MonsterKind kind{Acuity::Normal, false};
  const Tuning& t = kDefaultTuning;
  Perception p{};

  Senses seen{};
  seen.heard = false;
  seen.los_clear = true;
  seen.range = 2;
  seen.lamp_level = kLampLevelDefault;
  seen.party_position = Coord{4, 4};

  // One perception hit: patrol rhythm breaks.
  CHECK(step(p, seen, kind, t) == Tell::PatrolRhythmBreaks);
  CHECK(p.state == Awareness::Suspicious);
  CHECK(p.has_last_known);
  CHECK(p.last_known == Coord{4, 4});

  // A second hit inside the window: leaves the patrol route.
  CHECK(step(p, seen, kind, t) == Tell::LeavesPatrolRoute);
  CHECK(p.state == Awareness::Searching);

  // LOS clear and lit and in range: gait changes.
  CHECK(step(p, seen, kind, t) == Tell::GaitChanges);
  CHECK(p.state == Awareness::Hunting);

  // Lose the party. Nothing happens until the timer expires...
  Senses nothing{};
  nothing.lamp_level = 0;
  for (int i = 0; i < t.hunting_lost_ticks - 1; ++i) {
    CHECK(step(p, nothing, kind, t) == Tell::None);
    CHECK(p.state == Awareness::Hunting);
  }
  // ...and then it casts about.
  CHECK(step(p, nothing, kind, t) == Tell::CastsAbout);
  CHECK(p.state == Awareness::LostTrack);

  // And after the long timer, back to the patrol route.
  for (int i = 0; i < t.lost_track_ticks - 1; ++i) {
    CHECK(step(p, nothing, kind, t) == Tell::None);
    CHECK(p.state == Awareness::LostTrack);
  }
  CHECK(step(p, nothing, kind, t) == Tell::ResumesPatrol);
  CHECK(p.state == Awareness::Unaware);
}

TEST_CASE("SUSPICIOUS also escalates on unbroken line of sight alone", "[perception]") {
  // §6.1's second route into SEARCHING: "3 ticks of unbroken LOS".
  const MonsterKind kind{Acuity::Normal, false};
  Tuning t = kDefaultTuning;
  // Widen the second-hit window out of the way so only the LOS rule can fire.
  t.suspicious_second_hit_window = 0;

  Perception p{};
  Senses seen{};
  seen.los_clear = true;
  seen.range = 2;
  seen.lamp_level = kLampLevelDefault;

  CHECK(step(p, seen, kind, t) == Tell::PatrolRhythmBreaks);
  CHECK(p.state == Awareness::Suspicious);
  // The tick that made it suspicious was itself a tick of line of sight, and it
  // counts: §6.1 asks for "3 ticks of unbroken LOS", not three MORE.
  CHECK(p.los_streak == 1);

  for (int i = p.los_streak; i < t.suspicious_los_ticks - 1; ++i) {
    CHECK(step(p, seen, kind, t) == Tell::None);
    CHECK(p.state == Awareness::Suspicious);
  }
  CHECK(step(p, seen, kind, t) == Tell::LeavesPatrolRoute);
  CHECK(p.state == Awareness::Searching);
  CHECK(p.los_streak == t.suspicious_los_ticks);
}

TEST_CASE("awareness never regresses except by the §6.1 timers", "[perception][property]") {
  const MonsterKind kind{Acuity::Normal, false};
  const Tuning& t = kDefaultTuning;
  Perception p{};

  Senses seen{};
  seen.los_clear = true;
  seen.range = 1;
  seen.lamp_level = kLampLevelMax;

  // Climb to HUNTING.
  while (p.state != Awareness::Hunting) step(p, seen, kind, t);

  // Keep feeding it hits: it must not fall back, however long this runs.
  for (int i = 0; i < 500; ++i) {
    step(p, seen, kind, t);
    REQUIRE(p.state == Awareness::Hunting);
  }
}

TEST_CASE("a doused party breaks the escalation at the sight step", "[perception]") {
  // The mechanic, at the state machine rather than the predicate: dousing keeps
  // a SEARCHING monster from ever reaching HUNTING, because `saw` is the only
  // route.
  const MonsterKind kind{Acuity::Normal, false};
  Perception p{};
  p.state = Awareness::Searching;

  Senses dark{};
  dark.heard = true;      // it can still hear you
  dark.los_clear = true;  // the geometry is clear
  dark.range = 3;         // well within sight distance
  dark.lamp_level = 0;    // but the lamp is out

  const auto tells = settle(p, dark, kind, 100);
  CHECK(p.state == Awareness::Searching);
  for (const auto tell : tells) CHECK(tell == Tell::None);
}

TEST_CASE("LOST_TRACK has no route back to HUNTING — an open design question, pinned",
          "[perception]") {
  // §6.1's table lists LOST_TRACK -> UNAWARE and nothing else, so a monster
  // that re-acquires the party while casting about must run the 40-tick timer
  // out and come back round through SUSPICIOUS.
  //
  // This test does not claim that is good. It claims it is what §6.1 specifies,
  // so that changing it is a deliberate design decision with an authored tell
  // rather than a quiet edit to a switch statement.
  const MonsterKind kind{Acuity::Keen, false};
  const Tuning& t = kDefaultTuning;

  Perception p{};
  p.state = Awareness::LostTrack;

  Senses obvious{};
  obvious.heard = true;
  obvious.los_clear = true;
  obvious.range = 1;
  obvious.lamp_level = kLampLevelMax;

  for (int i = 0; i < t.lost_track_ticks - 1; ++i) {
    CHECK(step(p, obvious, kind, t) == Tell::None);
    REQUIRE(p.state == Awareness::LostTrack);
  }
  CHECK(step(p, obvious, kind, t) == Tell::ResumesPatrol);
  CHECK(p.state == Awareness::Unaware);
}

TEST_CASE("tick counters saturate rather than wrapping", "[perception]") {
  // A wrapped counter would turn a monster that has been idle for a very long
  // time into one that just heard something — a bug that only appears in a long
  // session and never in a test.
  Perception p{};
  p.ticks_since_hit = std::numeric_limits<std::int32_t>::max();
  p.ticks_in_state = std::numeric_limits<std::int32_t>::max();

  const MonsterKind kind{Acuity::Normal, false};
  Senses nothing{};
  nothing.lamp_level = 0;
  step(p, nothing, kind, kDefaultTuning);

  CHECK(p.ticks_since_hit == std::numeric_limits<std::int32_t>::max());
  CHECK(p.ticks_in_state == std::numeric_limits<std::int32_t>::max());
}
