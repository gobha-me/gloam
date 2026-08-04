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

  // AND IT KEEPS HEARING YOU, which is load-bearing now in a way it was not
  // when this case was written. gloam#32 gave SEARCHING an exit — the trail
  // going cold — and `heard` on every tick is what keeps `ticks_since_hit` at
  // zero and that exit shut. The claim is still "dousing blocks the sight step";
  // it is no longer "SEARCHING is where a monster stays for ever".
  const auto tells = settle(p, dark, kind, 100);
  CHECK(p.state == Awareness::Searching);
  for (const auto tell : tells) CHECK(tell == Tell::None);
}

// ── §6.1's SEARCHING -> LOST_TRACK row (gloam#32) ───────────────────────────

TEST_CASE("a searcher that runs out of trail gives up, and casts about", "[perception]") {
  // The row §6.1 shipped without. Its absence was invisible while a SEARCHING
  // monster could not move: "searching for ever" and "standing still" were the
  // same picture. Once it walks to the last known position, an exit-less
  // SEARCHING is a monster stranded off its route for the rest of the session.
  //
  // TWO CONDITIONS, and both are asserted separately below, because a timer
  // alone would make a monster give up mid-walk — which reads as losing interest
  // rather than as drawing a blank.
  const MonsterKind kind{Acuity::Normal, false};
  const Tuning& t = kDefaultTuning;

  Senses cold{};
  cold.heard = false;
  cold.los_clear = false;
  cold.lamp_level = 0;
  cold.range = 9;

  SECTION("the trail ends and the timer expires") {
    Perception p{};
    p.state = Awareness::Searching;
    cold.trail_exhausted = true;

    // `hunting_lost_ticks` of nothing, and not one tick sooner.
    for (int i = 1; i < t.hunting_lost_ticks; ++i) {
      INFO("tick " << i);
      REQUIRE(step(p, cold, kind, t) == Tell::None);
      REQUIRE(p.state == Awareness::Searching);
    }
    CHECK(step(p, cold, kind, t) == Tell::CastsAbout);
    CHECK(p.state == Awareness::LostTrack);
  }

  SECTION("the timer alone is not enough while there is still trail to walk") {
    // The monster is still on its way to the last known position. It does not
    // get to give up before it arrives, however long the walk.
    Perception p{};
    p.state = Awareness::Searching;
    cold.trail_exhausted = false;

    const auto tells = settle(p, cold, kind, 200);
    CHECK(p.state == Awareness::Searching);
    for (const auto tell : tells) CHECK(tell == Tell::None);
  }

  SECTION("seeing you beats the timer on the exact tick it expires") {
    // Ordered after the `saw` check, on gloam#12's precedent: losing someone on
    // the frame they walked into your light is the worst available reading.
    Perception p{};
    p.state = Awareness::Searching;
    cold.trail_exhausted = true;
    for (int i = 1; i < t.hunting_lost_ticks; ++i) static_cast<void>(step(p, cold, kind, t));

    Senses lit = cold;
    lit.los_clear = true;
    lit.range = 2;
    lit.lamp_level = kLampLevelMax;
    CHECK(step(p, lit, kind, t) == Tell::GaitChanges);
    CHECK(p.state == Awareness::Hunting);
  }
}

TEST_CASE("trail_exhausted defaults false, so no existing caller changed", "[perception]") {
  // The field was APPENDED to `Senses` with a false default precisely so that
  // every caller that never heard of it keeps its old meaning. This is that
  // promise, asserted rather than trusted: a default-constructed `Senses` cannot
  // fire the new row no matter how long it runs.
  const MonsterKind kind{Acuity::Normal, false};
  Perception p{};
  p.state = Awareness::Searching;

  Senses nothing{};
  nothing.lamp_level = 0;
  REQUIRE_FALSE(nothing.trail_exhausted);

  const auto tells = settle(p, nothing, kind, 500);
  CHECK(p.state == Awareness::Searching);
  for (const auto tell : tells) CHECK(tell == Tell::None);
}

TEST_CASE("a monster casting about re-acquires a party it can see, immediately", "[perception]") {
  // This case used to pin the OPPOSITE behaviour, as an open design question:
  // §6.1's table shipped with no LOST_TRACK -> HUNTING row, so a monster
  // looking straight at a lit, adjacent party did nothing for 43+ ticks. The
  // decision on gloam#12 added the row with its own tell; this is that decision
  // made executable.
  const MonsterKind kind{Acuity::Keen, false};
  const Tuning& t = kDefaultTuning;

  Perception p{};
  p.state = Awareness::LostTrack;

  Senses obvious{};
  obvious.heard = true;
  obvious.los_clear = true;
  obvious.range = 1;
  obvious.lamp_level = kLampLevelMax;

  // One tick, not forty-three.
  CHECK(step(p, obvious, kind, t) == Tell::SnapsBack);
  CHECK(p.state == Awareness::Hunting);
}

