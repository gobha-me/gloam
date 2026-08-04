#pragma once
#include <cstdint>
#include <string_view>

/*
 * Auto Generated File do not edit
 */

constexpr std::string_view PROGRAM_NAME{"@PROJECT_NAME@"};
constexpr std::uint32_t VERSION_MAJOR{@VERSION_MAJOR@};
constexpr std::uint32_t VERSION_MINOR{@VERSION_MINOR@};
constexpr std::uint32_t VERSION_PATCH{@VERSION_PATCH@};
constexpr std::uint32_t VERSION_TWEAK{@VERSION_TWEAK@};   // commits since the tag (0 when on it)
constexpr bool          VERSION_DIRTY{@VERSION_DIRTY@};   // working tree had uncommitted changes at configure time

// The build's version in the spelling `git describe` reports the tag in:
// 'v' followed by the three components above. This is what gloam::version_string()
// returns and what `--version` prints.
//
// Substituted from the SAME CMake variables as VERSION_MAJOR/MINOR/PATCH rather
// than composed independently, so the string and the numbers cannot drift apart
// — test/00version/ asserts they agree, and that assertion is only cheap to keep
// true because there is one source here rather than two.
//
// TWEAK and DIRTY are deliberately NOT in it: this string is the released
// identity, and it stays exactly vMAJOR.MINOR.PATCH on every build, tagged or
// not. Anything that wants to tell one build of a tag from another reads those
// two constants directly.
constexpr std::string_view VERSION_STRING{"v@VERSION_MAJOR@.@VERSION_MINOR@.@VERSION_PATCH@"};
