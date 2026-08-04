// SPEC §4.1, §11 — the cold-start upload, on the wire.
//
// This suite exists because of an asymmetry: every plate GLOAM ships today
// compresses to well under one 3,072-byte chunk, so the CHUNKING PATH HAS NO
// PRODUCTION COVERAGE AT ALL and will not acquire any until the first authored
// wall plate crosses the boundary. A bug there ships silently and surfaces as a
// corrupt image months later, in a plate nobody touched. So the boundaries are
// tested synthetically, here, before any real payload exists.
//
// The specific failure this file is built around: chunking the BASE64 OUTPUT at
// 4,096 instead of the RAW INPUT at 3,072 emits `=` padding into the middle of
// the stream. Kitty decodes that to garbage rather than refusing it, and every
// command GLOAM emits carries `q=2`, so the terminal's complaint is suppressed.
// A wrong image and no error is the worst outcome available, which is why the
// padding assertion is a test rather than a comment.
//
// Failure matrix first, per AGENTS.md; the golden literals are last.

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "gloam/base64.hpp"
#include "gloam/emit.hpp"
#include "gloam/kitty.hpp"

using gloam::emit::ByteSink;
using gloam::kitty::EmitError;
using gloam::kitty::emit_transmit;
using gloam::kitty::kTransmitChunkBase64Bytes;
using gloam::kitty::kTransmitChunkPayloadBytes;

namespace {

constexpr std::string_view kApcStart = "\033_G";
constexpr std::string_view kApcEnd = "\033\\";

/// `n` bytes with a position-dependent value, so a chunk emitted twice or in the
/// wrong order is visible in the payload rather than only in the byte count.
[[nodiscard]] auto ramp(std::size_t n) -> std::vector<std::byte> {
  std::vector<std::byte> out(n);
  for (std::size_t i = 0; i < n; ++i) out[i] = static_cast<std::byte>((i * 7 + 11) & 0xFF);
  return out;
}

/// Split an emitted stream into its APC sequences, introducer and terminator
/// stripped. A transmission is one or more of these and nothing else.
[[nodiscard]] auto chunks_of(std::string_view stream) -> std::vector<std::string> {
  std::vector<std::string> out;
  std::size_t pos = 0;
  while (pos < stream.size()) {
    const auto start = stream.find(kApcStart, pos);
    if (start == std::string_view::npos) break;
    const auto end = stream.find(kApcEnd, start);
    REQUIRE(end != std::string_view::npos);
    const auto body_start = start + kApcStart.size();
    out.emplace_back(stream.substr(body_start, end - body_start));
    pos = end + kApcEnd.size();
  }
  return out;
}

/// The `<base64>` half of one chunk body.
[[nodiscard]] auto payload_of(std::string_view body) -> std::string_view {
  const auto semi = body.find(';');
  REQUIRE(semi != std::string_view::npos);
  return body.substr(semi + 1);
}

/// The control-data half.
[[nodiscard]] auto control_of(std::string_view body) -> std::string_view {
  const auto semi = body.find(';');
  REQUIRE(semi != std::string_view::npos);
  return body.substr(0, semi);
}

/// Transmit into a fresh sink and hand back everything it produced.
[[nodiscard]] auto transmitted(std::span<const std::byte> payload, std::uint32_t id = 42)
    -> std::string {
  ByteSink sink;
  const auto r = emit_transmit(sink, payload, id);
  REQUIRE(r.error == EmitError::None);
  REQUIRE(r.bytes == sink.size());
  return std::string{sink.view()};
}

}  // namespace

// ── The failure matrix ──────────────────────────────────────────────────────

TEST_CASE("image id zero is refused and writes nothing", "[transmit]") {
  // Kitty reads i=0 as "no id", which for a transmit means the image is stored
  // under an id the terminal chooses and GLOAM can never place.
  const auto payload = ramp(64);
  ByteSink sink;
  const auto r = emit_transmit(sink, payload, 0);

  CHECK(r.error == EmitError::ZeroImageId);
  CHECK(r.bytes == 0);
  CHECK(sink.size() == 0);
  CHECK(sink.view().empty());
}

