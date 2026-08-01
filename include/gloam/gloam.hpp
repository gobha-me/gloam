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
/// Ten headers ship in `include/gloam/` and are NOT included below. Nine of
/// them are listed here; `sha256.hpp` is the tenth and has its own paragraph at
/// the end, because it is the one that arrives anyway. That is a decision, not
/// an oversight, and it is written down here so the next person does not
/// helpfully "fix" it:
///
///   budgets.hpp    §11's numbers, as constants and assertions
///   layer.hpp      §4.5's compositing bands and the z-index policy over them
///   emit.hpp       the byte sink §13.4's budget counters wrap around
///   kitty.hpp      the kitty call boundary — every escape sequence, one module
///   dither.hpp     §4.3's fixed ordered matrix, and the one comparison
///   plate.hpp      §10's palette, as two bit-packed planes over a caller's blob
///   lightfield.hpp §4.4's six full-frame screen-door fields
///   pack.hpp       §12's `pack.manifest`, and the bytes it describes
///   assets.hpp     the pack's content manifest — what actually goes in it
///
/// The first four are render-side; the last five are the offline asset pipeline
/// (§10). All nine are inside `gloam::lib` because they are pure — integers and
/// bytes, no clock, no file descriptor, no global, nothing beyond the standard
/// library — and because tests link only `${PROJECT_NAME}::lib`. PRODUCING bytes
/// is not the same as needing a terminal; WRITING them is, and that write lives
/// in `src/bin/`.
///
/// The pipeline five earn their place the same way and one way more: none of
/// them OWNS a plate. Every entry point takes a caller-owned span and says how
/// large to make it, so the "image ownership" objection `kitty.hpp` raises
/// against a transmit path does not apply — the buffers live in
/// `src/bin/bake.cpp`, which holds the pipeline's only file descriptor.
///
/// But none of them is the simulation, and this header is the simulation's
/// public surface. Pulling an escape-sequence emitter or a dither matrix into
/// every consumer's translation unit — including the headless diagnostic in
/// `src/bin/main.cpp` — is the wrong default. Include them by name when you want
/// them.
///
/// `sha256.hpp` is the tenth, and it is a special case in both directions. It
/// is not `#include`d below — so it is genuinely off the list above — but it
/// reaches every consumer anyway, transitively through `replay.hpp` and
/// `world.hpp`, because both name `hash::Digest` in their own interfaces.
///
/// Its old justification was "pipeline-side, not simulation". That stopped
/// being true when `world_hash` arrived: TEST-PLAN.md §2 defines a golden
/// replay in terms of a digest over simulation state, so the hash is now
/// load-bearing on both sides of the house. The sentence is corrected rather
/// than quietly left to rot, because a justification that has become false is
/// worse than none — the next person believes it.

#include <cstdint>

#include "gloam/geometry.hpp"
#include "gloam/level.hpp"
#include "gloam/noise.hpp"
#include "gloam/perception.hpp"
#include "gloam/replay.hpp"
#include "gloam/rng.hpp"
#include "gloam/runes.hpp"
#include "gloam/spells.hpp"
#include "gloam/tuning.hpp"
#include "gloam/world.hpp"

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
