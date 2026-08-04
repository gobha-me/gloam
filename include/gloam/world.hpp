#pragma once

/// SPEC §5.1, §6 · TEST-PLAN.md §2 — the simulation state a replay reproduces.
///
/// §5.1: "Fixed-tick integer simulation. No floating-point in simulation
/// state." TEST-PLAN.md §2: "A golden replay is `seed + input log -> world hash
/// at tick N`. The hash covers every byte of simulation state and NOTHING from
/// the render layer."
///
/// That second sentence is why this header exists. Until now the simulation was
/// a set of free functions over caller-owned values with no aggregate anywhere,
/// and the only tick loop in the tree was hand-rolled inside
/// `test/10budgets/test.cpp`. There was no "every byte of simulation state" to
/// hash, because nothing owned it.
///
///
/// THIS IS DELIBERATELY THE SMALLEST WORLD THAT CAN BE REPLAYED
///
/// `World` holds exactly what that reference tick already touched, and not one
/// field more. There is no party roster, no inventory, no per-character stat,
/// no item table — SPEC §7 and §8's structures are absent on purpose, because
/// BUILD-ORDER step 10 is explicit that they wait for the M0 gate: "Then, and
/// only then, open the party or magic sections." A `World` that anticipated
/// them would be committing to an entity model before the question that decides
/// it has been asked.
///
/// The consequence to know about: `party` is ONE coordinate, not a party. When
/// §7 lands it becomes a roster and `world_hash` grows to cover it, and
/// `kWorldHashVersion` is what makes that a deliberate change rather than a
/// mystery.
///
///
/// WHAT IS NOT MODELLED YET, AND IS NOT PRETENDED TO BE
///
///   * ARRIVAL HAS NO OUTCOME. A monster that notices you now comes after you
///     (gloam#32) — and stops one cell short, because there is no combat, no
///     death and no cell-occupancy rule for it to arrive INTO. It stands at
///     arm's reach facing you until you move. When §7 lands, that halt becomes
///     the attack and the pathing does not change.
///
///     THE RESTRAINT IS ON THE MONSTER AND ONLY ON THE MONSTER, which is worth
///     stating because the sentence above reads like a guarantee about the
///     CELL. `apply` has no occupancy rule either, so THE PARTY MAY WALK INTO A
///     HALTED HUNTER, and then the two share a cell until the party leaves.
///     That is the same missing model seen from the other side, not a hole in
///     the arrival rule.
///   * MONSTERS DO NOT COLLIDE WITH EACH OTHER, for the same missing model.
///     Two of them may stand in one cell. `test/22pursuit/` pins it so it is a
///     decision rather than an accident.
///   * `creep_tick_cost` IS NOT APPLIED TO THE PARTY. §6.2 makes creeping cost ticks as
///     well as halving the noise. Monsters now have a movement-rate model
///     (`Tuning::monster_move_ticks`, gloam#29) and the party still does not:
///     how often a `Step` may legally appear is the driver's rule, not
///     `advance`'s, and there is no driver until gloam#7. `step_noise` already
///     halves. The two become one decision the day a frame loop exists.

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "gloam/level.hpp"
#include "gloam/noise.hpp"
#include "gloam/perception.hpp"
#include "gloam/replay.hpp"
#include "gloam/rng.hpp"
#include "gloam/sha256.hpp"
#include "gloam/tuning.hpp"

namespace gloam::audio {

/// SPEC §9.3's `Sink`, forward-declared rather than included.
///
/// `advance` takes one BY POINTER, and a pointer parameter needs no definition.
/// That keeps `gloam/audio.hpp` — and with it a lock-free ring and a
/// `std::atomic` — out of every translation unit that merely ticks a world,
/// including the headless diagnostic in `src/bin/main.cpp`. `gloam.hpp`'s
/// umbrella rule is "include them by name when you want them", and this is what
/// honouring it costs: one line.
///
/// Deliberately NOT the `sha256.hpp` situation that header describes. That one
/// arrives transitively because `world_hash` names `hash::Digest` BY VALUE and
/// there is no way around it. A pointer has a way around it.
class Sink;

}  // namespace gloam::audio

