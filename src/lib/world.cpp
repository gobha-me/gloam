#include "gloam/world.hpp"

#include <type_traits>
#include <utility>

// `world.hpp` does NOT include this, and that is deliberate. A distance field is
// how `advance` moves a monster, not part of what a `World` IS — nothing in the
// header's interface names one, so pulling it into every translation unit that
// merely ticks a world would be the mistake the `audio::Sink` forward
// declaration exists to avoid, one header over.
#include "gloam/path.hpp"

// `world.hpp` only forward-declares `audio::Sink`, which is all a pointer
// parameter needs. `advance` CALLS through that pointer, so the definition has
// to arrive somewhere — here, in the one translation unit that needs it, rather
// than in the header where it would reach every consumer.
#include "gloam/audio.hpp"

#include "bytes.hpp"

namespace gloam {
namespace {

/// Feeds scalars to a digest little-endian, one field at a time.
///
/// Never a `memcpy` of `World` or of any struct inside it: ABI padding is not a
/// value the program ever set, and a padding byte inside a world hash is a
/// cross-compiler divergence that looks exactly like the determinism regression
/// this hash exists to detect. `src/lib/bytes.hpp` has the long form.
class Absorb {
 public:
  explicit Absorb(hash::Sha256& into) : m_hash{into} {}

  auto u8(std::uint8_t v) -> void {
    std::byte b[1]{};
    le::put_u8(b, 0, v);
    m_hash.update(b);
  }

  auto u16(std::uint16_t v) -> void {
    std::byte b[2]{};
    le::put_u16(b, 0, v);
    m_hash.update(b);
  }

  auto u32(std::uint32_t v) -> void {
    std::byte b[4]{};
    le::put_u32(b, 0, v);
    m_hash.update(b);
  }

  auto u64(std::uint64_t v) -> void {
    std::byte b[8]{};
    le::put_u64(b, 0, v);
    m_hash.update(b);
  }

  /// Two's complement, which C++20 guarantees. No sign-magnitude branch to be
  /// wrong on a machine nobody has.
  auto i32(std::int32_t v) -> void { u32(static_cast<std::uint32_t>(v)); }

  auto boolean(bool v) -> void { u8(v ? 1U : 0U); }

  /// One byte, and the `static_assert` is what keeps that honest.
  ///
  /// Every enum hashed today is `std::uint8_t`-backed, so nothing is lost — but
  /// that fact is load-bearing for the digest and would otherwise be held by
  /// nothing but coincidence. `rng::Stream` is already `std::uint64_t`-backed,
  /// so a wider enum in this codebase is not hypothetical. Widen `CellKind` for
  /// §7's terrain and, without this line, kind 256 and kind 0 feed the digest
  /// identical bytes: two different worlds collide, `world.hpp`'s "it must be
  /// covered by `world_hash`" rule is satisfied on paper and broken in fact,
  /// and the golden replay gate reports PASSING. Make it a build break instead.
  template <typename E>
  auto enumeration(E v) -> void {
    static_assert(sizeof(std::underlying_type_t<E>) == 1,
                  "widen this helper before hashing an enum wider than a byte — a silent "
                  "truncation here is a determinism hole no test can see");
    u8(static_cast<std::uint8_t>(v));
  }

