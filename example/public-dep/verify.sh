#!/usr/bin/env bash
# ── Public-dependency acceptance check (CT-15 / #29) ────────────────────────
# Proves that this project's install/export files can be copied verbatim into a
# fork whose library links a FetchContent'd dependency into its own public
# interface — and that the dependency's install rules follow that visibility
# instead of leaking into a downstream prefix.
#
#   example/public-dep/verify.sh
#
# Nothing here needs a compiler matrix or a network: the failure this exists to
# catch happens at CMake generate time, and the fixture dependency is a local
# directory. Everything is built under mktemp -d.
#
# ── Why a fork has to be synthesised at all ─────────────────────────────────
# This project's own library links nothing publicly, so it cannot exercise any
# of the above. The harness therefore builds a throwaway fork: a snapshot of the
# working tree, patched the four ways a real project would patch it when it
# gains its first public dependency. The patches are the specification — read
# them as "what a fork owes its install rules", because that is what they are.
#
# Considered and rejected: injecting all of this with
# CMAKE_PROJECT_<NAME>_INCLUDE, which needs no file edits at all. It runs at the
# end of project(), which is *before* the <PROJECT>_INSTALL option is declared —
# so the recipe line this harness exists to prove, `set(<DEP>_INSTALL
# ${${PROJECT_NAME}_INSTALL})`, would read an empty value. The leak assertion
# would then pass for the wrong reason, which is worse than not running it.
#
# ── What to break to re-prove each assertion ────────────────────────────────
# Same contract cmake/check_artifacts.cmake states above its Class-B rules. Each
# row was run; each produced the single failure named and no other.
#
#   N1  put export(EXPORT ...) back in cmake/install.cmake
#         -> A, 'export called with target ... not in any export set'.
#            F goes with it: restoring the export makes the build tree look
#            like a package again.                       (this IS CT-15)
#   N2  skip the find_dependency patch below
#         -> D only, 'referenced, but are missing'. A/B/E/F stay green.
#   N3  change the recipe to set(PUBDEP_INSTALL OFF) unconditionally
#         -> A, and C/D behind it. Note it lands on A, not on B: an
#            install(EXPORT) that cannot resolve a target fails during
#            generation, so it never reaches the build.
#   N4  drop the ${${PROJECT_NAME}_INSTALL} tracking from the recipe
#         -> E, listing the dependency's headers, package config and README
#            under the consumer's prefix. Note the README lands in
#            share/doc/<the fork>/, not under the dependency's own name:
#            CMAKE_INSTALL_DOCDIR is fixed by the FIRST include(GNUInstallDirs)
#            in the build and inherited by every subdirectory below it.
#   N5  install the fixture somewhere on the fork's CMAKE_PREFIX_PATH
#         -> the FetchContent self-check, NOT a silent full pass.
#   N6  add an export(TARGETS ...) call to the fixture
#         -> N1 stops reproducing. Proves the fixture's missing build export
#            set is what makes this bug appear, i.e. that we reproduce the
#            right one.
#   N7  change any patch anchor below
#         -> 'anchor ... matched 0 lines', before anything is configured.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HARNESS="${REPO_ROOT}/example/public-dep"
NAME="$(basename "${REPO_ROOT}")"

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

pass=0
fail=0

ok()  { echo "PASS $*"; pass=$((pass + 1)); }
bad() { echo "FAIL $*"; fail=$((fail + 1)); }

# A harness error is not a test failure. It means this script can no longer make
# the assertion it claims to make, which must never be reported as a pass or as
# an ordinary red — exit 2 so it cannot be mistaken for either.
die() { echo "HARNESS ERROR: $*" >&2; exit 2; }

# ── Preflight ───────────────────────────────────────────────────────────────
# The downstream project is example/consumer/ rather than a second copy of one.
# The two are a pair in this direction: delete that directory and this harness
# has nothing to consume with.
[ -f "${REPO_ROOT}/example/consumer/CMakeLists.txt" ] \
  || die "example/consumer/ is gone; this harness builds it as its downstream project"

# Property (2) of the fixture, asserted rather than assumed. An export() call
# appearing in it would quietly retire N1 while every assertion below still
# reported green.
if grep -rn '^[[:space:]]*export[[:space:]]*(' "${HARNESS}/fixture"; then
  die "the fixture registered a build export set; it then no longer reproduces #29"
