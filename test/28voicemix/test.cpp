// SPEC §9.2 — the real-time callback, audited without a real-time thread.
//
// > The callback never allocates, never locks, never touches sim state. Any of
// > the three is a dropout, and a dropout in a stealth game is a lie about the
// > world.
//
// Two of those three are structural and one is not. "Never touches sim state" is
// closed by `render`'s signature, which names no simulation type. "Never locks"
// is closed by there being nothing to lock — plus the thread case at the bottom,
// which is what TSan reads. "NEVER ALLOCATES" is closed by nothing at all unless
// something counts, so this file replaces the global allocation functions and
// counts.
//
// The other half of the suite is arithmetic that has to be exact rather than
// close: `audio.hpp` justifies `kGainUnity` being a power of two by claiming the
// float conversion is exact, and a test that accepted a tolerance there would let
// that claim rot silently. Every gain and pan assertion below uses `==`.
//
// Failure matrix first, per AGENTS.md; the happy path is last.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>
#include <span>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "gloam/audio.hpp"
#include "sfx.hpp"
#include "voice_mixer.hpp"

using namespace gloam;

// ── The allocation counter ─────────────────────────────────────────────────
//
// Global operator new/delete replacement, ARMED BY A FLAG rather than always on.
// Catch2 allocates constantly — building assertion strings, growing its section
// tracker — so a counter live at namespace scope would report Catch2's work as
// the mixer's. The flag is set immediately before the call under test and
// cleared immediately after.
//
// This is also the second reason src/bin/audio_device.cpp is not in this target:
// these operators are global, so anything else linked here would be counted too.
//
// ── EVERY FORM IS REPLACED, NOT ONLY THE SCALAR ONE ────────────────────────
//
// This file used to replace only `operator new(size_t)` and the two scalar
// deletes, relying on the library to route everything else through them. That
// holds for libstdc++, whose `operator new[](size_t)` is literally
// `jmp operator new@plt`, and it is false under GCC's AddressSanitizer:
// libasan.so exports all twenty allocation/deallocation functions itself, and
// its `operator new[]` calls `__asan::asan_memalign` directly, never
// `operator new`. libasan is first in the dynamic lookup order, so on the
// gcc/asan leg `::operator new[](64)` resolved to the runtime and the counter
// read 0 (#57, CI run 40549869087) — an array allocation in the callback would
// have been invisible on exactly the leg that exists to catch memory misuse.
//
// So the counter owns the whole set: scalar and array, plain, nothrow and
// align_val_t, and every matching delete. The deletes matter as much as the
// news. Replacing `operator new[]` but leaving `operator delete[]` to ASan would
// hand malloc'd memory to the runtime's `operator delete[]`, which ASan reports
// as alloc-dealloc-mismatch; owning both halves keeps every pair on
// malloc/aligned_alloc → free, which ASan checks and accepts. Nothing here
// forwards to another replaced operator, so no form depends on a library's
// choice of routing again.
//
// ── AND IT CANNOT EXIST UNDER ThreadSanitizer ──────────────────────────────
//
// TSan's runtime replaces the global operators itself, and Clang's copy defines
// them STRONGLY: linking this file against libclang_rt.tsan_cxx gives
// "multiple definition of `operator new(unsigned long)'" and the target does not
// build at all. GCC's TSan tolerated it (its definitions are weak) and Clang's
// ASan tolerated it, so this showed up on exactly ONE leg of eight — which is
// the argument for running the whole matrix rather than a representative corner
// of it.
//
// The guard below covers BOTH TSan implementations rather than only the one that
// breaks. Narrowing it to `__clang__` would keep the case alive on the GCC TSan
// leg, and was rejected: the claim is not compiler-specific, the non-TSan legs
// prove it, and a workaround keyed on which vendor's runtime happens to use weak
// symbols is exactly the kind of thing that rots without anyone noticing.
//
// The case below therefore SKIPs under TSan rather than compiling to nothing.
// A vanished test that still reports green is the failure mode this project
// spends most of its effort on; a skip says so in the ctest output. The claim
// itself is not compiler-specific, so proving it on the non-TSan legs is
// proof enough.
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define GLOAM_TSAN_ACTIVE 1
#endif
#endif
#if defined(__SANITIZE_THREAD__)
#define GLOAM_TSAN_ACTIVE 1
#endif
#ifndef GLOAM_TSAN_ACTIVE
#define GLOAM_TSAN_ACTIVE 0
#endif

namespace {
std::atomic<bool> g_counting{false};
std::atomic<std::uint64_t> g_allocations{0};

struct CountAllocations {
  CountAllocations() {
    g_allocations.store(0, std::memory_order_relaxed);
    g_counting.store(true, std::memory_order_relaxed);
  }
  ~CountAllocations() { g_counting.store(false, std::memory_order_relaxed); }
  CountAllocations(const CountAllocations&) = delete;
  auto operator=(const CountAllocations&) -> CountAllocations& = delete;

  [[nodiscard]] static auto count() -> std::uint64_t {
    return g_allocations.load(std::memory_order_relaxed);
  }
};
}  // namespace

#if !GLOAM_TSAN_ACTIVE
namespace {
/// The one place an allocation is counted and made. Returns nullptr on failure;
/// the throwing forms turn that into std::bad_alloc.
auto counted_allocate(std::size_t size, std::size_t alignment) noexcept -> void* {
  if (g_counting.load(std::memory_order_relaxed)) {
    g_allocations.store(g_allocations.load(std::memory_order_relaxed) + 1,
                        std::memory_order_relaxed);
  }
  if (alignment <= __STDCPP_DEFAULT_NEW_ALIGNMENT__) {
    return std::malloc(size == 0 ? 1 : size);
  }
  // aligned_alloc wants a size that is a non-zero multiple of the alignment.
  if (size > std::numeric_limits<std::size_t>::max() - alignment) return nullptr;
  const std::size_t rounded = (size + alignment - 1) / alignment * alignment;
  return std::aligned_alloc(alignment, rounded == 0 ? alignment : rounded);
}

auto counted_allocate_or_throw(std::size_t size, std::size_t alignment) -> void* {
  void* memory = counted_allocate(size, alignment);
  if (memory == nullptr) throw std::bad_alloc{};
  return memory;
}

constexpr std::size_t kPlain = __STDCPP_DEFAULT_NEW_ALIGNMENT__;
}  // namespace

