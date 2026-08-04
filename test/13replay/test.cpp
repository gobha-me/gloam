// SPEC §12, §5.1 · TEST-PLAN.md §2 — `replay.gloam`, and the determinism it
// exists to defend.
//
// The assertions that carry the most weight here are the refusals. §12's
// requirement is not "a replay round-trips" — it is that "a replay recorded
// against different tuning is REJECTED at load rather than silently
// mis-played, otherwise a golden test starts passing for the wrong reason". A
// container that accepts a file it should have refused turns this whole suite
// into one that reports success for sessions nobody recorded.
//
// So: every field gets a hostile value, every byte of the covered region gets
// flipped, and the one advisory field is asserted to be advisory in both
// directions — a `pack_hash` mismatch must not reject, and a `ruleset_hash`
// mismatch must not merely warn.
//
// Failure matrix first, per AGENTS.md. The round trip and the golden prefix are
// last, and prove the least.

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "gloam/replay.hpp"
#include "gloam/sha256.hpp"
#include "gloam/world.hpp"

using namespace gloam;
using gloam::replay::Event;
using gloam::replay::ReplayError;

namespace {

[[nodiscard]] auto hex_of(const hash::Digest& d) -> std::string {
  const auto h = hash::to_hex(d);
  return std::string(h.data(), h.size());
}

/// A short, valid session: every event kind at least once, two records sharing
/// a tick (which §3 permits), and a lamp change so the payload range matters.
[[nodiscard]] auto scripted_records() -> std::vector<replay::Record> {
  return {
      {0, Event::Lamp, 3},   {1, Event::Step, 1},  {2, Event::Step, 1},
      {3, Event::Creep, 1},  {3, Event::Turn, 2},  // same tick, applied in file order
      {5, Event::Wait, 0},   {8, Event::Lamp, 0},  {9, Event::Step, 3},
  };
}

constexpr std::uint64_t kSeed = 0xDEADBEEF12345678ULL;
constexpr std::uint64_t kRuleset = 0x0123456789ABCDEFULL;

struct Fixture {
  replay::Header header;
  std::vector<replay::Record> records;
  std::vector<std::byte> image;
};

[[nodiscard]] auto make(std::uint64_t pack_hash = replay::kNoPackHash) -> Fixture {
  Fixture f{};
  f.records = scripted_records();
  f.header.seed = kSeed;
  f.header.ruleset_hash = kRuleset;
  f.header.pack_hash = pack_hash;
  f.header.final_world_hash = hash::sha256({});
  f.image.resize(replay::image_bytes(static_cast<std::uint32_t>(f.records.size())));

  const auto res = replay::assemble(f.header, f.records, f.image);
  REQUIRE(res);
  REQUIRE(res.bytes == f.image.size());
  return f;
}

/// The expectations a freshly-made fixture is supposed to satisfy.
[[nodiscard]] auto matching_expect(const Fixture& f) -> replay::Expect {
  return {kRuleset, f.header.pack_hash};
}

}  // namespace

// ── Refusals: the header ────────────────────────────────────────────────────

TEST_CASE("a buffer shorter than a header is Truncated at every length", "[replay]") {
  const auto f = make();
  for (std::size_t n = 0; n < replay::kHeaderBytes; ++n) {
    replay::Header out{};
    const auto res = replay::read_header(std::span{f.image}.first(n), out);
    INFO("prefix length " << n);
    CHECK(res.error == ReplayError::Truncated);
  }
}

TEST_CASE("a truncated file is refused at every length short of its own claim", "[replay]") {
  const auto f = make();
  for (std::size_t n = replay::kHeaderBytes; n < f.image.size(); ++n) {
    INFO("prefix length " << n);
    CHECK_FALSE(replay::verify(std::span{f.image}.first(n)));
  }
}

TEST_CASE("each magic byte is checked", "[replay]") {
  for (std::size_t i = 0; i < replay::kMagic.size(); ++i) {
    auto f = make();
    f.image[i] = static_cast<std::byte>(static_cast<std::uint8_t>(f.image[i]) ^ 0xFFU);
    replay::Header out{};
    INFO("magic byte " << i);
    CHECK(replay::read_header(f.image, out).error == ReplayError::BadMagic);
  }
}

TEST_CASE("an unsupported version is refused, in both directions", "[replay]") {
  for (const std::uint16_t version : {std::uint16_t{0}, std::uint16_t{2}, std::uint16_t{65535}}) {
    auto f = make();
    f.image[4] = static_cast<std::byte>(version & 0xFFU);
    f.image[5] = static_cast<std::byte>((version >> 8) & 0xFFU);
    replay::Header out{};
    INFO("version " << version);
    CHECK(replay::read_header(f.image, out).error == ReplayError::UnsupportedVersion);
  }
}

TEST_CASE("tick_hz zero is refused", "[replay]") {
  // A replay that advances no ticks per second describes a session that never
  // happened, and dividing by it is the next bug along.
  auto f = make();
  f.image[6] = std::byte{0};
  f.image[7] = std::byte{0};
  replay::Header out{};
  CHECK(replay::read_header(f.image, out).error == ReplayError::ZeroTickHz);
}

TEST_CASE("an empty input log is refused rather than replayed", "[replay]") {
  replay::Header header{};
  std::vector<std::byte> image(replay::image_bytes(0));
  CHECK(replay::assemble(header, {}, image).error == ReplayError::ZeroRecords);
}

TEST_CASE("record_count and total_bytes must agree with each other", "[replay]") {
  auto f = make();
  const auto count = static_cast<std::uint32_t>(f.records.size());
  f.image[96] = static_cast<std::byte>((count + 1) & 0xFFU);
  replay::Header out{};
  CHECK(replay::read_header(f.image, out).error == ReplayError::RecordCountMismatch);
}

