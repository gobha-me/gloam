#pragma once

/// SPEC §9 — the audio sink, and the wall between it and the simulation.
///
/// §9 opens with the reason this subsystem is not decoration: "If the player can
/// be heard, the player must be able to hear." Monster awareness is read from
/// behaviour and sound, never from a UI element (§2, pillar 4), so a silent
/// build of GLOAM is a materially different and worse game rather than the same
/// game with a feature switched off.
///
///
/// WHAT IS HERE, AND WHAT IS DELIBERATELY NOT
///
/// Everything in this header is standard library and integer arithmetic: the
/// command type, the SPSC ring, and the derivation of gain and pan. RtAudio is
/// NOT here and never will be — §9.3 says "a null sink satisfies it, so core
/// code compiles and tests with no audio at all, and RtAudio stays behind one
/// translation unit", and AGENTS.md rule 1 puts anything that reaches a device
/// in `src/bin/`. The same split `kitty.hpp` documents: PRODUCING a command is
/// not needing a device; PLAYING it is.
///
/// That split is what lets the eight sanitizer jobs audit the lock-free ring
/// from the first commit. A ring living behind a device nobody in CI has is a
/// ring nothing ever checks.
///
///
/// THE ONE CLEVER PART (§9.3)
///
/// Gain and pan are computed by the SAME noise propagation monsters hear
/// through, evaluated from the party's position instead of the monster's. What
/// the player hears and what the monster hears are one system read from two
/// positions — so tuning `atten_closed_door` retunes the stealth model and the
/// mix in one edit, which is the only way the two stay honest with each other.
/// `noise.hpp` states the other half of this rule: do not add a second
/// propagation path for audio. There is none here; `mix_for` calls
/// `propagate_noise`.
///
///
/// WHY NO `Tuning` FIELD IS ADDED, INCLUDING THE ONE THAT LOOKS LIKE IT BELONGS
///
/// `tuning.hpp` holds "every tunable integer in §6 and §8", and `ruleset_hash`
/// covers all of them — a mismatch is a HARD REJECTION at load (§12). An
/// audio-only constant in there would refuse valid recorded sessions over a
/// change the simulation cannot observe.
///
/// `kStingEmission` below is in §6.2's units and still does not go there,
/// because NOTHING IN THE SIMULATION HEARS IT: monsters do not listen to each
/// other. It cannot move a monster, so it cannot reach `world_hash`, so it must
/// not reach `ruleset_hash` either. The consequence is worth stating plainly:
/// step 9 invalidates zero recorded replays, and `kWorldHashVersion` does not
/// move.
///
/// The attenuation the mix actually reads is already in `Tuning` and arrives
/// through `propagate_noise`. Putting a second audio knob beside it would create
/// a second place audio is tuned and undo the property §9.3 exists to create.
///
///
/// NOT INCLUDED: `budgets.hpp`
///
/// `emit.hpp` states the direction rule and it applies here unchanged: the sink
/// reports, the budget judges, and exactly one file can relax a budget. So
/// `kVoiceRingCapacity` is not compared against `budget::kMaxDroppedVoiceCommands`
/// in this header. Tests write that comparison; see `test/10budgets/`.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "gloam/level.hpp"
#include "gloam/noise.hpp"
#include "gloam/tuning.hpp"

