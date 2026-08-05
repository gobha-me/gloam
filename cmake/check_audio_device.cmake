# SPEC §9.2 — "Device loss is a degradation, not a crash: sink goes silent, game
# continues, message line reports it."
#
# THIS IS THE ONE ROW OF §9.2 THAT A MACHINE WITH NO SOUND CARD CAN VERIFY FOR
# REAL, and the reason this gate exists rather than a mock. Neither this
# project's dev box nor a GitHub runner has /dev/snd, so `gloam --audio` takes
# the no-device branch every time it runs here — which makes the absence of a
# device a fixture rather than an obstacle. A test double asserting that a
# hand-written `NoDevice` prints the right string would prove nothing about
# RtAudio's actual behaviour when there is nothing to open.
#
# The sibling gate, check_audio_mute.cmake, covers the OTHER half of §9: that
# audio cannot reach the simulation. It drives gloam_replay, which never links
# RtAudio. This one drives `gloam`, which does. Between them the two cover §9.2's
# determinism rule and its degradation rule, and neither needs hardware.
#
# WHAT THIS DOES NOT COVER, said plainly so it is not rediscovered later:
#
#   * the real tick -> first-sample figure (§11's row; needs a device to produce
#     a first sample — test/10budgets/ keeps the PENDING marker);
#   * RTAUDIO_DEVICE_DISCONNECT mid-run, which needs a device to unplug;
#   * whether the callback meets its 5.33 ms deadline under load;
#   * whether a driver grants the 256-frame buffer §9.2 asks for;
#   * whether the mix is audible, and the three sounds distinguishable, TO A
#     PERSON — which is BUILD-ORDER step 10's gate rather than a test.
#
# A NOTE ON STDERR. On a box with no /dev/snd, libasound writes its own
# diagnostics ("cannot find card '0'", "PulseAudio: Unable to connect") before
# RtAudio has an opinion. That is the sound system telling the truth, on the
# right stream, and it is not GLOAM's to suppress. Every assertion below reads
# STDOUT, where GLOAM's own report goes.
#
# Run standalone:
#   cmake -DGLOAM=<path/to/gloam> -DWORK_DIR=<scratch dir> \
#         -P cmake/check_audio_device.cmake

cmake_minimum_required(VERSION 3.28)

if (NOT DEFINED GLOAM)
  message(FATAL_ERROR "check_audio_device: pass -DGLOAM=<path to the gloam binary>")
endif ()

if (NOT DEFINED WORK_DIR)
  set(WORK_DIR "${CMAKE_CURRENT_BINARY_DIR}")
endif ()

set(RUN_DIR "${WORK_DIR}/audio-device")
file(MAKE_DIRECTORY "${RUN_DIR}")

# ── Run it twice with audio, and once muted ─────────────────────────────────
#
# Twice with audio because the arena digest is a determinism claim, and one
# sample of a digest is not a comparison.
execute_process(
  COMMAND "${GLOAM}" --audio --quiet
  WORKING_DIRECTORY "${RUN_DIR}"
  RESULT_VARIABLE AUDIO_STATUS
  OUTPUT_VARIABLE AUDIO_OUT
  ERROR_VARIABLE AUDIO_ERR
)

execute_process(
  COMMAND "${GLOAM}" --audio --quiet
  WORKING_DIRECTORY "${RUN_DIR}"
  RESULT_VARIABLE AUDIO_STATUS_2
  OUTPUT_VARIABLE AUDIO_OUT_2
  ERROR_VARIABLE AUDIO_ERR_2
)

execute_process(
  COMMAND "${GLOAM}" --mute --quiet
  WORKING_DIRECTORY "${RUN_DIR}"
  RESULT_VARIABLE MUTE_STATUS
  OUTPUT_VARIABLE MUTE_OUT
  ERROR_VARIABLE MUTE_ERR
)

