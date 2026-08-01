// SPEC §3.1, §4.3, §10 — a plate's two planes, and the exact 2:1 downsample.
//
// The two things this file is really guarding:
//
//   1. Sub-byte packing. Four pixels share an index byte and eight share a
//      stencil byte, so a shift that is off by one corrupts a neighbour rather
//      than the pixel you wrote — which reads as a rendering artefact somewhere
//      else entirely, months later, in a plate nobody touched.
//   2. That `downsample_2to1` is TOTAL and ORDER-FREE. §3.1 derives depths 2-4
//      from the authored rings, so a reduction rule with a tie nobody pinned
//      down is a rule that can differ between compilers — and §10 makes the pack
//      hash a build gate, so that difference is a red build on someone else's
//      machine and nowhere else.
//
// Failure matrix first, per AGENTS.md; the round trip is last.

#include <catch2/catch_all.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "gloam/geometry.hpp"
#include "gloam/plate.hpp"

using namespace gloam;
using gloam::plate::Ink;
using gloam::plate::PlateError;
using gloam::plate::PlateSpan;
using gloam::plate::PlateView;

namespace {

/// A blob sized for `w` x `h`, filled with a recognisable non-zero pattern so a
/// "the buffer was left untouched" assertion has something to be untouched.
[[nodiscard]] auto primed(int w, int h) -> std::vector<std::byte> {
  std::vector<std::byte> blob(plate::blob_bytes(w, h), std::byte{0xA5});
  return blob;
}

[[nodiscard]] auto view(const std::vector<std::byte>& b, int w, int h) -> PlateView {
  return {std::span<const std::byte>{b}, w, h};
}

[[nodiscard]] auto span(std::vector<std::byte>& b, int w, int h) -> PlateSpan {
  return {std::span<std::byte>{b}, w, h};
}

constexpr int kW = 24;  ///< a whole dither block wide, and not a multiple of 8 pixels' worth
constexpr int kH = 8;

}  // namespace

// ── validate: the extent failure matrix ─────────────────────────────────────

TEST_CASE("a degenerate extent is refused", "[plate]") {
  const auto enough = plate::blob_bytes(kW, kH);
  CHECK(plate::validate(0, kH, enough) == PlateError::NonPositiveExtent);
  CHECK(plate::validate(kW, 0, enough) == PlateError::NonPositiveExtent);
  CHECK(plate::validate(-8, kH, enough) == PlateError::NonPositiveExtent);
  CHECK(plate::validate(kW, -1, enough) == PlateError::NonPositiveExtent);
  CHECK(plate::validate(0, 0, enough) == PlateError::NonPositiveExtent);
}

TEST_CASE("an extent a manifest record cannot carry is refused", "[plate]") {
  // §12's record holds w and h as u16. An extent that cannot round trip through
  // a record must fail here rather than be truncated there, where it would
  // produce a manifest that describes a different plate than the one baked.
  constexpr int kMax = std::numeric_limits<std::uint16_t>::max();
  CHECK(plate::validate(kMax + 1, kH, 0) == PlateError::ExtentOutOfRange);
  CHECK(plate::validate(kW, kMax + 1, 0) == PlateError::ExtentOutOfRange);

  // One below the edge is an alignment question, not a range one.
  CHECK(plate::validate(kMax - 7, 1, plate::blob_bytes(kMax - 7, 1)) == PlateError::None);
}

TEST_CASE("dither alignment is a predicate, not a validation rule", "[plate]") {
  // §4.3's alignment is a property of the AUTHORED RING SIZES, and enforcing it
  // inside validate() was a trap: it refused a 24x24 rune glyph — §4.3's own
  // dither block, the natural authoring size — and, worse, it refused to halve
  // the ladder's own 168-wide depth-3 ring, because 84 is not a multiple of 8.
  CHECK(plate::dither_aligned(geometry::kDitherBlock));
  CHECK(plate::dither_aligned(geometry::kDitherCell));
  CHECK_FALSE(plate::dither_aligned(0));
  CHECK_FALSE(plate::dither_aligned(-8));
  for (int w = 1; w < geometry::kDitherCell; ++w) {
    INFO("width " << w);
    CHECK_FALSE(plate::dither_aligned(w));
  }

  // Every ring of the ladder satisfies it — which is the claim §4.3 actually
  // makes, and geometry.hpp static_asserts the same thing.
  for (const auto& ring : geometry::kDepths) {
    INFO("ring " << ring.width << "x" << ring.height);
    CHECK(plate::dither_aligned(ring.width));
  }

  // ...and validate() accepts the sizes that predicate rejects, because a plate
  // is not required to be a ring.
  CHECK(plate::validate(12, 12, plate::blob_bytes(12, 12)) == PlateError::None);
  CHECK(plate::validate(7, 7, plate::blob_bytes(7, 7)) == PlateError::None);
  CHECK(plate::validate(1, 1, plate::blob_bytes(1, 1)) == PlateError::None);
}