TEST_CASE("total_bytes must agree with the bytes actually present", "[replay]") {
  auto f = make();
  auto image = f.image;
  image.push_back(std::byte{0});  // one byte more than the header claims
  CHECK(replay::verify(image).error == ReplayError::TotalBytesMismatch);
}

// ── Refusals: the records ───────────────────────────────────────────────────

TEST_CASE("event 0 is reserved and never parses", "[replay]") {
  // A run of zero bytes is the likeliest shape of a corrupt or truncated file.
  // It must not read as a valid sequence of no-ops.
  auto f = make();
  f.image[replay::kHeaderBytes + 4] = std::byte{0};
  const auto res = replay::verify(f.image);
  CHECK(res.error == ReplayError::ReservedEvent);
  CHECK(res.record_index == 0);
}

TEST_CASE("an unknown event value is refused, and names its record", "[replay]") {
  auto f = make();
  const std::uint32_t victim = 3;
  f.image[replay::kHeaderBytes + replay::kRecordBytes * victim + 4] = std::byte{200};
  const auto res = replay::verify(f.image);
  CHECK(res.error == ReplayError::UnknownEvent);
  CHECK(res.record_index == victim);
}

TEST_CASE("a payload the event cannot mean is refused, not clamped", "[replay]") {
  // Clamping would play a session nobody recorded, reach a world hash nobody
  // predicted, and report it as a determinism regression.
  struct Case {
    Event event;
    std::uint16_t payload;
  };
  const Case cases[] = {
      {Event::Step, 4},   {Event::Step, 65535}, {Event::Turn, 4},
      {Event::Lamp, 6},   {Event::Creep, 2},    {Event::Wait, 1},
  };

  for (const auto& c : cases) {
    INFO("event " << static_cast<int>(c.event) << " payload " << c.payload);
    CHECK_FALSE(replay::payload_valid(c.event, c.payload));

    std::byte buf[replay::kRecordBytes]{};
    const replay::Record record{0, c.event, c.payload};
    CHECK(replay::write_record(buf, record).error == ReplayError::PayloadOutOfRange);
  }
}

TEST_CASE("every payload the events CAN mean is accepted", "[replay]") {
  for (std::uint16_t d = 0; d < kDirCount; ++d) {
    CHECK(replay::payload_valid(Event::Step, d));
    CHECK(replay::payload_valid(Event::Turn, d));
  }
  for (std::uint16_t l = kLampLevelMin; l <= kLampLevelMax; ++l) {
    CHECK(replay::payload_valid(Event::Lamp, l));
  }
  CHECK(replay::payload_valid(Event::Creep, 0));
  CHECK(replay::payload_valid(Event::Creep, 1));
  CHECK(replay::payload_valid(Event::Wait, 0));
}

TEST_CASE("ticks may repeat but may not go backwards", "[replay]") {
  // §3 says "ordered by tick". Two inputs in one tick is a real thing a player
  // does; a record that travels back in time is not.
  replay::Header header{};
  std::vector<std::byte> image(replay::image_bytes(3));

  const std::vector<replay::Record> equal{{4, Event::Wait, 0}, {4, Event::Wait, 0},
                                          {4, Event::Wait, 0}};
  CHECK(replay::assemble(header, equal, image));

  const std::vector<replay::Record> backwards{{4, Event::Wait, 0}, {5, Event::Wait, 0},
                                              {2, Event::Wait, 0}};
  const auto res = replay::assemble(header, backwards, image);
  CHECK(res.error == ReplayError::TicksOutOfOrder);
  CHECK(res.record_index == 2);
}

// ── Refusals: the digest ────────────────────────────────────────────────────

TEST_CASE("every single-byte flip in the covered region is refused", "[replay]") {
  // The point of the sweep is that NO flip inside [40, EOF) is accepted. Some
  // are caught by the structure checks before the digest is ever computed —
  // that is the intended order, and either refusal is a refusal.
  const auto f = make();
  for (std::size_t i = replay::kDigestCoverageStart; i < f.image.size(); ++i) {
    auto corrupt = f.image;
    corrupt[i] = static_cast<std::byte>(static_cast<std::uint8_t>(corrupt[i]) ^ 0x01U);
    INFO("flipped byte " << i);
    CHECK_FALSE(replay::verify(corrupt));
  }
}

TEST_CASE("a flip the structure checks cannot see is caught by the digest", "[replay]") {
  // The seed is eight bytes of arbitrary integer: nothing about it is
  // structurally checkable, so it is exactly the region the digest is for.
  auto f = make();
  f.image[40] = static_cast<std::byte>(static_cast<std::uint8_t>(f.image[40]) ^ 0x01U);
  CHECK(replay::verify(f.image).error == ReplayError::FileDigestMismatch);
}

TEST_CASE("flipping the digest field itself is caught", "[replay]") {
  auto f = make();
  f.image[replay::kDigestOffset] =
      static_cast<std::byte>(static_cast<std::uint8_t>(f.image[replay::kDigestOffset]) ^ 0x01U);
  CHECK(replay::verify(f.image).error == ReplayError::FileDigestMismatch);
}

TEST_CASE("magic and version are checked BEFORE the digest, not by it", "[replay]") {
  // They sit outside the coverage on purpose: you have to read them to know
  // whether the rest of the file has the shape you are about to hash, and a
  // digest you cannot check without first trusting the file is not a check.
  auto f = make();
  f.image[0] = std::byte{'X'};
  CHECK(replay::verify(f.image).error == ReplayError::BadMagic);

  auto g = make();
  g.image[4] = std::byte{99};
  CHECK(replay::verify(g.image).error == ReplayError::UnsupportedVersion);
}

