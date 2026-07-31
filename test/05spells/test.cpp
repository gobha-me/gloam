// SPEC §8 and §13.3 — the rune grammar and the spell resolver.
//
// "All 1,764 rune sequences resolve without throwing, and every row in
// spells.data is reachable from some placeable inscription set."

#include <catch2/catch_all.hpp>

#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "gloam/runes.hpp"
#include "gloam/spells.hpp"

using namespace gloam;

TEST_CASE("the combination space is exactly 1,764 and the index is a bijection", "[runes]") {
  std::set<int> indices;
  for (int p = 0; p < kPowerCount; ++p) {
    for (int e = 0; e < kElementCount; ++e) {
      for (int f = 0; f < kFormCount; ++f) {
        for (int m = 0; m < kModifierCount; ++m) {
          const RuneSeq seq{static_cast<Power>(p), static_cast<Element>(e), static_cast<Form>(f),
                            static_cast<Modifier>(m)};
          const int i = seq.index();
          REQUIRE(i >= 0);
          REQUIRE(i < kCombinationSpace);
          indices.insert(i);
          // from_index is the exact inverse.
          REQUIRE(RuneSeq::from_index(i) == seq);
        }
      }
    }
  }
  CHECK(indices.size() == static_cast<std::size_t>(kCombinationSpace));
  CHECK(kCombinationSpace == 1764);
}

TEST_CASE("all 1,764 sequences resolve without throwing", "[spells][property]") {
  const auto table = m0_seed_table();
  const CasterState caster{/*mana=*/100};
  const WorldState world{};

  int valid = 0;
  for (int i = 0; i < kCombinationSpace; ++i) {
    const auto seq = RuneSeq::from_index(i);
    SpellOutcome out{};
    REQUIRE_NOTHROW(out = resolve(seq, caster, world, table, kDefaultTuning));

    // Total, in the sense that matters: every input yields a usable outcome.
    REQUIRE(out.mana_cost > 0);
    REQUIRE(out.noise >= 0);
    REQUIRE(out.target_count >= 1);
    valid += out.valid ? 1 : 0;
  }
  CHECK(valid == static_cast<int>(table.size()));
}

TEST_CASE("invalid combinations still cost mana", "[spells]") {
  // §8.3: "Invalid combinations fail loudly and still cost mana.
  // Experimentation has a price." This is the rule most likely to be
  // "optimised" away by someone who reads an invalid spell as a no-op.
  const auto table = m0_seed_table();
  const RuneSeq nonsense{Power::Goth, Element::Vast, Form::Wyrd, Modifier::Ith};
  REQUIRE_FALSE(table.contains(nonsense));

  const auto out = resolve(nonsense, CasterState{100}, WorldState{}, table, kDefaultTuning);
  CHECK_FALSE(out.valid);
  CHECK(out.mana_cost == kDefaultTuning.power_mana(static_cast<int>(Power::Goth)));
  CHECK(out.mana_cost == 13);
  CHECK(out.noise == 130);
  CHECK(out.magnitude == 0);
}

TEST_CASE("GOTH.HESH with no form is the canonical dangerous failure", "[spells]") {
  // §8.3's worked example: "all that power, nowhere to go."
  const RuneSeq goth_hesh{Power::Goth, Element::Hesh, Form::None, Modifier::None};
  CHECK(danger_of_invalid(goth_hesh, kDefaultTuning) == DangerClass::Level);

  const auto table = m0_seed_table();
  REQUIRE_FALSE(table.contains(goth_hesh));
  const auto out = resolve(goth_hesh, CasterState{100}, WorldState{}, table, kDefaultTuning);
  CHECK_FALSE(out.valid);
  CHECK(out.danger == DangerClass::Level);
}

TEST_CASE("a form gives the power somewhere to go, so the failure is inert", "[spells]") {
  for (int e = 0; e < kElementCount; ++e) {
    for (int f = 0; f < kFormCount - 1; ++f) {  // every real form, excluding None
      const RuneSeq seq{Power::Goth, static_cast<Element>(e), static_cast<Form>(f),
                        Modifier::None};
      INFO("element " << e << " form " << f);
      CHECK(danger_of_invalid(seq, kDefaultTuning) == DangerClass::Inert);
    }
  }
}

