# SPEC §4.6-§4.8 — binary-side compositor and animation queue.

add_executable(33terminalcompositor-test
  ${CMAKE_CURRENT_LIST_DIR}/../main.cpp
  ${PROJECT_SOURCE_DIR}/src/bin/resident_plates.cpp
  ${PROJECT_SOURCE_DIR}/src/bin/terminal_compositor.cpp
  ${SRCS}
)

set_target_properties(33terminalcompositor-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/..
)

target_include_directories(33terminalcompositor-test
  PUBLIC ${PROJECT_SOURCE_DIR}/include
  PUBLIC ${PROJECT_SOURCE_DIR}/src/bin
  PUBLIC ${CMAKE_CURRENT_LIST_DIR}/../include
)

target_link_libraries(33terminalcompositor-test
  PRIVATE Catch2::Catch2
  PRIVATE termforge::lib
)

if (TARGET ${PROJECT_NAME}::lib)
  target_link_libraries(33terminalcompositor-test PRIVATE ${PROJECT_NAME}::lib)
endif ()
