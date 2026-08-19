#pragma once

/// SPEC §4.6-§4.8 — terminal-side execution of the pure placement list.
///
/// Binary-private by design: this is where GLOAM's pixel geometry meets
/// termforge cells, resident handles, output acceptance and animation clocks.

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <expected>
#include <optional>
#include <span>
#include <termforge/drivers/terminal_driver.hpp>
#include <utility>
#include <vector>

#include "gloam/compositor.hpp"
#include "gloam/replay.hpp"
#include "resident_plates.hpp"

namespace gloam::terminal {

enum class ErrorCode : std::uint8_t {
  InvalidCellGeometry = 0,
  InvalidPixelPlacement = 1,
  ComposeFailed = 2,
  DiffFailed = 3,
  PlacementFailed = 4,
  InvalidTransition = 5,
  AlreadyRegistered = 6,
  MissingTransition = 7,
  AnimationFailed = 8,
};

struct Error {
  ErrorCode code{ErrorCode::InvalidCellGeometry};
  std::optional<compositor::Error> compositor;
  std::optional<resident::Error> resident;
  std::optional<termforge::ErrorEvent> termforge;

  Error() = default;
  explicit Error(ErrorCode value) : code{value} {}
  Error(ErrorCode value, compositor::Error detail) : code{value}, compositor{std::move(detail)} {}
  Error(ErrorCode value, resident::Error detail) : code{value}, resident{std::move(detail)} {}
  Error(ErrorCode value, termforge::ErrorEvent detail)
      : code{value}, termforge{std::move(detail)} {}
};

/// Stages and emits one complete image frame. State is committed only after the
/// caller reports that the frame sink accepted termforge's flush.
class Compositor {
 public:
  explicit Compositor(resident::PlateSet& plates) : plates_{plates} {}

  [[nodiscard]] auto stage(const World& world, termforge::Extent cell_pixels)
      -> std::expected<meter::FrameClass, Error>;
  [[nodiscard]] auto emit(termforge::TerminalDriver& driver) -> std::expected<void, Error>;
  auto finish(bool output_accepted) -> void;

  /// Image invalidation makes every old terminal placement belief unusable.
  /// The PlateSet is repinned separately; the next scene is a full draw.
  auto invalidate() noexcept -> void;

  [[nodiscard]] auto committed() const noexcept -> std::span<const compositor::Placement> {
    return committed_;
  }

 private:
  resident::PlateSet& plates_;
  compositor::PlacementList committed_;
  compositor::PlacementList pending_;
  std::vector<compositor::Edit> edits_;
  std::optional<compositor::FrameState> committed_state_;
  std::optional<compositor::FrameState> pending_state_;
  termforge::Extent cell_pixels_{};
  bool staged_{false};
};

enum class TransitionKind : std::uint8_t { Forward = 0, Backward = 1, Turn = 2 };
inline constexpr std::size_t kTransitionKindCount = 3;

struct Action {
  replay::Event event{replay::Event::Wait};
  std::uint16_t payload{0};
  std::optional<TransitionKind> transition;

  [[nodiscard]] auto operator==(const Action&) const -> bool = default;
};

/// FIFO input gate around termforge's resident animation roots. An input is
/// returned to the simulation exactly once, when its visual may begin. Inputs
/// arriving during a one-shot remain queued until its 140 ms schedule
/// completes.
class StepTransitions {
 public:
  [[nodiscard]] auto register_sequence(termforge::TerminalDriver& driver, TransitionKind kind,
                                       std::span<const termforge::AnimationFrame> frames)
      -> std::expected<void, Error>;

  auto enqueue(Action action) -> void { queue_.push_back(action); }

  [[nodiscard]] auto poll(termforge::TerminalDriver& driver,
                          std::chrono::steady_clock::time_point now)
      -> std::expected<std::optional<Action>, Error>;

  /// Place or retain the currently-playing root. Omission after completion lets
  /// termforge collect the placement while keeping the registered sequence.
  [[nodiscard]] auto emit(termforge::TerminalDriver& driver, termforge::Extent cell_pixels)
      -> std::expected<void, Error>;
  auto finish(bool output_accepted) -> void;

  /// Called with the driver's image invalidation. Registered handles are stale;
  /// the caller re-registers from its caller-owned frame storage.
  auto forget_session() noexcept -> void;

  [[nodiscard]] auto queued() const noexcept -> std::size_t { return queue_.size(); }
  [[nodiscard]] auto active() const noexcept -> bool { return active_.has_value(); }

 private:
  [[nodiscard]] static constexpr auto index_of(TransitionKind kind) -> std::size_t {
    return static_cast<std::size_t>(kind);
  }

  std::array<std::optional<termforge::AnimationHandle>, kTransitionKindCount> handles_{};
  std::deque<Action> queue_;
  std::optional<TransitionKind> active_;
  std::optional<termforge::AnimationHandle> committed_visible_;
  std::optional<termforge::AnimationHandle> pending_visible_;
  bool visibility_staged_{false};
};

}  // namespace gloam::terminal