 private:
  hash::Sha256& m_hash;
};

/// `rng_state[stream - 1]`, bounds-checked.
///
/// `Stream` is a scoped enum over `uint64_t` whose enumerators start at 1, and
/// `world.hpp` states that invariant — but an enum class does not enforce its
/// own range. `Stream s{}` is well-formed and yields 0, and `0 - 1` on a
/// `size_t` is SIZE_MAX, so the unchecked form is an out-of-bounds write at a
/// wild offset through `std::array::operator[]`, which does no checking of its
/// own. `stream_of` and `save_stream` are public, so the argument can arrive
/// from a struct member nobody initialised or an integer read off a disk.
[[nodiscard]] auto stream_index(Stream s) -> std::size_t {
  const auto raw = static_cast<std::uint64_t>(s);
  if (raw < 1 || raw > kStreamCount) return 0;  // a real slot, never an underflow
  return static_cast<std::size_t>(raw - 1);
}

/// Saturating countdown. A cooldown that went negative would be a cooldown that
/// never fires again, which reads as "this monster stopped patrolling" and is
/// the hardest kind of bug to attribute.
auto count_down(std::int32_t& ticks) -> void {
  if (ticks > 0) --ticks;
}

/// `dwell[i]`, or zero when the vectors are not parallel.
///
/// `valid_route` refuses a mismatched pair, and `advance` does not call it —
/// which makes this the line that keeps the pump total over data the loader let
/// through. Zero rather than a clamp to the last element: a missing dwell is an
/// absent pause, not the previous one repeated.
[[nodiscard]] auto dwell_at(const Patrol& p, std::int32_t index) -> std::int32_t {
  if (index < 0 || static_cast<std::size_t>(index) >= p.dwell.size()) return 0;
  return static_cast<std::int32_t>(p.dwell[static_cast<std::size_t>(index)]);
}

/// Ticks the movement clock and reports whether this monster may step THIS tick.
///
/// Mutating and asking at once, which the name under-sells — but splitting them
/// would let one caller tick without asking or ask without ticking, and the
/// whole point of extracting it is that every mover charges the same clock.
/// `patrol_step`, `approach_step` and `rejoin_step` all go through here, so
/// §5.2's unnamed movement rate (gloam#29) cannot come to mean two different
/// things depending on WHY a monster is walking.
[[nodiscard]] auto ready_to_move(Monster& m) -> bool {
  count_down(m.move_cooldown);
  return m.move_cooldown <= 0;
}

/// The patrol form: an authored pause is paid BEFORE the clock is ticked.
///
/// SEQUENTIALLY, NOT IN PARALLEL, and the difference is the whole meaning of
/// `dwell`. Ticking both every tick makes the effective pause `max(dwell,
/// move_cooldown)` rather than `move_cooldown + dwell` — so at the default
/// period of 2, an authored dwell of 1 or 2 is a complete no-op and an author
/// gets no pause and no diagnostic. Measured before this line existed: dwell 0,
/// 1 and 2 all produced 20 moves in 40 ticks, and only dwell 3 moved the
/// number. `world.hpp` calls dwell "ticks owed on arriving", which is a debt
/// paid IN ADDITION to the cost of the step, not concurrently with it.
[[nodiscard]] auto ready_to_patrol(Monster& m) -> bool {
  // NOT `if (dwell) { --dwell; return false; } return ready_to_move(m);`, which
  // reads better and is a tick slower: it spends the tick that takes `dwell` to
  // zero, and then ANOTHER deciding the clock is clear. The original form lets
  // the step happen on the tick the debt is paid off, which is what P10's
  // arithmetic pins.
  if (m.patrol.dwell_left > 0) {
    --m.patrol.dwell_left;
  } else {
    count_down(m.move_cooldown);
  }
  return m.move_cooldown <= 0 && m.patrol.dwell_left <= 0;
}

/// Charges the cost of a step just taken.
///
/// Below 1 is clamped HERE, not in `Tuning` — tuning.hpp says why: a struct
/// that corrected itself would make `ruleset_hash` disagree with the bytes that
/// produced it.
///
/// `period`, NOT `period - 1`, and the difference is a tick. `ready_to_move`
/// runs before this on every tick, so the cooldown is already one lower by the
/// time the gate reads it — charging `period - 1` here bought only `period - 1`
/// ticks of gap, and at the default of 2 that is a monster stepping every
/// single tick while the tunable says otherwise. Caught by P3 and by the M0
/// golden, which is the pair worth having: one states the bound, the other
/// shows the walk.
auto charge_step(Monster& m, const Tuning& tuning) -> void {
  m.move_cooldown = tuning.monster_move_ticks > 1 ? tuning.monster_move_ticks : 1;
}

/// §6.4's idle variation (gloam#31), paid on ARRIVING at a route waypoint.
///
/// Only where the author already put a pause, so a plain corridor leg takes no
/// draw and a route with no pauses is a metronome. Widened to 64 bits before the
/// add because `dwell` tops out at 255 and the jitter is a tunable that could
/// arrive from a file as INT32_MAX.
auto pay_arrival_dwell(Patrol& p, std::int32_t index, const Tuning& tuning, Rng& patrol) -> void {
  const auto owed = dwell_at(p, index);
  auto total = static_cast<std::int64_t>(owed);
  if (owed > 0) total += patrol.range(0, tuning.patrol_idle_jitter_ticks);
  constexpr auto kMaxDwell = static_cast<std::int64_t>(INT32_MAX);
  p.dwell_left = static_cast<std::int32_t>(total > kMaxDwell ? kMaxDwell : total);
}

/// §6.4's pump: one tick of patrol for one monster. True if it changed cell.
///
/// Reports movement rather than emitting anything, and that is not tidiness.
/// The draw from `Stream::Patrol` happens in here; if this function could also
/// emit, the temptation would be to give it the sink, and a draw taken behind
/// `voices != nullptr` makes `--mute` and `--audio` produce different worlds.
/// §19 step 9's identity is a property of where the RNG lives.
[[nodiscard]] auto patrol_step(Monster& m, const Level& level, const Tuning& tuning, Rng& patrol)
    -> bool {
  const auto& route = m.patrol.route;
  if (route.size() < 2) return false;  // empty is "does not patrol"; one cell is malformed
  const auto count = static_cast<std::int32_t>(route.size());

  // A cursor off the end is survivable, not undefined. It can arrive from a
  // `level.gloam` that disagrees with itself, and a monster that stands still is
  // a reportable symptom where an out-of-bounds read is a crash somewhere else.
  if (m.patrol.waypoint < 0 || m.patrol.waypoint >= count) return false;

  if (!ready_to_patrol(m)) return false;

  // Ping-pong (gloam#28). `count >= 2` above is what makes both branches land
  // inside the route.
  bool reversed = m.patrol.reversed;
  std::int32_t next = 0;
  if (reversed) {
    if (m.patrol.waypoint == 0) {
      reversed = false;
      next = 1;
    } else {
      next = m.patrol.waypoint - 1;
    }
  } else {
    if (m.patrol.waypoint == count - 1) {
      reversed = true;
      next = count - 2;
    } else {
      next = m.patrol.waypoint + 1;
    }
  }

  // THE ONE MOVEMENT PREDICATE, and then a check that it went where the route
  // said. `Level::walk` alone would happily step somewhere adjacent when the
  // route names a cell that is not — a malformed route, or a monster standing
  // off its own route (gloam#32's gap). Requiring the destination to BE the
  // waypoint turns both into "does not move" instead of "drifts".
  const Coord target = route[static_cast<std::size_t>(next)];
  const Dir dir = facing_toward(m.at, target, m.facing);
  const auto destination = level.walk(m.at, dir);
  if (!destination || *destination != target) return false;

  m.at = *destination;
  m.facing = dir;
  m.patrol.waypoint = next;
  m.patrol.reversed = reversed;

  charge_step(m, tuning);
  pay_arrival_dwell(m.patrol, next, tuning, patrol);
  return true;
}

/// How close a HUNTING monster comes, in the pathfinder's own metric.
///
/// One: it stops on a cell cardinally adjacent to what it is chasing and never
/// walks onto it. Not a `Tuning` field, for `kAdjacentRange`'s reason — this is
/// not a knob, it is the shape of a rule that exists because §7's combat does
/// not. When combat lands, contact range becomes a property of a weapon rather
/// than a constant here.
inline constexpr std::int32_t kPursuitStandoff = 1;

/// One distance field per DISTINCT target per tick, built lazily.
///
/// THE SHARED-FIELD DISCIPLINE, AND IT IS NOT OPTIONAL. `advance` already builds
/// at most one noise field per tick and reads it at every monster; this is the
/// same trick for movement, and the comment further down records what skipping
/// it cost the last time — 13.6 ms against a 4 ms budget. Every HUNTING monster
/// that perceived the party this tick believes the same cell, so the common case
/// is one search for the whole roster. `test/10budgets/` times sixteen monsters
/// sharing one target against sixteen with sixteen, and requires the second to
/// cost more than twice the first — so reintroducing a search per monster goes
/// red on every box rather than only on a slow one.
///
/// A VECTOR WITH A LINEAR FIND, not a map: §5.1 forbids order-dependent
/// iteration over unordered containers, `Coord` has no ordering to give a sorted
/// one, and the roster is bounded at 31 by the voice ring — a linear scan over
/// at most 31 coordinates beats both.
///
/// CAPACITY IS RESERVED UP FRONT so that no insertion reallocates and every
/// reference handed out stays valid for the tick. That is a real invariant, not
/// a micro-optimisation: callers hold `const DistanceField&` across a step.
class FieldCache {
 public:
  explicit FieldCache(std::size_t roster) {
    // One target each plus the party's, one route each: the worst case is every
    // monster wanting something different, which is exactly what the budget row
    // measures.
    m_by_target.reserve(roster + 1);
    m_by_route.reserve(roster);
  }

