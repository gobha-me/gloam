#pragma once

/// Little-endian scalar serialization, one byte at a time.
///
/// PRIVATE TO `src/lib/`. This is not in `include/gloam/` on purpose: it is how
/// two file formats are written, not part of what the library offers. A
/// consumer that wants to read a pack or a replay uses `pack.hpp` or
/// `replay.hpp`; a consumer that wants a byte-poking utility should not be
/// getting it from a dungeon crawler.
///
///
/// WHY BYTE AT A TIME, AND WHY LITTLE-ENDIAN
///
/// The argument is `pack.hpp`'s and it now serves two formats, which is why it
/// lives here rather than in either of them. Every target GLOAM runs on is
/// little-endian, so an LE writer has no byte-swap branch at all — and an
/// untested swap path in an artifact whose hash is a build gate is precisely
/// the code that turns out to be wrong, years later, on the one machine nobody
/// has.
///
/// Never a `memcpy` of a compiler struct. ABI padding is not a value the
/// program ever set, and both formats hash the bytes they write: a struct copy
/// puts uninitialised padding inside a digest that is supposed to be
/// reproducible, and the failure surfaces as an unreproducible build on someone
/// else's compiler rather than as anything that points here.
///
/// `sha256.cpp`'s message schedule is big-endian because FIPS 180-4 says so.
/// That is the hash's internal business and unrelated to any of this.

#include <cstddef>
#include <cstdint>
#include <span>

#include "gloam/sha256.hpp"

namespace gloam::le {

inline auto put_u8(std::span<std::byte> out, std::size_t at, std::uint8_t v) -> void {
  out[at] = static_cast<std::byte>(v);
}

inline auto put_u16(std::span<std::byte> out, std::size_t at, std::uint16_t v) -> void {
  out[at + 0] = static_cast<std::byte>(v & 0xFFU);
  out[at + 1] = static_cast<std::byte>((v >> 8) & 0xFFU);
}

inline auto put_u32(std::span<std::byte> out, std::size_t at, std::uint32_t v) -> void {
  out[at + 0] = static_cast<std::byte>(v & 0xFFU);
  out[at + 1] = static_cast<std::byte>((v >> 8) & 0xFFU);
  out[at + 2] = static_cast<std::byte>((v >> 16) & 0xFFU);
  out[at + 3] = static_cast<std::byte>((v >> 24) & 0xFFU);
}

inline auto put_u64(std::span<std::byte> out, std::size_t at, std::uint64_t v) -> void {
  for (std::size_t i = 0; i < 8; ++i) {
    out[at + i] = static_cast<std::byte>((v >> (8 * i)) & 0xFFU);
  }
}

[[nodiscard]] inline auto get_u8(std::span<const std::byte> in, std::size_t at) -> std::uint8_t {
  return static_cast<std::uint8_t>(in[at]);
}

[[nodiscard]] inline auto get_u16(std::span<const std::byte> in, std::size_t at) -> std::uint16_t {
  return static_cast<std::uint16_t>(static_cast<std::uint16_t>(get_u8(in, at)) |
                                    static_cast<std::uint16_t>(get_u8(in, at + 1) << 8));
}

[[nodiscard]] inline auto get_u32(std::span<const std::byte> in, std::size_t at) -> std::uint32_t {
  return static_cast<std::uint32_t>(get_u8(in, at)) |
         (static_cast<std::uint32_t>(get_u8(in, at + 1)) << 8) |
         (static_cast<std::uint32_t>(get_u8(in, at + 2)) << 16) |
         (static_cast<std::uint32_t>(get_u8(in, at + 3)) << 24);
}

[[nodiscard]] inline auto get_u64(std::span<const std::byte> in, std::size_t at) -> std::uint64_t {
  std::uint64_t v = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    v |= static_cast<std::uint64_t>(get_u8(in, at + i)) << (8 * i);
  }
  return v;
}

/// A digest is 32 opaque bytes in the order SHA-256 produced them. There is no
/// endianness to apply — it is not a number — so these two exist to stop anyone
/// deciding it is one.
inline auto put_digest(std::span<std::byte> out, std::size_t at, const hash::Digest& d) -> void {
  for (std::size_t i = 0; i < d.size(); ++i) {
    out[at + i] = static_cast<std::byte>(d[i]);
  }
}

[[nodiscard]] inline auto get_digest(std::span<const std::byte> in, std::size_t at) -> hash::Digest {
  hash::Digest d{};
  for (std::size_t i = 0; i < d.size(); ++i) {
    d[i] = get_u8(in, at + i);
  }
  return d;
}

}  // namespace gloam::le
