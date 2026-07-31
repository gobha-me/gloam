// SPEC §3.2, §4.6, §11, §13.4 — the byte sink and the kitty call boundary.
//
// Two subjects in one file, sink first, because testing a byte counter apart
// from its only consumer is testing a std::string wrapper. The assertions worth
// having — that a refused command leaves the counter untouched, that no emitted
// byte sequence ever contains a cell-scaling key — are joint properties of the
// two, and they are what §3.2 and §13.4 actually ask for.
//
// Failure matrix first, per AGENTS.md. The golden byte literal is last: it is a
// smoke check plus a lock on key order, and it proves the least of anything here.

#include <catch2/catch_all.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "gloam/budgets.hpp"
#include "gloam/emit.hpp"
#include "gloam/kitty.hpp"
#include "gloam/layer.hpp"

using namespace gloam;
using gloam::kitty::CellPixelSize;
using gloam::kitty::EmitError;
using gloam::kitty::Placement;
using gloam::layer::Band;

namespace {

constexpr CellPixelSize kReferenceCell{10, 20};

/// A placement that passes validation, so each case can spoil exactly one field.
[[nodiscard]] auto good_placement() -> Placement {
  Placement p;
  p.image_id = 42;
  p.placement_id = 7;
  p.band = Band::Geometry;
  p.band_rank = 2;
  p.cell_col = 3;
  p.cell_row = 5;
  p.offset_x_px = 4;
  p.offset_y_px = 9;
  p.crop_x = 0;
  p.crop_y = 0;
  p.crop_w = 480;
  p.crop_h = 360;
  return p;
}

/// The key tokens of every APC body in `bytes`, split on ',' and truncated at
/// '='. Splitting rather than searching for "c=" matters: a substring search
/// stays accidentally correct today and rots the moment a key ending in 'c' is
/// added, which is exactly the drift §3.2's ban needs to survive.
[[nodiscard]] auto apc_keys(std::string_view bytes) -> std::vector<std::string> {
  std::vector<std::string> keys;
  constexpr std::string_view kStart = "\033_G";
  constexpr std::string_view kEnd = "\033\\";

  std::size_t at = 0;
  while ((at = bytes.find(kStart, at)) != std::string_view::npos) {
    const auto body_start = at + kStart.size();
    const auto body_end = bytes.find(kEnd, body_start);
    if (body_end == std::string_view::npos) break;
    const auto body = bytes.substr(body_start, body_end - body_start);

    std::size_t field = 0;
    while (field <= body.size()) {
      const auto comma = body.find(',', field);
      const auto piece =
          body.substr(field, comma == std::string_view::npos ? std::string_view::npos : comma - field);
      const auto eq = piece.find('=');
      keys.emplace_back(eq == std::string_view::npos ? piece : piece.substr(0, eq));
      if (comma == std::string_view::npos) break;
      field = comma + 1;
    }
    at = body_end + kEnd.size();
  }
  return keys;
}

[[nodiscard]] auto count_of(std::string_view haystack, std::string_view needle) -> int {
  int n = 0;
  for (std::size_t at = 0; (at = haystack.find(needle, at)) != std::string_view::npos;
       at += needle.size()) {
    ++n;
  }
  return n;
}

}  // namespace

// ════════════════════════════════════════════════════════════════════════════
//  Part A — the byte sink (§11, §13.4)
// ════════════════════════════════════════════════════════════════════════════

TEST_CASE("a fresh sink reports nothing in every dimension", "[emit]") {
  emit::ByteSink sink;
  CHECK(sink.size() == 0);
  CHECK(sink.total() == 0);
  CHECK(sink.frames() == 0);
  CHECK(sink.peak_frame() == 0);
  CHECK(sink.view().empty());
}

TEST_CASE("the cumulative total survives clear(), which is G-6's whole contract", "[emit]") {
  // If this regresses, every §11 byte budget silently resets each frame and
  // passes forever. It is the single most load-bearing assertion in the file.
  emit::ByteSink sink;
  sink.write("0123456789");
  REQUIRE(sink.size() == 10);
  REQUIRE(sink.total() == 10);

  sink.clear();
  CHECK(sink.size() == 0);
  CHECK(sink.total() == 10);

  sink.write("abcde");
  CHECK(sink.size() == 5);
  CHECK(sink.total() == 15);
}

