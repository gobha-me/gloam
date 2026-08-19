// SPEC §4.8 — the resident plate set across real terminal transitions.
//
// The failure matrix comes first. The production App cases then cross a real
// pty, so alt-screen/raw-mode teardown, driver acknowledgements, resize,
// reattach, SIGTSTP/SIGCONT and exception unwinding are observed together.

#include <catch2/catch_test_macros.hpp>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#if defined(__linux__)
#include <pty.h>
#else
#include <util.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <termforge/core/app.hpp>
#include <termforge/core/terminal.hpp>

#include "gloam/assets.hpp"
#include "gloam/geometry.hpp"
#include "gloam/pack.hpp"
#include "gloam/plate.hpp"
#include "resident_plates.hpp"
#include "terminal_sink.hpp"
#include "tty_writer.hpp"

using namespace std::chrono_literals;

namespace {

using gloam::resident::ErrorCode;
using gloam::resident::Placement;
using gloam::resident::PlateSet;

struct PackImage {
  std::vector<std::byte> bytes;
};

auto small_pack(std::size_t count) -> PackImage {
  std::vector<gloam::pack::Record> records(count);
  std::vector<std::vector<std::byte>> storage(count);
  std::vector<std::span<const std::byte>> blobs(count);

  for (std::size_t i = 0; i < count; ++i) {
    storage[i].assign(gloam::plate::blob_bytes(2, 2), std::byte{0});
    blobs[i] = storage[i];
    records[i] = gloam::pack::Record{
        .plate_id = static_cast<std::uint16_t>(i),
        .role = gloam::pack::Role::Ui,
        .depth = gloam::pack::kDepthFullFrame,
        .lateral = gloam::pack::Lateral::FullFrame,
        .wall_type = 0,
        .codec = gloam::pack::Codec::RawPlanes,
        .w = 2,
        .h = 2,
    };
  }

  PackImage image;
  image.bytes.resize(gloam::pack::image_bytes(records));
  const auto assembled = gloam::pack::assemble(records, blobs, image.bytes);
  REQUIRE(assembled);
  image.bytes.resize(assembled.bytes);
  return image;
}

auto shipped_pack() -> const std::vector<std::byte>& {
  static const std::vector<std::byte> image = [] {
    std::vector<std::byte> pixels(gloam::assets::pixel_bytes());
    std::vector<gloam::pack::Record> records(gloam::assets::kPlateCount);
    std::vector<std::span<const std::byte>> blobs(gloam::assets::kPlateCount);
    std::vector<std::byte> out(gloam::assets::image_bytes());
    const auto built = gloam::assets::build_pack(pixels, records, blobs, out);
    if (!built) throw std::runtime_error{"could not build the shipped plate pack"};
    out.resize(built.bytes);
    return out;
  }();
  return image;
}

class PinDriver final : public termforge::TerminalDriver {
 public:
  explicit PinDriver(std::size_t maximum = 256) : maximum_(maximum) {}

  auto init() -> std::expected<void, termforge::ErrorEvent> override { return {}; }
  auto draw_text(int, int, std::string_view, termforge::Rgb, termforge::Rgb,
                 termforge::Attr) -> void override {}
  auto draw_image(termforge::Rect, const termforge::Image&)
      -> std::expected<void, termforge::ErrorEvent> override {
    return {};
  }
  [[nodiscard]] auto preferred_pixel_extent(termforge::Rect cells) const noexcept
      -> termforge::Extent override {
    return {cells.w * cell_.w, cells.h * cell_.h};
  }
  auto flush() -> void override {}
  [[nodiscard]] auto capabilities() const noexcept -> termforge::Capabilities override {
    return termforge::Capabilities{.kitty_graphics = true};
  }
  [[nodiscard]] auto supports_placement_fit(termforge::PlacementFit fit) const noexcept
      -> bool override {
    return fit == termforge::PlacementFit::Stretch ||
           fit == termforge::PlacementFit::Exact;
  }
  [[nodiscard]] auto supports_image_placement(
      termforge::ImagePlacementOptions options) const noexcept -> bool override {
    return options.layer.z_index().has_value() &&
           supports_placement_fit(options.fit);
  }
  [[nodiscard]] auto max_pinned_images() const noexcept -> std::size_t override {
    return maximum_;
  }
  [[nodiscard]] auto residency() const noexcept -> termforge::ImageResidency override {
    return termforge::ImageResidency{.pinned_images = live_.size()};
  }
  [[nodiscard]] auto pinned_image_status(termforge::PinnedImage image) const noexcept
      -> termforge::PinnedImageStatus override {
    const auto found = live_.find(image.id);
    const bool valid = found != live_.end() && found->second == image;
    return termforge::PinnedImageStatus{.valid = valid, .content_ready = valid};
  }
  auto pin_image(const termforge::EncodedImage& image)
      -> std::expected<termforge::PinnedImage, termforge::ErrorEvent> override {
    const std::size_t call = pin_calls++;
    if (call == fail_pin_at || live_.size() >= maximum_ || image.empty()) {
      return std::unexpected{termforge::ErrorEvent{
          termforge::Severity::Warning, "fake", "pin refused"}};
    }
    formats.push_back(image.format);
    extents.push_back(image.pixels);
    payload_bytes.push_back(image.bytes.size());
    const termforge::PinnedImage handle{
        .id = next_id_++,
        .owner = owner_,
        .serial = next_serial_++,
    };
    live_.emplace(handle.id, handle);
    return handle;
  }
  auto unpin_image(termforge::PinnedImage image)
      -> std::expected<void, termforge::ErrorEvent> override {
    ++unpin_calls;
    const auto found = live_.find(image.id);
    if (fail_unpin || found == live_.end() || found->second != image) {
      return std::unexpected{termforge::ErrorEvent{
          termforge::Severity::Warning, "fake", "unpin refused"}};
    }
    live_.erase(found);
    return {};
  }
  auto draw_pinned(termforge::Rect cells, termforge::PinnedImage image,
                   termforge::ImagePlacementOptions options)
      -> std::expected<void, termforge::ErrorEvent> override {
    if (!pinned_image_status(image).valid) {
      return std::unexpected{termforge::ErrorEvent{
          termforge::Severity::Warning, "fake", "stale handle"}};
    }
    ++draw_calls;
    last_cells = cells;
    last_options = options;
    return {};
  }
  auto set_cell_pixel_size(termforge::Extent cell) noexcept -> void override {
    cell_ = cell;
  }
  auto invalidate_images() noexcept -> void override { live_.clear(); }

