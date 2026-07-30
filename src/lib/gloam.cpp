#include "gloam/gloam.hpp"

#include <version.hpp>

namespace gloam {

auto version_string() -> const char* { return PROGRAM_NAME.data(); }

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
