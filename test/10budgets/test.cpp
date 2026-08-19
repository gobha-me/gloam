// SPEC §11 and BUDGETS.md — every budget as an assertion.
//
// "Every row of §11 is an assertion in CI." Most of the rows constrain code
// that does not exist yet, and they are here anyway: §19 step 4's acceptance
// criterion is that the assertions EXIST "even where the numbers are trivially
// met", so that the first commit which can exceed one finds a test already
// waiting for it rather than a code review.
//
// Rows marked PENDING below assert their own constant and name the milestone
// that makes them measurable. That is deliberate: a row that quietly vanished
// would be indistinguishable from a row that was never written.

#include <catch2/catch_all.hpp>

#include <array>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <termforge/drivers/kitty_driver.hpp>
#include <vector>

#include "gloam/assets.hpp"
#include "gloam/audio.hpp"
#include "gloam/base64.hpp"
#include "gloam/budgets.hpp"
#include "gloam/deflate.hpp"
#include "gloam/emit.hpp"
#include "gloam/geometry.hpp"
#include "gloam/kitty.hpp"
#include "gloam/layer.hpp"
#include "gloam/lightfield.hpp"
#include "gloam/meter.hpp"
#include "gloam/noise.hpp"
#include "gloam/pack.hpp"
#include "gloam/perception.hpp"
#include "gloam/plate.hpp"
#include "gloam/png.hpp"
#include "gloam/replay.hpp"
#include "gloam/tuning.hpp"
#include "gloam/world.hpp"

// The one file in GLOAM that calls write(2), and the two that carry §9's float
// half. All three are reachable here because this test directory owns its own
// wiring — see CMakeLists.txt. None of them is in gloam::lib, and none of them
// includes RtAudio.
#include "resident_plates.hpp"
#include "sfx.hpp"
#include "terminal_compositor.hpp"
#include "tty_writer.hpp"
#include "voice_mixer.hpp"

#include "compositor_fixture.hpp"

using namespace gloam;

// Is this build sanitized?
//
// Written as nested `#if`s rather than one expression, because the obvious
// one-liner does not work: `defined(__has_feature) && __has_feature(...)` is NOT
// short-circuited by the preprocessor — GCC, which has no `__has_feature`, still
// has to parse the second operand and reports "missing binary operator before
// token '('". That failure is a compile error on the exact configuration the
// guard exists to serve, so it is worth the four extra lines. (Measured: it
// broke the GCC 13 build and left the ASan build silently running a stale
// binary.)
//
// Clang spells it `__has_feature`; GCC spells it `__SANITIZE_ADDRESS__` and
// `__SANITIZE_THREAD__`. UBSan is deliberately absent — it is recoverable and
// barely affects timing, so the absolute budget stays asserted there.
//
// ONE ROW HAS SINCE FOUND AN EXCEPTION TO THAT LAST CLAUSE, and it is worth
// knowing before trusting it again: gloam#32's pathfinder row is dense signed
// integer arithmetic, which is exactly what UBSan instruments, and it measured
// 1.9x slower under it — 2,695 us against 5,182 us at §11's reference scale on
// GCC 14, which is the difference between 67% of the 4 ms budget and 130% of it.
//
// Neither compiler predefines a macro for UBSan, and `cmake/toolchain/
// undefined.cmake` already ships `TEMPLATE_UBSAN` for that reason — so that row
// reads the define that is already there rather than inventing a second one.
// (An earlier attempt did invent one, and `check_artifacts.cmake`'s B3 rule
// caught it: the UBSan define is a coupled pair across two files, and the rule
// exists to keep it one pair rather than two.) Every OTHER absolute budget still
// asserts under UBSan exactly as before.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define GLOAM_TEST_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define GLOAM_TEST_SANITIZED 1
#endif
#endif

TEST_CASE("§4.2's slot inventory fits §11's residency cap", "[budget]") {
  CHECK(budget::resident_images_m0() == 71);
  CHECK(budget::resident_images_full() == 246);
  CHECK(budget::resident_images_full() <= budget::kMaxResidentImages);

  // The row that makes the table add up. See the note in budgets.hpp — the
  // naive sum is 74 and 252, and both are wrong.
  const int naive_m0 = budget::resident_images_m0() + budget::kTransitionSequences.m0;
  const int naive_full = budget::resident_images_full() + budget::kTransitionSequences.full;
  CHECK(naive_m0 == 74);
  CHECK(naive_full == 252);

  // Counting transitions would not blow the cap — 252 still fits under 256 —
  // which is exactly why this is worth a test rather than trust. The mistake is
  // survivable and therefore silent: it would report totals that disagree with
  // §4.2's stated 71 and 246, and it would spend 6 of the 10 remaining slots on
  // things that are not plates. The next person to ask "how much monster
  // breadth can we afford?" would get the wrong answer (§16's cut lever).
  CHECK(naive_full <= budget::kMaxResidentImages);
  CHECK(naive_m0 - budget::resident_images_m0() == budget::kTransitionSequences.m0);
  CHECK(naive_full - budget::resident_images_full() == budget::kTransitionSequences.full);

  // The headroom the art plan actually has.
  CHECK(budget::kMaxResidentImages - budget::resident_images_full() == 10);
}

TEST_CASE("§4.4 ships exactly one light field per lamp level", "[budget]") {
  // Six of them — one per lamp level, L0 opaque through L5 bright. If the lamp
  // ladder ever grows a level, this is what notices the plate set did not.
  CHECK(budget::kLightFields.m0 == kLampLevelCount);
  CHECK(budget::kLightFields.full == kLampLevelCount);
}

TEST_CASE("§11 byte budgets are declared and ordered sanely", "[budget]") {
  // An idle frame costs ZERO bytes (§4.6). Asserted as an equality rather than
  // a bound, because "small" is exactly the failure mode this budget exists to
  // rule out.
  CHECK(budget::kIdleFrameBytes == 0);

  CHECK(budget::kMaxAnimationFrameBytes == 400);
  CHECK(budget::kMaxRecompositionBytes == 2048);
  CHECK(budget::kMaxSustainedBytesPerSecond == 8192);

  // The ordering is itself a claim: an animation-only frame must be cheaper
  // than a full recomposition, or emit-on-change is not buying anything.
  CHECK(budget::kIdleFrameBytes < budget::kMaxAnimationFrameBytes);
  CHECK(budget::kMaxAnimationFrameBytes < budget::kMaxRecompositionBytes);

  // The idle row is now measured, not merely declared — see the §4.6 case below,
  // which runs a frame through gloam::emit::ByteSink. BUDGETS.md's "Per-frame
  // emission" wants the counter to wrap the emit path so it reports what
  // actually left the process rather than what the compositor believed it
  // produced; ByteSink is the produced half, and `tty::write_all` is the half
  // that can report what a syscall accepted.
  //
  // The animation, recomposition and sustained-p95 rows are measured at the end
  // of this file through G-7's real compositor, resident owner and Kitty driver.
}