  std::size_t fail_pin_at{static_cast<std::size_t>(-1)};
  bool fail_unpin{false};
  std::size_t pin_calls{0};
  std::size_t unpin_calls{0};
  std::size_t draw_calls{0};
  termforge::Rect last_cells{};
  termforge::ImagePlacementOptions last_options{};
  std::vector<termforge::ImageFormat> formats;
  std::vector<termforge::Extent> extents;
  std::vector<std::size_t> payload_bytes;

 private:
  std::size_t maximum_{256};
  termforge::Extent cell_{10, 20};
  std::uint32_t next_id_{17};
  std::uint32_t next_serial_{1};
  std::uint32_t owner_{91};
  std::unordered_map<std::uint32_t, termforge::PinnedImage> live_;
};

auto same_termios(const termios& lhs, const termios& rhs) -> bool {
  return lhs.c_iflag == rhs.c_iflag && lhs.c_oflag == rhs.c_oflag &&
         lhs.c_cflag == rhs.c_cflag && lhs.c_lflag == rhs.c_lflag &&
         std::equal(lhs.c_cc, lhs.c_cc + NCCS, rhs.c_cc);
}

auto key_value(std::string_view keys, std::string_view key) -> std::string {
  const std::string needle = std::string{key} + "=";
  for (std::size_t at = 0; (at = keys.find(needle, at)) != std::string_view::npos;
       at += needle.size()) {
    if (at != 0 && keys[at - 1] != ',') continue;
    const auto from = at + needle.size();
    const auto comma = keys.find(',', from);
    return std::string{keys.substr(from, comma == std::string_view::npos
                                            ? comma
                                            : comma - from)};
  }
  return {};
}

struct WireCounts {
  int transmits{0};
  int placements{0};
  int image_deletes{0};
  int delete_all{0};
  bool scaled_placement{false};
};

auto wire_counts(std::string_view wire) -> WireCounts {
  WireCounts counts;
  for (std::size_t at = 0; (at = wire.find("\033_G", at)) != std::string_view::npos;) {
    const auto body = at + 3;
    const auto end = wire.find("\033\\", body);
    if (end == std::string_view::npos) break;
    const auto sequence = wire.substr(body, end - body);
    const auto semi = sequence.find(';');
    const auto keys = sequence.substr(0, semi);
    const auto action = key_value(keys, "a");
    if (action == "t") ++counts.transmits;
    if (action == "p") {
      ++counts.placements;
      counts.scaled_placement = counts.scaled_placement ||
                                !key_value(keys, "c").empty() ||
                                !key_value(keys, "r").empty();
    }
    if (action == "d" && key_value(keys, "d") == "I") ++counts.image_deletes;
    if (action == "d" && key_value(keys, "d") == "A") ++counts.delete_all;
    at = end + 2;
  }
  return counts;
}

class PtyPeer {
 public:
  PtyPeer() {
    winsize size{};
    size.ws_col = 80;
    size.ws_row = 24;
    size.ws_xpixel = 800;
    size.ws_ypixel = 480;
    if (::openpty(&master_, &slave_, nullptr, nullptr, &size) != 0) return;
    const int flags = ::fcntl(master_, F_GETFL);
    if (flags < 0 || ::fcntl(master_, F_SETFL, flags | O_NONBLOCK) != 0) {
      ::close(slave_);
      ::close(master_);
      slave_ = master_ = -1;
      return;
    }
    worker_ = std::thread{[this] { run(); }};
  }