TEST_CASE("the ladder's depth-3 ring can actually be halved", "[plate]") {
  // The regression this rule change exists for. 168 / 2 = 84, and 84 % 8 != 0,
  // so an alignment rule inside validate() refused to derive anything from
  // depth 3 at all. The ladder only survived because depth 4 comes from depth 2.
  const auto& d3 = geometry::kDepths[3];
  REQUIRE(d3.width == 168);
  auto src = primed(d3.width, d3.height);
  auto dst = primed(d3.width / 2, d3.height / 2);
  CHECK(plate::downsample_2to1(view(src, d3.width, d3.height),
                               span(dst, d3.width / 2, d3.height / 2)));

  // And a 24x24 rune glyph halves to 12x12, which is the other case the rule
  // was silently forbidding.
  auto rune = primed(24, 24);
  auto small = primed(12, 12);
  CHECK(plate::downsample_2to1(view(rune, 24, 24), span(small, 12, 12)));
}

TEST_CASE("a blob one byte short is refused", "[plate]") {
  // One byte, not a wildly short buffer: the off-by-one is the bug that ships.
  const auto need = plate::blob_bytes(kW, kH);
  CHECK(plate::validate(kW, kH, need - 1) == PlateError::BufferTooSmall);
  CHECK(plate::validate(kW, kH, need) == PlateError::None);
  CHECK(plate::validate(kW, kH, need + 1) == PlateError::None);
  CHECK(plate::validate(kW, kH, 0) == PlateError::BufferTooSmall);
}

TEST_CASE("the two planes are sized independently and summed", "[plate]") {
  CHECK(plate::index_row_bytes(kW) == 6);    // 24 px at 4 per byte
  CHECK(plate::stencil_row_bytes(kW) == 3);  // 24 px at 8 per byte
  CHECK(plate::blob_bytes(kW, kH) == 6 * kH + 3 * kH);

  // Partial trailing bytes are counted, not dropped.
  CHECK(plate::index_row_bytes(1) == 1);
  CHECK(plate::index_row_bytes(4) == 1);
  CHECK(plate::index_row_bytes(5) == 2);
  CHECK(plate::stencil_row_bytes(1) == 1);
  CHECK(plate::stencil_row_bytes(8) == 1);
  CHECK(plate::stencil_row_bytes(9) == 2);

  for (const auto& ring : geometry::kDepths) {
    INFO("ring " << ring.width << "x" << ring.height);
    CHECK(plate::blob_bytes(ring.width, ring.height) ==
          static_cast<std::size_t>(ring.width) * ring.height / 4 +
              static_cast<std::size_t>(ring.width) * ring.height / 8);
  }
}

// ── read / write: coordinates and non-interference ──────────────────────────

TEST_CASE("a coordinate outside the plate is refused", "[plate]") {
  auto blob = primed(kW, kH);

  for (const auto& [x, y] : std::array<std::pair<int, int>, 6>{
           {{kW, 0}, {0, kH}, {-1, 0}, {0, -1}, {kW, kH}, {-1, -1}}}) {
    INFO("(" << x << ", " << y << ")");
    CHECK(plate::read(view(blob, kW, kH), x, y).error == PlateError::CoordinateOutOfRange);
    CHECK(plate::write(span(blob, kW, kH), x, y, Ink::Ink3, true).error ==
          PlateError::CoordinateOutOfRange);
  }

  // The last legitimate pixel is inside.
  CHECK(plate::read(view(blob, kW, kH), kW - 1, kH - 1).error == PlateError::None);
}

