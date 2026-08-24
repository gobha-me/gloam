#include "game.hpp"

/// Everything about the mapping, the two pumps and the seal is documented in
/// `game.hpp`; what lives here is only what needs statements.
///
/// NOTE WHAT THIS FILE DOES NOT INCLUDE: `gloam/audio.hpp`. The sink is only
/// ever forwarded BY POINTER into `advance`, and a pointer needs no
/// definition \u2014 `world.hpp`'s forward declaration is the whole interface this
/// translation unit uses. That is the load-bearing part of "the core never
/// gates on the sink": there is not even a member function in scope to call.

namespace gloam::game {

auto Core::handle_key(Key key) -> bool {
  if (m_finished) return false;
  const auto input = map_key(key, m_world);
  if (!input) return false;  // Unbound: the absence of an action, not an error

  if (m_mode == PumpMode::StepTimed) {
    // Commit, then pay the cost in ticks. The record tick is the tick the
    // action landed on, before any of these advances \u2014 `play()` will advance
    // the same ticks between this record and the next.
    const auto cost = commit(*input);
    for (std::int32_t i = 0; i < cost; ++i) advance(m_world, m_tuning, m_voices);
    return true;
  }

  // Real-time. A creep step's lockout refuses new actions outright \u2014 dropped,
  // not banked (the header comment is the argument). Otherwise the pending
  // slot is overwritten: latest-wins, because an uncommitted action is intent
  // and intent is replaceable.
  if (m_lockout > 0) return false;
  m_pending = *input;
  return true;
}

auto Core::tick() -> void {
  if (m_finished) return;
  if (m_mode != PumpMode::RealTime) return;  // step-timed: no action, no ticks

  if (m_lockout > 0) {
    // A tick the creep step still occupies: no action may commit, and the
    // pending slot stays empty because `handle_key` refused during the
    // lockout. The world advances anyway \u2014 the monsters are the point.
    --m_lockout;
  } else if (m_pending.has_value()) {
    const auto cost = commit(*m_pending);
    m_pending.reset();
    // The committing tick is the first of the cost; the rest are lockout.
    m_lockout = cost - 1;
  }
  advance(m_world, m_tuning, m_voices);
}

auto Core::finish(std::uint64_t pack_hash) -> Outcome {
  if (m_finished) return m_outcome;

  // An uncommitted pending action is discarded here: it never reached the
  // world, so it must not reach the file.
  m_pending.reset();

  // The sealing Wait \u2014 the header comment has the whole argument. Short form:
  // `play()` settles every replay one advance past its last record, so the
  // last record is made to be a Wait on the session's final tick, and the
  // live end state and the replayed end state coincide by construction.
  m_log.push_back({m_world.tick, replay::Event::Wait, 0});
  advance(m_world, m_tuning, m_voices);

  m_outcome = Outcome{m_world.seed, world_hash(m_world),
                      replay::Expect{ruleset_hash(m_tuning), pack_hash}};
  m_finished = true;
  return m_outcome;
}

}  // namespace gloam::game