// ── The reject / warn split, which is the whole of §12 ──────────────────────

TEST_CASE("a ruleset mismatch is REJECTED, and yields nothing", "[replay]") {
  const auto f = make();
  std::vector<replay::Record> out(f.records.size());
  replay::Header header{};

  const auto res = replay::load(f.image, {kRuleset ^ 1ULL, replay::kNoPackHash}, header, out);
  CHECK(res.error == ReplayError::RulesetMismatch);
  CHECK_FALSE(static_cast<bool>(res));
  // The HEADER is not handed back, which is the guarantee that matters: nothing
  // downstream can act on a replay this build was told to refuse. The records
  // span is deliberately NOT asserted untouched — `load` fills it as it reads,
  // and replay.hpp says so rather than promising an atomicity that held only by
  // accident of which check ran first.
  CHECK(header.seed == 0);
  CHECK(header.record_count == 0);
}

TEST_CASE("a pack mismatch WARNS: the load succeeds and the flag is set", "[replay]") {
  // SCHEMAS.md §3: "art changes do not affect simulation, so a mismatch warns
  // rather than rejects". If this ever becomes an error, a re-baked pack stops
  // every golden replay in the corpus for a reason that cannot affect them.
  const auto f = make(0xAABBCCDD11223344ULL);
  std::vector<replay::Record> out(f.records.size());
  replay::Header header{};

  const auto res = replay::load(f.image, {kRuleset, 0x9999999999999999ULL}, header, out);
  CHECK(res.error == ReplayError::None);
  CHECK(static_cast<bool>(res));
  CHECK(res.pack_hash_mismatch);
  CHECK(header.seed == kSeed);
  CHECK(out == f.records);
}

TEST_CASE("a matching pack hash does not warn", "[replay]") {
  const auto f = make(0xAABBCCDD11223344ULL);
  std::vector<replay::Record> out(f.records.size());
  replay::Header header{};

  const auto res = replay::load(f.image, matching_expect(f), header, out);
  CHECK(res);
  CHECK_FALSE(res.pack_hash_mismatch);
}

TEST_CASE("a replay that recorded no pack never warns about one", "[replay]") {
  // Every replay is sim-only until the compositor exists (#7). Warning on all
  // of them is how a warning becomes noise and then becomes ignored.
  const auto f = make(replay::kNoPackHash);
  std::vector<replay::Record> out(f.records.size());
  replay::Header header{};

  const auto res = replay::load(f.image, {kRuleset, 0x1234567812345678ULL}, header, out);
  CHECK(res);
  CHECK_FALSE(res.pack_hash_mismatch);
}

TEST_CASE("load refuses a records buffer too small to hold the log", "[replay]") {
  const auto f = make();
  std::vector<replay::Record> out(f.records.size() - 1);
  replay::Header header{};
  CHECK(replay::load(f.image, matching_expect(f), header, out).error ==
        ReplayError::BufferTooSmall);
}

TEST_CASE("write_header and write_record refuse a buffer too small", "[replay]") {
  std::vector<std::byte> tiny(replay::kHeaderBytes - 1);
  const replay::Header header{};
  CHECK(replay::write_header(tiny, header).error == ReplayError::BufferTooSmall);

  std::vector<std::byte> tiny_record(replay::kRecordBytes - 1);
  CHECK(replay::write_record(tiny_record, {0, Event::Wait, 0}).error ==
        ReplayError::BufferTooSmall);
}

// ── Layout locks ────────────────────────────────────────────────────────────

TEST_CASE("the format is little-endian, asserted by literal", "[replay]") {
  // SCHEMAS.md §3 never stated an endianness. This is where it is stated, in
  // the only form that cannot drift from the implementation.
  replay::Header header{};
  header.seed = 0x0807060504030201ULL;
  header.ruleset_hash = 0x1122334455667788ULL;
  std::vector<std::byte> image(replay::image_bytes(1));
  // The tick's top byte is zero because `kMaxTick` (10,000,000) refuses
  // anything larger — the byte order is still what is being asserted.
  const std::vector<replay::Record> records{{0x00030201U, Event::Turn, 0x0002U}};
  REQUIRE(replay::assemble(header, records, image));

  // seed at +40, least-significant byte first
  CHECK(image[40] == std::byte{0x01});
  CHECK(image[41] == std::byte{0x02});
  CHECK(image[47] == std::byte{0x08});

  // ruleset_hash at +48
  CHECK(image[48] == std::byte{0x88});
  CHECK(image[55] == std::byte{0x11});

  // record: tick:u32 at +0, event:u8 at +4, payload:u16 at +5
  const auto r = replay::kHeaderBytes;
  CHECK(image[r + 0] == std::byte{0x01});
  CHECK(image[r + 1] == std::byte{0x02});
  CHECK(image[r + 2] == std::byte{0x03});
  CHECK(image[r + 3] == std::byte{0x00});
  CHECK(image[r + 4] == std::byte{0x02});  // Event::Turn
  CHECK(image[r + 5] == std::byte{0x02});
  CHECK(image[r + 6] == std::byte{0x00});
}

TEST_CASE("the layout constants are what the header and records occupy", "[replay]") {
  CHECK(replay::kHeaderBytes == 104);
  CHECK(replay::kRecordBytes == 7);  // §3 says "packed"; seven is what packed means
  CHECK(replay::kDigestOffset == 8);
  CHECK(replay::kDigestCoverageStart == 40);
  CHECK(replay::image_bytes(0) == replay::kHeaderBytes);
  CHECK(replay::image_bytes(10) == replay::kHeaderBytes + 70);

  const auto f = make();
  CHECK(f.image.size() == replay::kHeaderBytes + replay::kRecordBytes * f.records.size());
}

