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