TEST_CASE("only the heavy power runes are dangerous when unformed", "[spells]") {
  const Tuning& t = kDefaultTuning;
  // KAI (1) through DUN (5) fizzle; BRAM (8) and GOTH (13) do not.
  CHECK(danger_of_invalid({Power::Kai, Element::Hesh, Form::None, Modifier::None}, t) ==
        DangerClass::Inert);
  CHECK(danger_of_invalid({Power::Dun, Element::Hesh, Form::None, Modifier::None}, t) ==
        DangerClass::Inert);
  CHECK(danger_of_invalid({Power::Bram, Element::Sil, Form::None, Modifier::None}, t) ==
        DangerClass::Caster);
  CHECK(danger_of_invalid({Power::Bram, Element::Hesh, Form::None, Modifier::None}, t) ==
        DangerClass::Party);
  CHECK(danger_of_invalid({Power::Goth, Element::Sil, Form::None, Modifier::None}, t) ==
        DangerClass::Party);
  CHECK(danger_of_invalid({Power::Goth, Element::Hesh, Form::None, Modifier::None}, t) ==
        DangerClass::Level);
}

TEST_CASE("§8.2's modifier arithmetic", "[spells]") {
  const auto table = m0_seed_table();
  const Tuning& t = kDefaultTuning;
  const CasterState rich{1000};

  // A valid row with no modifier, as the baseline. PEL is 3 mana.
  const RuneSeq base{Power::Pel, Element::Sil, Form::Mote, Modifier::None};
  REQUIRE(table.contains(base));
  const auto plain = resolve(base, rich, WorldState{}, table, t);
  CHECK(plain.valid);
  CHECK(plain.mana_cost == 3);
  CHECK(plain.noise == 30);
  CHECK(plain.duration == 3);
  CHECK(plain.magnitude == 3);
  CHECK(plain.target_count == 1);

  // ARR — extend: duration x3, cost x2. Checked on an invalid sequence so the
  // arithmetic is isolated from the table.
  const auto arr =
      resolve({Power::Pel, Element::Sil, Form::Mote, Modifier::Arr}, rich, WorldState{}, table, t);
  CHECK(arr.mana_cost == 6);

  // ESK — quieten: cast noise halved.
  const auto esk =
      resolve({Power::Pel, Element::Sil, Form::Mote, Modifier::Esk}, rich, WorldState{}, table, t);
  CHECK(esk.noise == 15);
  CHECK(esk.mana_cost == 3);

  // ITH — split: two targets.
  const auto ith =
      resolve({Power::Pel, Element::Sil, Form::Mote, Modifier::Ith}, rich, WorldState{}, table, t);
  CHECK(ith.target_count == 2);

  // OTH — delay: fires 10 ticks later.
  const auto oth =
      resolve({Power::Pel, Element::Sil, Form::Mote, Modifier::Oth}, rich, WorldState{}, table, t);
  CHECK(oth.delay_ticks == 10);

  // UMBRA — invert, URN — bind.
  CHECK(resolve({Power::Pel, Element::Sil, Form::Mote, Modifier::Umbra}, rich, WorldState{}, table,
                t)
            .inverted);
  CHECK(resolve({Power::Pel, Element::Sil, Form::Mote, Modifier::Urn}, rich, WorldState{}, table, t)
            .bound);
}

TEST_CASE("an underfunded cast is still charged and still makes noise", "[spells]") {
  const auto table = m0_seed_table();
  const RuneSeq expensive{Power::Goth, Element::Hesh, Form::Lom, Modifier::Arr};
  const auto out = resolve(expensive, CasterState{/*mana=*/1}, WorldState{}, table, kDefaultTuning);

  CHECK_FALSE(out.cast);          // could not pay
  CHECK(out.mana_cost == 26);     // 13 x 2 for ARR
  CHECK(out.noise == 130);        // the corridor heard you try
}

