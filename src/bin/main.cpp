// GLOAM — headless diagnostic entry point.
//
// There is no game here yet, and there deliberately is not: SPEC §19 puts the
// layer API, the lifecycle tests and the byte instruments before any game
// logic, and the terminal layer is blocked on upstream termforge work (see
// UPSTREAM.md). What this binary does instead is the part of §19 step 4 that
// can exist today — print the frozen constants and the ruleset hash, and run
// M0's corridor through the real perception model so the numbers in §6 can be
// read rather than trusted.
//
// Standard library only, apart from the one POSIX write in tty_writer.hpp. This
// binary will grow a termforge dependency; it must not grow one that reaches
// back into gloam::lib's determinism.

#include <signal.h>
#include <unistd.h>  // STDOUT_FILENO — do not lean on libstdc++ leaking it

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "gloam/budgets.hpp"
#include "gloam/deflate.hpp"
#include "gloam/emit.hpp"
#include "gloam/geometry.hpp"
#include "gloam/gloam.hpp"
#include "gloam/kitty.hpp"
#include "gloam/layer.hpp"
#include "gloam/lightfield.hpp"
#include "gloam/meter.hpp"
#include "gloam/plate.hpp"
#include "gloam/png.hpp"
#include "gloam/replay.hpp"
#include "tty_writer.hpp"

namespace {

using namespace gloam;

void print_geometry() {
  std::printf("geometry (SPEC 3, frozen)\n");
  std::printf("  viewport      %d x %d px, %d x %d cells at the reference cell\n",
              geometry::kViewportWidthPx, geometry::kViewportHeightPx, geometry::kViewport.cols(),
              geometry::kViewport.rows());
  std::printf("  reference     %d x %d grid at %d x %d px per cell\n", geometry::kReferenceCols,
              geometry::kReferenceRows, geometry::kReferenceCellWidthPx,
              geometry::kReferenceCellHeightPx);
  std::printf("  depth ladder  ");
  for (int d = 0; d < geometry::kDepthCount; ++d) {
    std::printf("%d:%dx%d%s", d, geometry::kDepths[d].width, geometry::kDepths[d].height,
                d + 1 < geometry::kDepthCount ? "  " : "\n");
  }
  std::printf("  sight         %d cells; depth %d is a cap plate, not a navigable ring\n",
              geometry::kSightDistance, geometry::kDepthCount - 1);
}

void print_ruleset() {
  const auto hash = ruleset_hash(kDefaultTuning);
  std::printf("\nruleset (SPEC 6, 8)\n");
  std::printf("  tunables      %zu integers, no floats\n", kTuningFieldCount);
  std::printf("  ruleset_hash  0x%016llx\n", static_cast<unsigned long long>(hash));
  std::printf("                a replay recorded under a different hash is rejected at load\n");
}

/// What the cold-start upload actually costs, measured rather than quoted.
struct ColdStart {
  std::size_t stream_bytes{0};
  std::size_t plates{0};
  long elapsed_ms{0};
};

/// Bake, encode and transmit the whole resident plate set into a sink.
///
/// The bytes go nowhere — this is the upload path run for its size and its cost,
/// not an upload. The real one belongs to the compositor's startup (gloam#7,
/// upstream #97's `on_start`), and doing it here would put a plate on a terminal
/// that has not asked for one.
///
/// A clock is read, which nothing in `gloam::lib` may do — which is exactly why
/// this function is in `src/bin/` and the thing it measures is not.
[[nodiscard]] auto measure_cold_start() -> ColdStart {
  const auto w = lightfield::kWidthPx;
  const auto h = lightfield::kHeightPx;

  std::vector<std::byte> blob(plate::blob_bytes(w, h));
  std::vector<std::byte> scanlines(png::scratch_bytes(w, h));
  std::vector<std::byte> encoded(png::bound(w, h));
  // A quarter of a megabyte of match tables, on the heap and owned here: the
  // library never allocates one, and a local would be a quarter-megabyte stack
  // frame.
  const auto matcher = std::make_unique<deflate::Scratch>();
  emit::ByteSink sink;

  const auto started = std::chrono::steady_clock::now();

  ColdStart out;
  for (int level = kLampLevelMin; level <= kLampLevelMax; ++level) {
    if (!lightfield::bake(level, blob)) return out;
    const auto image = png::encode(plate::PlateView{blob, w, h}, scanlines, *matcher, encoded);
    if (!image) return out;
    const auto id = static_cast<std::uint32_t>(level - kLampLevelMin) + 1;
    if (!kitty::emit_transmit(sink, std::span{encoded}.first(image.bytes), id)) return out;
    ++out.plates;
  }

  out.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - started)
                       .count();
  out.stream_bytes = sink.size();
  return out;
}

