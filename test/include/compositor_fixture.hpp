#pragma once

// Test-only authored manifest substitute for G-7. Production still ships only
// the six light fields; these tiny plates exercise every semantic compositor
// binding without pretending generated pixels are final art.

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

#include "gloam/pack.hpp"
#include "gloam/plate.hpp"
#include "gloam/tuning.hpp"

namespace gloam::test_support {

inline auto compositor_records(std::uint16_t extent = 8) -> std::vector<pack::Record> {
  std::vector<pack::Record> records;
  std::uint16_t id = 1;
  auto add = [&](pack::Role role, std::uint8_t depth, pack::Lateral lateral, std::uint8_t variant) {
    records.push_back(pack::Record{
        .plate_id = id++,
        .role = role,
        .depth = depth,
        .lateral = lateral,
        .variant = variant,
        .codec = pack::Codec::RawPlanes,
        .w = extent,
        .h = extent,
    });
  };
  for (std::uint8_t depth = 0; depth < 4; ++depth) {
    for (const auto lateral : {pack::Lateral::Left, pack::Lateral::Right}) {
      add(pack::Role::Wall, depth, lateral, 0);
      add(pack::Role::Wall, depth, lateral, 1);
    }
    add(pack::Role::Floor, depth, pack::Lateral::Centre, 0);
    add(pack::Role::Ceiling, depth, pack::Lateral::Centre, 0);
  }
  for (std::uint8_t depth = 1; depth <= 4; ++depth) {
    add(pack::Role::Wall, depth, pack::Lateral::Centre, 0);
    add(pack::Role::Wall, depth, pack::Lateral::Centre, 1);
  }
  for (std::uint8_t lamp = 0; lamp < kLampLevelCount; ++lamp) {
    add(pack::Role::LightField, pack::kDepthFullFrame, pack::Lateral::FullFrame, lamp);
  }
  for (std::uint8_t depth = 1; depth <= 3; ++depth) {
    for (const auto lateral : {pack::Lateral::Left, pack::Lateral::Centre, pack::Lateral::Right}) {
      for (std::uint8_t pose = 0; pose <= 2; ++pose) {
        add(pack::Role::Monster, depth, lateral, pose);
      }
    }
  }
  return records;
}

inline auto compositor_pack(std::uint16_t extent = 8) -> std::vector<std::byte> {
  auto records = compositor_records(extent);
  std::vector<std::vector<std::byte>> storage(records.size());
  std::vector<std::span<const std::byte>> blobs(records.size());
  for (std::size_t i = 0; i < records.size(); ++i) {
    storage[i].assign(plate::blob_bytes(records[i].w, records[i].h), std::byte{0});
    blobs[i] = storage[i];
  }
  std::vector<std::byte> image(pack::image_bytes(records));
  const auto built = pack::assemble(records, blobs, image);
  if (!built) throw std::runtime_error{"could not build compositor test pack"};
  image.resize(built.bytes);
  return image;
}

}  // namespace gloam::test_support
