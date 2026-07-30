# AGENTS.md — conventions for agents working in this repo

If you're an LLM (or an LLM-driven editor) about to make changes here, read this
first.

## What this repo is

**GLOAM** — a 1-bit grid dungeon crawler for the terminal, built on
[termforge](https://github.com/gobha-me/termforge) and the kitty graphics
protocol. Bootstrapped from the same author's CMake starter, so the build
machinery, toolchain files and testing conventions below are inherited on
purpose — treat them as decided unless there is a GLOAM-specific reason.

The design is a formal written specification. Section markers throughout the
code (§3, §6.2, §11 …) refer to it, and they are not decoration — they are how
you find out *why* a number is what it is before changing it.

### Three rules that outrank ordinary judgement here

1. **`gloam::lib` links nothing but the standard library.** No termforge, no
   RtAudio, no clock, no terminal, no I/O. §5.1 requires the simulation to be
   replayable, and a simulation that can reach a wall clock cannot be. If code
   needs a terminal, it belongs in `src/bin/`. If you find yourself adding an
   include that breaks this, the design is telling you the code is in the wrong
   place.

2. **No floating point in simulation state.** §6 is explicit: "Every number
   below is an integer." Where the design prose says a multiplier — creep is
   "× 0.5", ESK is "cast noise halved" — it is spelled as an explicit
   numerator/denominator pair in `tuning.hpp`. A float in sim state is a
   determinism bug that surfaces on someone else's CPU, months later, as a
   replay that no longer reproduces.

3. **Nothing from `<random>`.** `std::uniform_int_distribution` and friends are
   not portable across standard libraries — the same engine and seed may produce
   different values on libstdc++ and libc++. §19 step 7 requires a recorded
   session to replay to an identical world hash on **both** compilers, so
   `rng.hpp` spells out its own bounded draw in fixed-width integer arithmetic.

### Two things that must never be filed upstream

- **RtAudio.** termforge ships standard library only. §9.1: an audio subsystem
  cannot land there without breaking the library's central promise. Audio lives
  in GLOAM permanently.
- **Game content.** The depth ladder, the light-field plates, the noise model,
  the rune grammar. These are GLOAM's, not a TUI framework's.

Everything else that GLOAM needs and termforge lacks is a candidate for an
upstream feature request — the test is whether a *different* TUI project would
want it too. `UPSTREAM.md` tracks the open ones and shows the reasoning.

### Tunables and the ruleset hash

Every tunable integer in §6 and §8 lives in `include/gloam/tuning.hpp`, and
`ruleset_hash()` covers all of them. Adding or changing one is a real decision:
it invalidates every recorded replay, deliberately, because a replay recorded
against different tuning must be rejected at load rather than silently
mis-played. `visit_fields` is what makes the hash total — add a field there in
the same commit, and update `kTuningFieldCount`. The static_asserts will stop
you if you forget.

## Conventions that matter here

- **Toolchains are opt-in files** in `cmake/toolchain/`: `default.cmake`
  (respects env), `clang.cmake`, `address.cmake`, `thread.cmake`,
  `undefined.cmake`. To add a configuration, add a file that `include()`s
  `default.cmake` and layers its flags — don't edit `default.cmake` to force a
  specific setup.
- **Library pattern** in `src/lib/`: a compiled `STATIC` lib by default
  (toggle `${PROJECT_NAME}_BUILD_LIB`), public API in `include/lib.hpp`, with the
  header-only (`INTERFACE`) variant shown commented. Flipping to `INTERFACE`
  means every function in that header has to become an `inline` definition, or
  nothing that calls it links. Keep both patterns present and buildable — the
  template teaches by having both. That is a rule for *this* repo; a project
  bootstrapped from it picks one and deletes the other (see `NEW_PROJECT.md`).
- **Consumer-clean is a rule, not a nicety.** This project has to keep working
  when it is *not* the top-level one. Concretely:
  - Never `CMAKE_SOURCE_DIR` / `CMAKE_PROJECT_VERSION` / `CMAKE_PROJECT_NAME` —
    the `CMAKE_`-prefixed forms describe the top-level build, which belongs to
    someone else the moment we are consumed. Use `PROJECT_SOURCE_DIR`,
    `PROJECT_VERSION`, `PROJECT_NAME`.
  - A new `option()` defaults to `${PROJECT_IS_TOP_LEVEL}` unless it gates the
    library itself. Anything that builds an application, registers tests, or
    writes install rules is the consumer's business, not ours.
  - Public include directories are always
    `$<BUILD_INTERFACE:…>` / `$<INSTALL_INTERFACE:…>` genexes. A bare source
    path in a `PUBLIC` include directory makes `install(EXPORT)` fail at
    generate time — that failure is the feature, not the bug.
  - Anything the public header needs in order to compile (the C++ standard,
    a public dependency) travels on the target via `target_compile_features` /
    `PUBLIC` links. A consumer uses their own toolchain, so
    `cmake/toolchain/default.cmake` reaches them not at all.
  - `example/consumer/verify.sh` is what proves all of this; it is in "How to
    verify a change" below for that reason.
- **Tests are auto-discovered**: `test/CMakeLists.txt` loops over `test/*/`.
  A new test is just `test/<name>/test.cpp` (no CMakeLists needed); it gets
  `main()` from `test/main.cpp`, plus Catch2 and `${PROJECT_NAME}::lib` behind an
  `if (TARGET ...)` guard, so `-D<PROJECT>_BUILD_LIB=OFF` still configures. Add a
  `CMakeLists.txt` in the dir only if the test needs custom build control — that
  dir then owns its own wiring, the library link included. Directory names sort
  the glob, which sets registration order, not execution order; use fixtures or
  `DEPENDS` when order actually matters. After adding a test dir, re-run
  `cmake -B`.

## Testing philosophy (the important one)

**Test how code fails, not just that it produces the right output.** A
happy-path assertion (`REQUIRE(fun(10 / 5) == 2)`) only proves the code returns
what you already knew it returned, on input chosen because it works. The
valuable tests are the adversarial ones — bad input, boundaries, overflow,
malformed external data, error paths. Write the **failure matrix first**; the
happy-path check is the last, least-interesting test (a smoke check that the
harness runs). See `test/20failure-testing/` for the canonical example, and
`test/10example/` for the same discipline applied to this repo's own library
through its public header. `test/01example/` and `test/02example/` are
deliberately thin — they demonstrate discovery and custom build control, not how
to write a test.

## How to verify a change (do this before opening a PR)

```bash
cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure
# and cross-compiler, since the template supports both:
cmake -B build-clang -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain/clang.cmake \
  && cmake --build build-clang && ctest --test-dir build-clang

# and, for anything touching the build's shape, the consumed path — which no
# ctest covers, because every test runs with this repo as the top-level project:
example/consumer/verify.sh
CXX=clang++ example/consumer/verify.sh

# and, for anything touching install/export or the dependency recipes, the path
# this project cannot exercise on its own — a fork whose library links a public
# dependency. One compiler is enough: it fails during generation, if it fails.
example/public-dep/verify.sh
```

Both must build clean and pass all tests. A change that only builds on one
compiler is not done. (This is how the fmt-under-clang-20 breakage was caught —
build on both, always.)

CI (`.github/workflows/ci.yml`) enforces this on every push and pull request:
GCC and Clang × {default, address, thread, undefined} toolchains, plus the
`library disabled`, `consumer` (×2 compilers), `public dependency` and
`version-parse-selftest` jobs.
A one-compiler change turns that compiler's jobs red, so the template can't rot
unnoticed — run the commands above locally first.

CI pins its Clang jobs to Clang 20: Ubuntu 24.04's stock Clang 18 cannot compile
the C++23 `std::expected` example (`test/20failure-testing`) against libstdc++ —
use Clang 19+ (libstdc++) or any Clang with libc++. Same class of compiler/stdlib
break as the fmt-under-clang-20 note above; CI surfaced it. If you develop with
Clang, verify with a version CI would accept, not just whatever `clang++` resolves to.

**The same applies to GCC, and it is the easier one to get wrong**, because
`g++` on a dev box is usually *newer* than CI's, so a change can pass locally and
fail on the supported floor. CI runs GCC 13; `g++-13` is packaged alongside
`g++-14` on Ubuntu 24.04, so reach for `CXX=g++-13` before opening a PR that
touches language-level or usage-requirement behaviour. Worked example: C++23 mode
on GCC 13 reports `__cplusplus` as **202100L**, not 202302L (CMake selects
`-std=c++2b` there), so a `__cplusplus >= 202302L` check passes on GCC 14 and
Clang 20 and fails on the floor. Prefer a feature-test macro to a `__cplusplus`
comparison — see the note in `example/consumer/main.cpp`.

## Attribution

Follow the convention used across this org's repos: agent-authored commits
carry a trailer naming the model, e.g.

```
Co-authored-by: Kimi K3 (vcoder via Venice) <noreply@venice.ai>
Agent: vcoder / Kimi K3
```

and PRs note what was actually run to verify (per "How to verify" above).

## Notes for agents

- `include/version.hpp.in.cmake` is configured into `include/version.hpp` at
  build time; edit the `.in.cmake` source, not the generated file. If you touch
  it, keep the `#include <cstdint>` (std::uint32_t needs it).
- Version parsing is pure string logic in `cmake/version_parse.cmake`
  (`parse_git_describe`); `cmake/version.cmake` just runs `git describe` and calls
  it. If you change the parsing, add a row to and re-run the self-test:
  `cmake -P cmake/version_selftest.cmake` (also runs in ctest as
  `version-parse-selftest`). Failure-matrix-first, like the other tests.
- `NEW_PROJECT.md` is **fork-facing**: it instructs a project being bootstrapped
  out of this template, not this repo. Don't put template-maintenance rules in
  it, and don't let it drift from the file paths and line numbers it cites.
- `cmake/check_artifacts.cmake` looks for leftover template artifacts. It runs
  inverted here (ctest: `artifact-check-selftest`) — every Class-A rule must
  still MATCH something, because this repo legitimately contains all of them.
  Rename or delete an artifact that a rule targets and that rule matches
  nothing, the self-test goes red, and you update the rule to match. A fork that
  has deleted `NEW_PROJECT.md` runs the same script in plain enforcement mode
  instead. Class-B rules check wiring that can drift, are never inverted, and
  must stay green on both sides.
  **Never write one of the searched-for tokens into prose.** A rule counts hits
  across all tracked files, so a doc that quotes the token it is hunting keeps
  that rule green forever, whatever happened to the real artifact. The checker
  and `NEW_PROJECT.md` are excluded from the scan for exactly this reason; the
  fix anywhere else is to describe the token, not spell it.
- Build dirs (`build*/`) are gitignored — don't commit them.
- The dep pins in `cmake/deps/` are only audited when something breaks on a
  supported compiler; bump deliberately and say why in the commit.