fi

# ── Patch helper ────────────────────────────────────────────────────────────
# Every edit to the synthesised fork goes through this. It fails loudly when an
# anchor has drifted, because the alternative — a patch that silently no-ops —
# turns into a red three steps later whose message points at the wrong file, and
# that is how an injection harness rots into one nobody trusts.
inject() { # <file> <anchor-ere> <before|after> <text> <verify-substring>
  local f="$1" anchor="$2" where="$3" text="$4" verify="$5"
  local hits n ln

  [ -f "${f}" ] || die "patch target ${f} does not exist"

  hits="$(grep -nE "${anchor}" "${f}" | cut -d: -f1 || true)"
  n="$(printf '%s' "${hits}" | grep -c . || true)"
  [ "${n}" -eq 1 ] || die "anchor /${anchor}/ matched ${n} lines in ${f} (need exactly 1)"

  ln="${hits}"
  [ "${where}" = before ] && ln=$((ln - 1))

  { head -n "${ln}" "${f}"; printf '%s\n' "${text}"; tail -n +"$((ln + 1))" "${f}"; } > "${f}.tmp"
  mv "${f}.tmp" "${f}"

  grep -qF "${verify}" "${f}" || die "patch applied to ${f} but '${verify}' is not there"
}

# ── The fixture dependency, as something FetchContent can clone ─────────────
FIXTURE="${WORK}/pubdep"
cp -r "${HARNESS}/fixture" "${FIXTURE}"
(
  cd "${FIXTURE}"
  git init -q -b main
  git add -A
  # -c user.*: CI runners have no git identity configured, and commit would fail.
  git -c user.email=verify@example.invalid -c user.name=verify commit -qm "fixture"
) > "${WORK}/fixture-git.log" 2>&1
FIXTURE_SHA="$(git -C "${FIXTURE}" rev-parse HEAD)"

# ── The fork: this working tree, patched the way a real one would be ────────
# Snapshotted rather than cloned, for the reason example/consumer/verify.sh
# gives: fetching from REPO_ROOT would clone HEAD, so on a dirty tree this would
# test something other than the change being made.
#
# The directory is named after this project, and that is load-bearing rather
# than tidy: the project takes its name from its directory, so a snapshot in a
# differently-named directory is a differently-named project — every
# -D<NAME>_TESTS=OFF below would silently miss, and the consumer would look for
# a package that does not exist. Same trap as FetchContent's <name>-src
# checkout, which example/consumer/CMakeLists.txt pins SOURCE_DIR to avoid.
FORK="${WORK}/${NAME}"
mkdir -p "${FORK}"
(
  cd "${REPO_ROOT}"
  git ls-files --cached --others --exclude-standard -z | xargs -0 cp -p --parents -t "${FORK}"
  git -C "${FORK}" init -q -b main
) > "${WORK}/fork-git.log" 2>&1

# Patch 1 — the recipe. A fork writes this file; we copy the annotated one.
cp "${HARNESS}/pubdep.cmake" "${FORK}/cmake/deps/pubdep.cmake"

# Patch 2 — activate it. The anchor is the same line cmake/check_artifacts.cmake
# parses for rules B1 and B2, so this project cannot change its shape without
# also going red there.
inject "${FORK}/CMakeLists.txt" \
  '^set\(\$\{PROJECT_NAME\}_DEPS' after \
  '  pubdep    # harness fixture — a public dependency (src/lib)' \
  'pubdep    # harness fixture'

# Patch 3 — link it into the exported library, publicly.
inject "${FORK}/src/lib/CMakeLists.txt" \
  '^  target_compile_features\(\$\{PROJECT_NAME\}_lib PUBLIC cxx_std_23\)' after \
  '
  target_link_libraries(${PROJECT_NAME}_lib PUBLIC pubdep::pubdep)' \
  'target_link_libraries(${PROJECT_NAME}_lib PUBLIC pubdep::pubdep)'

