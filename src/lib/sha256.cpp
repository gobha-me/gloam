#include "gloam/sha256.hpp"

#include <bit>
#include <cstring>

namespace gloam::hash {
namespace {

/// FIPS 180-4 §4.2.2 — the first 32 bits of the fractional parts of the cube
/// roots of the first 64 primes.
constexpr std::array<std::uint32_t, 64> kK{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
    0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
    0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
    0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
    0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
    0xc67178f2U};

/// std::rotr rather than a hand-rolled shift pair: a shift by 32 is undefined
/// behaviour and the sanitizer toolchains would be the ones to find out.
[[nodiscard]] constexpr auto rotr(std::uint32_t x, int n) -> std::uint32_t {
  return std::rotr(x, n);
}

[[nodiscard]] constexpr auto ch(std::uint32_t x, std::uint32_t y, std::uint32_t z)
    -> std::uint32_t {
  return (x & y) ^ (~x & z);
}

[[nodiscard]] constexpr auto maj(std::uint32_t x, std::uint32_t y, std::uint32_t z)
    -> std::uint32_t {
  return (x & y) ^ (x & z) ^ (y & z);
}

[[nodiscard]] constexpr auto big_sigma0(std::uint32_t x) -> std::uint32_t {
  return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22);
}

[[nodiscard]] constexpr auto big_sigma1(std::uint32_t x) -> std::uint32_t {
  return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25);
}

[[nodiscard]] constexpr auto small_sigma0(std::uint32_t x) -> std::uint32_t {
  return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3);
}

[[nodiscard]] constexpr auto small_sigma1(std::uint32_t x) -> std::uint32_t {
  return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10);
}

/// The message schedule is big-endian by specification, and that has nothing to
/// do with the pack's little-endian field order (§12) — a hash that read host
/// order would produce a different digest on a big-endian box, which is exactly
/// the cross-machine reproducibility the pack hash exists to guarantee.
auto compress(std::array<std::uint32_t, 8>& h, const std::uint8_t* block) -> void {
  std::array<std::uint32_t, 64> w{};
  for (std::size_t i = 0; i < 16; ++i) {
    w[i] = (static_cast<std::uint32_t>(block[i * 4 + 0]) << 24) |
           (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
           (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
           static_cast<std::uint32_t>(block[i * 4 + 3]);
  }
  for (std::size_t i = 16; i < 64; ++i) {
    w[i] = small_sigma1(w[i - 2]) + w[i - 7] + small_sigma0(w[i - 15]) + w[i - 16];
  }

  auto a = h[0];
  auto b = h[1];
  auto c = h[2];
  auto d = h[3];
  auto e = h[4];
  auto f = h[5];
  auto g = h[6];
  auto hh = h[7];

  for (std::size_t i = 0; i < 64; ++i) {
    const auto t1 = hh + big_sigma1(e) + ch(e, f, g) + kK[i] + w[i];
    const auto t2 = big_sigma0(a) + maj(a, b, c);
    hh = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }

  h[0] += a;
  h[1] += b;
  h[2] += c;
  h[3] += d;
  h[4] += e;
  h[5] += f;
  h[6] += g;
  h[7] += hh;
}

}  // namespace

auto Sha256::update(std::span<const std::byte> bytes) -> void {
  // Bail before touching `data()`. An empty span may carry a null pointer, and
  // `memcpy(dst, nullptr, 0)` is undefined behaviour the sanitizer toolchains
  // do flag — a zero-length update is legitimate and must stay free.
  if (bytes.empty()) return;

  const auto* p = reinterpret_cast<const std::uint8_t*>(bytes.data());
  auto remaining = bytes.size();
  m_total_bits += static_cast<std::uint64_t>(remaining) * 8U;

  // Top up a partial block first. Everything downstream assumes m_block_len is
  // strictly less than 64 on entry, which is what makes the fast path below a
  // whole number of blocks.
  if (m_block_len > 0) {
    const auto want = 64U - m_block_len;
    const auto take = remaining < want ? remaining : want;
    std::memcpy(m_block.data() + m_block_len, p, take);
    m_block_len += take;
    p += take;
    remaining -= take;
    if (m_block_len < 64) return;
    compress(m_h, m_block.data());
    m_block_len = 0;
  }

  while (remaining >= 64) {
    compress(m_h, p);
    p += 64;
    remaining -= 64;
  }

  if (remaining > 0) {
    std::memcpy(m_block.data(), p, remaining);
    m_block_len = remaining;
  }
}

auto Sha256::finish() -> Digest {
  const auto bits = m_total_bits;

  // FIPS 180-4 §5.1.1: a 0x80 byte, then zeros, then the 64-bit big-endian
  // length. If the length does not fit in this block it spills into one more —
  // the 55/56 and 119/120 byte boundaries in `test/12pack/` are exactly this.
  m_block[m_block_len++] = 0x80U;
  if (m_block_len > 56) {
    while (m_block_len < 64) m_block[m_block_len++] = 0U;
    compress(m_h, m_block.data());
    m_block_len = 0;
  }
  while (m_block_len < 56) m_block[m_block_len++] = 0U;

  for (int i = 7; i >= 0; --i) {
    m_block[m_block_len++] = static_cast<std::uint8_t>((bits >> (i * 8)) & 0xFFU);
  }
  compress(m_h, m_block.data());

  Digest out{};
  for (std::size_t i = 0; i < 8; ++i) {
    out[i * 4 + 0] = static_cast<std::uint8_t>((m_h[i] >> 24) & 0xFFU);
    out[i * 4 + 1] = static_cast<std::uint8_t>((m_h[i] >> 16) & 0xFFU);
    out[i * 4 + 2] = static_cast<std::uint8_t>((m_h[i] >> 8) & 0xFFU);
    out[i * 4 + 3] = static_cast<std::uint8_t>(m_h[i] & 0xFFU);
  }

  *this = Sha256{};
  return out;
}

auto sha256(std::span<const std::byte> bytes) -> Digest {
  Sha256 h;
  h.update(bytes);
  return h.finish();
}

auto to_hex(const Digest& digest) -> std::array<char, 64> {
  constexpr char kDigits[] = "0123456789abcdef";
  std::array<char, 64> out{};
  for (std::size_t i = 0; i < digest.size(); ++i) {
    out[i * 2 + 0] = kDigits[(digest[i] >> 4) & 0x0FU];
    out[i * 2 + 1] = kDigits[digest[i] & 0x0FU];
  }
  return out;
}

}  // namespace gloam::hash
