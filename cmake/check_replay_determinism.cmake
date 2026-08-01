# SPEC §12, §19 step 7 — "Accept: a recorded session replays to an identical
# world hash on both compilers."
#
# TEST-PLAN.md §2 states the stakes: "There is no flaky-test triage path for
# this suite. A nondeterministic simulation has no correct behaviour to fall
# back to. A mismatch is a regression."
#
# WHY OUT OF PROCESS, when test/13replay/ already records and replays in one
# address space: the in-process check shares one allocator, one address-space
# layout and one set of already-touched pages, so a read of an uninitialised
# byte can easily return the same value both times. Recording in one process
# and replaying in another, through a file, does not share any of that. The two
# checks are not redundant; they fail for different reasons.
#
# The "on both compilers" half is CI's matrix, which runs this under GCC and
# Clang. This script asserts the property; the matrix asserts it twice.
#
# Run standalone:
#   cmake -DREPLAY=<path/to/gloam_replay> -DWORK_DIR=<scratch dir> \
#         -P cmake/check_replay_determinism.cmake

cmake_minimum_required(VERSION 3.28)

if (NOT DEFINED REPLAY)
  message(FATAL_ERROR "check_replay_determinism: pass -DREPLAY=<path to gloam_replay>")
endif ()

if (NOT DEFINED WORK_DIR)
  set(WORK_DIR "${CMAKE_CURRENT_BINARY_DIR}")
endif ()

set(RUN_A "${WORK_DIR}/replay-det-a")
set(RUN_B "${WORK_DIR}/replay-det-b")
file(MAKE_DIRECTORY "${RUN_A}" "${RUN_B}")

# ── Two recordings, in different working directories ────────────────────────
#
# Different directories as well as different output paths: a recorder that
# embedded a path, a timestamp or a hostname would produce two different files
# here and be indistinguishable from a correct one if both runs shared a
# directory.
foreach (RUN "${RUN_A}" "${RUN_B}")
  execute_process(
    COMMAND "${REPLAY}" record -o "${RUN}/replay.gloam" --quiet
    WORKING_DIRECTORY "${RUN}"
    RESULT_VARIABLE STATUS
    OUTPUT_VARIABLE STDOUT
    ERROR_VARIABLE STDERR
  )
  if (NOT STATUS EQUAL 0)
    message(FATAL_ERROR "check_replay_determinism: recording failed in ${RUN} (${STATUS})\n${STDERR}")
  endif ()
  string(STRIP "${STDOUT}" STDOUT)
  list(APPEND RECORDED "${STDOUT}")
endforeach ()

file(SHA256 "${RUN_A}/replay.gloam" FILE_A)
file(SHA256 "${RUN_B}/replay.gloam" FILE_B)

if (NOT FILE_A STREQUAL FILE_B)
  message(FATAL_ERROR
    "check_replay_determinism: two recordings of the SAME session produced different files.\n"
    "  ${RUN_A}/replay.gloam  ${FILE_A}\n"
    "  ${RUN_B}/replay.gloam  ${FILE_B}\n"
    "The container is not reproducible. Look for uninitialised padding reaching the digest, "
    "or a field written from something the environment supplied.")
endif ()

list(GET RECORDED 0 HASH_A)
list(GET RECORDED 1 HASH_B)
if (NOT HASH_A STREQUAL HASH_B)
  message(FATAL_ERROR
    "check_replay_determinism: two runs of the same seed and the same input log reached "
    "different world hashes.\n  ${HASH_A}\n  ${HASH_B}\n"
    "§5.1: seed in, world out. This is a determinism regression — look for a float in "
    "simulation state, a wall-clock read, or an iteration order that depends on addresses.")
endif ()

# ── Replay, in a process that did not record ────────────────────────────────
execute_process(
  COMMAND "${REPLAY}" play "${RUN_A}/replay.gloam" --quiet
  WORKING_DIRECTORY "${RUN_B}"
  RESULT_VARIABLE STATUS
  OUTPUT_VARIABLE REPLAYED
  ERROR_VARIABLE STDERR
)
if (NOT STATUS EQUAL 0)
  message(FATAL_ERROR
    "check_replay_determinism: replaying a file this process did not record FAILED (${STATUS}).\n"
    "${STDERR}\n"
    "§19 step 7's acceptance criterion is exactly this claim.")