TEST_CASE("a refused write leaves the blob byte-identical", "[plate]") {
  auto blob = primed(kW, kH);
  const auto before = blob;

  CHECK_FALSE(plate::write(span(blob, kW, kH), kW, 0, Ink::Ink3, true));
  CHECK(blob == before);

  CHECK_FALSE(plate::write(span(blob, kW, kH), -1, -1, Ink::Ink1, false));
  CHECK(blob == before);

  // A bad extent is refused before any coordinate arithmetic runs.
  PlateSpan bad{std::span<std::byte>{blob}, 0, kH};
  CHECK(plate::write(bad, 0, 0, Ink::Ink2, true).error == PlateError::NonPositiveExtent);
  CHECK(blob == before);
}

TEST_CASE("writing one pixel does not disturb the three sharing its byte", "[plate]") {
  // Swept over all four sub-byte positions, because a shift that is right for
  // one phase and wrong for another is the shape this bug actually takes.
  for (int target = 0; target < 4; ++target) {
    std::vector<std::byte> blob(plate::blob_bytes(kW, kH), std::byte{0});
    auto dst = span(blob, kW, kH);

    // Fill the first four pixels with distinct inks, then rewrite one.
    constexpr std::array<Ink, 4> kInks{Ink::Ink1, Ink::Ink2, Ink::Ink3, Ink::Ink1};
    for (int x = 0; x < 4; ++x) {
      REQUIRE(plate::write(dst, x, 0, kInks[static_cast<std::size_t>(x)], true));
    }
    REQUIRE(plate::write(dst, target, 0, Ink::Ink0, false));

    for (int x = 0; x < 4; ++x) {
      const auto px = plate::read(view(blob, kW, kH), x, 0);
      INFO("target " << target << " pixel " << x);
      REQUIRE(px.error == PlateError::None);
      if (x == target) {
        CHECK(px.ink == Ink::Ink0);
        CHECK_FALSE(px.opaque);
      } else {
        CHECK(px.ink == kInks[static_cast<std::size_t>(x)]);
        CHECK(px.opaque);
      }
    }
  }
}

TEST_CASE("the index and stencil planes are independent", "[plate]") {
  // They are separate byte ranges, and a layout bug that overlaps them shows up
  // as colour changing when opacity is written. Both directions, because only
  // one of them is symmetric.
  std::vector<std::byte> blob(plate::blob_bytes(kW, kH), std::byte{0});
  auto dst = span(blob, kW, kH);

  REQUIRE(plate::write(dst, 5, 3, Ink::Ink3, false));
  auto px = plate::read(view(blob, kW, kH), 5, 3);
  CHECK(px.ink == Ink::Ink3);
  CHECK_FALSE(px.opaque);

  REQUIRE(plate::write(dst, 5, 3, Ink::Ink3, true));
  px = plate::read(view(blob, kW, kH), 5, 3);
  CHECK(px.ink == Ink::Ink3);
  CHECK(px.opaque);

  REQUIRE(plate::write(dst, 5, 3, Ink::Ink0, true));
  px = plate::read(view(blob, kW, kH), 5, 3);
  CHECK(px.ink == Ink::Ink0);
  CHECK(px.opaque);
}

TEST_CASE("a row's trailing pad bits stay clear", "[plate]") {
  // 12 px is 3 index bytes exactly but 2 stencil bytes with 4 bits spare. Those
  // spare bits are hashed into the pack digest, so leaving them undefined would
  // make two runs differ for a reason no test would name.
  constexpr int w = 8;
  constexpr int h = 2;
  std::vector<std::byte> blob(plate::blob_bytes(w, h), std::byte{0});
  auto dst = span(blob, w, h);
  for (int x = 0; x < w; ++x) REQUIRE(plate::write(dst, x, 0, Ink::Ink3, true));

  // Row 1 was never written and must still read back empty.
  for (int x = 0; x < w; ++x) {
    const auto px = plate::read(view(blob, w, h), x, 1);
    INFO("pixel (" << x << ", 1)");
    CHECK(px.ink == Ink::Ink0);
    CHECK_FALSE(px.opaque);
  }
}

// ── downsample_2to1: the failure matrix ─────────────────────────────────────

TEST_CASE("an odd source extent cannot be halved exactly", "[plate]") {
  // §3.1's ladder is exact 2:1 and geometry.hpp static_asserts every parent is
  // even. An odd source would need a rounding rule, and a rounding rule is the
  // kind of thing that gets chosen twice, differently.
  auto src = primed(24, 7);
  auto dst = primed(12, 3);
  CHECK(plate::downsample_2to1(view(src, 24, 7), span(dst, 12, 3)).error ==
        PlateError::OddSourceExtent);

  // Even on both axes goes through, at any width.
  auto src2 = primed(16, 8);
  auto dst2 = primed(8, 4);
  CHECK(plate::downsample_2to1(view(src2, 16, 8), span(dst2, 8, 4)).error == PlateError::None);
}

