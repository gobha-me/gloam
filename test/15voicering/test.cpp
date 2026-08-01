/// SPEC §9.2 — the SPSC ring, which is "the whole interface".
///
/// The failure matrix, in the order the failures matter:
///
///   1. A full ring must DROP AND COUNT. Blocking is the one thing §9.2 forbids
///      outright, and a ring that blocks would deadlock a tick rather than lose
///      a footstep.
///   2. It must drop the NEWEST, never overwrite the oldest, because an
///      overwrite can rewrite a slot the consumer is part-way through reading —
///      turning a lost footstep into a corrupt one.
///   3. Index wrap must be right, which is untestable through the public API
///      because you cannot drive a 64-bit counter to its limit. That is why the
///      arithmetic is free functions.
///   4. The memory ordering must be right, which NO SINGLE-THREADED TEST CAN
///      SEE. The concurrent case at the bottom is the only thing in this repo
///      that gives the TSan job something to audit — weakening the release store
///      in `try_push` passes every other assertion in this file.
///
/// The happy path is the first case only because a ring you cannot push to is
/// not worth writing eleven adversarial tests about.

#include <atomic>
#include <thread>
#include <vector>

#include <catch2/catch_all.hpp>

#include "gloam/audio.hpp"

using namespace gloam::audio;

namespace {

[[nodiscard]] auto voice(std::uint32_t tick, Gain gain = kGainUnity) -> Command {
  Command c{};
  c.tick = tick;
  c.sound = SoundId::PartyFootfall;
  c.gain = gain;
  c.pan = kPanCentre;
  return c;
}

}  // namespace

TEST_CASE("a command survives a round trip intact", "[ring]") {
  Ring<4> ring;
  const auto sent = voice(7, 512);
  REQUIRE(ring.try_push(sent));

  Command got{};
  REQUIRE(ring.try_pop(got));
  CHECK(got == sent);
  CHECK(ring.empty());
}

// ── the full ring ───────────────────────────────────────────────────────────

TEST_CASE("a full ring refuses the newest command and counts it", "[ring][drop]") {
  Ring<4> ring;
  for (std::uint32_t i = 0; i < 4; ++i) REQUIRE(ring.try_push(voice(i)));

  CHECK(ring.size() == 4);
  CHECK(ring.dropped() == 0);

  // The fifth. §9.2: "a full ring drops the command and increments a counter.
  // It never blocks." If this ever hangs, that is the failure — hence the
  // TIMEOUT on this test in CMakeLists.txt.
  CHECK_FALSE(ring.try_push(voice(99)));
  CHECK(ring.dropped() == 1);
  CHECK(ring.size() == 4);  // unchanged: nothing was displaced
}

TEST_CASE("a full ring keeps the OLDEST commands, not the newest", "[ring][drop]") {
  // THE OVERWRITE BUG, WHICH LOOKS LIKE A FEATURE.
  //
  // "Keep the most recent audio" sounds reasonable and is wrong twice over: it
  // rewrites a slot the consumer may be mid-read on, and it makes `dropped()`
  // report zero while commands vanish. §9.2 says drop, and drop means the
  // arrival is refused — not that an earlier one is evicted.
  Ring<4> ring;
  for (std::uint32_t i = 0; i < 4; ++i) REQUIRE(ring.try_push(voice(i)));
  for (std::uint32_t i = 100; i < 110; ++i) CHECK_FALSE(ring.try_push(voice(i)));

  CHECK(ring.dropped() == 10);

  Command got{};
  for (std::uint32_t i = 0; i < 4; ++i) {
    REQUIRE(ring.try_pop(got));
    CHECK(got.tick == i);  // the original four, in order, untouched
  }
  CHECK_FALSE(ring.try_pop(got));
}

TEST_CASE("draining a full ring makes it writable again", "[ring][drop]") {
  Ring<4> ring;
  for (std::uint32_t i = 0; i < 4; ++i) REQUIRE(ring.try_push(voice(i)));
  CHECK_FALSE(ring.try_push(voice(4)));

  Command got{};
  REQUIRE(ring.try_pop(got));
  CHECK(ring.try_push(voice(4)));  // one slot freed, one accepted

  // The drop is permanent; freeing a slot does not un-drop it.
  CHECK(ring.dropped() == 1);
}

// ── empty, and wrap ─────────────────────────────────────────────────────────

