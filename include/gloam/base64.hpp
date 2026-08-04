#pragma once

/// SPEC §4.1, §11 — the encoding the cold-start upload pays for.
///
/// §4.1 puts every plate on the wire once, at startup, and kitty's direct
/// transmission medium carries the payload as base64 inside an APC sequence. That
/// is the ONLY place GLOAM base64-encodes anything: §4.1's whole thesis is that
/// during play a frame is placement commands and "no PNG payloads, no base64
/// through the tty". So this module exists to be used exactly once per plate, at
/// startup, and never again — which is also why it is worth being exact about.
///
/// §11 budgets 1.2 MB for that upload, and base64 is a fixed 4-bytes-per-3 tax on
/// whatever the encoder above it produces. `encoded_size` is therefore part of the
/// budget arithmetic, not a convenience: it is what lets a caller measure the wire
/// cost of a payload without encoding it.
///
///
/// WHY IT DOES NOT KNOW ABOUT A ByteSink
///
/// The obvious signature is `encode(bytes, ByteSink&)`, and it is wrong in the
/// direction that matters. `emit.hpp` is the budget instrument and `kitty.hpp` is
/// the wire grammar; base64 is neither, and a base64 that writes into a sink is a
/// base64 that cannot be tested without one. Bytes in, bytes out, into a span the
/// caller sized — the same shape `plate.hpp` and `pack.hpp` take, for the same
/// reason. `src/lib/kitty.cpp` chunks its own output and appends it itself.
///
/// Nothing here allocates, and nothing here can refuse for any reason other than
/// an output span too small to hold an answer whose size was computable up front.

#include <cstddef>
#include <cstdint>
#include <span>

namespace gloam::base64 {

/// Why an encode was refused. `None` is success.
///
/// A discriminated enum rather than `std::expected`, for the reason spelled out
/// in `kitty.hpp`: these headers are installed, and Clang 18 cannot compile
/// `<expected>` against libstdc++.
enum class Base64Error : std::uint8_t {
  None = 0,
  /// The output span is smaller than `encoded_size(input.size())`.
  OutputTooSmall = 1,
};

struct EncodeResult {
  Base64Error error{Base64Error::None};
  /// Characters written. Zero on refusal, and also zero for empty input — the
  /// pair is what distinguishes them.
  std::size_t bytes{0};

  [[nodiscard]] explicit operator bool() const noexcept { return error == Base64Error::None; }
};

/// Exactly how many characters `encode` will write, padding included.
///
/// Constexpr and total: this is budget arithmetic as much as it is a buffer size,
/// and `test/10budgets/` computes a projected wire cost from it.
///
/// The `+ 2` is the ceiling division that makes the padding fall out — 1 leftover
/// byte and 2 leftover bytes both cost a whole four-character group.
[[nodiscard]] constexpr auto encoded_size(std::size_t raw_bytes) -> std::size_t {
  return ((raw_bytes + 2) / 3) * 4;
}

/// Encode `input` into `output`, RFC 4648 §4 alphabet, always padded.
///
/// Always padded, deliberately. Kitty's escape-code protocol takes standard
/// base64 and the unpadded variant is a different encoding; more to the point,
/// a chunked upload that dropped padding on every chunk but the last would be
/// unparseable at the join. `src/lib/kitty.cpp` avoids interior padding by
/// chunking the INPUT at a multiple of three rather than chunking this output.
[[nodiscard]] auto encode(std::span<const std::byte> input, std::span<char> output) -> EncodeResult;

}  // namespace gloam::base64
