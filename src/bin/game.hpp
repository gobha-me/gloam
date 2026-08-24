#pragma once

/// SPEC \u00a75.2, \u00a715 (M0) \u2014 the driver `world.hpp` says does not exist yet.
///
/// `world.hpp` is explicit about the gap this module fills: "`creep_tick_cost`
/// IS NOT APPLIED TO THE PARTY. ... how often a `Step` may legally appear is
/// the driver's rule, not `advance`'s, and there is no driver until gloam#7."
/// This is that driver (gloam#8): a DEVICE-FREE game core that a termforge App
/// shell \u2014 a later unit \u2014 drives. No termforge, no RtAudio, no clock, no file
/// descriptor: the shell owns the keyboard, the frame schedule and the audio
/// device; this core owns the rules for what a key press may do to a `World`,
/// when the world ticks, and what gets recorded.
///
/// \u00a75.2: "Both play modes share ALL game code \u2014 same AI, same patrols, same
/// perception, same pathfinding. Only the pump differs." So there is ONE core
/// with two pump policies, not two cores: the mode chooses who drives the
/// clock, and every other line \u2014 the mapping, the recording, the sink \u2014 is
/// shared. What that buys is exactly what \u00a75.2 says it buys: real-time vs
/// step-timed stays an empirical question rather than two builds.
///
///
/// THE MAPPING
///
/// `map_key` is a pure function from (key, world) to at most one
/// `replay::Input`. It reads the world \u2014 not just `facing` \u2014 because two keys
/// are defined by the state they act on: LampUp/LampDown clamp against the
/// CURRENT lamp level, and CreepToggle's payload is the opposite of the
/// CURRENT creep flag. Step and Turn payloads are absolute `Dir`s, which is
/// the replay format's own rule (`replay.hpp`: Turn's payload is "the Dir now
/// faced"): the relative intent is resolved against the same state `apply`
/// will read, at the moment of commit, so the record and the effect cannot
/// drift apart.
///
/// It trusts the `World`'s invariants rather than defending against them: a
/// wild `facing` yields a payload `replay::payload_valid` refuses, which is a
/// loud failure in a world that was already outside every contract
/// `world.hpp` states \u2014 not something a game key should silently repair.
///
///
/// THE PUMP, REAL-TIME
///
/// The shell calls `tick()` at `replay::kTickHz` (10 Hz, \u00a75.2's real-time
/// row). At most ONE committed action lands per tick. Between ticks,
/// `handle_key` writes a pending slot, LATEST-WINS: a queued action has not
/// reached the world, so replacing it loses nothing, and the action the
/// player sees land is the last thing they asked for inside a 100 ms window
/// \u2014 shorter than a deliberate correction. First-wins would instead perform
/// an action the player had already mentally cancelled.
///
/// A creep step \u2014 a `Step` committed while `World::creeping` \u2014 occupies
/// `Tuning::creep_tick_cost` ticks: the tick it commits on, plus
/// `creep_tick_cost - 1` ticks on which `handle_key` is REFUSED. Refused
/// means dropped, not banked: a queue through the lockout would fire on the
/// exact release tick, which is a macro, not a decision, and \u00a76.2's "creeping
/// costs ticks" is supposed to be felt. The clock runs on regardless \u2014
/// `tick()` advances the world every call, so a patrolling monster covers
/// ground while you shuffle. That is the entire point of the mode.
///
/// THE PUMP, STEP-TIMED
///
/// `handle_key` commits immediately and advances the world by the action's
/// cost \u2014 `creep_tick_cost` for a creep step, 1 for everything else \u2014 and
/// `tick()` is a documented no-op: no action, no ticks (\u00a75.2's step-timed
/// row, "monsters consume N ticks per action"). The shell may keep calling
/// `tick()` from its 10 Hz `on_tick` in both modes and stay branch-free.
///
/// Cost is charged from the event and the state AT COMMIT, before `apply`:
/// a creep step into a wall still costs two ticks \u2014 `apply` refuses the move
/// and the noise ("you did not take a step"), but the care was spent. The two
/// rules are orthogonal and this module owns only the second. A
/// `creep_tick_cost` below 1 clamps to 1 at the point of use, never in
/// `Tuning` \u2014 `tuning.hpp`'s rule, followed here for the same reason.
///
///
/// THE RECORDING, AND WHY `finish()` APPENDS A WAIT
///
/// Every committed action is appended as `replay::Record{tick, event,
/// payload}` against `World::tick` AT COMMIT \u2014 the same tick `play()` will
/// apply it on ("advance until `record.tick`, then apply"), so a live run and
/// a replay of its log interleave `apply`/`advance` identically. Record ticks
/// strictly increase by construction: real-time commits and advances inside
/// one `tick()` call, step-timed commits and advances at least once before
/// the next key can land.
///
/// `finish()` seals the log by appending one final `Wait` at the current
/// tick and advancing once. The seal exists because `play()` settles EVERY
/// replay exactly one advance past its last record. Without it, a session
/// ending on a creep step (whose cost is 2) or on idle real-time ticks would
/// leave the live world past any tick its own log can reach, and the file the
/// shell writes would fail its own verify. The Wait is an honest record \u2014
/// "the player waited out the rest of the session" \u2014 and it makes the live
/// end state and the replayed end state the same state BY CONSTRUCTION rather
/// than by the caller remembering to stop on a cost-1 boundary. It also saves
/// an untouched session: an empty log is `replay::ZeroRecords`, refused at
/// load by design, so a session with no inputs finishes as exactly one Wait.
///
/// `finish()` also seals the core: afterwards `handle_key` is refused and
/// `tick()` is a no-op, and `finish()` returns the same `Outcome` again. One
/// session, one final hash \u2014 a shell that could keep playing after writing
/// the replay would be holding inputs no written file contains.
///
///
/// THE SINK, AND TUNING
///
/// The `audio::Sink*` is stored and FORWARDED to every `advance`, and that is
/// all: nothing in this module branches on it. `world.hpp`'s five facts make
/// muted and voiced runs byte-identical by construction; the discipline here
/// is to add no sixth way to break it. The pointer is forwarded-declared
/// rather than `gloam/audio.hpp` included, for `world.hpp`'s own reason \u2014 a
/// pointer parameter needs no definition, and ticking a world must not drag a
/// lock-free ring into every translation unit. `game.cpp` proves the
/// declaration sufficient: it never includes the definition either.
///
/// `Tuning` is stored BY VALUE. \u00a712 rejects a replay recorded against
/// different tuning, so a session's ruleset is fixed at construction; a
/// swapped-in tuning mid-session would write records across two rulesets
/// while `finish()` could name only one. The struct is 49 integers \u2014 the copy
/// is nothing next to the lifetime bug a reference member would invite.
///
///
/// THE WHOLE SURFACE THE SHELL NEEDS
///
/// construct from `scene::m0_world()`, `handle_key` per key event, `tick()`
/// per `on_tick`, `set_mode` on the pump-toggle key, `world()` to render,
/// `finish()` on quit. (`mode()` and `log()` are read accessors; `map_key` is
/// exposed for tests and for a shell that wants to show what a key WOULD do.)