auto operator new(std::size_t size) -> void* { return counted_allocate_or_throw(size, kPlain); }
auto operator new[](std::size_t size) -> void* { return counted_allocate_or_throw(size, kPlain); }
auto operator new(std::size_t size, const std::nothrow_t&) noexcept -> void* {
  return counted_allocate(size, kPlain);
}
auto operator new[](std::size_t size, const std::nothrow_t&) noexcept -> void* {
  return counted_allocate(size, kPlain);
}
auto operator new(std::size_t size, std::align_val_t alignment) -> void* {
  return counted_allocate_or_throw(size, static_cast<std::size_t>(alignment));
}
auto operator new[](std::size_t size, std::align_val_t alignment) -> void* {
  return counted_allocate_or_throw(size, static_cast<std::size_t>(alignment));
}
auto operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
    -> void* {
  return counted_allocate(size, static_cast<std::size_t>(alignment));
}
auto operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
    -> void* {
  return counted_allocate(size, static_cast<std::size_t>(alignment));
}

auto operator delete(void* memory) noexcept -> void { std::free(memory); }
auto operator delete[](void* memory) noexcept -> void { std::free(memory); }
auto operator delete(void* memory, std::size_t) noexcept -> void { std::free(memory); }
auto operator delete[](void* memory, std::size_t) noexcept -> void { std::free(memory); }
auto operator delete(void* memory, const std::nothrow_t&) noexcept -> void { std::free(memory); }
auto operator delete[](void* memory, const std::nothrow_t&) noexcept -> void { std::free(memory); }
auto operator delete(void* memory, std::align_val_t) noexcept -> void { std::free(memory); }
auto operator delete[](void* memory, std::align_val_t) noexcept -> void { std::free(memory); }
auto operator delete(void* memory, std::size_t, std::align_val_t) noexcept -> void {
  std::free(memory);
}
auto operator delete[](void* memory, std::size_t, std::align_val_t) noexcept -> void {
  std::free(memory);
}
auto operator delete(void* memory, std::align_val_t, const std::nothrow_t&) noexcept -> void {
  std::free(memory);
}
auto operator delete[](void* memory, std::align_val_t, const std::nothrow_t&) noexcept -> void {
  std::free(memory);
}
#endif

namespace {

using Clips = std::array<sfx::Clip, audio::kSoundIdCount>;

constexpr std::uint32_t kFrames = sfx::kBufferFrames;

/// A fixture holding a real arena, so voices render real samples rather than a
/// stub the mixer might treat differently.
struct Rig {
  std::vector<float> arena;
  Clips clips{};
  std::vector<float> out;
  audio::Ring<> ring;

  Rig() : arena(sfx::kArenaFrames), out(std::size_t{kFrames} * 2U, 0.0F) {
    REQUIRE(sfx::synthesise(0x9105A3ULL, arena,
                            std::span<sfx::Clip, audio::kSoundIdCount>{clips}));
  }

  [[nodiscard]] auto make_mixer() -> mix::Mixer {
    return mix::Mixer{std::span<const float>{arena},
                      std::span<const sfx::Clip, audio::kSoundIdCount>{clips}};
  }

  auto push(audio::SoundId sound, audio::Gain gain, audio::Pan pan,
            std::uint64_t stamp = 0) -> bool {
    audio::Command command{};
    command.sound = sound;
    command.gain = gain;
    command.pan = pan;
    command.stamp = stamp;
    return ring.try_push(command);
  }

  auto render(mix::Mixer& mixer, std::uint64_t now_ns = 0) -> void {
    mixer.render(ring, out, kFrames, now_ns);
  }

  [[nodiscard]] auto peak() const -> float {
    float highest = 0.0F;
    for (const float sample : out) highest = std::max(highest, std::abs(sample));
    return highest;
  }
};

}  // namespace

TEST_CASE("render allocates nothing", "[mix]") {
#if GLOAM_TSAN_ACTIVE
  // Not silently compiled away — see the banner above the counter. TSan owns the
  // global operators on this leg, so the instrument cannot exist here; the
  // non-TSan legs run it.
  SKIP("ThreadSanitizer defines the global operator new, so the counter cannot be installed");
#else
  Rig rig;
  auto mixer = rig.make_mixer();

  // A full ring, so the drain, the steal path and the limiter all run inside the
  // counted region. A quiet render proves much less.
  for (std::size_t i = 0; i < audio::Ring<>::capacity(); ++i) {
    rig.push(audio::SoundId::MonsterFootfall, audio::kGainUnity, audio::kPanCentre, 1000);
  }

  std::uint64_t allocations = 0;
  {
    const CountAllocations counting;
    mixer.render(rig.ring, rig.out, kFrames, 2000);
    allocations = CountAllocations::count();
  }

  CHECK(allocations == 0);
  // The counter must be capable of firing, or the assertion above is a
  // statement about the counter rather than about the mixer.
  // Call the allocation function explicitly: a new-expression's allocation
  // may be elided even when operator new has observable side effects (#57).
  std::uint64_t control = 0;
  {
    const CountAllocations counting;
    auto* probe = ::operator new(sizeof(int));
    control = CountAllocations::count();
    ::operator delete(probe);
  }
  CHECK(control == 1);
#endif
}

