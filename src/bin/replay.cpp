/// SPEC §12, §19 step 7 — the golden-replay harness, out of process.
///
/// §19 step 7's acceptance criterion is "a recorded session replays to an
/// identical world hash on both compilers". `test/13replay/` asserts that
/// in process; this binary is what asserts it ACROSS processes, and what lets a
/// bug report arrive as a file rather than as a description.
///
/// A SEPARATE EXECUTABLE, on `src/bin/bake.cpp`'s argument: the game must not be
/// able to record or replay through a flag, or "the simulation cannot reach a
/// file descriptor" becomes a convention rather than a fact.
///
/// THIS FILE HOLDS THE ONLY `read` AND `write` IN THE REPLAY PATH. Everything
/// in `gloam::replay` and `gloam::world` works on caller-owned spans; the
/// buffers live here, in `main`.
///
/// WHAT IT RECORDS TODAY: one scripted session over M0's corridor. There is no
/// input device to record from — the terminal layer is blocked upstream — so
/// the session is synthesised. `record` and `play` are still the two halves
/// that have to agree, and they are separated by a file and a process boundary,
/// which is the property the gate needs.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <system_error>
#include <string>
#include <vector>

// By name, because `world.hpp` only forward-declares `audio::Sink` — see the
// umbrella note in `gloam.hpp`. This binary constructs a real sink, so it needs
// the definition.
#include "gloam/audio.hpp"
#include "gloam/gloam.hpp"
#include "gloam/replay.hpp"
#include "gloam/world.hpp"

namespace {

constexpr auto kDefaultOutput = "replay.gloam";

auto usage() -> void {
  std::cout << "usage: gloam_replay record [-o PATH] [--quiet] [--audio]\n"
            << "       gloam_replay play PATH [--quiet] [--audio]\n"
            << "       gloam_replay verify PATH\n"
            << "\n"
            << "Records and replays a GLOAM session (SPEC §12). A replay recorded\n"
            << "against different tuning is rejected at load, never mis-played.\n"
            << "\n"
            << "  record         write a scripted session and its final world hash\n"
            << "  play PATH      replay PATH and check it reaches the hash it carries\n"
            << "  verify PATH    check the container only; do not simulate\n"
            << "  -o PATH        write the replay here (default: " << kDefaultOutput << ")\n"
            << "  --audio        attach a §9 voice sink; report its counters on stderr\n"
            << "  --mute         no voice sink at all (the default)\n"
            << "  --quiet        print only the final world hash\n"
            << "  --version      print the version and exit\n";
}

/// Run a session with a §9 sink attached and report what it heard, on stderr.
///
/// THIS BINARY NEVER LINKS RtAudio, AND THAT IS DELIBERATE. `gloam::audio`'s
/// ring and mix are standard library, so a device-free sink costs nothing and
/// needs nothing; the RtAudio stream belongs to `src/bin/main.cpp` for the same
/// reason `record` and `play` live here rather than behind a flag on the game.
/// Putting a device in the determinism harness would be exactly the mistake this
/// file's header argues against one paragraph up.
///
/// Counters go to STDERR because stdout is the world hash and nothing else —
/// `check_replay_determinism.cmake` and `check_audio_mute.cmake` both compare it
/// verbatim.
struct AudioRun {
  gloam::audio::RecordingSink<> sink{};
  std::uint64_t footfalls{0};
  std::uint64_t stings{0};
  std::uint64_t monster_footfalls{0};

  /// Drain what the simulation queued, as the audio callback would.
  ///
  /// ONE COUNTER PER EMITTER, and `check_audio_mute.cmake` requires each of them
  /// by name. Its own comment records why: killing the footfall path entirely
  /// once left that gate green on the sting alone. A third emitter that reported
  /// into a shared total would inherit the same hole.
  auto drain() -> void {
    gloam::audio::Command c{};
    while (sink.ring().try_pop(c)) {
      if (c.sound == gloam::audio::SoundId::PartyFootfall) ++footfalls;
      if (c.sound == gloam::audio::SoundId::HuntingSting) ++stings;
      if (c.sound == gloam::audio::SoundId::MonsterFootfall) ++monster_footfalls;
    }
  }

