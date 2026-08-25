#pragma once

/// SPEC \u00a710, \u00a719 step 5 \u2014 what is actually IN the pack.
///
/// `pack.hpp` is the container and knows nothing about GLOAM's art;
/// `lightfield.hpp` bakes pixels and knows nothing about the container. This is
/// the one place the two meet: the pack's CONTENT MANIFEST, the list of plates
/// GLOAM ships and the records that describe them.
///
/// It exists because that list had started to appear in three places \u2014 the
/// baker, `test/12pack/` and `test/10budgets/` \u2014 each independently spelling
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
/// Excluded from the `gloam/gloam.hpp` umbrella \u2014 pipeline-side, not simulation.
///
///
/// THE M0 INVENTORY, AND WHY ITS PIXELS ARE RULES AND NOT ART
///
/// The pack now covers the compositor's whole M0 slot inventory \u2014 \u00a74.2's
/// table minus its six UI frames, which nothing in the corridor slice renders.
/// Every plate except the six light fields is a PLACEHOLDER, drawn by a
/// deterministic integer rule in `assets.cpp` at exactly the slot's extent.
/// gloam#8 needs a corridor a human can READ before it needs a corridor that
/// is beautiful, and the alternative to a rule was a stand-in smuggled in as
/// art. These say what they are: the painters are named, the palette is the
/// mechanical grey ramp of `palette.hpp`, and UPSTREAM.md records the decision.
/// What closes it is authored art arriving plate-for-plate \u2014 the record table
/// below is the contract art slots into, and `test/12pack/`'s golden digest is
/// what makes a placeholder being REPLACED a deliberate line in a diff.
///
/// Rule-drawn also means every depth is painted at its own size directly.
/// \u00a73.1's derive-by-downsample ladder exists to save AUTHORED effort; a rule
/// pays nothing per ring, and painting each size natively keeps every edge
/// exactly where that ring's geometry puts it.

#include <array>
#include <cstddef>
#include <span>

#include "gloam/geometry.hpp"
#include "gloam/lightfield.hpp"
#include "gloam/pack.hpp"
#include "gloam/plate.hpp"
#include "gloam/tuning.hpp"

