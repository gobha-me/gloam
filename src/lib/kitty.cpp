/// SPEC §4.1, §4.6, §16 — the only file in the shipped tree that spells an
/// escape sequence.
///
/// `cmake/check_kitty_boundary.cmake` enforces that: it greps include/ and src/
/// for the APC introducer and requires exactly one hit, here. See kitty.hpp for
/// why this module is in `gloam::lib` rather than `src/bin/`, and for the
/// transmit seam that is deliberately still empty.

#include "gloam/kitty.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <system_error>

#include "gloam/base64.hpp"
#include "gloam/emit.hpp"
#include "gloam/layer.hpp"

namespace gloam::kitty {
namespace {

// ── The wire vocabulary ─────────────────────────────────────────────────────
//
// Written with "\033" rather than "\x1b" because "\x1b" followed by a hex digit
// is a different character: "\x1b_" is fine but "\x1bd" would parse as \x1bd.
// The octal form has a fixed length and cannot swallow what follows it.

constexpr std::string_view kApcStart = "\033_G";
constexpr std::string_view kApcEnd = "\033\\";
constexpr std::string_view kCsi = "\033[";

/// Longest std::int32_t is "-2147483648": 11 characters. 16 leaves slack.
using NumberBuffer = std::array<char, 16>;

/// Append a decimal integer without allocating.
///
/// `std::to_chars` rather than `std::format` — `${PROJECT_NAME}_DEPS` carries
/// only catch2, so fmtlib is not linked, and AGENTS.md records a history of
/// C++23 library surprises on the GCC 13 floor. Not `std::to_string` either: it
/// allocates per number, and this is the inner loop of §11's 2 ms budget for
/// compose, diff and emit.
auto append_int(emit::ByteSink& sink, std::int64_t value) -> void {
  NumberBuffer buf{};
  const auto [end, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), value);
  // The buffer is sized for the widest value the type can hold, so this cannot
  // fail. Checked anyway: a silent partial number would be a valid-looking
  // command with the wrong coordinate in it.
  if (ec != std::errc{}) return;
  sink.write(std::string_view{buf.data(), static_cast<std::size_t>(end - buf.data())});
}

auto append_key(emit::ByteSink& sink, std::string_view key, std::int64_t value) -> void {
  sink.write(key);
  append_int(sink, value);
}

/// Move the cursor. Kitty's `a=p` places at the cursor, so this and the
/// placement are one atomic byte string — see emit_placement's doc comment.
///
/// CUP is 1-based on both axes. The caller's origin is 0-based, and the +1 here
/// is why a negative origin has to be refused rather than clamped: cell_col = -1
/// would emit "\033[0;0H", which is not an error to a terminal at all. It is a
/// valid CUP meaning row 1, column 1 — a silent misplacement.
auto append_cursor_position(emit::ByteSink& sink, std::int32_t col, std::int32_t row) -> void {
  sink.write(kCsi);
  append_int(sink, static_cast<std::int64_t>(row) + 1);
  sink.write(';');
  append_int(sink, static_cast<std::int64_t>(col) + 1);
  sink.write('H');
}

/// Everything emit_placement refuses, in the order it refuses them.
///
/// Split out so that validation provably completes before a single byte is
/// written. A half-emitted APC sequence corrupts every command after it in the
/// stream, and the corruption surfaces somewhere else entirely — so "validate
/// fully, then write" is the invariant that makes this function safe to call
/// speculatively.
[[nodiscard]] auto validate(const Placement& p, CellPixelSize cell) -> EmitError {
  // The terminal's answer first: everything below is measured against it, and a
  // degenerate cell size is a terminal that has not replied yet rather than a
  // divide-by-zero. Same reading geometry::cells_to_cover takes.
  if (cell.width_px <= 0 || cell.height_px <= 0) return EmitError::DegenerateCellSize;

  if (p.image_id == 0) return EmitError::ZeroImageId;
  if (p.placement_id == 0) return EmitError::ZeroPlacementId;

  if (!layer::carries_image(p.band)) return EmitError::BandCarriesNoImage;
  if (!layer::image_z(p.band, p.band_rank)) return EmitError::RankOutOfRange;

  if (p.cell_col < 0 || p.cell_row < 0) return EmitError::NegativeCellOrigin;

  // An offset of a whole cell is a cell move, not a sub-cell offset. Kitty
  // clamps or rejects it and the plate lands one cell out of place.
  if (p.offset_x_px < 0 || p.offset_x_px >= cell.width_px) {
    return EmitError::SubCellOffsetOutOfRange;
  }
  if (p.offset_y_px < 0 || p.offset_y_px >= cell.height_px) {
    return EmitError::SubCellOffsetOutOfRange;
  }

  if (p.crop_x < 0 || p.crop_y < 0 || p.crop_w < 0 || p.crop_h < 0) {
    return EmitError::NegativeCrop;
  }
  // The subtlest one in this function. Kitty reads w=0 as "to the right edge of
  // the image" and h=0 as "to the bottom" — a DIFFERENT MEANING, not a no-op.
  // An empty crop would silently become a full-image draw, which for a 480x360
  // plate is the whole viewport appearing where a sliver was meant to.
  if (p.crop_w == 0 || p.crop_h == 0) return EmitError::EmptyCrop;

  return EmitError::None;
}

}  // namespace