  auto report(const char* what) -> void {
    drain();
    std::cerr << "gloam_replay: " << what << " voices=" << sink.ring().pushed()
              << " dropped=" << sink.ring().dropped() << " footfalls=" << footfalls
              << " stings=" << stings << " monster_footfalls=" << monster_footfalls << '\n';
  }
};

[[nodiscard]] auto hex_of(const gloam::hash::Digest& d) -> std::string {
  const auto h = gloam::hash::to_hex(d);
  return std::string(h.data(), h.size());
}

/// M0's corridor, with an alcove and two monsters. The same shape
/// `test/13replay/` builds — deliberately duplicated rather than shared,
/// because a scenario the gate and the suite both read from one definition can
/// drift into agreeing with itself about the wrong thing.
[[nodiscard]] auto corridor_world() -> gloam::World {
  using namespace gloam;

  Level level{9, 3};
  level.carve(Coord{0, 1}, Dir::East, 9);
  level.carve(Coord{4, 1}, Dir::North, 2);

  // The first monster PATROLS the alcove, and that is load-bearing rather than
  // scenery. `check_audio_mute.cmake` requires a non-zero `monster_footfalls`
  // by name, so without a monster that actually walks, the gate fails rather
  // than passing vacuously — which is the arrangement §19 step 9 asks for.
  //
  // It also makes `check_replay_determinism.cmake` measure something new for
  // free: this is the first scripted session in which a tick DRAWS from an RNG
  // stream, so the two recording processes now have to agree about `Stream::
  // Patrol` as well as about the world.
  //
  // IT SOUNDS ONCE, AND THAT IS THE BEHAVIOUR RATHER THAN A THIN SCENARIO.
  // This session walks a lit party to within two cells of the alcove, so the
  // monster escalates almost immediately — and a monster at SEARCHING or above
  // holds position (gloam#32). One step, then it stops patrolling and starts
  // paying attention to you. Lengthening the route or removing the pause does
  // not change that; only §6.1's timers running back down to UNAWARE would, and
  // those are 48 ticks against this session's 10.
  //
  // THE PAUSE IS ON WAYPOINT 1, NOT WAYPOINT 0, AND THAT IS NOT COSMETIC. The
  // pump owes a dwell on ARRIVING at a waypoint, so the dwell of the cell a
  // monster is spawned standing on is never read — it has not arrived there,
  // it started there. With the pause on waypoint 0 this session took zero draws
  // from `Stream::Patrol`, and the claim above about carrying an RNG draw
  // across a process boundary was quietly false. Measured: the stream's state
  // was bit-identical before and after the whole session.
  Monster alcove{};
  alcove.at = Coord{4, 0};
  alcove.kind = MonsterKind{Acuity::Keen, false};
  alcove.patrol.route = {Coord{4, 0}, Coord{4, 1}};
  alcove.patrol.dwell = {0, 1};

  std::vector<Monster> monsters{
      Monster{Coord{7, 1}, MonsterKind{Acuity::Normal, false}, {}},
      alcove,
  };

  auto w = make_world(0xDEADBEEF12345678ULL, std::move(level), std::move(monsters));
  w.party = Coord{1, 1};
  w.armour = Armour::Leather;
  return w;
}

[[nodiscard]] auto scripted_session() -> std::vector<gloam::replay::Record> {
  using gloam::replay::Event;
  return {
      {0, Event::Lamp, 3},  {1, Event::Step, 1}, {2, Event::Step, 1}, {3, Event::Creep, 1},
      {3, Event::Turn, 2},  {5, Event::Wait, 0}, {8, Event::Lamp, 0}, {9, Event::Step, 3},
  };
}

/// Read a whole file into a buffer, refusing everything that is not one.
///
/// THE REGULAR-FILE CHECK IS NOT BELT-AND-BRACES, IT IS THE LOAD-BEARING ONE.
/// Opening a directory with libstdc++ SUCCEEDS, and `tellg()` on it returns
/// `LLONG_MAX` rather than the -1 that unseekable streams are supposed to give
/// — so a size check alone passes it straight through to a resize of nine
/// exabytes and aborts the process on `bad_alloc`. Measured, not assumed:
/// `gloam_bake --verify .` did exactly that until this check was written, and
/// its comment claiming -1 is what sent this function down the same path.
[[nodiscard]] auto slurp(const std::string& path, std::vector<std::byte>& out) -> bool {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    // Distinguished from the check below because a typo'd path is by far the
    // likeliest failure, and "is not a regular file" sends whoever reads the
    // log hunting for a FIFO instead of a missing file.
    std::cerr << "gloam_replay: '" << path << "' does not exist\n";
    return false;
  }
  if (!std::filesystem::is_regular_file(path, ec)) {
    std::cerr << "gloam_replay: '" << path << "' is not a regular file\n";
    return false;
  }

  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) {
    std::cerr << "gloam_replay: cannot open '" << path << "'\n";
    return false;
  }

  const auto size = static_cast<std::streamoff>(in.tellg());
  if (size < 0) {
    std::cerr << "gloam_replay: cannot size '" << path << "'\n";
    return false;
  }
  if (static_cast<std::uintmax_t>(size) < gloam::replay::kHeaderBytes) {
    std::cerr << "gloam_replay: '" << path << "' is " << size
              << " B — too short to be a replay at all\n";
    return false;
  }

  in.seekg(0);
  out.resize(static_cast<std::size_t>(size));
  in.read(reinterpret_cast<char*>(out.data()), size);
  if (!in) {
    std::cerr << "gloam_replay: reading '" << path << "' failed\n";
    return false;
  }
  return true;
}

