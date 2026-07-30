#include "gloam/spells.hpp"

// For `cast_noise`. §8.3's "casting emits noise at 10 x the power rune's cost"
// is a rule of the noise system, not of the resolver, so it is defined once
// there and called here rather than restated.
#include "gloam/noise.hpp"

namespace gloam {

auto SpellTable::size() const -> std::size_t {
  std::size_t n = 0;
  for (const auto& row : m_rows) {
    if (row) ++n;
  }
  return n;
}

auto SpellTable::reachable_runes() const -> Reachable {
  Reachable r{};
  for (const auto& row : m_rows) {
    if (!row) continue;
    r.power[static_cast<std::size_t>(row->seq.power)] = true;
    r.element[static_cast<std::size_t>(row->seq.element)] = true;
    r.form[static_cast<std::size_t>(row->seq.form)] = true;
    r.modifier[static_cast<std::size_t>(row->seq.modifier)] = true;
  }
  return r;
}

auto danger_of_invalid(RuneSeq seq, const Tuning& tuning) -> DangerClass {
  // "All that power, nowhere to go." A combination that named a form had
  // somewhere to discharge, even if the pairing was meaningless — it fizzles.
  if (seq.form != Form::None) return DangerClass::Inert;

  const std::int32_t mana = tuning.power_mana(static_cast<int>(seq.power));
  // KAI through DUN have too little behind them to hurt anyone.
  const std::int32_t volatile_threshold = tuning.power_mana(static_cast<int>(Power::Bram));
  if (mana < volatile_threshold) return DangerClass::Inert;

  // BRAM turns on the caster; GOTH takes the party with it.
  auto danger = seq.power == Power::Goth ? DangerClass::Party : DangerClass::Caster;

  // Flame and rot are the elements that do not stay where they were put, so
  // they escalate one step. GOTH.HESH with no form lands on Level, which is
  // §8.3's canonical example.
  const bool unstable = seq.element == Element::Hesh || seq.element == Element::Fen;
  if (unstable) {
    danger = static_cast<DangerClass>(static_cast<std::uint8_t>(danger) + 1);
    if (static_cast<std::uint8_t>(danger) > static_cast<std::uint8_t>(DangerClass::Level)) {
      danger = DangerClass::Level;
    }
  }
  return danger;
}

auto resolve(RuneSeq seq, const CasterState& caster, const WorldState& world,
             const SpellTable& table, const Tuning& tuning) -> SpellOutcome {
  (void)world;  // reserved: §8 has no world-dependent valid rows yet

  SpellOutcome out{};

  const std::int32_t base_mana = tuning.power_mana(static_cast<int>(seq.power));
  std::int32_t mana_cost = base_mana;
  std::int32_t noise = cast_noise(base_mana, tuning);
  std::int32_t magnitude = base_mana;
  std::int32_t duration = base_mana;

  // ── Modifier arithmetic (§8.2). Integer only; the halvings truncate. ──────
  switch (seq.modifier) {
    case Modifier::Arr:  // extend: duration x3, cost x2
      duration *= tuning.arr_duration_multiplier;
      mana_cost *= tuning.arr_cost_multiplier;
      break;
    case Modifier::Esk:  // quieten: cast noise halved
      if (tuning.esk_noise_denominator != 0) {
        noise = noise * tuning.esk_noise_numerator / tuning.esk_noise_denominator;
      }
      break;
    case Modifier::Ith:  // split: two targets, half magnitude
      out.target_count = tuning.ith_target_count;
      if (tuning.ith_magnitude_denominator != 0) {
        magnitude = magnitude * tuning.ith_magnitude_numerator / tuning.ith_magnitude_denominator;
      }
      break;
    case Modifier::Oth:  // delay: fires N ticks later
      out.delay_ticks = tuning.oth_delay_ticks;
      break;
    case Modifier::Umbra:  // invert: the effect runs the other way
      out.inverted = true;
      break;
    case Modifier::Yrn:  // bind: attaches to an object, not a place
      out.bound = true;
      break;
    case Modifier::None:
      break;
  }

  out.mana_cost = mana_cost;
  out.noise = noise;

  // §8.3 — "Invalid combinations fail loudly and STILL COST MANA." The mana
  // cost and the noise are set above, before this branch, precisely so that
  // both paths carry them.
  const SpellRow* row = table.find(seq);
  if (row == nullptr) {
    out.valid = false;
    out.magnitude = 0;
    out.duration = 0;
    out.danger = danger_of_invalid(seq, tuning);
    // An invalid combination is still an attempt: the mana goes, the noise
    // happens, and `cast` reports whether the caster could actually pay.
    out.cast = caster.mana >= mana_cost;
    return out;
  }

  out.valid = true;
  out.effect_id = row->effect_id;
  out.danger = row->danger;
  out.magnitude = magnitude;
  out.duration = duration;
  out.cast = caster.mana >= mana_cost;
  return out;
}

auto m0_seed_table() -> SpellTable {
  SpellTable table;

  // M0 ships no magic at all (§15) and the full 60-80 row table is M2 content,
  // so this is a seed: one row per element, chosen to cover every form slot
  // including the empty one, so the resolver, the reachability rule and the
  // §13.3 properties all have something real to run against.
  //
  // Effect ids are stable identifiers, not indices into this list.
  struct Seed {
    Power power;
    Element element;
    Form form;
    Modifier modifier;
    std::uint16_t effect_id;
    DangerClass danger;
  };

  static constexpr Seed kSeeds[] = {
      // SIL — light. "Also refuels a lamp" (§8.2), which makes this the one
      // spell M0 could plausibly want if magic were in scope.
      {Power::Kai, Element::Sil, Form::Nil, Modifier::None, 0x0101, DangerClass::Inert},
      {Power::Pel, Element::Sil, Form::Mote, Modifier::None, 0x0102, DangerClass::Inert},
      // SHOR — cold: slows, and quiets.
      {Power::Tor, Element::Shor, Form::Lom, Modifier::None, 0x0201, DangerClass::Inert},
      {Power::Pel, Element::Shor, Form::Yarn, Modifier::Arr, 0x0202, DangerClass::Inert},
      // FEN — rot: over time, not at once.
      {Power::Dun, Element::Fen, Form::Rend, Modifier::None, 0x0301, DangerClass::Caster},
      // VAST — stone: walls, doors, weight.
      {Power::Pel, Element::Vast, Form::Wyrd, Modifier::None, 0x0401, DangerClass::Inert},
      {Power::Bram, Element::Vast, Form::Rend, Modifier::None, 0x0402, DangerClass::Level},
      // THULE — void: silence, a noise sink. The stealth spell.
      {Power::Tor, Element::Thule, Form::Yarn, Modifier::None, 0x0501, DangerClass::Inert},
      {Power::Tor, Element::Thule, Form::Nil, Modifier::Esk, 0x0502, DangerClass::Inert},
      // HESH — flame: loud, bright, obvious.
      {Power::Dun, Element::Hesh, Form::Lom, Modifier::None, 0x0601, DangerClass::Inert},
      {Power::Kai, Element::Hesh, Form::Mote, Modifier::Yrn, 0x0602, DangerClass::Inert},
      // One row with an empty form slot, so the table proves that "no form" is
      // not automatically invalid — it is only invalid when nothing in the
      // table claims it.
      {Power::Kai, Element::Sil, Form::None, Modifier::None, 0x0103, DangerClass::Inert},
  };

  for (const auto& s : kSeeds) {
    SpellRow row{};
    row.seq = RuneSeq{s.power, s.element, s.form, s.modifier};
    row.effect_id = s.effect_id;
    row.danger = s.danger;
    table.insert(row);
  }

  return table;
}

}  // namespace gloam