namespace gloam::audio {

// ── What can be heard ───────────────────────────────────────────────────────

/// The sounds the simulation can currently emit.
///
/// APPEND ONLY, never renumbered — the same discipline `replay::Event` follows,
/// and for a weaker but real version of the same reason: a `SoundId` recorded in
/// a bug report or a mix log should not change meaning under someone's feet.
///
/// TWO ENTRIES, AND STOPPING THERE IS THE POINT. These are the only two things
/// `world.cpp` can produce today. `apply()` writes `pending_noise` from
/// `Event::Step` and from nothing else, and `step()` returns a `Tell` that
/// `advance()` is the only caller of. §6.2's other emitter rows — door, melee,
/// fall, cast — have no emitter in the simulation: there is no door event in
/// `replay::Event`, combat is M2, and §8's casting waits on the M0 gate. Adding
/// their ids now would put values in every `switch` that no test can drive.
/// They arrive with their emitters.
///
/// Note what is ALSO absent: a per-armour footfall. Armour audibility is already
/// carried by `step_noise` -> `propagate_noise` -> gain, and a second sample per
/// armour would be a second place armour weight is expressed. §9.3's whole
/// argument is that there is exactly one.
enum class SoundId : std::uint8_t {
  None = 0,           ///< a default-constructed Command is inert
  PartyFootfall = 1,  ///< §6.2's step emitter, read at the party's own cell
  HuntingSting = 2,   ///< §6.1 SEARCHING -> HUNTING, and ONLY that transition
};

/// How many ids exist. A mixer sizes its arena by this.
inline constexpr std::size_t kSoundIdCount = 3;

// ── Gain and pan ────────────────────────────────────────────────────────────

/// Loudness at the listener, `kGainSilent` .. `kGainUnity`.
using Gain = std::int16_t;

/// Lateral position, `-kPanFull` (hard left) .. `+kPanFull` (hard right).
using Pan = std::int16_t;

inline constexpr Gain kGainSilent = 0;

/// Unity gain. A POWER OF TWO, and that is the whole reason for the value.
///
/// The device converts with `gain * (1.0f / 1024.0f)`, which is exact in binary
/// floating point — the same integer yields bit-identical float on every
/// IEEE-754 target. A denominator of 1000 would introduce a rounding step that
/// buys nothing and is impossible to reason about afterwards.
inline constexpr Gain kGainUnity = 1024;

inline constexpr Pan kPanCentre = 0;

/// Hard left / hard right. A power of two for the same reason as `kGainUnity`.
///
/// 256 lateral steps vastly exceeds the grid's angular resolution — a 4-cell
/// sight radius offers a few dozen distinguishable bearings — so the
/// quantisation loses nothing and leaves headroom if depth ever refines it.
inline constexpr Pan kPanFull = 256;

/// The HUNTING sting's emission, in §6.2's units.
///
/// NOT a `Tuning` field. See this header's preamble: nothing in the simulation
/// hears the sting, so it must not reach `ruleset_hash`.
///
/// Sits at melee-hit loudness (`noise_melee_hit`, 90) so a first sighting
/// carries across an open corridor and dies through a closed door. §6.1 already
/// requires clear line of sight and range within sight distance before this can
/// fire, so an audible sting is guaranteed wherever it is legal to emit one.
inline constexpr std::int32_t kStingEmission = 90;

/// What a listener hears: how loud, and from which side.
struct Mix {
  Gain gain{kGainSilent};
  Pan pan{kPanCentre};

