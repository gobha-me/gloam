// SPEC §6.2 and §13.3 — noise propagation, as properties.

#include <catch2/catch_all.hpp>

#include <vector>

#include "gloam/noise.hpp"

using namespace gloam;

namespace {

/// A straight corridor of `length` cells running east from (0,0).
auto corridor(int length) -> Level {
  Level level{length, 1};
  level.carve(Coord{0, 0}, Dir::East, length);
  return level;
}

/// Two rooms joined by a single door, so the door's attenuation is the only
/// thing between them.
auto two_rooms_with_a_door(EdgeKind kind, EdgeState state) -> Level {
  Level level{6, 1};
  level.carve(Coord{0, 0}, Dir::East, 6);
  level.link(Coord{2, 0}, Dir::East, Edge{kind, state, 0, 0});
  return level;
}

}  // namespace

TEST_CASE("noise is non-increasing along the propagation, in attenuation distance",
          "[noise][property]") {
  // §13.3's first property. Note carefully what it does and does not say.
  //
  // In a uniform corridor every edge costs the same, so loudness falls
  // monotonically with HOP distance too. That is what this checks.
  const Tuning& t = kDefaultTuning;
  const auto level = corridor(12);
  const auto field = propagate_noise(level, Coord{0, 0}, 100, t);

  std::int32_t previous = field.at(level, Coord{0, 0});
  CHECK(previous == 100);
  for (int x = 1; x < 12; ++x) {
    const auto here = field.at(level, Coord{x, 0});
    INFO("x=" << x << " loudness=" << here << " previous=" << previous);
    CHECK(here <= previous);
    previous = here;
  }
}

TEST_CASE("hop distance is NOT the right reading of the monotonicity property",
          "[noise][property]") {
  // Worth pinning, because it is the obvious misreading of §13.3 and it would
  // make a future "simplification" of propagate_noise look correct.
  //
  // A closed door costs 40; an open cell costs 2. So a cell two hops away
  // through open air is LOUDER than a cell one hop away through a closed door.
  // Monotonicity holds in attenuation-weighted distance, which is what
  // Dijkstra minimises — not in hop count.
  const Tuning& t = kDefaultTuning;

  Level level{3, 2};
  // Row 0: source at (0,0), a closed door east to (1,0).
  level.carve(Coord{0, 0}, Dir::East, 2);
  level.link(Coord{0, 0}, Dir::East, Edge{EdgeKind::Door, EdgeState::Closed, 0, 0});
  // Row 1: an open two-hop detour from the source round to (1,1).
  level.carve(Coord{0, 0}, Dir::South, 2);
  level.carve(Coord{0, 1}, Dir::East, 2);

  const auto field = propagate_noise(level, Coord{0, 0}, 100, t);

  const auto through_door = field.at(level, Coord{1, 0});   // 1 hop, -40
  const auto around_two_hops = field.at(level, Coord{1, 1});  // 2 hops, -2 -2

  CHECK(through_door == 60);
  CHECK(around_two_hops == 96);
  CHECK(around_two_hops > through_door);
}

TEST_CASE("every edge type attenuates by its §6.2 value", "[noise]") {
  const Tuning& t = kDefaultTuning;

  struct Case {
    EdgeKind kind;
    EdgeState state;
    std::int32_t expected_loss;
  };
  const Case cases[] = {
      {EdgeKind::Open, EdgeState::Open, t.atten_open_cell},
      {EdgeKind::Doorway, EdgeState::Open, t.atten_open_doorway},
      {EdgeKind::Door, EdgeState::Open, t.atten_open_doorway},
      {EdgeKind::Door, EdgeState::Closed, t.atten_closed_door},
      {EdgeKind::Door, EdgeState::Locked, t.atten_closed_door},
  };

  for (const auto& c : cases) {
    const auto level = two_rooms_with_a_door(c.kind, c.state);
    const auto field = propagate_noise(level, Coord{2, 0}, 100, t);
    INFO("edge kind " << static_cast<int>(c.kind) << " state " << static_cast<int>(c.state));
    CHECK(field.at(level, Coord{3, 0}) == 100 - c.expected_loss);
  }
}

TEST_CASE("solid rock does not leak", "[noise]") {
  const auto level = two_rooms_with_a_door(EdgeKind::Wall, EdgeState::Open);
  const auto field = propagate_noise(level, Coord{2, 0}, 200, kDefaultTuning);

  CHECK(field.at(level, Coord{2, 0}) == 200);
  CHECK(field.at(level, Coord{3, 0}) == 0);
}

