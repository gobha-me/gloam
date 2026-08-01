#pragma once

/// SPEC §10, §19 step 5 — what is actually IN the pack.
///
/// `pack.hpp` is the container and knows nothing about GLOAM's art;
/// `lightfield.hpp` bakes pixels and knows nothing about the container. This is
/// the one place the two meet: the pack's CONTENT MANIFEST, the list of plates
/// GLOAM ships and the records that describe them.
///
/// It exists because that list had started to appear in three places — the
/// baker, `test/12pack/` and `test/10budgets/` — each independently spelling
/// out the same seven descriptive fields. Three copies of "what a light-field
/// record looks like" is three chances to drift, and worse, it meant the golden
/// digest was asserted against a test's private copy rather than against the
/// bytes `gloam_bake` actually writes. Now there is one builder and everything
/// checks the same thing.
///
/// Allocates nothing and opens nothing: the caller owns every span, and
/// `pixel_bytes` / `pack::image_bytes` tell it how large to make them. The only
/// file descriptor in the pipeline is in `src/bin/bake.cpp`.
///
/// Excluded from the `gloam/gloam.hpp` umbrella — pipeline-side, not simulation.

#include <cstddef>
#include <span>

#include "gloam/lightfield.hpp"
#include "gloam/pack.hpp"

namespace gloam::assets {

/// Every plate GLOAM ships today.
///
/// Six, and they are all light fields. §4.2's inventory budgets 71 for M0, so
/// this is the beginning of the list rather than the whole of it — the wall,
/// monster and UI rows arrive with the authored art (gloam#1), and they arrive
/// HERE, next to the fields, rather than in another copy somewhere else.
inline constexpr int kPlateCount = lightfield::kFieldCount;

/// Bytes of pixel storage the caller must provide to `bake_all`.
[[nodiscard]] constexpr auto pixel_bytes() -> std::size_t {
  return plate::blob_bytes(lightfield::kWidthPx, lightfield::kHeightPx) *
         static_cast<std::size_t>(lightfield::kFieldCount);
}

/// Bake every plate into `pixels`, and fill `records` and `blobs` to describe
/// them. Nothing is assembled yet — the caller sizes its image buffer from
/// `pack::image_bytes(records)` and calls `pack::assemble`.
///
/// `pixels` must be `pixel_bytes()` long; `records` and `blobs` must both be
/// `kPlateCount` long. Returns the first bake failure, or `None`.
[[nodiscard]] auto bake_all(std::span<std::byte> pixels, std::span<pack::Record> records,
                            std::span<std::span<const std::byte>> blobs)
    -> lightfield::BakeResult;

/// The whole pipeline in one call: bake, describe, assemble, and verify the
/// result before handing it back.
///
/// Verifying here rather than leaving it to the caller is deliberate. §10 makes
/// a mismatched hash refuse to LAUNCH; a baker that can emit a pack it would
/// itself reject moves that failure from the build to the player. `image` must
/// be at least `image_bytes()` long.
[[nodiscard]] auto build_pack(std::span<std::byte> pixels, std::span<pack::Record> records,
                              std::span<std::span<const std::byte>> blobs,
                              std::span<std::byte> image) -> pack::PackResult;

/// The exact size `build_pack` will write. Constant today, because the plate
/// list is.
[[nodiscard]] auto image_bytes() -> std::size_t;

}  // namespace gloam::assets