TEST_CASE("pack_hash is the manifest digest's first eight bytes, in file order", "[replay]") {
  // The property the truncation was chosen for: the eight bytes this writes are
  // byte-for-byte the eight bytes a pack carries at its own offset 8, so the
  // two files can be held up against each other in a hex dump.
  hash::Digest digest{};
  for (std::size_t i = 0; i < digest.size(); ++i) digest[i] = static_cast<std::uint8_t>(i + 1);

  const auto truncated = replay::pack_hash_from(digest);
  CHECK(truncated == 0x0807060504030201ULL);

  auto f = make(truncated);
  for (std::size_t i = 0; i < 8; ++i) {
    INFO("pack_hash byte " << i);
    CHECK(f.image[56 + i] == static_cast<std::byte>(digest[i]));
  }
}

TEST_CASE("the event values are the replay compatibility contract", "[replay]") {
  // Append-only, never renumbered — the same contract test/02rng/ pins on
  // rng::Stream. These values live in files kept "per bug ever filed";
  // renumbering one silently re-interprets every replay recorded before it.
  CHECK(static_cast<std::uint8_t>(Event::None) == 0);
  CHECK(static_cast<std::uint8_t>(Event::Step) == 1);
  CHECK(static_cast<std::uint8_t>(Event::Turn) == 2);
  CHECK(static_cast<std::uint8_t>(Event::Lamp) == 3);
  CHECK(static_cast<std::uint8_t>(Event::Creep) == 4);
  CHECK(static_cast<std::uint8_t>(Event::Wait) == 5);
  CHECK(replay::kEventMax == 5);
}

TEST_CASE("describe never returns null, for every error", "[replay]") {
  for (std::uint8_t e = 0; e <= static_cast<std::uint8_t>(ReplayError::WorldHashMismatch); ++e) {
    INFO("error " << static_cast<int>(e));
    CHECK(replay::describe(static_cast<ReplayError>(e)) != nullptr);
  }
}

// ── Round trip, and the golden prefix ───────────────────────────────────────

TEST_CASE("a replay round-trips through bytes unchanged", "[replay]") {
  const auto f = make(0x00FF00FF00FF00FFULL);
  std::vector<replay::Record> out(f.records.size());
  replay::Header header{};

  REQUIRE(replay::load(f.image, matching_expect(f), header, out));
  CHECK(out == f.records);
  CHECK(header.seed == kSeed);
  CHECK(header.ruleset_hash == kRuleset);
  CHECK(header.pack_hash == 0x00FF00FF00FF00FFULL);
  CHECK(header.tick_hz == replay::kTickHz);
  CHECK(header.record_count == f.records.size());
  CHECK(header.total_bytes == f.image.size());
  CHECK(header.file_sha256 == f.header.file_sha256);
}

TEST_CASE("assembling the same session twice is byte-identical", "[replay]") {
  const auto a = make();
  const auto b = make();
  CHECK(a.image == b.image);
  CHECK(a.header.file_sha256 == b.header.file_sha256);
}

TEST_CASE("the golden header prefix", "[replay]") {
  // Last, and it proves the least: a lock on the first bytes any reader sees.
  const auto f = make();
  CHECK(f.image[0] == std::byte{'G'});
  CHECK(f.image[1] == std::byte{'L'});
  CHECK(f.image[2] == std::byte{'R'});
  CHECK(f.image[3] == std::byte{'P'});
  CHECK(f.image[4] == std::byte{0x01});
  CHECK(f.image[5] == std::byte{0x00});
  CHECK(f.image[6] == std::byte{0x0A});  // tick_hz = 10
  CHECK(f.image[7] == std::byte{0x00});
}

// ── The world hash: what it covers, proved by mutating it ───────────────────
//
// TEST-PLAN.md §2: "The hash covers every byte of simulation state and NOTHING
// from the render layer." A hash cannot be tested by asserting what it equals —
// that only pins today's answer. It is tested by moving each piece of state in
// turn and requiring the digest to notice, which is the only form of "covers
// every byte" a test can actually check.

namespace {

/// M0's shape: a corridor with an alcove, and two monsters watching it.
[[nodiscard]] auto corridor_world() -> World {
  Level level{9, 3};
  level.carve(Coord{0, 1}, Dir::East, 9);  // the corridor
  level.carve(Coord{4, 1}, Dir::North, 2); // an alcove off it

  std::vector<Monster> monsters{
      Monster{Coord{7, 1}, MonsterKind{Acuity::Normal, false}, {}},
      Monster{Coord{4, 0}, MonsterKind{Acuity::Keen, false}, {}},
  };

  auto w = make_world(kSeed, std::move(level), std::move(monsters));
  w.party = Coord{1, 1};
  w.armour = Armour::Leather;
  return w;
}

}  // namespace

