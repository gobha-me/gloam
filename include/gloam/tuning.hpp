#pragma once

/// SPEC §6 and §8 — every tunable integer in the simulation, in one place.
///
/// "Every number below is an integer, tunable from one data file, and none of
/// them are floats." (§6)
///
/// Where the design prose says a multiplier — creep is "x 0.5", ESK is "cast
/// noise halved" — it is spelled here as an explicit numerator/denominator
/// pair. A float in simulation state is a determinism bug that only shows up on
/// someone else's CPU, and §5.1 forbids it outright.
///
/// `ruleset_hash()` covers every field. It is written into `replay.gloam`
/// (§12) and a replay recorded against different tuning is REJECTED at load,
/// never silently mis-played — otherwise a golden test starts passing for the
/// wrong reason.

#include <cstdint>

namespace gloam {

/// Monster hearing acuity (§6.2) and sight distance (§6.3) come in three
/// grades. The enum is ordered by acuity, not by threshold value.
enum class Acuity : std::uint8_t { Keen = 0, Normal = 1, Dull = 2 };

/// §4.4 — lamp levels. The integer is load-bearing twice over: it selects the
/// light-field plate AND it is the illumination term in the perception model
/// (§6.3). There is no second light model.
inline constexpr int kLampLevelMin = 0;  ///< doused — navigate from memory and sound
inline constexpr int kLampLevelMax = 5;  ///< bright — wasteful, loud, safe
inline constexpr int kLampLevelDefault = 3;
inline constexpr int kLampLevelCount = kLampLevelMax - kLampLevelMin + 1;

/// §4.7 / §18 Q7 — the step-transition budget, as a named constant rather than
/// a literal. Tune against the real-time pump, where it competes with the
/// monster tick rate.
inline constexpr int kTransitionMs = 140;

/// The tunable set. Defaults are §6 and §8 as frozen in this revision.
struct Tuning {
  // ── §6.2 Noise emitters ───────────────────────────────────────────────────
  // Armour weight is the key coupling: plate is protection bought with
  // audibility, which is what makes the loadout decision tactical rather than a
  // stat-block comparison.
  std::int32_t noise_step_unarmoured{8};
  std::int32_t noise_step_leather{14};
  std::int32_t noise_step_mail{26};
  std::int32_t noise_step_plate{44};

  /// Creeping costs ticks and halves the step's noise.
  std::int32_t creep_tick_cost{2};
  std::int32_t creep_numerator{1};
  std::int32_t creep_denominator{2};

  std::int32_t noise_door{60};
  std::int32_t noise_melee_swing{70};
  std::int32_t noise_melee_hit{90};
  std::int32_t noise_fall{120};
  /// §8.3 — casting emits 10 x the power rune's mana cost.
  std::int32_t noise_cast_per_power{10};

  // ── §6.2 Attenuation per edge (magnitudes; applied as subtraction) ────────
  std::int32_t atten_open_cell{2};
  std::int32_t atten_corner{6};
  std::int32_t atten_open_doorway{8};
  std::int32_t atten_closed_door{40};

  // ── §6.2 Hearing thresholds by type ──────────────────────────────────────
  std::int32_t hear_threshold_keen{20};
  std::int32_t hear_threshold_normal{40};
  std::int32_t hear_threshold_dull{70};
  /// §18 Q5 — a HUNTING monster listens harder. Subtracted from the threshold.
  std::int32_t hear_hunting_bonus{10};

  // ── §6.3 Sight distance in cells, by type ────────────────────────────────
  std::int32_t sight_short{4};
  std::int32_t sight_medium{6};
  std::int32_t sight_long{8};

  // ── §6.1 Awareness timings, in ticks ─────────────────────────────────────
  /// SUSPICIOUS -> SEARCHING on a second hit inside this window...
  std::int32_t suspicious_second_hit_window{30};
  /// ...or on this many ticks of unbroken line of sight.
  std::int32_t suspicious_los_ticks{3};
  /// HUNTING -> LOST_TRACK after this many ticks with no perception hit.
  std::int32_t hunting_lost_ticks{8};
  /// LOST_TRACK -> UNAWARE after this many ticks.
  std::int32_t lost_track_ticks{40};

  // ── §4.4 Lamp fuel, per 100 ticks, indexed by lamp level 0..5 ────────────
  std::int32_t fuel_per_100_ticks[kLampLevelCount]{0, 1, 1, 2, 3, 4};

  // ── §8.2 Power rune mana costs, in slot order KAI TOR PEL DUN BRAM GOTH ──
  std::int32_t mana_cost[6]{1, 2, 3, 5, 8, 13};

