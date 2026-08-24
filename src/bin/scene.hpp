#pragma once

/// SPEC \u00a715 (M0), \u00a76's opening sentence \u2014 the corridor slice, as data-in-code.
///
/// \u00a715: "One corridor. Four cells long, one intersection. One patrolling
/// monster with the full perception model. One character. No inventory, no
/// combat, no magic. You can move, carry a lamp, and douse it." And the gate:
/// "Does glimpsing that monster cross the intersection produce genuine
/// tension?" \u00a76 opens with the moment itself: "you glimpse a monster crossing
/// a distant intersection, left to right, and you do not know whether it
/// registered you."
///
/// THE GEOMETRY OF THE REVEAL IS THE DESIGN. The M0 gate is a human judging
/// tension, so every authoring choice below is documented with its reason \u2014
/// changing a coordinate here is changing the question the gate answers.
///
///
/// THE MAP (7x5; `#` is floor, `.` is rock)
///
/// ```
///      x=0  1  2  3  4  5  6
/// y=0   .  .  .  M  .  .  .     M = monster spawn (3,0)
/// y=1   .  .  .  #  .  .  .
/// y=2   .  P  #  X  #  #  .     P = party spawn (1,2), facing EAST
/// y=3   .  .  .  #  .  .  .     X = the intersection (3,2)
/// y=4   .  .  .  #  .  .  .
/// ```
///
/// The corridor row is five carved cells, which is \u00a715's "four cells long,
/// one intersection": four corridor cells plus the intersection they meet at.
/// The side passage runs the full height of the map through it, so the
/// crossing reads as a glimpse of somewhere the corridor does not go.
///
/// This is the third writing of this shape, and the first SHARED one.
/// `src/bin/main.cpp`'s `build_corridor()`/`trace_patrol()` prototyped it as a
/// diagnostic, and `test/19patrol/` re-authored it as a fixture \u2014
/// `src/bin/replay.cpp`'s header records why a GATE and a SUITE deliberately
/// do not share one definition. This module is the opposite case: the App
/// shell (a later unit) and the game-core tests must mean the SAME corridor,
/// or the M0 gate judges one scene while the suite pins another.
///
///
/// WHY THIS SPAWN
///
/// (1,2) facing East puts the entire corridor and the intersection on one
/// line of sight, two cells ahead \u2014 inside a level-3 light field the moment
/// the lamp comes up, and one cell OUTSIDE `perception.hpp`'s
/// `kAdjacentRange`, so a `sees_unlit = false` monster cannot register a dark
/// party standing there. The spawn is the one cell that is simultaneously
/// safe in the dark and fatal in the light: lamp up while the monster is in
/// the intersection and it sees you (range 2, LOS clear, lit). Whether it
/// just did is the tension the gate judges, and it is produced by the
/// geometry, not scripted.
///
/// WHY THE LAMP STARTS DOUSED
///
/// \u00a715 gives the player exactly one verb beyond walking: "carry a lamp, and
/// douse it." Starting doused makes lighting it the first authored decision
/// of every session rather than a default nobody chose \u2014 and \u00a76.3 makes that
/// decision double-edged, because the same integer that reveals the corridor
/// to the player reveals the player to the monster. It also keeps the
/// authored patrol unperturbed while the party stands still: doused at range
/// 2 the party is invisible, and a standing party emits no noise (\u00a76.2), so
/// the crossing schedule below is a property of the scene and the seed alone.
/// `main.cpp`'s trace made the same call for the same reason: "doused: you
/// are watching from the dark."
///
///
/// WHY THIS ROUTE, AND WHY THIS DWELL
///
/// The route is the side passage end to end \u2014 (3,0)..(3,4), ping-pong, per
/// `world.hpp`'s `Patrol` docs \u2014 so it crosses the intersection once per leg
/// and satisfies `valid_route` outright (five navigable cells, each adjacent
/// and reachable, dwell parallel, no back-to-back repeat). Five cells rather
/// than three, so the monster spends most of its round trip OUT of the
/// corridor: it is heard before it is seen (\u00a79's monster footfall arrives
/// through the wall), and its reappearance is uncertain.
///
/// Dwell is {1, 0, 0, 0, 1}: a pause at each far end and NONE at the
/// intersection. A dwell on (3,2) would park the monster in the one cell the
/// lamp lights \u2014 that reads as a guard posted at the crossing, not a
/// glimpse, and it would collapse "did it register me?" into "there it is,
/// again." The end pauses are where \u00a76.4's jitter draws land
/// (`Stream::Patrol`, `patrol_idle_jitter_ticks`), so the rhythm of the
/// crossings varies by seed while the crossing itself stays a crossing: the
/// monster occupies (3,2) for exactly `monster_move_ticks` (2 ticks, 200 ms
/// at `replay::kTickHz`) per pass. Perceivable, and then gone.
///
/// `world.hpp` states the spawn-cell dwell is never paid \u2014 a monster has not
/// "arrived" at the cell it starts on \u2014 so the first pause happens on the
/// first RETURN to (3,0), and the whole outbound leg takes no draw at all.
///
///
/// THE SCHEDULE THAT FOLLOWS, AND WHAT THE TEST PINS
///
/// With `monster_move_ticks` = 2 and no dwell paid on the outbound leg, the
/// first crossing lands on tick `kM0FirstCrossingTick` (3): step at tick 1,
/// step at tick 3. That number is INDEPENDENT OF THE SEED \u2014 no jitter draw
/// happens before it \u2014 which is what makes it a scene invariant rather than
/// a distribution. Every later crossing is jitter-dependent and is pinned for
/// `kM0Seed` in `test/34gamecore/`, not here: this header states the design,
/// the suite holds the implementation to it.
///
/// Determinism: `make_world` seeds every stream from `seed`, and the only
/// stream this scene draws is `Stream::Patrol` (the dwell jitter), taken
/// unconditionally inside `advance` whether or not a sink is attached \u2014
/// `world.hpp`'s five facts cover the rest. Same seed, same session, every
/// target.
///
///
/// DEVICE-FREE, LIKE THE REST OF THE SIM'S NEIGHBOURS
///
/// No termforge, no RtAudio, no clock, no file descriptor \u2014 the App owns all
/// of that. This file is content, not machinery, which is why it lives in
/// `src/bin/` rather than the library; being free of devices is what lets
/// `test/34gamecore/` compile it directly, the way `test/27sfxarena/`
/// compiles `sfx.cpp`, and puts the M0 scene under all eight sanitizer legs.