endif ()
string(STRIP "${REPLAYED}" REPLAYED)

if (NOT REPLAYED STREQUAL HASH_A)
  message(FATAL_ERROR
    "check_replay_determinism: the recorder reached ${HASH_A} but the replayer reached "
    "${REPLAYED} from the recorder's own file.\n"
    "TEST-PLAN.md §2: there is no flaky-test triage path for this. It is a regression.")
endif ()

# ── A damaged replay must be REFUSED, not played ────────────────────────────
#
# Without this, the gate would still pass if `play` ignored the header entirely
# — which is precisely how a self-checking artifact quietly stops being one.
#
# The damage is one appended byte rather than a flipped one, because CMake can
# append an ASCII byte to a file and cannot write an arbitrary raw one. It is
# the better test anyway: a file one byte longer than its own `total_bytes` is
# what a truncated or padded transfer actually looks like.
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E copy "${RUN_A}/replay.gloam" "${RUN_B}/damaged.gloam"
  RESULT_VARIABLE STATUS
)
if (NOT STATUS EQUAL 0)
  message(FATAL_ERROR "check_replay_determinism: could not stage the damaged replay")
endif ()
file(APPEND "${RUN_B}/damaged.gloam" "X")

execute_process(
  COMMAND "${REPLAY}" play "${RUN_B}/damaged.gloam" --quiet
  RESULT_VARIABLE STATUS
  ERROR_VARIABLE STDERR
)
if (NOT STATUS MATCHES "^[0-9]+$")
  message(FATAL_ERROR
    "check_replay_determinism: a damaged replay killed the process abnormally (${STATUS}).\n"
    "${STDERR}\nIt must be refused with a message, not aborted.")
endif ()
if (STATUS EQUAL 0)
  message(FATAL_ERROR
    "check_replay_determinism: a replay with a byte appended was ACCEPTED and played.\n"
    "The container's own size and digest checks are not reaching the load path, so a "
    "corrupt artifact would be reported as a determinism regression rather than as corruption.")
endif ()

# ── A path that is not a regular file must not crash the process ────────────
#
# Opening a directory with libstdc++ succeeds and `tellg()` returns LLONG_MAX,
# not the -1 that unseekable streams are documented to give. Both binaries
# aborted on bad_alloc here until they checked. A crash and a refusal are
# different outcomes, and only one of them is a diagnosis.
execute_process(
  COMMAND "${REPLAY}" play "${RUN_A}"
  RESULT_VARIABLE STATUS
  ERROR_VARIABLE STDERR
)
if (STATUS EQUAL 0)
  message(FATAL_ERROR "check_replay_determinism: replaying a DIRECTORY reported success")
endif ()

# NOT `STATUS GREATER 127`. CMake does not use the shell's 128+N convention:
# `execute_process` sets RESULT_VARIABLE to a non-numeric STRING when the child
# dies by signal — "Illegal instruction", "Subprocess aborted" — and
# `if(<non-numeric> GREATER 127)` is FALSE. Measured on CMake 3.28.3. Written
# the obvious way, this check could never fire, and the abort it exists to catch
# would have been reported as a pass by the branch above.
if (NOT STATUS MATCHES "^[0-9]+$")
  message(FATAL_ERROR
    "check_replay_determinism: replaying a directory killed the process abnormally "
    "(${STATUS}).\n${STDERR}\n"
    "It must be refused with a message, not aborted.")
endif ()

# A header that lies about its own length — the 104-byte file claiming 613
# million records, which is what turns a truncated download into a 4.9 GB
# allocation — is asserted in test/13replay/ instead of here. CMake has no way
# to write an arbitrary raw byte, so the buffer cannot be built exactly at this
# layer; it can be built exactly in C++, and is.

message(STATUS "check_replay_determinism: CLEAN — ${HASH_A}")

message(STATUS "check_replay_determinism: CLEAN — ${HASH_A}")
