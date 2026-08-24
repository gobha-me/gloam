# SPEC 15 (M0), gloam#8 — the corridor slice's scene and game core.
#
# THE FIFTH TEST DIR IN THIS TREE TO OWN ITS WIRING, and for test/27sfxarena/'s
# reason exactly: it compiles translation units that are deliberately NOT in the
# library. src/bin/scene.cpp and src/bin/game.cpp are content and driver policy
# for a binary that does not exist on a consumer's machine — they cannot live in
# gloam::lib, and the auto-discovery branch cannot reach src/bin/.
#
# Both files are device-free by construction — no termforge, no RtAudio, no
# clock, no fd — so this target builds and runs on a machine with no terminal
# and no sound card, which is every machine this project builds on, and the M0
# scene and both pump policies stay under all eight sanitizer legs.
#
# Everything test/15voicering/CMakeLists.txt documents about the parent loop
# applies verbatim: the target name is load-bearing because add_test runs after
# this file, the `runners` fixture is attached by the parent, and main.cpp, both
# include dirs, Catch2 and the library are this file's business.

add_executable(34gamecore-test
  ${CMAKE_CURRENT_LIST_DIR}/../main.cpp
  ${PROJECT_SOURCE_DIR}/src/bin/scene.cpp
  ${PROJECT_SOURCE_DIR}/src/bin/game.cpp
  ${SRCS}
)

# A target created in a subdirectory lands in build/test/<dir>/ instead of
# build/test/, which ctest does not care about and a human running the binary by
# hand very much does. Put it where every sibling is.
set_target_properties(34gamecore-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/..
)

# src/bin is on the path so the test can include "scene.hpp" and "game.hpp"
# exactly as the shell will. It is NOT a public include directory of anything.
target_include_directories(34gamecore-test
  PUBLIC ${PROJECT_SOURCE_DIR}/include
  PUBLIC ${PROJECT_SOURCE_DIR}/src/bin
  PUBLIC ${CMAKE_CURRENT_LIST_DIR}/../include
)

target_link_libraries(34gamecore-test
  PRIVATE Catch2::Catch2
)

# Guarded like the parent loop guards it. The false branch is unreachable in this
# repo; keeping the spellings identical makes a future change to one an obvious
# omission in the others.
if (TARGET ${PROJECT_NAME}::lib)
  target_link_libraries(34gamecore-test
    PRIVATE ${PROJECT_NAME}::lib
  )
endif ()