  [[nodiscard]] auto for_target(const Level& level, Coord target) -> const DistanceField& {
    for (const auto& entry : m_by_target) {
      if (entry.first == target) return entry.second;
    }
    m_by_target.emplace_back(target, propagate_distance(level, target));
    return m_by_target.back().second;
  }

  /// Keyed on the route's own storage, because a route lives on its `Monster`
  /// and two monsters therefore never share one — the key is an identity, not a
  /// value, and comparing the cells instead would cost more than the search.
  [[nodiscard]] auto for_route(const Level& level, const std::vector<Coord>& route)
      -> const DistanceField& {
    const auto* key = route.data();
    for (const auto& entry : m_by_route) {
      if (entry.first == key) return entry.second;
    }
    m_by_route.emplace_back(key, propagate_distance(level, std::span<const Coord>{route}));
    return m_by_route.back().second;
  }

 private:
  std::vector<std::pair<Coord, DistanceField>> m_by_target{};
  std::vector<std::pair<const Coord*, DistanceField>> m_by_route{};
};

/// The lowest index in `route` naming `cell`, or -1.
///
/// LOWEST, because a route may name one cell twice and the two indices are
/// different waypoints with different dwells and different neighbours.
/// `test/19patrol/` already pins that a route "follows the index, not the
/// cell"; this is the same reading applied to arriving on one.
[[nodiscard]] auto lowest_index_of(const std::vector<Coord>& route, Coord cell) -> std::int32_t {
  for (std::size_t i = 0; i < route.size(); ++i) {
    if (route[i] == cell) return static_cast<std::int32_t>(i);
  }
  return -1;
}

/// §6.1's two translating tells that walk toward a remembered or seen position:
/// "leaves the patrol route, walks to the last known position" (SEARCHING, which
/// stops ON the cell) and "direct pursuit" (HUNTING, which stops one short).
///
/// `field` is rooted at `target`, so this is a descent rather than a search —
/// the cost is paid once per distinct target per tick by the caller's cache, not
/// once per monster.
///
/// STOPPING SHORT IS THE ARRIVAL RULE (gloam#32), and it is stated in the
/// PATHFINDER'S metric rather than in `range_between`'s. The field is
/// 4-connected and `kAdjacentRange` is Chebyshev, so the two disagree on the
/// diagonal; a rule that mixed them would be a rule nobody could predict. At M0
/// there is no combat, no death and no cell-occupancy rule, so a monster that
/// walks onto you is a more visible dead end than one that stops at the edge of
/// arm's reach and waits. When §7's combat lands, this halt becomes the attack
/// and not one line of the pathing changes.
[[nodiscard]] auto approach_step(Monster& m, const Level& level, const DistanceField& field,
                                 Coord target, std::int32_t stop_at, const Tuning& tuning) -> bool {
  const auto here = field.at(level, m.at);

  // No path at all: a target behind a shut door, inside rock, or in a sealed
  // room. The monster holds — never a drift toward it, which is what a greedy
  // stepper would do and is the failure §16's top risk row names.
  if (here == kUnreachable) return false;

  // Timers tick even on a tick it will not move, unlike the LOST_TRACK hold.
  // A monster halted at arm's reach is TRACKING you, not resting, so it steps
  // the moment you do rather than after up to `monster_move_ticks` of lag. It
  // still cannot exceed the rate: `charge_step` re-arms after every step, so
  // over any window the bound P3 asserts is unchanged.
  // LEAVING THE ROUTE DISCARDS THE PAUSE IT OWED. A monster part-way through a
  // four-tick authored dwell that notices you must not stand through the rest of
  // it first — you made a noise and it looked up. Cleared rather than saved: the
  // pause belongs to a waypoint, and the ping-pong re-authors it on arriving
  // there again.
  m.patrol.dwell_left = 0;
  const bool ready = ready_to_move(m);

  if (here <= stop_at) {
    // Arrived. Turn to face what it came for and hold — the halted-hunter pose
    // §4.2 will draw, and hashed state so a replay reproduces it.
    m.facing = facing_toward(m.at, target, m.facing);
    return false;
  }
  if (!ready) return false;

  const auto dir = step_down(field, level, m.at, m.facing);
  if (!dir) return false;
  const auto destination = level.walk(m.at, *dir);
  if (!destination) return false;  // belt and braces: `step_down` already walked it

  m.at = *destination;
  m.facing = *dir;
  charge_step(m, tuning);
  return true;
}

/// §6.1's third translating tell: "walks back to the patrol route and resumes".
///
/// `field` is seeded with EVERY cell of this monster's route, so descending it
/// walks to the nearest one and "nearest" is the search's answer rather than a
/// loop over candidate targets. Arriving adopts that cell's waypoint and pays
/// its dwell, because the monster did arrive there.
///
/// This is also the re-join rule gloam#28 left open, and the two are one code
/// path on purpose: the tell fires on the LOST_TRACK -> UNAWARE transition, and
/// the sustained behaviour is "would patrol, is not on its route, walks home".
/// A monster placed off its route by an author gets the same walk, which retires
/// `world.hpp`'s "never moves again" caveat.
[[nodiscard]] auto rejoin_step(Monster& m, const Level& level, const DistanceField& field,
                               const Tuning& tuning, Rng& patrol) -> bool {
  const auto here = field.at(level, m.at);
  if (here == kUnreachable) return false;  // its own route is walled off from it
  if (here == 0) return false;             // already home; the cursor is the caller's problem
  if (!ready_to_move(m)) return false;

  const auto dir = step_down(field, level, m.at, m.facing);
  if (!dir) return false;
  const auto destination = level.walk(m.at, *dir);
  if (!destination) return false;

  m.at = *destination;
  m.facing = *dir;
  charge_step(m, tuning);

  if (field.at(level, m.at) == 0) {
    const auto index = lowest_index_of(m.patrol.route, m.at);
    if (index >= 0) {
      m.patrol.waypoint = index;
      pay_arrival_dwell(m.patrol, index, tuning, patrol);
    }
  }
  return true;
}

/// True when this monster is standing exactly where its cursor says it is, and
/// may therefore ping-pong rather than walk home.
[[nodiscard]] auto at_its_waypoint(const Monster& m) -> bool {
  const auto count = static_cast<std::int32_t>(m.patrol.route.size());
  if (m.patrol.waypoint < 0 || m.patrol.waypoint >= count) return false;
  return m.patrol.route[static_cast<std::size_t>(m.patrol.waypoint)] == m.at;
}

/// One tick of movement for one monster, whatever it currently believes.
///
/// THE DISPATCHER §6.1 ASKS FOR. "The tell is the deliverable, not the state
/// machine", and this is where a state becomes a behaviour: the two tells that
/// are only a head turn are performed here, and everything else is routed to
/// the mover that matches what the monster is doing.
[[nodiscard]] auto monster_step(Monster& m, const Level& level, Tell tell, const Tuning& tuning,
                                Rng& patrol, FieldCache& fields) -> bool {
  // §6.1's two tells that are a head turn and nothing else. Both are performed
  // here rather than by a renderer because `facing` is hashed state — a tell a
  // replay cannot reproduce is not a tell, it is an animation.
  //
  // BEFORE THE TIMERS TICK, and that is what makes the halt cost anything. Run
  // after them, a halt only withholds a step on the one tick in `period` where
  // a step was actually due — at the default of 2 that is half of them, so
  // §6.1's "halt for one tick" would be observable or not depending on the
  // parity of the tick you were noticed on. Returning here delays the whole
  // schedule by exactly one tick, every time.
  switch (tell) {
    case Tell::PatrolRhythmBreaks:
      // "patrol rhythm breaks — halt for one tick, head turns toward the
      // source." ONE TICK, not a freeze: the monster patrols again next tick.
      // The source is where it believes you were, so a monster that heard you
      // through a wall turns toward the wall, which is correct and is most of
      // why the tell reads.
      if (m.mind.has_last_known) m.facing = facing_toward(m.at, m.mind.last_known, m.facing);
      return false;
    case Tell::CastsAbout:
      // "casts about at the last position, turning in place." Turning in place
      // is a facing write; "at the last position" is satisfied vacuously today,
      // because a monster that never pursued you never left where it stood.
      // That stops being vacuous with gloam#32.
      m.facing = static_cast<Dir>((static_cast<int>(m.facing) + 1) % kDirCount);
      return false;
    default: break;
  }

  switch (m.mind.state) {
    case Awareness::Unaware:
    case Awareness::Suspicious:
      // Patrolling, or walking back to the route it is not standing on. A route
      // of fewer than two cells is "does not patrol", and there is nothing to
      // walk home to.
      if (at_its_waypoint(m)) return patrol_step(m, level, tuning, patrol);
      if (m.patrol.route.size() < 2) return false;
      return rejoin_step(m, level, fields.for_route(level, m.patrol.route), tuning, patrol);

    case Awareness::Searching:
      // "leaves the patrol route, walks to the last known position." A monster
      // with nothing remembered has nowhere to go — SEARCHING is reached by two
      // perception hits, so this is a defensive read rather than a live case.
      if (!m.mind.has_last_known) return false;
      return approach_step(m, level, fields.for_target(level, m.mind.last_known),
                           m.mind.last_known, /*stop_at=*/0, tuning);

    case Awareness::Hunting:
      // "gait changes, direct pursuit." TOWARD WHAT IT BELIEVES, not toward
      // `w.party`: `hunting_lost_ticks` is 8, so a hunter can hold a stale
      // belief for eight ticks, and pursuing the true position would hand it
      // knowledge it does not have. A monster rounding a corner you have
      // already left is §6.1 working; one that tracks you through a wall it
      // cannot hear you through is §16's top risk row.
      if (!m.mind.has_last_known) return false;
      return approach_step(m, level, fields.for_target(level, m.mind.last_known),
                           m.mind.last_known, /*stop_at=*/kPursuitStandoff, tuning);

    case Awareness::LostTrack:
      // The one state that still holds position, and its timers stop with it:
      // a monster casting about is not quietly accruing the right to a step it
      // will take the instant it calms down.
      return false;
  }
  return false;
}

}  // namespace

auto make_world(std::uint64_t seed, Level level, std::vector<Monster> monsters) -> World {
  World w{};
  w.seed = seed;
  w.level = std::move(level);
  w.monsters = std::move(monsters);
  for (std::size_t i = 0; i < kStreamCount; ++i) {
    w.rng_state[i] = rng(seed, static_cast<Stream>(i + 1)).state();
  }
  return w;
}

auto stream_of(const World& w, Stream s) -> Rng { return Rng{w.rng_state[stream_index(s)]}; }

auto save_stream(World& w, Stream s, const Rng& generator) -> void {
  w.rng_state[stream_index(s)] = generator.state();
}

auto valid_route(const Level& level, std::span<const Coord> route,
                 std::span<const std::uint8_t> dwell) -> bool {
  // Empty is legal and means "does not patrol". A single cell is NOT a synonym
  // for that: `world.hpp` makes empty the one representation of standing still,
  // and a one-cell route is data that meant to say something else.
  if (route.empty()) return dwell.empty();
  if (route.size() < 2) return false;
  if (route.size() != dwell.size()) return false;

  for (std::size_t i = 0; i < route.size(); ++i) {
    if (!level.navigable(route[i])) return false;  // covers out of bounds: the void is not floor
    if (i + 1 == route.size()) break;
    if (route[i] == route[i + 1]) return false;

    // ADJACENT AND REACHABLE, checked with the predicate that will actually be
    // used to walk it. Testing adjacency alone would accept a route that steps
    // through a wall, which is exactly the data error a monster cannot report.
    bool reachable = false;
    for (int d = 0; d < kDirCount && !reachable; ++d) {
      const auto next = level.walk(route[i], static_cast<Dir>(d));
      reachable = next && *next == route[i + 1];
    }
    if (!reachable) return false;
  }
  return true;
}

auto world_hash(const World& w) -> hash::Digest {
  hash::Sha256 h{};
  Absorb in{h};

  // The version leads, so that a change to WHAT is hashed is distinguishable
  // from a change to what the simulation computed.
  in.u8(kWorldHashVersion);

  in.u64(w.seed);
  in.u32(w.tick);
  in.i32(w.party.x);
  in.i32(w.party.y);
  in.enumeration(w.facing);
  in.i32(w.lamp_level);
  in.boolean(w.creeping);
  in.enumeration(w.armour);
  in.i32(w.pending_noise);

  in.i32(w.level.width());
  in.i32(w.level.height());
  for (std::size_t i = 0; i < w.level.cell_count(); ++i) {
    const auto& cell = w.level.at(w.level.coord_of(i));
    in.enumeration(cell.kind);
    for (const auto& edge : cell.edges) {
      in.enumeration(edge.kind);
      in.enumeration(edge.state);
      in.u16(edge.key_id);
      in.u8(edge.attenuation_override);
    }
    in.u16(cell.inscription_id);
    // LENGTH-PREFIXED, and it has to be: without the count, a level with
    // [[1], [2]] in two cells and one with [[1, 2], []] feed the digest the
    // same bytes in the same order.
    in.u32(static_cast<std::uint32_t>(cell.contents.size()));
    for (const auto id : cell.contents) in.u16(id);
  }

  in.u32(static_cast<std::uint32_t>(w.monsters.size()));
  for (const auto& m : w.monsters) {
    in.i32(m.at.x);
    in.i32(m.at.y);
    in.enumeration(m.kind.acuity);
    in.boolean(m.kind.sees_unlit);
    in.enumeration(m.mind.state);
    in.i32(m.mind.ticks_in_state);
    in.i32(m.mind.los_streak);
    in.i32(m.mind.ticks_since_hit);
    in.i32(m.mind.last_known.x);
    in.i32(m.mind.last_known.y);
    in.boolean(m.mind.has_last_known);

    // §6.4's route and its cursor. BOTH VECTORS GET THEIR OWN LENGTH PREFIX,
    // for the reason the cell-contents prefix above states and one more: these
    // two are adjacent and are supposed to be parallel, so a single shared
    // count would make route {A} + dwell {B} and route {A,B} + dwell {} feed
    // the digest the same bytes — and telling those apart is the entire job of
    // a hash over data a loader might have got wrong.
    in.enumeration(m.facing);
    in.u32(static_cast<std::uint32_t>(m.patrol.route.size()));
    for (const auto c : m.patrol.route) {
      in.i32(c.x);
      in.i32(c.y);
    }
    in.u32(static_cast<std::uint32_t>(m.patrol.dwell.size()));
    for (const auto d : m.patrol.dwell) in.u8(d);

    // The CURSOR is hashed, not just the route. A replay resumed at tick N must
    // resume mid-dwell and mid-cooldown; a monster that reset to the top of its
    // route on load would diverge from the recording within two ticks while the
    // route itself compared equal.
    in.i32(m.patrol.waypoint);
    in.boolean(m.patrol.reversed);
    in.i32(m.patrol.dwell_left);
    in.i32(m.move_cooldown);
  }

  // Every stream EXCEPT Ambience, which rng.hpp calls "non-simulation flavour;
  // never feeds back into sim state". Drawing a different ambient sound must
  // not be reportable as a determinism regression.
  static_assert(static_cast<std::size_t>(Stream::Ambience) == kStreamCount,
                "Ambience must be the last stream for this loop bound to exclude it");
  for (std::size_t i = 0; i + 1 < kStreamCount; ++i) in.u64(w.rng_state[i]);

  return h.finish();
}

auto apply(World& w, replay::Event event, std::uint16_t payload, const Tuning& tuning) -> void {
  switch (event) {
    case replay::Event::Step: {
      const auto dir = static_cast<Dir>(payload);
      // ONE MOVEMENT PREDICATE, and `Level::walk` is it. This used to open-code
      // the passable-edge and navigable-destination pair, which was correct and
      // was also the second copy of a rule §6.4's patrols need a third of. A
      // party that can step somewhere a monster cannot is a bug nobody would
      // think to test for, because both halves look right in isolation.
      const auto destination = w.level.walk(w.party, dir);
      if (!destination) return;
      w.party = *destination;
      // Two inputs can share a tick (§3), and two footfalls 100 ms apart are
      // one sound event rather than a louder one. MAX, not sum: summing would
      // let an input stream manufacture arbitrary loudness out of nothing,
      // which §6.2 plainly does not intend even though it does not say so.
      const auto emitted = step_noise(w.armour, w.creeping, tuning);
      w.pending_noise = emitted > w.pending_noise ? emitted : w.pending_noise;
      return;
    }
    case replay::Event::Turn: w.facing = static_cast<Dir>(payload); return;
    case replay::Event::Lamp: w.lamp_level = static_cast<std::int32_t>(payload); return;
    case replay::Event::Creep: w.creeping = payload != 0; return;
    case replay::Event::Wait: return;
    case replay::Event::None: return;  // refused at load; total here rather than trusting that
  }
}

auto advance(World& w, const Tuning& tuning, audio::Sink* voices) -> void {
  const auto field = propagate_noise(w.level, w.party, w.pending_noise, tuning);

  if (voices != nullptr) {
    voices->note_tick(w.tick);

    // THE PARTY'S OWN FOOTFALL, READ OUT OF THE FIELD THE MONSTERS ARE ABOUT TO
    // BE TESTED AGAINST. Source is the listener, so it is unattenuated and dead
    // centre — and it costs no second propagation. §9.3's "one system read from
    // two positions", in one call.
    //
    // Derived from `pending_noise` rather than emitted beside `apply`, which is
    // the non-obvious half. `apply` already refuses to emit when a step runs
    // into an impassable edge ("you did not take a step"), and it already takes
    // the MAX of two footfalls sharing a tick rather than their sum. Reading the
    // state instead of intercepting the event inherits both rules for free
    // instead of restating them somewhere they could drift.
    if (w.pending_noise > 0) {
      const auto mix =
          audio::mix_at(field, w.level, w.party, w.facing, w.party, w.pending_noise);
      voices->play(audio::SoundId::PartyFootfall, mix.gain, mix.pan);
    }
  }

  // ONE FIELD FOR EVERY STING THIS TICK, NOT ONE PER MONSTER.
  //
  // §9.3 says the mix is the propagation "evaluated from the party's position
  // instead of the monster's", and that is meant literally: this field is rooted
  // at the party and read at each monster that sounds. Noise cost is symmetric
  // (see `audio::mix_reciprocal`), so the answer equals what a field rooted at
  // the monster would have given, and the cost stops scaling with the monster
  // count.
  //
  // That is not a micro-optimisation. Measured at §11's own reference scale —
  // 32x32 cells, 16 monsters, every one of them stinging on the same tick — a
  // propagation per monster cost 13.6 ms against a 4 ms tick budget, and the
  // shared field costs 0.97 ms. The obvious implementation was over budget by
  // 3.4x, so this is what keeps §11's row true rather than a tidy-up.
  // `test/10budgets/` measures exactly that tick, and would go red again.
  //
  // Built LAZILY: most ticks contain no sting at all, and an eagerly built field
  // would put the cost back on every tick instead of taking it off the rare one.
  // Seeded with `kStingEmission` exactly, which is the sizing rule
  // `audio::mix_reciprocal` documents — a smaller seed truncates the field early
  // and would silence a sting that should have been heard.
  NoiseField sting_field{};
  bool sting_field_built = false;

  // A SECOND lazy field, and it cannot be the one above. Same shape, same
  // reasoning, different seed — and the difference is load-bearing rather than
  // tidy. `gain_from_loudness` clamps to unity when the arriving loudness meets
  // the emission, so reading the 90-seeded sting field for a 14-emission
  // footfall would report every monster in the level walking at full volume.
  // `audio::kMonsterFootfallEmission` carries the arithmetic and the escape.
  //
  // Built lazily for the sting field's reason and one more: most ticks move no
  // monster at all, because `monster_move_ticks` is 2 and dwells are longer.
  NoiseField step_field{};
  bool step_field_built = false;

  // §6.4: "drawn from the `patrol` RNG stream, never the shared one."
  //
  // ONE generator for the whole roster, drawn in roster order, saved once below
  // UNCONDITIONALLY — saving after a tick with no draws writes the same value
  // back, and one code path is worth more than the store it saves. Built here
  // rather than per monster because a generator per monster would advance eight
  // independent copies of the same state and make the draw order unobservable.
  //
  // Note what this is NOT gated on: `voices`. `test/19patrol/` asserts that
  // after 1000 ticks only this stream has moved, and §19 step 9's `--mute`
  // identity depends on the draw being taken either way.
  Rng patrol = stream_of(w, Stream::Patrol);

  // §6.1's three translating tells, and the one field per distinct target that
  // serves them. Lazy for the same reason as the two above: most ticks ask for
  // nothing at all, because most monsters are patrolling a route they are
  // already standing on.
  FieldCache fields{w.monsters.size()};

  for (auto& m : w.monsters) {
    Senses senses{};
    senses.heard =
        hears(field, w.level, m.at, m.kind.acuity, m.mind.state == Awareness::Hunting, tuning);
    senses.los_clear = line_of_sight(w.level, m.at, w.party);
    senses.range = range_between(m.at, w.party);
    senses.lamp_level = w.lamp_level;
    senses.party_position = w.party;

    // §6.1'S SEARCHING EXIT, DERIVED HERE BECAUSE THIS IS WHERE THE FIELD IS.
    //
    // "The trail ends here" is a question about the map, and `perception.cpp`
    // must not learn how a monster paths — so the pathfinding half is answered
    // out here and handed over as a bool, exactly as `heard` and `los_clear`
    // are.
    //
    // GATED ON THE TICK THE EDGE COULD ACTUALLY FIRE, and that gate is what
    // keeps it to one field rather than two. `step` moves `last_known` on any
    // perception hit, so a trail question asked on a hit tick would be about a
    // different cell than the movement below walks toward, and the tick would
    // cost two searches. On a tick with no hit, `last_known` does not move and
    // the same cached field serves both. The residue is one extra field on the
    // exact tick a cold-trailed searcher SEES you — on which it hunts anyway.
    if (m.mind.state == Awareness::Searching && !senses.heard &&
        m.mind.ticks_since_hit + 1 >= tuning.hunting_lost_ticks) {
      if (!m.mind.has_last_known) {
        senses.trail_exhausted = true;  // nothing remembered is nothing left to search
      } else {
        const auto reach = fields.for_target(w.level, m.mind.last_known).at(w.level, m.at);
        senses.trail_exhausted = reach == kUnreachable || reach == 0;
      }
    }

    // The tell used to be discarded here. §6.1 calls it "the deliverable, not
    // the state machine", and this is the line that finally makes that true.
    const Tell tell = step(m.mind, senses, m.kind, tuning);

    // §6.4's pump. AFTER `step`, so this tick's tell and this tick's awareness
    // both drive it — a monster that just became SUSPICIOUS halts on the tick it
    // noticed you, not on the one after.
    const bool moved = monster_step(m, w.level, tell, tuning, patrol, fields);

    // §9's first-named sound, emitted from the cell the monster ARRIVED at.
    // Before the sting below, so that a tick carrying both puts them in the
    // order they happened: this monster took a step, and then something about
    // it changed. Roster order across monsters, step-then-sting within one.
    if (voices != nullptr && moved) {
      if (!step_field_built) {
        step_field = propagate_noise(w.level, w.party, audio::kMonsterFootfallEmission, tuning);
        step_field_built = true;
      }
      const auto mix = audio::mix_reciprocal(step_field, w.level, w.party, w.facing, m.at,
                                             audio::kMonsterFootfallEmission);
      voices->play(audio::SoundId::MonsterFootfall, mix.gain, mix.pan);
    }

    // §6.1: "gait changes, direct pursuit, ONE AUDIO STING."
    //
    // KEYED ON THE TELL, NOT ON THE STATE, and the difference is the whole
    // point. `Tell::SnapsBack` also arrives at HUNTING and pointedly gets
    // NOTHING — `perception.hpp` spells out why: the sting means "found you",
    // and its absence means "never lost you", which is the honest report from a
    // monster that was already searching the right place. Silence is the tell,
    // and it is the worse thing to hear.
    //
    // Writing this as `m.mind.state == Awareness::Hunting` passes every other
    // test in the suite and breaks that distinction. `test/16audiosim/` is what
    // catches it.
    if (voices != nullptr && tell == Tell::GaitChanges) {
      if (!sting_field_built) {
        sting_field = propagate_noise(w.level, w.party, audio::kStingEmission, tuning);
        sting_field_built = true;
      }
      const auto mix = audio::mix_reciprocal(sting_field, w.level, w.party, w.facing, m.at,
                                             audio::kStingEmission);
      voices->play(audio::SoundId::HuntingSting, mix.gain, mix.pan);
    }
  }

  save_stream(w, Stream::Patrol, patrol);

  w.pending_noise = 0;
  ++w.tick;
}

auto play(World& w, std::span<const replay::Record> records, const Tuning& tuning,
          audio::Sink* voices) -> void {
  for (const auto& record : records) {
    while (w.tick < record.tick) advance(w, tuning, voices);
    apply(w, record.event, record.payload, tuning);
  }
  // The last tick's inputs have been applied but not yet simulated. Without
  // this the final world hash would be taken from a world that had heard
  // nothing the player just did.
  advance(w, tuning, voices);
}

}  // namespace gloam