auto emit_placement(emit::ByteSink& sink, const Placement& placement, CellPixelSize cell)
    -> EmitResult {
  if (const auto error = validate(placement, cell); error != EmitError::None) {
    return EmitResult{error, 0};
  }

  const auto z = layer::image_z(placement.band, placement.band_rank);
  const auto before = sink.size();

  append_cursor_position(sink, placement.cell_col, placement.cell_row);

  sink.write(kApcStart);
  sink.write("a=p");
  append_key(sink, ",i=", placement.image_id);
  append_key(sink, ",p=", placement.placement_id);

  // Source crop — the shape termforge #115 (GL-B3) now exposes.
  append_key(sink, ",x=", placement.crop_x);
  append_key(sink, ",y=", placement.crop_y);
  append_key(sink, ",w=", placement.crop_w);
  append_key(sink, ",h=", placement.crop_h);

  // Sub-cell alignment. §3.2 names X=/Y= as the ONLY sanctioned mechanism, and
  // rules out c=/r= by name because cell scaling resamples.
  append_key(sink, ",X=", placement.offset_x_px);
  append_key(sink, ",Y=", placement.offset_y_px);

  // §4.5's band, resolved through the layer API. There is no other route.
  append_key(sink, ",z=", *z);

  // C=1: do not move the cursor. A frame is two dozen placements and a moving
  // cursor scrolls the screen out from under them.
  // q=2: suppress OK and error replies. Nothing reads this stream, and a reply
  // left in the input buffer gets mis-parsed as a key event.
  sink.write(",C=1,q=2");
  sink.write(kApcEnd);

  return EmitResult{EmitError::None, sink.size() - before};
}

auto emit_delete_placement(emit::ByteSink& sink, std::uint32_t image_id,
                           std::uint32_t placement_id) -> EmitResult {
  if (image_id == 0) return EmitResult{EmitError::ZeroImageId, 0};
  if (placement_id == 0) return EmitResult{EmitError::ZeroPlacementId, 0};

  const auto before = sink.size();
  sink.write(kApcStart);
  // d=i: this placement of this image. Lowercase leaves the image resident,
  // which is the entire point — §4.6's diff removes placements, never plates.
  sink.write("a=d,d=i");
  append_key(sink, ",i=", image_id);
  append_key(sink, ",p=", placement_id);
  sink.write(",q=2");
  sink.write(kApcEnd);

  return EmitResult{EmitError::None, sink.size() - before};
}

auto emit_transmit(emit::ByteSink& sink, std::span<const std::byte> payload,
                   std::uint32_t image_id) -> EmitResult {
  if (image_id == 0) return EmitResult{EmitError::ZeroImageId, 0};
  // Refused rather than emitted as a zero-length transfer. Kitty answers that
  // with an error, and every command in this file carries q=2, so the reply is
  // suppressed — this is the only place an empty upload can be noticed at all.
  if (payload.empty()) return EmitResult{EmitError::EmptyPayload, 0};

  const auto before = sink.size();

  // One chunk's worth of base64, on the stack. The whole point of the caller-
  // owned-span discipline is that this module allocates nothing; 4 KB is the
  // largest buffer the protocol can use, so it is also the largest this needs.
  std::array<char, kTransmitChunkBase64Bytes> encoded{};
  static_assert(kTransmitChunkBase64Bytes == base64::encoded_size(kTransmitChunkPayloadBytes),
                "the chunk constants must stay a matched pair, or a full chunk overruns");

  std::size_t offset = 0;
  bool first = true;

  while (offset < payload.size()) {
    const auto take = std::min(kTransmitChunkPayloadBytes, payload.size() - offset);
    const auto chunk = payload.subspan(offset, take);

    // No refusal is checked here and none is possible: `take` is capped at the
    // raw chunk size, and the static_assert above ties the buffer to exactly the
    // encoding of that. Making the impossibility a compile-time fact rather than
    // a runtime branch is what keeps this function free of a path no test can
    // reach — change either constant and the build stops, not the terminal.
    const auto coded = base64::encode(chunk, std::span<char>{encoded});

    offset += take;
    const bool last = offset >= payload.size();

    sink.write(kApcStart);
    if (first) {
      // a=t: transmit only. The display is a separate placement command, because
      // §4.6's diff places and un-places plates that were uploaded once.
      sink.write("a=t");
      append_key(sink, ",i=", image_id);
      // f=100: the payload is PNG. Kitty reads the geometry from the datastream
      // for this format, so no s=/v= is sent — png::encode declared it in IHDR.
      // t=d: direct, the payload is in the escape sequence.
      sink.write(",f=100,t=d");
      if (!last) sink.write(",m=1");
    } else {
      // Continuation chunks carry only the m key: kitty associates them with the
      // transfer already in progress, and repeating i= would start a new one.
      sink.write(last ? "m=0" : "m=1");
    }
    sink.write(",q=2");

    sink.write(';');
    sink.write(std::string_view{encoded.data(), coded.bytes});
    sink.write(kApcEnd);

    first = false;
  }

  return EmitResult{EmitError::None, sink.size() - before};
}

auto emit_delete_image(emit::ByteSink& sink, std::uint32_t image_id) -> EmitResult {
  if (image_id == 0) return EmitResult{EmitError::ZeroImageId, 0};

  const auto before = sink.size();
  sink.write(kApcStart);
  // d=I: this image and every placement of it, freeing terminal memory. Scoped
  // to one id — kitty's delete-everything form is never emitted from anywhere
  // in this file, because it would evict the resident plate set mid-session.
  sink.write("a=d,d=I");
  append_key(sink, ",i=", image_id);
  sink.write(",q=2");
  sink.write(kApcEnd);

  return EmitResult{EmitError::None, sink.size() - before};
}

}  // namespace gloam::kitty