namespace gloam {

/// §6.4's patrol route, plus the cursor walking it (gloam#28).
///
/// `SCHEMAS.md` §1's record is `patrol { monster_type:u8, route:[]CellIndex,
/// dwell:[]u8 }`. The first two members below ARE that record minus its
/// `monster_type`, which is the reason a route lives on a `Monster` rather than
/// in a shared table: the schema puts the monster inside the patrol, so a
/// `patrol` record is a spawn and there is no route id to index. The rest is
/// cursor — state a record does not carry, because a record describes a route
/// and this describes a monster part-way along one.
///
/// COORD, NOT CELLINDEX, and the wire form keeps the index. A `CellIndex` means
/// nothing without the level width, so storing one here would let a level
/// resize silently re-point every route in it. `level.gloam`'s loader converts
/// at load, which is the one place the width is in scope.
///
/// TRAVERSAL IS ALWAYS PING-PONG — forward to the end, then back. The tempting
/// alternative, "it is a cycle iff `back()` is adjacent to `front()`", makes
/// behaviour turn on an accident of authoring; a route that happens to close
/// would silently mean something else. A loop is authored by listing the cells
/// there and back.
struct Patrol {
  /// Cell-by-cell, each adjacent to and reachable from the last. Empty means
  /// this monster does not patrol, which is the ONE representation of standing
  /// still — `size() == 1` is malformed, not a synonym.
  std::vector<Coord> route{};
  /// Parallel to `route`: ticks owed ON ARRIVING at that waypoint, and paid IN
  /// ADDITION to `Tuning::monster_move_ticks` rather than concurrently with it —
  /// so a dwell of 1 at a period of 2 means three ticks between steps, not two.
  ///
  /// "On arriving" is literal, and has one consequence worth knowing before
  /// authoring a route: the dwell of the cell a monster is SPAWNED on is never
  /// paid, because it did not arrive there. A pause authored at `route[waypoint]`
  /// for a freshly-placed monster is dead until the ping-pong brings it back.
  std::vector<std::uint8_t> dwell{};

  /// Index into `route`. Out-of-range is survivable, not undefined: the pump
  /// reads it through a bounds check and a monster with a nonsense cursor
  /// simply does not move.
  std::int32_t waypoint{0};
  /// Which leg of the ping-pong. A `bool` has no invalid value, which is the
  /// reason it is not a direction enum.
  bool reversed{false};
  /// Ticks still owed at the current waypoint, authored plus §6.4's jitter.
  ///
  /// A PATROL DEBT, AND ONLY A PATROL DEBT. A monster that leaves its route to
  /// search or pursue (gloam#32) discards whatever pause it owed rather than
  /// standing through it first — you made a noise and it looked up, so a
  /// four-tick authored pause must not become four ticks of a monster ignoring
  /// you. `approach_step` clears it, and the pause is re-authored the next time
  /// the ping-pong arrives at that waypoint.
  std::int32_t dwell_left{0};

  [[nodiscard]] auto operator==(const Patrol&) const -> bool = default;
};

/// One monster: where it is, what it is, what it currently believes, and where
/// it is walking.
///
/// `perception.hpp`'s `Perception` is an awareness state with no position and
/// no kind attached, because it was written to be driven by a caller holding
/// both. This is that caller, made into a type.
///
/// `facing` and `patrol` are APPENDED rather than inserted, so every aggregate
/// initialiser in the tree that names the first three members still compiles
/// and value-initialises the rest into "does not patrol, looking north".
struct Monster {
  Coord at{};
  MonsterKind kind{};
  Perception mind{};
  /// Which way its head is pointing. Simulation state because §6.1 makes two of
  /// its five tells a head turn and nothing else — "head turns toward the
  /// source", "casts about... turning in place" — so a renderer that read this
  /// off anything but hashed state could not reproduce them from a replay.
  Dir facing{Dir::North};
  Patrol patrol{};
  /// Ticks until this monster may step again (`Tuning::monster_move_ticks`).
  ///
  /// ON THE MONSTER RATHER THAN INSIDE `Patrol`, since gloam#32. It is the
  /// movement clock, not a patrol cursor: a route-LESS monster that pursues you
  /// charges it too, and leaving it in `Patrol` would have meant a monster with
  /// no route quietly writing patrol state — which `test/13replay/` asserts does
  /// not happen, and was right to.
  ///
  /// Hashed in the position `Patrol::move_cooldown` used to occupy, so moving it
  /// changed no digest byte and `kWorldHashVersion` did not have to move for it.
  std::int32_t move_cooldown{0};

