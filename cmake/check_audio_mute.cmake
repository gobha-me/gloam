# SPEC §9.2, §19 step 9 — "Accept: `--mute` and unmuted runs produce identical
# replays."
#
# §9.2 states the property this gate defends: "Audio -> sim is nothing. Ever. No
# feedback path means determinism holds by construction, and `--mute` produces a
# bit-identical replay. This is the property that makes the whole subsystem
# safe."
#
# WHY OUT OF PROCESS, when test/16audiosim/ already asserts the same identity in
# one address space: the same argument check_replay_determinism.cmake makes.
# An in-process comparison shares an allocator, an address-space layout and one
# set of already-touched pages, so a difference can be masked. Two processes
# share none of it. The two checks are not redundant; they fail for different
# reasons.
#
# NO AUDIO DEVICE IS INVOLVED, ANYWHERE. `gloam::audio`'s ring and mix are
# standard library, and `gloam_replay` never links RtAudio — so this gate runs
# identically on a developer's laptop, on this project's dev box (which has no
# /dev/snd at all) and on a CI runner. That is deliberate: an acceptance gate
# that only runs where there is a sound card is an acceptance gate that does not
# run.
#
# Run standalone:
#   cmake -DREPLAY=<path/to/gloam_replay> -DWORK_DIR=<scratch dir> \
#         -P cmake/check_audio_mute.cmake

cmake_minimum_required(VERSION 3.28)

if (NOT DEFINED REPLAY)
  message(FATAL_ERROR "check_audio_mute: pass -DREPLAY=<path to gloam_replay>")
endif ()

if (NOT DEFINED WORK_DIR)
  set(WORK_DIR "${CMAKE_CURRENT_BINARY_DIR}")
endif ()

set(MUTED_DIR "${WORK_DIR}/audio-mute-muted")
set(HEARD_DIR "${WORK_DIR}/audio-mute-heard")
file(MAKE_DIRECTORY "${MUTED_DIR}" "${HEARD_DIR}")

# ── Record the same session twice, muted and unmuted ────────────────────────
#
# Different working directories as well as different output paths, for
# check_replay_determinism.cmake's reason: a recorder that embedded a path or a
# hostname would differ here and be indistinguishable from a correct one if both
# runs shared a directory.
execute_process(
  COMMAND "${REPLAY}" record --mute -o "${MUTED_DIR}/replay.gloam" --quiet
  WORKING_DIRECTORY "${MUTED_DIR}"
  RESULT_VARIABLE MUTED_STATUS
  OUTPUT_VARIABLE MUTED_HASH
  ERROR_VARIABLE MUTED_STDERR
)
if (NOT MUTED_STATUS EQUAL 0)
  message(FATAL_ERROR "check_audio_mute: the muted recording failed (${MUTED_STATUS})\n${MUTED_STDERR}")
endif ()
string(STRIP "${MUTED_HASH}" MUTED_HASH)

execute_process(
  COMMAND "${REPLAY}" record --audio -o "${HEARD_DIR}/replay.gloam" --quiet
  WORKING_DIRECTORY "${HEARD_DIR}"
  RESULT_VARIABLE HEARD_STATUS
  OUTPUT_VARIABLE HEARD_HASH
  ERROR_VARIABLE HEARD_STDERR
)
if (NOT HEARD_STATUS EQUAL 0)
  message(FATAL_ERROR "check_audio_mute: the unmuted recording failed (${HEARD_STATUS})\n${HEARD_STDERR}")
endif ()
string(STRIP "${HEARD_HASH}" HEARD_HASH)

# ── The unmuted run must ACTUALLY HAVE MADE SOUND ───────────────────────────
#
# THIS CHECK COMES FIRST BECAUSE IT IS THE ONE THAT KEEPS THE GATE HONEST.
#
# "Muted and unmuted are identical" is satisfied perfectly by an audio subsystem
# that does nothing at all. A sink that is never called, a footfall condition
# stuck false, a sting keyed on a tell that never fires — every one of those
# makes the comparison below pass while §9 is dead. An identity proved over
# silence is not evidence, so the traffic is asserted before the identity is.
if (NOT HEARD_STDERR MATCHES "voices=([0-9]+)")
  message(FATAL_ERROR
    "check_audio_mute: the --audio run reported no voice counters at all.\n"
    "stderr was:\n${HEARD_STDERR}\n"
    "Without them this gate cannot tell a working sink from a silent one, and "
    "the identity check below would pass either way.")
endif ()
set(VOICES "${CMAKE_MATCH_1}")

