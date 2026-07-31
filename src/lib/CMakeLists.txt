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
add_library(${PROJECT_NAME}_lib STATIC
  gloam.cpp
  kitty.cpp
  level.cpp
  noise.cpp
  perception.cpp
  runes.cpp
  spells.cpp
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
