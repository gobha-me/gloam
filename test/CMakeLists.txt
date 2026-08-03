add_test(NAME startup COMMAND ${CMAKE_CURRENT_LIST_DIR}/../cmake/startup.sh)
add_test(NAME shutdown COMMAND ${CMAKE_CURRENT_LIST_DIR}/../cmake/shutdown.sh)

# Pure cmake -P unit test for the git-describe version parser (cmake/version_parse.cmake).
# Registered standalone — it is a script, not a `<dir>-test` executable, so it must
# not go through the auto-discovery foreach below. No fixtures: it has no runtime deps.
add_test(
  NAME version-parse-selftest
  COMMAND ${CMAKE_COMMAND} -P ${PROJECT_SOURCE_DIR}/cmake/version_selftest.cmake
)

# Leftover-template-artifact check (cmake/check_artifacts.cmake), also standalone.
# The mode is keyed on NEW_PROJECT.md, whose last checklist step is "delete this
# file": while it exists we are the template, so the Class-A rules are inverted
# and assert each still MATCHES something (a rule that matches nothing has rotted
# and would wave every fork through). Once a bootstrapped project deletes it, the
# same script becomes a plain enforcement check with nothing to wire up.
if (EXISTS ${PROJECT_SOURCE_DIR}/NEW_PROJECT.md)
  add_test(
    NAME artifact-check-selftest
    COMMAND ${CMAKE_COMMAND} -DMODE=selftest -P ${PROJECT_SOURCE_DIR}/cmake/check_artifacts.cmake
  )
else ()
  add_test(
    NAME artifact-check
    COMMAND ${CMAKE_COMMAND} -P ${PROJECT_SOURCE_DIR}/cmake/check_artifacts.cmake
  )
endif ()

# SPEC §19 step 2 — the below-background z threshold appears exactly once in the
# shipped tree, inside the named layer API. A cmake -P script rather than a C++
# test, because the thing under test is the SOURCE, not the binary.
add_test(
  NAME layer-z-single-definition
  COMMAND ${CMAKE_COMMAND} -DPROJECT_ROOT=${PROJECT_SOURCE_DIR} -P
          ${PROJECT_SOURCE_DIR}/cmake/check_layer_z.cmake
)

# SPEC §19 step 2, the other half — "every escape sequence GLOAM emits goes
# through one module". The z threshold check above proves no application code
# hand-writes a z-index; this proves no application code hand-writes a kitty
# command. §16's "a vendored driver is a swap and not a rewrite" needs both.
add_test(
  NAME kitty-boundary-single-module
  COMMAND ${CMAKE_COMMAND} -DPROJECT_ROOT=${PROJECT_SOURCE_DIR} -P
          ${PROJECT_SOURCE_DIR}/cmake/check_kitty_boundary.cmake
)

# SPEC §19 step 5 — "Accept: two pipeline runs produce byte-identical packs."
#
# test/12pack/ asserts the same property in process; this runs the real binary
# twice, in two working directories, and compares the files. The two are not
# redundant — an in-process comparison shares an allocator and an address space,
# so an uninitialised byte can read the same value both times.
#
# Guarded on the target: ${PROJECT_NAME}_BUILD_BIN defaults to
# PROJECT_IS_TOP_LEVEL, so a consumer's build has no baker to run.
if (NOT TARGET ${PROJECT_NAME}_bake)
  # Say so at configure time rather than letting ctest quietly report 20 passed
  # instead of 21. A dropped acceptance gate that announces nothing is
  # indistinguishable from one that ran — the same reason the library guard
  # below prints a STATUS.
  message(STATUS
    "${PROJECT_NAME}_bake is not built — SPEC §19 step 5's pack-reproducible gate "
    "will NOT run. Pair -D${PROJECT_NAME}_BUILD_BIN=OFF with the knowledge that the "
    "out-of-process byte-identical check is unverified in this configuration."
  )
endif ()

if (TARGET ${PROJECT_NAME}_bake)
  add_test(
    NAME pack-reproducible
    COMMAND ${CMAKE_COMMAND}
            -DBAKE=$<TARGET_FILE:${PROJECT_NAME}_bake>
            -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -P ${PROJECT_SOURCE_DIR}/cmake/check_pack_repro.cmake
  )
endif ()

