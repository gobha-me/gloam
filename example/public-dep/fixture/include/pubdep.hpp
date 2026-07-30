// Public header of the stand-in dependency. Its only job is to be a real file
// with a real include directory behind it, so that the include path this target
// propagates is something the harness can find in an install prefix when it is
// checking whether the dependency leaked into someone else's tree.

#pragma once

namespace pubdep {

inline auto answer() -> int { return 42; }

}  // namespace pubdep
