# ── Install, export, and package config ─────────────────────────────────────
# This is the answer to the "TODO Install Template" the root CMakeLists carried
# for years, and the reason it matters: a project consuming this one should be
# able to write
#
#   find_package(<project> CONFIG REQUIRED)
#   target_link_libraries(app PRIVATE <project>::lib)
#
# and have it work against an installed prefix, with the *same* target spelling
# it would use via add_subdirectory() or FetchContent. One spelling, three
# acquisition modes. See example/consumer/ for all three, exercised.
#
# Included from the root CMakeLists behind ${PROJECT_NAME}_INSTALL, which
# defaults to PROJECT_IS_TOP_LEVEL: an embedded copy of this project must not
# inject rules into its consumer's `cmake --install`.
#
# Nothing here names the project literally. The package name is the project
# name, which is derived from the directory (see the root CMakeLists), so a fork
# copies this file verbatim.

include(CMakePackageConfigHelpers)

set(_cfg_install_dir ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME})

# ── The library ─────────────────────────────────────────────────────────────
if (TARGET ${PROJECT_NAME}_lib)
  # EXPORT_NAME is what makes the imported target read ${PROJECT_NAME}::lib
  # rather than ${PROJECT_NAME}::${PROJECT_NAME}_lib. Paired with NAMESPACE on
  # the install(EXPORT) below, a downstream `find_package` gets a target that is
  # spelled identically to the in-tree ALIAS in src/lib/CMakeLists.txt — so a
  # consumer can switch acquisition modes without touching its link lines.
  set_target_properties(${PROJECT_NAME}_lib PROPERTIES EXPORT_NAME lib)

  # One call covers both library variants. For the header-only (INTERFACE)
  # alternative the ARCHIVE/LIBRARY/RUNTIME destinations simply go unused —
  # there is no artifact to place — so switching src/lib/CMakeLists.txt needs no
  # edit here.
  install(TARGETS ${PROJECT_NAME}_lib
    EXPORT ${PROJECT_NAME}Targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  )

  install(EXPORT ${PROJECT_NAME}Targets
    FILE      ${PROJECT_NAME}Targets.cmake
    NAMESPACE ${PROJECT_NAME}::
    DESTINATION ${_cfg_install_dir}
  )

  # ── Why there is no export(EXPORT ...) here ───────────────────────────────
  # There used to be one, describing this same target set against the build tree
  # so a consumer could point CMAKE_PREFIX_PATH at a build directory. It was
  # removed (#29), and it must not come back, because it cannot survive being
  # copied into a project whose library links a dependency it did not build.
  #
  # export() enforces the same "every referenced target must be in an export
  # set" rule as install(EXPORT), but against *build* export sets — the ones
  # registered by export() itself. Plenty of dependencies register none: they
  # ship install(EXPORT) rules and no export() call anywhere. Link one into this
  # library and generation stops with
  #
  #   export called with target "<project>_lib" which requires target "<dep>"
  #   that is not in any export set.
  #
  # The install side above does not fail on the same target, which is why this
  # surprises: CMake finds <dep> in the install export set its own rules
  # registered and quietly rewrites the reference to <dep>::<dep>. Only the
  # build-tree path has nothing to find.
  #
  # It cannot be guarded, either — and that is worth stating, because "just skip
  # the export when a dependency is missing" is the obvious next idea. The
  # condition is not merely unqueryable (no property or command reports build
  # export set membership); it is unexpressible at the point the decision has to
  # be made. INTERFACE_LINK_LIBRARIES holds unevaluated generator expressions
  # — $<LINK_ONLY:...>, $<BUILD_INTERFACE:...>, $<TARGET_NAME_IF_EXISTS:...> —
  # that resolve during generation, after every line of CMake language has run.
  #
  # A fork that genuinely wants a build-tree package can put the missing
  # dependency in a build export set itself, per dependency:
  #
  #   export(TARGETS <dep> NAMESPACE <dep>:: FILE ${CMAKE_BINARY_DIR}/<dep>Targets.cmake)
  #
  # That is boilerplate this file cannot write on anyone's behalf, and it is
  # only half the job — a build-tree package also wants its own
  # configure_package_config_file() call, because the one below computes
  # PACKAGE_PREFIX_DIR for lib/cmake/<project>, three directories above a build
  # dir. Sharing it is harmless only for as long as the config resolves nothing
  # relative to itself.
  #
  # ⚠ Do not reach for export(TARGETS ... APPEND) to silence the error. APPEND
  # mode does not complain about the missing target; it writes the reference as
  # <project>::<dep> — a target in *this* project's namespace that nothing
  # anywhere defines. A loud generate-time failure becomes a Targets file that
  # is quietly wrong.
  #
  # For developing two projects side by side, use add_subdirectory() — same
  # target spelling, no packaging involved, and example/consumer/ covers it.

  # ── Headers ───────────────────────────────────────────────────────────────
  # *.hpp only, which picks up the public header and the generated version
  # header while leaving version.hpp.in.cmake behind (it does not end in .hpp).
  #
  # Installing the generated header is deliberate, not incidental: the
  # header-only variant inlines lib.hpp's function bodies, and those read
  # VERSION_MAJOR / PROGRAM_NAME from it — leave it out and that variant cannot
  # be consumed at all.
  #
  # ⚠ The cost, which a real project should weigh: the generated header declares
  # unprefixed globals (PROGRAM_NAME, VERSION_MAJOR, ...) and lands directly in
  # the consumer's include path, where it can collide with theirs. A project that
  # expects to be widely consumed should move its headers under
  # include/<project>/ and generate the version header there too.
  install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.hpp"
  )
