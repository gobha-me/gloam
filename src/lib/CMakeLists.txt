# ── Library target ──────────────────────────────────────────────────────────
# The CMake starter this project came from ships the library behind a
# <PROJECT>_BUILD_LIB option, because there the executable is independent of the
# library and "application only" is a real configuration.
#
# GLOAM DELIBERATELY DROPS THAT OPTION. The simulation core is not an optional
# component of the game — src/bin/ is a thin consumer of it, and a build with
# the library switched off has no application left to produce, only a link
# error. A knob that cannot work is worse than no knob, so the library is
# unconditional here.
#
# Two consequences, both intentional:
#   * the starter's "library disabled" CI job was removed with the option, since
#     there is nothing left for it to assert;
#   * test/CMakeLists.txt's `if (TARGET ${PROJECT_NAME}::lib)` guards are now
#     always true. They are kept as inherited defensive machinery, not because
#     the false branch is reachable.
#
# The header-only (INTERFACE) variant the starter documents as an alternative is
# not applicable and has been removed: the core has real translation units, and
# `version_string()` being declared here and defined in one of them is what makes
# example/consumer/ a link test rather than a compile test.

# The deterministic core (SPEC §5-§8, §12). Standard library only, by design:
# see the note at the top of include/gloam/gloam.hpp. Nothing here may grow a
# dependency on termforge, a clock or a terminal — that is the property the
# whole replay story rests on.
#
# The offline asset pipeline (§10) is here too — plate.cpp, dither (header-only),
# lightfield.cpp, pack.cpp and sha256.cpp. Same argument as kitty.cpp: producing
# bytes is not needing a terminal, and none of them reaches a clock, a file
# descriptor or a global. The buffers they write into are owned by their caller,
# and the only `open` in the pipeline is in src/bin/bake.cpp.
#
# audio.cpp is here on the same argument once more, and it is the one most worth
# stating because the file's name argues against it. §9's SINK is a device and
# lives in src/bin/; what is here is the SPSC ring, the command type and the
# gain/pan derivation — integers, atomics and a call into propagate_noise, with
# no RtAudio, no clock and no stream. Producing a voice command is not needing a
# device; playing it is. Keeping the ring on this side is also what puts it under
# the sanitizer matrix, which is the only automated auditor a lock-free queue
# ever gets.
add_library(${PROJECT_NAME}_lib STATIC
  assets.cpp
  audio.cpp
  gloam.cpp
  kitty.cpp
  level.cpp
  lightfield.cpp
  meter.cpp
  noise.cpp
  pack.cpp
  path.cpp
  perception.cpp
  plate.cpp
  replay.cpp
  runes.cpp
  sha256.cpp
  spells.cpp
  world.cpp
)

# Compiled libs get the project's include dir PUBLIC so consumers (bin, tests,
# downstream) inherit it transitively.
#
# The two generator expressions are not decoration. PROJECT_SOURCE_DIR is this
# project's root; CMAKE_SOURCE_DIR would be the *consumer's* root the moment
# someone add_subdirectory()s us, silently pointing the include path at the
# wrong tree. And a bare absolute source path in a PUBLIC include directory
# makes install(TARGETS ... EXPORT ...) fail outright — an installed package
# must describe where its headers landed, not where they were built from.
target_include_directories(${PROJECT_NAME}_lib
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

# The C++ standard is a property of this library's public headers, not of the
# build that happens to be compiling it. cmake/toolchain/default.cmake sets
# CMAKE_CXX_STANDARD for *our* builds, but a consumer uses their own toolchain —
# without this, they compile include/gloam/*.hpp at whatever their default is.
target_compile_features(${PROJECT_NAME}_lib PUBLIC cxx_std_23)

add_library(${PROJECT_NAME}::lib ALIAS ${PROJECT_NAME}_lib)
