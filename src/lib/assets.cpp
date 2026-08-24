#include "gloam/assets.hpp"

#include <algorithm>

#include "gloam/dither.hpp"

namespace gloam::assets {
namespace {

// ─── The placeholder painters (see assets.hpp's header for what they are) ───
//
// Every rule below is integer arithmetic over the plate's own extent:
// deterministic, seedless, float-free. They are compiled into gloam::lib for
// the reason pack.cpp and kitty.cpp are — producing bytes into a caller-owned
// span is not I/O — and they draw CONTENT, not mechanism: nothing here is
// referenced by the simulation, so AGENTS.md's rules 1-2 are honoured by
// construction rather than by care.
//
// The visual vocabulary, stated once:
//
//   * Geometry is FLAT-FILLED with structural lines. Cross-ink Bayer dither
//     appears only on the floor bands, whose widths are the ring widths —
//     every one a whole number of `kDitherCell`s, so §4.3's no-fractional-
//     boundary property holds for every dithered pixel in the pack. The
//     side-wall strips are 72/48/36/24 wide and 36 does not tile an 8-cell
//     pattern; flat is honest and placeholder-appropriate.
//   * Deeper is darker: the inks walk down the grey ramp with depth, and the
//     light field does the fine falloff on top, exactly as §4.4 intends.
//   * The monster is the BRIGHTEST ink — a pale thing in a dark corridor —
//     and its pose tells are eyes: none for calm (it has not registered you),
//     one for alert, two for hunting. §6.1 says the tell is the deliverable.

/// A coordinate slip in a rule below is a skipped pixel, never UB:
/// `plate::write` refuses out-of-range writes and leaves the blob untouched.
/// The rules are written to be in range; the discard is the safety, and the
/// tests in `test/35sceneplates/` are what prove the rules land.
void put(const plate::PlateSpan& p, int x, int y, plate::Ink ink, bool opaque) {
  static_cast<void>(plate::write(p, x, y, ink, opaque));
}

void fill(const plate::PlateSpan& p, plate::Ink ink, bool opaque) {
  for (int y = 0; y < p.height; ++y) {
    for (int x = 0; x < p.width; ++x) put(p, x, y, ink, opaque);
  }
}

void fill_rect(const plate::PlateSpan& p, int x0, int y0, int x1, int y1, plate::Ink ink,
               bool opaque) {
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) put(p, x, y, ink, opaque);
  }
}

/// The wall body ink for a depth: near walls catch the lamp, far walls sink.
[[nodiscard]] constexpr auto wall_ink(int depth) -> plate::Ink {
  switch (depth) {
    case 0: return plate::Ink::Ink2;
    case 1: return plate::Ink::Ink1;
    case 2: return plate::Ink::Ink1;
    default: return plate::Ink::Ink0;
  }
}

/// A side wall is the trapezoid strip between ring d and ring d+1: the outer
/// edge spans the plate's full height, the inner edge only the smaller ring's
/// span. `gap` is the vertical inset where the inner ring begins, and the two
/// diagonals are the receding top and bottom edges a corridor wall reads as.
void paint_side_wall(const plate::PlateSpan& p, int depth, std::uint8_t variant) {
  const auto h = p.height;
  const auto w = p.width;
  const int gap = (h - geometry::kDepths[static_cast<std::size_t>(depth + 1)].height) / 2;
  const auto body = wall_ink(depth);

  for (int x = 0; x < w; ++x) {
    // Integer lerp, evaluated per column: top runs 0..gap, bottom h..h-gap.
    const int top = gap * x / w;
    const int bottom = h - 1 - gap * x / w;
    for (int y = 0; y < h; ++y) {
      // Outside the trapezoid is the shadowed reveal of the adjoining faces.
      put(p, x, y, y < top || y > bottom ? plate::Ink::Ink0 : body, true);
    }
  }

  if (variant == 1) {
    // The door: a dark panel hung in the trapezoid's middle, framed light.
    fill_rect(p, w / 4, h / 3, (3 * w) / 4, (5 * h) / 6, plate::Ink::Ink0, true);
    for (int x = w / 4; x < (3 * w) / 4; ++x) put(p, x, h / 3, plate::Ink::Ink2, true);
  }
}