TEST_CASE("peak_frame is the largest frame, not the most recent one", "[emit]") {
  // §11's 400 B and 2 KB rows are per-frame caps, so a run is judged on its
  // worst frame. A "last frame" reading would pass a run that blew the budget
  // in the middle and then went quiet.
  emit::ByteSink sink;
  sink.write(std::string(100, 'x'));
  sink.clear();
  sink.write(std::string(10, 'y'));
  sink.clear();

  CHECK(sink.peak_frame() == 100);
  CHECK(sink.total() == 110);
  CHECK(sink.frames() == 2);
}

TEST_CASE("an idle frame is still a frame", "[emit]") {
  // §11's zero-byte idle row is unmeasurable if clear() on an empty sink is a
  // no-op: a run of idle frames would be indistinguishable from no run at all.
  emit::ByteSink sink;
  sink.clear();
  sink.clear();
  sink.clear();

  CHECK(sink.frames() == 3);
  CHECK(sink.total() == 0);
  CHECK(sink.peak_frame() == 0);
  CHECK(sink.size() == budget::kIdleFrameBytes);
}

TEST_CASE("reset_totals starts a window and is never implicit", "[emit]") {
  emit::ByteSink sink;
  sink.write("abcdef");
  sink.clear();
  sink.write("gh");

  // Neither write nor clear resets the window — a measurement window that
  // resets itself always passes.
  REQUIRE(sink.total() == 8);
  REQUIRE(sink.frames() == 1);
  REQUIRE(sink.peak_frame() == 6);

  sink.reset_totals();
  CHECK(sink.total() == 0);
  CHECK(sink.frames() == 0);
  CHECK(sink.peak_frame() == 0);
  // ...and the buffer is untouched: reset_totals ends a measurement, not a frame.
  CHECK(sink.size() == 2);
  CHECK(sink.view() == "gh");
}

TEST_CASE("a NUL inside a payload is stored, not treated as a terminator", "[emit]") {
  // Base64 is text, but the transmit path this sink will eventually carry deals
  // in binary chunks. A sink that stopped at an embedded zero would truncate a
  // plate and the terminal would render garbage from a valid-looking command.
  emit::ByteSink sink;
  const std::string payload{"a\0b", 3};
  sink.write(payload);

  CHECK(sink.size() == 3);
  CHECK(sink.total() == 3);
  CHECK(sink.view().size() == 3);
  CHECK(sink.view()[1] == '\0');
}

TEST_CASE("an empty write is a no-op and does not end a frame", "[emit]") {
  emit::ByteSink sink;
  sink.write(std::string_view{});
  CHECK(sink.size() == 0);
  CHECK(sink.total() == 0);
  CHECK(sink.frames() == 0);
}

TEST_CASE("saturating_add pins at the ceiling instead of wrapping", "[emit]") {
  // A wrapped byte counter reads as a PASSING budget, which is the one failure
  // mode a budget instrument must not have. Free function precisely so this
  // boundary is reachable without a test-only seam in shipped code — you cannot
  // realistically fill a 64-bit counter by writing to a sink.
  constexpr auto kMax = std::numeric_limits<std::uint64_t>::max();

  CHECK(emit::saturating_add(0, 0) == 0);
  CHECK(emit::saturating_add(10, 5) == 15);
  CHECK(emit::saturating_add(kMax, 0) == kMax);
  CHECK(emit::saturating_add(kMax, 1) == kMax);
  CHECK(emit::saturating_add(kMax - 3, 10) == kMax);
  CHECK(emit::saturating_add(kMax - 3, 3) == kMax);
  CHECK(emit::saturating_add(kMax - 4, 3) == kMax - 1);
}

TEST_CASE("the sink does not cap itself at any budget", "[emit]") {
  // Deliberately over §11's cold-start payload limit. The sink reports and the
  // budget judges; a sink that truncated would turn "over budget" into a silent
  // corruption instead of a test failure.
  emit::ByteSink sink;
  const std::size_t oversized = budget::kMaxColdStartPayloadBytes + 300'000;
  sink.write(std::string(oversized, 'z'));

  CHECK(sink.size() == oversized);
  CHECK(sink.total() == oversized);
  CHECK(sink.size() > budget::kMaxColdStartPayloadBytes);
}