#include <cstdint>

#include "gloam/world.hpp"

namespace gloam::scene {

/// The authored seed. Arbitrary \u2014 pinned so the patrol jitter schedule is a
/// fixed target that `test/34gamecore/` can pin exact crossing ticks against.
inline constexpr std::uint64_t kM0Seed = 0x5EEDCAFEULL;

/// Where the party starts, and which way it looks. See the header comment:
/// one LOS line over the corridor and the intersection, one cell outside a
/// dark monster's `kAdjacentRange`.
inline constexpr Coord kM0Spawn{1, 2};
inline constexpr Dir kM0SpawnFacing = Dir::East;

/// The cell the whole scene is about. The monster's route crosses it once
/// per leg; the party's lamp reveals it from spawn.
inline constexpr Coord kM0Intersection{3, 2};

/// Where the monster starts: the north end of its own route, `waypoint` 0.
inline constexpr Coord kM0MonsterSpawn{3, 0};

/// The world tick on which the monster first stands on the intersection:
/// two cells south of spawn at `monster_move_ticks` = 2, with no dwell paid
/// on the outbound leg. Seed-independent \u2014 see the header comment.
inline constexpr std::uint32_t kM0FirstCrossingTick = 3;

/// The M0 slice as a ready-to-tick `World`, deterministic from `seed`.
///
/// Party at `kM0Spawn` facing `kM0SpawnFacing`, lamp DOUSED
/// (`kLampLevelMin`), leather armour, not creeping. One monster,
/// `Acuity::Normal` and not `sees_unlit` \u2014 \u00a76.3's `sees_unlit` kind is M2
/// content and must not appear before the player has learned to trust the
/// dark \u2014 patrolling the side passage through `kM0Intersection`.
[[nodiscard]] auto m0_world(std::uint64_t seed = kM0Seed) -> World;

}  // namespace gloam::scene
