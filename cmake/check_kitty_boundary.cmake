# SPEC §19 step 2 — "Every escape sequence GLOAM emits goes through one module."
#
# §16's mitigation for the project's highest-severity risk is that "a vendored
# driver is a swap and not a rewrite". That only holds if there is exactly one
# file to swap. Without this check the rule is a convention, and conventions
# decay — the first compositor commit that emits a placement from somewhere else
# would be a code review away from landing.
#
# Scope is the SHIPPED code — include/ and src/. test/07emit/ asserts emitted
# bytes and therefore names the introducer on purpose, and this file necessarily
# contains it too. Counting either would make the check assert its own text.
# design/ is out of scope: it is the vendored specification, not application code.
#
# Run standalone:  cmake -P cmake/check_kitty_boundary.cmake

cmake_minimum_required(VERSION 3.28)

if (NOT DEFINED PROJECT_ROOT)
  get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif ()

# The kitty graphics protocol's Application Programming Command introducer, in
# the two C++ source spellings it can plausibly be written in. Searching for the
# bare two-character suffix would false-positive on any identifier ending in it,
# so the escape byte has to be part of the pattern — which means checking both
# the octal and hex forms rather than picking one and hoping.
#
# The emitter must therefore spell the introducer as one of these two. It uses
# the octal form, because a hex escape followed by a hex digit is a different
# character and the octal form cannot swallow what follows it.
set(INTRODUCERS "\\033_G" "\\x1b_G")
set(CANONICAL_HOME "src/lib/kitty.cpp")

file(GLOB_RECURSE SHIPPED_SOURCES
  "${PROJECT_ROOT}/include/*.hpp"
  "${PROJECT_ROOT}/include/*.cpp"
  "${PROJECT_ROOT}/src/*.hpp"
  "${PROJECT_ROOT}/src/*.cpp"
)

set(HITS "")
foreach (SOURCE ${SHIPPED_SOURCES})
  file(READ "${SOURCE}" CONTENT)
  foreach (INTRODUCER ${INTRODUCERS})
    string(FIND "${CONTENT}" "${INTRODUCER}" FOUND)
    if (NOT FOUND EQUAL -1)
      file(RELATIVE_PATH REL "${PROJECT_ROOT}" "${SOURCE}")
      list(APPEND HITS "${REL}")
      break ()
    endif ()
  endforeach ()
endforeach ()

list(REMOVE_DUPLICATES HITS)
list(LENGTH HITS HIT_COUNT)

if (HIT_COUNT EQUAL 0)
  message(FATAL_ERROR
    "check_kitty_boundary: no kitty APC introducer was found in the shipped tree at all. "
    "It should appear exactly once, in ${CANONICAL_HOME}. If the emitter now spells it some "
    "other way, add that spelling to INTRODUCERS in cmake/check_kitty_boundary.cmake — a "
    "check that matches nothing is worse than no check.")
endif ()

if (NOT HIT_COUNT EQUAL 1)
  string(REPLACE ";" "\n  " HIT_LIST "${HITS}")
  message(FATAL_ERROR
    "check_kitty_boundary: SPEC §19 step 2 requires every escape sequence to go through ONE "
    "module. Found the kitty APC introducer in ${HIT_COUNT} files:\n  ${HIT_LIST}\n"
    "Construct the command in ${CANONICAL_HOME} and call it from there — §16's \"a vendored "
    "driver is a swap and not a rewrite\" is only true while there is one file to swap.")
endif ()

if (NOT HITS STREQUAL CANONICAL_HOME)
  message(FATAL_ERROR
    "check_kitty_boundary: the introducer appears once, but in ${HITS} rather than "
    "${CANONICAL_HOME}. That is probably fine — but update CANONICAL_HOME deliberately "
    "rather than letting the boundary drift.")
endif ()

message(STATUS "check_kitty_boundary: CLEAN — the kitty APC introducer appears once, in ${CANONICAL_HOME}")