TEST_CASE("the world hash notices every field of the world", "[replay][determinism]") {
  const auto base = corridor_world();
  const auto baseline = world_hash(base);

  auto changed = [&](auto&& mutate) {
    auto w = base;
    mutate(w);
    return world_hash(w) != baseline;
  };

  CHECK(changed([](World& w) { w.seed ^= 1ULL; }));
  CHECK(changed([](World& w) { w.tick += 1; }));
  CHECK(changed([](World& w) { w.party.x += 1; }));
  CHECK(changed([](World& w) { w.party.y += 1; }));
  CHECK(changed([](World& w) { w.facing = Dir::South; }));
  CHECK(changed([](World& w) { w.lamp_level = 0; }));
  CHECK(changed([](World& w) { w.creeping = true; }));
  CHECK(changed([](World& w) { w.armour = Armour::Plate; }));
  CHECK(changed([](World& w) { w.pending_noise += 1; }));

  CHECK(changed([](World& w) { w.level = Level{9, 4}; }));
  CHECK(changed([](World& w) { w.level.at_mut(Coord{2, 1})->kind = CellKind::Void; }));
  CHECK(changed([](World& w) { w.level.at_mut(Coord{2, 1})->edges[0].kind = EdgeKind::Door; }));
  CHECK(changed([](World& w) { w.level.at_mut(Coord{2, 1})->edges[0].state = EdgeState::Locked; }));
  CHECK(changed([](World& w) { w.level.at_mut(Coord{2, 1})->edges[1].key_id = 7; }));
  CHECK(changed([](World& w) { w.level.at_mut(Coord{2, 1})->edges[2].attenuation_override = 9; }));
  CHECK(changed([](World& w) { w.level.at_mut(Coord{2, 1})->inscription_id = 3; }));
  CHECK(changed([](World& w) { w.level.at_mut(Coord{2, 1})->contents.push_back(11); }));

  CHECK(changed([](World& w) { w.monsters.pop_back(); }));
  CHECK(changed([](World& w) { w.monsters[0].at.x += 1; }));
  CHECK(changed([](World& w) { w.monsters[0].at.y += 1; }));
  CHECK(changed([](World& w) { w.monsters[0].kind.acuity = Acuity::Dull; }));
  CHECK(changed([](World& w) { w.monsters[0].kind.sees_unlit = true; }));
  CHECK(changed([](World& w) { w.monsters[0].mind.state = Awareness::Hunting; }));
  CHECK(changed([](World& w) { w.monsters[0].mind.ticks_in_state += 1; }));
  CHECK(changed([](World& w) { w.monsters[0].mind.los_streak += 1; }));
  CHECK(changed([](World& w) { w.monsters[0].mind.ticks_since_hit += 1; }));
  CHECK(changed([](World& w) { w.monsters[0].mind.last_known.x += 1; }));
  CHECK(changed([](World& w) { w.monsters[0].mind.last_known.y += 1; }));
  CHECK(changed([](World& w) { w.monsters[0].mind.has_last_known = true; }));

  // §6.4's route and its cursor. Six of these seven are the cursor, and the
  // cursor is the half most likely to be forgotten: a route that compares equal
  // while the monster is at a different point along it is a world that replays
  // to a different future from the same digest.
  CHECK(changed([](World& w) { w.monsters[0].facing = Dir::West; }));
  CHECK(changed([](World& w) { w.monsters[0].patrol.route.push_back(Coord{7, 1}); }));
  CHECK(changed([](World& w) { w.monsters[0].patrol.dwell.push_back(3); }));
  CHECK(changed([](World& w) { w.monsters[0].patrol.waypoint += 1; }));
  CHECK(changed([](World& w) { w.monsters[0].patrol.reversed = true; }));
  CHECK(changed([](World& w) { w.monsters[0].patrol.dwell_left += 1; }));
  CHECK(changed([](World& w) { w.monsters[0].patrol.move_cooldown += 1; }));

  // Every simulation stream. If one of these ever stops mattering, it is the
  // static_assert in world.cpp that should be arguing about it, not silence.
  for (std::size_t i = 0; i + 1 < kStreamCount; ++i) {
    INFO("stream index " << i);
    CHECK(changed([i](World& w) { w.rng_state[i] ^= 1ULL; }));
  }
}

TEST_CASE("the ambience stream is NOT in the world hash", "[replay][determinism]") {
  // rng.hpp: "non-simulation flavour; never feeds back into sim state". If this
  // ever goes red, a change to ambient sound has become reportable as a
  // determinism regression, and TEST-PLAN.md §2 has no triage path for those.
  const auto base = corridor_world();
  auto w = base;
  w.rng_state[static_cast<std::size_t>(Stream::Ambience) - 1] ^= 0xFFFFFFFFFFFFFFFFULL;
  CHECK(world_hash(w) == world_hash(base));
}

TEST_CASE("cell contents are length-prefixed, not just concatenated", "[replay][determinism]") {
  // Without the count, one item in each of two cells and two items in the first
  // feed the digest the same bytes in the same order.
  auto split = corridor_world();
  split.level.at_mut(Coord{1, 1})->contents = {1};
  split.level.at_mut(Coord{2, 1})->contents = {2};

  auto together = corridor_world();
  together.level.at_mut(Coord{1, 1})->contents = {1, 2};
  together.level.at_mut(Coord{2, 1})->contents = {};

  CHECK(world_hash(split) != world_hash(together));
}

TEST_CASE("routes are length-prefixed per monster, not concatenated", "[replay][determinism]") {
  // Cell contents' argument, one level up. Without a count PER MONSTER, a route
  // cell handed to the other monster feeds the digest the same bytes in the
  // same order — and the two worlds it fails to distinguish patrol completely
  // differently.
  auto split = corridor_world();
  split.monsters[0].patrol.route = {Coord{7, 1}};
  split.monsters[1].patrol.route = {Coord{4, 0}};

  auto together = corridor_world();
  together.monsters[0].patrol.route = {Coord{7, 1}, Coord{4, 0}};
  together.monsters[1].patrol.route = {};

  CHECK(world_hash(split) != world_hash(together));
}