# Patch 4 — re-find it from the installed package config. Inserted above the
# include() of the generated Targets file, which is what names pubdep::pubdep.
inject "${FORK}/cmake/project-config.cmake.in" \
  '^include\("\$\{CMAKE_CURRENT_LIST_DIR\}/@PROJECT_NAME@Targets.cmake"\)' before \
  'include(CMakeFindDependencyMacro)
find_dependency(pubdep)
' \
  'find_dependency(pubdep)'

# Committed only now, with the patches in it: assertion E clones this fork, and
# a clone of the unpatched snapshot would have no public dependency to leak.
(
  git -C "${FORK}" add -A
  git -C "${FORK}" -c user.email=verify@example.invalid -c user.name=verify \
      commit -qm "a fork that has grown a public dependency"
) >> "${WORK}/fork-git.log" 2>&1
FORK_SHA="$(git -C "${FORK}" rev-parse HEAD)"

# ── A. The fork configures ──────────────────────────────────────────────────
# This is CT-15. Everything below only gets to run because this passed.
#
# Library only: it keeps the harness offline (the other three dependencies are
# gated on the components that are off) and it is the build a consumer performs.
FORK_BUILD="${WORK}/fork-build"
PREFIX="${WORK}/prefix"

if cmake -S "${FORK}" -B "${FORK_BUILD}" \
      -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
      "-D${NAME}_BUILD_BIN=OFF" "-D${NAME}_TESTS=OFF" \
      -DPUBDEP_URI="file://${FIXTURE}" \
      -DPUBDEP_TAG="${FIXTURE_SHA}" > "${WORK}/A-configure.log" 2>&1
then
  ok "A: a fork with a public FetchContent dependency configures"
else
  bad "A: the fork does not configure (#29 if the export set is the complaint)"
  tail -n 25 "${WORK}/A-configure.log" | sed 's/^/     | /'
fi

# Self-check, not an assertion: if a copy of the fixture were installed
# somewhere on the default search path, find_package would satisfy the recipe
# with an IMPORTED target, no build export set would be involved, and every
# assertion here would pass while testing nothing.
if [ -d "${FORK_BUILD}/_deps/pubdep" ]; then
  echo "     (self-check: the dependency arrived via FetchContent, not find_package)"
else
  die "the fixture was satisfied by find_package; this harness proves nothing that way"
fi

# Every -D above is namespaced with the project name, so one wrong name turns
# them all into no-ops that CMake reports and nothing else notices. The first
# version of this script hit exactly that, and quietly built the full test suite
# and three unwanted dependencies while claiming to be a library-only build.
if grep -q "Manually-specified variables were not used" "${WORK}/A-configure.log"; then
  die "the fork ignored options this harness set; see ${WORK}/A-configure.log"
fi

# ── B. It builds and installs ───────────────────────────────────────────────
# The install side is where CMake rewrites the reference to the dependency using
# its install export set — the half that works, and only because the recipe
# turned the dependency's install rules ON while we are top level.
if { cmake --build "${FORK_BUILD}" --parallel \
     && cmake --install "${FORK_BUILD}"; } > "${WORK}/B-install.log" 2>&1
then
  ok "B: it builds, and install(EXPORT) resolves the public dependency"
else
  bad "B: build or install failed"
  tail -n 25 "${WORK}/B-install.log" | sed 's/^/     | /'
fi

# ── C. The dependency's package came along ──────────────────────────────────
# Implied by D, kept because it fails one step earlier with a clearer message.
if [ -d "${PREFIX}/lib/cmake/pubdep" ]; then
  ok "C: the public dependency installed its own package config"
else
  bad "C: no package config for the dependency in the prefix"
fi

# ── D. A downstream project can consume the result ──────────────────────────
# Runs the binary rather than only linking it, for the reason
# example/consumer/verify.sh gives: the printed name is what proves the library
# was linked rather than merely found.
CONSUMER="${FORK}/example/consumer"
if cmake -S "${CONSUMER}" -B "${WORK}/consumer-fp" \
      -DCONSUMER_MODE=find_package \
      -DTEMPLATE_PROJECT_NAME="${NAME}" \
      -DCMAKE_PREFIX_PATH="${PREFIX}" > "${WORK}/D-consumer.log" 2>&1 \
   && cmake --build "${WORK}/consumer-fp" --parallel >> "${WORK}/D-consumer.log" 2>&1