  [[nodiscard]] auto operator==(const Monster&) const -> bool = default;
};

/// Every invariant §6.4's route needs and `SCHEMAS.md`'s record cannot express:
/// empty or at least two cells; `dwell` parallel to `route`; every cell in
/// bounds and navigable; consecutive cells adjacent AND reachable through a
/// passable edge; no cell repeated back-to-back.
///
/// For `level.gloam`'s loader (§12) and for tests. `advance` DOES NOT CALL IT.
/// A hot path that re-validates its data every tick pays for a check the load
/// gate owes, and what makes skipping it safe is that the pump steps only
/// through `Level::walk` — an invalid route yields a monster that does not
/// move, never a teleport and never an out-of-bounds read. Same idiom as
/// `apply` refusing an impassable step and `Level::at` returning the void cell.
///
/// WHAT IT DOES NOT CHECK, because it is not told: where the monster is
/// STANDING. A perfectly valid route paired with a monster placed off it — or
/// on it but not at `route[waypoint]` — is still a route this function accepts.
///
/// THAT USED TO MEAN "a monster that never moves again", and it no longer does.
/// gloam#32 answers the two halves differently, because they are different
/// situations wearing one description:
///
///   * OFF the route entirely — the re-join rule walks it back to the nearest
///     cell of its own route and resumes the ping-pong from there.
///   * ON the route but not at `route[waypoint]` — the cursor is RESYNCED to
///     the cell it is standing on, lowest index first, and it patrols from
///     there. The position is authoritative and the cursor is advisory.
///
/// The second is not a nicety for malformed data: the pump produces that state
/// in ordinary play, because a hunter halts at arm's reach — often on a cell of
/// its own route — and then calms down. Treating the cursor as authoritative
/// froze those monsters permanently, and a fuzz over valid data found 256,797
/// of them in 8,000 trials while all 32 test cases stayed green.
///
/// The one case that still stands still is a route it cannot REACH — behind a
/// shut door, or across rock — which is a level that disagrees with itself, and
/// standing still is the same answer the pump gives every other kind of that.
[[nodiscard]] auto valid_route(const Level& level, std::span<const Coord> route,
                               std::span<const std::uint8_t> dwell) -> bool;

/// Every byte of simulation state, and nothing else.
///
/// If you are about to add a field here, it must be an integer (§5.1 forbids
/// floats in simulation state), it must be covered by `world_hash`, and
/// `kWorldHashVersion` must go up in the same commit. A field that is state but
/// is not hashed is a determinism hole that no test can see.
struct World {
  std::uint64_t seed{0};
  std::uint32_t tick{0};

  Coord party{};
  Dir facing{Dir::North};
  std::int32_t lamp_level{kLampLevelDefault};
  bool creeping{false};
  Armour armour{Armour::Leather};

  /// Noise emitted by inputs since the last `advance`, as a §6.2 magnitude.
  /// Real state, not a scratch variable: an input applied at tick N is heard
  /// during tick N, so this survives between `apply` and `advance` and must be
  /// hashed like anything else that does.
  std::int32_t pending_noise{0};

  Level level{};
  std::vector<Monster> monsters{};