# SPEC §19 step 7 — "Accept: a recorded session replays to an identical world
# hash on both compilers."
#
# test/13replay/ asserts it in process; this records in one process and replays
# in another, through a file. Same argument as the pack gate above: a shared
# allocator and address space can hide an uninitialised read that two processes
# cannot. The "both compilers" half is CI's matrix running this twice.
if (NOT TARGET ${PROJECT_NAME}_replay)
  message(STATUS
    "${PROJECT_NAME}_replay is not built — SPEC §19 step 7's replay-deterministic gate "
    "will NOT run. Pair -D${PROJECT_NAME}_BUILD_BIN=OFF with the knowledge that the "
    "out-of-process determinism check is unverified in this configuration."
  )
endif ()

if (TARGET ${PROJECT_NAME}_replay)
  add_test(
    NAME replay-deterministic
    COMMAND ${CMAKE_COMMAND}
            -DREPLAY=$<TARGET_FILE:${PROJECT_NAME}_replay>
            -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -P ${PROJECT_SOURCE_DIR}/cmake/check_replay_determinism.cmake
  )
  # The one test in the suite that runs a simulation for as long as a FILE tells
  # it to. `replay::kMaxTick` bounds that to a few seconds, but a bound enforced
  # only inside the thing under test is not a bound on the harness — if it ever
  # regresses, this must fail the build rather than hang it. Everything here
  # finishes in well under a second.
  set_tests_properties(replay-deterministic PROPERTIES TIMEOUT 120)

  # SPEC §19 step 9 — "Accept: `--mute` and unmuted runs produce identical
  # replays."
  #
  # test/16audiosim/ asserts the identity in process; this records and replays
  # the same session both ways in separate processes and compares the artifacts.
  # Same argument as the two gates above — a shared allocator and address space
  # can mask a difference that two processes cannot.
  #
  # It needs no audio device and never will: gloam::audio is standard library and
  # gloam_replay does not link RtAudio. See the header of check_audio_mute.cmake.
  add_test(
    NAME audio-mute-identical
    COMMAND ${CMAKE_COMMAND}
            -DREPLAY=$<TARGET_FILE:${PROJECT_NAME}_replay>
            -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -P ${PROJECT_SOURCE_DIR}/cmake/check_audio_mute.cmake
  )
  set_tests_properties(audio-mute-identical PROPERTIES TIMEOUT 120)
endif ()

