#pragma once

/// SPEC §4.1, §4.6, §16 — the kitty call boundary.
///
/// Every escape sequence GLOAM emits is constructed here and nowhere else.
/// `cmake/check_kitty_boundary.cmake` enforces that mechanically: exactly one
/// file in the shipped tree may contain the APC introducer, and it is
/// `src/lib/kitty.cpp`. Without the check, "every escape sequence through one
/// module" is a convention, and conventions decay.
///
/// §16's mitigation for the project's highest-severity risk — upstream GL-A never
/// landing and GLOAM having to vendor the driver — is that "a vendored driver is
/// a swap and not a rewrite". This header and its .cpp are what gets swapped.
/// `layer.hpp` (the band vocabulary) and `emit.hpp` (the byte counter) sit
/// outside it deliberately, so the swap cannot take them with it.
///
///
/// WHY THIS IS IN `gloam::lib` AND NOT `src/bin/`
///
/// AGENTS.md rule 1 says `gloam::lib` links nothing but the standard library,
/// and "if code needs a terminal, it belongs in `src/bin/`". Someone will
/// eventually read that sentence and try to move this file. The answer:
///
///   PRODUCING bytes is not needing a terminal. WRITING them is.
///
/// Everything here is `(Placement, CellPixelSize) -> bytes appended to a
/// caller-owned buffer`. It reaches no clock, no file descriptor, no global —
/// which is exactly what rule 1's stated rationale (§5.1 replayability) asks
/// for. The `::write` that puts those bytes on a terminal lives in `src/bin/`,
/// and that one line is the whole terminal-facing surface.
///
/// `budgets.hpp` is the precedent and it is exact: render-layer constants, inside
/// the lib, deliberately off the `gloam/gloam.hpp` umbrella. This header is the
/// same category and is excluded from the umbrella for the same reason.
///
///
/// WRITTEN AGAINST THE SHAPE TERMFORGE WILL HAVE
///
/// The placement command below carries a source crop and a sub-cell pixel offset,
/// which is what upstream #115 (GL-B3) will expose, and it carries no destination
/// cell rect at all, which is what upstream #137 (GL-B5) is asking for. That is
/// not today's `draw_image`. When those land this module should SHRINK rather
/// than change shape — see UPSTREAM.md.

#include <cstdint>

#include "gloam/emit.hpp"
#include "gloam/layer.hpp"

namespace gloam::kitty {

/// The terminal's cell size in pixels.
///
/// A parameter at every call site, never stored. The cell geometry belongs to
/// the terminal and is re-pushed on every SIGWINCH (upstream #100, landed), so
/// passing it in is what keeps that ownership visible — a cached copy is a stale
/// copy waiting for a resize.
struct CellPixelSize {
  std::int32_t width_px{0};
  std::int32_t height_px{0};

  [[nodiscard]] constexpr auto operator==(const CellPixelSize&) const -> bool = default;
};

/// One placement of an already-resident image.
///
/// THERE IS NO DESTINATION CELL RECT FIELD, AND THAT ABSENCE IS THE DESIGN.
/// §3.2: "Kitty's `c=`/`r=` cell scaling is never used — it resamples, and
/// resampling a pre-dithered plate is exactly the dither crawl §4.3 exists to
/// avoid." A struct with no field for a destination extent cannot emit one. The
/// grep test is the belt; the missing field is the braces.
///
/// There is no raw `z` field either. Band and rank are the only route to a
/// z-index, so §4.5's "that threshold integer must appear exactly once, inside a
/// named layer API" holds by construction rather than by discipline.
struct Placement {
  std::uint32_t image_id{0};      ///< i= — must be nonzero; kitty reads 0 as "unset"
  std::uint32_t placement_id{0};  ///< p= — must be nonzero, or it can never be deleted

  layer::Band band{layer::Band::Geometry};  ///< §4.5's compositing band
  std::int32_t band_rank{0};                ///< rank within the band; 0 is frontmost

  std::int32_t cell_col{0};  ///< 0-based cell origin; emitted 1-based as a CUP
  std::int32_t cell_row{0};

  std::int32_t offset_x_px{0};  ///< X= sub-cell offset, in [0, cell width)
  std::int32_t offset_y_px{0};  ///< Y= sub-cell offset, in [0, cell height)