TEST_CASE("a route and its dwell are prefixed SEPARATELY", "[replay][determinism]") {
  // Sharper than the case above, because `route` and `dwell` are ADJACENT in
  // the digest and are made of different-width elements. One `Coord` is eight
  // bytes and one dwell entry is one, so eight dwell bytes are byte-identical
  // to one route cell — below, both worlds would feed 07 00 00 00 01 00 00 00
  // and nothing else if the two vectors shared a prefix or had none.
  //
  // One is a monster that patrols to (7,1). The other is a monster that does
  // not patrol at all and is carrying eight bytes of malformed dwell. A digest
  // that cannot separate those cannot do the job §12 gives it.
  auto as_route = corridor_world();
  as_route.monsters[0].patrol.route = {Coord{7, 1}};
  as_route.monsters[0].patrol.dwell = {};

  auto as_dwell = corridor_world();
  as_dwell.monsters[0].patrol.route = {};
  as_dwell.monsters[0].patrol.dwell = {7, 0, 0, 0, 1, 0, 0, 0};

  CHECK(world_hash(as_route) != world_hash(as_dwell));
}

TEST_CASE("a world REACHED by replaying hashes the same as one built directly",
          "[replay][determinism]") {
  // Canonicality, and it has to be tested across the two paths that actually
  // differ. A digest that depended on how a Level's cell vector was grown, or
  // on anything a `play` leaves behind that a direct build does not, would pass
  // on the recording path and fail on the replay path — the worst possible
  // split, because the golden case in this file would stay green while
  // `gloam_replay play` went red.
  const Tuning& t = kDefaultTuning;

  auto walked = corridor_world();
  play(walked, {{{0, Event::Step, 1}, {1, Event::Step, 1}}}, t);

  // The same end state, assembled by hand rather than arrived at.
  auto built = corridor_world();
  built.party = Coord{3, 1};
  built.tick = walked.tick;
  built.pending_noise = walked.pending_noise;
  // `facing` joined `mind` with §6.4: these monsters do not patrol, but the
  // party walking past makes one of them SUSPICIOUS, and §6.1's tell for that
  // transition is a head turn. Copying `mind` alone left this case red for a
  // reason that was the pump working correctly.
  //
  // AND NOTHING ELSE IS COPIED, DELIBERATELY. Blanket-copying `patrol` and
  // `rng_state` across would also make this case pass, and would defeat the
  // exact thing it exists to catch — "anything a `play` leaves behind that a
  // direct build does not". Leaving them uncopied is what asserts that a
  // route-less world takes no patrol draw and writes no cursor. If §6.4 ever
  // touches either on a path a direct build cannot reproduce, this goes red
  // here rather than in `gloam_replay play`, which is the whole point.
  for (std::size_t i = 0; i < built.monsters.size(); ++i) {
    built.monsters[i].mind = walked.monsters[i].mind;
    built.monsters[i].facing = walked.monsters[i].facing;
  }

  CHECK(hex_of(world_hash(built)) == hex_of(world_hash(walked)));
}

TEST_CASE("the world hash is stable across repeated evaluation", "[replay][determinism]") {
  const auto w = corridor_world();
  CHECK(world_hash(w) == world_hash(w));
}

// ── The harness: record, write, read, replay, compare ───────────────────────

TEST_CASE("a replayed session reaches the hash the recording reached",
          "[replay][determinism]") {
  // This is BUILD-ORDER step 7's acceptance criterion, in process. The
  // out-of-process and cross-compiler halves are `replay-deterministic` and the
  // CI matrix respectively.
  const Tuning& t = kDefaultTuning;
  const auto records = scripted_records();

  auto recorded = corridor_world();
  play(recorded, records, t);
  const auto expected = world_hash(recorded);

  replay::Header header{};
  header.seed = kSeed;
  header.ruleset_hash = ruleset_hash(t);
  header.final_world_hash = expected;
  std::vector<std::byte> image(replay::image_bytes(static_cast<std::uint32_t>(records.size())));
  REQUIRE(replay::assemble(header, records, image));

  // Everything past here knows only what the file says.
  replay::Header loaded{};
  std::vector<replay::Record> log(records.size());
  REQUIRE(replay::load(image, {ruleset_hash(t), replay::kNoPackHash}, loaded, log));

  auto replayed = corridor_world();
  play(replayed, log, t);

  CHECK(hex_of(world_hash(replayed)) == hex_of(loaded.final_world_hash));
  CHECK(world_hash(replayed) == expected);
  CHECK(replayed.tick == records.back().tick + 1);
}

TEST_CASE("replaying twice from the same file lands in the same place",
          "[replay][determinism]") {
  const Tuning& t = kDefaultTuning;
  const auto records = scripted_records();

  auto a = corridor_world();
  auto b = corridor_world();
  play(a, records, t);
  play(b, records, t);
  CHECK(world_hash(a) == world_hash(b));
}

TEST_CASE("the inputs actually moved the world", "[replay][determinism]") {
  // Guards the harness against the way it could pass while proving nothing: if
  // `play` did nothing at all, every hash above would still agree.
  const Tuning& t = kDefaultTuning;
  const auto before = corridor_world();
  auto after = corridor_world();
  play(after, scripted_records(), t);

  CHECK(world_hash(after) != world_hash(before));
  CHECK(after.party != before.party);
  CHECK(after.creeping);
  CHECK(after.facing == Dir::South);
  CHECK(after.lamp_level == 0);
}

TEST_CASE("a step into a wall moves nothing and emits nothing", "[replay][determinism]") {
  // The decision world.hpp writes down: you did not take a step.
  const Tuning& t = kDefaultTuning;
  auto w = corridor_world();
  w.party = Coord{1, 1};

  apply(w, Event::Step, static_cast<std::uint16_t>(Dir::North), t);
  CHECK(w.party == Coord{1, 1});
  CHECK(w.pending_noise == 0);

  apply(w, Event::Step, static_cast<std::uint16_t>(Dir::East), t);
  CHECK(w.party == Coord{2, 1});
  CHECK(w.pending_noise == step_noise(Armour::Leather, false, t));
}

