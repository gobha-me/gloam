# ── RtAudio — SPEC §9's output stream ────────────────────────────────────────
#
# ⚠ THIS DEPENDENCY MUST NEVER BE FILED UPSTREAM (§9.1). termforge ships standard
# library only; an audio subsystem cannot land there without breaking the
# library's central promise. GLOAM owns audio permanently, the two never learn
# about each other, and `src/bin/audio_device.cpp` is the ONLY translation unit
# in this tree that includes RtAudio's header — `rtaudio-boundary-single-module`
# is the ctest case that keeps it that way.
#
# This recipe deviates from cmake/deps/catch2.cmake's canonical shape in two
# places, and both deviations are defects in RtAudio's build, not preferences.
#
# ── Deviation 1: find_package() carries NO version argument ──────────────────
# RtAudio's CMakeLists declares `project(RtAudio LANGUAGES CXX)` with no VERSION
# and then writes its package version file from the LIBTOOL ABI TRIPLE:
#
#     write_basic_package_version_file(... VERSION ${FULL_VER}
#                                          COMPATIBILITY AnyNewerVersion)
#
# where FULL_VER is computed from configure.ac's lt_current/lt_revision/lt_age.
# Release 5.2.0 advertises 6.0.2; release 6.0.1 advertises 7.0.0. Measured on
# this project's dev box: /usr/include/rtaudio/RtAudio.h says
# `#define RTAUDIO_VERSION "5.2.0"` and the library beside it is
# librtaudio.so.6.0.2.
#
# So `find_package(RtAudio 6 ...)` ACCEPTS 5.2.0 and rejects nothing. A version
# argument there is worse than no argument, because it looks like a gate. The
# distinction matters: pre-6 openStream() THROWS where audio_device.cpp checks a
# returned RtAudioErrorType, so a silently accepted 5.x fails deep inside a
# translation unit nobody would connect back to this file.
#
# The reliable discriminator is RTAUDIO_VERSION_MAJOR, which 6.x defines and
# 5.2.0 does not define at all. It is checked twice on purpose — here, so a
# pre-6 system copy falls through to the pinned fallback rather than failing the
# build, and again as an #error in audio_device.cpp, so the guarantee survives
# someone bypassing this recipe.
#
# ── Deviation 2: we take RtAudio's SOURCES and not its CMakeLists ────────────
# RtAudio 6.0.1 writes its package config into ${CMAKE_CURRENT_BINARY_DIR} and
# then installs it from ${CMAKE_BINARY_DIR}:
#
#     configure_package_config_file(... ${CMAKE_CURRENT_BINARY_DIR}/RtAudioConfig.cmake ...)
#     install(FILES ${CMAKE_BINARY_DIR}/RtAudioConfig.cmake ... DESTINATION ...)
#
# Standalone those are the same directory. Under FetchContent they are not: the
# files land in _deps/rtaudio-build/ and the install rule points at GLOAM's
# top-level build dir, so `cmake --install` fails outright. And there is no
# RTAUDIO_INSTALL option to switch off — step 3 of catch2.cmake has no lever to
# pull here, because every install() in that file is unconditional. GLOAM's own
# _INSTALL defaults to PROJECT_IS_TOP_LEVEL, so an ordinary dev build is exactly
# the configuration that breaks.
#
# The rejected alternative was to consume the CMakeLists anyway and repair it
# from outside — file(COPY) the two config files into ${CMAKE_BINARY_DIR} to
# stop the failure, and accept that RtAudio's headers, static library, .pc file
# and package config land in GLOAM's prefix. That is precisely the leak
# example/public-dep/ exists to assert against, so it was not taken.
#
# What we do instead costs one compile line. RtAudio is a single translation
# unit; this tree already owns harder ones (src/lib/deflate.cpp is a DEFLATE
# implementation). SOURCE_SUBDIR below names a directory that contains no
# CMakeLists.txt, which makes FetchContent_MakeAvailable download the sources
# and skip add_subdirectory() entirely — the documented way to say "I want the
# code, not the build".
#
# The cost: this branch is Linux-only, and GLOAM owns a two-line compile recipe
# it must re-check on every pin bump. src/bin/tty_writer.hpp already carries the
# precedent — "This is the first non-portable file in the tree, which is the
# correct place for it to be" — and audio_device.cpp is its sibling.
#
# Bump the pin deliberately and say why in the commit (see AGENTS.md).

find_package(RtAudio CONFIG QUIET)