// ── #57: is the counter itself falsifiable? ────────────────────────────────
//
// "render allocates nothing" is only as strong as the counter's ability to see an
// allocation. #57 was the counter silently going blind under Clang -O2/-O3: the
// control's `new int{7}` was elided, so the control read 0 and the suite failed —
// the lucky direction. The cases below pin the other half: every allocation shape
// that can reasonably appear in a callback is OBSERVED, through the same replaced
// `operator new` the mixer case uses, and the exact measure-pattern the mixer
// case uses (arm, call, read, disarm) reports an allocation injected beside a
// real `render`.
//
// No CHECK/REQUIRE inside a counted region: Catch2 allocates, and a count taken
// after an assertion would measure the framework. Counts are copied out first.
//
// Every pointer is passed through `escape` before the count is read. Without it
// the *compiler* may delete an unused allocation (the #57 mechanism), and these
// cases would be testing the optimiser instead of the counter.
namespace {
template <typename T>
void escape(T* pointer) noexcept {
  __asm__ volatile("" : : "g"(pointer) : "memory");
}
}  // namespace

TEST_CASE("the allocation counter observes every allocation shape, optimised or not",
          "[mix][counter]") {
#if GLOAM_TSAN_ACTIVE
  SKIP("ThreadSanitizer defines the global operator new, so the counter cannot be installed");
#else
  std::uint64_t scalar = 0;
  std::uint64_t array_form = 0;
  std::uint64_t zero_size = 0;
  std::uint64_t three = 0;
  std::uint64_t via_vector = 0;
  std::uint64_t via_new_expression = 0;
  {
    const CountAllocations counting;
    void* a = ::operator new(sizeof(int));
    escape(a);
    scalar = CountAllocations::count();
    ::operator delete(a);
  }
  {
    const CountAllocations counting;
    void* a = ::operator new[](64);
    escape(a);
    array_form = CountAllocations::count();
    ::operator delete[](a);
  }
  {
    const CountAllocations counting;
    void* a = ::operator new(0);
    escape(a);
    zero_size = CountAllocations::count();
    ::operator delete(a);
  }
  {
    const CountAllocations counting;
    void* a = ::operator new(1);
    void* b = ::operator new(2);
    void* c = ::operator new(3);
    escape(a);
    escape(b);
    escape(c);
    three = CountAllocations::count();
    ::operator delete(a);
    ::operator delete(b);
    ::operator delete(c);
  }
  {
    // std::allocator, i.e. the route a stray std::vector in the callback takes.
    const CountAllocations counting;
    std::vector<float> v(64);
    escape(v.data());
    via_vector = CountAllocations::count();
  }
  {
    // A new-expression whose result escapes cannot be elided, so this is the
    // optimiser-proof spelling of the control that #57 replaced.
    const CountAllocations counting;
    int* p = new int{7};
    escape(p);
    via_new_expression = CountAllocations::count();
    delete p;
  }

  CHECK(scalar == 1);
  CHECK(array_form == 1);
  // Zero-byte requests are still requests; a callback hiding behind `new T[0]`
  // would otherwise be invisible.
  CHECK(zero_size == 1);
  // Exactly three, not "at least one": a counter that saturates at 1 or counts
  // only the first allocation per arming would pass a weaker check.
  CHECK(three == 3);
  CHECK(via_vector == 1);
  CHECK(via_new_expression == 1);
#endif
}

TEST_CASE("the allocation counter observes the nothrow and over-aligned forms too",
          "[mix][counter]") {
#if GLOAM_TSAN_ACTIVE
  SKIP("ThreadSanitizer defines the global operator new, so the counter cannot be installed");
#else
  // The forms the case above does not reach. Each one is a distinct symbol a
  // runtime may define on its own terms — libasan defines all of them without
  // forwarding, and libstdc++'s aligned forms call posix_memalign rather than
  // `operator new` on every leg — so each needs its own observation. A callback
  // reaching for `new (std::nothrow)` or an `alignas(64)` scratch block is no
  // less an allocation for having taken a less common spelling.
  constexpr std::size_t kOver = 64;
  static_assert(kOver > __STDCPP_DEFAULT_NEW_ALIGNMENT__);
  const auto over = std::align_val_t{kOver};

  std::uint64_t nothrow_scalar = 0;
  std::uint64_t nothrow_array = 0;
  std::uint64_t aligned_scalar = 0;
  std::uint64_t aligned_array = 0;
  std::uint64_t aligned_nothrow_scalar = 0;
  std::uint64_t aligned_nothrow_array = 0;
  std::uintptr_t misalignment = 0;
  bool any_null = false;
  {
    const CountAllocations counting;
    void* a = ::operator new(16, std::nothrow);
    escape(a);
    nothrow_scalar = CountAllocations::count();
    any_null = any_null || a == nullptr;
    ::operator delete(a, std::nothrow);
  }
  {
    const CountAllocations counting;
    void* a = ::operator new[](16, std::nothrow);
    escape(a);
    nothrow_array = CountAllocations::count();
    any_null = any_null || a == nullptr;
    ::operator delete[](a, std::nothrow);
  }
  {
    const CountAllocations counting;
    void* a = ::operator new(16, over);
    escape(a);
    aligned_scalar = CountAllocations::count();
    misalignment |= reinterpret_cast<std::uintptr_t>(a) % kOver;
    ::operator delete(a, std::size_t{16}, over);
  }
  {
    const CountAllocations counting;
    void* a = ::operator new[](16, over);
    escape(a);
    aligned_array = CountAllocations::count();
    misalignment |= reinterpret_cast<std::uintptr_t>(a) % kOver;
    ::operator delete[](a, over);
  }
  {
    const CountAllocations counting;
    void* a = ::operator new(16, over, std::nothrow);
    escape(a);
    aligned_nothrow_scalar = CountAllocations::count();
    any_null = any_null || a == nullptr;
    misalignment |= reinterpret_cast<std::uintptr_t>(a) % kOver;
    ::operator delete(a, over, std::nothrow);
  }
  {
    const CountAllocations counting;
    void* a = ::operator new[](16, over, std::nothrow);
    escape(a);
    aligned_nothrow_array = CountAllocations::count();
    any_null = any_null || a == nullptr;
    misalignment |= reinterpret_cast<std::uintptr_t>(a) % kOver;
    ::operator delete[](a, std::size_t{16}, over);
  }

  CHECK(nothrow_scalar == 1);
  CHECK(nothrow_array == 1);
  CHECK(aligned_scalar == 1);
  CHECK(aligned_array == 1);
  CHECK(aligned_nothrow_scalar == 1);
  CHECK(aligned_nothrow_array == 1);
  // The replacement must still honour the contract it took over, or the suite
  // would be counting allocations the rest of the binary cannot safely use.
  CHECK_FALSE(any_null);
  CHECK(misalignment == 0);
#endif
}

