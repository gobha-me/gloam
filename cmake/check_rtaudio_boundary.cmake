# SPEC §9.1, §9.3 — "RtAudio stays behind one translation unit."
#
# The sibling of check_kitty_boundary.cmake, and it exists for a sharper reason
# than that one. §9.1 is absolute about where audio lives:
#
#   > RtAudio is a third-party dependency, so an audio subsystem cannot land
#   > upstream without breaking the library's central promise. Do not file it.
#   > GLOAM owns audio; termforge stays audio-free; the two never learn about
#   > each other.
#
# and AGENTS.md rule 1 is absolute about what gloam::lib may link. Both of those
# are one #include away from being false at any time, and neither the compiler
# nor the test suite would say a word: adding <RtAudio.h> to a second file
# builds, links and passes. This check is the only thing that notices.
#
# It is also what makes the ARCHITECTURE claim true rather than aspirational.
# src/bin/sfx.cpp and src/bin/voice_mixer.cpp are RtAudio-free so that
# test/27sfxarena/ and test/28voicemix/ can compile them under all eight
# sanitizer legs with no sound card present. The day someone includes RtAudio in
# either, those suites stop being able to build and the entire float half of §9
# leaves the matrix — silently, unless this runs.
#
# Scope is the SHIPPED code — include/ and src/ — matching its sibling. test/ is
# out of scope for the same reason, and design/ is the vendored specification.
#
# Run standalone:  cmake -P cmake/check_rtaudio_boundary.cmake

cmake_minimum_required(VERSION 3.28)

if (NOT DEFINED PROJECT_ROOT)
  get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif ()

# The three spellings an include of RtAudio's header can take: the angled form
# this project uses, the quoted form, and the path-qualified form a distro
# install invites (/usr/include/rtaudio/RtAudio.h). Matching on the bare file
# name would false-positive on prose that merely mentions it — including this
# file, which is why the pattern always carries the include punctuation.
set(INCLUDES "<RtAudio.h>" "\"RtAudio.h\"" "<rtaudio/RtAudio.h>")
set(CANONICAL_HOME "src/bin/audio_device.cpp")

file(GLOB_RECURSE SHIPPED_SOURCES
  "${PROJECT_ROOT}/include/*.hpp"
  "${PROJECT_ROOT}/include/*.cpp"
  "${PROJECT_ROOT}/src/*.hpp"
  "${PROJECT_ROOT}/src/*.cpp"
)

set(HITS "")
foreach (SOURCE ${SHIPPED_SOURCES})
  file(READ "${SOURCE}" CONTENT)
  foreach (INCLUDE ${INCLUDES})
    string(FIND "${CONTENT}" "${INCLUDE}" FOUND)
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
    "check_rtaudio_boundary: RtAudio is not included anywhere in the shipped tree. It should "
    "appear exactly once, in ${CANONICAL_HOME}. If the device was removed, remove this check "
    "in the same commit; if the include is now spelled some other way, add that spelling to "
    "INCLUDES in cmake/check_rtaudio_boundary.cmake — a check that matches nothing is worse "
    "than no check.")
endif ()

if (NOT HIT_COUNT EQUAL 1)
  string(REPLACE ";" "\n  " HIT_LIST "${HITS}")
  message(FATAL_ERROR
    "check_rtaudio_boundary: SPEC §9.3 requires RtAudio to stay behind ONE translation unit. "
    "Found it included in ${HIT_COUNT} files:\n  ${HIT_LIST}\n"
    "Put the device call in ${CANONICAL_HOME} and expose it through a header that does not "
    "name RtAudio. Two consequences if this is ignored: AGENTS.md rule 1 stops being "
    "enforceable, and any src/bin/ file a test compiles directly (sfx.cpp, voice_mixer.cpp) "
    "becomes unbuildable outside the executable — taking the float half of §9 out of the "
    "sanitizer matrix.")
endif ()

if (NOT HITS STREQUAL CANONICAL_HOME)
  message(FATAL_ERROR
    "check_rtaudio_boundary: RtAudio is included once, but in ${HITS} rather than "
    "${CANONICAL_HOME}. That is probably fine — but update CANONICAL_HOME deliberately "
    "rather than letting the boundary drift.")
endif ()

message(STATUS "check_rtaudio_boundary: CLEAN — RtAudio is included once, in ${CANONICAL_HOME}")
