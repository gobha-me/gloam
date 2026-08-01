add_executable(${PROJECT_NAME}
  main.cpp
)

# PROJECT_SOURCE_DIR, not CMAKE_SOURCE_DIR: the latter is the top-level build's
# root, which belongs to someone else as soon as this project is consumed.
target_include_directories(${PROJECT_NAME}
  PRIVATE ${PROJECT_SOURCE_DIR}/include
)

# The simulation core. termforge joins this line when the terminal layer starts
# — see the dependency note in the top-level CMakeLists.txt.
target_link_libraries(${PROJECT_NAME}
  PRIVATE ${PROJECT_NAME}::lib
)

# ── The offline asset pipeline (SPEC §10) ───────────────────────────────────
#
# A SEPARATE EXECUTABLE, not a flag on the game. §10 is explicit that nothing is
# generated at runtime and no model ships in the binary; a `--bake` switch on
# ${PROJECT_NAME} would make that a convention rather than a fact. This target
# holds the only file descriptor in the pipeline — everything it calls in
# ${PROJECT_NAME}::lib works on caller-owned spans.
add_executable(${PROJECT_NAME}_bake
  bake.cpp
)

target_include_directories(${PROJECT_NAME}_bake
  PRIVATE ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(${PROJECT_NAME}_bake
  PRIVATE ${PROJECT_NAME}::lib
)