// ════════════════════════════════════════════════════════════════════════════
//  Part B — the kitty emitter (§3.2, §4.5, §4.6)
// ════════════════════════════════════════════════════════════════════════════

// ── §3.2's ban, which is the reason this module exists ──────────────────────

TEST_CASE("no emitted placement ever carries a cell-scaling key", "[emit][kitty][property]") {
  // §3.2: "Kitty's c=/r= cell scaling is never used — it resamples, and
  // resampling a pre-dithered plate is exactly the dither crawl §4.3 exists to
  // avoid." Swept over every image-carrying band, the rank extremes, and both
  // zero and nonzero offsets and crops, because a key emitted conditionally is
  // the shape this could plausibly regress into.
  for (const auto band : {Band::Overlay, Band::Light, Band::Sprites, Band::Geometry,
                          Band::BelowBackground}) {
    for (const int rank : {0, 1, layer::kBandStride - 1}) {
      for (const int offset : {0, 1, kReferenceCell.width_px - 1}) {
        for (const int crop : {1, 240, 480}) {
          emit::ByteSink sink;
          auto p = good_placement();
          p.band = band;
          p.band_rank = rank;
          p.offset_x_px = offset;
          p.offset_y_px = offset % kReferenceCell.height_px;
          p.crop_x = crop / 2;
          p.crop_y = crop / 2;
          p.crop_w = crop;
          p.crop_h = crop;

          const auto result = kitty::emit_placement(sink, p, kReferenceCell);
          INFO("band " << static_cast<int>(band) << " rank " << rank << " offset " << offset
                       << " crop " << crop);
          REQUIRE(result);

          for (const auto& key : apc_keys(sink.view())) {
            INFO("key '" << key << "' in: " << sink.view());
            CHECK(key != "c");
            CHECK(key != "r");
            // U= is a different placement mechanism entirely (Unicode
            // placeholders); mixing it with X=/Y= silently drops the sub-cell
            // offsets §3.2 depends on.
            CHECK(key != "U");
          }
        }
      }
    }
  }
}

// ── Every refusal leaves the sink untouched ─────────────────────────────────