  ~PtyPeer() {
    stop();
    if (slave_ >= 0) ::close(slave_);
    if (master_ >= 0) ::close(master_);
  }
  PtyPeer(const PtyPeer&) = delete;
  auto operator=(const PtyPeer&) -> PtyPeer& = delete;

  [[nodiscard]] auto ok() const noexcept -> bool { return master_ >= 0; }
  [[nodiscard]] auto slave() const noexcept -> int { return slave_; }
  [[nodiscard]] auto wire() const -> const std::string& { return wire_; }

  auto stop() -> void {
    if (!worker_.joinable()) return;
    stopping_.store(true);
    worker_.join();
    drain();
    process();
  }

 private:
  auto feed_reply(std::uint32_t id) -> void {
    const std::string reply = "\033_Gi=" + std::to_string(id) + ";OK\033\\";
    (void)gloam::tty::write_all(master_, reply);
  }

  auto process() -> void {
    while (true) {
      const auto start = wire_.find("\033_G", scan_);
      if (start == std::string::npos) {
        scan_ = wire_.size() > 2 ? wire_.size() - 2 : 0;
        return;
      }
      const auto body = start + 3;
      const auto end = wire_.find("\033\\", body);
      if (end == std::string::npos) {
        scan_ = start;
        return;
      }
      const auto sequence = std::string_view{wire_}.substr(body, end - body);
      const auto semi = sequence.find(';');
      const auto keys = sequence.substr(0, semi);
      if (key_value(keys, "a") == "t") {
        const auto id = key_value(keys, "i");
        if (!id.empty()) current_image_ = static_cast<std::uint32_t>(std::stoul(id));
      }
      if (key_value(keys, "q") == "0" && current_image_) {
        feed_reply(*current_image_);
        current_image_.reset();
      }
      scan_ = end + 2;
    }
  }

  auto drain() -> void {
    std::array<char, 8192> bytes{};
    while (true) {
      const auto count = ::read(master_, bytes.data(), bytes.size());
      if (count > 0) {
        wire_.append(bytes.data(), static_cast<std::size_t>(count));
        continue;
      }
      if (count < 0 && errno == EINTR) continue;
      return;
    }
  }

  auto run() -> void {
    while (!stopping_.load()) {
      pollfd pfd{master_, POLLIN, 0};
      const int ready = ::poll(&pfd, 1, 10);
      if (ready < 0 && errno == EINTR) continue;
      if (ready > 0) {
        drain();
        process();
      }
    }
  }

  int master_{-1};
  int slave_{-1};
  std::atomic<bool> stopping_{false};
  std::thread worker_;
  std::string wire_;
  std::size_t scan_{0};
  std::optional<std::uint32_t> current_image_;
};

class PlateApp : public termforge::App {
 public:
  PlateApp(int fd, PlateSet plates) : fd_(fd), sink_(fd), plates_(std::move(plates)) {}

  auto configure() -> bool {
    termforge::Capabilities caps;
    caps.kitty_graphics = true;
    set_frame_ms(1);
    return terminal().set_io(termforge::TerminalIo{fd_, fd_}).has_value() &&
           terminal().set_capabilities(caps).has_value();
  }

  auto on_start() -> void override {
    driver().set_output(&sink_);
    const auto result = plates_.pin_all(driver());
    if (!result) {
      failed_ = result.error().code;
      quit();
    }
  }

  auto on_stop() noexcept -> void override { plates_.forget_session(); }

  [[nodiscard]] auto ready() -> bool {
    for (std::uint16_t id = 0; id < plates_.size(); ++id) {
      const auto image = plates_.handle(id);
      if (!image) return false;
      const auto status = driver().pinned_image_status(*image);
      if (!status.valid || !status.content_ready || status.update_pending) return false;
    }
    return true;
  }

