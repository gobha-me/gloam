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
///   * MONSTERS DO NOT MOVE. §6.4's patrol routes do not exist, so `advance`
///     runs each monster's awareness and leaves it where it stands. This is
///     what the reference tick did; naming it here stops it reading as a bug.
///   * NOTHING DRAWS FROM AN RNG STREAM. `Perception::step` is deterministic
///     given its senses. `rng_state` is carried and hashed anyway, so that the
///     first subsystem to draw does not also have to change the replay format.
///   * `creep_tick_cost` IS NOT APPLIED HERE. §6.2 makes creeping cost ticks as
///     well as halving the noise, but there is no movement-rate model to charge
///     them against — how often a `Step` may legally appear is the driver's
///     rule, not `advance`'s. `step_noise` already halves.

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

namespace gloam {

/// One monster: where it is, what it is, and what it currently believes.
///
/// `perception.hpp`'s `Perception` is an awareness state with no position and
/// no kind attached, because it was written to be driven by a caller holding
/// both. This is that caller, made into a type.
struct Monster {
  Coord at{};
  MonsterKind kind{};
  Perception mind{};

  [[nodiscard]] auto operator==(const Monster&) const -> bool = default;
};

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
inline constexpr std::uint8_t kWorldHashVersion = 1;

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
auto advance(World& w, const Tuning& tuning) -> void;

/// Run a whole input log: every record applied on its own tick, ticks in
/// between advanced empty, ending on the tick of the last record.
///
/// `records` must be ordered by tick — `replay::verify` is what guarantees it,
/// and `play` assumes it rather than re-checking, so that a caller cannot get a
/// silently different answer by skipping the load gate.
auto play(World& w, std::span<const replay::Record> records, const Tuning& tuning) -> void;

}  // namespace gloam