file(GLOB TEST_DIRS ${CMAKE_CURRENT_LIST_DIR}/*)

# Say this once, at configure time, instead of letting it surface as a link error
# buried in a build log. The guard below keeps *configuring* valid with the
# library switched off; it cannot conjure symbols for a test that calls into it.
if (NOT TARGET ${PROJECT_NAME}::lib)
  message(STATUS
    "${PROJECT_NAME}::lib is not built — discovered tests will not link against it. "
    "Pair -D${PROJECT_NAME}_BUILD_LIB=OFF with -D${PROJECT_NAME}_TESTS=OFF, or keep "
    "only tests that do not call into the library."
  )
endif ()

foreach(TEST_DIR ${TEST_DIRS})
  cmake_path(GET TEST_DIR FILENAME TEST_NAME_PATH)
  set(TEST_NAME ${TEST_NAME_PATH}-test)

  # Recompute the source list for THIS dir every iteration, before the branch
  # below. Both branches consume SRCS — the custom-CMakeLists branch inherits it
  # via add_subdirectory() — so setting it only inside the elseif would leak the
  # previous iteration's sources into a custom-CMakeLists test dir.
  file(GLOB SRCS ${TEST_DIR}/*.cpp)

  if(EXISTS ${TEST_DIR}/CMakeLists.txt)
    add_subdirectory(${TEST_DIR})
  elseif (EXISTS ${TEST_DIR}/test.cpp)
    message(STATUS "Configuring test setup ${TEST_NAME}")

    add_executable(${TEST_NAME}
      main.cpp
      ${SRCS}
    )

    # Kept even though ${PROJECT_NAME}::lib already propagates include/ below:
    # with -D<PROJECT>_BUILD_LIB=OFF there is no library to propagate anything,
    # and every test still includes the generated <version.hpp> from there. The
    # second path is a hook for shared test-only headers in test/include/.
    target_include_directories(${TEST_NAME}
      PUBLIC ${PROJECT_SOURCE_DIR}/include
      PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include
    )

    target_link_libraries(${TEST_NAME}
      PRIVATE Catch2::Catch2
    )

    # Auto-discovered tests get the project library for free. Without this,
    # testing your own code means either a per-dir CMakeLists.txt or copying
    # library sources into the test dir — and the first test written against
    # src/lib/ dies at link time with `undefined reference`, which reads like a
    # broken build rather than a missing line.
    #
    # The guard is not optional: -D<PROJECT>_BUILD_LIB=OFF never creates the
    # target, and an unguarded link line would turn that supported configuration
    # into a hard configure error.
    #
    # The ALIAS makes the STATIC/INTERFACE choice in src/lib/CMakeLists.txt
    # invisible here — linking an ALIAS to an INTERFACE library propagates its
    # usage requirements and archives nothing, so this line is correct for both.
    #
    # This lives in the elseif branch on purpose: a test dir with its own
    # CMakeLists.txt owns all of its wiring, this included (see test/02example).
    if (TARGET ${PROJECT_NAME}::lib)
      target_link_libraries(${TEST_NAME}
        PRIVATE ${PROJECT_NAME}::lib
      )
    endif ()
  else()
    continue()
  endif()

  # No -s here: it makes Catch2 report every *successful* assertion too, which
  # buries the interesting output in a full-suite run. Ask for it per invocation
  # when you actually want it — `ctest -R <name> -V`, or `./build/test/<dir>-test -s`.
  add_test(
    NAME ${TEST_NAME}
    COMMAND ${TEST_NAME}
  )

  set_tests_properties(${TEST_NAME} PROPERTIES FIXTURES_REQUIRED runners)
endforeach()

# SPEC §9.2 — "a full ring drops the command and increments a counter. It never
# blocks."
#
# A ring that blocks does not fail this suite, it HANGS it, and a hung runner is
# a worse outcome than a red one because nothing reports it. The timeout turns
# the one failure mode §9.2 names into an ordinary test failure. Same argument as
# replay-deterministic's timeout below; everything here finishes in about a
# second, TSan included.
#
# Set here rather than in test/15voicering/CMakeLists.txt because add_test runs
# in the loop above, AFTER that subdirectory is processed — the test does not
# exist yet at the point the subdirectory could name it.
if (TEST 15voicering-test)
  set_tests_properties(15voicering-test PROPERTIES TIMEOUT 120)
endif ()

# Same argument, different failure mode. test/18ttywrite/ is the only suite that
# drives real file descriptors, and BUDGETS.md's "what actually left the process"
# is measured by filling a pipe on purpose. A write to a pipe nobody drains
# BLOCKS; a `classify` regression that turned a stall into a retry LOOPS. Neither
# is a test failure without this line, and both are worse than one.
#
# Set here rather than in that directory's own CMakeLists.txt for the reason
# above: add_test runs in the loop, after the subdirectory is processed.
if (TEST 18ttywrite-test)
  set_tests_properties(18ttywrite-test PROPERTIES TIMEOUT 120)
endif ()

# SET FIXTURE DEPENDENCIES
set_tests_properties(startup PROPERTIES FIXTURES_SETUP runners)
set_tests_properties(shutdown PROPERTIES FIXTURES_CLEANUP runners)

# The sanitizer smoke test (test/30sanitizer-smoke) deliberately trips the active
# sanitizer to prove it is actually engaged (CT-03 — the whole point is to catch a
# silently no-op sanitizer toolchain). ASan/TSan abort the process on the injected
# bug, i.e. a NONZERO exit, which ctest would otherwise read as failure. So when
# such a sanitizer is active we assert on its diagnostic instead: the test passes
# iff the sanitizer's own report appears in the output. This turns "sanitizer
# engaged" into a green, enforceable signal — and if the sanitizer were ever
# silenced, the report vanishes and this test goes red. UBSan is recoverable
# (exits 0) and the default build never runs the bug, so neither needs this.
if (TARGET 30sanitizer-smoke-test)
  if (CMAKE_CXX_FLAGS MATCHES "-fsanitize=address")
    set_tests_properties(30sanitizer-smoke-test PROPERTIES PASS_REGULAR_EXPRESSION "AddressSanitizer")
  elseif (CMAKE_CXX_FLAGS MATCHES "-fsanitize=thread")
    set_tests_properties(30sanitizer-smoke-test PROPERTIES PASS_REGULAR_EXPRESSION "ThreadSanitizer")
  endif ()
endif ()
