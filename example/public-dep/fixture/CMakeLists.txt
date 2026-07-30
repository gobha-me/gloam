cmake_minimum_required(VERSION 3.28)

# ── A stand-in for a real public dependency ─────────────────────────────────
# The smallest project that reproduces the shape which broke CT-15 (#29). Three
# properties are load-bearing; everything else is scaffolding:
#
#   1. It installs and exports — install(TARGETS ... EXPORT) + install(EXPORT).
#   2. It registers NO build export set. There is deliberately no export() call
#      anywhere in this file, and example/public-dep/verify.sh asserts that
#      before it runs, because the moment one appears the harness stops
#      reproducing the bug while still reporting success.
#   3. Its install option defaults ON regardless of whether it is top level, so
#      a consuming project that does not override it gets this project's files
#      deposited in its prefix. Widely-used dependencies really do this.
#
# Together (1) and (2) are the asymmetry the issue is about: install(EXPORT) on
# a dependent target succeeds, because CMake finds this project's install export
# set and rewrites the reference to pubdep::pubdep; the build-tree export has no
# equivalent to find and fails at generate time.
#
# INTERFACE rather than a compiled library because that is the shape of the
# dependency that reported this (a header-only HTTP client), and because the
# dependent's link entry is what trips the check — this target's type never
# enters into it.

project(pubdep VERSION 1.0.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

add_library(pubdep INTERFACE)
add_library(pubdep::pubdep ALIAS pubdep)

target_include_directories(pubdep
  INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

# Defaults ON even when this is a subproject — see property (3) above. The
# project consuming this one is expected to say otherwise, which is exactly what
# the recipe in example/public-dep/pubdep.cmake demonstrates.
option(PUBDEP_INSTALL "Generate install rules for pubdep" ON)

if (PUBDEP_INSTALL)
  install(TARGETS pubdep EXPORT pubdepTargets)

  install(EXPORT pubdepTargets
    FILE      pubdepTargets.cmake
    NAMESPACE pubdep::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pubdep
  )

  install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.hpp"
  )

  # Installed to the doc directory on purpose. CMAKE_INSTALL_DOCDIR is derived
  # from the project name that was current when GNUInstallDirs was first
  # included — the TOP-LEVEL one — so this file lands under the consuming
  # project's documentation rather than its own. The harness asserts on it.
  install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/README.md
    DESTINATION ${CMAKE_INSTALL_DOCDIR}
  )

  configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/pubdep-config.cmake.in
    ${PROJECT_BINARY_DIR}/pubdepConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pubdep
  )

  write_basic_package_version_file(
    ${PROJECT_BINARY_DIR}/pubdepConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
    ARCH_INDEPENDENT
  )

  install(FILES
      ${PROJECT_BINARY_DIR}/pubdepConfig.cmake
      ${PROJECT_BINARY_DIR}/pubdepConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pubdep
  )
endif ()
