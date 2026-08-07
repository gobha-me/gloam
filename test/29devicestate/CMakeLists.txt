# SPEC §9.2 — the device lifecycle, with no device attached.
#
# `audio_device.cpp` is the one RtAudio translation unit and cannot be driven in
# CI past its no-device branch.  The state signal it uses lives in the
# RtAudio-free header so this target can put the disconnect/start race under the
# same sanitizer matrix as the ring and mixer.  This directory owns its wiring
# because `src/bin/` is not a public include directory.

find_package(Threads REQUIRED)

add_executable(29devicestate-test
  ${CMAKE_CURRENT_LIST_DIR}/../main.cpp
  ${SRCS}
)

set_target_properties(29devicestate-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/..
)

target_include_directories(29devicestate-test
  PUBLIC ${PROJECT_SOURCE_DIR}/include
  PUBLIC ${PROJECT_SOURCE_DIR}/src/bin
  PUBLIC ${CMAKE_CURRENT_LIST_DIR}/../include
)

target_link_libraries(29devicestate-test
  PRIVATE Catch2::Catch2
  PRIVATE Threads::Threads
)

if (TARGET ${PROJECT_NAME}::lib)
  target_link_libraries(29devicestate-test
    PRIVATE ${PROJECT_NAME}::lib
  )
endif ()
