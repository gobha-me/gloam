#pragma once

/// SPEC §11 — budgets.
///
/// "These are not optimisation targets. Each one is a test, wired from the
/// first commit, that fails the build when exceeded."
///
/// Every row of §11's table appears here as a named constant and in
/// test/10budgets/ as an assertion — including the rows nothing can exceed yet,
/// because §19 step 4's acceptance criterion is that the assertions EXIST
/// "even where the numbers are trivially met". A budget wired in after the code
/// it constrains is a budget that gets negotiated.

#include <cstddef>
#include <cstdint>

namespace gloam::budget {

// ── Residency ───────────────────────────────────────────────────────────────

/// §11 — hard cap on resident images, tested against the pack manifest.
inline constexpr int kMaxResidentImages = 256;

/// §11 — cold-start payload, base64 encoded.
inline constexpr std::size_t kMaxColdStartPayloadBytes = 1'200'000;

// ── Timing ──────────────────────────────────────────────────────────────────

inline constexpr int kMaxColdStartLocalMs = 800;
inline constexpr int kMaxColdStartThrottledMs = 12'000;  ///< over a 1 Mbit/s link
inline constexpr int kMaxSimulationTickMs = 4;           ///< at 10 Hz
inline constexpr int kMaxComposeDiffEmitMs = 2;
inline constexpr int kMaxAudioLatencyMs = 20;  ///< tick that emits -> first sample

// ── Emitted bytes (§4.6, emit-on-change) ───────────────────────────────────

/// An idle frame costs ZERO bytes. Not "few" — zero. In a discrete-cell game
/// with 90-degree turns an idle frame is genuinely idle, and this is the number
/// that makes the ssh promise hold.
inline constexpr std::size_t kIdleFrameBytes = 0;
inline constexpr std::size_t kMaxAnimationFrameBytes = 400;
inline constexpr std::size_t kMaxRecompositionBytes = 2'048;
/// p95 over a 200-tick scripted replay.
inline constexpr std::size_t kMaxSustainedBytesPerSecond = 8'192;

/// §9.2 — a full ring drops the command and increments a counter rather than
/// blocking. The counter is a budget assertion, not a log line.
inline constexpr int kMaxDroppedVoiceCommands = 0;
inline constexpr int kDroppedVoiceCommandWindowTicks = 1'000;

// ── §4.2 Slot inventory ────────────────────────────────────────────────────

/// One row of §4.2's table.
struct SlotClass {
  int m0{};
  int full{};
};

inline constexpr SlotClass kWallSlots{24, 48};        ///< 12 slots x wall types
inline constexpr SlotClass kFloorCeilingBands{8, 8};  ///< one pair per depth
inline constexpr SlotClass kLightFields{6, 6};        ///< §4.4, one per lamp level
inline constexpr SlotClass kMonsterPoses{27, 120};
inline constexpr SlotClass kItemsAndDecorations{0, 34};
inline constexpr SlotClass kUiFramesAndGlyphs{6, 30};

/// Transition frame sequences are "animation registrations, not placements"
/// (§4.2's own note), and they are therefore NOT resident images.
///
/// This is the arithmetic that makes §4.2's table add up, and it is worth
/// stating out loud because the naive reading is off by three. Summing every
/// row of the table gives 74 for M0 and 252 for the full game; §4.2's stated
/// totals are 71 and 246. The difference is exactly this row, both times. A
/// manifest test that counted transitions as plates would report 74 against a
/// budget written for 71 and be wrong in the safe direction — until the full
/// game, where it would be wrong in the other one.
inline constexpr SlotClass kTransitionSequences{3, 6};

[[nodiscard]] constexpr auto resident_images_m0() -> int {
  return kWallSlots.m0 + kFloorCeilingBands.m0 + kLightFields.m0 + kMonsterPoses.m0 +
         kItemsAndDecorations.m0 + kUiFramesAndGlyphs.m0;
}

[[nodiscard]] constexpr auto resident_images_full() -> int {
  return kWallSlots.full + kFloorCeilingBands.full + kLightFields.full + kMonsterPoses.full +
         kItemsAndDecorations.full + kUiFramesAndGlyphs.full;
}

static_assert(resident_images_m0() == 71, "§4.2's M0 total, excluding transition sequences");
static_assert(resident_images_full() == 246, "§4.2's full-game total");
static_assert(resident_images_full() <= kMaxResidentImages,
              "§4.2's own inventory must fit §11's cap, or the art plan is already over budget");

// ── §4.5 The compositing bands ──────────────────────────────────────────────
//
// The below-background z threshold used to live here, because there was nothing
// better to hold it. §4.5 asks for it "inside a named layer API", and that API
// now exists: see `gloam::layer::kBelowBackgroundZ` in `include/gloam/layer.hpp`,
// which is also where `cmake/check_layer_z.cmake` expects to find it.
//
// This comment deliberately describes the number instead of spelling it. That
// checker counts FILES containing the literal and requires exactly one, so a
// comment here quoting the value would make this file a second hit and fail the
// build — the same discipline AGENTS.md sets out for `check_artifacts.cmake`.

}  // namespace gloam::budget
