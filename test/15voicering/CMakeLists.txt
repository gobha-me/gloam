# SPEC §9.2 — the SPSC ring, exercised by two real threads.
#
# THE FIRST TEST DIR IN THIS TREE TO OWN ITS WIRING. test/CMakeLists.txt's
# discovery loop has always had the `if (EXISTS <dir>/CMakeLists.txt)` branch and
# nothing had ever used it — its comment still points at a `test/02example` that
# no longer exists. This is the worked example that branch was written for, so it
# is spelled out in full rather than leaning on anything the loop does.
#
# What the parent loop still supplies, and what it does NOT:
#   * it calls add_test(NAME 15voicering-test COMMAND 15voicering-test) AFTER
#     this file runs, so the target name below is load-bearing — rename it and
#     the parent registers a test that does not exist;
#   * it attaches the `runners` fixture;
#   * it supplies NOTHING else. The executable, main.cpp, both include
#     directories, Catch2 and the library are all this file's business, which is
#     exactly what the loop's own comment says: "a test dir with its own
#     CMakeLists.txt owns all of its wiring, this included."
#
# Why this dir needs its own wiring at all: it is the only test that starts a
# thread. §9.2's ring is lock-free between a simulation thread and a real-time
# callback thread, and a single-threaded test cannot see a memory-ordering bug —
# so the concurrent case is the ONLY thing in this repo the TSan job can audit
# here. That needs Threads::Threads, which the auto-discovery branch does not
# link.

find_package(Threads REQUIRED)

add_executable(15voicering-test
  ${CMAKE_CURRENT_LIST_DIR}/../main.cpp
  ${SRCS}
)

# The auto-discovered tests are created in test/, so their binaries land in
# build/test/. A target created in a SUBDIRECTORY lands in build/test/<dir>/
# instead, which ctest does not care about and a human running the binary by hand
# very much does. Put it where every sibling is.
set_target_properties(15voicering-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/..
)

target_include_directories(15voicering-test
  PUBLIC ${PROJECT_SOURCE_DIR}/include
  PUBLIC ${CMAKE_CURRENT_LIST_DIR}/../include
)

target_link_libraries(15voicering-test
  PRIVATE Catch2::Catch2
  PRIVATE Threads::Threads
)

# Guarded like the parent loop guards it. The false branch is unreachable in this
# repo (the library is unconditional — see src/lib/CMakeLists.txt) but copying
# the guard keeps the two spellings of "link the library" identical, so a future
# change to one is an obvious omission in the other.
if (TARGET ${PROJECT_NAME}::lib)
  target_link_libraries(15voicering-test
    PRIVATE ${PROJECT_NAME}::lib
  )
endif ()
