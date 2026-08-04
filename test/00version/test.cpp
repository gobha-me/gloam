// What the build says its version is — gloam::version_string().
//
// This test exists because of a defect that shipped in every tag from v0.1.0 to
// v0.8.0: version_string() was defined as `return PROGRAM_NAME.data()`, so it
// returned the *project name*. `gloam --version` printed `gloam`, and the banner
// read `GLOAM gloam - simulation core`. The CMake half was never wrong — the tag
// parsed cleanly and VERSION_MAJOR/MINOR/PATCH held 0/8/0 the whole time. The one
// function that all three call sites (src/bin/main.cpp x2, src/bin/replay.cpp,
// src/bin/bake.cpp) went through was.
//
// It survived eight releases because nothing asserted on the value, and the one
// check that looked at it — example/consumer/verify.sh — compared it to the
// project name and so *required* the bug in order to stay green. A test that
// pins wrong behaviour is worse than no test, which is why that check moved
// rather than merely gaining a friend here.
//
// Per AGENTS.md the failure matrix comes first: the things a version string must
// never be. Agreement with the numeric components is the last, least-interesting
// section.

#include <catch2/catch_test_macros.hpp>

#include "gloam/gloam.hpp"

#include <version.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace {

struct Parsed {
  bool          ok{false};
  std::uint32_t major{0};
  std::uint32_t minor{0};
  std::uint32_t patch{0};
};

// Strict `v<MAJOR>.<MINOR>.<PATCH>`: a literal 'v', then exactly three
// components separated by single dots, each a non-empty run of ASCII digits,
// with nothing before and nothing after.
//
// Hand-written rather than <regex> so that what "version-shaped" means is
// legible right here instead of encoded in a pattern language — and so the
// checker itself can be put under test below. A shape checker that silently
// accepts everything would wave the original bug straight through.
auto parse_version(std::string_view s) -> Parsed {
  Parsed out;

  if (s.empty() || s.front() != 'v') return out;
  s.remove_prefix(1);

  std::array<std::uint32_t, 3> components{};
  for (std::size_t i = 0; i < components.size(); ++i) {
    if (i > 0) {
      if (s.empty() || s.front() != '.') return out;  // missing / wrong separator
      s.remove_prefix(1);
    }

    std::size_t   digits = 0;
    std::uint32_t value  = 0;
    while (digits < s.size() && s[digits] >= '0' && s[digits] <= '9') {
      value = (value * 10U) + static_cast<std::uint32_t>(s[digits] - '0');
      ++digits;
    }
    if (digits == 0) return out;  // empty component

    s.remove_prefix(digits);
    components[i] = value;
  }

  if (!s.empty()) return out;  // trailing junk, including a stray newline

  out.ok    = true;
  out.major = components[0];
  out.minor = components[1];
  out.patch = components[2];
  return out;
}

// The string under test, as a view. version_string() promises NUL termination,
// so this is safe — and the first section below is what checks that promise
// before anything else leans on it.
auto version_view() -> std::string_view { return std::string_view{gloam::version_string()}; }

}  // namespace

// ── The checker, before the thing it checks ─────────────────────────────────
// If parse_version() is broken-permissive, every assertion below it is theatre.
TEST_CASE("the shape checker rejects what it must", "[version][failure]") {
  SECTION("the exact value of the original defect does not parse") {
    // The two spellings the bug actually produced: the project name in a normal
    // checkout, and the directory-derived name in a git worktree.
    CHECK_FALSE(parse_version("gloam").ok);
    CHECK_FALSE(parse_version("upbeat-chaum-624409").ok);
  }

  SECTION("structurally wrong strings do not parse") {
    CHECK_FALSE(parse_version("").ok);           // empty
    CHECK_FALSE(parse_version("v").ok);          // prefix only
    CHECK_FALSE(parse_version("0.8.0").ok);      // missing 'v'
    CHECK_FALSE(parse_version("V0.8.0").ok);     // wrong case
    CHECK_FALSE(parse_version("v0.8").ok);       // too few components
    CHECK_FALSE(parse_version("v0.8.0.1").ok);   // too many components
    CHECK_FALSE(parse_version("v0..0").ok);      // empty middle component
    CHECK_FALSE(parse_version("v0.8.").ok);      // empty trailing component
    CHECK_FALSE(parse_version("v.8.0").ok);      // empty leading component
    CHECK_FALSE(parse_version("v0x8x0").ok);     // wrong separators
    CHECK_FALSE(parse_version("v0.8.0-dirty").ok);
    CHECK_FALSE(parse_version("v0.8.0-5-gabc1234").ok);
    CHECK_FALSE(parse_version("v0.8.0\n").ok);   // trailing newline
    CHECK_FALSE(parse_version(" v0.8.0").ok);    // leading space
    CHECK_FALSE(parse_version("v0.8.0 ").ok);    // trailing space
  }

  SECTION("well-formed strings parse, and the components land in order") {
    const Parsed p = parse_version("v1.2.3");
    REQUIRE(p.ok);
    CHECK(p.major == 1U);
    CHECK(p.minor == 2U);
    CHECK(p.patch == 3U);

    // The tagless-fallback spelling (cmake/version.cmake's 0.0.0) is legal, not
    // a parse failure. It means "no tag reachable", which is a real build.
    CHECK(parse_version("v0.0.0").ok);
  }
}