# ── 1. It did not crash ─────────────────────────────────────────────────────
#
# The whole of §9.2's degradation rule starts here. A missing device is not a
# missing kitty protocol: §17 reserves fatality for the latter.
if (NOT AUDIO_STATUS EQUAL 0)
  message(FATAL_ERROR
    "check_audio_device: `gloam --audio` exited ${AUDIO_STATUS}. SPEC §9.2 says device loss is "
    "a DEGRADATION, not a crash — \"sink goes silent, game continues\". Failing to open a "
    "device must never be fatal.\nstdout:\n${AUDIO_OUT}\nstderr:\n${AUDIO_ERR}")
endif ()

if (NOT MUTE_STATUS EQUAL 0)
  message(FATAL_ERROR "check_audio_device: `gloam --mute` exited ${MUTE_STATUS}.\n${MUTE_OUT}")
endif ()

# ── 2. It reported which degradation happened ───────────────────────────────
#
# "message line reports it" is the third clause of the rule, and the one a
# silent-but-alive binary would fail. Any of the five states satisfies it; what
# is refused is a run that says nothing about the device at all.
if (NOT AUDIO_OUT MATCHES "device +(no output device|muted|open refused|running|disconnected)")
  message(FATAL_ERROR
    "check_audio_device: `gloam --audio` printed no `device` line. SPEC §9.2 requires the "
    "message line to report a degraded sink; a run that goes silent without saying so is the "
    "failure this gate exists to catch.\nstdout:\n${AUDIO_OUT}")
endif ()

# ── 3. Zero dropped voice commands ──────────────────────────────────────────
#
# §11's row is `budget::kMaxDroppedVoiceCommands == 0`, and this is the gate that
# catches the natural way to break it on a machine with no sound card:
# `DeviceSink::play` pushing unconditionally into a ring nobody drains. After 64
# voices the drop counter climbs forever, and the budget would then be measuring
# the absence of a device rather than the correctness of the ring —
# `audio.hpp`'s own argument about NullSink, which says so in as many words.
if (NOT AUDIO_OUT MATCHES "0 dropped by the ring")
  message(FATAL_ERROR
    "check_audio_device: `gloam --audio` did not report zero dropped voice commands. Either the "
    "sink queued into a ring with no consumer (see DeviceSink::play, and audio.hpp on NullSink) "
    "or the `voices` row changed shape.\nstdout:\n${AUDIO_OUT}")
endif ()

# ── 4. The instrument exists at all ─────────────────────────────────────────
#
# check_audio_mute.cmake's honesty clause, applied here: a gate that cannot tell
# a working sink from an absent one is not a gate. The `voices` row is the thing
# that would carry a real number on a machine with a device, so its absence must
# be a failure rather than a silent pass.
if (NOT AUDIO_OUT MATCHES "voices +[0-9]+ started")
  message(FATAL_ERROR
    "check_audio_device: no `voices` instrument row in `gloam --audio` output. Every assertion "
    "above reads that block; without it they pass vacuously.\nstdout:\n${AUDIO_OUT}")
endif ()

# ── 5. The arena was really synthesised, and deterministically ──────────────
#
# On a device-less box every voice counter is structurally zero, so they prove
# little on their own. The arena digest is what proves §9.2's "all PCM decoded at
# startup into a resident arena" actually happened — and comparing two runs
# proves the synthesiser is deterministic from outside the process, which is a
# different claim from test/27sfxarena/'s in-process memcmp.
string(REGEX MATCH "digest ([0-9a-f]+)" DIGEST_MATCH "${AUDIO_OUT}")
set(DIGEST_ONE "${CMAKE_MATCH_1}")
string(REGEX MATCH "digest ([0-9a-f]+)" DIGEST_MATCH_2 "${AUDIO_OUT_2}")
set(DIGEST_TWO "${CMAKE_MATCH_1}")