/// Build-order step 4: "Counters on the emit path and the upload path, printed
/// every run."
///
/// Printed through gloam::tty::write_all rather than printf, because this is the
/// process's ONE write syscall and the byte figure it reports has to be a real
/// measurement rather than a decoration. AGENTS.md has promised for two releases
/// that "the write that puts those bytes on a terminal stays in src/bin/, and
/// that one line is the whole terminal-facing surface"; this is that line.
///
/// It does NOT print a p95. There is no compositor and therefore no session, and
/// a shipped binary that printed a synthetic percentile would be quoted back as
/// a measurement inside a month. What it prints instead is the wire form and the
/// upload — both measured here, in this binary, by emitting a real placement and
/// running a real cold start — and the two rules that were undecided until now.
auto print_instruments() -> bool {
  // The measurement: one placement through the real emitter, at §3.2's
  // reference cell. Not a quoted constant.
  //
  // The field values are REPRESENTATIVE rather than minimal, and that is the
  // difference between a useful line and a flattering one. Every integer here
  // is `to_chars`'d, so an id of 1 at cell (0,0) is the cheapest placement that
  // can exist — measuring it would print 64 B and derive a headroom of 32, when
  // §4.2's own slot inventory measures 66.5 B and 30. A 6% optimistic figure on
  // the exact quantity gloam#26's overrun turns on is worse than no figure.
  emit::ByteSink probe;
  const kitty::Placement sample{
      .image_id = 103,
      .placement_id = 103,
      .band = layer::Band::Geometry,
      .band_rank = 0,
      .cell_col = 24,
      .cell_row = 11,
      .offset_x_px = 0,
      .offset_y_px = 0,
      .crop_x = 0,
      .crop_y = 0,
      .crop_w = geometry::kDepths[0].width,
      .crop_h = geometry::kDepths[0].height,
  };
  const auto placed = kitty::emit_placement(
      probe, sample,
      kitty::CellPixelSize{geometry::kReferenceCellWidthPx, geometry::kReferenceCellHeightPx});
  const auto placement_bytes = placed ? placed.bytes : 0;

  // Built by appending, not by snprintf into a fixed buffer. A truncating
  // snprintf produces a partial line and reports it through a return value
  // nobody checks, which is a poor property for the one block in this binary
  // whose whole claim is that its figures are measurements.
  const auto num = [](auto value) { return std::to_string(value); };

  std::string report;
  report.reserve(1024);
  report += "\ninstruments (BUDGETS.md, build-order step 4)\n";
  report += "  wire form     a representative placement is " + num(placement_bytes) +
            " B at the reference cell (measured here)\n";
  report += "  frame classes idle " + num(budget::kIdleFrameBytes) + " B / animation <= " +
            num(budget::kMaxAnimationFrameBytes) + " B / recomposition <= " +
            num(budget::kMaxRecompositionBytes) + " B\n";
  if (placement_bytes > 0) {
    report += "  headroom      " + num(budget::kMaxRecompositionBytes / placement_bytes) +
              " placements fit a recomposition, " +
              num(budget::kMaxAnimationFrameBytes / placement_bytes) + " an animation frame\n";
  }
  report +=
      "  classified on the STATE DELTA: party or facing -> recomposition;\n"
      "                lamp or awareness -> animation; else idle       (gloam#24)\n";
  report += "  sustained     p95 by nearest rank over sliding " + num(replay::kTickHz) +
            "-tick windows, <= " + num(budget::kMaxSustainedBytesPerSecond) + " B/s  (gloam#25)\n";

  // The cold-start row, measured by actually doing it. This is the half of
  // build-order step 4 that says "counters on the UPLOAD path", and until the
  // transmit path landed there was nothing to count. Six full-frame light fields
  // is the whole resident set that exists today; M0's inventory is 71 plates.
  const auto upload = measure_cold_start();
  report += "  cold start    " + num(upload.stream_bytes) + " B on the wire for " +
            num(upload.plates) + " plates in " + num(upload.elapsed_ms) + " ms, against " +
            num(budget::kMaxColdStartPayloadBytes) + " B and " +
            num(budget::kMaxColdStartLocalMs) + " ms (measured here)\n";
  report +=
      "                f=100 indexed PNG, GLOAM's own DEFLATE. The f=32 route\n"
      "                this replaces was 4.6x OVER the payload row  (gloam#17)\n";

  report +=
      "  measured      0 emit bytes this run. There is no compositor (gloam#7),\n"
      "                so no session was rendered and no p95 was computed. The\n"
      "                200-tick harness in test/10budgets/ is where that number\n"
      "                lives, and it is 3.3% OVER budget (gloam#26).\n";

  // printf and a raw write share a file descriptor and not a buffer, so the
  // sections printed above would otherwise land after this one.
  std::fflush(stdout);

  const auto produced = report.size();
  const auto result = tty::write_all(STDOUT_FILENO, report);

  // Produced against accepted, for the block above — not a running total that
  // would have to include its own line. That pair IS the distinction this whole
  // module exists to make: `ByteSink` can only ever report the left-hand number.
  const std::string tail = "  write path    " + num(produced) + " B produced, " +
                           num(result.bytes_written) +
                           " B accepted by write(2) for the block above\n";
  const auto tail_result = tty::write_all(STDOUT_FILENO, tail);

  const auto& failure = result ? tail_result : result;
  if (failure) return true;

  // To stderr, not stdout: a report that the output stream failed has no
  // business being sent down the output stream. `gloam > /dev/full` is the case
  // that makes the difference visible.
  std::fprintf(stderr, "gloam: instrument report stopped early after %zu B (error %d, errno %d)\n",
               failure.bytes_written, static_cast<int>(failure.error), failure.errno_value);

  // A reader that went away is not this process failing. `gloam | head -1` closes
  // the pipe on purpose, and exiting non-zero there would make an ordinary shell
  // idiom look like a fault. Anything else — a full disk, a bad fd, a stalled
  // write — means build-order step 4's "printed every run" did not happen, and
  // a caller checking $? has to be able to tell.
  return failure.error == tty::WriteError::Closed;
}