TEST_CASE("an empty payload is refused rather than emitted", "[transmit]") {
  // The one refusal that is about this command rather than about its arguments.
  // A zero-length transfer is an error kitty would report and q=2 suppresses, so
  // if it is not caught here it is not caught anywhere.
  ByteSink sink;
  const auto r = emit_transmit(sink, std::span<const std::byte>{}, 7);

  CHECK(r.error == EmitError::EmptyPayload);
  CHECK(r.bytes == 0);
  CHECK(sink.size() == 0);
}

TEST_CASE("a refusal leaves a sink that already holds a frame untouched", "[transmit]") {
  ByteSink sink;
  sink.write("PRIOR");
  const auto before = sink.size();

  CHECK(emit_transmit(sink, std::span<const std::byte>{}, 7).error == EmitError::EmptyPayload);
  CHECK(emit_transmit(sink, ramp(8), 0).error == EmitError::ZeroImageId);

  CHECK(sink.size() == before);
  CHECK(sink.view() == "PRIOR");
}

TEST_CASE("the chunk boundary is on the RAW payload, at every neighbouring size",
          "[transmit]") {
  struct Row {
    std::size_t bytes;
    std::size_t chunks;
  };
  // One below, exactly on, and one above each multiple — the three places an
  // off-by-one in the loop condition shows up as a wrong chunk count.
  const auto row = GENERATE(Row{1, 1}, Row{3071, 1}, Row{3072, 1}, Row{3073, 2}, Row{6143, 2},
                            Row{6144, 2}, Row{6145, 3}, Row{9216, 3}, Row{9217, 4});
  CAPTURE(row.bytes);

  const auto payload = ramp(row.bytes);
  const auto stream = transmitted(payload);
  const auto chunks = chunks_of(stream);

  CHECK(chunks.size() == row.chunks);

  // Every chunk but the last carries a full 4,096 characters of base64, and the
  // last carries what is left. Anything else is a boundary computed on the
  // encoded side.
  for (std::size_t i = 0; i + 1 < chunks.size(); ++i) {
    CAPTURE(i);
    CHECK(payload_of(chunks[i]).size() == kTransmitChunkBase64Bytes);
  }
  const auto tail = row.bytes - (row.chunks - 1) * kTransmitChunkPayloadBytes;
  CHECK(payload_of(chunks.back()).size() == gloam::base64::encoded_size(tail));
}