TEST_CASE("the allocation counter is armed only inside its scope and re-arming resets it",
          "[mix][counter]") {
#if GLOAM_TSAN_ACTIVE
  SKIP("ThreadSanitizer defines the global operator new, so the counter cannot be installed");
#else
  std::uint64_t before_arming = 0;
  std::uint64_t inside = 0;
  std::uint64_t after_disarm = 0;
  std::uint64_t rearmed = 0;

  // Outside any scope: allocations are not counted (Catch2 would swamp the number
  // otherwise), and the leftover from a previous test must not leak into the next.
  {
    const CountAllocations first;
    void* leftover = ::operator new(16);
    escape(leftover);
    ::operator delete(leftover);
  }
  void* outside = ::operator new(16);
  escape(outside);
  ::operator delete(outside);
  before_arming = CountAllocations::count();  // still the 1 from `first`, not 2

  {
    const CountAllocations counting;
    rearmed = CountAllocations::count();  // reset on arming
    void* p = ::operator new(16);
    escape(p);
    inside = CountAllocations::count();
    ::operator delete(p);
  }
  void* after = ::operator new(16);
  escape(after);
  ::operator delete(after);
  after_disarm = CountAllocations::count();

  CHECK(before_arming == 1);
  CHECK(rearmed == 0);
  CHECK(inside == 1);
  // Disarming must stop the count: a counter that stays live would charge Catch2's
  // own allocations to whatever `render` case runs next.
  CHECK(after_disarm == 1);
#endif
}

TEST_CASE("an allocation injected beside render is seen by the mixer case's own measurement",
          "[mix][counter]") {
#if GLOAM_TSAN_ACTIVE
  SKIP("ThreadSanitizer defines the global operator new, so the counter cannot be installed");
#else
  // Falsifiability of "render allocates nothing": the identical arm/call/read
  // pattern, with the allocation the mixer does not make supplied from outside,
  // must read non-zero. If this stays at zero, `allocations == 0` in the mixer
  // case proves nothing. (The same claim is proven against a mutated mixer, not
  // only a bystander allocation, in gloam-work/agy-proof.md.)
  Rig rig;
  auto mixer = rig.make_mixer();
  for (std::size_t i = 0; i < audio::Ring<>::capacity(); ++i) {
    rig.push(audio::SoundId::MonsterFootfall, audio::kGainUnity, audio::kPanCentre, 1000);
  }

  std::uint64_t render_only = 0;
  std::uint64_t render_then_alloc = 0;
  {
    const CountAllocations counting;
    mixer.render(rig.ring, rig.out, kFrames, 2000);
    render_only = CountAllocations::count();
    void* injected = ::operator new(sizeof(int));
    escape(injected);
    render_then_alloc = CountAllocations::count();
    ::operator delete(injected);
  }

  for (std::size_t i = 0; i < audio::Ring<>::capacity(); ++i) {
    rig.push(audio::SoundId::HuntingSting, audio::kGainUnity, audio::kPanCentre, 3000);
  }
  std::uint64_t alloc_then_render = 0;
  {
    const CountAllocations counting;
    void* injected = ::operator new(sizeof(int));
    escape(injected);
    mixer.render(rig.ring, rig.out, kFrames, 4000);
    // render must not reset or mask a count already taken.
    alloc_then_render = CountAllocations::count();
    ::operator delete(injected);
  }

  CHECK(render_only == 0);
  CHECK(render_then_alloc == 1);
  CHECK(alloc_then_render == 1);
#endif
}