then
  got="$("${WORK}/consumer-fp/consumer")"
  if [ "${got}" = "${NAME}" ]; then
    ok "D: find_package on the installed fork works and runs"
  else
    bad "D: consumer printed '${got}', expected '${NAME}'"
  fi
else
  bad "D: the installed package could not be consumed"
  tail -n 25 "${WORK}/D-consumer.log" | sed 's/^/     | /'
fi

# ── E. The dependency does not leak into a consumer's prefix ────────────────
# The other half of the recipe's install rule. Here the fork is a subproject, so
# its _INSTALL option is OFF, so the dependency's install rules must be OFF too.
# Without that, this dependency writes its headers, its package config and its
# README into a prefix belonging to a project two levels up.
#
# ⚠ FetchContent mode, not add_subdirectory mode, and that is not a preference.
# example/consumer/ passes EXCLUDE_FROM_ALL to add_subdirectory, and that flag
# makes CMake discard the whole subdirectory's install rules — so this assertion
# ran green there whether the dependency was leaking or not. It was checked by
# injecting the leak (N4) and watching it pass. FetchContent_MakeAvailable adds
# no such flag, and it is also what the README's consume-this-project section
# shows, so it is the path a real downstream project takes.
LEAK="${WORK}/leak"
if cmake -S "${CONSUMER}" -B "${WORK}/consumer-as" \
      -DCONSUMER_MODE=fetchcontent \
      -DTEMPLATE_PROJECT_NAME="${NAME}" \
      -DTEMPLATE_GIT_REPOSITORY="file://${FORK}" \
      -DTEMPLATE_GIT_TAG="${FORK_SHA}" \
      -DCMAKE_INSTALL_PREFIX="${LEAK}" \
      -DPUBDEP_URI="file://${FIXTURE}" \
      -DPUBDEP_TAG="${FIXTURE_SHA}" > "${WORK}/E-leak.log" 2>&1 \
   && cmake --build "${WORK}/consumer-as" --parallel >> "${WORK}/E-leak.log" 2>&1 \
   && cmake --install "${WORK}/consumer-as" >> "${WORK}/E-leak.log" 2>&1
then
  leaked="$(find "${LEAK}" -mindepth 1 2>/dev/null | sed "s|^${LEAK}/||" | sort || true)"
  if [ -z "${leaked}" ]; then
    ok "E: consuming the fork deposits nothing in the consumer's prefix"
  else
    bad "E: the consumed build installed files the consumer never asked for:"
    printf '%s\n' "${leaked}" | sed 's/^/     | /'
  fi
else
  bad "E: could not build or install the consumer in add_subdirectory mode"
  tail -n 25 "${WORK}/E-leak.log" | sed 's/^/     | /'
fi

# ── F. The build tree is not a half-working package ─────────────────────────
# There is no build-tree export any more (that is the #29 fix), so a build
# directory on CMAKE_PREFIX_PATH must be refused with a message that says why —
# not with 'include could not find requested file' from three files down.
if cmake -S "${CONSUMER}" -B "${WORK}/consumer-bt" \
      -DCONSUMER_MODE=find_package \
      -DTEMPLATE_PROJECT_NAME="${NAME}" \
      -DCMAKE_PREFIX_PATH="${FORK_BUILD}" > "${WORK}/F-buildtree.log" 2>&1
then
  bad "F: a build directory was accepted as an installed package"
# Whitespace is flattened before matching: CMake hard-wraps a NOT_FOUND_MESSAGE
# to its own width, and where the line breaks fall depends on the length of the
# path in it — so a plain grep for a phrase passes or fails according to how
# long mktemp -d made the working directory.
elif tr '\n' ' ' < "${WORK}/F-buildtree.log" | tr -s ' ' \
     | grep -q "no exported targets beside it"; then
  ok "F: a build directory is refused, with a directed reason"
else
  bad "F: a build directory was refused, but not by the guard in the package config"
  tail -n 25 "${WORK}/F-buildtree.log" | sed 's/^/     | /'
fi

echo "──────────────────────────────────────────────────────────"
echo "public-dep verify: ${pass} passed, ${fail} failed"
[ "${fail}" -eq 0 ]