TEST_CASE("§11 timing budgets are declared", "[budget]") {
  CHECK(budget::kMaxColdStartLocalMs == 800);
  CHECK(budget::kMaxColdStartThrottledMs == 12'000);
  CHECK(budget::kMaxComposeDiffEmitMs == 2);
  CHECK(budget::kMaxAudioLatencyMs == 20);
  CHECK(budget::kMaxColdStartPayloadBytes == 1'200'000);

  // The cold-start rows are no longer PENDING: the transmit path landed, and
  // all three are measured below — the payload row on the real stream, the two
  // timing rows on a real encode and a real write(2).
  // PENDING M0: compose+diff+emit needs the compositor (§19 step 6).
  // PENDING M2: the END-TO-END audio row STILL needs a DEVICE, and this marker
  //             is narrowed rather than deleted because the row is still not
  //             met.
  //
  //             WHAT LANDED SINCE THIS MARKER WAS LAST WRITTEN: gloam#4's second
  //             half — the RtAudio stream, the resident arena and the mixer.
  //             `Command::stamp` is now written by `DeviceSink::play` and read by
  //             `mix::Mixer::render`, so the instrument exists end to end.
  //
  //             WHAT IS NOW ASSERTED WITHOUT A DEVICE, and was not before:
  //               * §9.2's buffer choice fits the budget (the case below);
  //               * the stamp -> mixed arithmetic is EXACT over a synthetic
  //                 clock, and the three terms GLOAM owns fit inside 20 ms (the
  //                 case after that, and test/28voicemix/ for the arithmetic
  //                 itself).
  //
  //             WHAT REMAINS UNMEASURABLE HERE: the DEVICE's own contribution,
  //             `RtAudio::getStreamLatency()`. Neither this project's dev box
  //             nor a GitHub runner has /dev/snd, so the sink never leaves
  //             DeviceState::NoDevice and there is no first sample to measure.
  //             That is hardware, and it is the whole of what is left.
}

TEST_CASE("§9.2's audio buffer choice fits §11's latency budget", "[budget]") {
  // Not the end-to-end row — that needs a device — but the half of it that is
  // fixed by §9.2's own numbers and would silently break the budget if either
  // moved. "One output stream. 48 kHz, float32, 256-frame buffer (~5.3 ms)."
  //
  // Triple-buffered worst case: a command that just missed the current period
  // waits one period to be picked up, one to be mixed and one to reach the
  // device. 3 x 5.33 ms = 16 ms, against a 20 ms budget — so the choice leaves
  // about 4 ms for everything else in the chain, and there is no room to grow
  // the buffer to 512 without blowing the row.
  constexpr int kSampleRateHz = 48'000;
  constexpr int kBufferFrames = 256;
  constexpr int kWorstCasePeriods = 3;

  const int period_us = kBufferFrames * 1'000'000 / kSampleRateHz;
  INFO("one period is " << period_us << " us");
  CHECK(period_us == 5'333);

  const int worst_case_ms = kWorstCasePeriods * period_us / 1000;
  INFO("worst case " << worst_case_ms << " ms against a budget of "
                     << budget::kMaxAudioLatencyMs << " ms");
  CHECK(worst_case_ms <= budget::kMaxAudioLatencyMs);

  // And the headroom is genuinely thin. Pinned so that doubling the buffer is a
  // decision taken here, against this row, rather than a tweak in src/bin/.
  CHECK(kWorstCasePeriods * (kBufferFrames * 2) * 1000 / kSampleRateHz >
        budget::kMaxAudioLatencyMs);

  // The constants above are §9.2's, written out. These are the ones src/bin/
  // actually uses, and they must be the same numbers or this whole case is
  // about a stream nobody opens.
  STATIC_REQUIRE(sfx::kSampleRateHz == kSampleRateHz);
  STATIC_REQUIRE(sfx::kBufferFrames == kBufferFrames);
}

TEST_CASE("§11's tick-to-first-sample, for the two terms GLOAM owns", "[budget][audio]") {
  // NOT the end-to-end row — see the PENDING M2 marker above. The device's own
  // latency is the third term and it needs hardware. What is measurable here is
  // everything on GLOAM's side of the driver, through the REAL mixer.
  //
  // This case is possible only because `mix::Mixer::render` takes the current
  // time as a PARAMETER rather than reading a clock. Over a synthetic clock the
  // instrument's answer is an exact equality; a mixer that called
  // steady_clock::now() itself could only ever be checked with a tolerance, and
  // a tolerance on a 20 ms budget is most of the budget.
  std::vector<float> arena(sfx::kArenaFrames);
  std::array<sfx::Clip, audio::kSoundIdCount> clips{};
  REQUIRE(sfx::synthesise(0x9105A3ULL, arena, std::span<sfx::Clip, audio::kSoundIdCount>{clips}));

  mix::Mixer mixer{std::span<const float>{arena},
                   std::span<const sfx::Clip, audio::kSoundIdCount>{clips}};
  audio::Ring<> ring;

  constexpr int kPeriodUs = sfx::kBufferFrames * 1'000'000 / sfx::kSampleRateHz;

  // A command that just missed the current period waits one full period before
  // the callback that will start it runs. That is the worst case for the term
  // this instrument measures.
  constexpr std::uint64_t kStamp = 1'000'000;
  constexpr std::uint64_t kNow = kStamp + (std::uint64_t{kPeriodUs} * 1000ULL);

  audio::Command command{};
  command.sound = audio::SoundId::PartyFootfall;
  command.gain = audio::kGainUnity;
  command.stamp = kStamp;
  REQUIRE(ring.try_push(command));

  std::vector<float> out(std::size_t{sfx::kBufferFrames} * 2U, 0.0F);
  mixer.render(ring, out, sfx::kBufferFrames, kNow);

  REQUIRE(mixer.started() == 1);
  // Exact, not approximate.
  CHECK(mixer.worst_latency_ns() == kNow - kStamp);

  // Queueing delay plus the two periods it takes for those samples to reach the
  // driver's ear: three periods total, 16 ms, which is the same arithmetic the
  // case above does from constants — but now taken from the instrument that will
  // carry the real figure the day this runs on a machine with a sound card.
  const std::uint64_t owned_us = (mixer.worst_latency_ns() / 1000ULL) + (2ULL * kPeriodUs);
  INFO("GLOAM's own terms are " << owned_us << " us against a budget of "
                                << budget::kMaxAudioLatencyMs << " ms");
  CHECK(owned_us / 1000ULL <= static_cast<std::uint64_t>(budget::kMaxAudioLatencyMs));

  // What is left for the driver, and it is not much. Stated as an assertion so
  // that a future buffer-size change has to come past this line.
  const std::uint64_t driver_budget_us =
      (static_cast<std::uint64_t>(budget::kMaxAudioLatencyMs) * 1000ULL) - owned_us;
  INFO("leaving " << driver_budget_us << " us for the device itself");
  CHECK(driver_budget_us >= 3'000);
}

TEST_CASE("§11's residency cap, measured against a real manifest", "[budget]") {
  // §19 step 5 landed the pack, so the plate count is no longer a declaration:
  // it is read back out of a manifest that was actually assembled.
  //
  // The comparison happens HERE and not in pack.hpp, deliberately. emit.hpp
  // states the rule: "The sink reports; the budget judges; exactly one file can
  // relax a budget." A parser that knows the cap is a parser that can be
  // configured past it.
  const auto field_bytes = plate::blob_bytes(lightfield::kWidthPx, lightfield::kHeightPx);
  std::vector<std::byte> pixels(field_bytes * lightfield::kFieldCount);
  std::vector<pack::Record> records;
  std::vector<std::span<const std::byte>> blobs;

  for (int level = kLampLevelMin; level <= kLampLevelMax; ++level) {
    const auto slot = static_cast<std::size_t>(level - kLampLevelMin);
    const auto blob = std::span{pixels}.subspan(slot * field_bytes, field_bytes);
    REQUIRE(lightfield::bake(level, blob));

    pack::Record r{};
    r.plate_id = static_cast<std::uint16_t>(level);
    r.role = pack::Role::LightField;
    r.depth = pack::kDepthFullFrame;
    r.lateral = pack::Lateral::FullFrame;
    r.codec = pack::Codec::RawPlanes;
    r.w = static_cast<std::uint16_t>(lightfield::kWidthPx);
    r.h = static_cast<std::uint16_t>(lightfield::kHeightPx);
    records.push_back(r);
    blobs.push_back(blob);
  }

  std::vector<std::byte> image(pack::image_bytes(records));
  REQUIRE(pack::assemble(records, blobs, image));
  REQUIRE(pack::verify(image));

  pack::Header header{};
  REQUIRE(pack::read_header(image, header));
  CHECK(header.plate_count <= budget::kMaxResidentImages);
  CHECK(header.plate_count == budget::kLightFields.m0);
  CHECK(header.total_bytes == image.size());

  // A necessary condition, not the budget: the pack cannot be under 1.2 MB on
  // the wire if it is already over 1.2 MB at rest.
  CHECK(image.size() <= budget::kMaxColdStartPayloadBytes);
}

TEST_CASE("§11's cold-start payload row, measured on the real stream", "[budget]") {
  // §11 and BUDGETS.md budget 1.2 MB of BASE64 — the TRANSMIT payload, not the
  // pack. BUDGETS.md names a "manifest test" as the enforcer, and a manifest
  // test can only measure the pack, so the row as written cannot be checked by
  // the thing it names.
  //
  // THIS ROW WAS ASSERTED INVERTED until the transmit path landed (gloam#17,
  // UPSTREAM.md correction 7): kitty was going to be handed PIXELS, a RawPlanes
  // plate expands from 3 bits per pixel to 32, base64 adds another third, and
  // the six light fields alone came to roughly 5.5 MB against a 1.2 MB budget.
  // Rather than record that in four comment blocks and assert nothing, the row
  // asserted the overrun, so that fixing it would go RED and force whoever fixed
  // it to come here and write the real row. This is that row.
  //
  // What changed is the payload format, not the budget: `png.hpp` encodes the
  // plate as a 4-bit indexed PNG and `deflate.hpp` compresses it, so `f=100`
  // carries roughly a fiftieth of what `f=32` would have. The measurement below
  // is not a projection from constants — it bakes the six real fields, encodes
  // them, and transmits them through the real emitter into a real sink.
  std::vector<std::byte> blob(plate::blob_bytes(lightfield::kWidthPx, lightfield::kHeightPx));
  std::vector<std::byte> scratch(png::scratch_bytes(lightfield::kWidthPx, lightfield::kHeightPx));
  std::vector<std::byte> encoded(png::bound(lightfield::kWidthPx, lightfield::kHeightPx));

  deflate::Scratch matcher;

  emit::ByteSink sink;
  std::size_t png_bytes = 0;

  for (int level = kLampLevelMin; level <= kLampLevelMax; ++level) {
    REQUIRE(lightfield::bake(level, blob));
    const auto image = png::encode(plate::PlateView{blob, lightfield::kWidthPx,
                                                    lightfield::kHeightPx},
                                   scratch, matcher, encoded);
    REQUIRE(image.error == png::PngError::None);
    png_bytes += image.bytes;

    const auto id = static_cast<std::uint32_t>(level - kLampLevelMin) + 1;
    REQUIRE(kitty::emit_transmit(sink, std::span{encoded}.first(image.bytes), id).error ==
            kitty::EmitError::None);
  }

  // The whole stream, control data included — which is MORE than the row asks
  // for. BUDGETS.md wants the counter to measure what actually left the process,
  // and what leaves is escape sequences, not naked base64. Measuring the
  // stricter quantity is the only way the row cannot pass on a technicality.
  const auto stream_bytes = sink.size();
  const auto base64_bytes = base64::encoded_size(png_bytes);

  const auto pixels = static_cast<std::size_t>(lightfield::kWidthPx) *
                      static_cast<std::size_t>(lightfield::kHeightPx) *
                      static_cast<std::size_t>(assets::kPlateCount);

  // The blob really is 3 bits per pixel, and that is what makes the pack small.
  CHECK(assets::pixel_bytes() * 8 == pixels * 3);

  INFO("pack " << assets::image_bytes() << " B, PNG " << png_bytes << " B, base64 "
               << base64_bytes << " B, whole stream " << stream_bytes << " B, budget "
               << budget::kMaxColdStartPayloadBytes << " B");
  // Tenths, not integer percent: the true figure is 1.4%, and a percentage
  // truncated to 1% reads LOWER than the claim the prose makes from it.
  WARN("cold-start payload: " << stream_bytes << " B on the wire ("
                              << stream_bytes * 1000 / budget::kMaxColdStartPayloadBytes / 10 << "."
                              << stream_bytes * 1000 / budget::kMaxColdStartPayloadBytes % 10
                              << "% of §11's budget), from " << png_bytes << " B of PNG");

  CHECK(assets::image_bytes() <= budget::kMaxColdStartPayloadBytes);
  CHECK(base64_bytes <= budget::kMaxColdStartPayloadBytes);
  CHECK(stream_bytes <= budget::kMaxColdStartPayloadBytes);

  // A HEADROOM BAND, not decoration. §11's cap is for the whole resident set and
  // M0's inventory is 71 plates (`budget::resident_images_m0()`), of which these
  // six are the largest — they are the only FULL-FRAME plates. A row that passed
  // at 90% of budget with six of seventy-one plates would be a row that has
  // already failed and does not know it yet. Eight-to-one is what the encoder
  // delivers today with an order of magnitude to spare; if a change halves that,
  // this goes red while there is still time to do something about it.
  CHECK(stream_bytes * 8 <= budget::kMaxColdStartPayloadBytes);

  // And what it would have cost the naive way, kept as an assertion rather than
  // as a remark: this is the arithmetic gloam#17 recorded, and it is the reason
  // `f=100` is not an optimisation but the thing that made the row reachable.
  constexpr std::size_t kBytesPerWirePixel = 4;  // kitty f=32 RGBA
  const auto naive_base64 = pixels * kBytesPerWirePixel * 4 / 3;
  CHECK(naive_base64 > budget::kMaxColdStartPayloadBytes);
  CHECK(naive_base64 / stream_bytes >= 8);
}

TEST_CASE("§11's two cold-start timing rows, one measured and one modelled",
          "[budget]") {
  // READ THIS BEFORE QUOTING EITHER NUMBER.
  //
  // BUDGETS.md asks for "cold start, local terminal <= 800 ms" and "cold start,
  // 1 Mbit/s link <= 12 s", enforced by a "startup instrument" and a
  // "synthetic-throttle test". Neither can touch a real terminal in CI, so what
  // is measured here is everything on GLOAM's side of the tty and nothing on the
  // far side of it:
  //
  //   MEASURED  bake, encode, base64, chunk, and a real write(2) of the whole
  //             stream through the same `tty::write_all` main.cpp uses. The
  //             destination is a temporary file rather than a pipe, on purpose:
  //             a pipe holds 64 KiB and a payload that outgrew it would not fail
  //             this test, it would HANG it, and a hung runner reports nothing.
  //   NOT       the terminal's own decode and upload time, which is beyond the
  //             process boundary and unmeasurable from here.
  //
  // The throttled row is arithmetic over the measured byte count — 1 Mbit/s of
  // wire time plus the measured local cost. A test that actually slept for the
  // wire time would spend twelve seconds of CI proving that division works.
  std::vector<std::byte> blob(plate::blob_bytes(lightfield::kWidthPx, lightfield::kHeightPx));
  std::vector<std::byte> scratch(png::scratch_bytes(lightfield::kWidthPx, lightfield::kHeightPx));
  std::vector<std::byte> encoded(png::bound(lightfield::kWidthPx, lightfield::kHeightPx));
  deflate::Scratch matcher;
  emit::ByteSink sink;

  std::FILE* fp = std::tmpfile();
  REQUIRE(fp != nullptr);

  const auto started = std::chrono::steady_clock::now();

  for (int level = kLampLevelMin; level <= kLampLevelMax; ++level) {
    REQUIRE(lightfield::bake(level, blob));
    const auto image =
        png::encode(plate::PlateView{blob, lightfield::kWidthPx, lightfield::kHeightPx}, scratch,
                    matcher, encoded);
    REQUIRE(image.error == png::PngError::None);
    REQUIRE(kitty::emit_transmit(sink, std::span{encoded}.first(image.bytes),
                                 static_cast<std::uint32_t>(level - kLampLevelMin) + 1)
                .error == kitty::EmitError::None);
  }

  const auto written = tty::write_all(fileno(fp), sink.view());
  const auto elapsed = std::chrono::steady_clock::now() - started;
  std::fclose(fp);

  REQUIRE(written.error == tty::WriteError::None);
  REQUIRE(written.bytes_written == sink.size());

  const auto local_ms =
      static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());

  // 1 Mbit/s is 125,000 B/s, so a byte costs 8 microseconds of wire time. Integer
  // arithmetic throughout, as everywhere else in this file.
  const auto throttled_ms = local_ms + static_cast<int>(sink.size() * 8 / 1000);

  INFO("cold start: " << sink.size() << " B in " << local_ms << " ms locally, " << throttled_ms
                      << " ms over a 1 Mbit/s link");
  WARN("§11 cold start: " << local_ms << " ms local (budget " << budget::kMaxColdStartLocalMs
                          << "), " << throttled_ms << " ms throttled (budget "
                          << budget::kMaxColdStartThrottledMs << ") — the GLOAM half only");

  // The throttled row is machine-independent apart from the encode, so it is
  // asserted everywhere, sanitizers included.
  CHECK(throttled_ms <= budget::kMaxColdStartThrottledMs);

#ifndef GLOAM_TEST_SANITIZED
  CHECK(local_ms <= budget::kMaxColdStartLocalMs);
#endif

  // A headroom band on the modelled row, for the reason the payload row has one:
  // these six plates are 6 of M0's 71, and a row that only just fits today is a
  // row that has already failed.
  CHECK(throttled_ms * 4 <= budget::kMaxColdStartThrottledMs);
}

TEST_CASE("§9.2's dropped-voice-command budget is zero", "[budget]") {
  // "A full ring drops the command and increments a counter. It never blocks."
  // The counter is a budget assertion, not a log line — so the budget is zero,
  // not "few".
  CHECK(budget::kMaxDroppedVoiceCommands == 0);
  CHECK(budget::kDroppedVoiceCommandWindowTicks == 1000);
}

TEST_CASE("§9.2's dropped-voice-command budget, measured over its own window", "[budget]") {
  // §19 step 9 landed the ring, so this row stops declaring its constant and
  // starts measuring against it.
  //
  // THE DRAIN CADENCE IS THE WORST CASE, DELIBERATELY. §9.2's 256-frame buffer
  // at 48 kHz means the real consumer wakes roughly EIGHTEEN times per simulated
  // tick; this drains ONCE per tick. If the budget holds when the audio thread
  // gets a single look per tick, it holds comfortably at the cadence the device
  // actually runs at — and a test written at the real cadence would pass on a
  // ring far too small.
  const Tuning& t = kDefaultTuning;

  Level level{16, 3};
  level.carve(Coord{0, 1}, Dir::East, 16);
  // A MIXED ROSTER, SO THE WINDOW CARRIES ALL THREE EMITTERS, and getting that
  // right took a correction worth recording. The first attempt gave all eight
  // monsters routes and left them Keen under a bright lamp — so every one of
  // them escalated within a few ticks and, since a monster at SEARCHING or
  // above holds position (gloam#32), NOT ONE OF THEM EVER TOOK A STEP. The
  // routes were decoration and the window carried exactly the traffic it had
  // carried before §6.4 existed.
  //
  // So: two Keen listeners with no route, who hear the party's plate through
  // the corridor and sting; and six Dull patrollers, whose threshold of 70 is
  // above anything a 44-emission step can deliver at this range, so they never
  // notice and never stop walking. Doused, so nothing is seen either.
  std::vector<Monster> monsters;
  for (int m = 0; m < 8; ++m) {
    const Coord start{4 + m, 1};
    Monster mon{};
    mon.at = start;
    if (m < 2) {
      mon.kind = MonsterKind{Acuity::Keen, false};  // escalates, stings, then holds
    } else {
      mon.kind = MonsterKind{Acuity::Dull, false};
      mon.patrol.route = {start, start.step(Dir::East)};
      mon.patrol.dwell = {0, 0};
    }
    monsters.push_back(mon);
  }

  auto world = make_world(0xA0D10ULL, std::move(level), std::move(monsters));
  world.party = Coord{1, 1};
  world.armour = Armour::Plate;   // loudest emitter, so every tick has a voice
  world.lamp_level = kLampLevelMin;

  audio::RecordingSink<> sink;
  audio::Command drained{};
  int monster_footfalls = 0;

  for (int tick = 0; tick < budget::kDroppedVoiceCommandWindowTicks; ++tick) {
    world.pending_noise = step_noise(world.armour, world.creeping, t);
    advance(world, t, &sink);
    while (sink.ring().try_pop(drained)) {  // the audio thread's one look
      if (drained.sound == audio::SoundId::MonsterFootfall) ++monster_footfalls;
    }
  }

  // The window must have carried real traffic. Without this the row below is
  // satisfied by a ring nothing was ever pushed to — the same vacuous pass
  // test/16audiosim/ guards against, and the reason `pushed()` exists.
  INFO("voices pushed over the window: " << sink.ring().pushed());
  CHECK(sink.ring().pushed() >= static_cast<std::uint64_t>(
                                   budget::kDroppedVoiceCommandWindowTicks));

  // AND BY EMITTER, because the total above is met by the party's own footfall
  // on every tick. These monsters were given routes so the window would carry
  // roughly twice the traffic; a total-only guard cannot tell whether it did.
  INFO("monster footfalls over the window: " << monster_footfalls);
  CHECK(monster_footfalls > 0);

  INFO("dropped " << sink.ring().dropped() << " of " << sink.ring().pushed());
  CHECK(sink.ring().dropped() ==
        static_cast<std::uint64_t>(budget::kMaxDroppedVoiceCommands));
}

TEST_CASE("the drop counter is not merely stuck at zero", "[budget]") {
  // The row above passes if `dropped()` can never increment for any reason —
  // a counter wired to nothing reports a perfect budget forever. This drives
  // the ring past capacity with no consumer at all and requires the number to
  // move, which is what makes the zero above mean something.
  audio::Ring<4> ring;
  for (int i = 0; i < 16; ++i) static_cast<void>(ring.try_push(audio::Command{}));
  CHECK(ring.dropped() == 12);
  CHECK(ring.dropped() > static_cast<std::uint64_t>(budget::kMaxDroppedVoiceCommands));
}

TEST_CASE("§11's simulation tick budget, measured", "[budget]") {
  // The one timing row that is measurable today, because the simulation core
  // exists and the renderer does not.
  //
  // A tick's perception work is: propagate the party's step noise across the
  // level, then run every monster's senses against it. This runs that at a
  // level size and monster count well past M0's corridor, so the headroom is
  // real rather than an artefact of a four-cell test.
  //
  // It calls `gloam::advance` rather than open-coding the loop. This case used
  // to BE the only tick in the tree, and `world.cpp` took it verbatim; running
  // the copy would leave the measured tick free to drift away from the replayed
  // one, and the budget would then be guarding something nothing executes.
  const Tuning& t = kDefaultTuning;

  constexpr int kSide = 32;
  Level level{kSide, kSide};
  for (int y = 0; y < kSide; ++y) level.carve(Coord{0, y}, Dir::East, kSide);
  for (int x = 0; x < kSide; ++x) level.carve(Coord{x, 0}, Dir::South, kSide);

  constexpr int kMonsters = 16;
  std::vector<Monster> monsters;
  for (int m = 0; m < kMonsters; ++m) {
    monsters.push_back(Monster{Coord{2 + m % 28, 2 + m / 4}, MonsterKind{Acuity::Normal, false}, {}});
  }

  auto world = make_world(0xB0DEB0DEULL, std::move(level), std::move(monsters));
  world.party = Coord{1, 1};
  world.armour = Armour::Plate;

  constexpr int kTicks = 100;
  const auto start = std::chrono::steady_clock::now();
  for (int tick = 0; tick < kTicks; ++tick) {
    // Set the emission directly rather than stepping: the party stays put, so
    // every tick is a LOUD tick over the full field. That is the worst case the
    // budget has to hold for, and a walking party would spend most of these
    // ticks propagating silence.
    world.pending_noise = step_noise(world.armour, world.creeping, t);
    advance(world, t);
  }
  const auto elapsed = std::chrono::steady_clock::now() - start;
  const auto per_tick_us =
      std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count() / kTicks;

  // The tick must have DONE something. Delegating to `gloam::advance` bought
  // drift-protection and gave up the property that the work being timed was
  // written here — so without these two lines, a plausible early-out added to
  // `advance` (skip a silent tick, skip a monster out of noise range) would
  // make this row pass faster than ever while measuring almost nothing.
  CHECK(world.tick == kTicks);
  const auto roused = std::count_if(world.monsters.begin(), world.monsters.end(),
                                    [](const Monster& m) {
                                      return m.mind.state != Awareness::Unaware;
                                    });
  INFO("monsters no longer unaware: " << roused << " of " << kMonsters);
  CHECK(roused > 0);

  INFO("per-tick " << per_tick_us << " us against a budget of "
                   << budget::kMaxSimulationTickMs * 1000 << " us");
  CHECK(per_tick_us < budget::kMaxSimulationTickMs * 1000);
}

TEST_CASE("§11's tick budget still holds with sixteen monsters PATROLLING", "[budget]") {
  // THE ROW MOST AT RISK FROM §6.4, and the reason this case mirrors the one
  // above cell for cell: the two numbers are meant to be compared.
  //
  // What §6.4 adds to a tick is O(monsters) integer bookkeeping and, when a
  // monster moves with a sink attached, a SECOND propagation from the party.
  // One field per tick, not one per moving monster — which is the same
  // discipline `world.cpp:230-236` records paying 13.6 ms to learn. A footfall
  // field seeded at 14 reaches seven cells rather than the sting field's
  // forty-five, so it is the cheaper of the two, but "cheaper than the thing
  // that nearly blew the budget" is not an argument. This measures it.
  const Tuning& t = kDefaultTuning;

  constexpr int kSide = 32;
  Level level{kSide, kSide};
  for (int y = 0; y < kSide; ++y) level.carve(Coord{0, y}, Dir::East, kSide);
  for (int x = 0; x < kSide; ++x) level.carve(Coord{x, 0}, Dir::South, kSide);

  // Every monster on its own four-cell east-west route, all of them walking at
  // once. Nothing in play makes sixteen monsters step on the same tick forever;
  // that is why it is the budget's case rather than the game's.
  constexpr int kMonsters = 16;
  std::vector<Monster> monsters;
  for (int m = 0; m < kMonsters; ++m) {
    const Coord start{2 + (m % 7) * 4, 2 + m / 4};
    Monster mon{};
    mon.at = start;
    mon.kind = MonsterKind{Acuity::Normal, false};
    mon.patrol.route = {start, start.step(Dir::East), start.step(Dir::East).step(Dir::East)};
    mon.patrol.dwell = {0, 0, 0};
    monsters.push_back(mon);
  }

  auto world = make_world(0xB0DEB0DEULL, std::move(level), std::move(monsters));
  world.party = Coord{1, 1};
  world.armour = Armour::Plate;

  audio::RecordingSink<> sink;
  audio::Command drained{};
  int footfalls = 0;

  constexpr int kTicks = 100;
  const auto start = std::chrono::steady_clock::now();
  for (int tick = 0; tick < kTicks; ++tick) {
    world.pending_noise = step_noise(world.armour, world.creeping, t);
    advance(world, t, &sink);
    while (sink.ring().try_pop(drained)) {
      if (drained.sound == audio::SoundId::MonsterFootfall) ++footfalls;
    }
  }
  const auto elapsed = std::chrono::steady_clock::now() - start;
  const auto per_tick_us =
      std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count() / kTicks;

  // The traffic guard, for the reason `roused > 0` exists above: a pump that
  // silently stopped moving anything would make this the fastest row in the
  // file and measure nothing at all.
  //
  // COUNTED BY EMITTER, not by `pushed()`. The party emits a footfall on every
  // one of these ticks, so a total above `kTicks` is satisfied by the party
  // alone — the guard would have stayed green with the entire patrol subsystem
  // switched off, which is precisely the class of vacuous pass this file's other
  // guards exist to refuse.
  INFO("monster footfalls over " << kTicks << " ticks: " << footfalls);
  CHECK(footfalls > kTicks);
  CHECK(sink.ring().dropped() == 0);

  INFO("per-tick with patrols " << per_tick_us << " us against a budget of "
                                << budget::kMaxSimulationTickMs * 1000 << " us");
#if defined(GLOAM_TEST_SANITIZED)
  WARN("§11's absolute tick budget is not asserted under a sanitizer — see the worst-case "
       "sting row for the argument.");
#else
  CHECK(per_tick_us < budget::kMaxSimulationTickMs * 1000);
#endif
}

TEST_CASE("§9.2's ring has headroom for every voice one tick can carry", "[budget]") {
  // The worst tick is `2N + 1`: the party's own footfall, plus a footfall and a
  // sting for each of N monsters. At §11's reference scale that is 33 against a
  // capacity of 64.
  //
  // ASSERTED AS THE BOUND ON N RATHER THAN AS THE NUMBER 33, because the number
  // is what changes. `2N + 1 <= 64` gives N <= 31, so the commit that raises
  // the monster count past thirty-one lands on this line and has to think about
  // the ring instead of discovering it as dropped voices in a dark corridor.
  //
  // Note it is NOT the tighter bound this slice could justify. Today a monster
  // cannot move and sting on the same tick — `Tell::GaitChanges` only arrives
  // from SEARCHING, which holds position (gloam#32) — which would make the
  // worst case `N + 1`. Depending on that would be depending on a scope
  // boundary, and boundaries move.
  constexpr std::size_t kReferenceMonsters = 16;
  STATIC_REQUIRE(2 * kReferenceMonsters + 1 <= audio::kVoiceRingCapacity);

  constexpr std::size_t kMaxMonsters = (audio::kVoiceRingCapacity - 1) / 2;
  STATIC_REQUIRE(kMaxMonsters == 31);
  STATIC_REQUIRE(2 * (kMaxMonsters + 1) + 1 > audio::kVoiceRingCapacity);
}

TEST_CASE("§11's tick budget still holds with the audio sink attached", "[budget]") {
  // THE ROW MOST AT RISK FROM §19 STEP 9, AND THE REASON THIS CASE EXISTS.
  //
  // The party's footfall is free — it reads the field `advance` already
  // propagated. A monster's sting is NOT: it is sourced at the monster, so it
  // needs its own `propagate_noise` from the other end. That is the same
  // function `noise.hpp` insists on rather than a second path, but it is not
  // free, and a tick in which every monster transitions at once pays for one
  // propagation per monster.
  //
  // §6.1 makes that pathological — a sting is reserved for a FIRST sighting, so
  // in play it fires once per monster per encounter. But "rare" is not a budget
  // argument, and the honest way to hold this row is to measure the worst case
  // rather than to assume the common one. Same level size and monster count as
  // the case above, so the two numbers are directly comparable.
  const Tuning& t = kDefaultTuning;

  constexpr int kSide = 32;
  Level level{kSide, kSide};
  for (int y = 0; y < kSide; ++y) level.carve(Coord{0, y}, Dir::East, kSide);
  for (int x = 0; x < kSide; ++x) level.carve(Coord{x, 0}, Dir::South, kSide);

  constexpr int kMonsters = 16;
  std::vector<Monster> monsters;
  for (int m = 0; m < kMonsters; ++m) {
    monsters.push_back(
        Monster{Coord{2 + m % 28, 2 + m / 4}, MonsterKind{Acuity::Normal, false}, {}});
  }

  auto world = make_world(0xB0DEB0DEULL, std::move(level), std::move(monsters));
  world.party = Coord{1, 1};
  world.armour = Armour::Plate;
  world.lamp_level = kLampLevelMax;  // lit and in the open: every monster can escalate

  audio::RecordingSink<> sink;
  audio::Command drained{};

  constexpr int kTicks = 100;
  const auto start = std::chrono::steady_clock::now();
  for (int tick = 0; tick < kTicks; ++tick) {
    world.pending_noise = step_noise(world.armour, world.creeping, t);
    advance(world, t, &sink);
    while (sink.ring().try_pop(drained)) {}
  }
  const auto elapsed = std::chrono::steady_clock::now() - start;
  const auto per_tick_us =
      std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count() / kTicks;

  // The stings must actually have fired, or this is timing the footfall path
  // twice and calling it a worst case.
  INFO("voices over " << kTicks << " ticks: " << sink.ring().pushed());
  CHECK(sink.ring().pushed() > static_cast<std::uint64_t>(kTicks));

  INFO("per-tick with audio " << per_tick_us << " us against a budget of "
                              << budget::kMaxSimulationTickMs * 1000 << " us");
  CHECK(per_tick_us < budget::kMaxSimulationTickMs * 1000);
}

TEST_CASE("§11's tick budget holds when sixteen monsters PURSUE", "[budget]") {
  // WHAT THIS ROW MEASURES, AND THE FACT THAT DECIDES ITS SHAPE.
  //
  // A distance field has no cheap case: unlike `propagate_noise`, whose extent
  // is bounded by the emission, it always fills its reachable component and
  // there is no early-out. So the only lever on cost is how many DISTINCT
  // targets a tick asks about.
  //
  // AND SIXTEEN DISTINCT TARGETS REQUIRE SIXTEEN STALE BELIEFS. `step` writes
  // `mind.last_known` from `senses.party_position` on every perception hit, so
  // every monster that can currently see or hear the party believes the SAME
  // cell — the shared field is not a discipline the cache imposes, it is what
  // perception hands it. The expensive tick is therefore the one where sixteen
  // monsters are searching sixteen places the party is not, which is why this
  // case douses the lamp and keeps the party silent rather than lighting it.
  //
  // Measured while writing this, because the first attempt lit the lamp and
  // then reported sixteen "distinct" targets that were all the party's cell: a
  // whole tick came in at 521 us while sixteen fields in isolation cost 2676 us
  // in the same build. Two numbers that cannot both describe the same work.
  const Tuning& t = kDefaultTuning;

  constexpr int kSide = 32;
  constexpr int kMonsters = 16;
  constexpr int kRepeats = 50;
  const Coord party{16, 20};

  // Sixteen cells spread across the level, so no two searches share a source.
  const auto stale_target = [](int m) { return Coord{1 + (m % 4) * 8, 1 + (m / 4) * 8}; };

  const auto build = [&](bool distinct_targets, Awareness state) {
    Level level{kSide, kSide};
    for (int y = 0; y < kSide; ++y) level.carve(Coord{0, y}, Dir::East, kSide);
    for (int x = 0; x < kSide; ++x) level.carve(Coord{x, 0}, Dir::South, kSide);

    std::vector<Monster> monsters;
    for (int m = 0; m < kMonsters; ++m) {
      Monster mon{};
      mon.at = Coord{14 + m % 5, 14 + m / 5};
      mon.kind = MonsterKind{Acuity::Dull, false};  // deaf enough not to re-acquire
      mon.mind.state = state;
      mon.mind.has_last_known = true;
      mon.mind.last_known = distinct_targets ? stale_target(m) : stale_target(0);
      monsters.push_back(mon);
    }
    auto w = make_world(0xC0FFEEULL, std::move(level), std::move(monsters));
    w.party = party;
    w.armour = Armour::Leather;
    w.lamp_level = 0;  // doused: the beliefs above stay stale, which is the point
    return w;
  };

  const auto time_ticks = [&](World& world, bool distinct_targets, Awareness state) {
    const auto rearm = [&] {
      for (std::size_t i = 0; i < world.monsters.size(); ++i) {
        auto& mon = world.monsters[i];
        mon.at = Coord{14 + static_cast<int>(i) % 5, 14 + static_cast<int>(i) / 5};
        mon.move_cooldown = 0;
        mon.mind.state = state;
        mon.mind.ticks_since_hit = 0;  // so §6.1's give-up timer never fires
        mon.mind.last_known = distinct_targets ? stale_target(static_cast<int>(i)) : stale_target(0);
      }
    };
    rearm();
    advance(world, t);  // warm-up, deliberately untimed

    std::chrono::steady_clock::duration total{};
    for (int r = 0; r < kRepeats; ++r) {
      rearm();
      const auto start = std::chrono::steady_clock::now();
      advance(world, t);
      total += std::chrono::steady_clock::now() - start;
    }
    return std::chrono::duration_cast<std::chrono::microseconds>(total).count() / kRepeats;
  };

  auto shared = build(/*distinct_targets=*/false, Awareness::Searching);
  auto distinct = build(/*distinct_targets=*/true, Awareness::Searching);
  auto silent = build(/*distinct_targets=*/false, Awareness::Unaware);

  const auto silent_us = time_ticks(silent, false, Awareness::Unaware);
  const auto shared_us = time_ticks(shared, false, Awareness::Searching);
  const auto distinct_us = time_ticks(distinct, true, Awareness::Searching);

  // THE TRAFFIC GUARD, and without it this is the fastest row in the file while
  // measuring nothing. An early-out in `monster_step`, or a target nobody can
  // path to, would make every arrangement free and leave the ratio comparing two
  // descriptions of an empty tick.
  int moved = 0;
  for (int i = 0; i < kMonsters; ++i) {
    const Coord spawn{14 + i % 5, 14 + i / 5};
    if (distinct.monsters[static_cast<std::size_t>(i)].at != spawn) ++moved;
  }
  INFO("monsters that actually stepped on the last timed tick: " << moved);
  CHECK(moved == kMonsters);

  INFO("one shared target: " << shared_us << " us; " << kMonsters << " distinct: " << distinct_us
                             << " us; no pathing at all: " << silent_us << " us");

  // THE ANTI-REGRESSION INSTRUMENT, and it is a ratio so it survives every leg
  // of the matrix: a sanitizer slows both measurements equally. Reintroduce a
  // field per monster and `shared_us` becomes `distinct_us`, and this goes red
  // on every box rather than only on a slow one — which an absolute budget would
  // not do, since a fast machine fits sixteen searches inside 4 ms and waves the
  // regression through.
  CHECK(distinct_us > shared_us * 2);

  // ── AND THE ABSOLUTE ROW IS NOT ASSERTED, WHICH IS gloam#36 ──────────────
  //
  // Not an omission and not a sanitizer caveat: this row STRADDLES the budget by
  // machine and by compiler, which is a worse thing for a contract to be than
  // simply blown.
  //
  //   dev box, GCC 14 Debug        2,695 us    67% of the 4 ms budget
  //   CI runner, GCC 14 Debug      5,462 us   137%
  //   CI runner, Clang 20 Debug    under it — the same hardware, the other way
  //   dev box, GCC 14 UBSan        5,182 us   130%
  //
  // #17 and #26 are asserted INVERTED because they are blown everywhere, so
  // "assert what is true today and let it go red when someone fixes it" works.
  // It does not work here: an inverted assertion would be red on the dev box and
  // green on CI, and a machine-dependent assertion in EITHER direction is a
  // flaky test wearing a budget's clothes — which this file has already been
  // burned by once, at the worst-case sting row.
  //
  // What is asserted instead is the ratio above, which is machine-independent
  // and is the check that actually polices the shared-field discipline. The
  // absolute figure is measured and PRINTED every run, so it cannot quietly
  // vanish — the same discipline the PENDING rows in this file use, and for the
  // same reason: a row that disappeared would be indistinguishable from one that
  // was never written.
  //
  // Explicitly NOT done about it: a distance cap on the primitive (it makes
  // "far" and "unreachable" the same answer, and §6.1's SEARCHING exit is keyed
  // on exactly that distinction), and a cheaper scenario (choosing a worst case
  // to make a row pass is what BUDGETS.md exists to prevent). gloam#36 carries
  // the escape route and what would close it.
  WARN("§11 simulation tick, sixteen monsters pathing to sixteen distinct targets: "
       << distinct_us << " us against a " << budget::kMaxSimulationTickMs * 1000
       << " us budget — MEASURED, NOT ASSERTED (gloam#36): this row straddles the budget by "
          "machine and by compiler, so neither direction is a stable assertion.");
}

TEST_CASE("§11's tick budget holds when EVERY monster stings on one tick", "[budget]") {
  // THE ACTUAL WORST CASE, CONSTRUCTED RATHER THAN WAITED FOR.
  //
  // The case above is honest about cost but not about concurrency: escalation
  // takes several ticks and §6.1 advances at most one state per tick, so its
  // stings arrive a few at a time and no single tick pays for many
  // propagations. That measures the common case and calls it worst.
  //
  // This puts every monster one step below HUNTING and then satisfies §6.1's
  // sighting condition for all of them simultaneously, so ONE tick performs one
  // sting propagation per monster. If audio can blow the tick budget, it blows
  // it here and nowhere else.
  const Tuning& t = kDefaultTuning;

  constexpr int kSide = 32;
  Level level{kSide, kSide};
  for (int y = 0; y < kSide; ++y) level.carve(Coord{0, y}, Dir::East, kSide);
  for (int x = 0; x < kSide; ++x) level.carve(Coord{x, 0}, Dir::South, kSide);

  // Within sight distance of the party and in clear line of sight, so the
  // SEARCHING -> HUNTING row fires for each of them on the same tick.
  constexpr int kMonsters = 16;
  // OFF THE MONSTERS' OWN CELLS, which it did not have to be until gloam#32. At
  // (16,16) one of the sixteen spawned exactly on the party, and a monster that
  // has already arrived does not step — so the roster below would have been
  // fifteen steppers and one bystander for no stated reason. Six cells south is
  // still inside a Normal monster's sight from every one of them.
  const Coord party{16, 20};

  // One step below HUNTING, looking in the right place. Re-applied before every
  // timed tick below, so each one is a fresh worst case.
  const auto searching_mind = [](Coord seen_at) {
    Perception p{};
    p.state = Awareness::Searching;
    p.has_last_known = true;
    p.last_known = seen_at;
    return p;
  };

  std::vector<Monster> monsters;
  for (int m = 0; m < kMonsters; ++m) {
    monsters.push_back(Monster{Coord{14 + m % 5, 14 + m / 5},
                               MonsterKind{Acuity::Normal, false}, searching_mind(party)});
  }

  auto world = make_world(0xC0FFEEULL, std::move(level), std::move(monsters));
  world.party = party;
  world.armour = Armour::Plate;
  world.lamp_level = kLampLevelMax;
  world.pending_noise = step_noise(world.armour, world.creeping, t);

  // REPEATED AND AVERAGED, not measured once.
  //
  // A single-sample timing assertion is a flaky test wearing a budget's clothes:
  // the first `advance` pays for every first-touch page and every container's
  // initial allocation, and there is nothing to average that against. Measured —
  // one cold tick reported 4324 us under ASan on CI against a 4 ms budget, and
  // 973 us warm on a developer box. The row was not close to being blown; the
  // measurement was wrong.
  //
  // Averaging over repeats is also what the neighbouring case does, so the two
  // numbers stay directly comparable. Each repeat resets every monster to
  // SEARCHING outside the timed region, so every timed tick is a fresh worst
  // case rather than a monster that is already hunting.
  //
  // POSITIONS ARE RESET TOO, SINCE gloam#32, and leaving them out was a real
  // hole rather than an omission. A hunting monster now WALKS, so across fifty
  // repeats the roster closed on the party and parked at arm's reach — and
  // because the silent pass runs first, by the time the sink was attached every
  // monster had already arrived and none of them stepped again. The voice total
  // below stayed exactly right for a scenario that had quietly stopped being the
  // one described: sixteen monsters stinging while standing still. Resetting the
  // position makes every timed tick the tick this case is named after, on which
  // each monster both steps AND stings.
  audio::RecordingSink<> sink;
  audio::Command drained{};
  constexpr int kRepeats = 50;

  std::vector<Coord> spawn;
  spawn.reserve(world.monsters.size());
  for (const auto& m : world.monsters) spawn.push_back(m.at);

  const auto rearm = [&] {
    for (std::size_t i = 0; i < world.monsters.size(); ++i) {
      world.monsters[i].mind = searching_mind(party);
      world.monsters[i].at = spawn[i];
      world.monsters[i].move_cooldown = 0;
    }
    world.pending_noise = step_noise(world.armour, world.creeping, t);
  };

  // BY EMITTER, NEVER BY `pushed()`. A total is satisfiable by the wrong mix —
  // the lesson this file has now learned twice — and the whole point of the
  // reset above is that the mix changed.
  std::array<std::uint64_t, 8> heard{};

  const auto time_ticks = [&](audio::Sink* voices) {
    rearm();
    advance(world, t, voices);  // warm-up, deliberately untimed
    if (voices != nullptr) {
      while (sink.ring().try_pop(drained)) {}
      sink.ring().reset_counters();
      heard.fill(0);
    }

    std::chrono::steady_clock::duration total{};
    for (int r = 0; r < kRepeats; ++r) {
      rearm();
      const auto start = std::chrono::steady_clock::now();
      advance(world, t, voices);
      total += std::chrono::steady_clock::now() - start;
      if (voices != nullptr) {
        while (sink.ring().try_pop(drained)) ++heard[static_cast<std::size_t>(drained.sound)];
      }
    }
    return std::chrono::duration_cast<std::chrono::microseconds>(total).count() / kRepeats;
  };

  const auto silent_us = time_ticks(nullptr);
  const auto tick_us = time_ticks(&sink);

  // Every monster really did sting on every one of those ticks: 16 stings plus
  // one footfall, each repeat. Without this the average could be taken over
  // ticks that were never the worst case at all.
  const auto hunting = std::count_if(world.monsters.begin(), world.monsters.end(),
                                     [](const Monster& m) {
                                       return m.mind.state == Awareness::Hunting;
                                     });
  INFO("monsters that reached HUNTING on the last tick: " << hunting);
  CHECK(hunting == kMonsters);
  INFO("voices over " << kRepeats << " worst-case ticks: " << sink.ring().pushed());
  const auto repeats = static_cast<std::uint64_t>(kRepeats);
  const auto monsters_n = static_cast<std::uint64_t>(kMonsters);
  CHECK(heard[static_cast<std::size_t>(audio::SoundId::HuntingSting)] == repeats * monsters_n);
  CHECK(heard[static_cast<std::size_t>(audio::SoundId::PartyFootfall)] == repeats);
  CHECK(heard[static_cast<std::size_t>(audio::SoundId::MonsterFootfall)] == repeats * monsters_n);
  // Which is `2N + 1` per tick — the bound the ring-headroom case reasons about
  // and, until gloam#32, could not construct.
  CHECK(sink.ring().pushed() == repeats * (2 * monsters_n + 1));

  // ── What audio COSTS, which is the durable form of the question ──────────
  //
  // The same worst-case tick timed with and without the sink. This ratio is what
  // this case actually exists to police, and it is the assertion that survives
  // every build configuration: a sanitizer slows both measurements equally, so
  // the ratio does not move, while an absolute microsecond budget under ASan is
  // mostly measuring ASan.
  //
  // It is also the STRONGER check. Reverting to a propagation per stinging
  // monster costs 16x here and would be caught on any machine; against the
  // absolute budget alone, a fast enough unsanitized box could fit sixteen
  // propagations inside 4 ms and wave the regression through.
  //
  // The shared field means audio adds ONE propagation to a tick that already did
  // one — but not an equal one: the sting propagates at `kStingEmission` (90)
  // where the footfall propagates at a plate step (44), so it reaches further
  // and touches more cells. A little over 2x is therefore the honest expectation,
  // not 2x exactly.
  //
  // Measured across the matrix: 1.2x (clang), 1.6x (gcc), 1.9x (ubsan),
  // 2.2x (asan), 2.3x (tsan). The sanitizers sit highest because the extra field
  // is an allocation and they tax allocation hardest. Four leaves real headroom
  // above the worst observed reading while still catching the regression this
  // exists for by a factor of three or more.
  REQUIRE(silent_us > 0);
  INFO("worst-case tick: " << silent_us << " us silent, " << tick_us << " us with audio");
  CHECK(tick_us < silent_us * 4);

  // ── And the absolute row, where microseconds mean something ──────────────
  //
  // §11's 4 ms is a budget for the simulation, not for a build that is
  // deliberately three to five times slower. Measured on one box: 917 us
  // unsanitized, 3195 us under GCC 13 + ASan — the second number is 80% of the
  // budget and tells you nothing about whether the game is fast enough, but it
  // would fail the build on a busy runner. That is a flaky test, not a budget.
  //
  // So the row is asserted where it is meaningful, and the ratio above carries
  // the sanitizer builds. NOT skipped silently: a dropped assertion that
  // announces nothing is indistinguishable from one that ran.
#if defined(GLOAM_TEST_SANITIZED)
  WARN("§11's absolute tick budget is not asserted under a sanitizer — it would be measuring "
       "the sanitizer. The audio-cost ratio above is what covers this build; the absolute row "
       "runs in the default and ubsan configurations.");
#else
  INFO("worst-case tick " << tick_us << " us against a budget of "
                          << budget::kMaxSimulationTickMs * 1000 << " us");
  CHECK(tick_us < budget::kMaxSimulationTickMs * 1000);
#endif
}

TEST_CASE("§4.5's below-background threshold is reachable only through the layer API", "[budget]") {
  // The threshold's value is asserted in test/06layer/, next to the constant it
  // pins. This case deliberately does NOT spell the literal: two tests asserting
  // the same number in two files is two places for it to drift.
  //
  // What §11 needs from §4.5 is weaker and worth stating on its own — that the
  // back of the compositor is reachable through the named band API, and that it
  // really is below kitty's cell background rather than merely negative.
  CHECK(layer::image_z(layer::Band::BelowBackground, 0) == layer::kBelowBackgroundZ);
  CHECK(layer::kBelowBackgroundZ < -(std::int32_t{1} << 30));
}

TEST_CASE("§4.6's idle frame costs zero bytes, measured at the sink", "[budget]") {
  // §11's headline row, and the first one that stops being a declaration and
  // becomes a measurement. A frame in which the compositor placed nothing must
  // put nothing on the wire — not "almost nothing", not "a cursor move".
  emit::ByteSink sink;
  sink.clear();

  CHECK(sink.size() == budget::kIdleFrameBytes);
  CHECK(sink.total() == budget::kIdleFrameBytes);

  // An idle frame is still a frame. If clear() did not count it, a run of idle
  // frames would be indistinguishable from no frames at all and this row would
  // be unmeasurable rather than merely trivially met.
  CHECK(sink.frames() == 1);
}

// ════════════════════════════════════════════════════════════════════════════
//  TEST-PLAN.md §4's headline: a 200-tick scripted replay, counted at the real
//  compositor's terminal-driver boundary.
// ════════════════════════════════════════════════════════════════════════════

namespace {

auto acknowledge_compositor_pins(resident::PlateSet& plates,
                                  termforge::KittyDriver& driver) -> void {
  for (const auto& record : test_support::compositor_records()) {
    const auto handle = plates.handle(record.plate_id);
    REQUIRE(handle);
    driver.consume_reply(
        termforge::TerminalReply{handle->id, std::nullopt, "OK"});
  }
}

}  // namespace

TEST_CASE("§11's per-frame rows, over a 200-tick scripted replay", "[budget]") {
  const Tuning& t = kDefaultTuning;

  // ── The script, and its density ──────────────────────────────────────────
  //
  // THE STEP DENSITY IS DECIDED HERE AND DEFENDED, NOT TUNED UNTIL THE NUMBER
  // PASSES. §4.7 budgets a 140 ms step transition; at kTickHz = 10 the fastest
  // walk the simulation can express is one step every TWO ticks (200 ms), which
  // is already slower than §4.7 allows. So: a step on every even tick, and the
  // odd ticks alternate a lamp change with a wait. Anything slower would be
  // choosing a script to make a number, and §11's premise is that the numbers
  // are contracts.
  //
  // The waits are here so that all THREE classes are exercised rather than two —
  // a script with no idle tick makes the zero-byte row vacuously true. They move
  // the sustained number DOWNWARD, which is the direction that cannot be
  // accused of inflating the finding below.
  constexpr int kTicks = 200;
  constexpr int kRunLength = 20;  // cells before the party turns around

  Level level{kRunLength + 2, 3};
  level.carve(Coord{0, 1}, Dir::East, kRunLength + 2);

  // ROUTE-LESS, AND THAT IS A DECISION RATHER THAN A DEFAULT. These three
  // monsters must never patrol. `CHECK(idles > 0)` below is what keeps the
  // zero-byte row from going vacuous, and a monster stepping on its own
  // schedule turns idle ticks into animation ticks until there are none left.
  // The patrol rows have their own cases; this one measures §11's headline
  // number and needs a roster that only reacts.
  std::vector<Monster> monsters;
  for (int m = 0; m < 3; ++m) {
    monsters.push_back(Monster{Coord{4 + 5 * m, 1}, MonsterKind{Acuity::Normal, false}, {}});
  }

  auto world = make_world(0x6E1E5ULL, std::move(level), std::move(monsters));
  world.party = Coord{0, 1};
  world.facing = Dir::East;
  world.armour = Armour::Leather;
  for (const auto& m : world.monsters) REQUIRE(m.patrol.route.empty());

  std::vector<replay::Record> script;
  script.reserve(kTicks);
  {
    Dir facing = Dir::East;
    int walked = 0;
    std::int32_t lamp = 3;
    for (std::uint32_t tick = 0; tick < kTicks; ++tick) {
      if (tick % 2 == 0) {
        if (walked == kRunLength) {
          facing = opposite(facing);
          walked = 0;
          script.push_back({tick, replay::Event::Turn, static_cast<std::uint16_t>(facing)});
        } else {
          ++walked;
          script.push_back({tick, replay::Event::Step, static_cast<std::uint16_t>(facing)});
        }
      } else if (tick % 4 == 1) {
        lamp = lamp == 3 ? 4 : 3;
        script.push_back({tick, replay::Event::Lamp, static_cast<std::uint16_t>(lamp)});
      } else {
        script.push_back({tick, replay::Event::Wait, 0});
      }
    }
  }
  REQUIRE(script.size() == kTicks);

  // ── The run ──────────────────────────────────────────────────────────────
  //
  // apply/advance driven here rather than through `play`, because `play` has no
  // per-tick seam and this needs the world both sides of every tick. No seam is
  // added to `play` for it: a byte meter is downstream of a compositor, not of a
  // tick, and `advance` cannot classify from inside because it mutates in place
  // and retains nothing to compare against.
  const auto pack_image = test_support::compositor_pack();
  auto plates = resident::PlateSet::from_pack(pack_image);
  REQUIRE(plates);
  termforge::KittyDriver driver;
  driver.set_cell_pixel_size({geometry::kReferenceCellWidthPx,
                              geometry::kReferenceCellHeightPx});
  std::string wire;
  driver.set_output(&wire);
  REQUIRE(plates->pin_all(driver));
  driver.flush();
  acknowledge_compositor_pins(*plates, driver);
  wire.clear();
  terminal::Compositor renderer{*plates};
  meter::FrameMeter frame_meter;

  int recompositions = 0;
  int animations = 0;
  int idles = 0;

  for (const auto& record : script) {
    const auto before = compositor::snapshot(world);

    apply(world, record.event, record.payload, t);
    advance(world, t);

    const auto after = compositor::snapshot(world);
    const auto frame_class = compositor::classify(before, after);

    const auto staged = renderer.stage(
        world, {geometry::kReferenceCellWidthPx,
                geometry::kReferenceCellHeightPx});
    REQUIRE(staged);
    REQUIRE(*staged == frame_class);
    REQUIRE(renderer.emit(driver));
    driver.flush();
    const bool accepted = !driver.take_output_error().has_value();
    REQUIRE(accepted);
    renderer.finish(accepted);
    const auto bytes = driver.last_frame_bytes().total();
    frame_meter.record(frame_class, bytes);

    switch (frame_class) {
      case meter::FrameClass::Recomposition:
        ++recompositions;
        break;
      case meter::FrameClass::Animation:
        ++animations;
        break;
      case meter::FrameClass::Idle:
        // §4.6: identical list, zero bytes. Asserted INSIDE the loop, because a
        // single idle frame that emitted a cursor move would be invisible in the
        // totals and is exactly the regression this row exists to catch.
        CHECK(bytes == budget::kIdleFrameBytes);
        ++idles;
        break;
    }
  }

  // ── The traffic guard ────────────────────────────────────────────────────
  //
  // Without this the sustained row is satisfiable by a script that never walks —
  // the same defect the voice-ring gate had before it started requiring every
  // emitter by name rather than a nonzero total.
  INFO("frames: " << recompositions << " recomposition, " << animations << " animation, " << idles
                  << " idle");
  CHECK(world.tick == static_cast<std::uint32_t>(kTicks));
  CHECK(frame_meter.frames() == static_cast<std::uint64_t>(kTicks));
  CHECK(recompositions >= 40);

  // EVERY class has to be exercised, not just the expensive one. `peak()` of a
  // class with no samples is 0, so `peak(Idle) == 0` and `peak(Animation) <= 400`
  // both pass over an empty set — the three per-frame rows below would go
  // vacuous rather than red. Concretely: a tuning change that made monsters flip
  // awareness on every wait tick would drive `idles` to zero and silently stop
  // measuring §11's headline row.
  CHECK(idles > 0);
  CHECK(animations > 0);
  CHECK(frame_meter.frames(meter::FrameClass::Idle) == static_cast<std::uint64_t>(idles));
  CHECK(frame_meter.frames(meter::FrameClass::Animation) == static_cast<std::uint64_t>(animations));
  CHECK(frame_meter.frames(meter::FrameClass::Recomposition) ==
        static_cast<std::uint64_t>(recompositions));

  // ── The per-frame rows ───────────────────────────────────────────────────
  INFO("peak recomposition " << frame_meter.peak(meter::FrameClass::Recomposition) << " B");
  INFO("peak animation " << frame_meter.peak(meter::FrameClass::Animation) << " B");
  CHECK(frame_meter.peak(meter::FrameClass::Idle) == budget::kIdleFrameBytes);
  CHECK(frame_meter.peak(meter::FrameClass::Animation) <= budget::kMaxAnimationFrameBytes);
  CHECK(frame_meter.peak(meter::FrameClass::Recomposition) <= budget::kMaxRecompositionBytes);

  // ── The sustained row ────────────────────────────────────────────────────
  //
  // One second of ticks per window, derived rather than spelled: the budget is
  // per SECOND and this is a window SUM, so the units agree only while the two
  // agree. UPSTREAM.md item 12, gloam#25.
  const std::size_t window = replay::kTickHz;

  // The guard, spelled as a literal on purpose. Comparing kTickHz to itself
  // asserts nothing; what has to be caught is the tick rate MOVING, because the
  // budget is per second and this is a window sum, and the two agree only while
  // one second is this many frames. If this goes red, whoever changed the rate
  // has to come back and decide what "per second" means at the new one.
  STATIC_REQUIRE(replay::kTickHz == 10);

  const auto p95 = meter::sustained_p95(frame_meter, window);
  REQUIRE(p95.has_value());

  const auto windows = frame_meter.history().size() - window + 1;
  INFO("sustained p95 " << *p95 << " B/s over " << windows
                        << " one-second windows, against a budget of "
                        << budget::kMaxSustainedBytesPerSecond
                        << " B/s — measured compositor diff");
  // #26 named the escape: whole-image placement omits kitty's default crop
  // keys. G-7 now exercises that real wire form and the real diff.
  CHECK(*p95 <= budget::kMaxSustainedBytesPerSecond);
}

TEST_CASE("a monster that relocates without changing awareness is an ANIMATION frame",
          "[budget]") {
  // CASHING A COMMENT THAT WAS WRITTEN BEFORE THERE WAS ANYTHING TO CASH IT.
  //
  // `FrameState::positions` was captured in this file's harness while `advance`
  // still could not move a monster, against exactly this day: without it a
  // patrolling monster would classify IDLE, be asserted at zero bytes, emit
  // nothing, and quietly falsify the upper-bound claim the 200-tick case makes
  // while CI stayed green. §6.4 is what makes that reachable.
  //
  // BUDGETS.md's animation row is "monster pose, lamp flicker" at <= 400 B, and
  // a monster crossing a cell boundary is the purest example the design has.
  const Tuning& t = kDefaultTuning;

  Level level{12, 3};
  level.carve(Coord{0, 1}, Dir::East, 12);

  // Dull, doused and far from the party: nothing here can move an awareness
  // state, so a change of CLASS can only have come from the body moving.
  Monster mon{};
  mon.at = Coord{3, 1};
  mon.kind = MonsterKind{Acuity::Dull, false};
  mon.patrol.route = {Coord{3, 1}, Coord{4, 1}};
  mon.patrol.dwell = {0, 0};

  auto world = make_world(0x9A14ULL, std::move(level), {mon});
  world.party = Coord{1, 1};
  world.facing = Dir::East;
  world.lamp_level = 0;

  const auto pack_image = test_support::compositor_pack();
  auto plates = resident::PlateSet::from_pack(pack_image);
  REQUIRE(plates);
  termforge::KittyDriver driver;
  driver.set_cell_pixel_size({geometry::kReferenceCellWidthPx,
                              geometry::kReferenceCellHeightPx});
  std::string wire;
  driver.set_output(&wire);
  REQUIRE(plates->pin_all(driver));
  driver.flush();
  acknowledge_compositor_pins(*plates, driver);
  wire.clear();
  terminal::Compositor renderer{*plates};
  meter::FrameMeter frame_meter;

  REQUIRE(renderer.stage(world, {geometry::kReferenceCellWidthPx,
                                 geometry::kReferenceCellHeightPx}));
  REQUIRE(renderer.emit(driver));
  driver.flush();
  REQUIRE_FALSE(driver.take_output_error());
  renderer.finish(true);

  int animations = 0;
  int idles = 0;
  constexpr int kTicks = 12;
  for (int tick = 0; tick < kTicks; ++tick) {
    const auto before = compositor::snapshot(world);
    advance(world, t);
    const auto after = compositor::snapshot(world);

    // The party never acts, so nothing can classify as a recomposition.
    const auto klass = compositor::classify(before, after);
    REQUIRE(klass != meter::FrameClass::Recomposition);
    REQUIRE(before.minds == after.minds);  // awareness is NOT what moved

    REQUIRE(renderer.stage(world, {geometry::kReferenceCellWidthPx,
                                   geometry::kReferenceCellHeightPx}));
    REQUIRE(renderer.emit(driver));
    driver.flush();
    REQUIRE_FALSE(driver.take_output_error());
    renderer.finish(true);
    const auto bytes = driver.last_frame_bytes().total();
    if (klass == meter::FrameClass::Animation) {
      ++animations;
    } else {
      ++idles;
    }
    frame_meter.record(klass, bytes);
  }

  // Both classes exercised, or the two rows below are peak() over an empty set.
  INFO("frames: " << animations << " animation, " << idles << " idle");
  CHECK(animations > 0);
  CHECK(idles > 0);

  CHECK(frame_meter.peak(meter::FrameClass::Idle) == 0);
  CHECK(frame_meter.peak(meter::FrameClass::Animation) <= budget::kMaxAnimationFrameBytes);
  CHECK(frame_meter.peak(meter::FrameClass::Animation) > 0);
}
