// SPEC §4.6-§4.8 — measured terminal placement diff and queued transitions.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <termforge/core/byte_sink.hpp>
#include <termforge/drivers/kitty_driver.hpp>
#include <vector>

#include "compositor_fixture.hpp"
#include "gloam/budgets.hpp"
#include "gloam/geometry.hpp"
#include "gloam/pack.hpp"
#include "gloam/plate.hpp"
#include "resident_plates.hpp"
#include "terminal_compositor.hpp"

using namespace std::chrono_literals;

namespace {

using namespace gloam;

auto scene_records() -> std::vector<pack::Record> { return test_support::compositor_records(); }

auto scene_pack() -> std::vector<std::byte> { return test_support::compositor_pack(); }

auto acknowledge_pins(resident::PlateSet& plates, termforge::KittyDriver& driver) -> void {
  for (const auto& record : scene_records()) {
    const auto handle = plates.handle(record.plate_id);
    REQUIRE(handle);
    driver.consume_reply(termforge::TerminalReply{handle->id, std::nullopt, "OK"});
  }
}

auto world() -> World {
  Level level{9, 9};
  for (int y = 0; y < 9; ++y) level.carve({0, y}, Dir::East, 9);
  for (int x = 0; x < 9; ++x) level.carve({x, 0}, Dir::South, 9);
  auto result = make_world(0x3377, std::move(level), {});
  result.party = {4, 4};
  result.facing = Dir::North;
  result.lamp_level = 3;
  return result;
}

auto count(std::string_view text, std::string_view needle) -> std::size_t {
  std::size_t result = 0;
  for (std::size_t at = 0; (at = text.find(needle, at)) != std::string_view::npos;
       at += needle.size()) {
    ++result;
  }
  return result;
}

class RefusingSink final : public termforge::ByteSink {
 public:
  auto write(std::span<const char>) -> std::expected<void, termforge::ErrorEvent> override {
    return std::unexpected{
        termforge::ErrorEvent{termforge::Severity::Error, "test", "refused frame"}};
  }
};

}  // namespace

TEST_CASE("terminal compositor rejects invalid cell geometry before wire",
          "[terminal-compositor][failure]") {
  const auto image = scene_pack();
  auto plates = resident::PlateSet::from_pack(image);
  REQUIRE(plates);
  terminal::Compositor compositor{*plates};
  const auto staged = compositor.stage(world(), {0, 16});
  REQUIRE_FALSE(staged);
  CHECK(staged.error().code == terminal::ErrorCode::InvalidCellGeometry);
}

TEST_CASE("terminal compositor emits changes and retains idle at zero bytes",
          "[terminal-compositor][kitty][wire]") {
  const auto image = scene_pack();
  auto plates = resident::PlateSet::from_pack(image);
  REQUIRE(plates);

  termforge::KittyDriver driver;
  driver.set_cell_pixel_size({10, 20});
  std::string wire;
  driver.set_output(&wire);
  REQUIRE(plates->pin_all(driver));
  driver.flush();
  acknowledge_pins(*plates, driver);
  wire.clear();

  terminal::Compositor compositor{*plates};
  auto scene = world();
  REQUIRE(compositor.stage(scene, {10, 20}) == meter::FrameClass::Recomposition);
  const auto idle_emit = compositor.emit(driver);
  if (!idle_emit) {
    int resident_code = -1;
    std::string terminal_message;
    if (idle_emit.error().resident) {
      resident_code = static_cast<int>(idle_emit.error().resident->code);
      if (idle_emit.error().resident->terminal) {
        terminal_message = idle_emit.error().resident->terminal->message;
      }
    }
    FAIL("terminal compositor error " << static_cast<int>(idle_emit.error().code)
                                      << ", resident error " << resident_code << ": "
                                      << terminal_message);
  }
  REQUIRE(idle_emit);
  driver.flush();
  REQUIRE_FALSE(driver.take_output_error());
  compositor.finish(true);
  CHECK(count(wire, "a=p") > 0);
  CHECK(wire.find(",x=") == std::string::npos);
  CHECK(wire.find(",y=") == std::string::npos);
  CHECK(wire.find(",w=") == std::string::npos);
  CHECK(wire.find(",h=") == std::string::npos);

  wire.clear();
  REQUIRE(compositor.stage(scene, {10, 20}) == meter::FrameClass::Idle);
  const auto first_emit = compositor.emit(driver);
  if (!first_emit) {
    INFO("terminal compositor error " << static_cast<int>(first_emit.error().code));
    if (first_emit.error().resident) {
      INFO("resident error " << static_cast<int>(first_emit.error().resident->code));
      if (first_emit.error().resident->terminal) {
        INFO(first_emit.error().resident->terminal->message);
      }
    }
  }
  REQUIRE(first_emit);
  driver.flush();
  REQUIRE_FALSE(driver.take_output_error());
  compositor.finish(true);
  CHECK(wire.empty());
  CHECK(driver.last_frame_bytes().total() == 0);

  wire.clear();
  scene.lamp_level = 4;
  REQUIRE(compositor.stage(scene, {10, 20}) == meter::FrameClass::Animation);
  REQUIRE(compositor.emit(driver));
  driver.flush();
  REQUIRE_FALSE(driver.take_output_error());
  compositor.finish(true);
  CHECK(count(wire, "a=p") == 1);
}

