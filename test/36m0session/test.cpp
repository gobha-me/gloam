// gloam#8 — the M0 binary, played end to end on a pty.
//
// One test, because the point is the composition: pack in, pins ACKed by a
// cooperating peer, keys decoded by the real terminal input path, pump toggled
// mid-session, quit sealing a replay file — and that file passing the §12 load
// gate and reproducing, in this process, the world the binary's process ended
// on. The key schedule below is the assertion, not a script: real-time
// latest-wins and the creep lockout make a too-fast key land differently, so
// every gap is wider than the window it must clear, and the expected record
// sequence is written out in full.

#include <catch2/catch_all.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "gloam/assets.hpp"
#include "gloam/pack.hpp"
#include "gloam/replay.hpp"
#include "gloam/sha256.hpp"
#include "gloam/tuning.hpp"
#include "gloam/world.hpp"
#include "scene.hpp"
#include "tty_writer.hpp"

using namespace gloam;
using namespace std::chrono_literals;

namespace {

/// The pty peer, reduced to exactly what gloam_m0's session needs: answer the
/// capability probe (kitty graphics i=31 OK, then DA1), answer every pin
/// query, and collect the wire. q=2 means "stay quiet" — the probe's
/// animation cleanup relies on it.
class Peer {
 public:
  auto pump() -> void {
    std::array<char, 8192> bytes{};
    while (true) {
      const auto count = ::read(master_, bytes.data(), bytes.size());
      if (count > 0) {
        wire_.append(bytes.data(), static_cast<std::size_t>(count));
        answer();
        continue;
      }
      if (count < 0 && errno == EINTR) continue;
      if (count < 0 && errno == EAGAIN) return;
      eof_ = true;  // 0, or EIO: the child closed its side
      return;
    }
  }

  /// Answer a DA1 probe if one arrived since the last pump. Written without
  /// attributes that would advertise sixel: the offer is kitty or nothing.
  auto answer_da1() -> void {
    if (wire_.find("\033[c", da1_scanned_) == std::string::npos) return;
    da1_scanned_ = wire_.size();
    static constexpr char kDa1Reply[] = "\033[?62c";
    (void)tty::write_all(master_, kDa1Reply);
  }

  [[nodiscard]] auto eof() const -> bool { return eof_; }
  [[nodiscard]] auto wire() const -> const std::string& { return wire_; }

  /// The app's own status row, seen on the wire: proof that setup, the probe
  /// and enter_screen all completed and the loop is rendering. The script's
  /// safe-to-type signal — a key sent before this can be swallowed by the
  /// probe's own 150 ms input read.
  [[nodiscard]] auto rendering() const -> bool {
    return wire_.find("lamp ") != std::string::npos;
  }

  int master_{-1};
  std::string wire_;

 private:
  auto answer() -> void {
    while (true) {
      const auto start = wire_.find("\033_G", scan_);
      if (start == std::string::npos) {
        scan_ = wire_.size() > 2 ? wire_.size() - 2 : 0;
        return;
      }
      const auto body = start + 2;
      const auto end = wire_.find("\033\\", body);
      if (end == std::string::npos) {
        scan_ = start;
        return;
      }
      const auto sequence = std::string_view{wire_}.substr(body, end - body);
      scan_ = end + 2;
      if (sequence.find("q=2") != std::string_view::npos) continue;  // quiet
      const auto id = sequence.find("i=");
      if (id == std::string_view::npos) continue;
      const auto digits = sequence.substr(id + 2);
      // Replies are "\033_Gi=<id>;OK\033\\" — the terminal's whole half of the
      // pin and probe protocol.
      const std::string reply = "\033_Gi=" + std::string{digits} + ";OK\033\\";
      (void)tty::write_all(master_, reply);
    }
  }

