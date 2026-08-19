# ── TermForge — SPEC §4.8's terminal lifecycle ───────────────────────────────────
#
# Private to src/bin/ and test/31imagelifecycle/. It must never reach
# gloam::lib: the simulation core is standard-library-only and cannot own a
# terminal, a clock or an I/O policy (AGENTS.md rule 1).
#
# v0.55.0 is the first GLOAM pin. It contains the complete contract this layer
# consumes: application-resident PinnedImage handles, generation-qualified
# invalidation, native-resolution PlacementFit::Exact, opaque PNG upload, a
# caller-owned ByteSink and terminal cell-pixel geometry.

find_package(termforge 0.55 CONFIG QUIET)

if (NOT TARGET termforge::lib)
  if (NOT termforge_URI)
    set(termforge_URI https://github.com/gobha-me/termforge.git)
  endif ()

  if (NOT termforge_TAG)
    set(termforge_TAG v0.55.0)
  endif ()

  # This dependency is an implementation detail of unexported targets. Its
  # examples, CLI, tests and install rules are all somebody else's build.
  set(termforge_BIN OFF CACHE BOOL "" FORCE)
  set(termforge_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(termforge_TESTS OFF CACHE BOOL "" FORCE)
  set(termforge_INSTALL OFF CACHE BOOL "" FORCE)

  include(FetchContent)
  FetchContent_Declare(
    termforge
    GIT_REPOSITORY ${termforge_URI}
    GIT_TAG ${termforge_TAG}
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(termforge)
endif ()

if (NOT TARGET termforge::lib)
  message(FATAL_ERROR
    "TermForge was found or fetched, but it did not provide termforge::lib")
endif ()
