// GLOAM — the M0 corridor, playable (gloam#8).
//
// "Play it. Toggle the pump. Answer the M0 gate." This is the binary the gate
// is answered with: one corridor, four cells and an intersection, one
// patrolling monster with the full perception model, one lamp to carry and
// douse. Everything a key press may do to the World lives in the device-free
// core (`game.hpp`); everything about the corridor's geometry lives in
// `scene.hpp`. This file owns what neither may touch: the terminal, the
// clock, the audio device and the two file descriptors (the pack in, the
// replay out).
//
// WHAT IS DELIBERATELY NOT HERE
//
//   * §4.7's 140 ms step transitions. `terminal::StepTransitions` exists and
//     is pinned by test/33terminalcompositor, but its frames are CONTENT —
//     tween plates the placeholder set does not carry — and the gate's
//     question ("does glimpsing that monster cross the intersection produce
//     genuine tension?") does not change with 140 ms of tween between two
//     placements. The movement snaps; the glimpse is the same glimpse. Wiring
//     the sequencer between the core's input path and the compositor is the
//     follow-up, with the art.
//   * A runtime mute key. §9.2's degradation story is a launch flag and a
//     device-loss path, both live below; a mid-session toggle is interface
//     chrome the gate does not ask about.
//   * §3.2's chrome. One status row, drawn as text; the party strip is M1.
//
// THE STATUS ROW IS NOT A TELL. §6.1: "The player never sees a state label."
// The row shows what the PLAYER holds — lamp, gait, pump mode, tick — and
// never anything the monster believes.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <termforge/core/app.hpp>
#include <termforge/drivers/kitty_driver.hpp>

#include "audio_device.hpp"
#include "game.hpp"
#include "gloam/gloam.hpp"
#include "gloam/pack.hpp"
#include "gloam/replay.hpp"
#include "gloam/sha256.hpp"
#include "resident_plates.hpp"
#include "scene.hpp"
#include "sfx.hpp"
#include "terminal_compositor.hpp"

namespace {

using namespace gloam;

// ─── Key translation ────────────────────────────────────────────────────────
//
// The terminal's vocabulary into the core's. Press AND Repeat both map — under
// KeyboardMode::Legacy an OS auto-repeat arrives as a second Press anyway, and
// the core's pump (latest-wins, one commit per tick) is what makes holding a
// key safe rather than a macro. Releases are ignored: a discrete-cell game has
// no use for key-up at M0.
[[nodiscard]] auto to_game_key(const termforge::KeyEvent& key) -> game::Key {
  switch (key.key) {
    case termforge::Key::Up: return game::Key::Forward;
    case termforge::Key::Down: return game::Key::Back;
    case termforge::Key::Left: return game::Key::TurnLeft;
    case termforge::Key::Right: return game::Key::TurnRight;
    case termforge::Key::Enter: return game::Key::Wait;
    case termforge::Key::Char:
      switch (key.ch) {
        case U'w': case U'k': return game::Key::Forward;
        case U's': return game::Key::Back;
        case U'a': case U'h': return game::Key::TurnLeft;
        case U'd': case U'l': return game::Key::TurnRight;
        case U',': case U'<': return game::Key::LampDown;
        case U'.': case U'>': return game::Key::LampUp;
        case U'c': return game::Key::CreepToggle;
        case U' ': return game::Key::Wait;
        default: return game::Key::Unbound;
      }
    default: return game::Key::Unbound;
  }
}

[[nodiscard]] auto pump_name(game::PumpMode mode) -> std::string_view {
  return mode == game::PumpMode::RealTime ? "real-time" : "step-timed";
}

// ─── The app ────────────────────────────────────────────────────────────────
//
// One session: pins the pack, ticks the core at 10 Hz, draws at most 30
// frames a second, and on quit hands main() a sealed Outcome. All failure
// surfaces degrade rather than crash (§9.2's rule for the device, and the
// same posture for the frame path): a bad frame is dropped and named in the
// status row, never thrown across the loop.
class M0App final : public termforge::App {
 public:
  M0App(resident::PlateSet& plates, game::Core&& core, std::uint64_t pack_hash)
      : plates_{plates}, core_{std::move(core)}, pack_hash_{pack_hash} {}

  /// §17, enforced by declaration rather than by an if: "Kitty only; refuse
  /// to start otherwise." The driver is pinned so no fallback tier is ever
  /// selected, and the requirements make a terminal that cannot show the
  /// viewport refuse startup with an ErrorEvent instead of drawing garbage.
  auto configure() -> void {
    set_builtin_driver(termforge::BuiltinDriver::Kitty);
    termforge::AppRequirements requirements{};
    requirements.graphics = true;
    requirements.known_cell_pixels = true;
    // The viewport is 48x18 cells at the reference 10x20 cell; one row below
    // it is the status line. A terminal smaller than that is not M0's medium.
    requirements.min_cols = 48;
    requirements.min_rows = 19;
    require(requirements);

    // §5.2's real-time row. The SAME schedule drives both pump modes — the
    // core's step-timed tick() is a no-op, so the mode switch never touches
    // the frame loop. That is the point of the design: only the pump differs.
    set_tick_hz(static_cast<int>(replay::kTickHz));
    set_frame_ms(33);
  }