TEST_CASE("render allocates nothing on any path, not only the full-ring one", "[mix][counter]") {
#if GLOAM_TSAN_ACTIVE
  SKIP("ThreadSanitizer defines the global operator new, so the counter cannot be installed");
#else
  // The original case counts one render of a full ring of one sound. The claim in
  // §9.2 is about the callback, so cover the other paths it can take: an empty
  // ring, a zero-frame buffer, a burst past kVoiceSlots (the steal and refuse
  // paths), every SoundId, extreme pan/gain, and enough renders for voices to run
  // out and be reused.
  Rig rig;
  auto mixer = rig.make_mixer();

  std::uint64_t empty = 0;
  std::uint64_t zero_frames = 0;
  std::uint64_t burst = 0;
  std::uint64_t tail = 0;
  std::uint64_t extremes = 0;

  {
    const CountAllocations counting;
    mixer.render(rig.ring, rig.out, kFrames, 1);
    empty = CountAllocations::count();
  }
  {
    rig.push(audio::SoundId::PartyFootfall, audio::kGainUnity, audio::kPanCentre, 1);
    const CountAllocations counting;
    mixer.render(rig.ring, rig.out, 0, 2);
    zero_frames = CountAllocations::count();
  }
  {
    // Seventeen-plus voices of rising loudness: steals; then equal loudness: refuses.
    for (std::size_t i = 0; i < mix::kVoiceSlots + 8; ++i) {
      const auto id = static_cast<audio::SoundId>(1 + (i % (audio::kSoundIdCount - 1)));
      rig.push(id, static_cast<audio::Gain>(audio::kGainUnity / 4 + i), audio::kPanCentre, 5);
    }
    const CountAllocations counting;
    mixer.render(rig.ring, rig.out, kFrames, 6);
    burst = CountAllocations::count();
  }
  {
    // Run the voices to completion and past it, so slots are freed and reused.
    const CountAllocations counting;
    for (int i = 0; i < 64; ++i) mixer.render(rig.ring, rig.out, kFrames, 10 + i);
    tail = CountAllocations::count();
  }
  {
    rig.push(audio::SoundId::HuntingSting, audio::kGainUnity, audio::kPanFull, 7);
    rig.push(audio::SoundId::HuntingSting, audio::kGainUnity, -audio::kPanFull, 7);
    rig.push(audio::SoundId::MonsterFootfall, audio::kGainSilent, audio::kPanCentre, 7);
    const CountAllocations counting;
    mixer.render(rig.ring, rig.out, kFrames, std::uint64_t{0} - 1U);
    extremes = CountAllocations::count();
  }

  CHECK(empty == 0);
  CHECK(zero_frames == 0);
  CHECK(burst == 0);
  CHECK(tail == 0);
  CHECK(extremes == 0);
#endif
}

TEST_CASE("a full ring drains in one render", "[mix]") {
  Rig rig;
  auto mixer = rig.make_mixer();
  for (std::size_t i = 0; i < audio::Ring<>::capacity(); ++i) {
    rig.push(audio::SoundId::PartyFootfall, audio::kGainUnity / 4, audio::kPanCentre);
  }
  REQUIRE(rig.ring.size() == audio::Ring<>::capacity());

  rig.render(mixer);

  // A render that left commands behind would grow the ring's backlog every
  // buffer until it dropped — a slow failure that only appears under load.
  CHECK(rig.ring.empty());
  CHECK(rig.ring.dropped() == 0);
}

TEST_CASE("the drain is bounded, so a busy producer cannot hold the callback open", "[mix]") {
  Rig rig;
  auto mixer = rig.make_mixer();

  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> pushed{0};
  std::thread producer{[&] {
    while (!stop.load(std::memory_order_relaxed)) {
      audio::Command command{};
      command.sound = audio::SoundId::PartyFootfall;
      command.gain = audio::kGainUnity;
      if (rig.ring.try_push(command)) pushed.fetch_add(1, std::memory_order_relaxed);
    }
  }};

  // WAIT FOR THE PRODUCER TO ACTUALLY BE PRODUCING before starting the renders.
  //
  // Without this the case is a race it can lose: on the Clang ASan leg the main
  // thread finished all 100 renders before the producer was ever scheduled, and
  // `pushed` was still 0 — so the assertion below failed while the code under
  // test was perfectly fine. Worse than the red is what it means: a run where
  // the producer never ran is a run where the contention this case exists to
  // create never happened, and the case would have passed vacuously if the
  // assertion had been written any weaker.
  //
  // The spin is bounded by the ctest TIMEOUT on this target, which is the same
  // backstop the hang below relies on.
  while (pushed.load(std::memory_order_relaxed) == 0) {
  }

  // If `render` looped `while (try_pop(...))` this would never return, and the
  // ctest TIMEOUT on this target is what turns that into a red rather than a
  // wedged CI runner. The bound is the ring's capacity — see voice_mixer.cpp.
  for (int i = 0; i < 100; ++i) rig.render(mixer);

  stop.store(true, std::memory_order_relaxed);
  producer.join();
  CHECK(pushed.load() > 0);
}

TEST_CASE("more commands than slots steals the quietest voice and counts it", "[mix]") {
  Rig rig;
  auto mixer = rig.make_mixer();

  // Sixteen quiet voices fill every slot.
  for (std::size_t i = 0; i < mix::kVoiceSlots; ++i) {
    rig.push(audio::SoundId::HuntingSting, 64, audio::kPanCentre);
  }
  rig.render(mixer);
  REQUIRE(mixer.started() == mix::kVoiceSlots);
  REQUIRE(mixer.stolen() == 0);
  REQUIRE(mixer.peak_voices() == mix::kVoiceSlots);

  // A loud one arrives with nowhere to go.
  rig.push(audio::SoundId::HuntingSting, audio::kGainUnity, audio::kPanCentre);
  rig.render(mixer);

  CHECK(mixer.stolen() == 1);
  CHECK(mixer.started() == mix::kVoiceSlots + 1);
  // AND THE RING DID NOT DROP IT. Two different budgets: a stolen voice was
  // delivered and displaced something, a dropped command never arrived. Reading
  // them as one would make budget::kMaxDroppedVoiceCommands == 0 unreachable the
  // first time seventeen things sounded at once.
  CHECK(rig.ring.dropped() == 0);
}