TEST_CASE("popping an empty ring reports empty and does not touch the output", "[ring]") {
  // A phantom command from an uninitialised slot would be a footstep the player
  // hears in an empty corridor.
  Ring<4> ring;
  auto sentinel = voice(4242, 77);
  Command out = sentinel;
  CHECK_FALSE(ring.try_pop(out));
  CHECK(out == sentinel);

  // And still empty after a fill/drain cycle, which is where an off-by-one
  // between head and tail shows up.
  for (std::uint32_t i = 0; i < 4; ++i) REQUIRE(ring.try_push(voice(i)));
  Command got{};
  for (std::uint32_t i = 0; i < 4; ++i) REQUIRE(ring.try_pop(got));
  CHECK_FALSE(ring.try_pop(out));
  CHECK(out == sentinel);
}

TEST_CASE("FIFO order holds across many laps of the buffer", "[ring]") {
  // The classic index bug survives exactly one lap. Four laps of a 4-slot ring
  // with a one-in-one-out cadence walks every (head, tail) alignment there is.
  Ring<4> ring;
  Command got{};
  for (std::uint32_t i = 0; i < 64; ++i) {
    REQUIRE(ring.try_push(voice(i)));
    REQUIRE(ring.try_pop(got));
    CHECK(got.tick == i);
  }
  CHECK(ring.dropped() == 0);
  CHECK(ring.pushed() == 64);
}

TEST_CASE("interleaved bursts preserve order across the wrap point", "[ring]") {
  Ring<8> ring;
  std::uint32_t next_push = 0;
  std::uint32_t next_pop = 0;
  Command got{};

  for (int round = 0; round < 20; ++round) {
    for (int i = 0; i < 5; ++i) {
      if (ring.try_push(voice(next_push))) ++next_push;
    }
    for (int i = 0; i < 3; ++i) {
      if (ring.try_pop(got)) {
        CHECK(got.tick == next_pop);
        ++next_pop;
      }
    }
  }
  while (ring.try_pop(got)) {
    CHECK(got.tick == next_pop);
    ++next_pop;
  }
  CHECK(next_pop == next_push);
}

// ── the arithmetic, at the boundary the API cannot reach ────────────────────

TEST_CASE("ring_size is correct across a 64-bit index wrap", "[ring][arithmetic]") {
  // THE REASON THIS ARITHMETIC IS A FREE FUNCTION. Indices are monotonic and
  // masked only at slot access; unsigned overflow is well defined, so a ring
  // that ran long enough for `tail` to wrap past UINT64_MAX while `head` had not
  // must still report the right occupancy. You cannot push 2^64 commands to find
  // that out.
  CHECK(ring_size(0, 0) == 0);
  CHECK(ring_size(0, 5) == 5);
  CHECK(ring_size(UINT64_MAX - 2, UINT64_MAX) == 2);
  CHECK(ring_size(UINT64_MAX, 0) == 1);          // tail wrapped, head has not
  CHECK(ring_size(UINT64_MAX - 1, 2) == 4);      // wrapped by four
}

TEST_CASE("ring_full agrees with ring_size at and around capacity", "[ring][arithmetic]") {
  CHECK_FALSE(ring_full(0, 0, 4));
  CHECK_FALSE(ring_full(0, 3, 4));
  CHECK(ring_full(0, 4, 4));
  CHECK(ring_full(0, 5, 4));  // over-full is still full, never "empty again"
  CHECK(ring_full(UINT64_MAX - 1, 2, 4));
}

TEST_CASE("the counters saturate rather than wrapping", "[ring][arithmetic]") {
  // A WRAPPED COUNTER READS AS A PASSING BUDGET, which is the one failure mode
  // a budget instrument may not have. `emit::ByteSink::total` carries the same
  // reasoning for the same reason.
  CHECK(saturating_add(0, 1) == 1);
  CHECK(saturating_add(UINT64_MAX, 0) == UINT64_MAX);
  CHECK(saturating_add(UINT64_MAX, 1) == UINT64_MAX);
  CHECK(saturating_add(UINT64_MAX - 1, 5) == UINT64_MAX);
  CHECK(saturating_add(UINT64_MAX, UINT64_MAX) == UINT64_MAX);
}

