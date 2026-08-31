# SPEC §9.2, §10 — the pack's audio arena: the launch gate and the load (gloam#23).
#
# THE EIGHTH TEST DIR IN THIS TREE TO OWN ITS WIRING, and it needs both of the
# device-free audio TUs: sfx.cpp (the synthesiser's integer stage, which the
# reference arena and the in-test bake both draw on) and pack_audio.cpp (the
# gate and the loader under test). Both are deliberately NOT in gloam::lib —
# they produce float (AGENTS.md rule 2) — so this target compiles them
# directly, the way test/27sfxarena/ compiles sfx.cpp, and the whole load path
# sits under every sanitizer leg on machines with no sound card.
#
# Everything test/15voicering/CMakeLists.txt documents about the parent loop
# applies verbatim here: the target name is load-bearing because add_test runs
# after this file, the `runners` fixture is attached by the parent, and
# main.cpp, both include dirs, Catch2 and the library are this file's business.

add_executable(37packaudio-test
  ${CMAKE_CURRENT_LIST_DIR}/../main.cpp
  ${PROJECT_SOURCE_DIR}/src/bin/sfx.cpp
  ${PROJECT_SOURCE_DIR}/src/bin/pack_audio.cpp
  ${SRCS}
)

# A target created in a subdirectory lands in build/test/<dir>/ instead of
# build/test/, which ctest does not care about and a human running the binary by
# hand very much does. Put it where every sibling is.
set_target_properties(37packaudio-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/..
)

# src/bin is on the path so the test can include "sfx.hpp" and "pack_audio.hpp"
# exactly as m0.cpp does. It is NOT a public include directory of anything.
target_include_directories(37packaudio-test
  PUBLIC ${PROJECT_SOURCE_DIR}/include
  PUBLIC ${PROJECT_SOURCE_DIR}/src/bin
  PUBLIC ${CMAKE_CURRENT_LIST_DIR}/../include
)

target_link_libraries(37packaudio-test
  PRIVATE Catch2::Catch2
)

# Guarded like the parent loop guards it, and like the siblings copy it. The
# false branch is unreachable in this repo; keeping the spellings identical
# makes a future change to one an obvious omission in the others.
if (TARGET ${PROJECT_NAME}::lib)
  target_link_libraries(37packaudio-test
    PRIVATE ${PROJECT_NAME}::lib
  )
endif ()
