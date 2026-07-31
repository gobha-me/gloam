#pragma once

/// GLOAM — the public surface of the simulation library.
///
/// `gloam::lib` is the DETERMINISTIC CORE and nothing else: integer state, no
/// I/O, no terminal, no clock, no dependency beyond the standard library. That
/// is not tidiness, it is §5.1 — a simulation that can reach a wall clock or a
/// terminal is a simulation that cannot be replayed, and §13.2 has no
/// flaky-test triage path because a nondeterministic sim has no correct
/// behaviour to fall back to.
///
/// Everything that touches a terminal lives in `src/bin/` and links termforge.
/// If you find yourself wanting to include a driver header from this library,
/// that is the design telling you the code belongs on the other side.
///
///
/// WHAT THIS UMBRELLA DELIBERATELY LEAVES OUT
///
/// Four headers ship in `include/gloam/` and are NOT included below. That is a
/// decision, not an oversight, and it is written down here so the next person
/// does not helpfully "fix" it:
///
///   budgets.hpp  §11's numbers, as constants and assertions
///   layer.hpp    §4.5's compositing bands and the z-index policy over them
///   emit.hpp     the byte sink §13.4's budget counters wrap around
///   kitty.hpp    the kitty call boundary — every escape sequence, one module
///
/// All four are render-side. They are inside `gloam::lib` because they are pure
/// — integers and bytes, no clock, no file descriptor, no global, nothing beyond
/// the standard library — and because tests link only `${PROJECT_NAME}::lib`.
/// PRODUCING bytes is not the same as needing a terminal; WRITING them is, and
/// that write lives in `src/bin/`.
///
/// But they are not the simulation, and this header is the simulation's public
/// surface. Pulling an escape-sequence emitter into every consumer's translation
/// unit — including the headless diagnostic in `src/bin/main.cpp` — is the wrong
/// default. Include them by name when you want them.

#include <cstdint>

#include "gloam/geometry.hpp"
#include "gloam/level.hpp"
#include "gloam/noise.hpp"
#include "gloam/perception.hpp"
#include "gloam/rng.hpp"
#include "gloam/runes.hpp"
#include "gloam/spells.hpp"
#include "gloam/tuning.hpp"

namespace gloam {

/// The build's version, as a NUL-terminated string.
///
/// Declared here and defined in the library's translation unit on purpose: a
/// header of pure constexpr would compile and "pass" in a consumer that never
/// linked the archive. This pair is what makes `example/consumer/` a link test
/// rather than a compile test.
auto version_string() -> const char*;

/// Component-major-first version comparison. A larger minor never rescues a
/// smaller major.
auto version_at_least(std::uint32_t major, std::uint32_t minor, std::uint32_t patch) -> bool;

}  // namespace gloam