TEST_CASE("§8.2's mana ladder", "[spells]") {
  const Tuning& t = kDefaultTuning;
  CHECK(t.power_mana(static_cast<int>(Power::Kai)) == 1);
  CHECK(t.power_mana(static_cast<int>(Power::Tor)) == 2);
  CHECK(t.power_mana(static_cast<int>(Power::Pel)) == 3);
  CHECK(t.power_mana(static_cast<int>(Power::Dun)) == 5);
  CHECK(t.power_mana(static_cast<int>(Power::Bram)) == 8);
  CHECK(t.power_mana(static_cast<int>(Power::Goth)) == 13);
  // Out of range is zero, not a read past the end.
  CHECK(t.power_mana(-1) == 0);
  CHECK(t.power_mana(6) == 0);
}

// ── §8.1: the grammar is the hint ───────────────────────────────────────────

TEST_CASE("§8.1's phonetic rule classifies every rune in the vocabulary", "[runes][property]") {
  // A player who notices the rule has inferred the grammar from four wall
  // carvings. That only works if the rule actually holds over the shipped
  // vocabulary — so it is checked, rune by rune.
  for (int p = 0; p < kPowerCount; ++p) {
    const auto n = name(static_cast<Power>(p));
    INFO(n);
    CHECK(slot_from_inscription(n) == SlotHint::Power);
  }
  for (int e = 0; e < kElementCount; ++e) {
    const auto n = name(static_cast<Element>(e));
    INFO(n);
    CHECK(slot_from_inscription(n) == SlotHint::Element);
  }
  for (int f = 0; f < kFormCount - 1; ++f) {
    const auto n = name(static_cast<Form>(f));
    INFO(n);
    CHECK(slot_from_inscription(n) == SlotHint::Form);
  }
  for (int m = 0; m < kModifierCount - 1; ++m) {
    const auto n = name(static_cast<Modifier>(m));
    INFO(n);
    CHECK(slot_from_inscription(n) == SlotHint::Modifier);
  }
}

TEST_CASE("the grammar has no exceptions at all", "[runes][property]") {
  // This case used to assert the OPPOSITE: that YARN and YRN were both
  // Ambiguous, and were the only two. §8.1 lists Y among the liquids and never
  // among the vowels, so YRN — a Modifier — was simply not obeying its own
  // slot's phonology. Renamed to URN by design decision on gloam#11.
  //
  // Resolved rather than documented, because §17 rules out a spell list, a
  // known-spells screen and autocomplete: the phonetic rule is the ONLY
  // teaching mechanism in the game. A player who infers it, tries it on the
  // exception and gets a wrong answer has been mistaught by the one system
  // meant to teach them — and has no way to find out they were misled. A rule
  // with one exception is more interesting to discover only if discovering the
  // exception is possible.
  int ambiguous = 0;
  int unknown = 0;
  const auto tally = [&](std::string_view n) {
    INFO(n);
    const auto hint = slot_from_inscription(n);
    CHECK(hint != SlotHint::Ambiguous);
    CHECK(hint != SlotHint::Unknown);
    if (hint == SlotHint::Ambiguous) ++ambiguous;
    if (hint == SlotHint::Unknown) ++unknown;
  };
  for (int i = 0; i < kPowerCount; ++i) tally(name(static_cast<Power>(i)));
  for (int i = 0; i < kElementCount; ++i) tally(name(static_cast<Element>(i)));
  for (int i = 0; i < kFormCount - 1; ++i) tally(name(static_cast<Form>(i)));
  for (int i = 0; i < kModifierCount - 1; ++i) tally(name(static_cast<Modifier>(i)));
  CHECK(ambiguous == 0);
  CHECK(unknown == 0);

  // Y now belongs to exactly one class, as §8.1 always said it did.
  CHECK(slot_from_inscription("YARN") == SlotHint::Form);
  CHECK(slot_from_inscription("URN") == SlotHint::Modifier);

  // Every modifier opens on a true vowel — the claim §8.1 actually makes, and
  // the one YRN was breaking.
  for (int m = 0; m < kModifierCount - 1; ++m) {
    const auto n = name(static_cast<Modifier>(m));
    INFO(n);
    REQUIRE_FALSE(n.empty());
    CHECK(std::string_view{"AEIOU"}.find(n.front()) != std::string_view::npos);
  }
}

