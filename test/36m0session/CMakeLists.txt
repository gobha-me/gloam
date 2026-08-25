# gloam#8 — the playable corridor, end to end.
#
# THE SIXTH TEST DIR TO OWN ITS WIRING, and the reason is exec, not devices:
# this suite runs the REAL gloam_m0 binary on a real pty — a cooperating peer
# that answers termforge's capability probe and every kitty pin query — plays
# a scripted session through the actual input path, and then demands that the
# replay the binary sealed passes the load gate and reproduces the world hash
# a fresh scene world reaches under replay::play.
#
# What that pins, end to end and across a process boundary: pack verification
# at launch, the pin path against a live pty, key decode through the terminal,
# both pump modes and the Tab toggle between them, the seal, the file write —
# and the one property that makes §5.1's "bug reports arrive as playable
# artifacts" true of the playable binary rather than only of the harness.
#
# It is POSIX-only by the same licence as test/31imagelifecycle's pty work.

add_executable(36m0session-test
  ${CMAKE_CURRENT_LIST_DIR}/../main.cpp
  ${PROJECT_SOURCE_DIR}/src/bin/scene.cpp
  ${PROJECT_SOURCE_DIR}/src/bin/tty_writer.cpp
  ${SRCS}
)

set_target_properties(36m0session-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/..
)

target_include_directories(36m0session-test
  PUBLIC ${PROJECT_SOURCE_DIR}/include
  PUBLIC ${PROJECT_SOURCE_DIR}/src/bin
  PUBLIC ${CMAKE_CURRENT_LIST_DIR}/../include
)

# The test execs the built binary, so the binary is a build-order dependency,
# not just a configure-time path.
if (TARGET ${PROJECT_NAME}_m0)
  target_compile_definitions(36m0session-test PRIVATE
    GLOAM_M0_PATH="$<TARGET_FILE:${PROJECT_NAME}_m0>"
  )
  add_dependencies(36m0session-test ${PROJECT_NAME}_m0)
else ()
  # Configure keeps working without the binary; the test refuses at runtime
  # with a message rather than failing to compile in a -DBUILD_BIN=OFF tree.
  target_compile_definitions(36m0session-test PRIVATE GLOAM_M0_PATH="")
endif ()

target_link_libraries(36m0session-test
  PRIVATE Catch2::Catch2
)

if (TARGET ${PROJECT_NAME}::lib)
  target_link_libraries(36m0session-test
    PRIVATE ${PROJECT_NAME}::lib
  )
endif ()
