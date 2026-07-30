#pragma once

/// SPEC §6.2 — noise emission and propagation.
///
/// Every action emits an integer noise value at a grid position. Noise
/// propagates through the cell graph with per-edge attenuation. A monster hears
/// if attenuated noise at its own position exceeds its threshold.
///
/// §9.3 is the reason this is a free function over the level rather than a
/// method on the monster: the audio mix calls the SAME propagation, evaluated
/// from the party's position instead of the monster's. What the player hears
/// and what the monster hears are one system read from two positions, so
/// tuning attenuation tunes the stealth model and the mix together. Do not add
/// a second propagation path for audio.

#include <cstdint>
#include <vector>

#include "gloam/level.hpp"
#include "gloam/tuning.hpp"

namespace gloam {

/// Armour is the key coupling in §6.2: protection bought with audibility.
enum class Armour : std::uint8_t { None = 0, Leather = 1, Mail = 2, Plate = 3 };

/// The noise a single step emits. Creeping costs `creep_tick_cost` ticks and
/// halves the value — as integer division, never a float.
[[nodiscard]] constexpr auto step_noise(Armour a, bool creeping, const Tuning& t) -> std::int32_t {
  std::int32_t base = 0;
  switch (a) {
    case Armour::None: base = t.noise_step_unarmoured; break;
    case Armour::Leather: base = t.noise_step_leather; break;
    case Armour::Mail: base = t.noise_step_mail; break;
    case Armour::Plate: base = t.noise_step_plate; break;
  }
  if (!creeping) return base;
  if (t.creep_denominator == 0) return base;
  return base * t.creep_numerator / t.creep_denominator;
}

/// §8.3 — casting emits at 10 x the power rune's mana cost. GOTH in a quiet
/// corridor is 130, audible to a dull-eared monster through a closed door.
[[nodiscard]] constexpr auto cast_noise(std::int32_t power_mana, const Tuning& t) -> std::int32_t {
  return power_mana * t.noise_cast_per_power;
}

/// Attenuated loudness at every cell of a level, from one emission.
///
/// Cells the sound never reached read 0, which is the same as "inaudible" — a
/// monster's threshold is always positive, so the two cases need not be
/// distinguished by callers.
class NoiseField {
 public:
  NoiseField() = default;
  explicit NoiseField(const Level& level) : m_width{level.width()}, m_loudness(level.cell_count(), 0) {}

  [[nodiscard]] auto at(const Level& level, Coord c) const -> std::int32_t {
    if (!level.in_bounds(c)) return 0;
    const auto i = level.index_of(c);
    return i < m_loudness.size() ? m_loudness[i] : 0;
  }

  [[nodiscard]] auto raw() const noexcept -> const std::vector<std::int32_t>& { return m_loudness; }

  friend auto propagate_noise(const Level&, Coord, std::int32_t, const Tuning&) -> NoiseField;

 private:
  std::int32_t m_width{};
  std::vector<std::int32_t> m_loudness{};
};

/// Propagates `emission` outward from `source`, losing `edge_attenuation` at
/// every edge crossed.
///
/// Deterministic: a uniform-cost search whose frontier is ordered by (loudness
/// descending, cell index ascending), so the result never depends on container
/// iteration order — §5.1's third rule.
///
/// The result is monotone non-increasing in graph distance by construction,
/// which is the first property test in §13.3.
[[nodiscard]] auto propagate_noise(const Level& level, Coord source, std::int32_t emission,
                                   const Tuning& tuning) -> NoiseField;

/// Whether a monster at `listener` with this acuity and awareness hears the
/// field. HUNTING subtracts `hear_hunting_bonus` from the threshold — it
/// listens harder (§18 Q5).
[[nodiscard]] auto hears(const NoiseField& field, const Level& level, Coord listener, Acuity acuity,
                         bool hunting, const Tuning& tuning) -> bool;

}  // namespace gloam
