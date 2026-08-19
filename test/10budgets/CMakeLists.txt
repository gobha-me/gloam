# SPEC §11 · BUDGETS.md — every budget as an assertion, including the two that
# need a file descriptor.
#
# THE THIRD TEST DIR IN THIS TREE TO OWN ITS WIRING, and it is here for the same
# reason test/18ttywrite/ is: BUDGETS.md's cold-start rows are measured on what
# actually left the process, and the only `write(2)` in GLOAM is
# src/bin/tty_writer.cpp, which cannot live in gloam::lib (AGENTS.md rule 1,
# §5.1). Timing an encode into a std::string would be timing the compressor and
# calling it a cold start.
#
# Everything test/15voicering/CMakeLists.txt documents about the parent loop
# applies verbatim here:
#   * add_test(NAME 10budgets-test COMMAND 10budgets-test) runs AFTER this file,
#     so the target name is load-bearing;
#   * the `runners` fixture is attached by the parent;
#   * nothing else is supplied — main.cpp, both include dirs, Catch2 and the
#     library are this file's business.

#
# It picks up src/bin/voice_mixer.cpp and src/bin/sfx.cpp for the same reason one
# step further on: §11's tick-to-first-sample row is measured through the real
# mixer, and the mixer is float and therefore cannot live in gloam::lib either.
# Neither of those files includes RtAudio — which is what makes the row assertable
# on a machine with no sound card, and is enforced by
# `rtaudio-boundary-single-module`.

add_executable(10budgets-test
  ${CMAKE_CURRENT_LIST_DIR}/../main.cpp
  ${PROJECT_SOURCE_DIR}/src/bin/tty_writer.cpp
  ${PROJECT_SOURCE_DIR}/src/bin/voice_mixer.cpp
  ${PROJECT_SOURCE_DIR}/src/bin/sfx.cpp
  ${PROJECT_SOURCE_DIR}/src/bin/resident_plates.cpp
  ${PROJECT_SOURCE_DIR}/src/bin/terminal_compositor.cpp
  ${SRCS}
)

# A target created in a subdirectory lands in build/test/<dir>/ instead of
# build/test/, which ctest does not care about and a human running the binary by
# hand very much does. Put it where every sibling is.
set_target_properties(10budgets-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/..
)

# src/bin is on the path so the test can include "tty_writer.hpp" exactly as
# main.cpp does. It is NOT a public include directory of anything — nothing
# outside src/bin/ and the two tests that measure it may reach it.
target_include_directories(10budgets-test
  PUBLIC ${PROJECT_SOURCE_DIR}/include
  PUBLIC ${PROJECT_SOURCE_DIR}/src/bin
  PUBLIC ${CMAKE_CURRENT_LIST_DIR}/../include
)

target_link_libraries(10budgets-test
  PRIVATE Catch2::Catch2
  PRIVATE termforge::lib
)

# Guarded like the parent loop guards it, and like the two siblings copy it. The
# false branch is unreachable in this repo; keeping the spellings identical makes
# a future change to one an obvious omission in the others.
if (TARGET ${PROJECT_NAME}::lib)
  target_link_libraries(10budgets-test
    PRIVATE ${PROJECT_NAME}::lib
  )
endif ()