if (DIGEST_ONE STREQUAL "")
  message(FATAL_ERROR
    "check_audio_device: `gloam --audio` printed no arena digest, so nothing here shows the "
    "resident PCM was built at all.\nstdout:\n${AUDIO_OUT}")
endif ()

if (NOT DIGEST_ONE STREQUAL DIGEST_TWO)
  message(FATAL_ERROR
    "check_audio_device: two runs of `gloam --audio` synthesised DIFFERENT arenas "
    "(${DIGEST_ONE} vs ${DIGEST_TWO}). sfx.cpp is required to be deterministic in its seed and "
    "nothing else — no clock, no libm, no global state. See sfx.hpp's two rules.")
endif ()

if (AUDIO_STATUS_2 EQUAL 0)
else ()
  message(FATAL_ERROR "check_audio_device: the second `gloam --audio` run exited ${AUDIO_STATUS_2}")
endif ()

# ── 6. --mute perturbs nothing but the audio block ──────────────────────────
#
# §9.2's determinism rule, at the binary's own boundary. check_audio_mute.cmake
# proves it over a recorded replay; this proves the weaker but broader claim that
# turning audio on does not move any other instrument.
#
# TWO rows are excluded, and the second one is not obvious.
#
# The `cold start` row is a TIMING measurement and varies between any two runs —
# 78-109 ms across eight runs when it landed. That much is expected.
#
# The `write path` row has to go too, and finding out why cost a red matrix leg.
# It reports the BYTE COUNT of the instrument block, and the instrument block
# CONTAINS the cold-start row — so when that timing crosses from two digits to
# three, the block grows by one byte and the two totals stop matching.
#
# The first version of this comment said GCC 14 "measures 80-91 ms here and never
# straddles it" while GCC 13 is slow enough to land on both sides. That was a
# tidy story built on ten quiet-box samples and it is wrong: over twenty runs on
# a loaded dev box GCC 14 measures 86-303 ms, with eight of twenty above 100.
# NOTHING here reliably stays on one side of that boundary. The gate happened to
# fail on GCC 13 first because that leg ran first, not because of the compiler.
#
# Which makes the exclusion more necessary rather than less: a gate that fails
# for a reason unrelated to what it tests is worse than no gate, and a
# wall-clock digit count is exactly such a reason. Both rows keep their own
# assertions in test/10budgets/.
string(REGEX REPLACE "\naudio \\(SPEC 9.*" "" AUDIO_HEAD "${AUDIO_OUT}")
string(REGEX REPLACE "\naudio \\(SPEC 9.*" "" MUTE_HEAD "${MUTE_OUT}")
foreach (VARIABLE AUDIO_HEAD MUTE_HEAD)
  string(REGEX REPLACE "  cold start[^\n]*\n" "" ${VARIABLE} "${${VARIABLE}}")
  string(REGEX REPLACE "  write path[^\n]*\n" "" ${VARIABLE} "${${VARIABLE}}")
endforeach ()

if (NOT AUDIO_HEAD STREQUAL MUTE_HEAD)
  message(FATAL_ERROR
    "check_audio_device: `--audio` changed output OUTSIDE the audio block. SPEC §9.2's "
    "\"Audio -> sim is nothing. Ever.\" means opening a device must not move any other "
    "instrument.\n--- with --mute ---\n${MUTE_HEAD}\n--- with --audio ---\n${AUDIO_HEAD}")
endif ()

# ── 7. And muted really is silent ───────────────────────────────────────────
if (NOT MUTE_OUT MATCHES "device +muted")
  message(FATAL_ERROR
    "check_audio_device: `gloam --mute` did not report a muted sink. Without this the "
    "comparison above could be two identical no-device runs.\nstdout:\n${MUTE_OUT}")
endif ()

message(STATUS
  "check_audio_device: CLEAN — --audio degraded without crashing, reported it, dropped nothing, "
  "and synthesised the same arena twice (digest ${DIGEST_ONE})")