endif ()

# ── The application ─────────────────────────────────────────────────────────
# Installed when it is built, but deliberately not exported: an executable is
# something you run, not something another project links.
if (TARGET ${PROJECT_NAME} AND ${PROJECT_NAME}_BUILD_BIN)
  install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  )
endif ()

# ── Package config ──────────────────────────────────────────────────────────
# <project>Config.cmake is what find_package(<project> CONFIG) loads; it exists
# to pull in the Targets file (and, in a real project, to re-find the public
# dependencies those targets need — see the commented block in the .in file).
#
# Guarded on the same target as the library block above, and for a sharper
# reason than symmetry. This section used to run unconditionally, so
# -D<project>_BUILD_LIB=OFF installed a Config + ConfigVersion pair beside no
# Targets file at all. Every other library-shaped rule here, the headers
# included, was already behind the guard; this was the one piece that was not
# (#33).
#
# The tempting reading is that leaving it installed is *helpful*: the guard at
# the top of project-config.cmake.in reports <project>_FOUND FALSE with a
# message naming the cause, so the stray config looks like a useful signpost.
# It is not. A config that sets _FOUND FALSE does not politely step aside.
#
#   find_package does not resume searching the remaining prefixes after it. A
#   library-less prefix earlier on CMAKE_PREFIX_PATH shadows a perfectly good
#   install later on it, and the consumer is simply told the package was not
#   found.
#
#   ⚠ Worse, <project>_DIR is cached as a *real path* to that directory, so
#   CMake treats the package as resolved and never searches again.
#   Re-configuring with only the good prefix does not recover — the consumer
#   has to unset <project>_DIR, which nothing in the error message mentions. A
#   package that is merely absent caches <project>_DIR-NOTFOUND instead, and
#   self-heals as soon as a good prefix appears. So the stray config turns a
#   self-healing miss into a sticky one.
#
# That is the whole argument: it is not a signpost, it is a tombstone that
# swallows the next candidate. The diagnosis it buys goes to whoever passed
# BUILD_LIB=OFF themselves and already knows; the cost falls on a third party
# consuming an unrelated, working install. Not installing it does cost
# something — a consumer pointed at a library-less prefix now gets CMake's
# generic "add the installation prefix to CMAKE_PREFIX_PATH", which misdirects,
# since the prefix was right. But that is worse wording for a case that fails
# either way, and this is the cheaper of the two.
#
# It also makes the configuration coherent: BUILD_LIB=OFF with INSTALL=ON means
# "install the application", and a CMake package config is a -dev artifact.
# No library → no headers → no Targets → no Config.
if (TARGET ${PROJECT_NAME}_lib)
  configure_package_config_file(
    ${CMAKE_CURRENT_LIST_DIR}/project-config.cmake.in
    ${PROJECT_BINARY_DIR}/${PROJECT_NAME}Config.cmake
    INSTALL_DESTINATION ${_cfg_install_dir}
  )

  # ARCH_INDEPENDENT only for the header-only variant: a config that ships no
  # compiled artifact is usable from a build of any word size, and saying so
  # keeps the package from being rejected on a 32/64-bit mismatch that cannot
  # apply. Detected rather than configured, so swapping variants needs no edit
  # here.
  set(_version_file_args "")
  get_target_property(_lib_type ${PROJECT_NAME}_lib TYPE)
  if (_lib_type STREQUAL "INTERFACE_LIBRARY")
    set(_version_file_args ARCH_INDEPENDENT)
  endif ()

  # SameMajorVersion is the conventional read of semver: 1.4.0 satisfies a
  # request for 1.2.0, 2.0.0 does not. Swap to SameMinorVersion or ExactVersion
  # if your project's compatibility promise is narrower.
  #
  # ⚠ The version comes from `git describe` at configure time
  # (cmake/version.cmake). A build with no reachable tags reports 0.0.0, and a
  # consumer asking for a real version then gets a documented refusal from this
  # file rather than a mystery. If that happens in CI, the cause is almost
  # always a shallow clone — keep fetch-depth: 0.
  write_basic_package_version_file(
    ${PROJECT_BINARY_DIR}/${PROJECT_NAME}ConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
    ${_version_file_args}
  )

  install(FILES
      ${PROJECT_BINARY_DIR}/${PROJECT_NAME}Config.cmake
      ${PROJECT_BINARY_DIR}/${PROJECT_NAME}ConfigVersion.cmake
    DESTINATION ${_cfg_install_dir}
  )
endif ()