auto do_record(const std::string& output, bool quiet, bool audio) -> int {
  using namespace gloam;
  const Tuning& tuning = kDefaultTuning;

  const auto records = scripted_session();
  auto world = corridor_world();
  const auto seed = world.seed;

  // §19 step 9's acceptance criterion is that these two paths produce IDENTICAL
  // replays. Not similar, not equivalent — the same bytes and the same hash.
  AudioRun heard;
  play(world, records, tuning, audio ? &heard.sink : nullptr);
  if (audio) heard.report("recorded");

  const auto final_hash = world_hash(world);

  replay::Header header{};
  header.seed = seed;
  header.ruleset_hash = ruleset_hash(tuning);
  // No pack is loaded: there is nothing to upload plates to until the
  // compositor exists (#7), and `kNoPackHash` is how a replay says so without
  // making every load warn.
  header.pack_hash = replay::kNoPackHash;
  header.final_world_hash = final_hash;

  std::vector<std::byte> image(replay::image_bytes(static_cast<std::uint32_t>(records.size())));
  if (const auto res = replay::assemble(header, records, image); !res) {
    std::cerr << "gloam_replay: cannot assemble the replay: " << replay::describe(res.error)
              << " (record " << res.record_index << ")\n";
    return EXIT_FAILURE;
  }

  std::ofstream out(output, std::ios::binary | std::ios::trunc);
  if (!out) {
    std::cerr << "gloam_replay: cannot open '" << output << "' for writing\n";
    return EXIT_FAILURE;
  }
  out.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
  out.close();
  if (!out) {
    std::cerr << "gloam_replay: writing '" << output << "' failed\n";
    return EXIT_FAILURE;
  }

  const auto hex = hex_of(final_hash);
  if (quiet) {
    std::cout << hex << '\n';
  } else {
    std::cout << "wrote " << output << " — " << image.size() << " B, " << records.size()
              << " records, " << world.tick << " ticks\n"
              << "final world hash " << hex << '\n';
  }
  return EXIT_SUCCESS;
}