/// M0's corridor: four cells long, one intersection (SPEC 15).
auto build_corridor() -> Level {
  Level level{7, 5};
  // The main run, west to east along row 2.
  level.carve(Coord{1, 2}, Dir::East, 5);
  // The intersection: a side passage crossing it at the middle.
  level.carve(Coord{3, 2}, Dir::North, 3);
  level.carve(Coord{3, 2}, Dir::South, 3);
  return level;
}

void trace_corridor() {
  const Level level = build_corridor();
  const Tuning& t = kDefaultTuning;

  std::printf("\nM0 corridor - the perception model, read from the party's cell\n");
  std::printf("  graph symmetric: %s\n", level.symmetric() ? "yes" : "NO - edges disagree");

  const Coord party{1, 2};
  const Coord monster{5, 2};
  const MonsterKind kind{Acuity::Normal, /*sees_unlit=*/false};

  std::printf("\n  party at (%d,%d), monster at (%d,%d), %d cells apart\n", party.x, party.y,
              monster.x, monster.y, range_between(party, monster));
  std::printf("  line of sight: %s\n\n", line_of_sight(level, party, monster) ? "clear" : "broken");

  std::printf("  %-10s %-7s %-11s %s\n", "armour", "emits", "at monster", "heard?");
  const struct {
    const char* name;
    Armour armour;
  } kLoadouts[] = {{"unarmoured", Armour::None},
                   {"leather", Armour::Leather},
                   {"mail", Armour::Mail},
                   {"plate", Armour::Plate}};

  for (const auto& loadout : kLoadouts) {
    const auto emitted = step_noise(loadout.armour, /*creeping=*/false, t);
    const auto field = propagate_noise(level, party, emitted, t);
    const auto arriving = field.at(level, monster);
    const bool audible = hears(field, level, monster, kind.acuity, /*hunting=*/false, t);
    std::printf("  %-10s %-7d %-11d %s\n", loadout.name, emitted, arriving, audible ? "yes" : "no");
  }

  std::printf("\n  SPEC 6.2's coupling, in one table: plate is protection bought with audibility.\n");

  // 6.3 - the pillar, shown rather than asserted. The same lamp_level that
  // picks the light-field plate decides whether the party can be seen at all.
  std::printf("\n  lamp   visible to a sees_unlit=false monster at range %d?\n",
              range_between(party, monster));
  const bool los = line_of_sight(level, party, monster);
  const auto range = range_between(party, monster);
  for (int lamp = kLampLevelMax; lamp >= kLampLevelMin; --lamp) {
    const bool seen =
        party_visible(lamp, kind.sees_unlit, los, range, t.sight_distance(kind.acuity));
    std::printf("  %-6d %s\n", lamp, seen ? "seen" : "unseen - navigate from memory and sound");
  }
}

