# tty_writer.cpp is the only file in GLOAM that calls write(2), and it is here
# rather than in src/lib/ for the reason AGENTS.md rule 1 gives: producing bytes
# is not needing a terminal, writing them is. It joins this target and NOT bake
# or replay — those own file descriptors, but never a terminal.
#
# ── The three audio files, and why they are three rather than one (§9) ───────
#
# The same rule splits §9 the same way, and the split is the design rather than
# a filing convention:
#
#   sfx.cpp          synthesises the resident PCM arena. float32, so AGENTS.md
#                    rule 2 keeps it out of the library; deterministic integer
#                    arithmetic with no clock, no libm and no allocation.
#   voice_mixer.cpp  the work the real-time callback actually does. Knows
#                    nothing about RtAudio and reads no clock — `render` takes
#                    the time as a parameter.
#   audio_device.cpp the stream. THE ONLY TRANSLATION UNIT IN THIS TREE THAT
#                    INCLUDES RtAudio, enforced by the ctest case
#                    `rtaudio-boundary-single-module` (§9.1, §9.3).
#
# The first two are device-free on purpose: test/27sfxarena/ and
# test/28voicemix/ compile them directly, the way test/18ttywrite/ compiles
# tty_writer.cpp, which puts the entire float half of §9 under all eight
# sanitizer legs on machines that have no sound card — which is every machine
# this project builds on.
add_executable(${PROJECT_NAME}
  main.cpp
  tty_writer.cpp
  terminal_sink.cpp
  resident_plates.cpp
  terminal_compositor.cpp
  sfx.cpp
  voice_mixer.cpp
  audio_device.cpp
)

# PROJECT_SOURCE_DIR, not CMAKE_SOURCE_DIR: the latter is the top-level build's
# root, which belongs to someone else as soon as this project is consumed.
target_include_directories(${PROJECT_NAME}
  PRIVATE ${PROJECT_SOURCE_DIR}/include
)

# The simulation core, §9's output stream, and §4.8's terminal lifecycle.
# Both third-party targets remain private to this unexported executable.
#
# RtAudio reaches THIS TARGET AND NO OTHER. Neither it nor termforge reaches
# ${PROJECT_NAME}::lib (rule 1), _bake or _replay — these lines are what make
# that true rather than intended.
target_link_libraries(${PROJECT_NAME}
  PRIVATE ${PROJECT_NAME}::lib
  PRIVATE RtAudio::rtaudio
  PRIVATE termforge::lib
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