  /// Set when the player quits (the only clean exit the loop knows); main()
  /// reads it to write the replay. finish() is idempotent, so a second quit
  /// key changes nothing.
  std::optional<game::Core::Outcome> outcome;

  /// main() needs the log to write the replay. Read-only; the loop owns it.
  [[nodiscard]] auto core() const -> const game::Core& { return core_; }

 private:
  auto on_start() -> void override {
    const auto pinned = plates_.pin_all(driver());
    if (!pinned) {
      // A pack that cannot go resident cannot be shown; refuse the session
      // rather than open on a black screen.
      frame_error_ = "pin failed — resident image capacity refused the pack";
      quit();
    }
  }

  auto on_event(const termforge::Event& event) -> void override {
    if (const auto* key = std::get_if<termforge::KeyEvent>(&event)) {
      if (key->action == termforge::KeyAction::Release) return;
      // Quit and the pump toggle are SHELL keys: they are not replay events
      // and must never reach the recording. Everything else goes to the core.
      if (key->key == termforge::Key::Escape ||
          (key->key == termforge::Key::Char && key->ch == U'q')) {
        outcome = core_.finish(pack_hash_);
        quit();
        return;
      }
      if (key->key == termforge::Key::Tab) {
        core_.set_mode(core_.mode() == game::PumpMode::RealTime
                           ? game::PumpMode::StepTimed
                           : game::PumpMode::RealTime);
        return;
      }
      core_.handle_key(to_game_key(*key));
      return;
    }
    if (std::holds_alternative<termforge::ImageInvalidatedEvent>(event)) {
      // §4.8: old placement beliefs are unusable. Repin the set and let the
      // next stage() do a full draw.
      if (!plates_.repin_after_invalidation(driver())) {
        frame_error_ = "repin failed after image invalidation";
      }
      compositor_.invalidate();
      return;
    }
    if (std::holds_alternative<termforge::ErrorEvent>(event)) {
      frame_error_ = std::get<termforge::ErrorEvent>(event).message;
    }
  }

  auto on_tick(std::chrono::duration<double> /*dt*/) -> void override { core_.tick(); }

  auto on_render(termforge::Screen& screen) -> void override {
    draw_status(screen);

    // The driver is kitty because configure() pinned it; the requirements
    // floor makes the cell geometry known. Both are startup-refused
    // otherwise, so the cast and the read below cannot observe a stranger.
    const auto cell_pixels =
        static_cast<termforge::KittyDriver&>(driver()).cell_pixel_size();
    const auto staged = compositor_.stage(core_.world(), cell_pixels);
    if (!staged) {
      // A dropped frame, named. The scene on screen stays the last committed
      // one — which is the compositor's rollback contract doing its job.
      frame_error_ = "stage failed — cell geometry the viewport cannot use";
      return;
    }
    frame_class_ = *staged;
  }

  /// Image work belongs here and not in on_render (termforge's frame
  /// contract): placements queued in this hook flush with this frame's
  /// present, and last frame's flush result settles last frame's commit —
  /// the same sequence test/33terminalcompositor drives by hand.
  auto on_pixels(termforge::TerminalDriver& driver) -> void override {
    if (outstanding_) {
      compositor_.finish(!driver.take_output_error());
      outstanding_ = false;
    }
    if (compositor_.emit(driver)) {
      outstanding_ = true;
    } else {
      frame_error_ = "emit failed — the frame was dropped, not half-drawn";
    }
  }

  void draw_status(termforge::Screen& screen) {
    // Below the viewport when it fits, else the last row. Cell-pixel geometry
    // decides where the viewport sits; the text layer does not share it.
    int row = screen.rows() - 1;
    const auto cell_pixels =
        static_cast<termforge::KittyDriver&>(driver()).cell_pixel_size();
    if (!cell_pixels.empty()) {
      if (const auto viewport = resident::viewport_cells(cell_pixels)) {
        if (viewport->y + viewport->h < screen.rows()) row = viewport->y + viewport->h;
      }
    }

    // The frame class is a render-side instrument, not game state — the one
    // letter a playtester can quote from a bug report's screenshot.
    const char frame_letter = frame_class_ == meter::FrameClass::Idle         ? 'i'
                              : frame_class_ == meter::FrameClass::Animation  ? 'a'
                                                                              : 'r';
    const auto& w = core_.world();
    std::snprintf(status_.data(), status_.size(),
                  "lamp %d  %s  %s  tick %u  frame %c%s",
                  static_cast<int>(w.lamp_level), w.creeping ? "creep" : "walk  ",
                  pump_name(core_.mode()).data(), w.tick, frame_letter,
                  frame_error_ ? "  !" : "");
    static_cast<void>(screen.write_text(0, row, status_.data(), termforge::Rgb{170, 170, 170},
                                        termforge::Rgb{0, 0, 0}));
    if (frame_error_) {
      static_cast<void>(screen.write_text(0, row > 0 ? row - 1 : row, frame_error_->c_str(),
                                          termforge::Rgb{255, 85, 85}, termforge::Rgb{0, 0, 0}));
    }
  }

