# SPEC §9.2 — the resident PCM arena.
#
# THE FOURTH TEST DIR IN THIS TREE TO OWN ITS WIRING, and for test/18ttywrite/'s
# reason exactly: it compiles a translation unit that is deliberately NOT in the
# library. src/bin/sfx.cpp produces `float`, which AGENTS.md rule 2 keeps out of
# the simulation, and it owns no storage — so it cannot live in gloam::lib and
# the auto-discovery branch cannot reach it.
#
# That this file is possible at all is the argument for the three-way split in
# src/bin/CMakeLists.txt. sfx.cpp includes no RtAudio, so this target builds and
# runs on a machine with no sound card — which is every machine this project
# builds on — and therefore the synthesiser is covered by all eight sanitizer
# legs rather than by none of them.
#
# Everything test/15voicering/CMakeLists.txt documents about the parent loop
# applies verbatim: the target name is load-bearing because add_test runs after
# this file, the `runners` fixture is attached by the parent, and main.cpp, both
# include dirs, Catch2 and the library are this file's business.

add_executable(27sfxarena-test
  ${CMAKE_CURRENT_LIST_DIR}/../main.cpp
  ${PROJECT_SOURCE_DIR}/src/bin/sfx.cpp
  ${SRCS}
)

# A target created in a subdirectory lands in build/test/<dir>/ instead of
# build/test/, which ctest does not care about and a human running the binary by
# hand very much does. Put it where every sibling is.
set_target_properties(27sfxarena-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/..
)

# src/bin is on the path so the test can include "sfx.hpp" exactly as main.cpp
# does. It is NOT a public include directory of anything.
target_include_directories(27sfxarena-test
  PUBLIC ${PROJECT_SOURCE_DIR}/include
  PUBLIC ${PROJECT_SOURCE_DIR}/src/bin
  PUBLIC ${CMAKE_CURRENT_LIST_DIR}/../include
)

target_link_libraries(27sfxarena-test
  PRIVATE Catch2::Catch2
)

# Guarded like the parent loop guards it. The false branch is unreachable in this
# repo; keeping the spellings identical makes a future change to one an obvious
# omission in the others.
if (TARGET ${PROJECT_NAME}::lib)
  target_link_libraries(27sfxarena-test
    PRIVATE ${PROJECT_NAME}::lib
  )
endif ()
