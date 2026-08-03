# SPEC §11 · BUDGETS.md — the write syscall, and what it reports.
#
# THE SECOND TEST DIR IN THIS TREE TO OWN ITS WIRING, and for a different reason
# from the first. test/15voicering/ needs Threads::Threads; this one needs a
# translation unit that is deliberately NOT in the library.
#
# src/bin/tty_writer.cpp is the only file in GLOAM that calls write(2). It cannot
# live in gloam::lib — AGENTS.md rule 1, §5.1: a simulation that can reach a file
# descriptor cannot be replayed. The auto-discovery branch links Catch2 and
# ${PROJECT_NAME}::lib and nothing else, so a test for anything on the far side
# of that boundary has to name the source itself. Which is the point: if adding
# this test had required moving the file into the library, that would have been
# the design telling us the boundary was wrong.
#
# Everything test/15voicering/CMakeLists.txt documents about the parent loop
# applies verbatim here:
#   * add_test(NAME 18ttywrite-test COMMAND 18ttywrite-test) runs AFTER this
#     file, so the target name is load-bearing;
#   * the `runners` fixture is attached by the parent;
#   * nothing else is supplied — main.cpp, both include dirs, Catch2 and the
#     library are this file's business.

add_executable(18ttywrite-test
  ${CMAKE_CURRENT_LIST_DIR}/../main.cpp
  ${PROJECT_SOURCE_DIR}/src/bin/tty_writer.cpp
  ${SRCS}
)

# A target created in a subdirectory lands in build/test/<dir>/ instead of
# build/test/, which ctest does not care about and a human running the binary by
# hand very much does. Put it where every sibling is.
set_target_properties(18ttywrite-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/..
)

# src/bin is on the path so the test can include "tty_writer.hpp" exactly as
# main.cpp does. It is NOT a public include directory of anything — nothing
# outside src/bin/ and this test may reach it.
target_include_directories(18ttywrite-test
  PUBLIC ${PROJECT_SOURCE_DIR}/include
  PUBLIC ${PROJECT_SOURCE_DIR}/src/bin
  PUBLIC ${CMAKE_CURRENT_LIST_DIR}/../include
)

target_link_libraries(18ttywrite-test
  PRIVATE Catch2::Catch2
)

# Guarded like the parent loop guards it, and like test/15voicering/ copies it.
# The false branch is unreachable in this repo; keeping the two spellings
# identical makes a future change to one an obvious omission in the other.
if (TARGET ${PROJECT_NAME}::lib)
  target_link_libraries(18ttywrite-test
    PRIVATE ${PROJECT_NAME}::lib
  )
endif ()
