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

# ── The golden-replay harness (SPEC §12, §19 step 7) ────────────────────────
#
# Also a separate executable, and for the mirror of bake's reason. §5.1's
# replayability rests on the simulation being unable to reach a file descriptor
# or a clock; a `--record` switch on ${PROJECT_NAME} would put the only `read`
# and `write` in the replay path inside the process that must not have one.
#
# It exists as a binary rather than only as a test case because §19 step 7 is an
# ACROSS-process claim: test/13replay/ replays inside one address space, where a
# shared allocator and one set of already-touched pages can hide an
# uninitialised read that a fresh process would expose.
add_executable(${PROJECT_NAME}_replay
  replay.cpp
)

target_include_directories(${PROJECT_NAME}_replay
  PRIVATE ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(${PROJECT_NAME}_replay
  PRIVATE ${PROJECT_NAME}::lib
)