  [[nodiscard]] auto draw_all_plates() -> bool {
    const auto size = current_size();
    if (size.cols <= 0 || size.rows <= 0 || size.px_w <= 0 || size.px_h <= 0) {
      failed_ = ErrorCode::PlacementFailed;
      return false;
    }
    const termforge::Extent cell{size.px_w / size.cols, size.px_h / size.rows};
    const auto cells = gloam::resident::viewport_cells(cell);
    if (!cells) {
      failed_ = ErrorCode::PlacementFailed;
      return false;
    }
    rects.push_back(*cells);
    for (std::uint16_t id = 0; id < plates_.size(); ++id) {
      const auto result = plates_.draw(
          driver(), Placement{.plate_id = id,
                              .cells = *cells,
                              .band = gloam::layer::Band::Light,
                              .band_rank = id});
      if (!result) {
        failed_ = result.error().code;
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] auto handles() const -> std::vector<termforge::PinnedImage> {
    std::vector<termforge::PinnedImage> out;
    for (std::uint16_t id = 0; id < plates_.size(); ++id) {
      if (const auto image = plates_.handle(id)) out.push_back(*image);
    }
    return out;
  }

  std::optional<ErrorCode> failed_;
  std::vector<termforge::Rect> rects;

 protected:
  int fd_{-1};
  gloam::tty::TerminalSink sink_;
  PlateSet plates_;
};

class OneSessionApp final : public PlateApp {
 public:
  using PlateApp::PlateApp;

  auto on_render(termforge::Screen&) -> void override {
    if (!ready()) return;
    if (!draw_all_plates()) {
      quit();
      return;
    }
    final_handles = handles();
    quit();
  }

  std::vector<termforge::PinnedImage> final_handles;
};

class TransitionApp final : public PlateApp {
 public:
  using PlateApp::PlateApp;

  auto on_event(const termforge::Event& event) -> void override {
    if (std::holds_alternative<termforge::ResizeEvent>(event)) {
      ++resize_events;
      return;
    }
    if (std::holds_alternative<termforge::ImageInvalidatedEvent>(event)) {
      ++invalidations;
      const auto result = plates_.repin_after_invalidation(driver());
      if (!result) {
        failed_ = result.error().code;
        quit();
      }
      return;
    }
    if (std::holds_alternative<termforge::ErrorEvent>(event)) ++error_events;
  }

  auto on_render(termforge::Screen&) -> void override {
    if (failed_ || !ready()) return;

    if (phase_ == 0) {
      if (!draw_all_plates()) return quit();
      original_ = handles();
      if (!signal_resize(100, 30, 1000, 600)) {
        failed_ = ErrorCode::PlacementFailed;
        return quit();
      }
      phase_ = 1;
      return;
    }
    if (phase_ == 1 && resize_events >= 1) {
      if (!draw_all_plates()) return quit();
      grid_resize_preserved = handles() == original_;
      if (!signal_resize(90, 30, 900, 540)) {
        failed_ = ErrorCode::PlacementFailed;
        return quit();
      }
      phase_ = 2;
      return;
    }
    if (phase_ == 2 && resize_events >= 2) {
      if (!draw_all_plates()) return quit();
      cell_resize_preserved = handles() == original_;
      if (!invalidate_images(termforge::ImageInvalidationReason::Reattach)) {
        failed_ = ErrorCode::PlacementFailed;
        return quit();
      }
      phase_ = 3;
      return;
    }
    if (phase_ == 3 && invalidations >= 1) {
      if (!draw_all_plates()) return quit();
      loop_repin_changed = handles() != original_;
      before_thread_ = handles();
      std::thread poster{[this] {
        post(termforge::Event{termforge::ImageInvalidatedEvent{
            termforge::ImageInvalidationReason::Reattach}});
      }};
      poster.join();
      phase_ = 4;
      return;
    }
    if (phase_ == 4 && invalidations >= 2) {
      if (!draw_all_plates()) return quit();
      thread_repin_changed = handles() != before_thread_;
      quit();
    }
  }

  int resize_events{0};
  int invalidations{0};
  int error_events{0};
  bool grid_resize_preserved{false};
  bool cell_resize_preserved{false};
  bool loop_repin_changed{false};
  bool thread_repin_changed{false};

 private:
  auto signal_resize(int cols, int rows, int px_w, int px_h) -> bool {
    winsize size{};
    size.ws_col = static_cast<unsigned short>(cols);
    size.ws_row = static_cast<unsigned short>(rows);
    size.ws_xpixel = static_cast<unsigned short>(px_w);
    size.ws_ypixel = static_cast<unsigned short>(px_h);
    return ::ioctl(fd_, TIOCSWINSZ, &size) == 0 && ::raise(SIGWINCH) == 0;
  }

  int phase_{0};
  std::vector<termforge::PinnedImage> original_;
  std::vector<termforge::PinnedImage> before_thread_;
};

class ThrowApp final : public PlateApp {
 public:
  using PlateApp::PlateApp;
  auto on_render(termforge::Screen&) -> void override {
    throw std::runtime_error{"frame failed"};
  }
};

}  // namespace

TEST_CASE("the terminal frame sink counts accepted bytes and surfaces refusal",
          "[imagelifecycle][failure][sink]") {
  int fds[2]{-1, -1};
  REQUIRE(::pipe(fds) == 0);

  gloam::tty::TerminalSink sink{fds[1]};
  constexpr std::array<char, 4> payload{'G', 'L', 'O', 'M'};
  const auto accepted = sink.write(payload);
  REQUIRE(accepted);
  CHECK(sink.bytes_accepted() == payload.size());
  CHECK(sink.last_frame_bytes() == payload.size());

  std::array<char, payload.size()> received{};
  REQUIRE(::read(fds[0], received.data(), received.size()) ==
          static_cast<std::ptrdiff_t>(received.size()));
  CHECK(received == payload);

  const auto empty = sink.write(std::span<const char>{});
  REQUIRE(empty);
  CHECK(sink.bytes_accepted() == payload.size());
  CHECK(sink.last_frame_bytes() == 0);
  ::close(fds[1]);
  ::close(fds[0]);

  gloam::tty::TerminalSink refused{-1};
  const auto failure = refused.write(payload);
  REQUIRE_FALSE(failure);
  CHECK(failure.error().severity == termforge::Severity::Warning);
  CHECK(failure.error().source == "terminal");
  CHECK(refused.bytes_accepted() == 0);
  CHECK(refused.last_frame_bytes() == 0);
}

TEST_CASE("malformed, empty and duplicate plate packs fail before pinning",
          "[imagelifecycle][failure]") {
  PinDriver driver;

  const std::vector<std::byte> empty;
  const auto no_pack = PlateSet::from_pack(empty);
  REQUIRE_FALSE(no_pack);
  CHECK(no_pack.error().code == ErrorCode::InvalidPack);

  auto truncated = small_pack(2).bytes;
  truncated.pop_back();
  const auto short_pack = PlateSet::from_pack(truncated);
  REQUIRE_FALSE(short_pack);
  CHECK(short_pack.error().code == ErrorCode::InvalidPack);

  auto duplicate = small_pack(2).bytes;
  // plate_id is the first u16 in a record. The second record is changed from 1
  // to 0; verification reaches ordering before the now-stale pack digest.
  duplicate[gloam::pack::kHeaderBytes + gloam::pack::kRecordBytes] = std::byte{0};
  duplicate[gloam::pack::kHeaderBytes + gloam::pack::kRecordBytes + 1] = std::byte{0};
  const auto duplicate_pack = PlateSet::from_pack(duplicate);
  REQUIRE_FALSE(duplicate_pack);
  CHECK(duplicate_pack.error().pack_error == gloam::pack::PackError::RecordsOutOfOrder);
  CHECK(driver.pin_calls == 0);
}

TEST_CASE("capacity refusal is total and a mid-batch failure rolls back",
          "[imagelifecycle][failure][rollback]") {
  const auto pack = small_pack(6);

  auto too_large = PlateSet::from_pack(pack.bytes);
  REQUIRE(too_large);
  PinDriver short_driver{5};
  const auto capacity = too_large->pin_all(short_driver);
  REQUIRE_FALSE(capacity);
  CHECK(capacity.error().code == ErrorCode::InsufficientCapacity);
  CHECK(short_driver.pin_calls == 0);
  CHECK(short_driver.residency().pinned_images == 0);

  auto interrupted = PlateSet::from_pack(pack.bytes);
  REQUIRE(interrupted);
  PinDriver failing;
  failing.fail_pin_at = 3;
  const auto partial = interrupted->pin_all(failing);
  REQUIRE_FALSE(partial);
  CHECK(partial.error().code == ErrorCode::PinFailed);
  CHECK(partial.error().plate_id == 3);
  CHECK(failing.pin_calls == 4);
  CHECK(failing.unpin_calls == 3);
  CHECK(failing.residency().pinned_images == 0);
  CHECK_FALSE(interrupted->pinned());
}

TEST_CASE("a rollback failure is surfaced instead of claiming atomic cleanup",
          "[imagelifecycle][failure][rollback]") {
  auto plates = PlateSet::from_pack(small_pack(4).bytes);
  REQUIRE(plates);
  PinDriver driver;
  driver.fail_pin_at = 2;
  driver.fail_unpin = true;
  const auto result = plates->pin_all(driver);
  REQUIRE_FALSE(result);
  CHECK(result.error().code == ErrorCode::RollbackFailed);
  CHECK(result.error().terminal.has_value());
  CHECK(driver.residency().pinned_images == 2);
}

TEST_CASE("the full 256-slot contract pins and 257 is refused without eviction",
          "[imagelifecycle][capacity]") {
  auto full = PlateSet::from_pack(small_pack(256).bytes);
  REQUIRE(full);
  PinDriver driver{256};
  REQUIRE(full->pin_all(driver));
  CHECK(driver.pin_calls == 256);
  CHECK(driver.residency().pinned_images == 256);

  const auto placement = Placement{
      .plate_id = 255,
      .cells = {0, 0, 1, 1},
      .band = gloam::layer::Band::Overlay,
  };
  REQUIRE(full->draw(driver, placement));
  REQUIRE(full->draw(driver, placement));
  CHECK(driver.pin_calls == 256);

  auto overflow = PlateSet::from_pack(small_pack(257).bytes);
  REQUIRE(overflow);
  PinDriver fresh{256};
  const auto refused = overflow->pin_all(fresh);
  REQUIRE_FALSE(refused);
  CHECK(refused.error().code == ErrorCode::InsufficientCapacity);
  CHECK(fresh.pin_calls == 0);
  CHECK(fresh.residency().pinned_images == 0);
}

TEST_CASE("resize retains payload handles while invalidation requires fresh serials",
          "[imagelifecycle][resize][invalidate]") {
  auto plates = PlateSet::from_pack(small_pack(2).bytes);
  REQUIRE(plates);
  PinDriver driver;
  REQUIRE(plates->pin_all(driver));
  const auto old = plates->handle(0);
  REQUIRE(old);

  const auto reference = gloam::resident::viewport_cells({10, 20});
  const auto narrower_rows = gloam::resident::viewport_cells({10, 18});
  REQUIRE(reference);
  REQUIRE(narrower_rows);
  CHECK(*reference == termforge::Rect{0, 0, 48, 18});
  CHECK(*narrower_rows == termforge::Rect{0, 0, 48, 20});

  REQUIRE(plates->draw(driver, Placement{.plate_id = 0,
                                         .cells = *reference,
                                         .band = gloam::layer::Band::Light}));
  driver.set_cell_pixel_size({10, 18});
  REQUIRE(plates->draw(driver, Placement{.plate_id = 0,
                                         .cells = *narrower_rows,
                                         .band = gloam::layer::Band::Light}));
  CHECK(plates->handle(0) == old);
  CHECK(driver.pin_calls == 2);
  CHECK(driver.last_options.fit == termforge::PlacementFit::Exact);
  CHECK(driver.last_options.layer.z_index() ==
        gloam::layer::image_z(gloam::layer::Band::Light, 0));

  const auto premature = plates->repin_after_invalidation(driver);
  REQUIRE_FALSE(premature);
  CHECK(premature.error().code == ErrorCode::HandlesStillValid);
  CHECK(plates->handle(0) == old);

  driver.invalidate_images();
  const auto stale = plates->draw(driver, Placement{.plate_id = 0,
                                                     .cells = *narrower_rows,
                                                     .band = gloam::layer::Band::Light});
  REQUIRE_FALSE(stale);
  CHECK(stale.error().code == ErrorCode::StaleHandle);
  REQUIRE(plates->repin_after_invalidation(driver));
  const auto fresh = plates->handle(0);
  REQUIRE(fresh);
  CHECK(*fresh != *old);
  CHECK(fresh->serial != old->serial);
}

TEST_CASE("enter, leave and re-enter pin once per terminal session",
          "[imagelifecycle][pty][session]") {
  PtyPeer peer;
  REQUIRE(peer.ok());

  termios before{};
  REQUIRE(::tcgetattr(peer.slave(), &before) == 0);

  std::vector<termforge::PinnedImage> first_handles;
  {
    auto plates = PlateSet::from_pack(shipped_pack());
    REQUIRE(plates);
    OneSessionApp app{peer.slave(), std::move(*plates)};
    REQUIRE(app.configure());
    REQUIRE(app.run() == 0);
    REQUIRE_FALSE(app.failed_);
    first_handles = app.final_handles;
    REQUIRE(first_handles.size() == gloam::assets::kPlateCount);
  }

  termios after_first{};
  REQUIRE(::tcgetattr(peer.slave(), &after_first) == 0);
  CHECK(same_termios(before, after_first));

  std::vector<termforge::PinnedImage> second_handles;
  {
    auto plates = PlateSet::from_pack(shipped_pack());
    REQUIRE(plates);
    OneSessionApp app{peer.slave(), std::move(*plates)};
    REQUIRE(app.configure());
    REQUIRE(app.run() == 0);
    REQUIRE_FALSE(app.failed_);
    second_handles = app.final_handles;
    REQUIRE(second_handles.size() == gloam::assets::kPlateCount);
  }

  termios after_second{};
  REQUIRE(::tcgetattr(peer.slave(), &after_second) == 0);
  CHECK(same_termios(before, after_second));
  CHECK(second_handles != first_handles);

  peer.stop();
  const auto counts = wire_counts(peer.wire());
  CHECK(counts.transmits == 2 * gloam::assets::kPlateCount);
  CHECK(counts.placements == 2 * gloam::assets::kPlateCount);
  CHECK(counts.image_deletes == 0);
  CHECK(counts.delete_all == 2);
  CHECK_FALSE(counts.scaled_placement);
}

TEST_CASE("grid and cell resize retain payloads; both reattach routes repin",
          "[imagelifecycle][pty][resize][reattach]") {
  PtyPeer peer;
  REQUIRE(peer.ok());
  termios before{};
  REQUIRE(::tcgetattr(peer.slave(), &before) == 0);

  auto plates = PlateSet::from_pack(shipped_pack());
  REQUIRE(plates);
  TransitionApp app{peer.slave(), std::move(*plates)};
  REQUIRE(app.configure());
  REQUIRE(app.run() == 0);
  REQUIRE_FALSE(app.failed_);

  termios after{};
  REQUIRE(::tcgetattr(peer.slave(), &after) == 0);
  CHECK(same_termios(before, after));
  CHECK(app.resize_events == 2);
  CHECK(app.invalidations == 2);
  CHECK(app.error_events == 0);
  CHECK(app.grid_resize_preserved);
  CHECK(app.cell_resize_preserved);
  CHECK(app.loop_repin_changed);
  CHECK(app.thread_repin_changed);
  REQUIRE(app.rects.size() == 5);
  CHECK(app.rects[0] == termforge::Rect{0, 0, 48, 18});
  CHECK(app.rects[1] == termforge::Rect{0, 0, 48, 18});
  CHECK(app.rects[2] == termforge::Rect{0, 0, 48, 20});

  peer.stop();
  const auto counts = wire_counts(peer.wire());
  // One cold start plus one six-plate batch for each reattach. Neither real
  // SIGWINCH contributes a transmit.
  CHECK(counts.transmits == 3 * gloam::assets::kPlateCount);
  // Six placements at cold start; grid-only SIGWINCH keeps their exact same
  // destinations and is a no-op; cell-pixel SIGWINCH reflows all six; each
  // re-pin creates six fresh placements.
  CHECK(counts.placements == 4 * gloam::assets::kPlateCount);
  CHECK(counts.image_deletes == 0);
  CHECK(counts.delete_all == 1);
  CHECK_FALSE(counts.scaled_placement);
}

TEST_CASE("a frame exception restores the terminal and the next session runs",
          "[imagelifecycle][pty][exception]") {
  PtyPeer peer;
  REQUIRE(peer.ok());
  termios before{};
  REQUIRE(::tcgetattr(peer.slave(), &before) == 0);

  bool threw = false;
  {
    auto plates = PlateSet::from_pack(shipped_pack());
    REQUIRE(plates);
    ThrowApp app{peer.slave(), std::move(*plates)};
    REQUIRE(app.configure());
    try {
      (void)app.run();
    } catch (const std::runtime_error& error) {
      threw = true;
      CHECK(std::string_view{error.what()} == "frame failed");
    }
  }
  REQUIRE(threw);

  termios after_throw{};
  REQUIRE(::tcgetattr(peer.slave(), &after_throw) == 0);
  CHECK(same_termios(before, after_throw));

  {
    auto plates = PlateSet::from_pack(shipped_pack());
    REQUIRE(plates);
    OneSessionApp app{peer.slave(), std::move(*plates)};
    REQUIRE(app.configure());
    REQUIRE(app.run() == 0);
    REQUIRE_FALSE(app.failed_);
  }

  termios after_recovery{};
  REQUIRE(::tcgetattr(peer.slave(), &after_recovery) == 0);
  CHECK(same_termios(before, after_recovery));
  peer.stop();
  CHECK(wire_counts(peer.wire()).delete_all == 2);
}

namespace {

class SuspendApp final : public PlateApp {
 public:
  SuspendApp(int fd, int report_fd, PlateSet plates)
      : PlateApp(fd, std::move(plates)), report_fd_(report_fd) {}