#include <cstdint>
#include <optional>
#include <vector>

#include "gloam/replay.hpp"
#include "gloam/sha256.hpp"
#include "gloam/tuning.hpp"
#include "gloam/world.hpp"

namespace gloam::audio {
// Forward-declared, not included \u2014 see the header comment. `world.hpp` does
// the same and says why.
class Sink;
}  // namespace gloam::audio

namespace gloam::game {

/// What the player asked for. Device-free by construction: the shell
/// translates its terminal events into these, and `Unbound` exists so it can
/// pass EVERY key through one function and let the core say no \u2014 a key the
/// game does not use is not an error, it is the absence of one.
enum class Key : std::uint8_t {
  Unbound = 0,
  Forward = 1,
  Back = 2,
  TurnLeft = 3,
  TurnRight = 4,
  LampUp = 5,
  LampDown = 6,
  CreepToggle = 7,
  Wait = 8,
};

/// \u00a75.2's two play modes. "Toggleable mid-session by a playtester" is the
/// sentence that makes this a field and not a template parameter: the mode
/// decides WHO drives the clock and nothing else, so `set_mode` is a cheap,
/// safe operation \u2014 the pump is driver policy, never hashed state, and the
/// record log is keyed on `World::tick`, which only ever advances.
enum class PumpMode : std::uint8_t { RealTime = 0, StepTimed = 1 };

/// One resolved input: the `replay::Event` and payload a commit will apply
/// and record. `replay::Event::None` never appears in one \u2014 an unbindable key
/// maps to `std::nullopt`, not to a record the load gate would refuse.
struct Input {
  replay::Event event{replay::Event::None};
  std::uint16_t payload{0};