/// A front wall is the full face of ring d: a framed panel, or a door.
void paint_front_wall(const plate::PlateSpan& p, std::uint8_t variant) {
  const auto h = p.height;
  const auto w = p.width;
  fill(p, plate::Ink::Ink1, true);
  // The frame: two dark pixels at the edges, one light seam inside them.
  fill_rect(p, 0, 0, w, 2, plate::Ink::Ink0, true);
  fill_rect(p, 0, h - 2, w, h, plate::Ink::Ink0, true);
  fill_rect(p, 0, 0, 2, h, plate::Ink::Ink0, true);
  fill_rect(p, w - 2, 0, w, h, plate::Ink::Ink0, true);
  for (int x = 3; x < w - 3; ++x) {
    put(p, x, 3, plate::Ink::Ink2, true);
    put(p, x, h - 4, plate::Ink::Ink2, true);
  }

  if (variant == 1) {
    // Bottom-anchored, a third of the face wide, two-thirds tall.
    const int x0 = w / 3;
    const int x1 = (2 * w) / 3;
    const int y0 = h / 3;
    fill_rect(p, x0, y0, x1, h - 2, plate::Ink::Ink0, true);
    for (int y = y0; y < h - 2; ++y) {
      put(p, x0, y, plate::Ink::Ink2, true);
      put(p, x1 - 1, y, plate::Ink::Ink2, true);
    }
    for (int x = x0; x < x1; ++x) put(p, x, y0, plate::Ink::Ink2, true);
  }
}

/// A floor band: horizontal tread stripes, brightening underfoot — the bottom
/// rows are the cell the lamp stands in, so they get the one dithered
/// gradient in the pack, Bayer-blended between Ink1 and Ink2.
void paint_floor(const plate::PlateSpan& p) {
  const auto h = p.height;
  const auto w = p.width;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      if (y >= h / 2) {
        // Coverage in [0, kLevels): grows toward the viewer. Widths here are
        // ring widths — dither-aligned by geometry.hpp's static asserts.
        const int coverage = dither::kLevels * (y - h / 2 + 1) / (h - h / 2 + 1) / 3;
        put(p, x, y, dither::opaque_at(coverage, x, y) ? plate::Ink::Ink2 : plate::Ink::Ink1,
            true);
      } else {
        put(p, x, y, (y / 2) % 2 == 0 ? plate::Ink::Ink1 : plate::Ink::Ink0, true);
      }
    }
  }
}

/// A ceiling band: dark, with two beam lines to move under the light field.
void paint_ceiling(const plate::PlateSpan& p) {
  fill(p, plate::Ink::Ink0, true);
  const auto h = p.height;
  const auto w = p.width;
  if (h >= 6) {
    for (int x = 0; x < w; ++x) {
      put(p, x, h / 3, plate::Ink::Ink1, true);
      put(p, x, (2 * h) / 3, plate::Ink::Ink1, true);
    }
  } else if (h >= 2) {
    for (int x = 0; x < w; ++x) put(p, x, h / 2, plate::Ink::Ink1, true);
  }
}

/// The monster: a robed figure in the brightest ink, transparent elsewhere.
/// Poses are §6.1's tells made pixels — calm faces away (no eyes), alert has
/// turned its head (one eye), hunting faces you (two) and reaches.
void paint_monster(const plate::PlateSpan& p, std::uint8_t pose) {
  fill(p, plate::Ink::Ink0, /*opaque=*/false);

  const auto h = p.height;
  const auto w = p.width;
  const int reach = pose == 2 ? 2 : 0;
  const int fw = std::max(4, (2 * w) / 3) + reach;
  const int cx = w / 2;
  const auto pale = plate::Ink::Ink3;

  // The robe: a trapezoid widening from waist to the plate's floor.
  for (int y = h / 2; y < h; ++y) {
    const int half = fw / 4 + (y - h / 2) * (fw / 2 - fw / 4) / std::max(1, h / 2);
    fill_rect(p, cx - half, y, cx + half + 1, y + 1, pale, true);
  }
  // The torso, leaning a touch when hunting.
  const int lean = pose == 2 ? std::max(1, w / 16) : 0;
  fill_rect(p, cx - fw / 4 + lean, h / 4, cx + fw / 4 + lean, h / 2, pale, true);
  // The head. Calm faces away; alert has turned — the head slides sideways.
  const int hs = std::max(3, fw / 4);
  const int head_dx = pose == 1 ? std::max(1, w / 10) : lean;
  const int hx = cx - hs / 2 + head_dx;
  const int hy = h / 4 - hs;
  fill_rect(p, hx, hy, hx + hs, hy + hs, pale, true);

  // The eyes are the tell: none, one, two. Ink0 pixels inside the head.
  const int ey = hy + hs / 2;
  const int eo = std::max(1, hs / 4);
  if (pose == 1) put(p, hx + hs / 2 + eo, ey, plate::Ink::Ink0, true);
  if (pose == 2) {
    put(p, hx + hs / 2 - eo, ey, plate::Ink::Ink0, true);
    put(p, hx + hs / 2 + eo, ey, plate::Ink::Ink0, true);
    // And the reach: two diagonal arms ending below the shoulders.
    for (int i = 0; i < std::max(2, hs); ++i) {
      put(p, cx - fw / 4 + lean - i / 2, h / 4 + i, pale, true);
      put(p, cx + fw / 4 + lean + i / 2, h / 4 + i, pale, true);
    }
  }
}