TEST_CASE("a destination that is not exactly half is refused", "[plate]") {
  auto src = primed(48, 8);
  auto dst = primed(48, 8);
  CHECK(plate::downsample_2to1(view(src, 48, 8), span(dst, 48, 8)).error ==
        PlateError::DestinationExtentMismatch);
  CHECK(plate::downsample_2to1(view(src, 48, 8), span(dst, 24, 8)).error ==
        PlateError::DestinationExtentMismatch);
  CHECK(plate::downsample_2to1(view(src, 48, 8), span(dst, 8, 4)).error ==
        PlateError::DestinationExtentMismatch);
}

TEST_CASE("an undersized span on either side is refused", "[plate]") {
  auto src = primed(16, 4);
  auto dst = primed(8, 2);
  dst.pop_back();
  CHECK(plate::downsample_2to1(view(src, 16, 4), span(dst, 8, 2)).error ==
        PlateError::BufferTooSmall);

  auto src_short = primed(16, 4);
  src_short.pop_back();
  auto dst_ok = primed(8, 2);
  CHECK(plate::downsample_2to1(view(src_short, 16, 4), span(dst_ok, 8, 2)).error ==
        PlateError::BufferTooSmall);
}

TEST_CASE("the stencil reduction breaks its tie toward opaque", "[plate][property]") {
  // Every one of the sixteen opacity patterns in a 2x2 quad, so the >= 2 rule is
  // pinned at the boundary rather than sampled either side of it.
  for (int mask = 0; mask < 16; ++mask) {
    std::vector<std::byte> src(plate::blob_bytes(16, 2), std::byte{0});
    std::vector<std::byte> dst(plate::blob_bytes(8, 1), std::byte{0xFF});
    auto s = span(src, 16, 2);

    int opaque_count = 0;
    for (int i = 0; i < 4; ++i) {
      const bool on = ((mask >> i) & 1) != 0;
      if (on) ++opaque_count;
      REQUIRE(plate::write(s, i % 2, i / 2, Ink::Ink2, on));
    }

    REQUIRE(plate::downsample_2to1(view(src, 16, 2), span(dst, 8, 1)));
    const auto px = plate::read(view(dst, 8, 1), 0, 0);
    INFO("mask " << mask << " with " << opaque_count << " opaque sources");
    REQUIRE(px.error == PlateError::None);
    CHECK(px.opaque == (opaque_count >= 2));
    if (opaque_count == 0) CHECK(px.ink == Ink::Ink0);
  }
}

TEST_CASE("a transparent source's ink never votes", "[plate]") {
  // Three transparent Ink3 pixels and one opaque Ink1. A rule that counted all
  // four would pick Ink3 and drag the shadow entry into every silhouette edge.
  std::vector<std::byte> src(plate::blob_bytes(16, 2), std::byte{0});
  std::vector<std::byte> dst(plate::blob_bytes(8, 1), std::byte{0});
  auto s = span(src, 16, 2);
  REQUIRE(plate::write(s, 0, 0, Ink::Ink1, true));
  REQUIRE(plate::write(s, 1, 0, Ink::Ink3, false));
  REQUIRE(plate::write(s, 0, 1, Ink::Ink3, false));
  REQUIRE(plate::write(s, 1, 1, Ink::Ink3, false));

  REQUIRE(plate::downsample_2to1(view(src, 16, 2), span(dst, 8, 1)));
  const auto px = plate::read(view(dst, 8, 1), 0, 0);
  CHECK(px.ink == Ink::Ink1);
  CHECK_FALSE(px.opaque);  // one of four is below the >= 2 threshold
}