TEST_CASE("a voice quieter than everything sounding is dropped, not swapped in", "[mix]") {
  Rig rig;
  auto mixer = rig.make_mixer();
  for (std::size_t i = 0; i < mix::kVoiceSlots; ++i) {
    rig.push(audio::SoundId::HuntingSting, audio::kGainUnity, audio::kPanCentre);
  }
  rig.render(mixer);
  REQUIRE(mixer.started() == mix::kVoiceSlots);

  rig.push(audio::SoundId::PartyFootfall, 1, audio::kPanCentre);
  rig.render(mixer);

  // The whole point of stealing the quietest is that loud things survive. A
  // policy that always displaced something would let a distant footfall silence
  // a sting one cell away — handing the player less information than the
  // monster has, which `gain_from_loudness` refuses by name.
  CHECK(mixer.stolen() == 0);
  CHECK(mixer.started() == mix::kVoiceSlots);
  // And it is COUNTED rather than discarded quietly. The ring is not the place
  // that notices: the command arrived, it simply never sounded.
  CHECK(mixer.refused() == 1);
  CHECK(rig.ring.dropped() == 0);
}

TEST_CASE("the stolen voice is the quietest, not the oldest", "[mix]") {
  Rig rig;
  auto mixer = rig.make_mixer();

  // One very quiet voice FIRST, then fifteen loud ones. A FIFO steal would take
  // a loud one; the correct answer takes the quiet one that arrived first.
  rig.push(audio::SoundId::HuntingSting, 8, audio::kPanCentre);
  for (std::size_t i = 1; i < mix::kVoiceSlots; ++i) {
    rig.push(audio::SoundId::HuntingSting, audio::kGainUnity, audio::kPanCentre);
  }
  rig.render(mixer);
  REQUIRE(mixer.started() == mix::kVoiceSlots);

  const float loud_peak = rig.peak();

  rig.push(audio::SoundId::HuntingSting, audio::kGainUnity, audio::kPanCentre);
  rig.render(mixer);
  REQUIRE(mixer.stolen() == 1);

  // Still fifteen loud voices plus the newcomer: the level did not collapse,
  // which it would have if a loud voice had been evicted.
  CHECK(rig.peak() >= loud_peak * 0.5F);
}

TEST_CASE("gain converts exactly at every boundary", "[mix]") {
  // `audio.hpp` justifies kGainUnity being 1024 by claiming this conversion is
  // exact in IEEE-754. `==` rather than a tolerance, because a tolerance would
  // pass on an implementation that made the claim false.
  CHECK(mix::gain_to_float(audio::kGainSilent) == 0.0F);
  CHECK(mix::gain_to_float(audio::kGainUnity) == 1.0F);
  CHECK(mix::gain_to_float(512) == 0.5F);
  CHECK(mix::gain_to_float(256) == 0.25F);
  CHECK(mix::gain_to_float(1) == 1.0F / 1024.0F);
  // Negative is SILENCE, not inversion. This line asserted -1.0 until the clamp
  // landed; see "an out-of-range gain clamps, like an out-of-range pan" for why
  // a phase-inverted voice was worth refusing rather than rendering.
  CHECK(mix::gain_to_float(-1024) == 0.0F);

  // Every representable gain round-trips through a multiply by 1024 without
  // drift, which is the property that makes the divisor's power-of-two-ness
  // load-bearing rather than decorative.
  for (audio::Gain g = 0; g < 1024; ++g) {
    CHECK(mix::gain_to_float(g) * 1024.0F == static_cast<float>(g));
  }
}

TEST_CASE("pan maps the scale to a pair summing to exactly one", "[mix]") {
  CHECK(mix::pan_to_lr(audio::kPanCentre).left == 0.5F);
  CHECK(mix::pan_to_lr(audio::kPanCentre).right == 0.5F);
  CHECK(mix::pan_to_lr(-audio::kPanFull).left == 1.0F);
  CHECK(mix::pan_to_lr(-audio::kPanFull).right == 0.0F);
  CHECK(mix::pan_to_lr(audio::kPanFull).left == 0.0F);
  CHECK(mix::pan_to_lr(audio::kPanFull).right == 1.0F);

  // All 513 legal pans, exactly. A pan law that lost energy in the middle would
  // make a centred sound quieter than a hard-panned one, which is a bug the ear
  // notices long before a test does — unless the test is this one.
  for (audio::Pan p = -audio::kPanFull; p <= audio::kPanFull; ++p) {
    const auto lr = mix::pan_to_lr(p);
    CHECK(lr.left + lr.right == 1.0F);
    CHECK(lr.left >= 0.0F);
    CHECK(lr.right >= 0.0F);
  }
}