auto do_play(const std::string& path, bool quiet, bool audio) -> int {
  using namespace gloam;
  const Tuning& tuning = kDefaultTuning;

  std::vector<std::byte> image;
  if (!slurp(path, image)) return EXIT_FAILURE;

  replay::Header probe{};
  if (const auto res = replay::read_header(image, probe); !res) {
    std::cerr << "gloam_replay: '" << path << "': " << replay::describe(res.error) << '\n';
    return EXIT_FAILURE;
  }

  std::vector<replay::Record> records(probe.record_count);
  replay::Header header{};
  const replay::Expect expect{ruleset_hash(tuning), replay::kNoPackHash};
  const auto res = replay::load(image, expect, header, records);
  if (!res) {
    std::cerr << "gloam_replay: '" << path << "': " << replay::describe(res.error);
    if (res.record_index != 0) std::cerr << " (record " << res.record_index << ")";
    std::cerr << '\n';
    return EXIT_FAILURE;
  }

  // SCHEMAS.md §3: art changes do not affect simulation. Advisory, on stderr,
  // and the replay runs anyway.
  if (res.pack_hash_mismatch) {
    std::cerr << "gloam_replay: warning: '" << path
              << "' was recorded against a different asset pack; art does not affect the "
                 "simulation, so this is advisory\n";
  }

  auto world = corridor_world();

  // THE WORLD IS NOT IN THE FILE YET, SO REFUSE THE ONES WE CANNOT BUILD.
  //
  // `replay.gloam` carries the seed but not the level or the monster roster —
  // there is no `level.gloam` reader, so `play` can only ever run against the
  // scenario compiled in above. Left unchecked, a replay recorded from any
  // other world loads cleanly, replays against the wrong one, reaches a
  // different hash, and gets reported as `WorldHashMismatch`: "a regression,
  // not a flake". The harness would be diagnosing a determinism bug for a file
  // it simply refused to set up correctly — the exact misdiagnosis §12's
  // ruleset gate exists to prevent, one field over. Better to say plainly that
  // this build cannot construct that world.
  if (header.seed != world.seed) {
    std::cerr << "gloam_replay: '" << path << "' was recorded against seed 0x" << std::hex
              << header.seed << ", and this build can only reconstruct 0x" << world.seed << std::dec
              << ".\nThe replay format does not carry the level or the monster roster yet, so "
                 "there is no world to replay it against. This is a limitation, NOT a "
                 "determinism regression.\n";
    return EXIT_FAILURE;
  }

  AudioRun heard;
  play(world, records, tuning, audio ? &heard.sink : nullptr);
  if (audio) heard.report("replayed");

  const auto reached = world_hash(world);

  const auto hex = hex_of(reached);
  if (quiet) {
    std::cout << hex << '\n';
  } else {
    std::cout << "replayed " << records.size() << " records over " << world.tick << " ticks\n"
              << "final world hash " << hex << '\n';
  }

  if (reached != header.final_world_hash) {
    std::cerr << "gloam_replay: " << replay::describe(replay::ReplayError::WorldHashMismatch)
              << "\n  expected " << hex_of(header.final_world_hash) << "\n  reached  " << hex
              << "\nA nondeterministic simulation has no correct behaviour to fall back to "
                 "(TEST-PLAN.md §2): this is a regression, not a flake.\n";
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}

auto do_verify(const std::string& path) -> int {
  std::vector<std::byte> image;
  if (!slurp(path, image)) return EXIT_FAILURE;

  const auto res = gloam::replay::verify(image);
  if (!res) {
    std::cerr << "gloam_replay: '" << path << "': " << gloam::replay::describe(res.error);
    if (res.record_index != 0) std::cerr << " (record " << res.record_index << ")";
    std::cerr << '\n';
    return EXIT_FAILURE;
  }

  gloam::replay::Header header{};
  static_cast<void>(gloam::replay::read_header(image, header));
  std::cout << path << ": intact — " << image.size() << " B, " << header.record_count
            << " records, tick_hz " << header.tick_hz << '\n'
            << "  final world hash " << hex_of(header.final_world_hash) << '\n';
  return EXIT_SUCCESS;
}

}  // namespace

auto main(int argc, char** argv) -> int {
  std::string mode;
  std::string path;
  std::string output = kDefaultOutput;
  bool quiet = false;
  bool audio = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];

    if (arg == "--help" || arg == "-h") {
      usage();
      return EXIT_SUCCESS;
    }
    if (arg == "--version") {
      std::cout << gloam::version_string() << '\n';
      return EXIT_SUCCESS;
    }
    if (arg == "--quiet") {
      quiet = true;
      continue;
    }
    if (arg == "--audio") {
      audio = true;
      continue;
    }
    // Accepted and does nothing, because nothing is what it means: `--mute` is
    // the default. Spelled out rather than rejected so that the two halves of
    // §19 step 9's criterion can be written as a symmetric pair of commands —
    // `--mute` against `--audio` — instead of a flag against its own absence.
    if (arg == "--mute") {
      audio = false;
      continue;
    }
    if (arg == "-o") {
      if (i + 1 >= argc) {
        std::cerr << "gloam_replay: " << arg << " needs a path\n";
        return EXIT_FAILURE;
      }
      output = argv[++i];
      continue;
    }
    if (arg == "record" || arg == "play" || arg == "verify") {
      if (!mode.empty()) {
        std::cerr << "gloam_replay: '" << mode << "' and '" << arg
                  << "' are two modes; pick one\n";
        return EXIT_FAILURE;
      }
      mode = arg;
      continue;
    }
    if (!arg.empty() && arg.front() == '-') {
      std::cerr << "gloam_replay: unrecognised argument '" << arg << "'\n";
      usage();
      return EXIT_FAILURE;
    }
    if (!path.empty()) {
      std::cerr << "gloam_replay: '" << path << "' and '" << arg
                << "' are two paths; pick one\n";
      return EXIT_FAILURE;
    }
    path = arg;
  }

  if (mode.empty()) {
    std::cerr << "gloam_replay: no mode given\n";
    usage();
    return EXIT_FAILURE;
  }
  if (mode == "record") return do_record(output, quiet, audio);

  if (path.empty()) {
    std::cerr << "gloam_replay: " << mode << " needs a path\n";
    return EXIT_FAILURE;
  }
  if (mode == "play") return do_play(path, quiet, audio);
  return do_verify(path);
}