  resident::PlateSet& plates_;
  game::Core core_;
  std::uint64_t pack_hash_;
  terminal::Compositor compositor_{plates_};
  meter::FrameClass frame_class_{meter::FrameClass::Recomposition};
  bool outstanding_{false};
  std::optional<std::string> frame_error_{};
  std::array<char, 96> status_{};
};

// ─── The two file descriptors ───────────────────────────────────────────────

struct Cli {
  std::filesystem::path pack_path;
  std::filesystem::path record_path{"m0-session.gloam"};
  std::uint64_t seed = scene::kM0Seed;
  game::PumpMode mode = game::PumpMode::RealTime;
  bool muted = false;
  bool record = true;
};

void usage(const char* argv0) {
  std::printf(
      "usage: %s [--pack PATH] [--seed N] [--step] [--mute] [--record PATH | --no-record]\n"
      "\n"
      "GLOAM's M0 corridor, playable (gloam#8). Kitty terminal required.\n"
      "\n"
      "  arrows / wasd   move and turn (relative to facing)\n"
      "  , .             lamp down / up (0 is doused)\n"
      "  c               creep (quieter, costs ticks)\n"
      "  space / enter   wait one tick\n"
      "  tab             TOGGLE THE PUMP: real-time <-> step-timed\n"
      "  q / esc         quit and seal the replay\n"
      "\n"
      "  --pack PATH     pack.gloam to load (default: beside the binary, then cwd)\n"
      "  --seed N        session seed (default: the scene's authored seed)\n"
      "  --step          start step-timed (tab toggles mid-session either way)\n"
      "  --mute          no audio device (a missing device degrades the same way)\n"
      "  --record PATH   write the session replay here (default: m0-session.gloam)\n"
      "  --no-record     do not write a replay\n",
      argv0);
}

[[nodiscard]] auto find_pack(const std::filesystem::path& given, const char* argv0)
    -> std::optional<std::filesystem::path> {
  namespace fs = std::filesystem;
  if (!given.empty() && fs::is_regular_file(given)) return given;
  if (const char* env = std::getenv("GLOAM_PACK")) {
    if (fs::is_regular_file(env)) return fs::path{env};
  }
  const auto beside = fs::path{argv0}.parent_path() / "pack.gloam";
  if (fs::is_regular_file(beside)) return beside;
  if (fs::is_regular_file("pack.gloam")) return "pack.gloam";
  return std::nullopt;
}

[[nodiscard]] auto read_file_bytes(const std::filesystem::path& path)
    -> std::optional<std::vector<std::byte>> {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  in.seekg(0, std::ios::end);
  const auto size = in.tellg();
  if (size < 0) return std::nullopt;
  in.seekg(0, std::ios::beg);
  std::vector<std::byte> bytes(static_cast<std::size_t>(size));
  if (!in.read(reinterpret_cast<char*>(bytes.data()), size)) return std::nullopt;
  return bytes;
}

}  // namespace