namespace gloam::assets {

/// One entry of the content manifest: the semantic key the compositor looks
/// up, plus the pixel extent the plate is painted at. The single source both
/// `bake_all` and the record table are generated from \u2014 a plate and its
/// record cannot drift apart, because there is only one list.
struct PlateSpec {
  pack::Role role;
  std::uint8_t depth;
  pack::Lateral lateral;
  std::uint8_t variant;
  int width;
  int height;
};

/// \u00a74.2's wall slot arithmetic. A side wall at depth d is the trapezoid strip
/// between ring d and ring d+1 on its side; a floor or ceiling band is the
/// same gap on the horizontal. `compositor.cpp`'s `anchor()` flush-aligns
/// exactly these minimal extents \u2014 a side wall plate is its strip, not a
/// full-ring plate with the middle left transparent.
[[nodiscard]] constexpr auto side_wall_width(int depth) -> int {
  return (geometry::kDepths[static_cast<std::size_t>(depth)].width -
          geometry::kDepths[static_cast<std::size_t>(depth + 1)].width) /
         2;
}

[[nodiscard]] constexpr auto band_height(int depth) -> int {
  return (geometry::kDepths[static_cast<std::size_t>(depth)].height -
          geometry::kDepths[static_cast<std::size_t>(depth + 1)].height) /
         2;
}

/// A monster plate is a quarter of its ring's width and the ring's full
/// height: the figure stands on the ring floor with transparency above and
/// beside it, and `anchor()`'s lateral alignment puts the strip on the left,
/// centre or right third's side as the slot asks.
[[nodiscard]] constexpr auto monster_width(int depth) -> int {
  return geometry::kDepths[static_cast<std::size_t>(depth)].width / 4;
}

/// The whole M0 manifest, in pack order. Light fields keep plate ids 0-5 \u2014
/// they shipped first and `light_field_record`'s "L0 is id 0" note still
/// holds. Walls, bands and monsters follow in the order \u00a74.2's table lists
/// them. 65 plates: 6 light fields, 24 wall plates (16 side + 8 front, plain
/// and door each), 8 floor/ceiling bands, 27 monster poses \u2014 \u00a74.2's M0 total
/// of 71 minus its 6 UI frames, which the corridor slice does not render.
[[nodiscard]] constexpr auto inventory() -> std::array<PlateSpec, 65> {
  std::array<PlateSpec, 65> out{};
  std::size_t at = 0;
  for (int lamp = kLampLevelMin; lamp <= kLampLevelMax; ++lamp) {
    out[at++] = PlateSpec{pack::Role::LightField, pack::kDepthFullFrame,
                          pack::Lateral::FullFrame,
                          static_cast<std::uint8_t>(lamp - kLampLevelMin),
                          lightfield::kWidthPx, lightfield::kHeightPx};
  }
  for (int depth = 0; depth < geometry::kDepthCount - 1; ++depth) {
    for (const auto lateral : {pack::Lateral::Left, pack::Lateral::Right}) {
      for (std::uint8_t variant = 0; variant <= 1; ++variant) {
        out[at++] = PlateSpec{pack::Role::Wall, static_cast<std::uint8_t>(depth), lateral,
                              variant, side_wall_width(depth),
                              geometry::kDepths[static_cast<std::size_t>(depth)].height};
      }
    }
  }
  for (int depth = 1; depth < geometry::kDepthCount; ++depth) {
    for (std::uint8_t variant = 0; variant <= 1; ++variant) {
      out[at++] = PlateSpec{pack::Role::Wall, static_cast<std::uint8_t>(depth),
                            pack::Lateral::Centre, variant,
                            geometry::kDepths[static_cast<std::size_t>(depth)].width,
                            geometry::kDepths[static_cast<std::size_t>(depth)].height};
    }
  }
  for (int depth = 0; depth < geometry::kDepthCount - 1; ++depth) {
    out[at++] = PlateSpec{pack::Role::Floor, static_cast<std::uint8_t>(depth),
                          pack::Lateral::Centre, 0,
                          geometry::kDepths[static_cast<std::size_t>(depth)].width,
                          band_height(depth)};
  }
  for (int depth = 0; depth < geometry::kDepthCount - 1; ++depth) {
    out[at++] = PlateSpec{pack::Role::Ceiling, static_cast<std::uint8_t>(depth),
                          pack::Lateral::Centre, 0,
                          geometry::kDepths[static_cast<std::size_t>(depth)].width,
                          band_height(depth)};
  }
  for (int depth = 1; depth <= 3; ++depth) {
    for (const auto lateral :
         {pack::Lateral::Left, pack::Lateral::Centre, pack::Lateral::Right}) {
      for (std::uint8_t pose = 0; pose <= 2; ++pose) {
        out[at++] = PlateSpec{pack::Role::Monster, static_cast<std::uint8_t>(depth), lateral,
                              pose, monster_width(depth),
                              geometry::kDepths[static_cast<std::size_t>(depth)].height};
      }
    }
  }
  return out;
}

/// Every plate GLOAM ships today. Was six; see the header comment for what the
/// other 59 are and are not.
inline constexpr int kPlateCount = static_cast<int>(inventory().size());

/// Bytes of pixel storage the caller must provide to `bake_all`: the blobs
/// back to back, each at its spec's extent. No longer a uniform product \u2014
/// side walls, bands and monster strips are not full frames.
[[nodiscard]] constexpr auto pixel_bytes() -> std::size_t {
  std::size_t total = 0;
  for (const auto& spec : inventory()) {
    total += plate::blob_bytes(spec.width, spec.height);
  }
  return total;
}

/// Bake every plate into `pixels`, and fill `records` and `blobs` to describe
/// them. Nothing is assembled yet \u2014 the caller sizes its image buffer from
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
/// Verifying here rather than leaving it to the caller is deliberate. \u00a710 makes
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
