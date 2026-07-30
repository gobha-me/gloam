# ── Dependency recipe template (canonical example) ──────────────────────────
# Copy this file for a new dependency, then add its stem to ${PROJECT_NAME}_DEPS
# in the root CMakeLists.txt to activate it (presence alone does nothing).
#
# The shape every recipe follows:
#   1. find_package(<Name> [version] QUIET)   — prefer a system/distro copy.
#   2. On miss, FetchContent fallback with overridable, pinned coordinates:
#        <Dep>_URI  — git repository (default set below; override with -D or by
#                     setting it before this file is included).
#        <Dep>_TAG  — git tag/ref to pin (reproducible fallback).
#   3. Decide the dependency's *install* rules, which are not a fixed default —
#      see below. This recipe needs none: Catch2 serves only the test suite.
# Bump pins deliberately and say why in the commit (see AGENTS.md).
#
# ── Step 3: a dependency's install rules follow where it is linked ──────────
# Only relevant to the FetchContent branch. A dependency found by find_package
# is already installed somewhere and exports nothing of ours.
#
# Two cases, opposite answers:
#
#   Private to the executable or the tests — neither is ever exported, so the
#   dependency must not add rules to our prefix:
#
#       set(<DEP>_INSTALL OFF)
#
#     as cmake/deps/fmtlib.cmake does. Nothing else is needed.
#
#   Linked into the library — the library IS exported, so the dependency has to
#   track our own install option rather than sit at a fixed value:
#
#       set(<DEP>_INSTALL ${${PROJECT_NAME}_INSTALL})
#
#     ON while we are top level, or the dependency's target is in no install
#     export set, our install(EXPORT) cannot resolve the reference to it, and
#     the build stops during generation. OFF while we are consumed, or the
#     dependency deposits its headers, its package config and its README into a
#     downstream project's prefix — which is not ours to write to, and which
#     happens by default, because these options usually default ON even for a
#     subproject.
#
#     Such a dependency also needs a find_dependency() line in
#     cmake/project-config.cmake.in. The two go together: this half makes the
#     package build, that half makes it consumable.
#
# ⚠ "Linked into the library" means either visibility. A PRIVATE link into a
# static library still reaches the exported target — CMake records it as
# $<LINK_ONLY:...> because a consumer must link it too — and is checked exactly
# like a PUBLIC one. Reading "private" as "no install rules needed" is the
# mistake this note exists to prevent.
#
# ⚠ Whether `set(<DEP>_INSTALL ...)` is honoured at all depends on policy
# CMP0077 as evaluated under the *dependency's* policy stack, which its own
# cmake_minimum_required() resets — ours does not decide it. A dependency
# declaring a floor below 3.13 without a policy range ignores the variable and
# keeps its option()'s default; force it with
# CMAKE_POLICY_DEFAULT_CMP0077 set to NEW before FetchContent_MakeAvailable.
#
# ⚠ CMAKE_INSTALL_DOCDIR is derived from PROJECT_NAME at the point
# GNUInstallDirs is *first* included in the build, and subdirectories inherit
# that value. A dependency installing its own README there therefore files it
# under the enclosing project's name — never its own — so a leak of this kind
# shows up as documentation you appear to have written.
#
# example/public-dep/ builds a fork that has one such dependency and asserts
# both halves, including by injecting each mistake above and confirming it goes
# red. example/public-dep/pubdep.cmake is the worked recipe.

find_package(Catch2 3 QUIET)

if (Catch2_FOUND)
else ()
    if (NOT Catch2_URI)
        set(Catch2_URI https://github.com/catchorg/Catch2.git) 
    endif()

    if (NOT Catch2_TAG)
        set(Catch2_TAG v3.5.2)
    endif()

    include(FetchContent)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY ${Catch2_URI}
        GIT_TAG ${Catch2_TAG}
    )

    FetchContent_MakeAvailable(Catch2)
endif()
