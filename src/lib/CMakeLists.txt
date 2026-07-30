# ── Library target ──────────────────────────────────────────────────────────
# The template's opinionated answer to "how do I structure a library":
# a compiled static lib by default, with the header-only (INTERFACE) variant
# shown as a commented alternative below. Disable entirely with:
#   cmake -B build -D<PROJECT>_BUILD_LIB=OFF
# (consumers that only want the bin, or none of the lib, can opt out.)
#
# OFF removes the target outright, so nothing can link it — including the
# auto-discovered tests, which pick it up automatically when it exists (see
# test/CMakeLists.txt). Pair OFF with -D<PROJECT>_TESTS=OFF, or keep only tests
# that do not call into this library.

option(${PROJECT_NAME}_BUILD_LIB "Build the compiled library target" ON)

if (${PROJECT_NAME}_BUILD_LIB)
  # Default: a compiled static library from src/lib/*.cpp, public headers in
  # include/ (starting with lib.hpp). Add sources here as the project grows.
  # The deterministic core (SPEC §5-§8, §12). Standard library only, by design:
  # see the note at the top of include/gloam/gloam.hpp. Nothing here may grow a
  # dependency on termforge, a clock or a terminal.
  add_library(${PROJECT_NAME}_lib STATIC
    gloam.cpp
    level.cpp
    noise.cpp
    perception.cpp
    runes.cpp
    spells.cpp
  )

  # Compiled libs get the project's include dir PUBLIC so consumers (bin,
  # tests, downstream) inherit it transitively.
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

  # The C++ standard is a property of this library's public header, not of the
  # build that happens to be compiling it. cmake/toolchain/default.cmake sets
  # CMAKE_CXX_STANDARD for *our* builds, but a consumer uses their own toolchain
  # — without this, they compile include/lib.hpp at whatever their default is.
  target_compile_features(${PROJECT_NAME}_lib PUBLIC cxx_std_23)

  add_library(${PROJECT_NAME}::lib ALIAS ${PROJECT_NAME}_lib)
endif ()

# ── Alternative: header-only (INTERFACE) library ────────────────────────────
# For header-only projects (e.g. a single-include API client), drop the
# compiled target above and use an INTERFACE library instead — no .cpp, no
# archive, just include dirs (+ any usage requirements) propagated to
# consumers.
#
# ⚠ Switching to INTERFACE means there is no translation unit, so every function
# in the public header must become a definition rather than a declaration: mark
# each one `inline` and move its body into include/lib.hpp. Skip that and the
# header still compiles everywhere while nothing that calls it links — which is
# the whole reason src/lib/lib.cpp exists in the default variant.
#
# Everything else carries over unchanged: same target name, same alias, same
# usage requirements (INTERFACE instead of PUBLIC, since there is nothing to
# compile here), and cmake/install.cmake needs no edit — it detects the target
# type and installs either variant.
#
# Uncomment to use:
#
# add_library(${PROJECT_NAME}_lib INTERFACE)
# target_include_directories(${PROJECT_NAME}_lib
#   INTERFACE
#     $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
#     $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
# )
# target_compile_features(${PROJECT_NAME}_lib INTERFACE cxx_std_23)
# add_library(${PROJECT_NAME}::lib ALIAS ${PROJECT_NAME}_lib)
