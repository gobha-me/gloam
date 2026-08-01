/// SPEC §9.3 — the gain and pan derivation.
///
/// The failure matrix comes first (AGENTS.md): degenerate emissions, the
/// boundaries of both scales, the rotation table, and hostile coordinates. The
/// happy path is last and least interesting.
///
/// The case that matters most here is the ROTATION TABLE. Pan is the only place
/// in this subsystem where a sign error produces output that is plausible,
/// self-consistent, and wrong — the game would simply pan everything to the
/// wrong side, forever, and no other test in the suite would notice. Four
/// facings against four bearings is sixteen assertions that cost nothing.

#include <catch2/catch_all.hpp>

#include "gloam/audio.hpp"
#include "gloam/level.hpp"
#include "gloam/noise.hpp"
#include "gloam/tuning.hpp"

using namespace gloam;
using namespace gloam::audio;

namespace {

/// A straight east-west corridor with a side passage, wide enough that a
/// listener can sit away from every wall.
[[nodiscard]] auto corridor() -> Level {
  Level level{12, 3};
  level.carve(Coord{0, 1}, Dir::East, 12);
  return level;
}

}  // namespace

// ── gain: the degenerate inputs, first ──────────────────────────────────────

TEST_CASE("a non-positive emission is silent rather than a division by zero", "[audio][gain]") {
  // The first thing that reaches this function from a fuzzer, and the one that
  // would be UB rather than a wrong answer.
  CHECK(gain_from_loudness(50, 0) == kGainSilent);
  CHECK(gain_from_loudness(50, -1) == kGainSilent);
  CHECK(gain_from_loudness(0, 0) == kGainSilent);
  CHECK(gain_from_loudness(-7, -13) == kGainSilent);
}

TEST_CASE("an inaudible arrival is silent, and that is a value not an error", "[audio][gain]") {
  // `NoiseField::at` returns 0 both for "the sound never reached here" and for
  // "that coordinate is off the level". noise.hpp says callers need not
  // distinguish them; the mix must honour that rather than treating either as
  // exceptional.
  CHECK(gain_from_loudness(0, 90) == kGainSilent);
  CHECK(gain_from_loudness(-3, 90) == kGainSilent);
}

TEST_CASE("gain never exceeds unity, even when the arrival exceeds its own emission",
          "[audio][gain]") {
  // `propagate_noise` cannot produce this — attenuation only subtracts — but the
  // function is public and constexpr, and a mix louder than the model the
  // monster uses is exactly the decoupling §9.3 exists to prevent. Clamp, do not
  // trust.
  CHECK(gain_from_loudness(200, 90) == kGainUnity);
  CHECK(gain_from_loudness(91, 90) == kGainUnity);
  CHECK(gain_from_loudness(90, 90) == kGainUnity);  // the boundary itself
}

TEST_CASE("gain truncates toward silence rather than rounding", "[audio][gain]") {
  // The direction is deliberate and worth pinning: truncation can only make a
  // voice quieter than the model says. Erring quiet errs toward "you are heard
  // more easily than you hear", which is the honest bias for a stealth game.
  //
  // A change to rounding (`+ emission / 2`) would show up here and nowhere else
  // in the suite.
  CHECK(gain_from_loudness(1, 3) == 341);  // 1024/3 = 341.33
  CHECK(gain_from_loudness(2, 3) == 682);  // 1024*2/3 = 682.67
  CHECK(gain_from_loudness(1, 7) == 146);  // 1024/7 = 146.28
}

TEST_CASE("gain is linear in the residual, because §6.2's attenuation is", "[audio][gain]") {
  // Half the emission arriving is half gain. A dB curve here would break this
  // and would mean the player's sense of distance and the monster's threshold
  // test disagree about what "far" means.
  CHECK(gain_from_loudness(7, 14) == kGainUnity / 2);
  CHECK(gain_from_loudness(32, 128) == kGainUnity / 4);
  CHECK(gain_from_loudness(96, 128) == kGainUnity * 3 / 4);
}

