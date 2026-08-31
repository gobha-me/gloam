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
///
///
/// THE AUDIO INVENTORY (gloam#23, UPSTREAM.md item 10, pack.hpp deviation 5)
///
/// The pack's second record kind is sound: §9.2 requires the resident PCM
/// arena in the pack, covered by the manifest hash. What the pack carries is
/// the synthesiser's INTEGER stage — `src/bin/sfx.cpp` builds its samples as
/// 16-bit integers and only scales them to float for the device — so the blob
/// is s16le, mono, at `audio::kSampleRateHz`, and the expansion back to float
/// is exact (1/32768 is a power of two). One record per real `SoundId`.
///
/// The PCM itself arrives at `build_pack` as a PARAMETER, the way pixels do:
/// synthesis belongs to the sink side (sfx.cpp's three-way split, and the
/// float half could never live here), and this file's job is to describe and
/// carry content, not to generate it. The constants the content is generated
/// TO — rate, frame counts — live in `audio.hpp`, beside the SoundIds.

#include <array>
#include <cstddef>
#include <span>

#include "gloam/audio.hpp"
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

/// One sound in the pack's audio inventory: which `SoundId` the record
/// describes, and how many frames of it the pack carries. Mono s16le at
/// `audio::kSampleRateHz` is fixed for the whole inventory — a record carries
/// its own copy of all three, so a second format lands as data, not a code
/// change. Ids strictly increase down the table: pack records of a kind are
/// id-ordered, and the audio run is no exception.
struct AudioSpec {
  std::uint16_t sound_id;     ///< == the `audio::SoundId` value, on the wire
  std::uint32_t frame_count;  ///< frames per channel
};

/// The audio inventory, in record (id) order. The synthesis arena's in-memory
/// order is generation order (party, monster, sting) — a historical accident
/// nobody outside the clips table can observe; the PACK order is id order,
/// and `sfx::pack_sources` is the one place the two are reconciled.
inline constexpr auto kAudioInventory = std::array{
    AudioSpec{static_cast<std::uint16_t>(audio::SoundId::PartyFootfall),
              audio::kPartyFootfallFrames},
    AudioSpec{static_cast<std::uint16_t>(audio::SoundId::HuntingSting),
              audio::kHuntingStingFrames},
    AudioSpec{static_cast<std::uint16_t>(audio::SoundId::MonsterFootfall),
              audio::kMonsterFootfallFrames},
};

/// How many audio records the pack carries. `SoundId::None` gets no record —
/// it names silence, and silence has no samples.
inline constexpr std::size_t kAudioCount = kAudioInventory.size();

/// Arena frames in total, and the bytes their s16le serialisation occupies.
[[nodiscard]] constexpr auto audio_arena_frames() -> std::size_t {
  std::size_t total = 0;
  for (const auto& spec : kAudioInventory) total += spec.frame_count;
  return total;
}
inline constexpr std::size_t kAudioArenaFrames = audio_arena_frames();
inline constexpr std::size_t kAudioBlobBytes = kAudioArenaFrames * 2;  // s16le, mono

/// One sound's PCM, handed to `build_pack` by the caller: which sound, and
/// its samples as native int16 (the synthesiser's own output). `build_pack`
/// serialises to s16le itself, a byte at a time — pack.hpp's little-endian
/// law reaches blob payloads, so host endianness never reaches the artifact.
struct AudioSource {
  std::uint16_t sound_id{0};
  std::span<const std::int16_t> samples{};
};

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
/// result before handing it back. THE pack — plates and the audio arena alike
/// (gloam#23): there is no plates-only overload, because a pack without its
/// sounds is a thing the launch gate refuses, and this header does not keep a
/// producer for artifacts the runtime rejects.
///
/// `audio` must be `kAudioCount` long, in `kAudioInventory` order, each
/// entry's samples exactly that spec's frame count — `sfx::pack_sources`
/// hands over exactly that shape from a freshly synthesised arena.
/// `audio_bytes` must be at least `kAudioBlobBytes` long; it receives the
/// s16le serialisation the audio records' blobs point into. `records` and
/// `blobs` must both be at least `kPlateCount + kAudioCount` long.
///
/// Verifying here rather than leaving it to the caller is deliberate. \u00a710 makes
/// a mismatched hash refuse to LAUNCH; a baker that can emit a pack it would
/// itself reject moves that failure from the build to the player. `image` must
/// be at least `image_bytes()` long.
[[nodiscard]] auto build_pack(std::span<std::byte> pixels, std::span<pack::Record> records,
                              std::span<std::span<const std::byte>> blobs,
                              std::span<const AudioSource> audio,
                              std::span<std::byte> audio_bytes,
                              std::span<std::byte> image) -> pack::PackResult;

/// The exact size `build_pack` will write. Constant today, because the plate
/// list and the audio inventory both are.
[[nodiscard]] auto image_bytes() -> std::size_t;

}  // namespace gloam::assets