  [[nodiscard]] auto operator==(const Mix&) const -> bool = default;
};

/// Attenuated loudness -> gain, linear in the residual.
///
/// LINEAR, NOT LOGARITHMIC, and the reason is §9.3 rather than taste: §6.2's
/// attenuation is subtractive-linear, so a dB curve here would mean the player's
/// sense of distance and the monster's threshold test disagree about what "far"
/// means. That is exactly the decoupling §9.3 exists to prevent.
///
/// Truncating division, and the direction is deliberate: truncation can only
/// make a voice quieter than the model says, never louder. Erring quiet errs
/// toward "you are heard more easily than you hear", which is the honest bias
/// for a stealth game. Erring loud would hand the player information the monster
/// does not have.
///
/// Total on hostile input: a non-positive emission, a non-positive arrival, and
/// an arrival exceeding its own emission all have defined answers. `NoiseField`
/// reads 0 both for "never reached" and for "out of bounds", so silence is a
/// value here and never an error.
[[nodiscard]] constexpr auto gain_from_loudness(std::int32_t heard, std::int32_t emission)
    -> Gain {
  if (emission <= 0 || heard <= 0) return kGainSilent;
  if (heard >= emission) return kGainUnity;

  // `heard < emission` and both are positive here, so `kGainUnity * heard`
  // cannot exceed `kGainUnity * INT32_MAX` — which it could, in principle, from
  // a hand-built emission. Widen for the multiply rather than trusting the
  // caller: this function is `constexpr` and a signed overflow would be a
  // hard error in a constant expression and UB everywhere else.
  const auto scaled = static_cast<std::int64_t>(kGainUnity) * heard / emission;
  return static_cast<Gain>(scaled);
}

/// Bearing -> pan, as the lateral component normalised against total offset.
///
/// The source is rotated into the listener's view space first, so pan is
/// relative to where the party is FACING and not to the level's north. `y` grows
/// SOUTH — `Coord::step` in `level.hpp` is the definition — and getting that
/// backwards is the single likeliest bug in this file, which is why
/// `test/14audio/` tabulates all four facings against all four bearings.
///
/// L1 normalisation (`|right| + |forward|`) rather than a true angle: it needs
/// no trigonometry, no table and no float, it is exact in integers, and it has
/// the two endpoints that matter — a source abeam pans hard, a source dead ahead
/// centres.
///
/// A SOURCE DEAD ASTERN ALSO CENTRES, and that is a real front/back ambiguity
/// rather than an oversight. Two channels cannot resolve it; resolving it needs
/// a second cue (a duller sting behind you), which is a mix decision §9 does not
/// make. It is asserted as-is in the suite so the limitation is on the record
/// and cannot be "fixed" by accident.
[[nodiscard]] constexpr auto pan_from_bearing(Dir facing, Coord listener, Coord source) -> Pan {
  // Clamped before the arithmetic, not after. `Coord` is signed and its bounds
  // are not enforced by `Level`, so a coordinate read off a disk could otherwise
  // overflow `kPanFull * right`. The clamp costs nothing and makes the UBSan
  // job's silence mean something.
  constexpr std::int32_t kLimit = 32'767;
  const auto clamp = [](std::int32_t v) -> std::int32_t {
    if (v > kLimit) return kLimit;
    if (v < -kLimit) return -kLimit;
    return v;
  };

  const auto dx = clamp(source.x - listener.x);
  const auto dy = clamp(source.y - listener.y);

  std::int32_t forward = 0;
  std::int32_t right = 0;
  switch (facing) {
    case Dir::North: forward = -dy; right = dx; break;
    case Dir::East: forward = dx; right = dy; break;
    case Dir::South: forward = dy; right = -dx; break;
    case Dir::West: forward = -dx; right = -dy; break;
  }

  const auto magnitude = [](std::int32_t v) -> std::int32_t { return v < 0 ? -v : v; };
  const auto spread = magnitude(right) + magnitude(forward);
  if (spread == 0) return kPanCentre;  // the source IS the listener

  return static_cast<Pan>(std::int32_t{kPanFull} * right / spread);
}

/// Read an ALREADY-PROPAGATED field at the listener.
///
/// This is the overload `advance` uses for the party's own footfall: the field
/// the monsters are about to be tested against is the field the player hears
/// through, so one propagation serves both readings. §9.3's "one system read
/// from two positions", literally.
[[nodiscard]] auto mix_at(const NoiseField& field, const Level& level, Coord listener, Dir facing,
                          Coord source, std::int32_t emission) -> Mix;

/// Propagate `emission` from `source` and read it at `listener`.
///
/// Calls the SAME `propagate_noise` monsters hear through. There is no second
/// propagation path here and `noise.hpp` forbids adding one; `test/14audio/`
/// asserts equality against a hand-rolled `propagate_noise` + `NoiseField::at`
/// rather than merely similar behaviour.
///
/// Costs a full propagation per call. When several sources sound in one tick,
/// use `mix_reciprocal` and one shared field instead — see below.
[[nodiscard]] auto mix_for(const Level& level, Coord listener, Dir facing, Coord source,
                           std::int32_t emission, const Tuning& tuning) -> Mix;

/// The same mix, read out of a field propagated FROM THE LISTENER.
///
/// THIS IS WHAT §9.3 ACTUALLY SAYS, AND IT IS ALSO THE FAST ONE. The sentence is
/// "the same noise-propagation function monsters hear through, EVALUATED FROM
/// THE PARTY'S POSITION INSTEAD OF THE MONSTER'S" — one field, rooted at the
/// party, read at each source. `mix_for`'s reading (root at the emitter, read at
/// the party) gives the same answer and costs one propagation per sounding
/// source; this costs one per tick however many monsters sound at once.
///
/// WHY THE TWO AGREE. `propagate_noise` computes
/// `max(0, emission - mincost(root, cell))`, where `mincost` sums per-EDGE
/// attenuations along the quietest path. Edges are stored per cell but written
/// in pairs — `Level::link` sets both sides and `Level::symmetric()` asserts the
/// invariant — so the graph is undirected with symmetric weights and
/// `mincost(a, b) == mincost(b, a)`. Reversing which end is the root therefore
/// cannot change the number.
///
/// THE PRECONDITION IS REAL AND IS TESTED, NOT ASSUMED. On a level whose edges
/// disagree with their twins, sound crosses a wall in one direction only and
/// these two functions diverge. `test/14audio/` asserts `symmetric()` first and
/// the equality second, the same order `test/04perception/` uses for line of
/// sight, because both are theorems about a symmetric graph rather than
/// coincidences.
///
/// ONE SIZING RULE, AND IT IS NOT OPTIONAL. Build `from_listener` with the SAME
/// `emission` passed here. `propagate_noise` drops cells once the arriving
/// loudness reaches zero, so a field built with a smaller emission is truncated
/// early and would silence a source that should have been audible. Built with
/// exactly this emission, the field reaches precisely the cells from which this
/// sound could be heard at all — nothing audible is lost, and nothing inaudible
/// is computed.
[[nodiscard]] auto mix_reciprocal(const NoiseField& from_listener, const Level& level,
                                  Coord listener, Dir facing, Coord source,
                                  std::int32_t emission) -> Mix;

// ── The command ─────────────────────────────────────────────────────────────

/// One voice, as it crosses the wall. §9.2: "one SPSC ring of fixed-size
/// `VoiceCommand`."
///
/// UNLIKE EVERYTHING IN `World`, THIS STRUCT'S PADDING IS HARMLESS, and it is
/// worth saying so next to `world.cpp`'s `Absorb`, which exists because padding
/// inside a hashed struct is a cross-compiler divergence that looks exactly like
/// a determinism bug. A `Command` never reaches a digest. That is what lets a
/// slot write be a plain store with no constructor, no destructor and no branch
/// — which is what makes the consumer side callback-safe.
struct Command {
  /// Opaque to this library, which never reads it. `src/bin/` stamps a monotonic
  /// clock reading here so the device can measure §11's tick-to-first-sample
  /// row. Carrying eight bytes needs no clock; THIS is what keeps the latency
  /// instrument out of `gloam::lib` without giving up on measuring it.
  std::uint64_t stamp{0};
  std::uint32_t tick{0};
  Gain gain{kGainSilent};
  Pan pan{kPanCentre};
  SoundId sound{SoundId::None};