TEST_CASE("a refused placement writes nothing at all", "[emit][kitty]") {
  // "Validate fully, then write" is what makes this function safe to call
  // speculatively. A half-emitted APC sequence corrupts every command after it
  // in the stream, and the corruption surfaces somewhere else entirely.
  struct Case {
    const char* name;
    Placement placement;
    CellPixelSize cell;
    EmitError expected;
  };

  std::vector<Case> cases;
  {
    auto p = good_placement();
    p.image_id = 0;
    // Kitty reads i=0 as "no id": the placement binds to nothing and simply
    // does not appear, with no error anywhere.
    cases.push_back({"zero image id", p, kReferenceCell, EmitError::ZeroImageId});
  }
  {
    auto p = good_placement();
    p.placement_id = 0;
    // Without a placement id the placement can never be targeted by a delete,
    // so §4.6's frame diff leaks placements for the rest of the session.
    cases.push_back({"zero placement id", p, kReferenceCell, EmitError::ZeroPlacementId});
  }
  {
    auto p = good_placement();
    p.band = Band::Text;
    cases.push_back({"text band", p, kReferenceCell, EmitError::BandCarriesNoImage});
  }
  {
    auto p = good_placement();
    p.band = Band::CellBackground;
    cases.push_back({"cell background band", p, kReferenceCell, EmitError::BandCarriesNoImage});
  }
  {
    auto p = good_placement();
    p.band_rank = layer::kBandStride;
    cases.push_back({"rank past the end", p, kReferenceCell, EmitError::RankOutOfRange});
  }
  {
    auto p = good_placement();
    p.band_rank = -1;
    cases.push_back({"negative rank", p, kReferenceCell, EmitError::RankOutOfRange});
  }
  {
    auto p = good_placement();
    p.cell_col = -1;
    // CUP is 1-based, so col -1 would emit ESC[0;0H — which is not an error to
    // a terminal, it is a valid CUP meaning row 1 column 1. A silent
    // misplacement, which is precisely why it has to be refused here.
    cases.push_back({"negative column", p, kReferenceCell, EmitError::NegativeCellOrigin});
  }
  {
    auto p = good_placement();
    p.cell_row = -1;
    cases.push_back({"negative row", p, kReferenceCell, EmitError::NegativeCellOrigin});
  }
  {
    auto p = good_placement();
    p.offset_x_px = kReferenceCell.width_px;
    // An offset of exactly one cell is a cell move, not a sub-cell offset.
    cases.push_back({"offset of a whole cell", p, kReferenceCell,
                     EmitError::SubCellOffsetOutOfRange});
  }
  {
    auto p = good_placement();
    p.offset_y_px = -1;
    cases.push_back({"negative offset", p, kReferenceCell, EmitError::SubCellOffsetOutOfRange});
  }
  {
    auto p = good_placement();
    p.crop_w = 0;
    // The subtlest one. Kitty reads w=0 as "to the right edge of the image" — a
    // DIFFERENT MEANING, not a no-op — so a zero-size crop silently becomes a
    // full-image draw. For a 480x360 plate that is the whole viewport appearing
    // where a sliver was meant to.
    cases.push_back({"zero crop width", p, kReferenceCell, EmitError::EmptyCrop});
  }
  {
    auto p = good_placement();
    p.crop_h = 0;
    cases.push_back({"zero crop height", p, kReferenceCell, EmitError::EmptyCrop});
  }
  {
    auto p = good_placement();
    p.crop_x = -1;
    cases.push_back({"negative crop origin", p, kReferenceCell, EmitError::NegativeCrop});
  }
  {
    auto p = good_placement();
    p.crop_w = -480;
    cases.push_back({"negative crop extent", p, kReferenceCell, EmitError::NegativeCrop});
  }
  // A degenerate cell size is a terminal that has not answered yet, not a
  // divide-by-zero — the same reading geometry::cells_to_cover takes.
  cases.push_back({"zero cell size", good_placement(), CellPixelSize{0, 0},
                   EmitError::DegenerateCellSize});
  cases.push_back({"zero cell height", good_placement(), CellPixelSize{10, 0},
                   EmitError::DegenerateCellSize});
  cases.push_back({"negative cell size", good_placement(), CellPixelSize{-10, 20},
                   EmitError::DegenerateCellSize});

  for (const auto& c : cases) {
    emit::ByteSink sink;
    // Prime the sink so "unchanged" is a real claim rather than "still zero".
    sink.write("PRIOR");
    const auto baseline_size = sink.size();
    const auto baseline_total = sink.total();

    const auto result = kitty::emit_placement(sink, c.placement, c.cell);

    INFO(c.name);
    CHECK_FALSE(static_cast<bool>(result));
    CHECK(result.error == c.expected);
    CHECK(result.bytes == 0);
    CHECK(sink.size() == baseline_size);
    CHECK(sink.total() == baseline_total);
    CHECK(sink.view() == "PRIOR");
  }
}

TEST_CASE("the boundary cases either side of each refusal are accepted", "[emit][kitty]") {
  // Every rejection above is one step outside a range; the step inside it must
  // work, or the check is off by one in the other direction.
  auto p = good_placement();

  p.offset_x_px = kReferenceCell.width_px - 1;
  p.offset_y_px = kReferenceCell.height_px - 1;
  {
    emit::ByteSink sink;
    CHECK(static_cast<bool>(kitty::emit_placement(sink, p, kReferenceCell)));
  }

  p = good_placement();
  p.cell_col = 0;
  p.cell_row = 0;
  {
    emit::ByteSink sink;
    REQUIRE(static_cast<bool>(kitty::emit_placement(sink, p, kReferenceCell)));
    // 0-based in, 1-based out.
    CHECK(sink.view().starts_with("\033[1;1H"));
  }

  p = good_placement();
  p.band_rank = layer::kBandStride - 1;
  {
    emit::ByteSink sink;
    CHECK(static_cast<bool>(kitty::emit_placement(sink, p, kReferenceCell)));
  }

  p = good_placement();
  p.crop_w = 1;
  p.crop_h = 1;
  {
    emit::ByteSink sink;
    CHECK(static_cast<bool>(kitty::emit_placement(sink, p, kReferenceCell)));
  }
}

