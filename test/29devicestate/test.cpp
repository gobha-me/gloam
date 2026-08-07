#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <thread>

#include "device_state.hpp"

using namespace gloam;

TEST_CASE("a disconnect before startup publishes cannot be overwritten", "[device-state]") {
  device::detail::StateSignal signal;

  signal.mark_lost();
  CHECK_FALSE(signal.mark_running());

  CHECK(signal.state() == device::DeviceState::Lost);
  CHECK(signal.take_change());
  CHECK_FALSE(signal.take_change());
}

TEST_CASE("a running device becomes lost exactly once and close preserves the reason",
          "[device-state]") {
  device::detail::StateSignal signal;
  REQUIRE(signal.mark_running());
  REQUIRE(signal.take_change());  // the Running transition

  signal.mark_lost();
  CHECK(signal.state() == device::DeviceState::Lost);
  CHECK(signal.take_change());
  CHECK_FALSE(signal.take_change());

  signal.mark_lost();
  CHECK_FALSE(signal.take_change());  // Lost -> Lost is not a transition

  signal.mark_closed();
  CHECK(signal.state() == device::DeviceState::Lost);
  CHECK_FALSE(signal.take_change());
}

TEST_CASE("startup and disconnect racing always leave Lost sticky", "[device-state]") {
  // THE TSAN-VISIBLE VERSION OF THE STARTUP RACE.  `startStream()` may launch
  // the thread that reports disconnect before `open()` publishes Running.  Run
  // both orders against many independent signals: regardless of which thread
  // reaches a signal first, Lost must be the final state.
  constexpr std::size_t kSignals = 4096;
  std::array<device::detail::StateSignal, kSignals> signals{};
  std::atomic<bool> go{false};

  std::thread disconnect{[&] {
    while (!go.load(std::memory_order_acquire)) {
    }
    for (auto& signal : signals) signal.mark_lost();
  }};

  go.store(true, std::memory_order_release);
  for (auto& signal : signals) static_cast<void>(signal.mark_running());
  disconnect.join();

  for (auto& signal : signals) CHECK(signal.state() == device::DeviceState::Lost);
}

TEST_CASE("an opening failure cannot replace a concurrent disconnect", "[device-state]") {
  device::detail::StateSignal signal;

  signal.mark_lost();
  CHECK_FALSE(signal.mark_failed(device::DeviceState::Failed));
  CHECK(signal.state() == device::DeviceState::Lost);
}

TEST_CASE("a later open attempt can recover from an earlier failure", "[device-state]") {
  device::detail::StateSignal signal;
  REQUIRE(signal.mark_failed(device::DeviceState::NoDevice));
  REQUIRE(signal.take_change());

  signal.begin_open();
  CHECK(signal.state() == device::DeviceState::Muted);
  CHECK_FALSE(signal.take_change());
  CHECK(signal.mark_running());
  CHECK(signal.state() == device::DeviceState::Running);
}

TEST_CASE("a normal start and close reports both transitions", "[device-state]") {
  // Happy path last.  The adversarial cases above are the reason this seam
  // exists; this only proves the ordinary lifecycle still works.
  device::detail::StateSignal signal;
  CHECK(signal.state() == device::DeviceState::Muted);
  CHECK_FALSE(signal.take_change());

  REQUIRE(signal.mark_running());
  CHECK(signal.state() == device::DeviceState::Running);
  CHECK(signal.take_change());

  signal.mark_closed();
  CHECK(signal.state() == device::DeviceState::Muted);
  CHECK(signal.take_change());
}