TEST_CASE("the two paths into HUNTING carry different tells", "[perception]") {
  // §6.1: "every transition has an authored tell, and the tell is the
  // deliverable". Two transitions now land on HUNTING, so the tell has to be
  // keyed on the pair rather than the destination — otherwise re-acquiring
  // someone you were already hunting reads exactly like spotting them fresh,
  // and the audio sting stops meaning anything.
  const MonsterKind kind{Acuity::Keen, false};
  const Tuning& t = kDefaultTuning;

  Senses obvious{};
  obvious.heard = true;
  obvious.los_clear = true;
  obvious.range = 1;
  obvious.lamp_level = kLampLevelMax;

  Perception searching{};
  searching.state = Awareness::Searching;
  CHECK(step(searching, obvious, kind, t) == Tell::GaitChanges);
  CHECK(searching.state == Awareness::Hunting);

  Perception lost{};
  lost.state = Awareness::LostTrack;
  CHECK(step(lost, obvious, kind, t) == Tell::SnapsBack);
  CHECK(lost.state == Awareness::Hunting);

  // The distinction is the whole point of the decision.
  CHECK(Tell::SnapsBack != Tell::GaitChanges);
}

TEST_CASE("re-acquisition needs sight, not merely noise", "[perception]") {
  // The new row's condition is `saw` — §6.3's one and only illumination test —
  // and NOT `hit`. Getting that wrong would let a monster re-acquire a doused
  // party by ear alone, which is a hole in §6.3's pillar at exactly the moment
  // the pillar matters most: you have just broken line of sight and doused.
  //
  // Range is deliberately beyond adjacent. §6.3 makes a doused party visible at
  // an adjacent cell — "effectively invisible BEYOND adjacent cells" — so a
  // range of 1 here would be testing the wrong claim and would pass for the
  // wrong reason. (It caught exactly that mistake when this case was written.)
  const MonsterKind kind{Acuity::Keen, false};
  const Tuning& t = kDefaultTuning;
  const std::int32_t beyond_adjacent = kAdjacentRange + 2;
  REQUIRE(beyond_adjacent <= t.sight_distance(Acuity::Keen));

  Perception p{};
  p.state = Awareness::LostTrack;

  Senses loud_but_dark{};
  loud_but_dark.heard = true;
  loud_but_dark.los_clear = true;
  loud_but_dark.range = beyond_adjacent;
  loud_but_dark.lamp_level = 0;  // doused

  for (int i = 0; i < t.lost_track_ticks - 1; ++i) {
    INFO("tick " << i);
    REQUIRE(step(p, loud_but_dark, kind, t) == Tell::None);
    REQUIRE(p.state == Awareness::LostTrack);
  }
  // It still forgets on the timer, exactly as before the new row was added.
  CHECK(step(p, loud_but_dark, kind, t) == Tell::ResumesPatrol);
  CHECK(p.state == Awareness::Unaware);
}

TEST_CASE("a doused party IS re-acquired at an adjacent cell", "[perception]") {
  // The other side of the boundary above, stated on purpose so nobody "fixes"
  // it. §6.3 is explicit that dousing buys invisibility BEYOND adjacent cells,
  // not inside them — standing next to a monster in the dark is still standing
  // next to a monster. Dousing is a decision, not a dominant strategy.
  const MonsterKind kind{Acuity::Keen, false};
  const Tuning& t = kDefaultTuning;

  Perception p{};
  p.state = Awareness::LostTrack;

  Senses adjacent_and_dark{};
  adjacent_and_dark.los_clear = true;
  adjacent_and_dark.range = kAdjacentRange;
  adjacent_and_dark.lamp_level = 0;

  CHECK(step(p, adjacent_and_dark, kind, t) == Tell::SnapsBack);
  CHECK(p.state == Awareness::Hunting);
}

TEST_CASE("re-acquisition is checked before the forget timer", "[perception]") {
  // Ordering inside the LOST_TRACK arm. On the exact tick the timer expires, a
  // monster that can see the party must hunt rather than forget — losing them
  // on the frame you walked into its light would be the worst possible reading.
  const MonsterKind kind{Acuity::Keen, false};
  const Tuning& t = kDefaultTuning;

  Perception p{};
  p.state = Awareness::LostTrack;
  p.ticks_in_state = t.lost_track_ticks;

  Senses obvious{};
  obvious.heard = true;
  obvious.los_clear = true;
  obvious.range = 1;
  obvious.lamp_level = kLampLevelMax;

  CHECK(step(p, obvious, kind, t) == Tell::SnapsBack);
  CHECK(p.state == Awareness::Hunting);
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
