# SPEC §19 step 2 — "Accept: -1073741825 appears exactly once in the tree; a
# grep test enforces it."
#
# §4.5 requires the below-background z threshold to live in exactly one place,
# inside a named layer API, so that no application code hand-writes it. This is
# that enforcement.
#
# Scope is the SHIPPED code — include/ and src/. test/06layer/ names the
# literal on purpose (it asserts the constant's value, so that the constant and
# this check cannot drift apart), and this file necessarily contains it too.
# Counting either would make the check assert its own text.
#
# design/ is also out of scope: it is the vendored specification, and §4.5's own
# table states the threshold. A design document quoting the number it specifies
# is not application code hand-writing it.
#
# Run standalone:  cmake -P cmake/check_layer_z.cmake

cmake_minimum_required(VERSION 3.28)

if (NOT DEFINED PROJECT_ROOT)
  get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif ()

set(THRESHOLD "-1073741825")
set(CANONICAL_HOME "include/gloam/layer.hpp")

file(GLOB_RECURSE SHIPPED_SOURCES
  "${PROJECT_ROOT}/include/*.hpp"
  "${PROJECT_ROOT}/include/*.cpp"
  "${PROJECT_ROOT}/src/*.hpp"
  "${PROJECT_ROOT}/src/*.cpp"
)

set(HITS "")
foreach (SOURCE ${SHIPPED_SOURCES})
  file(READ "${SOURCE}" CONTENT)
  string(FIND "${CONTENT}" "${THRESHOLD}" FOUND)
  if (NOT FOUND EQUAL -1)
    file(RELATIVE_PATH REL "${PROJECT_ROOT}" "${SOURCE}")
    list(APPEND HITS "${REL}")
  endif ()
endforeach ()

list(LENGTH HITS HIT_COUNT)

if (HIT_COUNT EQUAL 0)
  message(FATAL_ERROR
    "check_layer_z: the below-background threshold ${THRESHOLD} was not found in the shipped "
    "tree at all. It should live exactly once, in ${CANONICAL_HOME}. If the layer API moved, "
    "update CANONICAL_HOME in cmake/check_layer_z.cmake.")
endif ()

if (NOT HIT_COUNT EQUAL 1)
  string(REPLACE ";" "\n  " HIT_LIST "${HITS}")
  message(FATAL_ERROR
    "check_layer_z: SPEC §4.5 requires ${THRESHOLD} to appear exactly once, inside the named "
    "layer API. Found ${HIT_COUNT} occurrences:\n  ${HIT_LIST}\n"
    "No application code should hand-write this literal — use gloam::budget::kBelowBackgroundZ.")
endif ()

if (NOT HITS STREQUAL CANONICAL_HOME)
  message(FATAL_ERROR
    "check_layer_z: ${THRESHOLD} appears once, but in ${HITS} rather than ${CANONICAL_HOME}. "
    "That is probably fine — but §4.5 wants it inside the named layer API, so update "
    "CANONICAL_HOME deliberately rather than letting it drift.")
endif ()

message(STATUS "check_layer_z: CLEAN — ${THRESHOLD} appears once, in ${CANONICAL_HOME}")
