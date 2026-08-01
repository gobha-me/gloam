#include "gloam/world.hpp"

#include <type_traits>

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
      const auto destination = w.party.step(dir);
      if (!w.level.edge(w.party, dir).passable()) return;
      if (!w.level.navigable(destination)) return;
      w.party = destination;
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

  for (auto& m : w.monsters) {
    Senses senses{};
    senses.heard =
        hears(field, w.level, m.at, m.kind.acuity, m.mind.state == Awareness::Hunting, tuning);
    senses.los_clear = line_of_sight(w.level, m.at, w.party);
    senses.range = range_between(m.at, w.party);
    senses.lamp_level = w.lamp_level;
    senses.party_position = w.party;

    // The tell used to be discarded here. §6.1 calls it "the deliverable, not
    // the state machine", and this is the line that finally makes that true.
    const Tell tell = step(m.mind, senses, m.kind, tuning);

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