  [[nodiscard]] auto operator==(const Command&) const -> bool = default;
};

static_assert(std::is_trivially_copyable_v<Command>,
              "a slot write must be a plain store; the audio callback cannot run a constructor");
static_assert(std::is_trivially_destructible_v<Command>,
              "a slot overwrite must not run a destructor on the audio thread");

// ── Ring arithmetic ─────────────────────────────────────────────────────────
//
// Free functions rather than private members, following `emit::saturating_add`
// and its stated reason: the wrap boundary is directly testable this way without
// adding a test-only seam to shipped code. You cannot realistically drive a
// 64-bit index to its limit through the public API, and untested wrap arithmetic
// inside a lock-free ring is precisely the code that turns out to be wrong.

/// Saturating add, for the drop and push counters.
///
/// A DELIBERATE DUPLICATE of `emit::saturating_add`, and the duplication is the
/// cheaper mistake. Including `emit.hpp` here would drag it through `world.hpp`
/// into every consumer's translation unit and silently reverse the umbrella
/// exclusion `gloam.hpp` documents by name. Three lines beat quietly undoing a
/// written decision; if a third copy ever appears, that is the moment to give
/// them a shared home rather than now.
[[nodiscard]] constexpr auto saturating_add(std::uint64_t a, std::uint64_t b) -> std::uint64_t {
  if (a > UINT64_MAX - b) return UINT64_MAX;
  return a + b;
}

/// How many commands sit between the consumer's `head` and the producer's
/// `tail`.
///
/// The indices are MONOTONIC and masked only at slot access, never pre-wrapped.
/// That is what lets "full" and "empty" be distinguished without sacrificing a
/// slot, and unsigned overflow is well defined, so this stays correct across the
/// point where `tail` wraps past `UINT64_MAX` and `head` has not.
[[nodiscard]] constexpr auto ring_size(std::uint64_t head, std::uint64_t tail) -> std::uint64_t {
  return tail - head;
}

[[nodiscard]] constexpr auto ring_full(std::uint64_t head, std::uint64_t tail,
                                       std::uint64_t capacity) -> bool {
  return ring_size(head, tail) >= capacity;
}

// ── The ring ────────────────────────────────────────────────────────────────

/// §9.2's default capacity, justified against `budget::kDroppedVoiceCommandWindowTicks`.
///
/// The budget window is 1000 ticks at 10 Hz, i.e. 100 s. §9.2's callback runs
/// every 256/48000 s = 5.33 ms, so the consumer drains ROUGHLY EIGHTEEN TIMES
/// PER TICK and steady-state occupancy sits far below one tick's production. The
/// ring therefore only has to absorb one tick's worst burst — a footfall plus a
/// sting per monster that transitioned — plus a scheduling hiccup.
///
/// `test/10budgets/` already exercises 16 monsters. 64 is four times that,
/// absorbs a full tick's production even if the audio thread misses three
/// consecutive callbacks, and costs 64 slots of a small POD, resident, with zero
/// allocation after construction. It is a power of two because the slot index is
/// a mask rather than a division the callback cannot afford.
inline constexpr std::size_t kVoiceRingCapacity = 64;

/// Padding to keep the producer's and the consumer's hot indices off one
/// another's cache line.
///
/// A named constant rather than `std::hardware_destructive_interference_size`,
/// which is ABI-gated on libstdc++ and warns when used. The number is a
/// portability guess either way; this way it is a guess with a name.
inline constexpr std::size_t kCacheLineBytes = 64;

/// The single-producer, single-consumer ring. §9.2: "the ring is the whole
/// interface."
///
/// `try_push` is the SIMULATION THREAD's, `try_pop` is the AUDIO THREAD's, and
/// calling either from the other side is undefined. That restriction is what
/// buys the lock-free property; it is not a lock in disguise.
///
/// A FULL RING DROPS THE NEWEST COMMAND AND COUNTS IT. It never blocks — §9.2:
/// "a blocked sim is a stalled tick, which is worse than a missing footstep" —
/// and it never overwrites the oldest, because an overwriting ring can rewrite a
/// slot the consumer is part-way through reading. That turns a dropped footstep
/// into a corrupt one, which is a worse failure and a much harder one to see.
template <std::size_t Capacity = kVoiceRingCapacity>
class Ring {
  static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                "power of two: the slot index is a mask, because the audio callback cannot "
                "afford a division");
  static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                "§9.2 forbids a lock in the callback, and a non-lock-free atomic IS one");

 public:
  Ring() = default;
  Ring(const Ring&) = delete;
  auto operator=(const Ring&) -> Ring& = delete;

  /// Producer side. Returns whether the command was accepted.
  ///
  /// The simulation never looks at the answer — `Sink::play` returns void, which
  /// is what makes §9.2's "audio -> sim is nothing" a type rather than a
  /// convention. The bool is for the device and for tests.
  auto try_push(const Command& c) noexcept -> bool {
    // Relaxed: this thread is the only writer of `m_tail`, and a thread always
    // observes its own writes.
    const auto tail = m_tail.load(std::memory_order_relaxed);
    // Acquire: pairs with the consumer's release store of `m_head`, so a slot
    // the consumer has finished with is visibly free.
    const auto head = m_head.load(std::memory_order_acquire);

    if (ring_full(head, tail, Capacity)) {
      // Relaxed: an instrument, not a synchroniser. It orders nothing, and must
      // not pay for a fence on a path the simulation runs every tick.
      m_dropped.store(saturating_add(m_dropped.load(std::memory_order_relaxed), 1),
                      std::memory_order_relaxed);
      return false;
    }

    m_slots[static_cast<std::size_t>(tail & (Capacity - 1))] = c;

    // Release: publishes the slot write above. THIS IS THE ONLY EDGE that makes
    // the payload visible to the consumer — weaken it and the ring still passes
    // every single-threaded test in the suite.
    m_tail.store(tail + 1, std::memory_order_release);
    m_pushed.store(saturating_add(m_pushed.load(std::memory_order_relaxed), 1),
                   std::memory_order_relaxed);
    return true;
  }

  /// Consumer side. Returns false and leaves `out` untouched when empty.
  auto try_pop(Command& out) noexcept -> bool {
    const auto head = m_head.load(std::memory_order_relaxed);  // sole writer
    const auto tail = m_tail.load(std::memory_order_acquire);  // pairs with try_push's release
    if (ring_size(head, tail) == 0) return false;

    out = m_slots[static_cast<std::size_t>(head & (Capacity - 1))];
    m_head.store(head + 1, std::memory_order_release);  // frees the slot
    return true;
  }

  /// Commands refused because the ring was full. §11's row; `budget::kMaxDroppedVoiceCommands`
  /// is zero, so this is a budget assertion and not a log line.
  ///
  /// Saturating rather than wrapping, for `emit::ByteSink::total`'s reason: a
  /// wrapped counter reads as a PASSING budget, which is the one failure mode a
  /// budget instrument may not have.
  [[nodiscard]] auto dropped() const noexcept -> std::uint64_t {
    return m_dropped.load(std::memory_order_relaxed);
  }

  /// Commands accepted. Exists so a test can tell "identical because audio is
  /// correct" from "identical because audio did nothing" — which is how this
  /// subsystem's central gate would otherwise rot into a vacuous pass.
  [[nodiscard]] auto pushed() const noexcept -> std::uint64_t {
    return m_pushed.load(std::memory_order_relaxed);
  }

  [[nodiscard]] auto size() const noexcept -> std::uint64_t {
    return ring_size(m_head.load(std::memory_order_acquire),
                     m_tail.load(std::memory_order_acquire));
  }

  [[nodiscard]] auto empty() const noexcept -> bool { return size() == 0; }

  [[nodiscard]] static constexpr auto capacity() noexcept -> std::size_t { return Capacity; }

  /// Start a measurement window. Never called implicitly, for `ByteSink::reset_totals`'s
  /// reason: a window that resets itself always passes.
  auto reset_counters() noexcept -> void {
    m_dropped.store(0, std::memory_order_relaxed);
    m_pushed.store(0, std::memory_order_relaxed);
  }

 private:
  std::array<Command, Capacity> m_slots{};

  // Producer-owned and consumer-owned indices, on separate cache lines. Without
  // the padding the two threads contend for one line on every push and pop, and
  // the ring is lock-free but not remotely fast.
  alignas(kCacheLineBytes) std::atomic<std::uint64_t> m_tail{0};
  alignas(kCacheLineBytes) std::atomic<std::uint64_t> m_head{0};
  alignas(kCacheLineBytes) std::atomic<std::uint64_t> m_dropped{0};
  std::atomic<std::uint64_t> m_pushed{0};
};