  auto on_event(const termforge::Event& event) -> void override {
    if (!std::holds_alternative<termforge::ImageInvalidatedEvent>(event)) return;
    ++invalidations;
    const auto result = plates_.repin_after_invalidation(driver());
    if (!result) {
      failed_ = result.error().code;
      quit();
    }
  }

  auto on_render(termforge::Screen&) -> void override {
    if (failed_ || !ready()) return;
    if (!draw_all_plates()) return quit();

    if (invalidations == 0 && !announced_ready_) {
      announced_ready_ = notify('R');
      if (!announced_ready_) {
        failed_ = ErrorCode::PlacementFailed;
        quit();
      }
      return;
    }
    if (invalidations == 1) {
      if (!notify('D')) failed_ = ErrorCode::PlacementFailed;
      quit();
    }
  }

  int invalidations{0};

 private:
  auto notify(char byte) -> bool {
    return static_cast<bool>(
        gloam::tty::write_all(report_fd_, std::string_view{&byte, 1}));
  }

  int report_fd_{-1};
  bool announced_ready_{false};
};

class ParentReplyPump {
 public:
  explicit ParentReplyPump(int fd) : fd_(fd) {}

  auto pump() -> void {
    std::array<char, 8192> bytes{};
    while (true) {
      const auto count = ::read(fd_, bytes.data(), bytes.size());
      if (count > 0) {
        wire_.append(bytes.data(), static_cast<std::size_t>(count));
        continue;
      }
      if (count < 0 && errno == EINTR) continue;
      break;
    }
    process();
  }

