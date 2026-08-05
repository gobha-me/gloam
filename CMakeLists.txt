cmake_minimum_required(VERSION 3.28)

# Toolchain defaulting, top-level only. A toolchain file has to be set before the
# first project() call in the whole build, so this cannot use PROJECT_IS_TOP_LEVEL
# — that variable does not exist until project() runs, a dozen lines down. This is
# the same test, spelled the only way that works this early.
#
# When this project is consumed (add_subdirectory / FetchContent) the consumer
# owns the toolchain, and setting one here would be dead weight at best.
if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR AND NOT CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE ${CMAKE_CURRENT_LIST_DIR}/cmake/toolchain/default.cmake)
endif()

include(${CMAKE_CURRENT_LIST_DIR}/cmake/version.cmake)

# https://stackoverflow.com/questions/12344368/automatically-use-the-directory-as-the-project-name-in-cmake
get_filename_component(ProjectId ${CMAKE_CURRENT_LIST_DIR} NAME)
string(REPLACE " " "_" ProjectId ${ProjectId})

project(${ProjectId}
  VERSION ${VERSION}
  LANGUAGES CXX
)

# CMAKE_INSTALL_{INCLUDE,LIB,BIN}DIR, used by src/lib and cmake/install.cmake.
include(GNUInstallDirs)

# PROJECT_VERSION, not CMAKE_PROJECT_VERSION: the CMAKE_ prefixed variable holds
# the *top-level* project's version, which is someone else's when we are consumed.
message(STATUS "${PROJECT_NAME}:${PROJECT_VERSION} (tweak=${VERSION_TWEAK} dirty=${VERSION_DIRTY} top_level=${PROJECT_IS_TOP_LEVEL})")

set (VERSION_HEADER_IN "${CMAKE_CURRENT_LIST_DIR}/include/version.hpp.in.cmake")

if (EXISTS ${VERSION_HEADER_IN})
  string (REPLACE ".hpp.in.cmake" ".hpp" VERSION_HEADER ${VERSION_HEADER_IN})
  message(STATUS "${VERSION_HEADER_IN} ${VERSION_HEADER}")
  configure_file (${VERSION_HEADER_IN} ${VERSION_HEADER})
endif ()

# ── What gets built ─────────────────────────────────────────────────────────
# Everything except the library defaults to PROJECT_IS_TOP_LEVEL, so a project
# that consumes this one via add_subdirectory() or FetchContent gets the library
# and nothing else — no demo executable, no test suite, and no install rules
# landing in someone else's `cmake --install`. Building this repo directly is
# unchanged: all three are ON.
#
# The library itself defaults ON in src/lib/CMakeLists.txt — it is the thing
# worth consuming.
#
# Declared here, above the dependency list, because that list is gated on them.
option(${PROJECT_NAME}_BUILD_BIN "Build the executable in src/bin"    ${PROJECT_IS_TOP_LEVEL})
option(${PROJECT_NAME}_TESTS     "Build and register the test suite"  ${PROJECT_IS_TOP_LEVEL})
option(${PROJECT_NAME}_INSTALL   "Generate install / package-config rules" ${PROJECT_IS_TOP_LEVEL})

# Dependencies this project uses. Each name maps to cmake/deps/<name>.cmake
# (find_package first, FetchContent fallback). This is the prune point for a new
# project: drop a name to stop fetching that dep.
# GLOAM's own dependency floor is deliberately at the standard library, matching
# termforge's stated policy. The simulation core links nothing at all — neither
# name below reaches ${PROJECT_NAME}::lib, and that is a hard architectural
# boundary rather than a coincidence (AGENTS.md rule 1).
#
# One dep is still coming and is NOT here yet, on purpose:
#   termforge — the terminal layer, once the compositor has an API to sit on
#               (upstream #83 and the GL-A residency epic; see UPSTREAM.md).
set(${PROJECT_NAME}_DEPS
  catch2    # Catch2 v3 — test framework (test/)
  rtaudio   # SPEC §9 — the output stream (src/bin/). ⚠ It lives in GLOAM
            # PERMANENTLY and must NEVER be pushed upstream: termforge ships
            # standard library only, and RtAudio would break that promise
            # (§9.1). Exactly one translation unit includes it, enforced by the
            # ctest case `rtaudio-boundary-single-module`.
)

# Component gating: a dependency that exists only to serve a target we are not
# building does not get fetched. Without this, consuming the library alone still
# downloads a test framework and two application libraries.
#
# The list above stays the single prune point — these lines only ever REMOVE
# names from it, never add one. Removing a name that is already gone is a no-op,
# so a project that prunes the list can leave these alone.
if (NOT ${PROJECT_NAME}_TESTS)
  list(REMOVE_ITEM ${PROJECT_NAME}_DEPS catch2)
endif ()

# RtAudio serves src/bin/ alone — it is linked into no library and exported from
# no target set. This prune is what keeps example/consumer/ and
# example/public-dep/ from fetching an audio library to build a project that has
# no audio in it: all three consumer modes have GLOAM non-top-level, so BUILD_BIN
# defaults OFF, and mode 3 passes -D<project>_BUILD_BIN=OFF explicitly.
#
# termforge lands beside it the moment the terminal layer starts, which is why
# that line is kept commented rather than deleted.
if (NOT ${PROJECT_NAME}_BUILD_BIN)
  list(REMOVE_ITEM ${PROJECT_NAME}_DEPS rtaudio)
  # list(REMOVE_ITEM ${PROJECT_NAME}_DEPS termforge)
endif ()

include(${CMAKE_CURRENT_LIST_DIR}/cmake/dependencies.cmake)

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/src/lib)

if (${PROJECT_NAME}_BUILD_BIN)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/src/bin)
endif ()

if (${PROJECT_NAME}_TESTS)
  include(CTest)
  enable_testing()

  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/test)
endif (${PROJECT_NAME}_TESTS)

# Install rules, package config and the exported target set. Replaces the
# long-standing "TODO Install Template": find_package(<project> CONFIG) is real
# now, and works against an install tree.
if (${PROJECT_NAME}_INSTALL)
  include(${CMAKE_CURRENT_LIST_DIR}/cmake/install.cmake)
endif ()
