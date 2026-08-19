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
/// Seventeen headers ship in `include/gloam/` and are NOT included below. Sixteen
/// of them are listed here; `sha256.hpp` is the seventeenth and has its own
/// paragraph at the end, because it is the one that arrives anyway. That is a
/// decision, not an oversight, and it is written down here so the next person
/// does not helpfully "fix" it:
///
///   budgets.hpp    §11's numbers, as constants and assertions
///   layer.hpp      §4.5's compositing bands and the z-index policy over them
///   emit.hpp       the byte sink §11's budget counters wrap around
///   meter.hpp      §11's per-frame classes, and the p95 over their history
///   kitty.hpp      the kitty call boundary — every escape sequence, one module
///   compositor.hpp §4's pure world-to-placement-list and diff
///   audio.hpp      §9's voice ring, and the gain/pan derivation over §6.2
///   dither.hpp     §4.3's fixed ordered matrix, and the one comparison
///   plate.hpp      §10's palette, as two bit-packed planes over a caller's blob
///   lightfield.hpp §4.4's six full-frame screen-door fields
///   pack.hpp       §12's `pack.manifest`, and the bytes it describes
///   assets.hpp     the pack's content manifest — what actually goes in it
///   palette.hpp    §10's four colours as concrete RGBA, outside `pack_sha256`
///   png.hpp        §10's plate as an `f=100` payload — the cold-start form
///   deflate.hpp    a zlib stream, because §11's payload budget needs one
///   base64.hpp     the encoding §4.1's one upload per plate pays for
///
/// The first six are render-side; `audio.hpp` is §9; the last nine are the
/// offline asset pipeline and the upload path it feeds (§10, §4.1). All sixteen
/// are inside `gloam::lib` because they
/// are pure — integers and bytes, no clock, no file descriptor, no global,
/// nothing beyond the standard library — and because tests link only
/// `${PROJECT_NAME}::lib`. PRODUCING values or bytes is not the same as needing a
/// terminal; WRITING them is, and that write lives in `src/bin/`.
///
/// `audio.hpp` is the one whose NAME argues hardest against its placement, so it
/// gets a sentence: what is in `gloam::lib` is the SPSC ring, the command type
/// and the arithmetic that turns §6.2's propagation into a gain and a pan. The
/// RtAudio stream is a device and lives in `src/bin/`, exactly as §9.3 asks
/// ("RtAudio stays behind one translation unit"). It stays off this umbrella
/// even so, because `advance` names `audio::Sink` only through a POINTER —
/// `world.hpp` forward-declares it — and there is no reason for a translation
/// unit that merely ticks a world to compile a `std::atomic` ring it will never
/// touch. Include it by name when you want it.
///
/// The pipeline nine earn their place the same way and one way more: none of
/// them OWNS a plate. Every entry point takes a caller-owned span and says how
/// large to make it, so the "image ownership" objection `kitty.hpp` raised
/// against a transmit path never applied — the buffers live in
/// `src/bin/bake.cpp`, which holds the pipeline's only file descriptor, and in
/// `src/bin/main.cpp`, which owns the one `deflate::Scratch` a cold start needs.
/// That objection is now retired rather than pending: the transmit path landed
/// and the ownership it was worried about did not arrive with it.
///
/// But none of them is the simulation, and this header is the simulation's
/// public surface. Pulling an escape-sequence emitter or a dither matrix into
/// every consumer's translation unit — including the headless diagnostic in
/// `src/bin/main.cpp` — is the wrong default. Include them by name when you want
/// them.
///
/// `sha256.hpp` is the sixteenth, and it is a special case in both directions. It
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
#include "gloam/path.hpp"
#include "gloam/perception.hpp"
#include "gloam/replay.hpp"
#include "gloam/rng.hpp"
#include "gloam/runes.hpp"
#include "gloam/spells.hpp"
#include "gloam/tuning.hpp"
#include "gloam/world.hpp"

namespace gloam {

/// The build's version, as a NUL-terminated string: exactly `vMAJOR.MINOR.PATCH`,
/// and never the project name. No `-dirty` marker and no commits-since-tag
/// suffix — those live in VERSION_DIRTY / VERSION_TWEAK in the generated header.
///
/// The shape is a promise, not a description: test/00version/ parses it.
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