TEST_CASE("an ink tie resolves to the lowest Ink", "[plate]") {
  // Two opaque Ink1 and two opaque Ink3: a genuine tie, and the rule has to pick
  // the same one on both compilers or §10's byte-identical pack does not hold.
  std::vector<std::byte> src(plate::blob_bytes(16, 2), std::byte{0});
  std::vector<std::byte> dst(plate::blob_bytes(8, 1), std::byte{0});
  auto s = span(src, 16, 2);
  REQUIRE(plate::write(s, 0, 0, Ink::Ink3, true));
  REQUIRE(plate::write(s, 1, 0, Ink::Ink1, true));
  REQUIRE(plate::write(s, 0, 1, Ink::Ink3, true));
  REQUIRE(plate::write(s, 1, 1, Ink::Ink1, true));

  REQUIRE(plate::downsample_2to1(view(src, 16, 2), span(dst, 8, 1)));
  const auto px = plate::read(view(dst, 8, 1), 0, 0);
  CHECK(px.ink == Ink::Ink1);
  CHECK(px.opaque);
}

TEST_CASE("a clear majority wins regardless of where it sits in the quad", "[plate]") {
  for (int position = 0; position < 4; ++position) {
    std::vector<std::byte> src(plate::blob_bytes(16, 2), std::byte{0});
    std::vector<std::byte> dst(plate::blob_bytes(8, 1), std::byte{0});
    auto s = span(src, 16, 2);
    for (int i = 0; i < 4; ++i) {
      REQUIRE(plate::write(s, i % 2, i / 2, i == position ? Ink::Ink1 : Ink::Ink3, true));
    }
    REQUIRE(plate::downsample_2to1(view(src, 16, 2), span(dst, 8, 1)));
    INFO("minority ink at quad position " << position);
    CHECK(plate::read(view(dst, 8, 1), 0, 0).ink == Ink::Ink3);
  }
}

TEST_CASE("the ladder's derived rings halve exactly, twice over", "[plate]") {
  // §3.1: depth 2 is depth 0 halved, depth 3 is depth 1 halved, depth 4 is
  // depth 2 halved. Run against real ring extents, not a toy size.
  const auto step = [](int d_src, int d_dst) {
    const auto& s = geometry::kDepths[static_cast<std::size_t>(d_src)];
    const auto& t = geometry::kDepths[static_cast<std::size_t>(d_dst)];
    std::vector<std::byte> src(plate::blob_bytes(s.width, s.height), std::byte{0});
    std::vector<std::byte> dst(plate::blob_bytes(t.width, t.height), std::byte{0xFF});
    PlateSpan ss{std::span<std::byte>{src}, s.width, s.height};
    for (int y = 0; y < s.height; ++y) {
      for (int x = 0; x < s.width; ++x) {
        REQUIRE(plate::write(ss, x, y, Ink::Ink2, ((x / 2) + (y / 2)) % 2 == 0));
      }
    }
    const auto res =
        plate::downsample_2to1(PlateView{std::span<const std::byte>{src}, s.width, s.height},
                               PlateSpan{std::span<std::byte>{dst}, t.width, t.height});
    REQUIRE(res);
    CHECK(res.bytes == plate::blob_bytes(t.width, t.height));

    // The 2x2 checker collapses to a 1x1 checker, exactly.
    for (int y = 0; y < t.height; ++y) {
      for (int x = 0; x < t.width; ++x) {
        const auto px = plate::read(PlateView{std::span<const std::byte>{dst}, t.width, t.height},
                                    x, y);
        REQUIRE(px.error == PlateError::None);
        REQUIRE(px.opaque == ((x + y) % 2 == 0));
      }
    }
  };

  step(0, 2);
  step(1, 3);
  step(2, 4);
}

// ── The round trip, last and least interesting ──────────────────────────────

TEST_CASE("every (ink, opacity) pair round trips", "[plate]") {
  std::vector<std::byte> blob(plate::blob_bytes(kW, kH), std::byte{0});
  auto dst = span(blob, kW, kH);

  const auto ink_at = [](int x, int y) { return static_cast<Ink>((x + y * 3) % 4); };
  const auto opaque_at = [](int x, int y) { return ((x * 5 + y) % 3) != 0; };

  for (int y = 0; y < kH; ++y) {
    for (int x = 0; x < kW; ++x) {
      REQUIRE(plate::write(dst, x, y, ink_at(x, y), opaque_at(x, y)));
    }
  }
  for (int y = 0; y < kH; ++y) {
    for (int x = 0; x < kW; ++x) {
      const auto px = plate::read(view(blob, kW, kH), x, y);
      INFO("pixel (" << x << ", " << y << ")");
      REQUIRE(px.error == PlateError::None);
      REQUIRE(px.ink == ink_at(x, y));
      REQUIRE(px.opaque == opaque_at(x, y));
    }
  }
}
