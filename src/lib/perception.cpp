#include "gloam/perception.hpp"

#include <limits>

namespace gloam {

namespace {

/// Saturating increment. A tick counter that wraps would turn a monster that
/// has been searching for a very long time into one that just heard something,
/// which is the kind of bug that only appears in a long session and never in a
/// test.
constexpr auto tick_up(std::int32_t v) -> std::int32_t {
  return v == std::numeric_limits<std::int32_t>::max() ? v : v + 1;
}

/// One directional raycast, cardinal steps only.
///
/// Not symmetric on its own — the error term's rounding favours the origin, so
/// a ray from A to B can clip a corner that a ray from B to A does not.
/// `line_of_sight` runs it both ways and requires agreement, which is what
/// makes the §13.3 symmetry property hold.
auto raycast(const Level& level, Coord from, Coord to) -> bool {
  if (from == to) return true;
  if (!level.in_bounds(from) || !level.in_bounds(to)) return false;

  const std::int32_t dx = to.x > from.x ? to.x - from.x : from.x - to.x;
  const std::int32_t dy = to.y > from.y ? to.y - from.y : from.y - to.y;
  const Dir horizontal = to.x > from.x ? Dir::East : Dir::West;
  const Dir vertical = to.y > from.y ? Dir::South : Dir::North;

  std::int32_t error = dx - dy;
  Coord cursor = from;

  // One cardinal step per iteration, so the walk can never cut a corner
  // diagonally through two walls. Bounded by the Manhattan distance, which is
  // exactly the number of steps a cardinal-only walk takes.
  for (std::int32_t guard = dx + dy; guard > 0 && cursor != to; --guard) {
    const std::int32_t doubled = error * 2;
    // Prefer the horizontal step when both axes are due. The choice only has to
    // be deterministic — the both-ways agreement above is what makes it fair.
    const bool step_horizontal = doubled > -dy && cursor.x != to.x;

    Dir dir{};
    if (step_horizontal) {
      error -= dy;
      dir = horizontal;
    } else {
      error += dx;
      dir = vertical;
    }

    if (!level.edge(cursor, dir).transparent()) return false;
    cursor = cursor.step(dir);
    if (!level.in_bounds(cursor)) return false;
  }

  return cursor == to;
}

}  // namespace

auto line_of_sight(const Level& level, Coord a, Coord b) -> bool {
  return raycast(level, a, b) && raycast(level, b, a);
}

auto step(Perception& p, const Senses& senses, const MonsterKind& kind, const Tuning& tuning)
    -> Tell {
  const std::int32_t sight = tuning.sight_distance(kind.acuity);

  // §6.3 — the one and only illumination test.
  const bool saw =
      party_visible(senses.lamp_level, kind.sees_unlit, senses.los_clear, senses.range, sight);
  const bool hit = senses.heard || saw;

  // Unbroken line of sight, for the SUSPICIOUS -> SEARCHING rule. "Unbroken
  // LOS" means the monster could actually see the party, not merely that the
  // geometry was clear — an unlit party in a straight corridor is not being
  // watched.
  p.los_streak = saw ? tick_up(p.los_streak) : 0;

  if (hit) {
    p.ticks_since_hit = 0;
    p.last_known = senses.party_position;
    p.has_last_known = true;
  } else {
    p.ticks_since_hit = tick_up(p.ticks_since_hit);
  }
  p.ticks_in_state = tick_up(p.ticks_in_state);

  const Awareness before = p.state;
  Awareness next = before;

  // Exactly one transition may fire per call. This switch is the whole state
  // machine, and §13.3 asserts that awareness never advances two states in one
  // tick — which holds because there is no loop here and no fallthrough.
  switch (before) {
    case Awareness::Unaware:
      // One perception hit, heard or seen.
      if (hit) next = Awareness::Suspicious;
      break;

    case Awareness::Suspicious:
      // A second hit within the window, or N ticks of unbroken LOS.
      if ((hit && p.ticks_in_state <= tuning.suspicious_second_hit_window) ||
          p.los_streak >= tuning.suspicious_los_ticks) {
        next = Awareness::Searching;
      }
      break;

    case Awareness::Searching:
      // LOS clear AND party lit AND range <= sight — which is exactly `saw`.
      if (saw) next = Awareness::Hunting;
      break;

    case Awareness::Hunting:
      if (p.ticks_since_hit >= tuning.hunting_lost_ticks) next = Awareness::LostTrack;
      break;

    case Awareness::LostTrack:
      // Re-acquisition, ordered BEFORE the timer so a monster that can see the
      // party does not spend a tick deciding to forget them.
      //
      // §6.1's table shipped without this row, which meant a monster casting
      // about at the last known position could be looking straight at a lit,
      // adjacent party and do nothing for 43+ ticks — the full LOST_TRACK timer
      // plus the climb back through SUSPICIOUS and SEARCHING. That reads to a
      // player as broken AI, and it undercuts the tension the whole §6 model
      // exists to produce.
      //
      // Added by design decision on gloam#12, WITH its own tell — see
      // Tell::SnapsBack. §6.1 is explicit that a transition without an authored
      // tell is the failure mode, which is why this was left unimplemented
      // rather than quietly patched until the tell was decided.
      if (saw) {
        next = Awareness::Hunting;
      } else if (p.ticks_in_state >= tuning.lost_track_ticks) {
        next = Awareness::Unaware;
      }
      break;
  }

  if (next == before) return Tell::None;

  p.state = next;
  p.ticks_in_state = 0;

  // Keyed on the PAIR, not the destination. Two paths now reach HUNTING and
  // §6.1's "every transition has an authored tell" means they must not read
  // alike: SEARCHING -> HUNTING is a discovery and gets the sting, LOST_TRACK ->
  // HUNTING is a re-acquisition and pointedly does not.
  if (next == Awareness::Hunting) {
    return before == Awareness::LostTrack ? Tell::SnapsBack : Tell::GaitChanges;
  }

  switch (next) {
    case Awareness::Suspicious: return Tell::PatrolRhythmBreaks;
    case Awareness::Searching: return Tell::LeavesPatrolRoute;
    case Awareness::LostTrack: return Tell::CastsAbout;
    case Awareness::Unaware: return Tell::ResumesPatrol;
    case Awareness::Hunting: return Tell::GaitChanges;  // handled above
  }
  return Tell::None;
}

}  // namespace gloam
