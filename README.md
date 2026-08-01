# GLOAM

[![CI](https://github.com/gobha-me/gloam/actions/workflows/ci.yml/badge.svg)](https://github.com/gobha-me/gloam/actions/workflows/ci.yml)

A 1-bit grid dungeon crawler where your lamp is the thing that lets you see and
the thing that gets you seen — and the renderer transmits zero pixels during
play.

GLOAM is a terminal game built on [termforge](https://github.com/gobha-me/termforge)
and the kitty graphics protocol. It does not raycast. Like *Eye of the Beholder*
and *Dungeon Master* before it, it is a sprite compositor: there is a fixed set
of wall positions relative to the viewer, each with a pre-drawn, pre-scaled
plate. Every plate is transmitted **once**, at startup, and stays resident. A
frame during play is a list of placement commands — no PNG payloads, no base64
through the tty. An idle frame costs **zero bytes**, which is what makes the
whole thing playable over ssh.

## Status

**Pre-M0.** There is no playable game yet, and the `gloam` binary is a headless
diagnostic rather than a game. What exists is the deterministic simulation core:

| Built | SPEC |
| --- | --- |
| Geometry ladder, screen layout, the reflow rules | §3 |
| Named RNG streams, portable across GCC and Clang | §5.1 |
| Noise emission, per-edge attenuation, propagation | §6.2 |
| Awareness state machine and its five authored tells | §6.1 |
| Light, sight and the doused-party pillar | §6.3 |
| Rune grammar, spell resolver, danger classes | §8 |
| Budgets, wired as assertions before the code they constrain | §11 |
| The compositing bands, and the kitty call boundary over them | §4.5, §16 |

The **compositor** is still blocked upstream — see [UPSTREAM.md](UPSTREAM.md).
termforge stretches a placed image to fill its cell rect and states that scaling
is the contract, which §3.2 rules out by name because resampling a pre-dithered
plate reintroduces the dither crawl the whole pipeline exists to avoid. So
[#7](https://github.com/gobha-me/gloam/issues/7) has nothing to sit on yet.

The layer API is built anyway, and deliberately so: §16's mitigation for that
exact risk is to keep every kitty call behind GLOAM's own boundary from day one,
"so a vendored driver is a swap and not a rewrite". A boundary built after the
driver arrives is not insurance. Two ctest cases keep it honest —
`layer-z-single-definition` (no code hand-writes a z-index) and
`kitty-boundary-single-module` (no code outside `src/lib/kitty.cpp` writes an
escape sequence).

The **asset pipeline** ([#1](https://github.com/gobha-me/gloam/issues/1)) has
landed its first slice, and it is the one thing on the critical path that never
needed termforge. `gloam_bake` writes a versioned, hashed `pack.gloam`: §12's
manifest, §4.3's fixed ordered dither, §3.1's exact 2:1 downsample, and the six
full-frame light fields §4.4 asks for — the one asset class §10 marks
*procedural*, so it could be built before any art exists. Two runs produce a
byte-identical pack, verified under GCC 13, GCC 14 and Clang 20; the
`pack-reproducible` ctest case runs the binary twice and compares the files,
because §10 makes that hash a build gate rather than a nicety.

What that slice does **not** include is the authored depth-0 and depth-1 wall
rings, because no art and no authoring format exist yet. #1 stays open for them.

It also put a number on something uncomfortable: §11's 1.2 MB cold-start budget
is for the **base64 transmit payload**, and a 2-bit plate expands to RGBA before
it goes on the wire. The six light fields are 388,800 B in the pack and roughly
5.5 MB transmitted — see [#17](https://github.com/gobha-me/gloam/issues/17).

Also unblocked: the [replay harness](https://github.com/gobha-me/gloam/issues/3)
and the [audio sink](https://github.com/gobha-me/gloam/issues/4).

## Design

The full specification is [`design/SPEC.md`](design/SPEC.md), vendored from the
design project that owns it — see [`design/README.md`](design/README.md) for the
provenance and for which copy wins. Section numbers throughout this codebase
(§3, §6.2, §11 …) refer to it, and every non-obvious constant carries the sentence
it came from. The rule the code follows: if a number appears here, the reason it
has that value appears next to it.

`design/` also carries the slices pulled out for specific jobs —
[`BUDGETS.md`](design/BUDGETS.md), [`SCHEMAS.md`](design/SCHEMAS.md),
[`TEST-PLAN.md`](design/TEST-PLAN.md), [`BUILD-ORDER.md`](design/BUILD-ORDER.md) —
and the rune data. `UPSTREAM.md` is where the design document's own staleness is
tracked; the snapshot is not patched in place.

Three commitments shape almost every file:

* **The lamp is the game.** Light determines what is drawn *and* what can see
  you. One `lamp_level` integer drives both the render and the perception model;
  there is no second light model.
* **Deterministic to the bit.** Seed in, world out. Fixed-tick integer
  simulation, no floats in simulation state, no wall-clock reads, no
  `std::random_device`, and no `<random>` distributions — they are not portable
  across standard libraries, and a replay must reproduce on both compilers.
* **Budgets are contracts.** Every number in §11 is a test that fails the build,
  not a target.

## Layout

```
design/           the specification the code cites — a snapshot; see design/README.md
include/gloam/    the deterministic core's public headers, plus ten off-umbrella
src/lib/          its implementation — standard library only, no I/O, no clock
src/bin/          the diagnostic binary and gloam_bake; termforge lands here too
test/             property tests (§13.3) and budget assertions (§11)
cmake/            the template's build machinery, plus check_layer_z.cmake (§4.5),
                  check_kitty_boundary.cmake (§16) and check_pack_repro.cmake (§10)
```

Ten headers in `include/gloam/` are not simulation and are deliberately left out
of the `gloam/gloam.hpp` umbrella: four render-side (`budgets.hpp`, `layer.hpp`,
`emit.hpp`, `kitty.hpp`) and six for the offline pipeline (`dither.hpp`,
`plate.hpp`, `lightfield.hpp`, `pack.hpp`, `sha256.hpp`, `assets.hpp`). They live inside the
standard-library-only boundary anyway, because **producing** bytes is not the
same as needing a terminal; the `write` that puts them on one is in `src/bin/`,
and so is the only `open` in the pipeline. None of the pipeline six owns a
plate — they take caller-owned spans and report the size they need, which is what
kept image ownership out of the library when the pack format arrived.
`gloam.hpp` says all of this at the top, so the exclusion does not read as an
oversight.

`gloam::lib` links nothing beyond the standard library, and that is a hard
architectural boundary rather than a coincidence: a simulation that can reach a
terminal or a clock is a simulation that cannot be replayed.

## Cheat sheet

**Configure, build, test — and picking a toolchain**

```bash
cmake -B build                                                            # $CXX, C++23, Debug
cmake -B build-clang -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain/clang.cmake   # clang
CXX=clang++ cmake -B build-asan -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain/address.cmake

cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Run these from the repo root — the toolchain paths are relative. You cannot pass
two toolchain files, so a sanitizer composes with clang via `CXX=`, as above.

**Add a dependency**

```bash
cp cmake/deps/catch2.cmake cmake/deps/<name>.cmake   # catch2.cmake is the annotated recipe template
$EDITOR cmake/deps/<name>.cmake                      # find_package first, FetchContent fallback
$EDITOR CMakeLists.txt                               # add <name> to <PROJECT>_DEPS
cmake -B build
```

The recipe file alone does nothing — the list is the switch. A name on the list
with no recipe is a hard configure error; a recipe not on the list is inert. To
remove a dependency, delete its name from the list; the file can stay.

If the **library** links the new dependency — rather than the executable or the
tests — it needs two more lines, because it becomes part of what this project
exports: `set(<DEP>_INSTALL ${${PROJECT_NAME}_INSTALL})` in the recipe, and a
`find_dependency(<dep>)` in `cmake/project-config.cmake.in`. Without the first,
installing fails during generation; without the second, consumers get a package
whose targets refer to something they cannot find. A `PRIVATE` link counts —
visibility is not the test. The annotated recipe explains both, and
`example/public-dep/` is a working example that is checked in CI.

**Add a test**

```bash
mkdir test/40myfeature
$EDITOR test/40myfeature/test.cpp    # TEST_CASEs only — test/main.cpp provides main()
cmake -B build && cmake --build build --parallel && ctest --test-dir build
```

It becomes the target and ctest name `40myfeature-test`. **Re-run `cmake -B`
after adding a directory** — discovery is a configure-time glob. The numeric
prefix only sorts that glob; it does not order the run (see the Tests bullet
above). The target links Catch2 and, when the library target exists,
`<PROJECT>::lib` — so `#include <lib.hpp>` and call into `src/lib/` directly,
with nothing to wire up. If a test needs custom build control, give it its own
`test/<dir>/CMakeLists.txt`: it inherits `TEST_NAME` and `SRCS` from the parent
scope, must define a target named exactly `${TEST_NAME}`, and must do its own
linking — the discovery loop's link lines do not reach it (see
`test/02example/`).

**Run one test**

```bash
ctest --test-dir build -N                                              # list them
ctest --test-dir build -R 20failure-testing-test --output-on-failure   # one, plus its fixtures
ctest --test-dir build -R 20failure-testing-test -FS . -FC .           # one, fixtures skipped

./build/test/20failure-testing-test --list-tests                       # Catch2 cases within it
./build/test/20failure-testing-test "[failure]"                        # one case, or a tag
./build/test/20failure-testing-test -s                                 # show successful assertions too
```

`-R` is a regex match on the test name. Every discovered test carries
`FIXTURES_REQUIRED runners`, so ctest re-adds `startup` and `shutdown` even when
you filter — `-R` alone reports **three** tests, not one. `-FS . -FC .` excludes
the fixtures. Tests with their own `CMakeLists.txt` build into
`build/test/<dir>/`; the rest land in `build/test/`.

**Consume this project from another project**

Three ways in, one target name — `<PROJECT>::lib` — so switching between them
never touches a link line.

```cmake
# 1. vendored / submoduled
add_subdirectory(third_party/<project>)

# 2. FetchContent
include(FetchContent)
FetchContent_Declare(<project>
  GIT_REPOSITORY <url>
  GIT_TAG        v1.2.3
  SOURCE_DIR     ${FETCHCONTENT_BASE_DIR}/<project>   # ← see below
)
FetchContent_MakeAvailable(<project>)

# 3. installed
find_package(<project> CONFIG REQUIRED)

target_link_libraries(app PRIVATE <project>::lib)     # all three, unchanged
```

⚠ **Pin `SOURCE_DIR` in the FetchContent case.** This project takes its name
from its directory, and FetchContent checks out into `<base>/<name>-src` — so
without that line the project comes out named `<name>-src` and the target you
have to link is `<name>-src::lib`. This applies to any directory-named project,
not just this one.

You inherit the include directory *and* C++23 as usage requirements of the
target; a consumer sets neither. `example/consumer/` is a working downstream
project that builds all three ways, and `example/consumer/verify.sh` runs them.

**Install it**

```bash
cmake -B build -DCMAKE_INSTALL_PREFIX=/opt/<project>
cmake --build build --parallel
cmake --install build
```

Installs the library, `include/*.hpp`, the executable, and a package config at
`<prefix>/lib/cmake/<project>/`. Build with `-D<PROJECT>_BUILD_BIN=OFF
-D<PROJECT>_TESTS=OFF` for a library-only install.

Three things worth knowing before you depend on it:

* The package exists **only in an install prefix**. Pointing
  `CMAKE_PREFIX_PATH` at a build directory finds the config file that was
  generated there and gets a directed refusal, not a package: this project
  exports its targets at install time only. For side-by-side development use
  `add_subdirectory` — same target name, no packaging in the way.
* The version in the package config comes from `git describe` at configure time.
  A build with no reachable tags reports `0.0.0`, and a consumer's
  `find_package(<project> 1.2.3 CONFIG REQUIRED)` is then refused — correctly,
  but the real cause is usually a shallow clone. Compatibility is
  `SameMajorVersion`.
* Headers install flat into `<prefix>/include`, generated `version.hpp`
  included. That header declares *unprefixed* constants (`PROGRAM_NAME`,
  `VERSION_MAJOR`, …). A project expecting wide consumption should move its
  headers under `include/<project>/` first.

**Cut a release tag**

```bash
git tag -a v1.2.3 -m "v1.2.3"
git push origin v1.2.3
cmake -B build          # the version is read at CONFIGURE time — re-configure or it is stale
cmake --build build --parallel
```

The format is enforced: optionally `v`- or `r`-prefixed, then exactly three
numeric components. `v1.2`, `v1.2.3.4` and `v1.2.3-rc1` are rejected by design
and fall back to `0.0.0` with a `STATUS` line naming the reason. Between tags,
`VERSION_TWEAK` counts commits since the tag and `VERSION_DIRTY` flags an unclean
tree; both land in `include/version.hpp`.

## Continuous integration

`.github/workflows/ci.yml` builds and tests on every push to `main` and every
pull request, enforcing the "both compilers, always" rule:

* **GCC and Clang** ×
* the **default** toolchain plus every sanitizer (**address**, **thread**,
  **undefined**) — 8 build/test jobs in all,
* a **library disabled** job, covering the `-D<PROJECT>_BUILD_LIB=OFF` path the
  matrix never takes — it installs as well as builds, and asserts the prefix
  gets the executable and nothing else,
* two **consumer** jobs (one per compiler) building `example/consumer/` against
  this project three ways — the only coverage of the consumed, not-top-level
  path,
* a **public dependency** job, which synthesises a fork whose library links a
  fetched dependency and checks the install/export files survive it — the one
  shape this project cannot exercise as itself,
* plus a fast, dependency-free `version-parse-selftest` job.

A change that only builds on one compiler turns that compiler's jobs red, so a
one-sided break is visible on the PR.

**Copying this into a new project:** the workflow hardcodes nothing
project-specific — the project name is derived from the checkout directory, so
copy `.github/workflows/ci.yml` verbatim, and keep `fetch-depth: 0` or
`git describe` stops finding tags. The one edit a fork owes CI is the badge URL
above; that step and everything else a new project must change live in
[NEW_PROJECT.md](NEW_PROJECT.md).
