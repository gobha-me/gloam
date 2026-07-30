# pubdep

A stand-in for a real public dependency, used by `example/public-dep/verify.sh`.
It is not a general-purpose library and nothing outside that harness should
depend on it.

This file exists to be installed. The fixture's `CMakeLists.txt` installs it to
`${CMAKE_INSTALL_DOCDIR}`, which resolves against the **top-level** project's
name — so when this fixture is built as a subproject, this file lands under the
consuming project's documentation directory rather than under one of its own.
The harness asserts on exactly that, because it is the failure a real dependency
produces when its install rules are left switched on inside someone else's build.