// ── Failure matrix for version_string() itself ──────────────────────────────
TEST_CASE("version_string is never the project name", "[version][failure]") {
  // THE regression. Kept as its own case, and first, because it is the specific
  // thing that shipped eight times.
  CHECK(version_view() != PROGRAM_NAME);

  // Stronger than inequality: the project name must not appear anywhere in it.
  // Catches a "fix" that concatenates rather than replaces, e.g. "gloam v0.8.0",
  // which would make the banner read `GLOAM gloam v0.8.0 - simulation core`.
  CHECK(version_view().find(PROGRAM_NAME) == std::string_view::npos);
}

TEST_CASE("version_string is a usable C string", "[version][failure]") {
  const char* const raw = gloam::version_string();

  SECTION("not null") {
    REQUIRE(raw != nullptr);
  }

  SECTION("not empty") {
    // An empty string would still satisfy "not the project name" while making
    // `--version` print a blank line.
    CHECK(std::strlen(raw) > 0);
  }

  SECTION("NUL-terminated where the view ends") {
    // The header documents a NUL-terminated string; every call site hands it to
    // printf("%s") or operator<<, both of which read until NUL.
    CHECK(std::strlen(raw) == version_view().size());
  }

  SECTION("stable across calls") {
    // It is a view over a literal, so repeated calls must agree. A future body
    // that formatted into a static buffer would be a data race waiting for the
    // sanitizer matrix; this is the cheap smoke check for that shape change.
    CHECK(std::strcmp(gloam::version_string(), gloam::version_string()) == 0);
  }
}

TEST_CASE("version_string carries no formatting the callers would double up on", "[version][failure]") {
  const std::string_view v = version_view();

  // src/bin/main.cpp does printf("%s\n", ...) and embeds the value mid-sentence
  // in the banner. Any embedded whitespace or newline corrupts both.
  CHECK(v.find('\n') == std::string_view::npos);
  CHECK(v.find('\r') == std::string_view::npos);
  CHECK(v.find(' ') == std::string_view::npos);
  CHECK(v.find('\t') == std::string_view::npos);
}

TEST_CASE("version_string is version-shaped", "[version][failure]") {
  // The umbrella assertion, and the one that would have caught the defect on
  // its own: `gloam` does not parse as vMAJOR.MINOR.PATCH.
  INFO("version_string() = '" << version_view() << "'");
  CHECK(parse_version(version_view()).ok);
}

// ── Agreement: the string and the numbers are one fact, not two ─────────────
TEST_CASE("version_string agrees with the numeric components", "[version]") {
  const Parsed p = parse_version(version_view());
  REQUIRE(p.ok);

  // version.hpp.in.cmake substitutes both from the same CMake variables, so a
  // mismatch here means the template grew a second source of truth.
  CHECK(p.major == VERSION_MAJOR);
  CHECK(p.minor == VERSION_MINOR);
  CHECK(p.patch == VERSION_PATCH);

  // And the function hands back the header's constant rather than a literal of
  // its own — the failure this catches is a hardcoded version in gloam.cpp that
  // stops tracking the tag.
  CHECK(version_view() == VERSION_STRING);
}

TEST_CASE("version_at_least agrees with version_string", "[version]") {
  // The two halves of the public API describe the same build, so they must not
  // be able to disagree. Written against the parsed string rather than against
  // the macros, so this stays a cross-check and not a restatement.
  const Parsed p = parse_version(version_view());
  REQUIRE(p.ok);

  SECTION("the build is at least itself") {
    CHECK(gloam::version_at_least(p.major, p.minor, p.patch));
  }

  SECTION("anything strictly newer is refused") {
    CHECK_FALSE(gloam::version_at_least(p.major, p.minor, p.patch + 1));
    CHECK_FALSE(gloam::version_at_least(p.major, p.minor + 1, 0));
    CHECK_FALSE(gloam::version_at_least(p.major + 1, 0, 0));
  }

  SECTION("a larger lower component never rescues a smaller higher one") {
    // The property the header claims by name. Guarded, because the current
    // version legitimately has zero components and unsigned 0 - 1 is not a
    // smaller version, it is 4294967295.
    if (p.major > 0) {
      CHECK(gloam::version_at_least(p.major - 1, p.minor + 1, p.patch + 1));
    }
    if (p.minor > 0) {
      CHECK(gloam::version_at_least(p.major, p.minor - 1, p.patch + 1));
    }
    if (p.patch > 0) {
      CHECK(gloam::version_at_least(p.major, p.minor, p.patch - 1));
    }
  }
}

// ── Happy path, last and least interesting ─────────────────────────────────
TEST_CASE("version_string reads the way a release does", "[version]") {
  // The smoke check: what a human running `gloam --version` actually sees.
  INFO("banner would read: GLOAM " << version_view() << " - simulation core");
  CHECK(version_view().front() == 'v');
  CHECK(version_view().size() >= 6);  // shortest legal form is "v0.0.0"
}
