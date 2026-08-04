#pragma once

/// SPEC §10 — §10's four colours, as concrete RGBA, on the far side of the pack.
///
/// §10 fixes the palette's COUNT and never its VALUES: "four colours plus
/// transparent, 1-bit dithered. Aesthetic discipline and payload discipline in
/// one decision." `plate.hpp` declined to name them, on purpose — "the concrete
/// RGBA the uploader expands these into is a look decision that belongs with the
/// transmit path, and putting it here would make a palette tweak change
/// `pack_sha256`." This header is that transmit path's half of the bargain, and
/// where the values finally get named.
///
/// THE PLACEMENT IS THE POINT. `pack_sha256` is a build gate (§10: two pipeline
/// runs must produce byte-identical packs), and `ruleset_hash` invalidates every
/// recorded replay when it moves. Neither covers this file, and neither should:
/// a look decision must be able to land without invalidating a single replay or
/// re-baking a single pack. What it does move is the wire bytes, which is why
/// `test/25png/` pins a digest over an encoded plate rather than over the pack.
///
///
/// THE VALUES ARE DERIVED, NOT CHOSEN
///
/// A four-step greyscale ramp, `ink * 255 / 3` — 0, 85, 170, 255. That is not an
/// art direction and is not pretending to be one. No art exists yet (gloam#1's
/// authored rings are still open), and a rule stated in one line is a thing the
/// first real look decision can be a deliberate diff AGAINST. A hand-picked
/// four-colour ramp landing now would be indistinguishable from a considered one
/// six months from now, and nobody would know which it was.
///
/// The one part that is a decision rather than a default: TRANSPARENCY IS ENTRY
/// ZERO, not an ink. `plate.hpp` keeps opacity in its own plane precisely so that
/// "stealing a palette entry for transparency" does not cost 25% of the art
/// direction; the encoder re-joins the two planes into one indexed image, so the
/// wire format needs a fifth entry that the ART does not have. Putting it first
/// keeps the four inks' entry numbers in ink order, and keeps `tRNS` — which is a
/// prefix of the palette, per the PNG specification — one byte long.

#include <cstdint>

#include "gloam/plate.hpp"

namespace gloam::palette {

/// One palette entry. No alpha: PNG carries that in `tRNS`, and an alpha here
/// would be a second place to state which entry is the transparent one.
struct Rgb {
  std::uint8_t r{0};
  std::uint8_t g{0};
  std::uint8_t b{0};

  [[nodiscard]] friend constexpr auto operator==(Rgb, Rgb) -> bool = default;
};

/// The transparent entry. First, so `tRNS` is one byte rather than five.
inline constexpr std::uint8_t kTransparentEntry = 0;

/// Four inks plus the transparent entry. Five states need three bits, and PNG
/// has no three-bit depth — which is what makes the encoded form 4 bits per
/// pixel rather than the 3 the pack stores.
inline constexpr int kEntryCount = plate::kInkCount + 1;

/// Which palette entry an ink occupies. Ink order is preserved.
[[nodiscard]] constexpr auto entry_of(plate::Ink ink) -> std::uint8_t {
  return static_cast<std::uint8_t>(static_cast<int>(ink) + 1);
}

/// The ramp: `ink * 255 / 3`, exact at both ends and evenly spaced between.
[[nodiscard]] constexpr auto grey_of(plate::Ink ink) -> std::uint8_t {
  return static_cast<std::uint8_t>(static_cast<int>(ink) * 255 / (plate::kInkCount - 1));
}

[[nodiscard]] constexpr auto rgb_of(plate::Ink ink) -> Rgb {
  const auto v = grey_of(ink);
  return Rgb{v, v, v};
}

/// The entry at index `i` of the encoded palette, transparent entry included.
///
/// The transparent entry's colour is arbitrary — nothing paints it — so it is
/// black, which is also what a terminal that ignores `tRNS` would show behind a
/// screen-door light field. A visible failure is better than an invisible one,
/// but a QUIET failure is better than a bright one.
[[nodiscard]] constexpr auto entry(int i) -> Rgb {
  if (i == kTransparentEntry) return Rgb{0, 0, 0};
  return rgb_of(static_cast<plate::Ink>(i - 1));
}

}  // namespace gloam::palette