// ── Formatting under extreme values ─────────────────────────────────────────

TEST_CASE("extreme coordinates are emitted digit for digit", "[emit][kitty]") {
  // The classic std::to_chars bug is an undersized buffer, and it is only ever
  // caught by the widest value the type can hold — never by a typical one.
  emit::ByteSink sink;
  auto p = good_placement();
  p.image_id = std::numeric_limits<std::uint32_t>::max();
  p.placement_id = std::numeric_limits<std::uint32_t>::max();
  p.cell_col = std::numeric_limits<std::int32_t>::max() - 1;
  p.cell_row = std::numeric_limits<std::int32_t>::max() - 1;
  p.crop_w = std::numeric_limits<std::int32_t>::max();
  p.crop_h = std::numeric_limits<std::int32_t>::max();

  REQUIRE(static_cast<bool>(kitty::emit_placement(sink, p, kReferenceCell)));
  const auto bytes = sink.view();
  INFO(bytes);
  CHECK(bytes.find("i=4294967295") != std::string_view::npos);
  CHECK(bytes.find("p=4294967295") != std::string_view::npos);
  CHECK(bytes.find("w=2147483647") != std::string_view::npos);
  CHECK(bytes.find("h=2147483647") != std::string_view::npos);
  // cell_col is INT32_MAX - 1, emitted 1-based, so exactly INT32_MAX.
  CHECK(bytes.find("\033[2147483647;2147483647H") != std::string_view::npos);
}

TEST_CASE("a below-background placement emits its negative z in full", "[emit][kitty]") {
  emit::ByteSink sink;
  auto p = good_placement();
  p.band = Band::BelowBackground;
  p.band_rank = 0;

  REQUIRE(static_cast<bool>(kitty::emit_placement(sink, p, kReferenceCell)));
  INFO(sink.view());
  CHECK(sink.view().find("z=-1073741825") != std::string_view::npos);
}

// ── Structural integrity of the byte stream ─────────────────────────────────

TEST_CASE("one placement is exactly one well-formed APC sequence", "[emit][kitty]") {
  emit::ByteSink sink;
  REQUIRE(static_cast<bool>(kitty::emit_placement(sink, good_placement(), kReferenceCell)));
  const auto bytes = sink.view();

  CHECK(count_of(bytes, "\033_G") == 1);
  CHECK(count_of(bytes, "\033\\") == 1);

  // No stray ESC inside the APC body. An escape in the payload terminates the
  // sequence early and the remainder of the command lands on screen as text.
  const auto body_start = bytes.find("\033_G") + 3;
  const auto body_end = bytes.find("\033\\");
  REQUIRE(body_end > body_start);
  const auto body = bytes.substr(body_start, body_end - body_start);
  INFO("body: " << body);
  CHECK(body.find('\033') == std::string_view::npos);

  // The cursor move precedes the APC, as one atomic string.
  CHECK(bytes.starts_with("\033["));
  CHECK(bytes.find('H') < body_start);
}

TEST_CASE("emitting the same placement twice produces identical bytes", "[emit][kitty]") {
  // §12's replay comparison rests on byte-for-byte reproducibility, so a
  // nondeterministic emitter would make a golden replay fail for a reason that
  // has nothing to do with the simulation.
  emit::ByteSink a;
  emit::ByteSink b;
  const auto p = good_placement();
  REQUIRE(static_cast<bool>(kitty::emit_placement(a, p, kReferenceCell)));
  REQUIRE(static_cast<bool>(kitty::emit_placement(b, p, kReferenceCell)));
  CHECK(a.view() == b.view());

  emit::ByteSink twice;
  const auto first = kitty::emit_placement(twice, p, kReferenceCell);
  const auto second = kitty::emit_placement(twice, p, kReferenceCell);
  REQUIRE(static_cast<bool>(first));
  REQUIRE(static_cast<bool>(second));
  CHECK(first.bytes == second.bytes);
  CHECK(twice.total() == first.bytes + second.bytes);
  CHECK(twice.size() == twice.total());
}

// ── Deletes (§4.6's frame diff is place AND delete) ─────────────────────────