if (VOICES EQUAL 0)
  message(FATAL_ERROR
    "check_audio_mute: the --audio run produced ZERO voices.\n"
    "The identity check below would pass trivially, so this is a failure rather "
    "than a curiosity: either the sink is not being passed to `play`, or nothing "
    "in the scripted session emits any more. §19 step 9's criterion is about a "
    "subsystem that works, not one that is absent.")
endif ()

# EVERY EMITTER, NOT JUST SOME EMITTER.
#
# A total count above zero is a weaker guard than it looks: §19 step 9 asks for
# "footfalls and one sting", and killing either one on its own leaves the other
# still counting. Measured — disabling the footfall path entirely left this gate
# green on the sting alone. The scripted session produces both, so both are
# required by name.
foreach (EMITTER footfalls stings)
  if (NOT HEARD_STDERR MATCHES "${EMITTER}=([0-9]+)")
    message(FATAL_ERROR
      "check_audio_mute: the --audio run reported no `${EMITTER}` counter\n${HEARD_STDERR}")
  endif ()
  if (CMAKE_MATCH_1 EQUAL 0)
    message(FATAL_ERROR
      "check_audio_mute: the --audio run produced NO ${EMITTER}.\n"
      "stderr was:\n${HEARD_STDERR}\n"
      "§19 step 9 is \"footfalls and one sting\" — one working emitter is not the "
      "criterion, and the identity check below cannot tell the difference.")
  endif ()
endforeach ()

# §11's row: zero dropped voice commands per 1000-tick replay. This session is
# far shorter than the window, so anything above zero here is a ring that is
# wrong rather than a ring that is busy.
if (NOT HEARD_STDERR MATCHES "dropped=([0-9]+)")
  message(FATAL_ERROR "check_audio_mute: the --audio run reported no drop counter\n${HEARD_STDERR}")
endif ()
if (NOT CMAKE_MATCH_1 EQUAL 0)
  message(FATAL_ERROR
    "check_audio_mute: the --audio run DROPPED ${CMAKE_MATCH_1} voice commands over a "
    "session of a few ticks.\n"
    "§11 budgets zero drops per 1000 ticks. A ring that overflows this early is "
    "either far too small or is not being drained at all.")
endif ()

# ── The two recordings must be the same artifact ────────────────────────────
file(SHA256 "${MUTED_DIR}/replay.gloam" MUTED_FILE)
file(SHA256 "${HEARD_DIR}/replay.gloam" HEARD_FILE)

if (NOT MUTED_FILE STREQUAL HEARD_FILE)
  message(FATAL_ERROR
    "check_audio_mute: recording the SAME session muted and unmuted produced different files.\n"
    "  muted    ${MUTED_FILE}\n"
    "  unmuted  ${HEARD_FILE}\n"
    "§9.2: \"Audio -> sim is nothing. Ever.\" Something in the audio path is "
    "writing to simulation state or reading a value back out of the sink. Look "
    "for a `play` that returns something, or a field added to World.")
endif ()

if (NOT MUTED_HASH STREQUAL HEARD_HASH)
  message(FATAL_ERROR
    "check_audio_mute: the muted run reached ${MUTED_HASH} and the unmuted run reached "
    "${HEARD_HASH}.\n"
    "Attaching a sink changed the simulation. This is the failure §19 step 9's "
    "acceptance criterion exists to detect, and TEST-PLAN.md §2 applies: there is "
    "no flaky-test triage path here, it is a regression.")
endif ()

# ── And replaying, which is the other half of "identical replays" ───────────
#
# A recorder could be audio-clean while the PLAYER is not — `play` forwards the
# sink to every `advance`, so it has its own opportunity to diverge. Replaying
# one file both ways is what covers that.
foreach (MODE "--mute" "--audio")
  execute_process(
    COMMAND "${REPLAY}" play "${MUTED_DIR}/replay.gloam" ${MODE} --quiet
    WORKING_DIRECTORY "${HEARD_DIR}"
    RESULT_VARIABLE STATUS
    OUTPUT_VARIABLE REPLAYED
    ERROR_VARIABLE STDERR
  )
  if (NOT STATUS EQUAL 0)
    message(FATAL_ERROR "check_audio_mute: `play ${MODE}` failed (${STATUS})\n${STDERR}")
  endif ()
  string(STRIP "${REPLAYED}" REPLAYED)
  if (NOT REPLAYED STREQUAL MUTED_HASH)
    message(FATAL_ERROR
      "check_audio_mute: replaying with ${MODE} reached ${REPLAYED}, but the file carries "
      "${MUTED_HASH}.\n"
      "The sink is reaching the simulation through `play`.")
  endif ()
endforeach ()

message(STATUS "check_audio_mute: CLEAN — ${VOICES} voices, 0 dropped, ${MUTED_HASH}")
