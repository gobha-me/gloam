# SPEC §10, §12 — the pack: schema, failure matrix, and the golden digest.
#
# THE SEVENTH TEST DIR IN THIS TREE TO OWN ITS WIRING, and for
# test/27sfxarena/'s reason exactly: the golden is over the pack gloam_bake
# SHIPS, and since gloam#23 that pack carries the audio arena — whose PCM is
# synthesised by src/bin/sfx.cpp, a translation unit that is deliberately NOT
# in gloam::lib (it produces float; AGENTS.md rule 2). Compiling it directly
# keeps the golden asserted against the real bytes rather than a stand-in.
#
# Everything test/15voicering/CMakeLists.txt documents about the parent loop
# applies verbatim here: the target name is load-bearing because add_test runs
# after this file, the `runners` fixture is attached by the parent, and
# main.cpp, both include dirs, Catch2 and the library are this file's business.

add_executable(12pack-test
  ${CMAKE_CURRENT_LIST_DIR}/../main.cpp
  ${PROJECT_SOURCE_DIR}/src/bin/sfx.cpp
  ${SRCS}
)

# A target created in a subdirectory lands in build/test/<dir>/ instead of
# build/test/, which ctest does not care about and a human running the binary by
# hand very much does. Put it where every sibling is.
set_target_properties(12pack-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/..
)

# src/bin is on the path so the test can include "sfx.hpp" exactly as main.cpp
# does. It is NOT a public include directory of anything.
target_include_directories(12pack-test
  PUBLIC ${PROJECT_SOURCE_DIR}/include
  PUBLIC ${PROJECT_SOURCE_DIR}/src/bin
  PUBLIC ${CMAKE_CURRENT_LIST_DIR}/../include
)

target_link_libraries(12pack-test
  PRIVATE Catch2::Catch2
)

# Guarded like the parent loop guards it, and like the siblings copy it. The
# false branch is unreachable in this repo; keeping the spellings identical
# makes a future change to one an obvious omission in the others.
if (TARGET ${PROJECT_NAME}::lib)
  target_link_libraries(12pack-test
    PRIVATE ${PROJECT_NAME}::lib
  )
endif ()
