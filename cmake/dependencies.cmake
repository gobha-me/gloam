# ── Dependency activation (declarative opt-in) ──────────────────────────────
# Includes exactly the recipes named in ${PROJECT_NAME}_DEPS (root CMakeLists),
# each mapping to cmake/deps/<name>.cmake. Nothing is fetched unless it's on the
# list, so a new project prunes dependencies by editing one line — not by
# deleting recipe files, and not by whatever happens to sit in cmake/deps/.
#   Add a dep:    drop cmake/deps/<name>.cmake AND add <name> to the list.
#   Remove a dep: delete <name> from the list (the recipe file may stay).

foreach(DEP IN LISTS ${PROJECT_NAME}_DEPS)
  set(_dep_recipe ${CMAKE_CURRENT_LIST_DIR}/deps/${DEP}.cmake)
  if (NOT EXISTS ${_dep_recipe})
    message(FATAL_ERROR
      "Dependency '${DEP}' is in ${PROJECT_NAME}_DEPS but recipe ${_dep_recipe} "
      "is missing. Add cmake/deps/${DEP}.cmake or drop '${DEP}' from the list.")
  endif()
  message(STATUS "Including dependency ${DEP} (${_dep_recipe})")
  include(${_dep_recipe})
endforeach()
