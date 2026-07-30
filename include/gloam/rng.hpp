#pragma once

/// SPEC §5.1 — deterministic random number generation.
///
/// Seed in, world out. Three rules this header exists to enforce:
///
///  1. Every subsystem draws from its own NAMED stream, so adding a subsystem
///     later does not perturb the draws of any existing one and old replays
///     stay valid. This is why `Stream` values are explicit and must never be
///     renumbered.
///  2. Nothing here touches `std::random_device`, the wall clock, or any global
///     state.
///  3. Nothing here uses `<random>`. The standard distributions are NOT
///     portable — `std::uniform_int_distribution` is free to produce different
///     values on libstdc++ and libc++ from the same engine and the same seed.
///     §19 step 7 requires a recorded session to replay to an identical world
///     hash on BOTH compilers, so the bounded draw is spelled out here in
///     fixed-width integer arithmetic that has exactly one answer.

#include <cstdint>

namespace gloam {

/// Named RNG streams. Values are part of the replay format's compatibility
/// contract: APPEND ONLY, and never renumber an existing entry.
enum class Stream : std::uint64_t {
  Level = 1,      ///< level generation — layout, doors, keys
  Patrol = 2,     ///< patrol routes and idle variation (§6.4)
  Perception = 3, ///< perception jitter
  Combat = 4,     ///< to-hit and damage rolls
  Loot = 5,       ///< item placement and drops
  Spell = 6,      ///< spell outcome variance (§8)
  Inscription = 7,///< rune inscription placement (§8.3)
  Ambience = 8,   ///< non-simulation flavour; never feeds back into sim state
};

/// SplitMix64. Used both to derive a stream's seed from the world seed and as
/// the stream's own generator: it is a bijection on 64 bits with a known-good
/// avalanche, which is all this needs, and it has no UB and no platform
/// variance.
[[nodiscard]] constexpr auto splitmix64(std::uint64_t& state) noexcept -> std::uint64_t {
  state += 0x9E3779B97F4A7C15ULL;
  std::uint64_t z = state;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

/// One subsystem's deterministic stream.
///
/// Construct via `rng(seed, Stream::Foo)`. Two streams built from the same world
/// seed but different `Stream` values are independent, and drawing from one
/// never advances the other.
class Rng {
 public:
  constexpr Rng() = default;

  constexpr explicit Rng(std::uint64_t state) noexcept : m_state{state} {}

  /// The raw next value. Uniform over the full 64-bit range.
  [[nodiscard]] constexpr auto next() noexcept -> std::uint64_t { return splitmix64(m_state); }

  /// Uniform in [0, bound). Returns 0 for a bound of 0.
  ///
  /// Modulo with the biased tail rejected. Writing 2^64 = q*bound + rem, the
  /// values [0, rem) are the ones that would make some residues more likely
  /// than others, so they are drawn again; what remains is exactly q*bound
  /// values and `% bound` over them is uniform.
  ///
  /// Deliberately NOT Lemire's multiply-shift, which is faster but needs a
  /// 128-bit integer type. `unsigned __int128` is a compiler extension, and
  /// this project builds with -Wpedantic on both GCC and Clang; more to the
  /// point, §19 step 7 requires identical replays across both compilers, and
  /// an extension is exactly the wrong foundation for that. Every operation
  /// below is defined by the standard and has one answer.
  [[nodiscard]] constexpr auto below(std::uint64_t bound) noexcept -> std::uint64_t {
    if (bound <= 1) return 0;
    // 2^64 mod bound, computed without ever forming 2^64: UINT64_MAX - bound + 1
    // is 2^64 - bound exactly, and that is congruent to 2^64 modulo bound.
    constexpr std::uint64_t kMax = ~std::uint64_t{0};
    const std::uint64_t remainder = (kMax - bound + 1) % bound;
    std::uint64_t draw = next();
    while (draw < remainder) draw = next();
    return draw % bound;
  }

  /// Uniform in [lo, hi], inclusive of both. Returns `lo` when hi < lo.
  [[nodiscard]] constexpr auto range(std::int64_t lo, std::int64_t hi) noexcept -> std::int64_t {
    if (hi <= lo) return lo;
    const auto span = static_cast<std::uint64_t>(hi - lo) + 1;
    return lo + static_cast<std::int64_t>(below(span));
  }

  /// True with probability `numerator / denominator`. Integer-only, so a
  /// designer's "one in eight" stays one in eight on every platform.
  [[nodiscard]] constexpr auto chance(std::uint64_t numerator,
                                      std::uint64_t denominator) noexcept -> bool {
    if (denominator == 0) return false;
    return below(denominator) < numerator;
  }

  /// Exposed so a replay can be checkpointed and resumed mid-session.
  [[nodiscard]] constexpr auto state() const noexcept -> std::uint64_t { return m_state; }

 private:
  std::uint64_t m_state{};
};

/// The named-stream constructor. `rng(seed, Stream::Patrol)` is the ONLY
/// sanctioned way to get a generator: §6.4 requires patrol schedules to be
/// drawn from the patrol stream rather than a shared one, and the same holds
/// for every other subsystem.
[[nodiscard]] constexpr auto rng(std::uint64_t seed, Stream stream) noexcept -> Rng {
  // Mix the stream id into the seed through the same bijection, so streams are
  // decorrelated rather than merely offset from one another.
  std::uint64_t mixer = seed ^ (static_cast<std::uint64_t>(stream) * 0xD1B54A32D192ED03ULL);
  const std::uint64_t derived = splitmix64(mixer);
  return Rng{derived};
}

}  // namespace gloam
