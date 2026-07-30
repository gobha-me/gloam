#pragma once

/// SPEC §8.3 — the spell resolver.
///
/// "Implementation is a pure function: (RuneSeq, CasterState, WorldState) ->
/// SpellOutcome. Deterministic, exhaustively unit-testable across all 1,764
/// inputs, and the valid table is a data file."
///
/// Two rules from §8.3 that are easy to lose and expensive to re-add:
///
///  - Invalid combinations fail LOUDLY and STILL COST MANA. Experimentation has
///    a price. `resolve` therefore always returns a mana cost, and callers must
///    charge it whether or not `valid` is set.
///  - The valid table is read by BOTH the resolver and the generator's
///    inscription placement, from ONE file, so a level can never teach a rune
///    whose partners are unreachable. `SpellTable` is that shared object; do
///    not give the generator its own copy of the rules.

#include <cstdint>
#include <optional>
#include <vector>

#include "gloam/runes.hpp"
#include "gloam/tuning.hpp"

namespace gloam {

/// §12 — `danger_class` distinguishes inert from harmful.
enum class DangerClass : std::uint8_t {
  Inert = 0,   ///< nothing happens; the mana is still gone
  Caster = 1,  ///< backfires on the caster
  Party = 2,   ///< backfires on the party
  Level = 3,   ///< backfires on the level — a collapsed wall, a woken floor
};

/// One row of `spells.data`. Absent rows are invalid BY DEFINITION — there is
/// no "invalid" row and no negative space to keep in sync.
struct SpellRow {
  RuneSeq seq{};
  std::uint16_t effect_id{0};
  std::int16_t params[4]{};
  DangerClass danger{DangerClass::Inert};
};

/// The valid-combination table.
///
/// A dense 1,764-slot index rather than a hash map: lookup is a subscript, the
/// memory is trivial, and — the reason that matters here — iteration order is
/// the rune enumeration order rather than a hash order, which §5.1 requires of
/// anything the simulation walks.
class SpellTable {
 public:
  SpellTable() : m_rows(static_cast<std::size_t>(kCombinationSpace)) {}

  void insert(const SpellRow& row) { m_rows[static_cast<std::size_t>(row.seq.index())] = row; }

  [[nodiscard]] auto find(RuneSeq seq) const -> const SpellRow* {
    const auto& slot = m_rows[static_cast<std::size_t>(seq.index())];
    return slot ? &*slot : nullptr;
  }

  [[nodiscard]] auto contains(RuneSeq seq) const -> bool { return find(seq) != nullptr; }

  [[nodiscard]] auto size() const -> std::size_t;

  /// Every rune that appears in at least one valid row. §13.3 asserts that
  /// every row is reachable from some placeable inscription set — this is what
  /// the generator's inscription placement draws from, so that a level never
  /// teaches a rune whose partners are unreachable.
  struct Reachable {
    bool power[kPowerCount]{};
    bool element[kElementCount]{};
    bool form[kFormCount]{};
    bool modifier[kModifierCount]{};
  };
  [[nodiscard]] auto reachable_runes() const -> Reachable;

 private:
  std::vector<std::optional<SpellRow>> m_rows;
};

/// The caster, at the moment of casting.
struct CasterState {
  std::int32_t mana{0};
};

/// What the resolver is allowed to know about the world. Deliberately narrow:
/// widening it is how a pure function stops being testable across all 1,764
/// inputs.
struct WorldState {
  std::int32_t lamp_level{kLampLevelDefault};
};

struct SpellOutcome {
  /// Was this a row in the table?
  bool valid{false};
  /// Did the caster have the mana? An underfunded cast is still charged
  /// whatever mana was available and still makes its noise.
  bool cast{false};

  std::uint16_t effect_id{0};
  std::int32_t magnitude{0};
  std::int32_t duration{0};
  /// Always charged — valid or not, cast or not (§8.3).
  std::int32_t mana_cost{0};
  /// §6.2 — this feeds straight into `propagate_noise`.
  std::int32_t noise{0};
  std::int32_t delay_ticks{0};
  std::int32_t target_count{1};
  bool inverted{false};
  bool bound{false};
  DangerClass danger{DangerClass::Inert};

  [[nodiscard]] auto operator==(const SpellOutcome&) const -> bool = default;
};

/// The pure resolver.
///
/// Total over the whole combination space: every one of the 1,764 sequences
/// returns an outcome and none of them throw (§13.3). Integer arithmetic only.
[[nodiscard]] auto resolve(RuneSeq seq, const CasterState& caster, const WorldState& world,
                           const SpellTable& table, const Tuning& tuning) -> SpellOutcome;

/// The danger class of a combination that is NOT in the table.
///
/// §8.3: "A subset of invalid combinations are actively dangerous rather than
/// merely inert. GOTH.HESH with no form is the canonical example: all that
/// power, nowhere to go."
///
/// Encoded as exactly that sentence: power with no form to carry it, escalating
/// with the volatility of the element.
[[nodiscard]] auto danger_of_invalid(RuneSeq seq, const Tuning& tuning) -> DangerClass;

/// The M0 seed table.
///
/// M0 has no magic at all (§15) and the full 60-80 row table is M2 content, so
/// this is deliberately small: enough rows to exercise the resolver, the
/// generator's reachability rule and the §13.3 properties, and no more.
/// Authoring the full table is a design job, not a code job — see §8.2's
/// "original vocabulary" note.
[[nodiscard]] auto m0_seed_table() -> SpellTable;

}  // namespace gloam