  [[nodiscard]] auto operator==(const Input&) const -> bool = default;
};

/// The pure mapping. `Forward` is `facing`'s own dir, `Back` its opposite;
/// the turns resolve to the absolute `Dir` now faced (left is one quarter-turn
/// counter-clockwise, right is clockwise \u2014 `Dir` is numbered clockwise from
/// North). `LampUp`/`LampDown` clamp the CURRENT level into
/// [`kLampLevelMin`, `kLampLevelMax`], so dousing is `Lamp 0` and leaning on
/// a railed key records an idempotent event \u2014 kept, not suppressed, because
/// the record is of what the player asked for and `apply` makes it a no-op.
/// `CreepToggle` records the opposite of the current flag.
[[nodiscard]] constexpr auto map_key(Key key, const World& w) -> std::optional<Input> {
  const auto dir16 = [](Dir d) { return static_cast<std::uint16_t>(d); };
  const auto turn16 = [&dir16](Dir d, std::uint16_t quarter_turns) {
    return static_cast<std::uint16_t>(
        (dir16(d) + quarter_turns) % static_cast<std::uint16_t>(kDirCount));
  };
  switch (key) {
    case Key::Forward: return Input{replay::Event::Step, dir16(w.facing)};
    case Key::Back: return Input{replay::Event::Step, dir16(opposite(w.facing))};
    case Key::TurnLeft: return Input{replay::Event::Turn, turn16(w.facing, 3)};
    case Key::TurnRight: return Input{replay::Event::Turn, turn16(w.facing, 1)};
    case Key::LampUp:
      return Input{replay::Event::Lamp,
                   static_cast<std::uint16_t>(w.lamp_level < kLampLevelMax ? w.lamp_level + 1
                                                                          : kLampLevelMax)};
    case Key::LampDown:
      return Input{replay::Event::Lamp,
                   static_cast<std::uint16_t>(w.lamp_level > kLampLevelMin ? w.lamp_level - 1
                                                                          : kLampLevelMin)};
    case Key::CreepToggle:
      return Input{replay::Event::Creep, static_cast<std::uint16_t>(w.creeping ? 0 : 1)};
    case Key::Wait: return Input{replay::Event::Wait, 0};
    case Key::Unbound: return std::nullopt;
  }
  return std::nullopt;  // a Key this version does not know is Unbound's answer
}

class Core {
 public:
  /// What the shell needs to write the `.gloam` replay file with
  /// `replay::assemble`: the seed, the final world hash, and the `Expect` the
  /// file's own load gate must be passed against. `pack_hash` is
  /// `replay::kNoPackHash` \u2014 no pack is loaded until the compositor exists
  /// (#7), which is exactly what that constant is for.
  struct Outcome {
    std::uint64_t seed{0};
    hash::Digest world_hash{};
    replay::Expect expect{};

    /// Field-wise: `replay::Expect` is a load-gate parameter with no == of its
    /// own, so the defaulted operator never existed here.
    [[nodiscard]] auto operator==(const Outcome& o) const -> bool {
      return seed == o.seed && world_hash == o.world_hash &&
             expect.ruleset_hash == o.expect.ruleset_hash &&
             expect.pack_hash == o.expect.pack_hash;
    }
  };