// ── The sink ────────────────────────────────────────────────────────────────

/// §9.3's interface: "the game only ever calls `Sink::play(SoundId, Gain, Pan)`."
class Sink {
 public:
  Sink() = default;
  Sink(const Sink&) = delete;
  auto operator=(const Sink&) -> Sink& = delete;
  virtual ~Sink() = default;

  /// RETURNS VOID, AND THAT IS THE ENTIRE DETERMINISM ARGUMENT.
  ///
  /// If this handed back "did it fit", the ring's drop counter would be an
  /// audio -> sim channel and §9.2's "Audio -> sim is nothing. Ever." would be a
  /// convention that a future refactor could break without any test noticing. A
  /// dropped voice MUST be invisible to the simulation, and the way to guarantee
  /// that is to leave the caller nothing to read. Every parameter is a scalar by
  /// value for the same reason.
  virtual auto play(SoundId sound, Gain gain, Pan pan) -> void = 0;

  /// Set by `advance` once per tick, before it emits anything.
  ///
  /// Non-virtual and one-way: it lets a sink stamp its own commands with the
  /// tick being simulated without widening the virtual above, and the simulation
  /// never reads it back.
  auto note_tick(std::uint32_t t) noexcept -> void { m_tick = t; }

  [[nodiscard]] auto tick() const noexcept -> std::uint32_t { return m_tick; }

