# SPEC §4.8 — the binary-side resident plate owner over a real pty.
#
# Custom wiring is required for the same reason as test/18ttywrite: all three
# implementation files are deliberately outside gloam::lib. This target is the
# only other consumer of them, putting the terminal boundary, PNG-to-pin owner
# and rollback paths under every sanitizer leg without making termforge a
# dependency of the deterministic core.

find_package(Threads REQUIRED)
find_library(GLOAM_UTIL_LIBRARY util REQUIRED)

add_executable(31imagelifecycle-test
  ${CMAKE_CURRENT_LIST_DIR}/../main.cpp
  ${PROJECT_SOURCE_DIR}/src/bin/tty_writer.cpp
  ${PROJECT_SOURCE_DIR}/src/bin/terminal_sink.cpp
  ${PROJECT_SOURCE_DIR}/src/bin/resident_plates.cpp
  ${SRCS}
)

set_target_properties(31imagelifecycle-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/..
)

target_include_directories(31imagelifecycle-test
  PUBLIC ${PROJECT_SOURCE_DIR}/include
  PUBLIC ${PROJECT_SOURCE_DIR}/src/bin
  PUBLIC ${CMAKE_CURRENT_LIST_DIR}/../include
)

target_link_libraries(31imagelifecycle-test
  PRIVATE Catch2::Catch2
  PRIVATE termforge::lib
  PRIVATE Threads::Threads
  PRIVATE ${GLOAM_UTIL_LIBRARY}
)

if (TARGET ${PROJECT_NAME}::lib)
  target_link_libraries(31imagelifecycle-test
    PRIVATE ${PROJECT_NAME}::lib
  )
endif ()