  std::size_t scan_{0};
  std::size_t da1_scanned_{0};
  bool eof_{false};
};

struct KeyStroke {
  char key;
  int settle_ms;  // silence after the key — wider than the window it must clear
};

/// The session. Delays are sized against the 10 Hz tick: 250 ms lands a key in
/// its own tick even on a loaded CI box (real-time commits one per 100 ms
/// tick, latest-wins), and the 600 ms after the creep step clears its two-tick
/// lockout with room to spare. Margins, not synchronisation: the assertion is
/// the record sequence, and a dropped or merged key shows up there.
constexpr KeyStroke kScript[] = {
    {'.', 250},  // lamp 1
    {'.', 250},  // lamp 2
    {'.', 250},  // lamp 3
    {'w', 250},  // step forward (walking: one tick)
    {'c', 250},  // creep on
    {'w', 600},  // creep step — costs two ticks
    {'\t', 250}, // TAB: the pump toggle, real-time -> step-timed
    {'w', 250},  // step-timed: commits at once, still creeping — two ticks
    {'a', 250},  // turn left (East -> North)
    {'w', 250},  // step north
    {'q', 0},    // quit: seal and write
};

}  // namespace

TEST_CASE("the M0 binary plays a session and its replay reproduces the world",
          "[m0session][pty]") {
  if (std::string_view{GLOAM_M0_PATH}.empty()) {
    FAIL("gloam_m0 was not built — configure with the binary targets enabled");
  }

  // The pack, baked in-process through the pipeline gloam_bake runs, written
  // where the child can read it. tmpnam would race; mkstemp does not.
  std::vector<std::byte> pixels(assets::pixel_bytes());
  std::vector<pack::Record> records(static_cast<std::size_t>(assets::kPlateCount));
  std::vector<std::span<const std::byte>> blobs(static_cast<std::size_t>(assets::kPlateCount));
  std::vector<std::byte> pack_image(assets::image_bytes());
  REQUIRE(assets::build_pack(pixels, records, blobs, pack_image));

  char pack_path[] = "/tmp/gloam-m0-pack-XXXXXX";
  const int pack_fd = ::mkstemp(pack_path);
  REQUIRE(pack_fd >= 0);
  const auto pack_written = tty::write_all(
      pack_fd, std::string_view{reinterpret_cast<const char*>(pack_image.data()),
                                pack_image.size()});
  REQUIRE(pack_written.error == tty::WriteError::None);
  ::close(pack_fd);

  char replay_path[] = "/tmp/gloam-m0-replay-XXXXXX";
  const int replay_fd = ::mkstemp(replay_path);
  REQUIRE(replay_fd >= 0);
  ::close(replay_fd);

  winsize size{};
  size.ws_col = 80;
  size.ws_row = 24;
  size.ws_xpixel = 800;
  size.ws_ypixel = 480;
  Peer peer;
  int slave = -1;
  REQUIRE(::openpty(&peer.master_, &slave, nullptr, nullptr, &size) == 0);
  const int flags = ::fcntl(peer.master_, F_GETFL);
  REQUIRE(flags >= 0);
  REQUIRE(::fcntl(peer.master_, F_SETFL, flags | O_NONBLOCK) == 0);

  const pid_t child = ::fork();
  REQUIRE(child >= 0);
  if (child == 0) {
    ::close(peer.master_);
    ::dup2(slave, STDIN_FILENO);
    ::dup2(slave, STDOUT_FILENO);
    ::execl(GLOAM_M0_PATH, GLOAM_M0_PATH, "--pack", pack_path, "--record", replay_path,
            "--mute", static_cast<const char*>(nullptr));
    _exit(47);  // exec failed: a distinct status the parent can name
  }
  ::close(slave);

  // Drive the session: answer the probe, ack the pins, then play the script.
  const auto deadline = std::chrono::steady_clock::now() + 30s;
  std::size_t stroke = 0;
  auto next_stroke_at = std::chrono::steady_clock::time_point::max();
  bool script_started = false;
  int status = 0;
  bool exited = false;

  while (std::chrono::steady_clock::now() < deadline) {
    pollfd pfd{peer.master_, POLLIN, 0};
    const int polled = ::poll(&pfd, 1, 10);
    if (polled < 0 && errno != EINTR) break;
    if (polled > 0) {
      peer.pump();
      peer.answer_da1();
    }
    if (peer.eof()) break;

    // The script starts once the app is RENDERING — not on a pin count, which
    // batching makes the wrong thing to wait for. Input is handled whether or
    // not the plates have all landed, and the record assertions do not look
    // at the screen.
    if (!script_started && peer.rendering()) {
      script_started = true;
      next_stroke_at = std::chrono::steady_clock::now() + 400ms;
    }
    if (script_started && stroke < std::size(kScript) &&
        std::chrono::steady_clock::now() >= next_stroke_at) {
      const auto& [key, settle_ms] = kScript[stroke++];
      (void)tty::write_all(peer.master_, std::string_view{&key, 1});
      next_stroke_at = std::chrono::steady_clock::now() + settle_ms * 1ms;
    }

    const pid_t changed = ::waitpid(child, &status, WNOHANG);
    if (changed == child) {
      exited = true;
      break;
    }
  }

  if (!exited) {
    // Pty EOF means the child closed its side — it may already be a zombie
    // awaiting the reap, so give the ordinary wait a moment before the kill.
    const auto reap_deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < reap_deadline) {
      if (::waitpid(child, &status, WNOHANG) == child) {
        exited = true;
        break;
      }
      ::usleep(10'000);
    }
  }
  if (!exited) {
    (void)::kill(child, SIGKILL);
    REQUIRE(::waitpid(child, &status, 0) == child);
  }
  ::close(peer.master_);
  ::unlink(pack_path);

  INFO("wire tail: " << peer.wire().substr(peer.wire().size() > 200 ? peer.wire().size() - 200
                                                                    : 0));
  REQUIRE(exited);
  REQUIRE(WIFEXITED(status));
  REQUIRE(WEXITSTATUS(status) == 0);
  REQUIRE(script_started);
  CHECK(stroke == std::size(kScript));

  // The artifact the binary sealed: through the §12 load gate, then replayed
  // against a fresh scene world in THIS process.
  std::ifstream in(replay_path, std::ios::binary | std::ios::ate);
  REQUIRE(in);
  const auto replay_size = in.tellg();
  REQUIRE(replay_size > 0);
  in.seekg(0, std::ios::beg);
  std::vector<std::byte> image(static_cast<std::size_t>(replay_size));
  REQUIRE(in.read(reinterpret_cast<char*>(image.data()), replay_size));
  ::unlink(replay_path);

  pack::Header pack_header{};
  REQUIRE(pack::read_header(pack_image, pack_header));
  const replay::Expect expect{ruleset_hash(kDefaultTuning),
                              replay::pack_hash_from(pack_header.pack_sha256)};

  replay::Header header{};
  std::vector<replay::Record> log(image.size() / replay::kRecordBytes + 1);
  REQUIRE(replay::load(image, expect, header, log));
  log.resize(header.record_count);

  // The exact session, as a SEQUENCE: three lamp-ups, a walking step, creep
  // on, a creep step, then the step-timed step/turn/step, then the seal. The
  // Tab toggle is NOT a record — it is shell policy, not simulation. Record
  // ticks are NOT pinned: which tick a real-time key commits on depends on
  // the session's phase when the byte arrives, and pinning that would test
  // the machine's scheduler, not the game. What must hold is the §3 order
  // rule — non-decreasing ticks.
  REQUIRE(log.size() == 10);
  constexpr std::pair<replay::Event, std::uint16_t> want[] = {
      {replay::Event::Lamp, 1},  {replay::Event::Lamp, 2}, {replay::Event::Lamp, 3},
      {replay::Event::Step, 1},  {replay::Event::Creep, 1}, {replay::Event::Step, 1},
      {replay::Event::Step, 1},  {replay::Event::Turn, 0}, {replay::Event::Step, 0},
      {replay::Event::Wait, 0},
  };
  for (std::size_t i = 0; i < log.size(); ++i) {
    CHECK(log[i].event == want[i].first);
    CHECK(log[i].payload == want[i].second);
    if (i > 0) CHECK(log[i - 1].tick <= log[i].tick);
  }

  auto fresh = scene::m0_world(scene::kM0Seed);
  play(fresh, log, kDefaultTuning);
  CHECK(world_hash(fresh) == header.final_world_hash);
}
