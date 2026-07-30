#!/usr/bin/env bash
# ── Consumer acceptance check (CT-04 / #5) ──────────────────────────────────
# Builds example/consumer/ against this project three ways — add_subdirectory,
# FetchContent, and an installed find_package — and runs the result each time.
#
#   example/consumer/verify.sh                 # $CXX or the platform default
#   CXX=clang++ example/consumer/verify.sh     # the other compiler
#
# Every mode must produce a binary that prints the project name. Printing it is
# the point: version_string() is defined in the library's translation unit, so a
# consumer that got the headers but not the archive fails to link rather than
# passing while testing nothing.
#
# Everything is built under mktemp -d, so this leaves no build dirs behind and
# needs no .gitignore entry.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CONSUMER_DIR="${REPO_ROOT}/example/consumer"
NAME="$(basename "${REPO_ROOT}")"

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

pass=0
fail=0

# Run one mode end to end: configure, build, execute, compare stdout.
# Extra -D arguments for the mode are passed after the mode name.
run_mode() {
  local mode="$1"; shift
  local build="${WORK}/build-${mode}"

  echo "── ${mode} ───────────────────────────────────────────────"

  if cmake -S "${CONSUMER_DIR}" -B "${build}" \
        -DCONSUMER_MODE="${mode}" \
        -DTEMPLATE_PROJECT_NAME="${NAME}" \
        "$@" > "${build}.log" 2>&1 \
     && cmake --build "${build}" --parallel >> "${build}.log" 2>&1
  then
    local got
    got="$("${build}/consumer")"
    if [ "${got}" = "${NAME}" ]; then
      echo "PASS ${mode}: consumer printed '${got}'"
      pass=$((pass + 1))
      return 0
    fi
    # A wrong name is not a near-miss. It means the consumer linked a different
    # project than it meant to — the classic cause being FetchContent's
    # <name>-src checkout directory renaming a directory-derived project.
    echo "FAIL ${mode}: expected '${NAME}', got '${got}'"
  else
    echo "FAIL ${mode}: configure or build failed"
    tail -n 30 "${build}.log" | sed 's/^/     | /'
  fi

  fail=$((fail + 1))
  return 0
}

# ── Mode 1: add_subdirectory ────────────────────────────────────────────────
run_mode add_subdirectory -DTEMPLATE_SOURCE_DIR="${REPO_ROOT}"

# ── Mode 2: FetchContent ────────────────────────────────────────────────────
# A real consumer writes a public URL and a tag. We point at a throwaway repo
# built from the *working tree* instead, which keeps this offline and — the part
# that matters — tests the code you are about to commit.
#
# Fetching from ${REPO_ROOT} directly would clone HEAD, so on a dirty tree this
# mode would quietly build different sources than the other two and report a
# failure that has nothing to do with your changes. That is not a caveat worth
# documenting; it is a mode worth fixing.
#
# The snapshot has no tags, so the template falls back to version 0.0.0 with a
# reason — which incidentally exercises the tagless path for free.
SNAPSHOT="${WORK}/snapshot"
mkdir -p "${SNAPSHOT}"
(
  cd "${REPO_ROOT}"
  # Tracked files plus new, non-ignored ones: what a commit right now would hold.
  git ls-files --cached --others --exclude-standard -z | xargs -0 cp -p --parents -t "${SNAPSHOT}"
  git -C "${SNAPSHOT}" init -q -b main
  git -C "${SNAPSHOT}" add -A
  # -c user.*: CI runners have no git identity configured, and commit would fail.
  git -C "${SNAPSHOT}" -c user.email=verify@example.invalid -c user.name=verify \
      commit -qm "working tree snapshot"
) > "${WORK}/snapshot.log" 2>&1

run_mode fetchcontent \
  -DTEMPLATE_GIT_REPOSITORY="file://${SNAPSHOT}" \
  -DTEMPLATE_GIT_TAG="$(git -C "${SNAPSHOT}" rev-parse HEAD)"

# ── Mode 3: installed find_package ──────────────────────────────────────────
# Install the library only. The app and tests are off by their own options here,
# which doubles as proof that a library-only install is a supported build.
PREFIX="${WORK}/prefix"
{
  cmake -S "${REPO_ROOT}" -B "${WORK}/build-install" \
    -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
    "-D${NAME}_BUILD_BIN=OFF" "-D${NAME}_TESTS=OFF"
  cmake --build "${WORK}/build-install" --parallel
  cmake --install "${WORK}/build-install"
} > "${WORK}/install.log" 2>&1 || {
  echo "FAIL find_package: could not build and install the project"
  tail -n 30 "${WORK}/install.log" | sed 's/^/     | /'
  fail=$((fail + 1))
}

if [ -d "${PREFIX}" ]; then
  run_mode find_package -DCMAKE_PREFIX_PATH="${PREFIX}"
fi

echo "──────────────────────────────────────────────────────────"
echo "consumer verify: ${pass} passed, ${fail} failed  (CXX=${CXX:-default})"
[ "${fail}" -eq 0 ]
