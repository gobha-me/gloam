// SPEC §9.2, §10 — the pack's audio arena: the launch GATE and the LOAD
// (gloam#23, UPSTREAM.md item 10's resolution).
//
// What is pinned here and nowhere else: the layering between verify() and the
// gate. `pack::verify` is structural — it can say an audio record is
// well-formed, but it cannot know what a SoundId IS, so "the pack carries
// every sound this binary was built with" is a semantic question and it is
// pack_audio's. Several cases below are images verify ACCEPTS and the gate
// refuses; that refusal landing in the right layer is the design.
//
// Failure matrix first, per AGENTS.md. The happy path — bake, load, and the
// bit-exact expansion back to the synthesised arena — is the LAST case.

#include <catch2/catch_all.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "gloam/assets.hpp"
#include "gloam/audio.hpp"
#include "gloam/pack.hpp"
#include "gloam/plate.hpp"

#include "pack_audio.hpp"
#include "sfx.hpp"

using namespace gloam;

namespace {

constexpr int kW = 8;  // 12pack's dither-aligned minimum, reused: small enough
constexpr int kH = 2;  // to reason about, valid under plate::validate.

// The real pack, baked once, through the same call gloam_bake runs. The gate
// and the load are exercised against the shipped artifact, not a stand-in.
auto baked_image() -> const std::vector<std::byte>& {
  static const std::vector<std::byte> image = [] {
    std::vector<std::byte> pixels(assets::pixel_bytes());
    std::vector<std::int16_t> pcm(assets::kAudioArenaFrames);
    std::array<sfx::Clip, audio::kSoundIdCount> clips{};
    REQUIRE(sfx::synthesise_i16(sfx::kArenaSeed, pcm, clips));
    std::array<assets::AudioSource, assets::kAudioCount> audio{};
    REQUIRE(sfx::pack_sources(pcm, clips, audio));
    std::vector<std::byte> audio_bytes(assets::kAudioBlobBytes);
    constexpr auto kRecords =
        static_cast<std::size_t>(assets::kPlateCount) + assets::kAudioCount;
    std::vector<pack::Record> records(kRecords);
    std::vector<std::span<const std::byte>> blobs(kRecords);
    std::vector<std::byte> out(assets::image_bytes());
    REQUIRE(assets::build_pack(pixels, records, blobs, audio, audio_bytes, out));
    return out;
  }();
  return image;
}

// A hand-built pack: ONE plate (so the pack is structurally complete — an
// empty plate run is ZeroPlates, a different lesson) plus an audio record per
// id in `sound_ids`, each carrying `frames` frames of zeroed s16le. assemble
// and verify both pass it: every refusal the gate gives below is the SEMANTIC
// layer, arrived at over a structurally valid image.
auto hand_built(std::span<const std::uint16_t> sound_ids, std::uint32_t frames = 100)
    -> std::vector<std::byte> {
  std::vector<pack::Record> records;
  std::vector<std::vector<std::byte>> blob_store;

  pack::Record plate{};
  plate.plate_id = 0;
  plate.role = pack::Role::Wall;
  plate.depth = pack::kDepthFullFrame;
  plate.lateral = pack::Lateral::FullFrame;
  plate.variant = 0;
  plate.codec = pack::Codec::RawPlanes;
  plate.w = kW;
  plate.h = kH;
  records.push_back(plate);
  blob_store.emplace_back(plate::blob_bytes(kW, kH), std::byte{0});

  for (const auto id : sound_ids) {
    pack::Record sound{};
    sound.plate_id = id;  // the sound id under Role::Audio
    sound.role = pack::Role::Audio;
    sound.sample_rate = static_cast<std::uint16_t>(audio::kSampleRateHz);
    sound.channels = 1;
    sound.sample_format = pack::SampleFormat::S16Le;
    sound.frame_count = frames;
    records.push_back(sound);
    blob_store.emplace_back(static_cast<std::size_t>(frames) * 2, std::byte{0});
  }

  std::vector<std::span<const std::byte>> blobs;
  for (const auto& blob : blob_store) blobs.emplace_back(blob);
  std::vector<std::byte> out(pack::image_bytes(records));
  REQUIRE(pack::assemble(records, blobs, out));
  return out;
}

// Byte surgery on the baked image, little-endian and by hand — the point is a
// pack that is DIGEST-WRONG but parser-readable, because the gate's own reads
// are what is under test. (verify would catch every one of these by digest;
// the gate is exercised ALONE, which is the layer the refusal belongs to.)
auto audio_record_at(std::vector<std::byte>& image, std::uint16_t audio_index)
    -> std::size_t {
  pack::Header header{};
  REQUIRE(pack::read_header(image, header));
  return pack::kHeaderBytes +
         pack::kRecordBytes * static_cast<std::size_t>(header.plate_count + audio_index);
}

void put_u16_at(std::vector<std::byte>& image, std::size_t at, std::uint16_t value) {
  image[at] = static_cast<std::byte>(value & 0xFFU);
  image[at + 1] = static_cast<std::byte>(value >> 8U);
}

void put_u32_at(std::vector<std::byte>& image, std::size_t at, std::uint32_t value) {
  for (std::uint32_t i = 0; i < 4; ++i) {
    image[at + i] = static_cast<std::byte>((value >> (8U * i)) & 0xFFU);
  }
}

}  // namespace

