#include "terminal_compositor.hpp"

#include <chrono>
#include <variant>

#include "gloam/geometry.hpp"
#include "gloam/tuning.hpp"

namespace gloam::terminal {
namespace {

[[nodiscard]] auto to_resident(const compositor::Placement& placement,
                               termforge::Extent cell_pixels)
    -> std::optional<resident::Placement> {
  if (cell_pixels.empty()) return std::nullopt;
  const auto& pixels = placement.pixels;
  if (pixels.x < 0 || pixels.y < 0 || pixels.w <= 0 || pixels.h <= 0 ||
      pixels.x + pixels.w > geometry::kViewportWidthPx ||
      pixels.y + pixels.h > geometry::kViewportHeightPx) {
    return std::nullopt;
  }

  const int offset_x = pixels.x % cell_pixels.w;
  const int offset_y = pixels.y % cell_pixels.h;
  return resident::Placement{
      .plate_id = placement.plate_id,
      .cells =
          termforge::Rect{
              pixels.x / cell_pixels.w,
              pixels.y / cell_pixels.h,
              geometry::cells_to_cover(offset_x + pixels.w, cell_pixels.w),
              geometry::cells_to_cover(offset_y + pixels.h, cell_pixels.h),
          },
      .band = placement.band,
      .band_rank = placement.band_rank,
      .pixel_offset = termforge::PixelPoint{offset_x, offset_y},
      // Whole-plate placement deliberately omits x/y/w/h. Kitty defaults to
      // the full image, and #26 measured those redundant crop keys as the
      // difference between passing and missing §11's sustained budget.
      .source = std::nullopt,
  };
}

[[nodiscard]] auto options(layer::Band band, int rank)
    -> std::optional<termforge::ImagePlacementOptions> {
  const auto z = layer::image_z(band, rank);
  if (!z) return std::nullopt;
  return termforge::ImagePlacementOptions{
      .fit = termforge::PlacementFit::Exact,
      .layer = termforge::ImageLayer::raw(*z),
  };
}

}  // namespace

auto Compositor::stage(const World& world, termforge::Extent cell_pixels)
    -> std::expected<meter::FrameClass, Error> {
  if (cell_pixels.empty()) {
    return std::unexpected{Error{ErrorCode::InvalidCellGeometry}};
  }
  auto next = compositor::compose(world, plates_.catalog());
  if (!next) {
    return std::unexpected{Error{ErrorCode::ComposeFailed, next.error()}};
  }
  // Validate every conversion before replacing the staged scene. A bad plate
  // extent cannot leave half a frame ready to emit.
  for (const auto& placement : *next) {
    if (!to_resident(placement, cell_pixels)) {
      return std::unexpected{Error{ErrorCode::InvalidPixelPlacement}};
    }
  }
  auto edits = compositor::diff(committed_, *next);
  if (!edits) {
    return std::unexpected{Error{ErrorCode::DiffFailed, edits.error()}};
  }

  pending_ = std::move(*next);
  edits_ = std::move(*edits);
  pending_state_ = compositor::snapshot(world);
  cell_pixels_ = cell_pixels;
  staged_ = true;
  return committed_state_ ? compositor::classify(*committed_state_, *pending_state_)
                          : meter::FrameClass::Recomposition;
}

auto Compositor::emit(termforge::TerminalDriver& driver) -> std::expected<void, Error> {
  if (!staged_) return {};
  for (const auto& edit : edits_) {
    if (edit.kind == compositor::EditKind::Retire) continue;
    const auto placement = to_resident(edit.placement, cell_pixels_);
    if (!placement) {
      return std::unexpected{Error{ErrorCode::InvalidPixelPlacement}};
    }
    auto result = edit.kind == compositor::EditKind::Retain ? plates_.retain(driver, *placement)
                                                            : plates_.draw(driver, *placement);
    if (!result) {
      return std::unexpected{Error{ErrorCode::PlacementFailed, result.error()}};
    }
  }
  return {};
}

auto Compositor::finish(bool output_accepted) -> void {
  if (!staged_) return;
  if (output_accepted) {
    committed_ = std::move(pending_);
    committed_state_ = std::move(pending_state_);
  }
  pending_.clear();
  pending_state_.reset();
  edits_.clear();
  staged_ = false;
}

auto Compositor::invalidate() noexcept -> void {
  committed_.clear();
  pending_.clear();
  edits_.clear();
  committed_state_.reset();
  pending_state_.reset();
  staged_ = false;
}

auto StepTransitions::register_sequence(termforge::TerminalDriver& driver, TransitionKind kind,
                                        std::span<const termforge::AnimationFrame> frames)
    -> std::expected<void, Error> {
  const auto index = index_of(kind);
  if (handles_[index]) {
    return std::unexpected{Error{ErrorCode::AlreadyRegistered}};
  }
  if (frames.size() < 2) {
    return std::unexpected{Error{ErrorCode::InvalidTransition}};
  }
  std::chrono::milliseconds duration{0};
  for (std::size_t i = 0; i + 1 < frames.size(); ++i) {
    if (frames[i].gap().count() < 0) {
      return std::unexpected{Error{ErrorCode::InvalidTransition}};
    }
    duration += frames[i].gap();
  }
  if (duration != std::chrono::milliseconds{kTransitionMs}) {
    return std::unexpected{Error{ErrorCode::InvalidTransition}};
  }

  auto handle = driver.register_animation(frames);
  if (!handle) {
    return std::unexpected{Error{ErrorCode::AnimationFailed, handle.error()}};
  }
  handles_[index] = *handle;
  return {};
}

auto StepTransitions::poll(termforge::TerminalDriver& driver,
                           std::chrono::steady_clock::time_point now)
    -> std::expected<std::optional<Action>, Error> {
  if (active_) {
    const auto handle = handles_[index_of(*active_)];
    if (!handle) {
      return std::unexpected{Error{ErrorCode::MissingTransition}};
    }
    const auto status = driver.animation_status(*handle, now);
    if (!status) {
      return std::unexpected{Error{ErrorCode::AnimationFailed, status.error()}};
    }
    if (status->playing()) return std::optional<Action>{};
    active_.reset();
  }

  if (queue_.empty()) return std::optional<Action>{};
  Action action = queue_.front();
  if (action.transition) {
    const auto handle = handles_[index_of(*action.transition)];
    if (!handle) {
      return std::unexpected{Error{ErrorCode::MissingTransition}};
    }
    const auto played = driver.play_animation(*handle, termforge::AnimationPlayMode::Once,
                                              termforge::AnimationReplay::Restart, now);
    if (!played) {
      return std::unexpected{Error{ErrorCode::AnimationFailed, played.error()}};
    }
    active_ = action.transition;
  }
  queue_.pop_front();
  return std::optional<Action>{action};
}

auto StepTransitions::emit(termforge::TerminalDriver& driver, termforge::Extent cell_pixels)
    -> std::expected<void, Error> {
  if (cell_pixels.empty()) {
    return std::unexpected{Error{ErrorCode::InvalidCellGeometry}};
  }
  pending_visible_.reset();
  visibility_staged_ = true;
  if (!active_) return {};
  const auto handle = handles_[index_of(*active_)];
  if (!handle) {
    return std::unexpected{Error{ErrorCode::MissingTransition}};
  }
  const auto cells = resident::viewport_cells(cell_pixels);
  const auto placement = options(layer::Band::Sprites, 0);
  if (!cells || !placement) {
    return std::unexpected{Error{ErrorCode::InvalidCellGeometry}};
  }
  auto result = committed_visible_ == handle ? driver.retain_animation(*cells, *handle, *placement)
                                             : driver.draw_animation(*cells, *handle, *placement);
  if (!result) {
    return std::unexpected{Error{ErrorCode::AnimationFailed, result.error()}};
  }
  pending_visible_ = handle;
  return {};
}

auto StepTransitions::finish(bool output_accepted) -> void {
  if (!visibility_staged_) return;
  if (output_accepted) committed_visible_ = pending_visible_;
  pending_visible_.reset();
  visibility_staged_ = false;
}

auto StepTransitions::forget_session() noexcept -> void {
  handles_.fill(std::nullopt);
  active_.reset();
  committed_visible_.reset();
  pending_visible_.reset();
  visibility_staged_ = false;
}

}  // namespace gloam::terminal