  // ── §8.2 Modifier arithmetic ─────────────────────────────────────────────
  std::int32_t arr_duration_multiplier{3};  ///< ARR — extend: duration x3...
  std::int32_t arr_cost_multiplier{2};      ///< ...cost x2
  std::int32_t esk_noise_numerator{1};      ///< ESK — quieten: cast noise halved
  std::int32_t esk_noise_denominator{2};
  std::int32_t ith_target_count{2};         ///< ITH — split: two targets...
  std::int32_t ith_magnitude_numerator{1};  ///< ...half magnitude
  std::int32_t ith_magnitude_denominator{2};
  std::int32_t oth_delay_ticks{10};         ///< OTH — delay: fires 10 ticks later

  [[nodiscard]] auto operator==(const Tuning&) const -> bool = default;

  /// Visits every tunable scalar exactly once, in a fixed order.
  ///
  /// `ruleset_hash()` is the only caller today, but the ordering is the
  /// contract: it is what makes the hash reproducible, so DO NOT reorder these
  /// lines to tidy them. Adding a field means adding a line here — and the
  /// static_assert below is what makes forgetting a compile error rather than
  /// a replay that validates against tuning it was not recorded under.
  template <typename F>
  constexpr void visit_fields(F&& f) const {
    f(noise_step_unarmoured); f(noise_step_leather);
    f(noise_step_mail);       f(noise_step_plate);
    f(creep_tick_cost);       f(creep_numerator);      f(creep_denominator);
    f(noise_door);            f(noise_melee_swing);    f(noise_melee_hit);
    f(noise_fall);            f(noise_cast_per_power);

    f(atten_open_cell);       f(atten_corner);
    f(atten_open_doorway);    f(atten_closed_door);

    f(hear_threshold_keen);   f(hear_threshold_normal);
    f(hear_threshold_dull);   f(hear_hunting_bonus);

    f(sight_short);           f(sight_medium);         f(sight_long);

    f(suspicious_second_hit_window); f(suspicious_los_ticks);
    f(hunting_lost_ticks);           f(lost_track_ticks);

    for (const auto v : fuel_per_100_ticks) f(v);
    for (const auto v : mana_cost) f(v);

    f(arr_duration_multiplier); f(arr_cost_multiplier);
    f(esk_noise_numerator);     f(esk_noise_denominator);
    f(ith_target_count);
    f(ith_magnitude_numerator); f(ith_magnitude_denominator);
    f(oth_delay_ticks);
  }

  // ── Lookups ──────────────────────────────────────────────────────────────

  [[nodiscard]] constexpr auto hearing_threshold(Acuity a) const -> std::int32_t {
    switch (a) {
      case Acuity::Keen: return hear_threshold_keen;
      case Acuity::Normal: return hear_threshold_normal;
      case Acuity::Dull: return hear_threshold_dull;
    }
    return hear_threshold_normal;
  }

  [[nodiscard]] constexpr auto sight_distance(Acuity a) const -> std::int32_t {
    switch (a) {
      case Acuity::Keen: return sight_long;
      case Acuity::Normal: return sight_medium;
      case Acuity::Dull: return sight_short;
    }
    return sight_medium;
  }

  /// §8.2 — mana cost of a power rune, by its index in the power slot.
  [[nodiscard]] constexpr auto power_mana(int power_index) const -> std::int32_t {
    if (power_index < 0 || power_index >= 6) return 0;
    return mana_cost[power_index];
  }
};

/// The count of scalars `visit_fields` covers. Kept as a compile-time check so
/// a field added to the struct but forgotten in `visit_fields` fails the build
/// instead of silently falling out of `ruleset_hash()`.
inline constexpr std::size_t kTuningFieldCount = [] {
  std::size_t n = 0;
  Tuning{}.visit_fields([&n](std::int32_t) { ++n; });
  return n;
}();

static_assert(kTuningFieldCount == 47,
              "a tunable was added or removed: update visit_fields and this count together, "
              "then expect every recorded replay to be rejected at load — which is correct");

// Every field is the same width, so the struct is exactly the visited scalars.
// A field of a different type, or one the visitor missed, breaks this.
static_assert(sizeof(Tuning) == kTuningFieldCount * sizeof(std::int32_t),
              "Tuning gained a field that visit_fields does not cover");

/// FNV-1a over every tunable, in `visit_fields` order.
///
/// Written into the replay header (§12). A replay whose `ruleset_hash` does not
/// match the running build is rejected at load.
[[nodiscard]] constexpr auto ruleset_hash(const Tuning& t) -> std::uint64_t {
  std::uint64_t h = 0xCBF29CE484222325ULL;
  t.visit_fields([&h](std::int32_t v) {
    const auto u = static_cast<std::uint32_t>(v);
    for (int byte = 0; byte < 4; ++byte) {
      h ^= (u >> (byte * 8)) & 0xFFU;
      h *= 0x100000001B3ULL;
    }
  });
  return h;
}

/// The frozen defaults.
inline constexpr Tuning kDefaultTuning{};

}  // namespace gloam