TEST_CASE("padding appears only in the final chunk, and only when it must",
          "[transmit]") {
  // THE failure this file exists for. Interior padding is not a malformed
  // escape sequence — it is a well-formed one carrying a corrupt image.
  const auto bytes = GENERATE(std::size_t{3073}, std::size_t{4000}, std::size_t{6145},
                              std::size_t{9216}, std::size_t{10'000});
  CAPTURE(bytes);

  const auto chunks = chunks_of(transmitted(ramp(bytes)));
  REQUIRE(chunks.size() >= 2);

  for (std::size_t i = 0; i + 1 < chunks.size(); ++i) {
    CAPTURE(i);
    CHECK(payload_of(chunks[i]).find('=') == std::string_view::npos);
  }

  // And a payload that divides exactly has no padding anywhere at all.
  const auto exact = chunks_of(transmitted(ramp(kTransmitChunkPayloadBytes * 3)));
  for (const auto& c : exact) CHECK(payload_of(c).find('=') == std::string_view::npos);
}

TEST_CASE("only the first chunk carries control data, and every chunk carries m",
          "[transmit]") {
  const auto chunks = chunks_of(transmitted(ramp(kTransmitChunkPayloadBytes * 2 + 5)));
  REQUIRE(chunks.size() == 3);

  CHECK(control_of(chunks[0]) == "a=t,i=42,f=100,t=d,m=1,q=2");
  // Repeating i= on a continuation starts a SECOND transfer under the same id
  // rather than continuing this one.
  CHECK(control_of(chunks[1]) == "m=1,q=2");
  CHECK(control_of(chunks[2]) == "m=0,q=2");

  for (const auto& c : chunks) {
    CHECK(control_of(c).find("i=") == (&c == &chunks[0] ? 4U : std::string_view::npos));
  }
}

TEST_CASE("a single-chunk transmission carries no m key at all", "[transmit]") {
  // m=0 on its own would be legal, but m is the "more coming" flag and a
  // transfer that never said "more" should not have to say "no more".
  const auto chunks = chunks_of(transmitted(ramp(16)));
  REQUIRE(chunks.size() == 1);
  CHECK(control_of(chunks[0]).find("m=") == std::string_view::npos);
}

TEST_CASE("the concatenated payload decodes back to the input, in order",
          "[transmit]") {
  const auto bytes = GENERATE(std::size_t{1}, std::size_t{3072}, std::size_t{3073},
                              std::size_t{7000}, std::size_t{12'289});
  CAPTURE(bytes);

  const auto payload = ramp(bytes);
  const auto chunks = chunks_of(transmitted(payload));

  // Re-encode the input in one go and compare against the chunks joined. Equal
  // strings mean the chunk boundaries fell on group boundaries AND that the
  // chunks are in order with none repeated or dropped.
  std::vector<char> whole(gloam::base64::encoded_size(payload.size()));
  const auto r = gloam::base64::encode(payload, std::span<char>{whole});
  REQUIRE(r.error == gloam::base64::Base64Error::None);

  std::string joined;
  for (const auto& c : chunks) joined += payload_of(c);
  CHECK(joined == std::string(whole.data(), r.bytes));
}

TEST_CASE("the reported byte count is what the sink actually grew by",
          "[transmit]") {
  // §11's budget instrument reads this number. A transmit that under-reports is
  // a cold-start row that passes for the wrong reason.
  ByteSink sink;
  sink.write("XY");

  const auto payload = ramp(5000);
  const auto r = emit_transmit(sink, payload, 3);
  REQUIRE(r.error == EmitError::None);

  CHECK(r.bytes == sink.size() - 2);
  CHECK(sink.total() == sink.size());
}

TEST_CASE("a NUL-bearing payload survives the sink", "[transmit]") {
  // An IDAT is full of zero bytes and `emit::ByteSink` is NUL-transparent; this
  // is the pair that makes that property load-bearing rather than incidental.
  std::vector<std::byte> zeros(300, std::byte{0});
  zeros.back() = std::byte{0xFF};

  const auto chunks = chunks_of(transmitted(zeros));
  REQUIRE(chunks.size() == 1);
  CHECK(payload_of(chunks[0]).size() == gloam::base64::encoded_size(zeros.size()));
  CHECK(payload_of(chunks[0]).substr(0, 8) == "AAAAAAAA");
}

// ── The goldens, last ───────────────────────────────────────────────────────

TEST_CASE("the single-chunk wire form is byte-for-byte what it was", "[transmit]") {
  const std::vector<std::byte> payload{std::byte{0x89}, std::byte{'P'}, std::byte{'N'},
                                       std::byte{'G'}};
  ByteSink sink;
  REQUIRE(emit_transmit(sink, payload, 42).error == EmitError::None);

  CHECK(sink.view() == "\033_Ga=t,i=42,f=100,t=d,q=2;iVBORw==\033\\");
}

TEST_CASE("the three chunked wire forms are byte-for-byte what they were",
          "[transmit]") {
  // 3,073 bytes: the smallest payload that chunks. Only the control halves are
  // pinned literally — the base64 halves are 4 KB and are checked by the
  // round-trip case above.
  const auto stream = transmitted(ramp(3073), 1);
  const auto chunks = chunks_of(stream);
  REQUIRE(chunks.size() == 2);

  CHECK(control_of(chunks[0]) == "a=t,i=1,f=100,t=d,m=1,q=2");
  CHECK(control_of(chunks[1]) == "m=0,q=2");

  CHECK(stream.starts_with("\033_Ga=t,i=1,f=100,t=d,m=1,q=2;"));
  CHECK(stream.ends_with("\033\\"));

  // A middle chunk needs three, and it is the form nothing else produces.
  const auto middle = chunks_of(transmitted(ramp(kTransmitChunkPayloadBytes * 2 + 1), 1));
  REQUIRE(middle.size() == 3);
  CHECK(control_of(middle[1]) == "m=1,q=2");
}