TEST_CASE("an out-of-range pan clamps rather than steering past hard left", "[mix]") {
  // `pan_from_bearing` cannot produce these. A Command is a POD read off a ring
  // and Pan is a plain int16_t, so the mixer trusts the wire, not the producer.
  for (const audio::Pan p : {audio::Pan{-30'000}, audio::Pan{32'767}, audio::Pan{-32'768},
                             audio::Pan{257}, audio::Pan{-257}}) {
    const auto lr = mix::pan_to_lr(p);
    CHECK(lr.left >= 0.0F);
    CHECK(lr.right >= 0.0F);
    CHECK(lr.left <= 1.0F);
    CHECK(lr.right <= 1.0F);
    CHECK(lr.left + lr.right == 1.0F);
  }
}

TEST_CASE("a hostile gain does not invert or explode the mix", "[mix]") {
  Rig rig;
  auto mixer = rig.make_mixer();

  rig.push(audio::SoundId::HuntingSting, 32'767, audio::kPanCentre);
  rig.push(audio::SoundId::PartyFootfall, -32'768, audio::kPanCentre);
  rig.render(mixer);

  // Whatever the arithmetic does with a 32x gain, the buffer handed to the
  // driver stays inside the range a DAC can accept.
  for (const float sample : rig.out) {
    CHECK(sample == sample);
    CHECK(sample >= -1.0F);
    CHECK(sample <= 1.0F);
  }

  // AND IT NO LONGER GETS AS FAR AS THE LIMITER. This asserted `clipped() > 0`
  // while `gain_to_float` was unclamped: 32,767 is 32x unity, so a single voice
  // sat on the limiter for most of the buffer. Now the gain is clamped at the
  // conversion and the limiter never sees it — two voices at unity and silence
  // respectively cannot reach full scale. Containing a hostile value earlier is
  // strictly better than containing it later, so the assertion inverts.
  CHECK(mixer.clipped() == 0);
  CHECK(mixer.started() == 2);
}

TEST_CASE("a non-finite arena sample is contained and counted, not passed on", "[mix]") {
  // The limiter is two float comparisons, and NaN compares false against both.
  // Before this case existed, an arena of NaN produced 512 of 512 non-finite
  // output samples with `clipped()` reading zero — the one value class that
  // walked past the thing voice_mixer.hpp calls "what stands between the mix and
  // the DAC".
  //
  // Not reachable from sfx::synthesise, which is fuzzed over millions of seeds
  // and never produces one. Reachable from `Mixer`, which takes the arena as an
  // unvalidated span from whoever constructs it.
  Clips clips{};
  clips[static_cast<std::size_t>(audio::SoundId::PartyFootfall)] = sfx::Clip{0, 64};

  const std::vector<float> poison(64, std::numeric_limits<float>::quiet_NaN());
  mix::Mixer mixer{std::span<const float>{poison},
                   std::span<const sfx::Clip, audio::kSoundIdCount>{clips}};

  audio::Ring<> ring;
  audio::Command command{};
  command.sound = audio::SoundId::PartyFootfall;
  command.gain = audio::kGainUnity;
  REQUIRE(ring.try_push(command));

  std::vector<float> out(std::size_t{kFrames} * 2U, 0.0F);
  mixer.render(ring, out, kFrames, 0);

  for (const float sample : out) {
    CHECK(sample == sample);
    CHECK(sample >= -1.0F);
    CHECK(sample <= 1.0F);
  }
  CHECK(mixer.clipped() > 0);

  // The other non-finite values were already contained; pinned so a future
  // rewrite of the limiter has to keep all of them.
  for (const float poison_value : {std::numeric_limits<float>::infinity(),
                                   -std::numeric_limits<float>::infinity(), 1e30F, -1e30F}) {
    const std::vector<float> filled(64, poison_value);
    mix::Mixer other{std::span<const float>{filled},
                     std::span<const sfx::Clip, audio::kSoundIdCount>{clips}};
    audio::Ring<> other_ring;
    REQUIRE(other_ring.try_push(command));
    std::vector<float> other_out(std::size_t{kFrames} * 2U, 0.0F);
    other.render(other_ring, other_out, kFrames, 0);
    for (const float sample : other_out) {
      CHECK(sample == sample);
      CHECK(sample >= -1.0F);
      CHECK(sample <= 1.0F);
    }
  }
}

TEST_CASE("an out-of-range gain clamps, like an out-of-range pan", "[mix]") {
  // The counterpart the suite was missing. `pan_to_lr` clamped on the stated
  // principle that a Command is a POD read off a ring; `gain_to_float` did not,
  // three lines away, and 32,767 is 32x unity.
  CHECK(mix::gain_to_float(32'767) == 1.0F);
  CHECK(mix::gain_to_float(audio::kGainUnity + 1) == 1.0F);
  CHECK(mix::gain_to_float(-1) == 0.0F);
  CHECK(mix::gain_to_float(-32'768) == 0.0F);

  // Legal values are untouched, so this is a defence and not a retune.
  for (audio::Gain g = audio::kGainSilent; g <= audio::kGainUnity; ++g) {
    CHECK(mix::gain_to_float(g) == static_cast<float>(g) * (1.0F / 1024.0F));
  }

  // And the consequence that made it worth fixing rather than documenting: the
  // steal policy ranks voices by level, so a negative gain — a full-volume
  // phase inversion — used to sort BELOW a gain of 1 and be stolen first. A
  // clamped level cannot be negative, so the ranking is monotone in loudness.
  CHECK(mix::gain_to_float(-1024) <= mix::gain_to_float(1));
}

TEST_CASE("a command naming a sound outside the table starts nothing", "[mix]") {
  Rig rig;
  auto mixer = rig.make_mixer();

  audio::Command command{};
  command.sound = static_cast<audio::SoundId>(200);  // off the end of the clip table
  command.gain = audio::kGainUnity;
  REQUIRE(rig.ring.try_push(command));

  rig.render(mixer);

  // An out-of-range enum read off a ring is exactly the shape of bug that turned
  // into a stack buffer overflow in the G-9b pathfinder slice. Here it must be a
  // silent no-op, not an index.
  CHECK(mixer.started() == 0);
  CHECK(rig.peak() == 0.0F);
}

TEST_CASE("SoundId::None is silent rather than an index into the arena", "[mix]") {
  Rig rig;
  auto mixer = rig.make_mixer();
  rig.push(audio::SoundId::None, audio::kGainUnity, audio::kPanCentre);
  rig.render(mixer);

  CHECK(mixer.started() == 0);
  CHECK(rig.peak() == 0.0F);
}

TEST_CASE("sixteen unity voices clip to the limit rather than leaving the buffer", "[mix]") {
  Rig rig;
  auto mixer = rig.make_mixer();
  for (std::size_t i = 0; i < mix::kVoiceSlots; ++i) {
    rig.push(audio::SoundId::MonsterFootfall, audio::kGainUnity, audio::kPanCentre);
  }
  rig.render(mixer);

  REQUIRE(mixer.started() == mix::kVoiceSlots);
  for (const float sample : rig.out) {
    CHECK(sample >= -1.0F);
    CHECK(sample <= 1.0F);
  }
  // Sixteen voices peaking near 0.58 each sum well past 1.0, so the limiter must
  // have engaged. If it did not, the arithmetic above is not doing what this
  // case claims to be testing.
  CHECK(mixer.clipped() > 0);
}

TEST_CASE("an undersized output buffer is refused rather than overrun", "[mix]") {
  Rig rig;
  auto mixer = rig.make_mixer();
  rig.push(audio::SoundId::PartyFootfall, audio::kGainUnity, audio::kPanCentre);

  std::vector<float> tiny(4, 99.0F);
  mixer.render(rig.ring, tiny, kFrames, 0);

  // Nothing written, nothing started. A driver that asked for more frames than
  // the buffer it handed over is a driver bug; writing past the end would make
  // it GLOAM's crash.
  for (const float sample : tiny) CHECK(sample == 99.0F);
  CHECK(mixer.started() == 0);
}

TEST_CASE("the reported latency is exactly now minus the stamp", "[mix]") {
  Rig rig;
  auto mixer = rig.make_mixer();

  // The whole reason `render` takes `now_ns` as a parameter: over a synthetic
  // clock this is an exact equality, not a bound. A mixer that read its own
  // clock could only ever be tested with a tolerance.
  constexpr std::uint64_t kStamp = 1'000'000;
  constexpr std::uint64_t kNow = 6'333'000;
  rig.push(audio::SoundId::PartyFootfall, audio::kGainUnity, audio::kPanCentre, kStamp);
  rig.render(mixer, kNow);

  CHECK(mixer.worst_latency_ns() == kNow - kStamp);

  // And it is a WORST, not a last: a later, faster voice must not erase it.
  rig.push(audio::SoundId::PartyFootfall, audio::kGainUnity, audio::kPanCentre, kNow);
  rig.render(mixer, kNow + 1000);
  CHECK(mixer.worst_latency_ns() == kNow - kStamp);
}

TEST_CASE("a zero stamp is not a fifty-year latency", "[mix]") {
  Rig rig;
  auto mixer = rig.make_mixer();

  // `Command{}` carries stamp 0, and so does anything a RecordingSink built.
  // Treating that as a timestamp would report the entire uptime of the machine
  // as the latency of one footstep — and it would do it the first time anything
  // other than DeviceSink pushed a command.
  rig.push(audio::SoundId::PartyFootfall, audio::kGainUnity, audio::kPanCentre, 0);
  rig.render(mixer, 999'999'999'999ULL);

  CHECK(mixer.started() == 1);
  CHECK(mixer.worst_latency_ns() == 0);
}

TEST_CASE("a producer thread and the render loop agree on every command", "[mix]") {
  Rig rig;
  auto mixer = rig.make_mixer();

  // THE TSAN-VISIBLE CASE. Everything else here runs on one thread, so this is
  // the only place the sanitizer can audit the ring's release/acquire pairing
  // against the mixer as its consumer — the counterpart of test/15voicering/'s
  // two threaded cases, one layer up.
  constexpr std::uint64_t kTotal = 4'000;
  std::atomic<std::uint64_t> accepted{0};
  std::atomic<bool> done{false};

  std::thread producer{[&] {
    for (std::uint64_t i = 0; i < kTotal; ++i) {
      audio::Command command{};
      command.sound = audio::SoundId::PartyFootfall;
      command.gain = audio::kGainUnity;
      command.stamp = i + 1;
      if (rig.ring.try_push(command)) accepted.fetch_add(1, std::memory_order_relaxed);
    }
    done.store(true, std::memory_order_relaxed);
  }};

  while (!done.load(std::memory_order_relaxed) || !rig.ring.empty()) {
    rig.render(mixer, kTotal + 1'000'000);
  }
  producer.join();
  rig.render(mixer, kTotal + 1'000'000);

  // EVERY COMMAND IS ACCOUNTED FOR, in one of exactly three ways: it was refused
  // by the ring (full), it started a voice, or it was refused by the mixer
  // (every slot busy with something at least as loud).
  //
  // This assertion is why `Mixer::refused` exists. It was first written as
  // `started == accepted`, and it failed with the left side pinned at 16 — the
  // voice-slot count — against a right side in the low hundreds: every command
  // here carries the same gain, so after the first sixteen filled the slots the
  // rest were dropped on the floor and counted nowhere. That is the
  // footstep-nobody-hears failure this suite is supposed to be looking for, and
  // the mixer had it.
  //
  // Only the 16 is deterministic. How many commands the producer gets through
  // before the consumer drains is a race — measured 192 to 394 accepted across
  // sixteen runs — which is why the assertions below are accounting identities
  // rather than counts.
  CHECK(accepted.load() + rig.ring.dropped() == kTotal);
  CHECK(mixer.started() + mixer.refused() == accepted.load());
  // Sixteen slots, all filled by equally loud voices: nothing can displace
  // anything, so every command past the first sixteen is refused rather than
  // stolen from.
  CHECK(mixer.stolen() == 0);
  CHECK(mixer.refused() > 0);
}

TEST_CASE("one command at unity and centre renders the clip into both channels", "[mix]") {
  // Happy path last, and the least interesting case in the file.
  Rig rig;
  auto mixer = rig.make_mixer();
  rig.push(audio::SoundId::MonsterFootfall, audio::kGainUnity, audio::kPanCentre);
  rig.render(mixer);

  CHECK(mixer.started() == 1);
  CHECK(mixer.clipped() == 0);

  float left = 0.0F;
  float right = 0.0F;
  for (std::uint32_t i = 0; i < kFrames; ++i) {
    left = std::max(left, std::abs(rig.out[std::size_t{i} * 2U]));
    right = std::max(right, std::abs(rig.out[(std::size_t{i} * 2U) + 1U]));
  }
  CHECK(left > 0.0F);
  CHECK(left == right);  // centre is exactly centre
}
