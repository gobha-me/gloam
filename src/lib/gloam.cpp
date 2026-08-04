#include "gloam/gloam.hpp"

#include <version.hpp>

namespace gloam {

// The version, not the project name. This returned PROGRAM_NAME for the first
// eight tags, which made `gloam --version` print `gloam` and the banner read
// `GLOAM gloam - simulation core`. Every call site was already correct; the one
// function all three of them shared was not. test/00version/ is what keeps it
// that way.
//
// .data() is NUL-terminated here because VERSION_STRING is a view over a string
// literal in the generated header — the same reason the old body was allowed to
// hand out PROGRAM_NAME.data().
auto version_string() -> const char* { return VERSION_STRING.data(); }

auto version_at_least(std::uint32_t major, std::uint32_t minor, std::uint32_t patch) -> bool {
  // Component-major-first: each component settles the answer outright unless it
  // ties, in which case the next is consulted. Written as explicit comparisons
  // rather than by packing the three into one integer — packing looks tidier
  // and silently gives the wrong answer the moment a component outgrows the
  // field width it was assigned.
  if (VERSION_MAJOR != major) return VERSION_MAJOR > major;
  if (VERSION_MINOR != minor) return VERSION_MINOR > minor;
  return VERSION_PATCH >= patch;
}

}  // namespace gloam
