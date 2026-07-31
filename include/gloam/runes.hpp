#pragma once

/// SPEC §8.1-§8.2 — the rune vocabulary and its grammar.
///
/// Spells are composed, not selected. The player finds rune inscriptions in the
/// dungeon and works out what combines with what. There is no spell list, no
/// known-spells screen and no autocomplete — §17 lists all three as non-goals,
/// so nothing in this header may grow a "list all valid spells" entry point
/// that a UI could later call.
///
/// The grammar is the hint (§8.1): the syllables encode their own slot, and
/// nothing tells the player so. Power runes begin with a stop, elements with a
/// fricative, forms with a liquid or nasal, modifiers with a vowel. A player
/// who notices that has inferred the grammar from four wall carvings.

#include <cstdint>
#include <string_view>

namespace gloam {

/// A spell is an ordered sequence of up to four runes, one per slot.
enum class Slot : std::uint8_t { Power = 0, Element = 1, Form = 2, Modifier = 3 };

/// Slot 1 — Power. Stops. REQUIRED. Ordered by magnitude.
enum class Power : std::uint8_t {
  Kai = 0,   ///< a flicker
  Tor = 1,   ///< slight
  Pel = 2,   ///< ordinary
  Dun = 3,   ///< strong
  Bram = 4,  ///< severe
  Goth = 5,  ///< ruinous
};

/// Slot 2 — Element. Fricatives. REQUIRED.
enum class Element : std::uint8_t {
  Sil = 0,    ///< light — also refuels a lamp
  Shor = 1,   ///< cold — slows, and quiets
  Fen = 2,    ///< rot — over time, not at once
  Vast = 3,   ///< stone — walls, doors, weight
  Thule = 4,  ///< void — silence; a noise sink
  Hesh = 5,   ///< flame — loud, bright, obvious
};

/// Slot 3 — Form. Liquids and nasals. OPTIONAL.
enum class Form : std::uint8_t {
  Lom = 0,   ///< bolt — travels the corridor
  Rend = 1,  ///< touch — adjacent cell only
  Wyrd = 2,  ///< ward — persistent barrier on a cell edge
  Yarn = 3,  ///< field — fills a cell and its neighbours
  Mote = 4,  ///< mote — a placed object that persists
  Nil = 5,   ///< self — the caster
  None = 6,  ///< the slot is left empty
};

/// Slot 4 — Modifier. Vowels. OPTIONAL.
enum class Modifier : std::uint8_t {
  Arr = 0,    ///< extend — duration x3, cost x2
  Esk = 1,    ///< quieten — cast noise halved
  Ith = 2,    ///< split — two targets, half magnitude
  Oth = 3,    ///< delay — fires 10 ticks later
  Umbra = 4,  ///< invert — the effect runs the other way
  Urn = 5,    ///< bind — attaches to an object, not a place
  None = 6,   ///< the slot is left empty
};

inline constexpr int kPowerCount = 6;
inline constexpr int kElementCount = 6;
/// Six forms plus the empty slot.
inline constexpr int kFormCount = 7;
/// Six modifiers plus the empty slot.
inline constexpr int kModifierCount = 7;

/// §8.1 — "Combination space is 6 x 6 x 7 x 7 = 1,764." Valid spells number
/// 60-80; discovery is the content.
inline constexpr int kCombinationSpace = kPowerCount * kElementCount * kFormCount * kModifierCount;
static_assert(kCombinationSpace == 1764, "the combination space is frozen at 1,764");

/// An ordered rune sequence. Power and Element are required, which is why they
/// have no `None`; Form and Modifier default to empty.
struct RuneSeq {
  Power power{Power::Kai};
  Element element{Element::Sil};
  Form form{Form::None};
  Modifier modifier{Modifier::None};

  [[nodiscard]] constexpr auto operator==(const RuneSeq&) const -> bool = default;

  /// A dense index in [0, 1764). The enumeration order is the wire order of
  /// `spells.data` rows, so it must not change.
  [[nodiscard]] constexpr auto index() const -> int {
    return ((static_cast<int>(power) * kElementCount + static_cast<int>(element)) * kFormCount +
            static_cast<int>(form)) *
               kModifierCount +
           static_cast<int>(modifier);
  }