TEST_CASE("deletes refuse a zero id and write nothing", "[emit][kitty]") {
  emit::ByteSink sink;
  sink.write("PRIOR");

  auto r = kitty::emit_delete_placement(sink, 0, 7);
  CHECK(r.error == EmitError::ZeroImageId);
  CHECK(r.bytes == 0);

  r = kitty::emit_delete_placement(sink, 42, 0);
  CHECK(r.error == EmitError::ZeroPlacementId);
  CHECK(r.bytes == 0);

  r = kitty::emit_delete_image(sink, 0);
  CHECK(r.error == EmitError::ZeroImageId);
  CHECK(r.bytes == 0);

  CHECK(sink.view() == "PRIOR");
  CHECK(sink.total() == 5);
}

TEST_CASE("no delete this module can emit is a delete-all", "[emit][kitty]") {
  // Kitty's d=A form wipes every image the terminal holds. Emitting it would
  // evict the resident plate set mid-session and force a full re-upload,
  // blowing §11's 1.2 MB cold-start payload budget in the middle of a game —
  // the exact failure §4.8 and GL-A1 exist to prevent.
  emit::ByteSink sink;
  REQUIRE(static_cast<bool>(kitty::emit_delete_placement(sink, 42, 7)));
  REQUIRE(static_cast<bool>(kitty::emit_delete_image(sink, 42)));

  const auto bytes = sink.view();
  INFO(bytes);
  CHECK(bytes.find("d=A") == std::string_view::npos);
  CHECK(bytes.find("d=a") == std::string_view::npos);
  CHECK(count_of(bytes, "d=i") == 1);
  CHECK(count_of(bytes, "d=I") == 1);
}

// ── §11 composition: the sink reports, the budget judges ────────────────────

TEST_CASE("a single placement sits well inside §11's animation-frame budget", "[emit][kitty]") {
  emit::ByteSink sink;
  REQUIRE(static_cast<bool>(kitty::emit_placement(sink, good_placement(), kReferenceCell)));

  INFO("one placement is " << sink.size() << " B against an animation budget of "
                           << budget::kMaxAnimationFrameBytes << " B and a recomposition budget of "
                           << budget::kMaxRecompositionBytes << " B");
  CHECK(sink.size() < budget::kMaxAnimationFrameBytes);

  // §4.2's twelve wall slots plus floor and ceiling bands is the shape of a
  // full recomposition. It has to fit in 2 KB or emit-on-change buys nothing.
  emit::ByteSink frame;
  for (int slot = 0; slot < 24; ++slot) {
    auto p = good_placement();
    p.placement_id = static_cast<std::uint32_t>(slot + 1);
    p.band_rank = slot % layer::kBandStride;
    REQUIRE(static_cast<bool>(kitty::emit_placement(frame, p, kReferenceCell)));
  }
  INFO("24 placements is " << frame.size() << " B against " << budget::kMaxRecompositionBytes);
  CHECK(frame.size() <= budget::kMaxRecompositionBytes);
}

// ── The golden literal. Last, and least interesting. ────────────────────────

TEST_CASE("the placement command reads back byte for byte", "[emit][kitty]") {
  // A lock on key ORDER as much as on content: §12's replay comparison needs a
  // reordered key set to be a test failure rather than a diff nobody reads.
  emit::ByteSink sink;
  REQUIRE(static_cast<bool>(kitty::emit_placement(sink, good_placement(), kReferenceCell)));

  CHECK(sink.view() ==
        "\033[6;4H\033_Ga=p,i=42,p=7,x=0,y=0,w=480,h=360,X=4,Y=9,z=-67,C=1,q=2\033\\");

  // z=-67 is Geometry rank 2 through the layer API, and nothing else.
  CHECK(layer::image_z(Band::Geometry, 2) == -67);
}

TEST_CASE("the delete commands read back byte for byte", "[emit][kitty]") {
  emit::ByteSink placement_delete;
  REQUIRE(static_cast<bool>(kitty::emit_delete_placement(placement_delete, 42, 7)));
  CHECK(placement_delete.view() == "\033_Ga=d,d=i,i=42,p=7,q=2\033\\");

  emit::ByteSink image_delete;
  REQUIRE(static_cast<bool>(kitty::emit_delete_image(image_delete, 42)));
  CHECK(image_delete.view() == "\033_Ga=d,d=I,i=42,q=2\033\\");
}