/// SPEC 6, first sentence, as a trace: "you glimpse a monster crossing a distant
/// intersection, left to right, and you do not know whether it registered you."
///
/// This is the one block in the binary that ticks a real World rather than
/// calling the model's pieces by hand. Everything above reads a static
/// arrangement; 6.4's pump is the first thing in the tree with a schedule, and a
/// schedule has to be watched to be judged.
///
/// It is also the closest the project gets to answering gloam#8 without a
/// renderer. The gate asks whether the crossing produces tension, and that
/// question needs pixels - but "is the monster where the model says, when the
/// model says, and does it know about me" is answerable here and is the half
/// that has to be right before the pixels can help.
/// True when this monster is standing on a cell of its own patrol route.
[[nodiscard]] auto on_route(const Monster& m) -> bool {
  return std::find(m.patrol.route.begin(), m.patrol.route.end(), m.at) != m.patrol.route.end();
}

void trace_patrol() {
  const Tuning& t = kDefaultTuning;

  // The monster walks the side passage: (3,0) south to (3,4) and back, crossing
  // the intersection at (3,2). The party stands west along row 2 and watches.
  Monster m{};
  m.at = Coord{3, 0};
  m.kind = MonsterKind{Acuity::Normal, /*sees_unlit=*/false};
  m.patrol.route = {Coord{3, 0}, Coord{3, 1}, Coord{3, 2}, Coord{3, 3}, Coord{3, 4}};
  m.patrol.dwell = {1, 0, 0, 0, 1};  // a pause at each end, so 6.4's jitter draws

  auto world = make_world(0x6104A3ULL, build_corridor(), {m});
  world.party = Coord{1, 2};
  world.facing = Dir::East;
  world.lamp_level = kLampLevelMin;  // doused: you are watching from the dark

  // The lamp comes up here, and that is the whole point of the trace. Doused,
  // the monster patrols past you unaware; lit, it registers you. One integer,
  // both behaviours - which is SPEC 6.3's pillar and 4.4's plate selector being
  // the same number.
  // THE LAMP GOES UP, AND THEN IT GOES OUT AGAIN, which is the whole cycle
  // rather than half of it. Lit, the thing notices you and comes; doused, it
  // loses you, gives up, and walks back to the route it left. Every one of
  // those is a SPEC 6.1 tell, and until gloam#32 the trace stopped at the
  // first.
  constexpr std::uint32_t kLampUpAtTick = 14;
  constexpr std::uint32_t kLampOutAtTick = 26;
  // AND THEN YOU WALK AWAY, because in a four-cell corridor dousing alone does
  // not save you: SPEC 6.3 keeps an unlit party visible at an ADJACENT cell, so
  // a monster already at arm's reach goes on seeing you with the lamp out. That
  // is the mechanic working, and it is worth seeing in the table. Getting clear
  // is what actually breaks the contact - and a leather step at 14 is below a
  // normal monster's hearing threshold of 40, so walking away is silent.
  constexpr std::uint32_t kFirstRetreatTick = 28;
  constexpr int kRetreatSteps = 4;
  constexpr int kTicks = 95;

  std::printf("\nM0 corridor - the patrol, ticked (SPEC 6.4)\n");
  std::printf("  party at (%d,%d) facing east; monster patrols (3,0)..(3,4) through\n",
              world.party.x, world.party.y);
  std::printf("  the intersection at (3,2), one step every %d ticks. You start doused,\n",
              t.monster_move_ticks);
  std::printf("  light the lamp at tick %u, douse it at tick %u, and then back away\n",
              kLampUpAtTick, kLampOutAtTick);
  std::printf("  east from tick %u.\n\n", kFirstRetreatTick);

  std::printf("  %-5s %-8s %-7s %-12s %-5s %-8s %s\n", "tick", "monster", "facing", "awareness",
              "lamp", "you", "sees you");

  static const char* kFacings[] = {"north", "east", "south", "west"};
  static const char* kStates[] = {"unaware", "suspicious", "searching", "hunting", "lost-track"};

  auto previous_state = world.monsters[0].mind.state;
  auto previous_at = world.monsters[0].at;
  auto previous_party = world.party;

  for (int tick = 0; tick < kTicks; ++tick) {
    if (world.tick == kLampUpAtTick) apply(world, replay::Event::Lamp, 3, t);
    if (world.tick == kLampOutAtTick) apply(world, replay::Event::Lamp, 0, t);
    for (int r = 0; r < kRetreatSteps; ++r) {
      if (world.tick == kFirstRetreatTick + static_cast<std::uint32_t>(r) * 2) {
        apply(world, replay::Event::Step, static_cast<std::uint16_t>(Dir::East), t);
      }
    }
    advance(world, t);
    const auto& mon = world.monsters[0];

    // Read the same way the simulation does, so this table cannot disagree with
    // the tick that produced it.
    const bool los = line_of_sight(world.level, mon.at, world.party);
    const bool seen = party_visible(world.lamp_level, mon.kind.sees_unlit, los,
                                    range_between(mon.at, world.party),
                                    t.sight_distance(mon.kind.acuity));

    // ONE LINE PER EVENT, NOT PER TICK. Ninety-six rows of a monster standing
    // still is a table nobody reads to the end; the interesting ticks are the
    // ones where it changed its mind or its cell.
    const bool interesting = mon.mind.state != previous_state || mon.at != previous_at ||
                             world.tick == kLampUpAtTick + 1 || world.tick == kLampOutAtTick + 1 ||
                             world.party != previous_party;
    previous_state = mon.mind.state;
    previous_at = mon.at;
    previous_party = world.party;
    if (!interesting) continue;

    const char* note = "";
    if (mon.at == Coord{3, 2}) note = "   <- crossing";
    if (!on_route(mon)) note = "   <- off its route";

    std::printf("  %-5u (%d,%d)    %-7s %-12s %-5d (%d,%d)    %s%s\n", world.tick, mon.at.x,
                mon.at.y, kFacings[static_cast<int>(mon.facing)],
                kStates[static_cast<int>(mon.mind.state)], world.lamp_level, world.party.x,
                world.party.y, seen ? "yes" : "no", note);
  }

  const auto& mon = world.monsters[0];
  std::printf("\n  Doused, it walks past and never knows. Lit, it leaves the route and\n"
              "  comes for you - and stops one cell short, because there is no combat\n"
              "  at M0 and a thing standing ON you is a worse dead end than a thing\n"
              "  standing next to you. Douse again and it searches where you were,\n"
              "  gives up, casts about, and walks home.\n");
  std::printf("  Ends on (%d,%d), %s, %s.\n", mon.at.x, mon.at.y,
              kStates[static_cast<int>(mon.mind.state)],
              on_route(mon) ? "back on its route" : "still off its route");
}

}  // namespace

