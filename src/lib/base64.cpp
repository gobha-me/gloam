/// SPEC §4.1 — base64, for the one upload per plate that §4.1 permits.

#include "gloam/base64.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace gloam::base64 {
namespace {

/// RFC 4648 §4. Written out rather than generated: the URL-safe variant differs
/// in exactly two characters, and a table you can read is a table you can check
/// against the RFC without running anything.
constexpr std::array<char, 64> kAlphabet{
    'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P',
    'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f',
    'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v',
    'w', 'x', 'y', 'z', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '+', '/'};

constexpr char kPad = '=';

/// The six bits at `shift`, as a character.
///
/// `std::byte` is not an arithmetic type, so every input byte is widened to
/// `std::uint32_t` before it is shifted. That widening is the reason this file
/// has no signed arithmetic at all and no `char` sign-extension bug: a plate's
/// bytes routinely have the high bit set, and `char` is signed on x86.
[[nodiscard]] constexpr auto digit(std::uint32_t group, int shift) -> char {
  return kAlphabet[static_cast<std::size_t>((group >> shift) & 0x3FU)];
}

}  // namespace

auto encode(std::span<const std::byte> input, std::span<char> output) -> EncodeResult {
  const auto needed = encoded_size(input.size());
  if (output.size() < needed) return EncodeResult{Base64Error::OutputTooSmall, 0};

  std::size_t read = 0;
  std::size_t written = 0;

  // Whole three-byte groups first: 24 bits in, four six-bit digits out, no
  // padding and no branch in the loop body.
  for (; read + 3 <= input.size(); read += 3) {
    const auto group = (static_cast<std::uint32_t>(input[read]) << 16) |
                       (static_cast<std::uint32_t>(input[read + 1]) << 8) |
                       static_cast<std::uint32_t>(input[read + 2]);
    output[written++] = digit(group, 18);
    output[written++] = digit(group, 12);
    output[written++] = digit(group, 6);
    output[written++] = digit(group, 0);
  }

  // The tail is one or two bytes, or nothing. The missing bytes are encoded as
  // zero bits and then covered by padding — NOT dropped: the digit that straddles
  // the boundary carries real bits from the last input byte and must be written.
  const auto remaining = input.size() - read;
  if (remaining == 1) {
    const auto group = static_cast<std::uint32_t>(input[read]) << 16;
    output[written++] = digit(group, 18);
    output[written++] = digit(group, 12);
    output[written++] = kPad;
    output[written++] = kPad;
  } else if (remaining == 2) {
    const auto group = (static_cast<std::uint32_t>(input[read]) << 16) |
                       (static_cast<std::uint32_t>(input[read + 1]) << 8);
    output[written++] = digit(group, 18);
    output[written++] = digit(group, 12);
    output[written++] = digit(group, 6);
    output[written++] = kPad;
  }

  return EncodeResult{Base64Error::None, written};
}

}  // namespace gloam::base64