TEST_CASE("gain survives an emission large enough to overflow a 32-bit scale", "[audio][gain]") {
  // `kGainUnity * heard` is 1024x, which overflows int32 well before int32's own
  // limit. The widening to int64 inside is what stops this being UB; without it
  // the UBSan job would find it and a release build would not.
  CHECK(gain_from_loudness(1'000'000, 2'000'000) == kGainUnity / 2);
  CHECK(gain_from_loudness(1, 2'000'000'000) == 0);
  CHECK(gain_from_loudness(2'000'000'000 / 2, 2'000'000'000) == kGainUnity / 2);
}

// ── pan: the rotation, which is the bug-prone part ──────────────────────────

TEST_CASE("a source at the listener's own cell centres rather than dividing by zero",
          "[audio][pan]") {
  const Coord here{5, 5};
  for (int d = 0; d < kDirCount; ++d) {
    CHECK(pan_from_bearing(static_cast<Dir>(d), here, here) == kPanCentre);
  }
}

TEST_CASE("pan rotates into view space: four facings against four bearings", "[audio][pan]") {
  // THE TABLE THAT CATCHES THE SIGN ERROR. `y` grows SOUTH (Coord::step), so
  // "north is up" and "north is -y" are the same statement, and inverting it
  // swaps left and right for two of the four facings only — which is why a
  // spot-check of one facing would pass a broken implementation.
  //
  // Read a row as: facing this way, a source lying in that compass direction
  // should be heard from this side.
  const Coord me{5, 5};
  const Coord north{5, 4};
  const Coord east{6, 5};
  const Coord south{5, 6};
  const Coord west{4, 5};

  SECTION("facing north") {
    CHECK(pan_from_bearing(Dir::North, me, north) == kPanCentre);  // ahead
    CHECK(pan_from_bearing(Dir::North, me, east) == kPanFull);     // starboard
    CHECK(pan_from_bearing(Dir::North, me, west) == -kPanFull);    // port
    CHECK(pan_from_bearing(Dir::North, me, south) == kPanCentre);  // astern — see below
  }
  SECTION("facing east") {
    CHECK(pan_from_bearing(Dir::East, me, east) == kPanCentre);
    CHECK(pan_from_bearing(Dir::East, me, south) == kPanFull);
    CHECK(pan_from_bearing(Dir::East, me, north) == -kPanFull);
    CHECK(pan_from_bearing(Dir::East, me, west) == kPanCentre);
  }
  SECTION("facing south") {
    CHECK(pan_from_bearing(Dir::South, me, south) == kPanCentre);
    CHECK(pan_from_bearing(Dir::South, me, west) == kPanFull);
    CHECK(pan_from_bearing(Dir::South, me, east) == -kPanFull);
    CHECK(pan_from_bearing(Dir::South, me, north) == kPanCentre);
  }
  SECTION("facing west") {
    CHECK(pan_from_bearing(Dir::West, me, west) == kPanCentre);
    CHECK(pan_from_bearing(Dir::West, me, north) == kPanFull);
    CHECK(pan_from_bearing(Dir::West, me, south) == -kPanFull);
    CHECK(pan_from_bearing(Dir::West, me, east) == kPanCentre);
  }
}

TEST_CASE("a source dead astern centres, and that is the documented limitation",
          "[audio][pan]") {
  // NOT AN OVERSIGHT, AND NOT TO BE "FIXED" WITHOUT DECIDING IT.
  //
  // Two channels cannot distinguish ahead from behind; resolving it needs a
  // second cue, which is a mix decision §9 does not make. Asserted here so the
  // ambiguity is on the record rather than discovered by a player wondering why
  // the monster behind them sounded like it was in front.
  const Coord me{5, 5};
  CHECK(pan_from_bearing(Dir::North, me, Coord{5, 4}) ==
        pan_from_bearing(Dir::North, me, Coord{5, 6}));
}

TEST_CASE("pan is bounded by hard left and hard right for every offset", "[audio][pan]") {
  const Coord me{16, 16};
  for (int d = 0; d < kDirCount; ++d) {
    for (std::int32_t y = 0; y < 32; ++y) {
      for (std::int32_t x = 0; x < 32; ++x) {
        const auto pan = pan_from_bearing(static_cast<Dir>(d), me, Coord{x, y});
        CHECK(pan >= -kPanFull);
        CHECK(pan <= kPanFull);
      }
    }
  }
}

TEST_CASE("a source abeam pans hard, and only a source abeam does", "[audio][pan]") {
  // |pan| == kPanFull exactly when the forward component is zero. This is what
  // makes the normalisation's endpoints meaningful rather than approximate.
  const Coord me{20, 20};
  CHECK(pan_from_bearing(Dir::North, me, Coord{25, 20}) == kPanFull);   // due starboard
  CHECK(pan_from_bearing(Dir::North, me, Coord{25, 19}) < kPanFull);    // starboard and ahead
  CHECK(pan_from_bearing(Dir::North, me, Coord{25, 19}) > kPanCentre);
  CHECK(pan_from_bearing(Dir::North, me, Coord{21, 15}) < kPanFull / 2);  // mostly ahead
}

TEST_CASE("pan is monotone as a source swings from ahead to abeam", "[audio][pan]") {
  const Coord me{0, 0};
  Pan previous = kPanCentre;
  // Fixed lateral offset, closing forward distance: the bearing swings steadily
  // from near-ahead to abeam and pan must not wobble.
  for (std::int32_t forward = 8; forward >= 0; --forward) {
    const auto pan = pan_from_bearing(Dir::North, me, Coord{4, -forward});
    CHECK(pan >= previous);
    previous = pan;
  }
  CHECK(previous == kPanFull);
}

TEST_CASE("an extreme coordinate does not overflow the pan arithmetic", "[audio][pan]") {
  // `Coord` is signed and `Level` does not enforce its bounds on the arithmetic,
  // so a coordinate read off a disk can be anything. Without the clamp,
  // `kPanFull * right` is signed overflow — UB, which the UBSan job would catch
  // only if a test drove it. This is that test.
  const Coord me{0, 0};
  for (int d = 0; d < kDirCount; ++d) {
    const auto facing = static_cast<Dir>(d);
    CHECK(pan_from_bearing(facing, me, Coord{INT32_MAX, INT32_MAX}) >= -kPanFull);
    CHECK(pan_from_bearing(facing, me, Coord{INT32_MAX, INT32_MAX}) <= kPanFull);
    CHECK(pan_from_bearing(facing, me, Coord{INT32_MIN, INT32_MIN}) >= -kPanFull);
    CHECK(pan_from_bearing(facing, me, Coord{INT32_MIN, INT32_MIN}) <= kPanFull);
    CHECK(pan_from_bearing(facing, Coord{INT32_MIN, 0}, Coord{INT32_MAX, 0}) <= kPanFull);
  }
}

// ── the mix, and the rule that there is only one propagation path ───────────

TEST_CASE("mix_for is propagate_noise read at the listener, and nothing else", "[audio][mix]") {
  // THE RULE noise.hpp STATES BY NAME: "the audio mix calls the SAME
  // propagation ... do not add a second propagation path for audio."
  //
  // Asserted as EQUALITY against a hand-rolled propagation rather than as
  // similar behaviour, because a second path would start out agreeing and drift
  // later — which is precisely the failure the rule exists to prevent, and it
  // would be invisible to a test that only checked the gain looked reasonable.
  const auto level = corridor();
  const Tuning& t = kDefaultTuning;
  const Coord listener{1, 1};
  constexpr std::int32_t kEmission = 90;

  for (std::int32_t x = 0; x < 12; ++x) {
    const Coord source{x, 1};
    const auto field = propagate_noise(level, source, kEmission, t);
    const auto expected_gain = gain_from_loudness(field.at(level, listener), kEmission);
    const auto expected_pan = pan_from_bearing(Dir::East, listener, source);

    const auto mix = mix_for(level, listener, Dir::East, source, kEmission, t);
    CHECK(mix.gain == expected_gain);
    CHECK(mix.pan == expected_pan);
  }
}

TEST_CASE("a source at the listener is heard at unity and centred", "[audio][mix]") {
  // The party's own footfall. `propagate_noise` seeds the source cell with the
  // full emission, so this is unity by construction — and it is the reading
  // `advance` takes for SoundId::PartyFootfall.
  const auto level = corridor();
  const Coord me{4, 1};
  const auto mix = mix_for(level, me, Dir::North, me, 14, kDefaultTuning);
  CHECK(mix.gain == kGainUnity);
  CHECK(mix.pan == kPanCentre);
}

TEST_CASE("gain falls off monotonically with graph distance", "[audio][mix][property]") {
  // §13.3's first property test, read from the PLAYER's side rather than the
  // monster's. The two readings are the same field, which is the entire claim
  // of §9.3.
  const auto level = corridor();
  const Coord source{0, 1};
  Gain previous = kGainUnity + 1;
  for (std::int32_t x = 0; x < 12; ++x) {
    const auto mix = mix_for(level, Coord{x, 1}, Dir::East, source, 90, kDefaultTuning);
    CHECK(mix.gain <= previous);
    previous = mix.gain;
  }
}

TEST_CASE("a closed door attenuates the mix exactly as it attenuates a monster's hearing",
          "[audio][mix]") {
  // The coupling §9.3 exists to create, stated as a test: retuning
  // `atten_closed_door` must move the player's mix and the monster's threshold
  // test together, because they read one field.
  Level level{4, 3};
  level.carve(Coord{0, 1}, Dir::East, 4);
  auto quiet = kDefaultTuning;

  const auto open_mix = mix_for(level, Coord{3, 1}, Dir::East, Coord{0, 1}, 90, quiet);

  // Shut a door between the two ends and the same emission must arrive quieter.
  // `link`, not a direct edge write: it sets BOTH sides, and an edge that
  // disagrees with its twin conducts sound in one direction only.
  level.link(Coord{1, 1}, Dir::East, Edge{EdgeKind::Door, EdgeState::Closed, 0, 0});
  const auto shut_mix = mix_for(level, Coord{3, 1}, Dir::East, Coord{0, 1}, 90, quiet);

  CHECK(shut_mix.gain < open_mix.gain);
  CHECK(shut_mix.pan == open_mix.pan);  // a door changes loudness, not bearing
}

TEST_CASE("reciprocity: a field rooted at the listener gives the same mix", "[audio][mix]") {
  // THE PROPERTY THAT LETS `advance` PROPAGATE ONCE PER TICK INSTEAD OF ONCE PER
  // STINGING MONSTER, and the one that would silently produce wrong gains if it
  // stopped holding.
  //
  // Symmetry is asserted FIRST, exactly as test/04perception asserts a symmetric
  // graph before testing that line of sight is symmetric. Reciprocity is a
  // theorem about an undirected graph with symmetric edge weights, not a
  // coincidence — so if the level is asymmetric this test must fail on the
  // precondition rather than on the conclusion.
  // Long enough that the far end is genuinely out of earshot, with a closed door
  // partway so the two regimes are not just "near" and "far". At 2 per open cell
  // and 40 for the shut door, a sting of 90 dies around x = 26 — so both sides
  // of the boundary are exercised, which the non-vacuity checks below enforce.
  Level level{48, 3};
  level.carve(Coord{0, 1}, Dir::East, 48);
  level.carve(Coord{4, 1}, Dir::North, 2);
  level.link(Coord{6, 1}, Dir::East, Edge{EdgeKind::Door, EdgeState::Closed, 0, 0});
  REQUIRE(level.symmetric());

  const Tuning& t = kDefaultTuning;
  const Coord listener{0, 1};
  constexpr std::int32_t kEmission = audio::kStingEmission;

  // One field rooted at the listener, reused for every source — which is exactly
  // what `advance` does with its sting field.
  const auto from_listener = propagate_noise(level, listener, kEmission, t);

  bool saw_audible = false;
  bool saw_silent = false;
  for (std::size_t i = 0; i < level.cell_count(); ++i) {
    const auto source = level.coord_of(i);
    if (!level.navigable(source)) continue;

    const auto reciprocal =
        mix_reciprocal(from_listener, level, listener, Dir::East, source, kEmission);
    const auto direct = mix_for(level, listener, Dir::East, source, kEmission, t);

    INFO("source (" << source.x << "," << source.y << ")");
    CHECK(reciprocal == direct);

    if (reciprocal.gain > kGainSilent) saw_audible = true;
    if (reciprocal.gain == kGainSilent) saw_silent = true;
  }

  // Non-vacuity, both ways. A level where everything is audible would not
  // exercise the truncation the sizing rule exists for, and one where nothing is
  // would prove only that zero equals zero — the closed door above is placed to
  // guarantee both cases occur.
  CHECK(saw_audible);
  CHECK(saw_silent);
}

TEST_CASE("a field seeded below the emission truncates, which is why the sizing rule exists",
          "[audio][mix]") {
  // The failure `mix_reciprocal`'s sizing rule prevents, demonstrated rather
  // than described. Seed the shared field too quietly and a source that should
  // have been audible goes silent — a bug that would present as "the sting
  // sometimes does not play", intermittently, at distance.
  // Eleven open-cell hops from listener to source, so the quietest path costs
  // 11 x atten_open_cell = 22. A sting of 90 arrives at 68 — clearly audible.
  // A field seeded at 22 dies exactly at the source and reports silence.
  Level level{12, 3};
  level.carve(Coord{0, 1}, Dir::East, 12);
  const Tuning& t = kDefaultTuning;
  const Coord listener{0, 1};
  const Coord source{11, 1};
  REQUIRE(11 * t.atten_open_cell == audio::kStingEmission / 4);

  const auto correct = propagate_noise(level, listener, audio::kStingEmission, t);
  const auto starved = propagate_noise(level, listener, audio::kStingEmission / 4, t);

  CHECK(mix_reciprocal(correct, level, listener, Dir::East, source, audio::kStingEmission).gain >
        kGainSilent);
  CHECK(mix_reciprocal(starved, level, listener, Dir::East, source, audio::kStingEmission).gain ==
        kGainSilent);
}

TEST_CASE("mix_at reads a caller's field without propagating again", "[audio][mix]") {
  // The overload `advance` uses for the footfall: the field the monsters are
  // about to be tested against, reused rather than recomputed. Equality with
  // mix_for over the same field is what says the two overloads cannot drift.
  const auto level = corridor();
  const Coord source{2, 1};
  const auto field = propagate_noise(level, source, 90, kDefaultTuning);

  for (std::int32_t x = 0; x < 12; ++x) {
    const Coord listener{x, 1};
    const auto from_field = mix_at(field, level, listener, Dir::North, source, 90);
    const auto from_scratch = mix_for(level, listener, Dir::North, source, 90, kDefaultTuning);
    CHECK(from_field == from_scratch);
  }
}

TEST_CASE("an out-of-bounds source or listener is silent, not a crash", "[audio][mix]") {
  const auto level = corridor();
  CHECK(mix_for(level, Coord{1, 1}, Dir::North, Coord{-5, -5}, 90, kDefaultTuning).gain ==
        kGainSilent);
  CHECK(mix_for(level, Coord{999, 999}, Dir::North, Coord{1, 1}, 90, kDefaultTuning).gain ==
        kGainSilent);
  CHECK(mix_for(level, Coord{1, 1}, Dir::North, Coord{1, 1}, 0, kDefaultTuning).gain ==
        kGainSilent);
}

// ── the smoke check ─────────────────────────────────────────────────────────

TEST_CASE("the two sound ids and their scales are what §9 froze", "[audio]") {
  CHECK(static_cast<int>(SoundId::None) == 0);
  CHECK(static_cast<int>(SoundId::PartyFootfall) == 1);
  CHECK(static_cast<int>(SoundId::HuntingSting) == 2);

  // Powers of two, so the device's conversion to float is exact.
  CHECK(kGainUnity == 1024);
  CHECK(kPanFull == 256);
  CHECK((kGainUnity & (kGainUnity - 1)) == 0);
  CHECK((kPanFull & (kPanFull - 1)) == 0);

  // The sting sits at melee-hit loudness. Pinned so that moving it is a
  // decision rather than a typo — and note it is NOT a Tuning field, so moving
  // it does not invalidate a single recorded replay.
  CHECK(kStingEmission == kDefaultTuning.noise_melee_hit);
}