  /// Takes the world BY MOVE: the scene authors it, the core owns it.
  explicit Core(World world, PumpMode mode, audio::Sink* voices = nullptr,
                const Tuning& tuning = kDefaultTuning)
      : m_world{std::move(world)}, m_mode{mode}, m_voices{voices}, m_tuning{tuning} {}

  /// One key press. Real-time: queues it into the pending slot, LATEST-WINS,
  /// unless a creep step's lockout is running \u2014 then it is dropped. Either
  /// way `true` means "accepted by the pump", not "committed"; commitment is
  /// `tick()`'s. Step-timed: commits immediately and advances the world by
  /// the action's cost before returning. `Unbound` and keys after `finish()`
  /// return false and change nothing.
  auto handle_key(Key key) -> bool;

  /// Switch the pump mid-session (\u00a75.2's "toggleable mid-session by a
  /// playtester", and half of what gloam#8's gate exists to answer). Policy,
  /// not simulation: nothing hashed changes, and the log stays valid because
  /// record ticks key on `World::tick`, which only advances. An uncommitted
  /// pending action is DISCARDED at the boundary \u2014 it never reached the
  /// world, so it must not reach the file \u2014 and a running creep lockout is
  /// kept: it only ever counts down inside real-time `tick()`, so in
  /// step-timed it is inert, and toggling back resumes it honestly. Switching
  /// TO step-timed therefore forgives the unpaid remainder of a creep step's
  /// cost; accepted and documented, because the toggle is a playtest
  /// instrument, not an exploit path \u2014 the same carelessness as pausing.
  auto set_mode(PumpMode mode) -> void {
    if (m_finished || mode == m_mode) return;
    m_pending.reset();
    m_mode = mode;
  }

  /// One 10 Hz beat. Real-time: commits the pending action if the lockout
  /// allows, then advances the world exactly once \u2014 the monsters walk whether
  /// or not you acted. Step-timed: does nothing, by design ("no action, no
  /// ticks"), so the shell can call it unconditionally in both modes.
  auto tick() -> void;

  /// Seals the session: appends the final `Wait`, advances once, and returns
  /// the `Outcome` a replay file is written from. Idempotent \u2014 see the header
  /// comment for why the Wait exists and why the core seals.
  auto finish() -> Outcome;

  [[nodiscard]] auto world() const -> const World& { return m_world; }
  [[nodiscard]] auto mode() const -> PumpMode { return m_mode; }
  [[nodiscard]] auto log() const -> const std::vector<replay::Record>& { return m_log; }
  [[nodiscard]] auto finished() const -> bool { return m_finished; }

 private:
  /// \u00a76.2's creep cost, as this driver's rule: a `Step` while creeping costs
  /// `creep_tick_cost`, everything else costs 1. Read from state at commit
  /// time \u2014 `apply` of a Step never writes `creeping`, so before and after
  /// agree \u2014 and clamped to at least 1 at the point of use, per
  /// `tuning.hpp`'s rule that data does not correct itself.
  [[nodiscard]] auto cost_of(replay::Event event) const -> std::int32_t {
    if (event == replay::Event::Step && m_world.creeping) {
      return m_tuning.creep_tick_cost > 1 ? m_tuning.creep_tick_cost : 1;
    }
    return 1;
  }

  /// Apply, record against the pre-advance tick, report the cost. The record
  /// tick is the tick `apply` lands on, which is the tick `play()` will apply
  /// it on \u2014 the header comment has the interleaving argument.
  auto commit(Input input) -> std::int32_t {
    const auto cost = cost_of(input.event);
    apply(m_world, input.event, input.payload, m_tuning);
    m_log.push_back({m_world.tick, input.event, input.payload});
    return cost;
  }

  World m_world;
  PumpMode m_mode;
  audio::Sink* m_voices;  // forwarded to `advance`, never read, never gated on
  Tuning m_tuning;
  std::vector<replay::Record> m_log{};
  std::optional<Input> m_pending{};  // real-time only; latest-wins
  std::int32_t m_lockout{0};         // ticks still occupied by a creep step
  bool m_finished{false};
  Outcome m_outcome{};
};

}  // namespace gloam::game