TEST_CASE("counters never reset themselves", "[ring]") {
  // A measurement window that resets implicitly always passes. `reset_counters`
  // is the only way to start one, and draining is not it.
  Ring<2> ring;
  REQUIRE(ring.try_push(voice(0)));
  REQUIRE(ring.try_push(voice(1)));
  CHECK_FALSE(ring.try_push(voice(2)));

  Command got{};
  while (ring.try_pop(got)) {}
  CHECK(ring.empty());
  CHECK(ring.dropped() == 1);  // survives the drain
  CHECK(ring.pushed() == 2);

  ring.reset_counters();
  CHECK(ring.dropped() == 0);
  CHECK(ring.pushed() == 0);
}

TEST_CASE("capacity is a power of two and fixed at compile time", "[ring]") {
  CHECK(Ring<4>::capacity() == 4);
  CHECK(Ring<>::capacity() == kVoiceRingCapacity);
  CHECK((kVoiceRingCapacity & (kVoiceRingCapacity - 1)) == 0);
}

// ── the concurrent case: the only thing TSan can audit ──────────────────────

TEST_CASE("a producer thread and a consumer thread agree on every payload",
          "[ring][concurrency]") {
  // THIS IS THE CASE THE MEMORY ORDERING EXISTS FOR, and the only one that can
  // fail because of it. Under the TSan job, weakening `try_push`'s release store
  // or `try_pop`'s acquire load turns this into a reported data race; under the
  // others it is a correctness check on the payloads.
  //
  // A deliberately small ring so the producer genuinely runs into a full buffer
  // and the drop path is exercised concurrently rather than only in isolation.
  constexpr std::uint32_t kCommands = 200'000;
  Ring<16> ring;

  std::atomic<bool> producer_done{false};
  std::vector<std::uint32_t> received;
  received.reserve(kCommands);

  std::thread producer([&] {
    for (std::uint32_t i = 0; i < kCommands; ++i) {
      // Spin until accepted: this test asserts ORDER and INTEGRITY, so it must
      // not lose commands. The drop path is asserted single-threaded above; here
      // the retry is what keeps the sequence complete and checkable.
      while (!ring.try_push(voice(i, static_cast<Gain>(i % kGainUnity)))) {
        std::this_thread::yield();
      }
    }
    producer_done.store(true, std::memory_order_release);
  });

  std::thread consumer([&] {
    Command got{};
    for (;;) {
      if (ring.try_pop(got)) {
        received.push_back(got.tick);
        // The payload must match the index — a torn read caused by a missing
        // acquire would show up as a gain that does not belong to its tick.
        REQUIRE(got.gain == static_cast<Gain>(got.tick % kGainUnity));
        REQUIRE(got.sound == SoundId::PartyFootfall);
        continue;
      }
      if (producer_done.load(std::memory_order_acquire) && ring.empty()) break;
      std::this_thread::yield();
    }
  });

  producer.join();
  consumer.join();

  REQUIRE(received.size() == kCommands);
  for (std::uint32_t i = 0; i < kCommands; ++i) CHECK(received[i] == i);
  CHECK(ring.pushed() == kCommands);
}

TEST_CASE("a slow consumer loses commands but never the ordering of the rest",
          "[ring][concurrency]") {
  // The realistic degradation: the audio thread is late, the ring fills, and the
  // simulation carries on. What must survive is that the commands which DID get
  // through arrive in order and intact — a ring that dropped by corrupting would
  // pass the count and fail here.
  constexpr std::uint32_t kCommands = 50'000;
  Ring<8> ring;
  std::atomic<bool> producer_done{false};
  std::vector<std::uint32_t> received;

  std::thread producer([&] {
    for (std::uint32_t i = 0; i < kCommands; ++i) static_cast<void>(ring.try_push(voice(i)));
    producer_done.store(true, std::memory_order_release);
  });

  std::thread consumer([&] {
    Command got{};
    for (;;) {
      if (ring.try_pop(got)) {
        received.push_back(got.tick);
        continue;
      }
      if (producer_done.load(std::memory_order_acquire) && ring.empty()) break;
      std::this_thread::yield();
    }
  });

  producer.join();
  consumer.join();

  CHECK(ring.pushed() + ring.dropped() == kCommands);  // every command accounted for
  CHECK(received.size() == ring.pushed());
  // Strictly increasing: gaps are expected, reordering and duplication are not.
  for (std::size_t i = 1; i < received.size(); ++i) CHECK(received[i] > received[i - 1]);
}
