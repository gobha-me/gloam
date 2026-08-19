#include "gloam/assets.hpp"

#include <array>

namespace gloam::assets {
namespace {

/// The descriptive fields of one light-field record, and the only place they
/// are spelled out. §4.4 ships one field per lamp level, full-frame, so depth
/// and lateral both take their sentinel values.
[[nodiscard]] auto light_field_record(int lamp_level) -> pack::Record {
  pack::Record r{};
  // §12's plate ids are the PACK's namespace, not kitty's — see the note in
  // pack.hpp about why no image id lives here. L0 is id 0.
  r.plate_id = static_cast<std::uint16_t>(lamp_level - kLampLevelMin);
  r.role = pack::Role::LightField;
  r.depth = pack::kDepthFullFrame;
  r.lateral = pack::Lateral::FullFrame;
  // The generic variant byte selects the member within a semantic role. For
  // light fields that member is exactly the lamp level (§4.4).
  r.variant = static_cast<std::uint8_t>(lamp_level - kLampLevelMin);
  r.codec = pack::Codec::RawPlanes;
  r.w = static_cast<std::uint16_t>(lightfield::kWidthPx);
  r.h = static_cast<std::uint16_t>(lightfield::kHeightPx);
  return r;
}

}  // namespace

auto bake_all(std::span<std::byte> pixels, std::span<pack::Record> records,
              std::span<std::span<const std::byte>> blobs) -> lightfield::BakeResult {
  if (pixels.size() < pixel_bytes() || records.size() < static_cast<std::size_t>(kPlateCount) ||
      blobs.size() < static_cast<std::size_t>(kPlateCount)) {
    return {lightfield::BakeError::BufferTooSmall, 0, 0};
  }

  const auto field_bytes = plate::blob_bytes(lightfield::kWidthPx, lightfield::kHeightPx);
  std::size_t written = 0;
  std::size_t opaque = 0;

  for (int level = kLampLevelMin; level <= kLampLevelMax; ++level) {
    const auto slot = static_cast<std::size_t>(level - kLampLevelMin);
    const auto blob = pixels.subspan(slot * field_bytes, field_bytes);

    const auto baked = lightfield::bake(level, blob);
    if (!baked) return baked;

    records[slot] = light_field_record(level);
    blobs[slot] = blob;
    written += baked.bytes;
    opaque += baked.opaque_pixels;
  }

  return {lightfield::BakeError::None, written, opaque};
}

auto build_pack(std::span<std::byte> pixels, std::span<pack::Record> records,
                std::span<std::span<const std::byte>> blobs, std::span<std::byte> image)
    -> pack::PackResult {
  if (const auto baked = bake_all(pixels, records, blobs); !baked) {
    // A bake failure is a buffer failure by the time it reaches here — the lamp
    // levels are ours, not the caller's — so it maps to the pack's own name for
    // the same thing rather than leaking a second error vocabulary.
    return {pack::PackError::BufferTooSmall, 0, 0};
  }

  const auto plates = records.first(static_cast<std::size_t>(kPlateCount));
  const auto sources = blobs.first(static_cast<std::size_t>(kPlateCount));

  if (const auto assembled = pack::assemble(plates, sources, image); !assembled) {
    return assembled;
  }

  // §10: the pack has to survive its own gate before anyone sees it.
  const auto total = pack::image_bytes(plates);
  const auto res = pack::verify(std::span<const std::byte>{image}.first(total));
  if (!res) return res;
  return {pack::PackError::None, total, 0};
}

auto image_bytes() -> std::size_t {
  std::array<pack::Record, static_cast<std::size_t>(kPlateCount)> records{};
  for (int level = kLampLevelMin; level <= kLampLevelMax; ++level) {
    records[static_cast<std::size_t>(level - kLampLevelMin)] = light_field_record(level);
  }
  return pack::image_bytes(records);
}

}  // namespace gloam::assets