  /// The inverse of `index()`.
  [[nodiscard]] static constexpr auto from_index(int i) -> RuneSeq {
    RuneSeq s{};
    s.modifier = static_cast<Modifier>(i % kModifierCount);
    i /= kModifierCount;
    s.form = static_cast<Form>(i % kFormCount);
    i /= kFormCount;
    s.element = static_cast<Element>(i % kElementCount);
    i /= kElementCount;
    s.power = static_cast<Power>(i % kPowerCount);
    return s;
  }
};

// ── Inscriptions ────────────────────────────────────────────────────────────
//
// The written form of each rune. §8.2: "do not let the generator print the
// class of a rune on its inscription" — these strings are the ONLY thing a
// player ever sees, and none of them name their slot.

[[nodiscard]] constexpr auto name(Power p) -> std::string_view {
  switch (p) {
    case Power::Kai: return "KAI";
    case Power::Tor: return "TOR";
    case Power::Pel: return "PEL";
    case Power::Dun: return "DUN";
    case Power::Bram: return "BRAM";
    case Power::Goth: return "GOTH";
  }
  return {};
}

[[nodiscard]] constexpr auto name(Element e) -> std::string_view {
  switch (e) {
    case Element::Sil: return "SIL";
    case Element::Shor: return "SHOR";
    case Element::Fen: return "FEN";
    case Element::Vast: return "VAST";
    case Element::Thule: return "THULE";
    case Element::Hesh: return "HESH";
  }
  return {};
}

[[nodiscard]] constexpr auto name(Form f) -> std::string_view {
  switch (f) {
    case Form::Lom: return "LOM";
    case Form::Rend: return "REND";
    case Form::Wyrd: return "WYRD";
    case Form::Yarn: return "YARN";
    case Form::Mote: return "MOTE";
    case Form::Nil: return "NIL";
    case Form::None: return {};
  }
  return {};
}

[[nodiscard]] constexpr auto name(Modifier m) -> std::string_view {
  switch (m) {
    case Modifier::Arr: return "ARR";
    case Modifier::Esk: return "ESK";
    case Modifier::Ith: return "ITH";
    case Modifier::Oth: return "OTH";
    case Modifier::Umbra: return "UMBRA";
    case Modifier::Urn: return "URN";
    case Modifier::None: return {};
  }
  return {};
}

// ── The grammar, as a decodable rule ────────────────────────────────────────

/// What §8.1's phonetic rule alone can tell you about an inscription.
///
/// `Ambiguous` is not a defect in this function — it would be a property of the
/// authored vocabulary. No shipped inscription produces it today (see
/// `slot_from_inscription`), and it is kept so that a future rune which
/// reintroduced a collision has somewhere honest to land rather than being
/// silently filed under the wrong slot.
enum class SlotHint : std::uint8_t { Power, Element, Form, Modifier, Ambiguous, Unknown };

/// Infers a rune's slot from its opening sound, exactly as a player would.
///
/// This is the inference §8.1 wants the player to make, written down so it can
/// be TESTED — a level that teaches the grammar is only teaching it if the
/// grammar actually holds over the shipped vocabulary.
///
/// It holds over the whole shipped vocabulary: all 24 runes report their own
/// slot, with no exceptions. That is a stronger claim than it looks, and
/// `test/05spells/` asserts it exhaustively rather than by sampling — the rule
/// is the ONLY teaching mechanism in the game (§17 rules out a spell list, a
/// known-spells screen and autocomplete), so a player who infers it and gets a
/// wrong answer has been actively mistaught by the one system meant to teach
/// them, with no way to discover they were misled.
///
/// It did not always hold. The vocabulary shipped with a Modifier opening on Y
/// (YRN) beside the Form YARN, and §8.1 lists Y among the liquids — so YRN was
/// simply not obeying its own slot's phonology. Resolved by renaming it to URN
/// (gloam#11) rather than by documenting an exception, because the exception
/// would have cost the rule its only job.
[[nodiscard]] auto slot_from_inscription(std::string_view inscription) -> SlotHint;

}  // namespace gloam
