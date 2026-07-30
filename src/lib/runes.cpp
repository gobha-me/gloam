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
    // Y is listed by §8.1 as BOTH a liquid (opening a Form) and a vowel
    // (opening a Modifier). The shipped vocabulary uses it for one of each —
    // YARN and YRN — so the rule cannot separate them.
    case 'Y':
      return SlotHint::Ambiguous;
    default:
      return SlotHint::Unknown;
  }
}

}  // namespace

auto slot_from_inscription(std::string_view inscription) -> SlotHint {
  return opening_class(inscription);
}

}  // namespace gloam
