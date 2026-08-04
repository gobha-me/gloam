// A downstream project, in miniature. Its whole job is to prove that this
// template's library can be *consumed* — the same way a real project would use
// it, and identically across all three acquisition modes (see verify.sh).
//
// It prints two lines, and they check different things — see verify.sh:
//
//   1. the project name, out of the generated header, which catches the
//      consumer linking a *different* project than it meant to;
//   2. the version, out of the library's archive, which is what makes this a
//      link test rather than a compile test: version_string() is declared in the
//      public header and defined in the library's translation unit, so a build
//      that gets the include directory but not the archive fails at link. That
//      distinction is the reason include/lib.hpp exists at all — a header of
//      pure constexpr would compile and "pass" while linking nothing.
//
// The two used to be ONE line, because version_string() wrongly returned the
// project name and this check compared it to the directory name. That made the
// consumer gate the thing keeping a real bug alive: fixing version_string()
// turned it red. Splitting the two properties apart is what stops one from
// being able to hold the other hostage again.

#include <gloam/gloam.hpp>

// The generated header, for PROGRAM_NAME. cmake/install.cmake installs it
// deliberately and says why; this is the consumer that proves the claim.
#include <version.hpp>

#include <cstdio>

// The library advertises cxx_std_23 as a PUBLIC usage requirement, so a
// consumer inherits it even though it never set CMAKE_CXX_STANDARD and never
// used this project's toolchain files. Delete that line from
// src/lib/CMakeLists.txt and this is what goes red.
//
// Checked with a C++23 language feature-test macro rather than with
// __cplusplus, because that value is not a reliable "is this C++23" signal:
// CMake maps cxx_std_23 to -std=c++2b on GCC 13, where __cplusplus is 202100L
// rather than 202302L. The first version of this check asserted >= 202302L,
// passed on the Clang 20 job and on a GCC 14 workstation, and went red on CI's
// GCC 13 — which is the template's own documented floor. An undefined macro
// evaluates to 0 in #if, so this catches "the requirement never arrived" too.
#if __cpp_if_consteval < 202106L
#error "the library's cxx_std_23 usage requirement did not reach the consumer"
#endif

auto main() -> int {
  // Line 1 — constexpr, from the installed header. verify.sh compares it to the
  // repo directory name, which is what catches FetchContent's <name>-src
  // checkout renaming a directory-derived project out from under us.
  std::printf("%s\n", PROGRAM_NAME.data());

  // Line 2 — a real call into the archive. verify.sh checks it is version-shaped
  // rather than checking it equals anything: the version legitimately differs
  // between the three modes (the FetchContent snapshot has no tags, so it takes
  // the 0.0.0 fallback), and pinning an exact string would just re-create the
  // over-tight assertion this file used to carry.
  std::printf("%s\n", gloam::version_string());

  // A third call, through the other half of the public API, so the link check is
  // not one symbol wide.
  return gloam::version_at_least(0, 0, 0) ? 0 : 1;
}