  /// One `Rng::state()` per `Stream`, indexed by `stream - 1` — streams
  /// are numbered from 1 so that stream 0 cannot be a default-constructed
  /// mistake. Rehydrate with `Rng{state}`; snapshot with `Rng::state()`, which
  /// `rng.hpp` says is "exposed so a replay can be checkpointed and resumed
  /// mid-session".
  std::array<std::uint64_t, kStreamCount> rng_state{};
};

/// A world with every stream seeded from `seed`, ready to tick.
[[nodiscard]] auto make_world(std::uint64_t seed, Level level, std::vector<Monster> monsters)
    -> World;

/// Index `rng_state` for a stream, and hand back a generator over it.
[[nodiscard]] auto stream_of(const World& w, Stream s) -> Rng;
auto save_stream(World& w, Stream s, const Rng& generator) -> void;

/// What the hash covers, as a number.
///
/// Bumped whenever the SET of hashed state changes — a new field, a dropped
/// one, a reordering. Without it, "the golden hash moved because we started
/// hashing the lamp" and "the golden hash moved because the simulation is
/// non-deterministic" are the same observation, and they have opposite fixes.
///
/// 1 -> 2: §6.4's patrols. `Monster` gained `facing` and a `Patrol`, so the set
/// of hashed state grew by seven fields including two variable-length vectors.
inline constexpr std::uint8_t kWorldHashVersion = 2;

/// Every byte of simulation state, and nothing from the render layer.
///
/// EXCLUDED, each for a stated reason:
///   * everything render-side — TEST-PLAN.md §2 says so in as many words;
///   * `NoiseField` — it is recomputed every tick from state already covered
///     here, so hashing it would pin an implementation rather than a state;
///   * `Stream::Ambience` — `rng.hpp` calls it "non-simulation flavour;
///     never feeds back into sim state". Hashing it would make a change to
///     ambient sound look exactly like a determinism regression.
[[nodiscard]] auto world_hash(const World& w) -> hash::Digest;

/// Apply one input. §5.2: inputs land on the tick they were recorded against.
///
/// A `Step` into an impassable edge does not move the party AND emits no noise:
/// you did not take a step. That is a decision §6.2 does not make either way,
/// and it is written down here rather than left in the code.
auto apply(World& w, replay::Event event, std::uint16_t payload, const Tuning& tuning) -> void;

/// Advance the simulation by exactly one tick.
///
/// Propagate whatever noise this tick's inputs emitted, run every monster's
/// senses against it, then clear the emission and count the tick. This is
/// `test/10budgets/test.cpp`'s reference loop, moved rather than reinvented —
/// the budget case now calls this, so the measured tick and the replayed tick
/// cannot drift apart.
///
/// Named `advance` and not `tick` because `World::tick` is the counter it
/// increments, and a verb that shadows its own noun reads badly at both.
///
///
/// `voices` IS SPEC §9's ONE-WAY WALL, AND `nullptr` IS `--mute`
///
/// Passing a sink changes what is HEARD and nothing that is HASHED. §19 step 9's
/// acceptance criterion — "`--mute` and unmuted runs produce identical replays"
/// — holds here by construction rather than by care, on five structural facts:
///
///   1. `audio::Sink::play` returns void and takes scalars by value, so there is
///      no expression in `advance` that can read anything back from a sink.
///      §9.2's "Audio -> sim is nothing. Ever." is closed by the type system.
///   2. Nothing a sink is handed is a field a sink can write. §9 added no state
///      to `World` at all, and §6.4 added `Monster::facing` and `Patrol` — both
///      written by the pump, which never sees `voices`.
///   3. Everything handed to `play` is derived from state that is already final
///      for the tick, through const-qualified reads.
///   4. THE ONE THAT IS NOT FREE, AND IT ARRIVED WITH §6.4. `advance` now draws
///      from `Stream::Patrol`, and `rng_state` IS hashed — so the identity holds
///      only because that draw is taken unconditionally, outside any
///      `voices != nullptr` test. Gating it on the sink to "save work when
///      muted" would make a muted world and a voiced world diverge within a few
///      ticks, and every other fact in this list would still be true.
///      `test/19patrol/`'s stream-isolation property and `test/16audiosim/`'s
///      "patrols run identically muted and unmuted" are what hold it.
///   5. The only thing a non-null sink changes is work performed — extra
///      propagations — never a byte written into `w`.
///
/// `test/16audiosim/` proves it empirically as well, including against a sink
/// that deliberately allocates and thrashes inside `play`.
///
/// What this list deliberately no longer claims: that no replay is invalidated.
/// That was true of §9 and is NOT true of §6.4 — `kWorldHashVersion` went 1 -> 2
/// and `kTuningFieldCount` 47 -> 49, so every file recorded before it is refused
/// at load. The `--mute` identity and replay compatibility are separate
/// properties, and running them together in one list is how the second gets
/// asserted by a comment nobody re-derived.
auto advance(World& w, const Tuning& tuning, audio::Sink* voices = nullptr) -> void;

/// Run a whole input log: every record applied on its own tick, ticks in
/// between advanced empty, ending on the tick of the last record.
///
/// `records` must be ordered by tick — `replay::verify` is what guarantees it,
/// and `play` assumes it rather than re-checking, so that a caller cannot get a
/// silently different answer by skipping the load gate.
///
/// `voices` is forwarded to every `advance`, so a replayed session and a live
/// one emit the same voices through the same seam. That is deliberate and it is
/// why the footfall is derived inside `advance` rather than beside `apply`: one
/// emission path means a replay cannot sound different from the session it
/// recorded, and there is no second path to keep in step.
auto play(World& w, std::span<const replay::Record> records, const Tuning& tuning,
          audio::Sink* voices = nullptr) -> void;

}  // namespace gloam
