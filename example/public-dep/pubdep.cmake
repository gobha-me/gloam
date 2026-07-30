# ── Dependency recipe for the harness fixture ───────────────────────────────
# example/public-dep/verify.sh copies this to cmake/deps/pubdep.cmake inside a
# throwaway fork of this project. It follows the shape cmake/deps/catch2.cmake
# documents, and it is deliberately the *public* case: the fork's library links
# this dependency into its own public interface, so this file is also the
# worked example of the install-visibility rule that recipe describes.
#
# Not activated in this project — the name is not in the dependency list in the
# root CMakeLists.txt, so nothing here runs during a normal build. It lives
# outside cmake/deps/ for the same reason: a recipe sitting in that directory
# invites the reader to think presence activates it, which it does not.

find_package(pubdep QUIET)

if (pubdep_FOUND)
else ()
    # No default: the harness always passes both, pointing at a local checkout
    # so the whole run stays offline. A real recipe hardcodes a public URL and a
    # pinned tag here — see cmake/deps/catch2.cmake.
    if (NOT PUBDEP_URI OR NOT PUBDEP_TAG)
        message(FATAL_ERROR "pass -DPUBDEP_URI= and -DPUBDEP_TAG=")
    endif ()

    # ── The line this harness exists to prove ───────────────────────────────
    # This dependency is linked into the exported library, so its install rules
    # have to track ours rather than sit at a fixed value:
    #
    #   ON when we are top level  — the dependency's target must land in an
    #     install export set, or our own install(EXPORT) cannot resolve the
    #     reference to it and the build fails at generate time.
    #   OFF when we are consumed  — otherwise this dependency deposits its
    #     headers, its README and its package config into a downstream
    #     project's prefix, which is not ours to write to.
    #
    # A fixed `set(PUBDEP_INSTALL OFF)`, the right answer for a dependency that
    # is private to the executable, breaks the first case. Leaving it alone
    # breaks the second, because this dependency's own option defaults ON.
    set(PUBDEP_INSTALL ${${PROJECT_NAME}_INSTALL})

    include(FetchContent)
    FetchContent_Declare(
        pubdep
        GIT_REPOSITORY ${PUBDEP_URI}
        GIT_TAG ${PUBDEP_TAG}
        SOURCE_DIR ${FETCHCONTENT_BASE_DIR}/pubdep
    )

    FetchContent_MakeAvailable(pubdep)
endif ()