# Deviation 1, enforced. A found package proves nothing about the API until this
# compiles, so treat a pre-6 copy as a miss rather than as an error: the pinned
# fallback below is a perfectly good answer, and failing the build over a distro
# package the developer never asked for would not be.
if (RtAudio_FOUND)
  include(CMakePushCheckState)
  include(CheckCXXSourceCompiles)

  cmake_push_check_state(RESET)
  set(CMAKE_REQUIRED_LIBRARIES RtAudio::rtaudio)
  check_cxx_source_compiles("
    #include <RtAudio.h>
    #if !defined(RTAUDIO_VERSION_MAJOR) || RTAUDIO_VERSION_MAJOR < 6
    #error pre-6 RtAudio: openStream throws instead of returning RtAudioErrorType
    #endif
    int main() { RtAudio r; return static_cast<int>(r.getDeviceIds().size()); }
  " GLOAM_RTAUDIO_IS_6)
  cmake_pop_check_state()

  if (NOT GLOAM_RTAUDIO_IS_6)
    message(STATUS
      "RtAudio found but pre-6 (throwing API; its package version is a libtool "
      "ABI triple, so no find_package version argument can reject it) — using "
      "the pinned fallback instead")
    set(RtAudio_FOUND FALSE)
  endif ()
endif ()

if (RtAudio_FOUND)
else ()
  if (NOT RtAudio_URI)
    set(RtAudio_URI https://github.com/thestk/rtaudio.git)
  endif()

  if (NOT RtAudio_TAG)
    set(RtAudio_TAG 6.0.1)
  endif()

  include(FetchContent)
  FetchContent_Declare(
    RtAudio
    GIT_REPOSITORY ${RtAudio_URI}
    GIT_TAG ${RtAudio_TAG}
    GIT_SHALLOW TRUE
    # Deviation 2. This directory does not exist in RtAudio's tree, which is the
    # point: with no CMakeLists.txt to find, MakeAvailable populates the source
    # and does not add_subdirectory() it. Renaming this to a real path would
    # silently re-enable RtAudio's install rules and break `cmake --install`.
    SOURCE_SUBDIR gloam-does-not-build-rtaudios-cmakelists
  )

  FetchContent_MakeAvailable(RtAudio)

  find_package(ALSA REQUIRED)
  find_package(Threads REQUIRED)

  add_library(rtaudio STATIC ${rtaudio_SOURCE_DIR}/RtAudio.cpp)
  add_library(RtAudio::rtaudio ALIAS rtaudio)

  # SYSTEM covers what GLOAM's own translation units see when they include
  # RtAudio.h — warnings from a third-party header we will never act on bury the
  # ones we would.
  target_include_directories(rtaudio SYSTEM PUBLIC ${rtaudio_SOURCE_DIR})

  # SYSTEM does NOT cover compiling RtAudio.cpp itself; that inherits the
  # toolchain's global -Wall -Wextra -pedantic, and RtAudio 6.0.1 trips -Wvla
  # twice in its ALSA backend. Measured, not assumed — those two are the entire
  # warning output of this target under GCC 14. There is no -Werror in this
  # tree, so this is legibility rather than breakage; it is scoped to this
  # target so it cannot mask a warning in any file GLOAM wrote.
  target_compile_options(rtaudio PRIVATE -w)

  # ALSA ONLY. This dev box has libasound2-dev, libpulse-dev and libjack-dev all
  # installed, so RtAudio's own probes would otherwise link three backends, and
  # `audio-no-device-degrades` would be enumerating three ways to have no sound
  # card instead of one.
  #
  # Measured rather than assumed, on a box with no /dev/snd: `gloam --audio
  # --quiet` takes 89-108 ms over five runs against 84-91 ms for `--mute`, so
  # enumerating a device that is not there costs single-digit milliseconds and
  # the no-device ctest case is a fast pass rather than a timeout.
  #
  # One nuance worth writing down, because the obvious reading of the line above
  # is wrong: switching RtAudio's PulseAudio backend off does NOT stop a
  # PulseAudio connection being attempted. alsa-lib's own `default` PCM is
  # routed through a pulse plugin on this distro, so opening it prints
  # "PulseAudio: Unable to connect: Connection refused" — from libasound, on
  # stderr, before RtAudio has an opinion. Refused is instant; a host where a
  # pulse server is unreachable but not refusing is the case that could stall,
  # and it is not one that exists in CI. GLOAM's own report goes to stdout and
  # is unaffected either way, which is what the ctest case asserts on.
  target_compile_definitions(rtaudio PRIVATE __LINUX_ALSA__)
  target_link_libraries(rtaudio PUBLIC ALSA::ALSA Threads::Threads)

  # RtAudio is a C++11 codebase. GLOAM's toolchain sets CMAKE_CXX_STANDARD 23
  # globally, and compiling someone else's C++11 under C++23 rules is a way to
  # inherit their deprecations as our build failures.
  set_target_properties(rtaudio PROPERTIES
    CXX_STANDARD 11
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
  )
endif()