TEST_CASE("two steps in one tick emit one sound, not a louder one",
          "[replay][determinism]") {
  const Tuning& t = kDefaultTuning;
  auto w = corridor_world();
  apply(w, Event::Step, static_cast<std::uint16_t>(Dir::East), t);
  const auto one = w.pending_noise;
  apply(w, Event::Step, static_cast<std::uint16_t>(Dir::East), t);
  CHECK(w.pending_noise == one);
}

// ── Goldens, last ───────────────────────────────────────────────────────────

TEST_CASE("a golden world hash pins the simulation across compilers",
          "[replay][determinism]") {
  // §19 step 7: "a recorded session replays to an identical world hash on both
  // compilers". The value is whatever the implementation produces — the point
  // is that it never changes, not what it is. If this moves, either the
  // simulation changed or `kWorldHashVersion` should have.
  //
  // Moved with §6.4's patrols, for TWO reasons, and both are the good kind:
  //
  //   1. `kWorldHashVersion` 1 -> 2. The version byte leads the digest, so this
  //      alone would have moved it.
  //   2. A monster in this world turns its head. It does NOT patrol — the
  //      monsters here are deliberately route-less, so the pump itself is a
  //      no-op for them — but the party walking past makes one SUSPICIOUS, and
  //      §6.1's tell for that transition is "head turns toward the source".
  //      `facing` is hashed, because a tell a replay cannot reproduce is not a
  //      tell.
  //
  // Being able to say which is which is the whole point of the version byte.
  const auto records = scripted_records();
  auto w = corridor_world();
  play(w, records, kDefaultTuning);
  CHECK(hex_of(world_hash(w)) == "5d594c7b996e6072592a3dc9abd212f88d9c594ed15ee76cf87b2b6f56b9f91c");
}

TEST_CASE("a golden file digest pins the container across compilers",
          "[replay][determinism]") {
  const auto f = make();
  CHECK(hex_of(f.header.file_sha256) == "3e696df0c3581fb109225f0e50bf99d3da6618106601d181a73a1ac5e4b73b12");
  CHECK(f.image.size() == 160);
}

// ── The refusals the review found missing ───────────────────────────────────

TEST_CASE("a header claiming more records than the file holds is refused", "[replay]") {
  // THE ALLOCATION GUARD. `record_count` and `total_bytes` only agree with each
  // other, and both come from the file: a 104-byte header can claim 613 million
  // records and satisfy that identity exactly. A caller sizing a buffer from an
  // unchecked count allocates ~4.9 GB and dies on bad_alloc. `read_header` must
  // therefore never hand out a count it has not measured against real bytes.
  auto f = make();
  std::vector<std::byte> header_only(f.image.begin(), f.image.begin() + replay::kHeaderBytes);

  replay::Header out{};
  const auto res = replay::read_header(header_only, out);
  CHECK(res.error == ReplayError::TotalBytesMismatch);
  CHECK(out.record_count == 0);  // nothing a caller could size an allocation with

  CHECK(replay::verify(header_only).error == ReplayError::TotalBytesMismatch);
}

TEST_CASE("the largest record_count a u32 total can express is still refused", "[replay]") {
  // The precise hostile header: 613,566,741 records, 4,294,967,291 bytes. The
  // internal identity holds perfectly; only the file's real length disagrees.
  std::vector<std::byte> image(replay::kHeaderBytes);
  const auto ok = replay::write_header(image, replay::Header{});
  REQUIRE(ok);
  image[0] = std::byte{'G'};
  image[1] = std::byte{'L'};
  image[2] = std::byte{'R'};
  image[3] = std::byte{'P'};
  image[4] = std::byte{0x01};
  image[6] = std::byte{0x0A};
  constexpr std::uint32_t kCount = 613566741;
  constexpr std::uint32_t kTotal = 4294967291;
  for (std::size_t i = 0; i < 4; ++i) {
    image[96 + i] = static_cast<std::byte>((kCount >> (8 * i)) & 0xFFU);
    image[100 + i] = static_cast<std::byte>((kTotal >> (8 * i)) & 0xFFU);
  }
  REQUIRE(replay::image_bytes(kCount) == kTotal);  // the identity really does hold

  replay::Header out{};
  CHECK(replay::read_header(image, out).error == ReplayError::TotalBytesMismatch);
}

TEST_CASE("a tick past kMaxTick is refused rather than replayed", "[replay]") {
  // Bounding WORK, not taste. Ticks are reached one at a time by `play`, so an
  // unbounded u32 tick is a 111-byte file that pins a core for twenty minutes.
  replay::Header header{};
  std::vector<std::byte> image(replay::image_bytes(1));

  const std::vector<replay::Record> evil{{replay::kMaxTick + 1, Event::Wait, 0}};
  const auto res = replay::assemble(header, evil, image);
  CHECK(res.error == ReplayError::TickOutOfRange);
  CHECK(res.record_index == 0);

  // And on the read path, which is the one that faces a file from outside.
  const std::vector<replay::Record> fine{{replay::kMaxTick, Event::Wait, 0}};
  replay::Header ok_header{};
  REQUIRE(replay::assemble(ok_header, fine, image));
  image[replay::kHeaderBytes + 0] = static_cast<std::byte>((replay::kMaxTick + 1) & 0xFFU);
  image[replay::kHeaderBytes + 1] = static_cast<std::byte>(((replay::kMaxTick + 1) >> 8) & 0xFFU);
  image[replay::kHeaderBytes + 2] = static_cast<std::byte>(((replay::kMaxTick + 1) >> 16) & 0xFFU);
  image[replay::kHeaderBytes + 3] = static_cast<std::byte>(((replay::kMaxTick + 1) >> 24) & 0xFFU);
  CHECK(replay::verify(image).error == ReplayError::TickOutOfRange);
}

