#pragma once

/// SPEC §10, §12 — the hash the pack's integrity rests on.
///
/// §10: "Bake into a versioned pack with a manifest hash. The build verifies it.
/// Because plates are transmitted once at startup, pack integrity is a hard
/// startup requirement: a mismatched hash refuses to launch rather than
/// half-uploading a corrupt plate set."
///
/// FIPS 180-4 SHA-256, written out here rather than pulled in. That is not
/// invented-here: `gloam::lib` links nothing but the standard library
/// (AGENTS.md rule 1) and `${PROJECT_NAME}_DEPS` carries catch2 and nothing else,
/// deliberately. A hash is ~200 lines of fixed-width integer arithmetic with a
/// published test vector set, which makes it one of the few things cheaper to
/// write than to depend on.
///
/// Nothing here reaches a clock, a file descriptor or a global — bytes in,
/// 32 bytes out — so it sits inside the library on exactly the same footing as
/// `kitty.cpp`. It is excluded from the `gloam/gloam.hpp` umbrella because it is
/// pipeline-side, not simulation.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace gloam::hash {

/// A SHA-256 digest. 32 bytes, exactly what §12's manifest record carries.
using Digest = std::array<std::uint8_t, 32>;

/// Streaming SHA-256.
///
/// Streaming rather than one-shot because the pack is assembled a field at a
/// time into a caller-owned span, and hashing it would otherwise mean either a
/// second full copy of a ~400 KB image or a hash that only ever sees the
/// finished bytes. `update` may be called with any split of the message,
/// including empty spans, and the digest does not depend on where the splits
/// fall — which is the property `test/12pack/` sweeps rather than assumes.
class Sha256 {
 public:
  Sha256() = default;

  /// Absorb `bytes`. An empty span is a no-op, not an error.
  auto update(std::span<const std::byte> bytes) -> void;

  /// Pad, absorb the length, and produce the digest.
  ///
  /// Consumes the state: the object is reset to a fresh Sha256 afterwards, so a
  /// second `finish()` returns the digest of the empty message rather than
  /// something that depends on how many times it has been called. A hasher whose
  /// output depends on its call history is a reproducibility bug waiting to
  /// happen in the one artifact whose hash is a build gate.
  [[nodiscard]] auto finish() -> Digest;

 private:
  /// FIPS 180-4 §5.3.3 initial hash value.
  std::array<std::uint32_t, 8> m_h{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                                   0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
  std::array<std::uint8_t, 64> m_block{};
  std::size_t m_block_len{0};
  std::uint64_t m_total_bits{0};
};

/// One-shot digest of a contiguous message.
[[nodiscard]] auto sha256(std::span<const std::byte> bytes) -> Digest;

/// Lowercase hex, 64 characters, no terminator.
///
/// Returned by value as a fixed array rather than a std::string so this stays
/// allocation-free like the rest of the pipeline. The baker prints it; the
/// reproducibility check compares it.
[[nodiscard]] auto to_hex(const Digest& digest) -> std::array<char, 64>;

}  // namespace gloam::hash