TEST_CASE("§6.2's armour table, and the coupling it exists to create", "[noise]") {
  const Tuning& t = kDefaultTuning;
  CHECK(step_noise(Armour::None, false, t) == 8);
  CHECK(step_noise(Armour::Leather, false, t) == 14);
  CHECK(step_noise(Armour::Mail, false, t) == 26);
  CHECK(step_noise(Armour::Plate, false, t) == 44);

  // Creeping halves it, in integer arithmetic. 14/2 is 7; there is no 7.5.
  CHECK(step_noise(Armour::None, true, t) == 4);
  CHECK(step_noise(Armour::Leather, true, t) == 7);
  CHECK(step_noise(Armour::Mail, true, t) == 13);
  CHECK(step_noise(Armour::Plate, true, t) == 22);
}

TEST_CASE("§6.2's worked example: plate is heard through five open cells, unarmoured is not",
          "[noise]") {
  // "a plated character steps at 44 and is heard by a normal-threshold monster
  // through five open cells; unarmoured at 8 is inaudible past the fourth."
  //
  // This is the sentence that makes the loadout decision tactical rather than a
  // stat-block comparison, so it is worth an assertion rather than trust.
  const Tuning& t = kDefaultTuning;
  const auto level = corridor(10);
  const Coord party{0, 0};

  const auto plate = propagate_noise(level, party, step_noise(Armour::Plate, false, t), t);
  const auto bare = propagate_noise(level, party, step_noise(Armour::None, false, t), t);

  // Five open cells away: 5 x -2 = -10.
  const Coord five{5, 0};
  CHECK(plate.at(level, five) == 34);
  CHECK(hears(plate, level, five, Acuity::Normal, false, t) == false);  // 34 < 40

  // A keen ear does hear the plate there, which is the whole point of acuity.
  CHECK(hears(plate, level, five, Acuity::Keen, false, t));

  // Unarmoured is inaudible to everything by the fourth cell.
  const Coord four{4, 0};
  CHECK(bare.at(level, four) == 0);
  CHECK_FALSE(hears(bare, level, four, Acuity::Keen, false, t));
}

TEST_CASE("a HUNTING monster listens harder", "[noise]") {
  // §18 Q5 — the only thing that varies by awareness state.
  const Tuning& t = kDefaultTuning;
  const auto level = corridor(10);
  const auto field = propagate_noise(level, Coord{0, 0}, 45, t);

  // At two cells: 45 - 4 = 41... which a normal ear already hears. Step out to
  // where it sits between the two thresholds.
  const Coord listener{4, 0};
  CHECK(field.at(level, listener) == 37);
  CHECK_FALSE(hears(field, level, listener, Acuity::Normal, /*hunting=*/false, t));  // 37 < 40
  CHECK(hears(field, level, listener, Acuity::Normal, /*hunting=*/true, t));         // 37 > 30
}

TEST_CASE("hearing is strictly greater than the threshold", "[noise]") {
  // §6.2 says "exceeds", and the boundary is tuned against that reading.
  const Tuning& t = kDefaultTuning;
  Level level{1, 1};
  level.carve(Coord{0, 0}, Dir::East, 1);

  const auto exactly = propagate_noise(level, Coord{0, 0}, t.hear_threshold_normal, t);
  CHECK_FALSE(hears(exactly, level, Coord{0, 0}, Acuity::Normal, false, t));

  const auto one_more = propagate_noise(level, Coord{0, 0}, t.hear_threshold_normal + 1, t);
  CHECK(hears(one_more, level, Coord{0, 0}, Acuity::Normal, false, t));
}

TEST_CASE("§8.3's casting noise, including the GOTH example", "[noise][spells]") {
  const Tuning& t = kDefaultTuning;
  // "Casting emits noise at 10 x the power rune's cost. GOTH in a quiet
  // corridor is 130 — audible to a dull-eared monster through a closed door."
  CHECK(cast_noise(13, t) == 130);

  const auto level = two_rooms_with_a_door(EdgeKind::Door, EdgeState::Closed);
  const auto field = propagate_noise(level, Coord{2, 0}, cast_noise(13, t), t);
  CHECK(field.at(level, Coord{3, 0}) == 90);
  CHECK(hears(field, level, Coord{3, 0}, Acuity::Dull, false, t));  // 90 > 70
}

TEST_CASE("propagation is defined for degenerate inputs", "[noise]") {
  const auto level = corridor(4);
  const Tuning& t = kDefaultTuning;

  CHECK(propagate_noise(level, Coord{0, 0}, 0, t).at(level, Coord{0, 0}) == 0);
  CHECK(propagate_noise(level, Coord{0, 0}, -5, t).at(level, Coord{0, 0}) == 0);
  // A source outside the level is silence, not a crash.
  CHECK(propagate_noise(level, Coord{99, 99}, 100, t).at(level, Coord{0, 0}) == 0);

  const Level empty;
  CHECK(propagate_noise(empty, Coord{0, 0}, 100, t).at(empty, Coord{0, 0}) == 0);
}
