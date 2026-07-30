#pragma once

/// SPEC §6.1 and §6.3 — awareness, sight and light.
///
/// The target experience: you glimpse a monster crossing a distant
/// intersection, left to right, and you do not know whether it registered you.
///
/// The player NEVER sees a state label. Awareness is read entirely from
/// behaviour, so every transition carries an authored tell — and §6.1 is
/// explicit that "the tell is the deliverable, not the state machine". That is
/// why `step()` returns a `Tell`: the state change is bookkeeping, the tell is
/// the thing the render and audio layers exist to perform.

#include <cstdint>

#include "gloam/level.hpp"
#include "gloam/tuning.hpp"

namespace gloam {

/// §6.1 — UNAWARE -> SUSPICIOUS -> SEARCHING -> HUNTING -> LOST_TRACK -> UNAWARE
enum class Awareness : std::uint8_t {
  Unaware = 0,
  Suspicious = 1,
  Searching = 2,
  Hunting = 3,
  LostTrack = 4,
};

/// The authored behaviour that makes a transition legible without a label.
/// One per row of §6.1's table.
enum class Tell : std::uint8_t {
  None = 0,
  /// UNAWARE -> SUSPICIOUS: patrol rhythm breaks — halt for one tick, head
  /// turns toward the source.
  PatrolRhythmBreaks = 1,
  /// SUSPICIOUS -> SEARCHING: leaves the patrol route and walks to the last
  /// known position.
  LeavesPatrolRoute = 2,
  /// SEARCHING -> HUNTING: gait changes, direct pursuit, one audio sting.
  GaitChanges = 3,
  /// HUNTING -> LOST_TRACK: casts about at the last position, turning in place.
  CastsAbout = 4,
  /// LOST_TRACK -> UNAWARE: walks back to the patrol route and resumes.
  ResumesPatrol = 5,
};

/// A monster's perception profile. §6.3's `sees_unlit` monster arrives at M2 and
/// exists so that dousing is a decision rather than a dominant strategy — it
/// should not appear before the player has learned to trust the dark.
struct MonsterKind {
  Acuity acuity{Acuity::Normal};
  bool sees_unlit{false};
};

/// Range within which an unlit party is still visible to a `sees_unlit = false`
/// monster. §6.3: unlit is "effectively invisible beyond adjacent cells".
inline constexpr std::int32_t kAdjacentRange = 1;

/// §6.3 — the sight predicate, in full.
///
/// A monster sees the party if AND ONLY IF line of sight is clear, the party's
/// cell illumination is greater than zero, and range is within its sight
/// distance. `lamp_level` is the same integer that selects the light-field
/// plate in §4.4. There is no second light model — if you find yourself adding
/// an illumination term that is not this one, that is the bug.
[[nodiscard]] constexpr auto party_visible(std::int32_t lamp_level, bool sees_unlit,
                                           bool los_clear, std::int32_t range,
                                           std::int32_t sight_distance) -> bool {
  if (!los_clear) return false;
  if (range > sight_distance) return false;
  if (lamp_level > 0) return true;
  if (sees_unlit) return true;
  return range <= kAdjacentRange;
}

/// Line of sight between two cells.
///
/// Symmetric BY CONSTRUCTION: the walk is run from both endpoints and both must
/// agree. A one-directional raycast over a grid is not naturally symmetric —
/// the rounding at each step favours the origin — and an asymmetric LOS means a
/// monster can see a party that cannot see it, which reads to a player as the
/// game cheating rather than as a bug. §13.3 asserts the property; this is what
/// makes it hold.
[[nodiscard]] auto line_of_sight(const Level& level, Coord a, Coord b) -> bool;

/// Chebyshev range in cells. The grid is 90-degree movement, but sight is not
/// restricted to the axes, so diagonal distance counts as one step.
[[nodiscard]] constexpr auto range_between(Coord a, Coord b) -> std::int32_t {
  const auto dx = a.x > b.x ? a.x - b.x : b.x - a.x;
  const auto dy = a.y > b.y ? a.y - b.y : b.y - a.y;
  return dx > dy ? dx : dy;
}

/// What the world offered a monster's senses this tick.
struct Senses {
  bool heard{false};       ///< attenuated noise at the monster exceeded its threshold
  bool los_clear{false};   ///< `line_of_sight` between monster and party
  std::int32_t range{0};   ///< `range_between` the two
  std::int32_t lamp_level{kLampLevelDefault};
  Coord party_position{};
};

/// One monster's awareness state. Integer-only, per §5.1.
struct Perception {
  Awareness state{Awareness::Unaware};
  /// Ticks spent in the current state. Drives the LOST_TRACK and UNAWARE
  /// timers.
  std::int32_t ticks_in_state{0};
  /// Ticks of unbroken line of sight, for the SUSPICIOUS -> SEARCHING rule.
  std::int32_t los_streak{0};
  /// Ticks since the most recent perception hit; drives the SUSPICIOUS second-
  /// hit window and the HUNTING loss timer. Saturates rather than overflowing.
  std::int32_t ticks_since_hit{0};
  /// Where the party was when last perceived. §6.1's SEARCHING walks here.
  Coord last_known{};
  bool has_last_known{false};

  [[nodiscard]] auto operator==(const Perception&) const -> bool = default;
};

/// Advances one monster's awareness by exactly one tick.
///
/// Guarantees, both asserted in §13.3:
///  - the state advances at most ONE step per tick;
///  - it never regresses except by the §6.1 timers (HUNTING -> LOST_TRACK after
///    `hunting_lost_ticks`, LOST_TRACK -> UNAWARE after `lost_track_ticks`).
///
/// Returns the tell to perform, or `Tell::None` when the state did not change.
auto step(Perception& p, const Senses& senses, const MonsterKind& kind, const Tuning& tuning)
    -> Tell;

}  // namespace gloam