 private:
  std::uint32_t m_tick{0};
};

/// §9.3's null sink: "a null sink satisfies it, so core code compiles and tests
/// with no audio at all."
///
/// IT COUNTS NOTHING, on purpose. A null sink owning a ring nobody drains would
/// report drops on any machine without a sound card, and §11's drop budget would
/// then be measuring the absence of a device rather than the correctness of the
/// ring.
class NullSink final : public Sink {
 public:
  auto play(SoundId, Gain, Pan) -> void override {}
};

/// A sink that keeps what it was given, for tests and for the device-free
/// harness in `src/bin/replay.cpp`.
template <std::size_t Capacity = kVoiceRingCapacity>
class RecordingSink final : public Sink {
 public:
  auto play(SoundId sound, Gain gain, Pan pan) -> void override {
    Command c{};
    c.tick = tick();
    c.sound = sound;
    c.gain = gain;
    c.pan = pan;
    // The bool is deliberately discarded: a full ring is not the simulation's
    // business. `ring().dropped()` is where it is answered.
    static_cast<void>(m_ring.try_push(c));
  }

  [[nodiscard]] auto ring() noexcept -> Ring<Capacity>& { return m_ring; }
  [[nodiscard]] auto ring() const noexcept -> const Ring<Capacity>& { return m_ring; }

 private:
  Ring<Capacity> m_ring{};
};

}  // namespace gloam::audio