void paint(const plate::PlateSpan& p, const PlateSpec& spec) {
  switch (spec.role) {
    case pack::Role::Wall:
      if (spec.lateral == pack::Lateral::Centre) {
        paint_front_wall(p, spec.variant);
      } else {
        paint_side_wall(p, spec.depth, spec.variant);
      }
      return;
    case pack::Role::Floor: paint_floor(p); return;
    case pack::Role::Ceiling: paint_ceiling(p); return;
    case pack::Role::Monster: paint_monster(p, spec.variant); return;
    default: fill(p, plate::Ink::Ink0, true); return;  // unreachable at M0
  }
}

/// The descriptive record for one inventory entry. `plate_id` is the index:
/// §12's plate ids are the PACK's namespace, not kitty's — see the note in
/// pack.hpp about why no image id lives here. Light fields keep ids 0-5.
[[nodiscard]] auto record_for(std::size_t index, const PlateSpec& spec) -> pack::Record {
  pack::Record r{};
  r.plate_id = static_cast<std::uint16_t>(index);
  r.role = spec.role;
  r.depth = spec.depth;
  r.lateral = spec.lateral;
  r.variant = spec.variant;
  r.codec = pack::Codec::RawPlanes;
  r.w = static_cast<std::uint16_t>(spec.width);
  r.h = static_cast<std::uint16_t>(spec.height);
  return r;
}

}  // namespace

auto bake_all(std::span<std::byte> pixels, std::span<pack::Record> records,
              std::span<std::span<const std::byte>> blobs) -> lightfield::BakeResult {
  if (pixels.size() < pixel_bytes() || records.size() < static_cast<std::size_t>(kPlateCount) ||
      blobs.size() < static_cast<std::size_t>(kPlateCount)) {
    return {lightfield::BakeError::BufferTooSmall, 0, 0};
  }

  const auto specs = inventory();
  std::size_t offset = 0;
  std::size_t written = 0;
  std::size_t opaque = 0;

  for (std::size_t slot = 0; slot < specs.size(); ++slot) {
    const auto& spec = specs[slot];
    const auto bytes = plate::blob_bytes(spec.width, spec.height);
    const auto blob = pixels.subspan(offset, bytes);
    offset += bytes;

    if (spec.role == pack::Role::LightField) {
      // The one plate class with its own module and its own error vocabulary:
      // the lamp level is the variant, offset back into §4.4's numbering.
      const auto baked =
          lightfield::bake(static_cast<int>(spec.variant) + kLampLevelMin, blob);
      if (!baked) return baked;
      written += baked.bytes;
      opaque += baked.opaque_pixels;
    } else {
      paint(plate::PlateSpan{blob, spec.width, spec.height}, spec);
      written += bytes;
    }

    records[slot] = record_for(slot, spec);
    blobs[slot] = blob;
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
  const auto specs = inventory();
  std::array<pack::Record, 65> records{};
  for (std::size_t slot = 0; slot < specs.size(); ++slot) {
    records[slot] = record_for(slot, specs[slot]);
  }
  return pack::image_bytes(records);
}

}  // namespace gloam::assets