auto main(int argc, char** argv) -> int {
  // A raw write to a closed pipe kills the process by default, and this binary
  // now has one: `gloam | head -1` would die rather than report a short write.
  // printf masks this; write(2) does not.
  ::signal(SIGPIPE, SIG_IGN);

  bool quiet = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg{argv[i]};
    if (arg == "--version") {
      std::printf("%s\n", gloam::version_string());
      return 0;
    }
    if (arg == "--quiet") {
      quiet = true;
      continue;
    }
    if (arg == "--help" || arg == "-h") {
      std::printf(
          "usage: gloam [--version] [--quiet]\n\n"
          "Headless diagnostic for the GLOAM simulation core.\n"
          "There is no playable game yet - see UPSTREAM.md.\n");
      return 0;
    }
    std::fprintf(stderr, "gloam: unrecognised argument '%s'\n", argv[i]);
    return 2;
  }

  std::printf("GLOAM %s - simulation core\n\n", gloam::version_string());
  print_geometry();
  print_ruleset();
  // Before the --quiet guard, on purpose: build-order step 4 says the counters
  // are "printed every run", and a flag that silences an instrument makes the
  // instrument optional.
  const bool instrumented = print_instruments();
  if (!quiet) {
    trace_corridor();
    trace_patrol();
  }

  // Non-zero when the instruments could not be printed at all. Step 4's contract
  // is that they are printed every run; a script that reads $? and sees success
  // over an empty stdout has been told something false.
  return instrumented ? 0 : 1;
}