  [[nodiscard]] auto wire() const -> const std::string& { return wire_; }

 private:
  auto reply(std::uint32_t id) -> void {
    const std::string response = "\033_Gi=" + std::to_string(id) + ";OK\033\\";
    (void)gloam::tty::write_all(fd_, response);
  }

  auto process() -> void {
    while (true) {
      const auto start = wire_.find("\033_G", scan_);
      if (start == std::string::npos) {
        scan_ = wire_.size() > 2 ? wire_.size() - 2 : 0;
        return;
      }
      const auto body = start + 3;
      const auto end = wire_.find("\033\\", body);
      if (end == std::string::npos) {
        scan_ = start;
        return;
      }
      const auto sequence = std::string_view{wire_}.substr(body, end - body);
      const auto semi = sequence.find(';');
      const auto keys = sequence.substr(0, semi);
      if (key_value(keys, "a") == "t") {
        const auto id = key_value(keys, "i");
        if (!id.empty()) current_image_ = static_cast<std::uint32_t>(std::stoul(id));
      }
      if (key_value(keys, "q") == "0" && current_image_) {
        reply(*current_image_);
        current_image_.reset();
      }
      scan_ = end + 2;
    }
  }

  int fd_{-1};
  std::string wire_;
  std::size_t scan_{0};
  std::optional<std::uint32_t> current_image_;
};

}  // namespace

TEST_CASE("SIGTSTP followed by SIGCONT repins once without stale deletes",
          "[imagelifecycle][pty][signal][invalidate]") {
  // Build before fork: the child inherits immutable pack bytes and does no
  // Catch2 work. There are no helper threads alive on this path.
  (void)shipped_pack();

  winsize size{};
  size.ws_col = 80;
  size.ws_row = 24;
  size.ws_xpixel = 800;
  size.ws_ypixel = 480;
  int master = -1;
  int slave = -1;
  REQUIRE(::openpty(&master, &slave, nullptr, nullptr, &size) == 0);
  const int master_flags = ::fcntl(master, F_GETFL);
  REQUIRE(master_flags >= 0);
  REQUIRE(::fcntl(master, F_SETFL, master_flags | O_NONBLOCK) == 0);

  int report[2]{-1, -1};
  REQUIRE(::pipe(report) == 0);
  const int report_flags = ::fcntl(report[0], F_GETFL);
  REQUIRE(report_flags >= 0);
  REQUIRE(::fcntl(report[0], F_SETFL, report_flags | O_NONBLOCK) == 0);

  termios before{};
  REQUIRE(::tcgetattr(slave, &before) == 0);

  const pid_t child = ::fork();
  REQUIRE(child >= 0);
  if (child == 0) {
    // A job-control stop is discarded for an orphaned process group. Give the
    // child its own group while its parent remains in this session, which is
    // the real foreground-job topology SIGTSTP is defined for.
    if (::setpgid(0, 0) != 0) _exit(19);
    ::close(master);
    ::close(report[0]);
    auto plates = PlateSet::from_pack(shipped_pack());
    if (!plates) _exit(20);
    SuspendApp app{slave, report[1], std::move(*plates)};
    if (!app.configure()) _exit(21);
    try {
      const int result = app.run();
      if (result != 0 || app.failed_ || app.invalidations != 1) _exit(22);
    } catch (...) {
      _exit(23);
    }
    ::close(report[1]);
    ::close(slave);
    _exit(0);
  }

  REQUIRE(::setpgid(child, child) == 0);

  ::close(report[1]);
  ParentReplyPump pump{master};
  bool ready = false;
  bool requested_stop = false;
  bool observed_stop = false;
  bool resumed = false;
  bool completed = false;
  bool exited = false;
  int status = 0;
  const auto deadline = std::chrono::steady_clock::now() + 15s;

  while (!exited && std::chrono::steady_clock::now() < deadline) {
    pollfd fds[2]{{master, POLLIN, 0}, {report[0], POLLIN, 0}};
    int polled = -1;
    do {
      polled = ::poll(fds, 2, 10);
    } while (polled < 0 && errno == EINTR);
    (void)polled;
    pump.pump();

    std::array<char, 16> notes{};
    while (true) {
      const auto count = ::read(report[0], notes.data(), notes.size());
      if (count > 0) {
        for (std::ptrdiff_t i = 0; i < count; ++i) {
          ready = ready || notes[static_cast<std::size_t>(i)] == 'R';
          completed = completed || notes[static_cast<std::size_t>(i)] == 'D';
        }
        continue;
      }
      if (count < 0 && errno == EINTR) continue;
      break;
    }

    if (ready && !requested_stop) {
      REQUIRE(::kill(child, SIGTSTP) == 0);
      requested_stop = true;
    }

    const pid_t changed = ::waitpid(child, &status, WNOHANG | WUNTRACED | WCONTINUED);
    if (changed == child) {
      if (WIFSTOPPED(status)) {
        observed_stop = true;
        CHECK(WSTOPSIG(status) == SIGTSTP);
        REQUIRE(::kill(child, SIGCONT) == 0);
      } else if (WIFCONTINUED(status)) {
        resumed = true;
      } else if (WIFEXITED(status) || WIFSIGNALED(status)) {
        exited = true;
      }
    }
  }

  if (!exited) {
    (void)::kill(child, SIGKILL);
    REQUIRE(::waitpid(child, &status, 0) == child);
  }
  pump.pump();

  CHECK(ready);
  CHECK(requested_stop);
  CHECK(observed_stop);
  CHECK(resumed);
  CHECK(completed);
  REQUIRE(exited);
  REQUIRE(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 0);

  termios after{};
  REQUIRE(::tcgetattr(slave, &after) == 0);
  CHECK(same_termios(before, after));
  const auto counts = wire_counts(pump.wire());
  CHECK(counts.transmits == 2 * gloam::assets::kPlateCount);
  CHECK(counts.placements == 2 * gloam::assets::kPlateCount);
  CHECK(counts.image_deletes == 0);
  CHECK(counts.delete_all == 1);
  CHECK_FALSE(counts.scaled_placement);

  ::close(report[0]);
  ::close(slave);
  ::close(master);
}