TEST_CASE("a structurally valid pack with no audio run passes verify and is refused at the gate",
          "[packaudio]") {
  const auto image = hand_built({});
  // THE LAYERING, PINNED: verify has no opinion — a plates-only pack is
  // well-formed — and the gate has the decisive one. §9.2's degradation
  // clause covers a missing audio DEVICE; missing audio CONTENT refuses.
  REQUIRE(pack::verify(image));
  const auto res = pack_audio::check(image);
  CHECK(res.error == pack_audio::LoadError::MissingAudio);
  // No run at all implicates no single SoundId — 0 is "none named", unlike
  // the coverage-hole case below, which names the hole.
  CHECK(res.sound_id == 0);
}

TEST_CASE("a coverage hole is MissingAudio naming the missing SoundId", "[packaudio]") {
  const auto image = hand_built(std::array<std::uint16_t, 2>{1, 3});
  REQUIRE(pack::verify(image));
  const auto res = pack_audio::check(image);
  CHECK(res.error == pack_audio::LoadError::MissingAudio);
  CHECK(res.sound_id == 2);
}

TEST_CASE("a sound this binary was not built with is UnknownSound, and verify has no opinion",
          "[packaudio]") {
  // The forward case: a future pack carrying a fourth sound read by THIS
  // binary. Structure is fine — the refusal is the semantic layer's.
  const auto image = hand_built(std::array<std::uint16_t, 4>{1, 2, 3, 7});
  REQUIRE(pack::verify(image));
  const auto res = pack_audio::check(image);
  CHECK(res.error == pack_audio::LoadError::UnknownSound);
  CHECK(res.sound_id == 7);
}

