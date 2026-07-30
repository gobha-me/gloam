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
// Standard library only. This binary will grow a termforge dependency; it must
// not grow one that reaches back into gloam::lib's determinism.

#include <cstdio>
#include <string_view>

#include "gloam/gloam.hpp"

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

}  // namespace

auto main(int argc, char** argv) -> int {
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
  if (!quiet) trace_corridor();
  return 0;
}
