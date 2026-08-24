#include "scene.hpp"

/// The build is three `carve` calls and one aggregate \u2014 every load-bearing
/// choice is documented in `scene.hpp`; the comments here are only the ones
/// the code itself can carry.

namespace gloam::scene {

auto m0_world(std::uint64_t seed) -> World {
  Level level{7, 5};
  // The corridor: (1,2)..(5,2) \u2014 four cells plus the intersection.
  level.carve(kM0Spawn, Dir::East, 5);
  // The side passage, both halves carved FROM the intersection so the crossing
  // cell is carved once and its four edges are the passage's own.
  level.carve(kM0Intersection, Dir::North, 3);
  level.carve(kM0Intersection, Dir::South, 3);

  Monster m{};
  m.at = kM0MonsterSpawn;
  m.kind = MonsterKind{Acuity::Normal, /*sees_unlit=*/false};
  // South, the first step the ping-pong will take anyway \u2014 authored so the
  // hashed state says what the scene means from tick 0, not after one step.
  m.facing = Dir::South;
  // Ping-pong across the intersection; dwell at the far ends only. The header
  // comment is the design; this is its spelling.
  m.patrol.route = {Coord{3, 0}, Coord{3, 1}, Coord{3, 2}, Coord{3, 3}, Coord{3, 4}};
  m.patrol.dwell = {1, 0, 0, 0, 1};

  auto w = make_world(seed, std::move(level), {m});
  w.party = kM0Spawn;
  w.facing = kM0SpawnFacing;
  w.lamp_level = kLampLevelMin;  // doused: the reveal is the player's to make
  w.armour = Armour::Leather;    // the middle of \u00a76.2's weight/noise table
  return w;
}

}  // namespace gloam::scene