TEST_CASE("a record that redescribes the stream is refused, not reinterpreted", "[packaudio]") {
  {
    // A 44.1 kHz claim: the arena the mixer reads is 48 kHz mono float, so
    // the stream's shape is not the record's to redescribe.
    auto image = baked_image();
    put_u16_at(image, audio_record_at(image, 0) + 3, 44'100);
    const auto res = pack_audio::check(image);
    CHECK(res.error == pack_audio::LoadError::UnexpectedFormat);
    CHECK(res.sound_id == 1);
  }
  {
    // A stereo claim, same refusal for the same reason.
    auto image = baked_image();
    image[audio_record_at(image, 1) + 5] = std::byte{2};
    const auto res = pack_audio::check(image);
    CHECK(res.error == pack_audio::LoadError::UnexpectedFormat);
    CHECK(res.sound_id == 2);
  }
  {
    // Zero frames: a sound with no samples is not silence, it is a bug.
    auto image = baked_image();
    put_u32_at(image, audio_record_at(image, 2) + 8, 0);
    const auto res = pack_audio::check(image);
    CHECK(res.error == pack_audio::LoadError::UnexpectedFormat);
    CHECK(res.sound_id == 3);
  }
}

TEST_CASE("two records for one SoundId are DuplicateSound — verify's ordering gets there first",
          "[packaudio]") {
  auto image = baked_image();
  put_u16_at(image, audio_record_at(image, 1) + 0, 1);  // the sting's record claims id 1
  // verify's strictly-increasing-ids rule refuses this image (ids 1, 1, 3),
  // which is exactly why the loader's branch exists: the loader's own
  // invariant — one record per SoundId — never depends on that ordering.
  CHECK_FALSE(pack::verify(image));
  const auto res = pack_audio::check(image);
  CHECK(res.error == pack_audio::LoadError::DuplicateSound);
  CHECK(res.sound_id == 1);
}

TEST_CASE("the load refuses a short arena, having written nothing", "[packaudio]") {
  // sfx::synthesise's rule, kept: a short arena means the binary and the pack
  // disagree about a constant, and clamping would make it a sound bug nobody
  // traces back here.
  std::vector<float> arena(100, 0.25f);
  const auto before = arena;
  std::array<sfx::Clip, audio::kSoundIdCount> clips{};
  const auto res = pack_audio::load(baked_image(), arena, clips);
  CHECK(res.error == pack_audio::LoadError::ArenaTooSmall);
  CHECK(std::memcmp(arena.data(), before.data(), arena.size() * sizeof(float)) == 0);
}

TEST_CASE("the gate costs no arena and reports the extent the records describe",
          "[packaudio]") {
  // This is what lets m0 run the gate ALWAYS, muted or not: records only, no
  // float arena, no device.
  const auto res = pack_audio::check(baked_image());
  REQUIRE(res);
  CHECK(res.frames == assets::kAudioArenaFrames);
}

TEST_CASE("every LoadError has its own spelling", "[packaudio]") {
  for (std::uint8_t v = 0; v <= 6; ++v) {
    const auto text = pack_audio::name(static_cast<pack_audio::LoadError>(v));
    CHECK_FALSE(text.empty());
    CHECK(text != "UnknownLoadError");
  }
}

TEST_CASE("the loaded arena is the synthesised one, clip by clip, bit-exactly",
          "[packaudio]") {
  // The e2e, and the last case because it only proves the thing works: bake
  // through gloam_bake's own call, load through the runtime path m0 runs, and
  // every sample must equal the float synthesis BIT-EXACTLY — the expansion
  // is a multiply by 1/32768, a power of two, so approximate compare would be
  // a lie about what is guaranteed.
  std::vector<float> reference(sfx::kArenaFrames);
  std::array<sfx::Clip, audio::kSoundIdCount> reference_clips{};
  REQUIRE(sfx::synthesise(sfx::kArenaSeed, reference, reference_clips));

  std::vector<float> arena(sfx::kArenaFrames);
  std::array<sfx::Clip, audio::kSoundIdCount> clips{};
  clips[0] = sfx::Clip{9, 9};  // SoundId::None gets no record; the load resets it
  REQUIRE(pack_audio::load(baked_image(), arena, clips));

  CHECK(clips[0].offset == 0);
  CHECK(clips[0].frames == 0);

  // THE LAYOUT NOTE, asserted: clips land at cumulative offsets in record
  // (id) order — party, sting, monster — which is NOT the synthesis arena's
  // generation order. The clips table is the only map anyone reads, and the
  // comparison below is per-clip for exactly that reason.
  CHECK(clips[static_cast<std::size_t>(audio::SoundId::PartyFootfall)].offset == 0);
  CHECK(clips[static_cast<std::size_t>(audio::SoundId::HuntingSting)].offset ==
        audio::kPartyFootfallFrames);
  CHECK(clips[static_cast<std::size_t>(audio::SoundId::MonsterFootfall)].offset ==
        audio::kPartyFootfallFrames + audio::kHuntingStingFrames);

  for (std::size_t id = 1; id < audio::kSoundIdCount; ++id) {
    INFO("sound " << id);
    CHECK(clips[id].frames == reference_clips[id].frames);
    CHECK(std::memcmp(arena.data() + clips[id].offset, reference.data() + reference_clips[id].offset,
                      clips[id].frames * sizeof(float)) == 0);
  }
}