TEST_CASE("a refused frame is redrawn instead of committed", "[terminal-compositor][rollback]") {
  const auto image = scene_pack();
  auto plates = resident::PlateSet::from_pack(image);
  REQUIRE(plates);
  termforge::KittyDriver driver;
  driver.set_cell_pixel_size({10, 20});
  std::string wire;
  driver.set_output(&wire);
  REQUIRE(plates->pin_all(driver));
  driver.flush();
  acknowledge_pins(*plates, driver);
  wire.clear();

  terminal::Compositor compositor{*plates};
  auto scene = world();
  REQUIRE(compositor.stage(scene, {10, 20}));
  REQUIRE(compositor.emit(driver));
  RefusingSink refusing;
  driver.set_output(&refusing);
  driver.flush();
  REQUIRE(driver.take_output_error());
  compositor.finish(false);

  driver.set_output(&wire);
  REQUIRE(compositor.stage(scene, {10, 20}));
  REQUIRE(compositor.emit(driver));
  driver.flush();
  REQUIRE_FALSE(driver.take_output_error());
  compositor.finish(true);
  CHECK(count(wire, "a=p") > 0);
}

TEST_CASE("image invalidation clears the compositor's retained scene",
          "[terminal-compositor][invalidation]") {
  const auto image = scene_pack();
  auto plates = resident::PlateSet::from_pack(image);
  REQUIRE(plates);
  termforge::KittyDriver driver;
  driver.set_cell_pixel_size({10, 20});
  std::string wire;
  driver.set_output(&wire);
  REQUIRE(plates->pin_all(driver));
  driver.flush();
  acknowledge_pins(*plates, driver);
  wire.clear();

  terminal::Compositor compositor{*plates};
  const auto scene = world();
  REQUIRE(compositor.stage(scene, {10, 20}));
  REQUIRE(compositor.emit(driver));
  driver.flush();
  REQUIRE_FALSE(driver.take_output_error());
  compositor.finish(true);

  driver.invalidate_images();
  compositor.invalidate();
  REQUIRE(plates->repin_after_invalidation(driver));
  driver.flush();
  acknowledge_pins(*plates, driver);
  wire.clear();

  REQUIRE(compositor.stage(scene, {10, 20}) == meter::FrameClass::Recomposition);
  REQUIRE(compositor.emit(driver));
  driver.flush();
  REQUIRE_FALSE(driver.take_output_error());
  compositor.finish(true);
  CHECK(count(wire, "a=p") > 0);
}