auto main(int argc, char** argv) -> int {
  Cli cli;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--pack" && i + 1 < argc) {
      cli.pack_path = argv[++i];
    } else if (arg == "--record" && i + 1 < argc) {
      cli.record_path = argv[++i];
    } else if (arg == "--no-record") {
      cli.record = false;
    } else if (arg == "--seed" && i + 1 < argc) {
      cli.seed = std::strtoull(argv[++i], nullptr, 0);
    } else if (arg == "--step") {
      cli.mode = game::PumpMode::StepTimed;
    } else if (arg == "--mute") {
      cli.muted = true;
    } else if (arg == "--help" || arg == "-h") {
      usage(argv[0]);
      return 0;
    } else {
      std::fprintf(stderr, "gloam_m0: unrecognised argument '%.*s'\n",
                   static_cast<int>(arg.size()), arg.data());
      usage(argv[0]);
      return 2;
    }
  }

  // §10's launch gate, held by construction: the pack is verified before the
  // terminal is ever entered, and a pack that fails its own digests refuses
  // to launch rather than fail in front of a player.
  const auto pack_path = find_pack(cli.pack_path, argv[0]);
  if (!pack_path) {
    std::fprintf(stderr,
                 "gloam_m0: no pack found — bake one with `gloam_bake` (pack.gloam beside\n"
                 "the binary or in the working directory, or pass --pack PATH)\n");
    return 1;
  }
  const auto pack_image = read_file_bytes(*pack_path);
  if (!pack_image) {
    std::fprintf(stderr, "gloam_m0: could not read %s\n", pack_path->c_str());
    return 1;
  }
  auto plates = resident::PlateSet::from_pack(*pack_image);
  if (!plates) {
    std::fprintf(stderr, "gloam_m0: %s failed the pack's own verification\n",
                 pack_path->c_str());
    return 1;
  }
  // The replay names the pack it was recorded against (#16's advisory field).
  pack::Header pack_header{};
  std::uint64_t pack_hash = replay::kNoPackHash;
  if (pack::read_header(*pack_image, pack_header)) {
    pack_hash = replay::pack_hash_from(pack_header.pack_sha256);
  }

  // §9.2: a missing or refused device is a degradation, not a crash. The
  // arena is synthesised from a FIXED seed — the same bytes on every run of
  // every build, so audio content can never be a determinism variable
  // (Stream::Ambience is excluded from world_hash; world.hpp says why).
  std::vector<float> arena(sfx::kArenaFrames);
  std::array<sfx::Clip, audio::kSoundIdCount> clips{};
  std::optional<device::DeviceSink> sink;
  audio::Sink* voices = nullptr;
  bool device_running = false;
  if (!cli.muted) {
    constexpr std::uint64_t kArenaSeed = 0x9105A3ULL;  // main.cpp's, deliberately
    if (!sfx::synthesise(kArenaSeed, arena, clips)) {
      std::fprintf(stderr, "gloam_m0: could not synthesise the audio arena\n");
      return 1;
    }
    sink.emplace(std::span<const float>{arena},
                 std::span<const sfx::Clip, audio::kSoundIdCount>{clips});
    device_running = sink->open();
    if (!device_running) {
      std::fprintf(stderr,
                   "gloam_m0: no output device — playing silent (SPEC 9.2's degradation)\n");
    }
    voices = &*sink;
  }

  M0App app{*plates, game::Core{scene::m0_world(cli.seed), cli.mode, voices}, pack_hash};
  app.configure();
  const int result = app.run();

  // The terminal is restored by run()'s own teardown before anything below
  // prints — the failure paths here are ordinary stderr lines.
  if (result != 0 && !app.outcome) {
    std::fprintf(stderr,
                 "gloam_m0: the terminal refused the session. SPEC 17: kitty only, and the\n"
                 "viewport asks for 48x19 cells of known pixel size. On a terminal that is\n"
                 "not kitty this refusal is the design, not a bug.\n");
    return result;
  }

  // A quit key seals inside the loop; any other exit is a session without a
  // seal, and a session without a seal writes no file — an empty or partial
  // log claiming to be a session is the artifact version of a shrug.
  if (!app.outcome) {
    std::fprintf(stderr, "gloam_m0: session ended without a seal; no replay written\n");
    return result;
  }
  const auto& outcome = *app.outcome;

  if (!cli.record) return result;

  const auto& log = app.core().log();
  replay::Header header{};
  header.seed = outcome.seed;
  header.ruleset_hash = outcome.expect.ruleset_hash;
  header.pack_hash = outcome.expect.pack_hash;
  header.final_world_hash = outcome.world_hash;
  std::vector<std::byte> replay_image(
      replay::image_bytes(static_cast<std::uint32_t>(log.size())));
  if (!replay::assemble(header, log, replay_image)) {
    std::fprintf(stderr, "gloam_m0: the session log failed to assemble into a replay\n");
    return 1;
  }
  std::ofstream out(cli.record_path, std::ios::binary | std::ios::trunc);
  if (!out) {
    std::fprintf(stderr, "gloam_m0: could not open %s for the replay\n",
                 cli.record_path.c_str());
    return 1;
  }
  out.write(reinterpret_cast<const char*>(replay_image.data()),
            static_cast<std::streamsize>(replay_image.size()));
  if (!out) {
    std::fprintf(stderr, "gloam_m0: could not write %s\n", cli.record_path.c_str());
    return 1;
  }

  // The session, stated where a player can quote it: the replay is the bug
  // report (§5.1), and the hash line is how the report's claim is checked.
  const auto hex = hash::to_hex(outcome.world_hash);
  std::printf("gloam_m0: %zu inputs sealed; world %.8s…\n", log.size(), hex.data());
  std::printf("gloam_m0: replay written to %s — verify with: gloam_replay play %s\n",
              cli.record_path.c_str(), cli.record_path.c_str());
  return result;
}
