# SPEC §9.2 — the real-time callback's body, tested with no device.
#
# THE FIFTH TEST DIR IN THIS TREE TO OWN ITS WIRING, and it needs both of the
# reasons its predecessors have one. Like test/18ttywrite/, it compiles a
# translation unit that cannot live in gloam::lib (src/bin/voice_mixer.cpp and
# src/bin/sfx.cpp are float, and rule 2 keeps float out of the simulation). Like
# test/15voicering/, it starts a thread — the producer/consumer case is the only
# place in this suite where TSan can audit the mixer against a real producer.
#
# WHAT THIS FILE'S EXISTENCE PROVES ABOUT THE DESIGN
#
# `mix::Mixer::render` takes the ring, the output buffer, the frame count and the
# CURRENT TIME as parameters, and owns no thread. That is what lets this target
# call it on the main thread, on a machine with no sound card, under all eight
# sanitizer legs. If the mixer had opened its own stream or read its own clock,
# none of the twenty cases in test.cpp could exist and §9.2's three
# prohibitions would be comments.
#
# It deliberately does NOT compile src/bin/audio_device.cpp. That TU links
# RtAudio, and test.cpp replaces global operator new to prove `render` allocates
# nothing — a claim that would be measuring RtAudio's allocations too if the two
# shared a binary.
#
# Everything test/15voicering/CMakeLists.txt documents about the parent loop
# applies verbatim; the target name is load-bearing because add_test runs after
# this file.

find_package(Threads REQUIRED)

add_executable(28voicemix-test
  ${CMAKE_CURRENT_LIST_DIR}/../main.cpp
  ${PROJECT_SOURCE_DIR}/src/bin/voice_mixer.cpp
  ${PROJECT_SOURCE_DIR}/src/bin/sfx.cpp
  ${SRCS}
)

set_target_properties(28voicemix-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/..
)

target_include_directories(28voicemix-test
  PUBLIC ${PROJECT_SOURCE_DIR}/include
  PUBLIC ${PROJECT_SOURCE_DIR}/src/bin
  PUBLIC ${CMAKE_CURRENT_LIST_DIR}/../include
)

target_link_libraries(28voicemix-test
  PRIVATE Catch2::Catch2
  PRIVATE Threads::Threads
)

if (TARGET ${PROJECT_NAME}::lib)
  target_link_libraries(28voicemix-test
    PRIVATE ${PROJECT_NAME}::lib
  )
endif ()