TEST_CASE("no two runes share an inscription", "[runes][property]") {
  // A rename is exactly the edit that could collide two runes on one written
  // form, and an inscription that means two things breaks discovery outright.
  std::vector<std::string_view> all;
  for (int i = 0; i < kPowerCount; ++i) all.push_back(name(static_cast<Power>(i)));
  for (int i = 0; i < kElementCount; ++i) all.push_back(name(static_cast<Element>(i)));
  for (int i = 0; i < kFormCount - 1; ++i) all.push_back(name(static_cast<Form>(i)));
  for (int i = 0; i < kModifierCount - 1; ++i) all.push_back(name(static_cast<Modifier>(i)));

  CHECK(all.size() == 24);
  for (std::size_t i = 0; i < all.size(); ++i) {
    CHECK_FALSE(all[i].empty());
    for (std::size_t j = i + 1; j < all.size(); ++j) {
      INFO(all[i] << " vs " << all[j]);
      CHECK(all[i] != all[j]);
    }
  }
}

TEST_CASE("the digraphs are classified by their sound, not their first letter", "[runes]") {
  // SHOR opens on "sh" and THULE on "th" — both fricatives, both Elements. A
  // naive single-letter test would file THULE as a stop, because T is one.
  CHECK(slot_from_inscription("SHOR") == SlotHint::Element);
  CHECK(slot_from_inscription("THULE") == SlotHint::Element);
  CHECK(slot_from_inscription("TOR") == SlotHint::Power);  // a real stop
}

TEST_CASE("no inscription names its own class", "[runes]") {
  // §8.2: "do not let the generator print the class of a rune on its
  // inscription."
  const char* kForbidden[] = {"POWER", "ELEMENT", "FORM", "MODIFIER", "SLOT"};
  const auto check = [&](std::string_view n) {
    for (const auto* word : kForbidden) {
      INFO(n << " contains " << word);
      CHECK(n.find(word) == std::string_view::npos);
    }
  };
  for (int i = 0; i < kPowerCount; ++i) check(name(static_cast<Power>(i)));
  for (int i = 0; i < kElementCount; ++i) check(name(static_cast<Element>(i)));
  for (int i = 0; i < kFormCount - 1; ++i) check(name(static_cast<Form>(i)));
  for (int i = 0; i < kModifierCount - 1; ++i) check(name(static_cast<Modifier>(i)));
}

TEST_CASE("every table row is reachable from a placeable inscription set", "[spells][property]") {
  // §13.3's last property, and §8.3's rule: inscriptions are placed from the
  // same table the resolver reads, "so a level never teaches a rune whose
  // partners are unreachable."
  const auto table = m0_seed_table();
  const auto reachable = table.reachable_runes();

  for (int i = 0; i < kCombinationSpace; ++i) {
    const auto seq = RuneSeq::from_index(i);
    const auto* row = table.find(seq);
    if (row == nullptr) continue;

    INFO("row " << i);
    CHECK(reachable.power[static_cast<std::size_t>(seq.power)]);
    CHECK(reachable.element[static_cast<std::size_t>(seq.element)]);
    CHECK(reachable.form[static_cast<std::size_t>(seq.form)]);
    CHECK(reachable.modifier[static_cast<std::size_t>(seq.modifier)]);
  }
}

TEST_CASE("the M0 seed table covers every element", "[spells]") {
  // Not a §13.3 property — a sanity check on the seed content, so that the
  // properties above are running against something with breadth rather than
  // three rows in a corner.
  const auto reachable = m0_seed_table().reachable_runes();
  for (int e = 0; e < kElementCount; ++e) {
    INFO("element " << e);
    CHECK(reachable.element[static_cast<std::size_t>(e)]);
  }
  CHECK(m0_seed_table().size() >= 12);
}