TEST_CASE("load refuses an empty records span rather than reporting success", "[replay]") {
  const auto f = make();
  replay::Header header{};
  CHECK(replay::load(f.image, matching_expect(f), header, {}).error ==
        ReplayError::BufferTooSmall);
}

TEST_CASE("assemble accepts an oversized buffer and uses only what it needs", "[replay]") {
  // A recorder that sizes for a worst-case session once and assembles a shorter
  // one is the obvious shape for a real input recorder. If the digest were
  // taken over the whole span rather than over `total`, every such file would
  // be rejected by its own verify() as corrupt.
  const auto records = scripted_records();
  const auto needed = replay::image_bytes(static_cast<std::uint32_t>(records.size()));

  replay::Header header{};
  std::vector<std::byte> big(needed + 64, std::byte{0xAB});
  const auto res = replay::assemble(header, records, big);
  REQUIRE(res);
  CHECK(res.bytes == needed);
  CHECK(header.total_bytes == needed);

  // The trailing slack is untouched, and the file made of the first `bytes`
  // bytes verifies on its own.
  CHECK(big[needed] == std::byte{0xAB});
  CHECK(replay::verify(std::span{big}.first(res.bytes)));

  const auto exact = make();
  CHECK(std::equal(big.begin(), big.begin() + static_cast<std::ptrdiff_t>(needed),
                   exact.image.begin()) == (header.seed == exact.header.seed));
}

TEST_CASE("write_record refuses a reserved or unknown event, with the right error", "[replay]") {
  // Not folded into the payload case: `payload_valid` also returns false for
  // both, so without asserting the SPECIFIC error these guards can be deleted
  // and the record is still refused — just diagnosed as a bad payload on a
  // record whose payload is fine and whose EVENT is the problem.
  std::byte buf[replay::kRecordBytes]{};
  CHECK(replay::write_record(buf, {0, Event::None, 0}).error == ReplayError::ReservedEvent);
  CHECK(replay::write_record(buf, {0, static_cast<Event>(200), 0}).error ==
        ReplayError::UnknownEvent);
  CHECK(replay::write_record(buf, {0, static_cast<Event>(6), 0}).error ==
        ReplayError::UnknownEvent);
}

TEST_CASE("read_record refuses a span too short to hold one", "[replay]") {
  const std::byte buf[replay::kRecordBytes]{};
  replay::Record out{};
  for (std::size_t n = 0; n < replay::kRecordBytes; ++n) {
    INFO("span length " << n);
    CHECK(replay::read_record(std::span{buf}.first(n), out).error == ReplayError::Truncated);
  }
}

TEST_CASE("every error describes itself distinctly", "[replay]") {
  // The not-null version of this passed with all sixteen arms collapsed to one
  // string. `describe` is the ONLY diagnosis a user gets from a rejected file,
  // and check_replay_determinism.cmake relays it into its own failure text, so
  // two errors sharing a sentence is a bug report that says the wrong thing.
  std::vector<std::string> seen;
  for (std::uint8_t e = 0; e <= static_cast<std::uint8_t>(ReplayError::TickOutOfRange); ++e) {
    const auto* text = replay::describe(static_cast<ReplayError>(e));
    INFO("error " << static_cast<int>(e));
    REQUIRE(text != nullptr);
    CHECK(std::string{text} != "unknown error");  // no arm falls off the switch
    seen.emplace_back(text);
  }
  std::sort(seen.begin(), seen.end());
  CHECK(std::adjacent_find(seen.begin(), seen.end()) == seen.end());
}

TEST_CASE("the stream accessors index by stream - 1, and refuse to underflow",
          "[replay][determinism]") {
  // Untested until now, and a mutation to `[s % kStreamCount]` — an
  // out-of-bounds read for Ambience and a neighbour's generator for every other
  // stream — passed the whole suite.
  auto w = make_world(kSeed, Level{2, 2}, {});

  for (std::uint64_t i = 1; i <= kStreamCount; ++i) {
    const auto s = static_cast<Stream>(i);
    INFO("stream " << i);
    CHECK(stream_of(w, s).state() == w.rng_state[i - 1]);
    CHECK(stream_of(w, s).state() == rng(kSeed, s).state());
  }

  // Distinct per stream: the whole point of naming them (§5.1).
  CHECK(stream_of(w, Stream::Level).state() != stream_of(w, Stream::Patrol).state());

  save_stream(w, Stream::Combat, Rng{0xFEEDFACEULL});
  CHECK(w.rng_state[static_cast<std::size_t>(Stream::Combat) - 1] == 0xFEEDFACEULL);

  // Out of range clamps to a real slot instead of computing SIZE_MAX and
  // writing through it.
  const auto before = w.rng_state;
  save_stream(w, static_cast<Stream>(0), Rng{1});
  save_stream(w, static_cast<Stream>(99), Rng{2});
  CHECK(w.rng_state[0] == 2);
  CHECK(std::equal(before.begin() + 1, before.end(), w.rng_state.begin() + 1));
}

TEST_CASE("make_world seeds every stream from the world seed", "[replay][determinism]") {
  const auto w = make_world(0x1234'5678'9ABC'DEF0ULL, Level{1, 1}, {});
  for (std::uint64_t i = 1; i <= kStreamCount; ++i) {
    INFO("stream " << i);
    CHECK(w.rng_state[i - 1] == rng(0x1234'5678'9ABC'DEF0ULL, static_cast<Stream>(i)).state());
  }
  CHECK(w.seed == 0x1234'5678'9ABC'DEF0ULL);
}