TEST_CASE("transition registration enforces the named 140 ms schedule", "[transition][failure]") {
  termforge::KittyDriver driver;
  driver.set_image_animation_support(true);
  std::string wire;
  driver.set_output(&wire);
  const termforge::Image image{2, 2, std::vector<termforge::Pixel>(4)};
  const std::array bad{termforge::AnimationFrame{image, 139ms},
                       termforge::AnimationFrame{image, 0ms}};
  terminal::StepTransitions transitions;
  const auto result = transitions.register_sequence(driver, terminal::TransitionKind::Forward, bad);
  REQUIRE_FALSE(result);
  CHECK(result.error().code == terminal::ErrorCode::InvalidTransition);
  CHECK(wire.empty());
}

TEST_CASE("an unavailable transition does not consume its queued input",
          "[transition][queue][failure]") {
  termforge::KittyDriver driver;
  driver.set_image_animation_support(true);
  std::string wire;
  driver.set_output(&wire);
  terminal::StepTransitions transitions;
  const terminal::Action action{replay::Event::Step, 1,
                                terminal::TransitionKind::Forward};
  transitions.enqueue(action);

  const auto missing = transitions.poll(driver, std::chrono::steady_clock::time_point{});
  REQUIRE_FALSE(missing);
  CHECK(missing.error().code == terminal::ErrorCode::MissingTransition);
  CHECK(transitions.queued() == 1);

  const termforge::Image image{2, 2, std::vector<termforge::Pixel>(4)};
  const std::array frames{termforge::AnimationFrame{image, 140ms},
                          termforge::AnimationFrame{image, 0ms}};
  REQUIRE(transitions.register_sequence(driver, terminal::TransitionKind::Forward, frames));
  driver.flush();
  const auto recovered = transitions.poll(driver, std::chrono::steady_clock::time_point{});
  REQUIRE(recovered);
  REQUIRE(*recovered);
  CHECK(**recovered == action);
  CHECK(transitions.queued() == 0);
}

TEST_CASE("inputs queued during a transition are returned once in FIFO order",
          "[transition][queue][kitty]") {
  termforge::KittyDriver driver;
  driver.set_image_animation_support(true);
  std::string wire;
  driver.set_output(&wire);
  const termforge::Image image{2, 2, std::vector<termforge::Pixel>(4)};
  const std::array frames{termforge::AnimationFrame{image, 140ms},
                          termforge::AnimationFrame{image, 0ms}};
  terminal::StepTransitions transitions;
  REQUIRE(transitions.register_sequence(driver, terminal::TransitionKind::Forward, frames));
  driver.flush();
  wire.clear();

  const terminal::Action first{replay::Event::Step, 1, terminal::TransitionKind::Forward};
  const terminal::Action second{replay::Event::Step, 2, terminal::TransitionKind::Forward};
  transitions.enqueue(first);
  transitions.enqueue(second);
  const auto epoch = std::chrono::steady_clock::time_point{};

  const auto started = transitions.poll(driver, epoch);
  REQUIRE(started);
  REQUIRE(started->has_value());
  CHECK(**started == first);
  REQUIRE(transitions.emit(driver, {10, 20}));
  driver.flush();
  REQUIRE_FALSE(driver.take_output_error());
  transitions.finish(true);
  CHECK(transitions.queued() == 1);
  CHECK(count(wire, "a=p") == 1);
  CHECK(driver.last_frame_bytes().total() <= budget::kMaxRecompositionBytes);

  wire.clear();
  const auto still_playing = transitions.poll(driver, epoch + 100ms);
  REQUIRE(still_playing);
  CHECK_FALSE(still_playing->has_value());
  REQUIRE(transitions.emit(driver, {10, 20}));
  driver.flush();
  transitions.finish(true);
  CHECK(wire.empty());

  const auto next = transitions.poll(driver, epoch + 140ms);
  REQUIRE(next);
  REQUIRE(next->has_value());
  CHECK(**next == second);
  CHECK(transitions.queued() == 0);
}
