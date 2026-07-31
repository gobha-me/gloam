#include "gloam/runes.hpp"

namespace gloam {

namespace {

/// §8.1's phonetic classes, as the opening of an inscription.
///
/// Digraphs first: SHOR opens on "sh" and THULE on "th", both fricatives, and
/// testing the single leading letter would file them as a stop (S is a
/// fricative, but T is not).
auto opening_class(std::string_view s) -> SlotHint {
  if (s.size() >= 2) {
    const auto two = s.substr(0, 2);
    if (two == "SH" || two == "TH") return SlotHint::Element;
  }
  if (s.empty()) return SlotHint::Unknown;

  switch (s.front()) {
    // Stops.
    case 'K': case 'T': case 'P': case 'D': case 'B': case 'G':
      return SlotHint::Power;
    // Fricatives.
    case 'S': case 'F': case 'V': case 'H':
      return SlotHint::Element;
    // Liquids and nasals.
    case 'L': case 'R': case 'W': case 'M': case 'N':
      return SlotHint::Form;
    // Vowels.
    case 'A': case 'E': case 'I': case 'O': case 'U':
      return SlotHint::Modifier;
    // §8.1 lists Y explicitly among the liquids and nasals — "(l, r, w, y, m,
    // n)" — and never among the vowels. So Y opens a Form, full stop.
    //
    // This used to return Ambiguous, because the shipped vocabulary had a
    // Modifier opening on Y (YRN) alongside the Form YARN, which made the two
    // indistinguishable by §8.1's rule. That was a vocabulary bug rather than a
    // rule with an exception: YRN simply did not obey its own slot's phonology.
    // Renamed to URN by design decision on gloam#11, and the rule is now exact
    // over all 24 runes — `test/05spells/` asserts that exhaustively.
    case 'Y':
      return SlotHint::Form;
    default:
      return SlotHint::Unknown;
  }
}

}  // namespace

auto slot_from_inscription(std::string_view inscription) -> SlotHint {
  return opening_class(inscription);
}

}  // namespace gloam
