# SPEC §10, §19 step 5 — "Accept: two pipeline runs produce byte-identical
# packs."
#
# §10 makes this a hard requirement rather than a nicety: "the pipeline run
# twice on the same input must produce byte-identical plates, because the pack
# hash is a build gate." A pipeline that is not reproducible cannot have its
# integrity verified at startup, and §10 makes a mismatched hash refuse to
# launch rather than half-upload a corrupt plate set.
#
# WHY OUT OF PROCESS, when test/12pack/ already assembles the same pack twice
# and compares: the in-process check shares one allocator, one address-space
# layout and one set of already-touched pages, so a read of an uninitialised
# byte can easily return the same value both times. Two separate processes
# writing two separate files do not share any of that. The two checks are not
# redundant; they fail for different reasons.
#
# Run standalone:
#   cmake -DBAKE=<path/to/gloam_bake> -DWORK_DIR=<scratch dir> -P cmake/check_pack_repro.cmake

cmake_minimum_required(VERSION 3.28)

if (NOT DEFINED BAKE)
  message(FATAL_ERROR "check_pack_repro: pass -DBAKE=<path to the gloam_bake executable>")
endif ()

if (NOT DEFINED WORK_DIR)
  set(WORK_DIR "${CMAKE_CURRENT_BINARY_DIR}")
endif ()

set(RUN_A "${WORK_DIR}/pack-repro-a")
set(RUN_B "${WORK_DIR}/pack-repro-b")
file(MAKE_DIRECTORY "${RUN_A}" "${RUN_B}")

# Different working directories as well as different output paths: a pipeline
# that embedded a path, a timestamp or a hostname would produce two different
# files here and be indistinguishable from a correct one if both runs shared a
# directory.
foreach (RUN "${RUN_A}" "${RUN_B}")
  execute_process(
    COMMAND "${BAKE}" -o "${RUN}/pack.gloam" --quiet
    WORKING_DIRECTORY "${RUN}"
    RESULT_VARIABLE STATUS
    OUTPUT_VARIABLE STDOUT
    ERROR_VARIABLE STDERR
  )
  if (NOT STATUS EQUAL 0)
    message(FATAL_ERROR "check_pack_repro: the baker failed in ${RUN} (${STATUS})\n${STDERR}")
  endif ()
  string(STRIP "${STDOUT}" STDOUT)
  list(APPEND REPORTED "${STDOUT}")
endforeach ()

file(SHA256 "${RUN_A}/pack.gloam" HASH_A)
file(SHA256 "${RUN_B}/pack.gloam" HASH_B)
file(SIZE "${RUN_A}/pack.gloam" SIZE_A)
file(SIZE "${RUN_B}/pack.gloam" SIZE_B)

if (NOT SIZE_A EQUAL SIZE_B)
  message(FATAL_ERROR
    "check_pack_repro: two runs produced packs of different SIZES — ${SIZE_A} vs ${SIZE_B} B. "
    "§10 requires byte-identical output; something in the pipeline depends on the environment.")
endif ()

if (NOT HASH_A STREQUAL HASH_B)
  message(FATAL_ERROR
    "check_pack_repro: two runs produced different packs.\n"
    "  ${RUN_A}/pack.gloam  ${HASH_A}\n"
    "  ${RUN_B}/pack.gloam  ${HASH_B}\n"
    "§10 makes this a build gate: an unreproducible pipeline cannot have its integrity "
    "verified at startup. Look for a float, an uninitialised byte, or an iteration order "
    "that depends on a container's addresses.")
endif ()

# The baker prints the digest it computed. If that disagrees with the file on
# disk, the pack was written incompletely or something changed it afterwards —
# a distinct failure from the two runs disagreeing with each other.
list(GET REPORTED 0 REPORTED_A)
if (NOT REPORTED_A STREQUAL HASH_A)
  message(FATAL_ERROR
    "check_pack_repro: the baker reported ${REPORTED_A} but the file on disk hashes to "
    "${HASH_A}. The pack was not written whole.")
endif ()

message(STATUS "check_pack_repro: CLEAN — two runs, ${SIZE_A} B, ${HASH_A}")