  std::int32_t crop_x{0};  ///< x= source crop origin within the image
  std::int32_t crop_y{0};  ///< y=
  std::int32_t crop_w{0};  ///< w= — must be > 0; see EmitError::EmptyCrop
  std::int32_t crop_h{0};  ///< h=
};

/// Why a command was refused. `None` is success.
///
/// A discriminated enum rather than `std::expected`, deliberately. These headers
/// are installed (`cmake/install.cmake` globs `include/`), and AGENTS.md records
/// that Clang 18 cannot compile `<expected>` against libstdc++. Today that break
/// is confined to one test translation unit; in a public header it would become
/// every downstream consumer's problem, and `example/consumer/verify.sh` runs
/// with whatever `clang++` resolves to.
enum class EmitError : std::uint8_t {
  None = 0,
  BandCarriesNoImage = 1,      ///< §4.5's Text or CellBackground row
  RankOutOfRange = 2,          ///< outside [0, layer::rank_limit(band))
  ZeroImageId = 3,
  ZeroPlacementId = 4,
  NegativeCellOrigin = 5,
  DegenerateCellSize = 6,      ///< a terminal that has not answered yet
  SubCellOffsetOutOfRange = 7, ///< an offset of a whole cell is a cell move
  EmptyCrop = 8,               ///< kitty reads w=0 as "to the right edge"
  NegativeCrop = 9,
};

struct EmitResult {
  EmitError error{EmitError::None};
  std::size_t bytes{0};  ///< bytes appended; 0 on every error

  [[nodiscard]] constexpr explicit operator bool() const { return error == EmitError::None; }
};

/// Append one placement command to `sink`.
///
/// Validates completely before writing anything. On any error the sink is
/// untouched — not partially written — because a half-emitted APC sequence
/// corrupts every command that follows it in the stream, and the corruption
/// surfaces somewhere else entirely.
///
/// The emitted form, for a geometry plate at band rank 2, cell (3, 5), a 4x9 px
/// sub-cell offset and a full 480x360 crop, on a 10x20 px cell:
///
///     ESC [ 6 ; 4 H   ESC _G   a=p,i=42,p=7,x=0,y=0,w=480,h=360,
///                              X=4,Y=9,z=-67,C=1,q=2   ESC ST
///
/// where ST is the string terminator (a backslash). The golden literal in
/// `test/07emit/` is the authoritative form; this is the readable one.
///
/// The cursor move is emitted inside the same call so that one placement is one
/// atomic byte string: `a=p` places at the cursor, and splitting the two lets a
/// caller move the cursor in between. `C=1` keeps the cursor still — a
/// compositor emits two dozen placements per frame and a moving cursor scrolls
/// the screen. `q=2` suppresses both OK and error replies, because GLOAM's emit
/// path is fire-and-forget with no reader; `q=1` would leave error replies in
/// the input stream to be mis-parsed as key events.
///
/// The key order is fixed and locked by a golden literal in `test/07emit/`.
/// Byte-for-byte reproducibility is what §12's replay comparison rests on, so a
/// reordered key set has to be a test failure rather than a diff nobody reads.
[[nodiscard]] auto emit_placement(emit::ByteSink& sink, const Placement& placement,
                                  CellPixelSize cell) -> EmitResult;

/// Delete one placement, leaving the image resident. `ESC _G a=d,d=i,i=..,p=..,q=2 ESC \`
///
/// §4.6's frame diff is place-and-delete: the compositor builds a placement list
/// each tick and reconciles it against what is on screen. A placement that
/// cannot be removed is a leak, so this belongs here rather than arriving later
/// in a second file — which is precisely what the boundary check would catch.
[[nodiscard]] auto emit_delete_placement(emit::ByteSink& sink, std::uint32_t image_id,
                                         std::uint32_t placement_id) -> EmitResult;

/// Delete an image and every placement of it. `ESC _G a=d,d=I,i=..,q=2 ESC \`
///
/// Scoped to one image id on purpose. Kitty's delete-all form would wipe the
/// resident plate set and force a full re-upload mid-session, blowing §11's
/// 1.2 MB cold-start payload budget in the middle of a game. There is no
/// function here that emits it, and `test/07emit/` asserts as much.
[[nodiscard]] auto emit_delete_image(emit::ByteSink& sink, std::uint32_t image_id) -> EmitResult;

// ── §4.1 Transmit — deliberately not here yet ───────────────────────────────
//
// The startup upload (`a=t`, base64, chunked at 4096 bytes) belongs in this
// module and will be added to `src/lib/kitty.cpp`. It is not here yet for three
// reasons, written down so the next person adds it HERE rather than inventing a
// second emitter in `src/bin/`:
//
//   1. It needs a pixel source, and the pixel source is the asset pipeline
//      (G-1, gloam#1), which does not exist. Writing an encoder against a
//      payload format nobody has decided is how you get a rewrite, not a shrink.
//   2. It would drag image OWNERSHIP — a buffer of plate data — into
//      `gloam::lib`. That is the first thing in the library that looks like an
//      asset, and it is a boundary argument worth having once, with the pack
//      format (§12) actually in hand.
//   3. Transmit is the one command whose size is enormous (§11's 1.2 MB cold
//      start). A byte-count assertion for it is only meaningful against a real
//      plate.

}  // namespace gloam::kitty
