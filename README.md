# GLOAM

[![CI](https://github.com/gobha-me/gloam/actions/workflows/ci.yml/badge.svg)](https://github.com/gobha-me/gloam/actions/workflows/ci.yml)

A 1-bit grid dungeon crawler where your lamp is the thing that lets you see and
the thing that gets you seen — and the renderer transmits zero pixels during
play.

GLOAM is a terminal game built on [termforge](https://github.com/gobha-me/termforge)
and the kitty graphics protocol. It does not raycast. Like *Eye of the Beholder*
and *Dungeon Master* before it, it is a sprite compositor: there is a fixed set
of wall positions relative to the viewer, each with a pre-drawn, pre-scaled
plate. Every plate is transmitted **once**, at startup, and stays resident. A
frame during play is a list of placement commands — no PNG payloads, no base64
through the tty. An idle frame costs **zero bytes**, which is what makes the
whole thing playable over ssh.

## Status

**Pre-M0.** There is no playable game yet, and the `gloam` binary is a headless
diagnostic rather than a game. What exists is the deterministic simulation core:

| Built | SPEC |
| --- | --- |
| Geometry ladder, screen layout, the reflow rules | §3 |
| Named RNG streams, portable across GCC and Clang | §5.1 |
| Noise emission, per-edge attenuation, propagation | §6.2 |
| Awareness state machine and its five authored tells | §6.1 |
| Light, sight and the doused-party pillar | §6.3 |
| Rune grammar, spell resolver, danger classes | §8 |
| Budgets, wired as assertions before the code they constrain | §11 |
| The compositing bands, and the kitty call boundary over them | §4.5, §16 |
| `replay.gloam`, the world hash, and the golden-replay harness | §12, §13.2 |
| The voice ring, the gain/pan mix, and the wall the sim talks through | §9.2, §9.3 |
| The frame classes, the sustained percentile, and the one write syscall | §11 |
| Patrol routes, the movement pump, and the monster you can hear | §6.4, §9 |
| The pathfinder, the pursuit, and the walk home | §6.1, §5.2 |
| The transmit path: indexed PNG, DEFLATE, and the cold-start upload | §4.1, §10, §11 |

The **compositor** is still blocked upstream — see [UPSTREAM.md](UPSTREAM.md).
termforge stretches a placed image to fill its cell rect and states that scaling
is the contract, which §3.2 rules out by name because resampling a pre-dithered
plate reintroduces the dither crawl the whole pipeline exists to avoid. So
[#7](https://github.com/gobha-me/gloam/issues/7) has nothing to sit on yet.

The layer API is built anyway, and deliberately so: §16's mitigation for that
exact risk is to keep every kitty call behind GLOAM's own boundary from day one,
"so a vendored driver is a swap and not a rewrite". A boundary built after the
driver arrives is not insurance. Two ctest cases keep it honest —
`layer-z-single-definition` (no code hand-writes a z-index) and
`kitty-boundary-single-module` (no code outside `src/lib/kitty.cpp` writes an
escape sequence).

The **asset pipeline** ([#1](https://github.com/gobha-me/gloam/issues/1)) has
landed its first slice, and it is the one thing on the critical path that never
needed termforge. `gloam_bake` writes a versioned, hashed `pack.gloam`: §12's
manifest, §4.3's fixed ordered dither, §3.1's exact 2:1 downsample, and the six
full-frame light fields §4.4 asks for — the one asset class §10 marks
*procedural*, so it could be built before any art exists. Two runs produce a
byte-identical pack, verified under GCC 13, GCC 14 and Clang 20; the
`pack-reproducible` ctest case runs the binary twice and compares the files,
because §10 makes that hash a build gate rather than a nicety.

What that slice does **not** include is the authored depth-0 and depth-1 wall
rings, because no art and no authoring format exist yet. #1 stays open for them.

It also put a number on something uncomfortable: §11's 1.2 MB cold-start budget
is for the **base64 transmit payload**, and a plate expands to RGBA before it
goes on the wire. The six light fields are 388,800 B in the pack and would have
been roughly 5.5 MB transmitted — 4.6× over, from the six procedural plates
alone. That was [#17](https://github.com/gobha-me/gloam/issues/17), and the
transmit path below is what closed it.

The **replay harness** ([#3](https://github.com/gobha-me/gloam/issues/3)) has
landed, and with it build-order step 7's acceptance criterion: *a recorded
session replays to an identical world hash on both compilers.* `gloam_replay`
records a session to a versioned, hashed `replay.gloam` and replays it in a
different process; `replay-deterministic` is the ctest case that does exactly
that and compares. A replay recorded against different tuning is **rejected** at
load rather than mis-played, because §12 is explicit that otherwise a golden
test starts passing for the wrong reason — while a `pack_hash` mismatch only
warns, because art cannot affect the simulation.

That needed something the tree did not have: an aggregate to hash. `gloam::World`
is deliberately the smallest one that can be replayed — a party coordinate, a
lamp level, a level, N monsters, the eight RNG states — and it is exactly what
the reference tick in `test/10budgets/` was already assembling by hand. It does
not open §7's party or §8's magic; BUILD-ORDER step 10 is explicit that those
wait for the M0 gate. The budget case now calls the same `advance()` the replay
does, so the measured tick and the replayed tick cannot drift apart.

The **audio sink** ([#4](https://github.com/gobha-me/gloam/issues/4)) has landed
its first half, and it is the half that makes the subsystem safe. Build-order
step 9's acceptance criterion is *`--mute` and unmuted runs produce identical
replays*, and `audio-mute-identical` is the ctest case that records and replays
the same session both ways in separate processes and compares the bytes as well
as the hashes — while also insisting the unmuted run actually made a sound, because
an identity proved over silence is not evidence.

That property holds by construction rather than by care. `Sink::play` returns
`void` and takes scalars by value, so there is no expression in `advance` that
can read anything back; §9.2's *"Audio → sim is nothing. Ever."* is closed by the
type system. `World` gained no field for §9, so `kWorldHashVersion` did not move
and **no recorded replay was invalidated by the audio slice** — `ruleset_hash`,
the golden world hash and `replay.gloam`'s bytes were all unchanged from the
commit before. (§6.4 below *does* move both; the two are separate properties, and
the `--mute` identity survives it because the patrol RNG draw is taken
unconditionally rather than behind `voices != nullptr`.)

What is in `gloam::lib` is the SPSC ring, the `VoiceCommand` type and the
arithmetic that turns §6.2's propagation into a gain and a pan — integers and
atomics, no device. That placement is the point: the ring is lock-free between a
simulation thread and a real-time callback, and keeping it library-side is what
puts it under the sanitizer matrix. Weakening the release store in `try_push`
turns `15voicering-test` into a reported data race under TSan; that was checked,
not assumed.

§9.3's *"one clever part"* is real and now load-bearing: gain comes from the
**same** `propagate_noise` monsters hear through, so retuning `atten_closed_door`
moves the stealth model and the mix together. The first implementation propagated
once per stinging monster and cost **13.6 ms** on a tick where all sixteen sting,
against §11's 4 ms budget. §9.3's actual wording — *evaluated from the party's
position instead of the monster's* — is one field per tick rather than one per
monster, and noise cost is symmetric, so the two readings agree; that is 0.97 ms,
and `test/10budgets/` measures exactly that tick.

The **byte instruments** ([#6](https://github.com/gobha-me/gloam/issues/6)) have
landed the half that does not need a compositor, which is build-order step 4's
*"counters on the emit path… printed every run"*. `gloam::meter` classifies a
frame and computes §11's sustained percentile; `src/bin/tty_writer.hpp` holds the
process's **only** `write` syscall and reports what the kernel accepted, which is
what `BUDGETS.md` asks for and what a `std::string` counter structurally cannot
answer. `gloam --quiet` prints the instrument block, because a flag that silences
an instrument makes the instrument optional.

Two things the design had never decided had to be decided to write any of it, and
both are recorded rather than absorbed: **how a frame's class is determined**
([#24](https://github.com/gobha-me/gloam/issues/24) — on the state delta, not the
input event, because `apply()` already refuses a step into a wall and an input log
must not be able to manufacture its own budget class) and **what a p95 of a total
means** ([#25](https://github.com/gobha-me/gloam/issues/25) — nearest rank over
sliding one-second windows, in integers).

Then the harness measured something worth having:

| Row | Measured | Budget | |
| --- | --- | --- | --- |
| Full recomposition, 24 placements | 1,596 B | 2,048 B | passes |
| Animation-only frame | 137 B | 400 B | passes |
| Idle frame | 0 B | 0 B | passes |
| Sustained p95 | **8,459 B/s** | 8,192 B/s | **3.3% over** |

§4.7's 140 ms step transition and §11's 8 KB/s sustained p95 are **mutually
unreachable** at §4.1's own placement count and the current wire form — and the
measured figure is an *upper bound*, taken with no diff at the fastest walk 10 Hz
can express. [#26](https://github.com/gobha-me/gloam/issues/26) carries the
arithmetic, the one assumption the bound rests on (the compositor must reuse
placement ids per slot), and the escape: dropping kitty's defaulted crop keys
saves 20 of a placement's 66 bytes and puts the row back inside budget with room.
`test/10budgets/` asserts the overrun **inverted**, the way #17's cold-start row
is asserted, so it goes red the day someone fixes it.

The **transmit path** is the other half of #6, and it closed
[#17](https://github.com/gobha-me/gloam/issues/17). §4.1 puts every plate on the
wire once, at startup, and §11 budgets 1.2 MB of base64 for it. The naive route
does not fit and never could: kitty is handed pixels, so a 480 × 360 plate goes
out as 691,200 B of RGBA, and the six procedural light fields alone are 5.5 MB
encoded — which is why that row spent four slices asserted **inverted**, true in
the direction it was broken, so that fixing it would go red.

What ships instead is `f=100` — §10's plate as a **4-bit indexed PNG**, five
palette entries (four inks plus transparent), `tRNS` one byte long, compressed by
GLOAM's own DEFLATE. Measured on the real stream, not projected from constants:

| | Bytes | Against |
| --- | --- | --- |
| Six light fields, in the pack | 388,800 | — |
| Encoded as PNG | 12,808 | — |
| The whole APC stream, control data included | **17,284** | 1,200,000 |
| The `f=32` route it replaces | 5,529,600 | 461% of budget |

A factor of 320, and the row now carries a **headroom band** at eight to one —
because six plates are 6 of M0's 71, and a row that only just fits today is a row
that has already failed. The other two cold-start rows are measured too: 78 ms
local against 800, and 216 ms modelled over a 1 Mbit/s link against 12 s, both
labelled in the case banner as GLOAM's half only.

The compressor is GLOAM's rather than a linked zlib for the reason `test/25png/`
pins a `sha256` over an encoded light field: that digest is worth something only
if the encoder is the one in this tree. It is fixed-Huffman and greedy, with a
**hash chain capped by a named constant** — a light field's scanline is 86,760
bytes of near-uniform screen door whose three-byte prefixes nearly all collide,
and an uncapped chain there is quadratic, so `24deflate-test` carries a ctest
timeout the way `15voicering-test` does.

Two things kept it from being self-confirming. The inflater that verifies the
round trip lives in `test/include/`, was written from RFC 1951 rather than from
the encoder, and is itself **anchored on three streams a real zlib produced**. And
the chunking path — which no plate GLOAM ships today exercises, since every one
fits in a single 3,072-byte chunk — is tested synthetically at the boundaries,
because chunking the base64 *output* instead of the raw *input* emits `=` padding
mid-stream, which kitty decodes to garbage rather than refusing.

The pack stays `RawPlanes`: `pack::Codec::Png` is still refused
([#16](https://github.com/gobha-me/gloam/issues/16)), because a PNG inside the
pack would put the compressor, the palette and the filter choice inside
`pack_sha256` — a build gate that has nothing to do with any of them.

**Monsters move.** §6.4's patrol routes are the one thing left on the critical
path that never needed termforge, and #8's gate is a sentence about a monster
crossing an intersection — so a monster that could not cross one could not answer
it. `gloam` now ticks a real world in its trace and shows exactly that: doused,
the thing walks past the intersection twice and never knows you are there; light
the lamp and on the next crossing it halts, turns its head toward you, and
escalates. One `lamp_level`, both behaviours.

The slice's boundary is one sentence — *it moves monsters along authored routes
and turns their heads; it never translates a monster toward the party* — which
maps exactly onto "needs no pathfinder". Three of §6.1's five tells are a walk
toward a target, and §6 never says how a monster paths; that, plus the fact that
**arrival has no defined outcome** at M0 (no combat, no collision rule, no
cell-occupancy rule), is [#32](https://github.com/gobha-me/gloam/issues/32). A
monster that pursues you and then stands *on* you is a more visible dead end than
one that holds position. The two tells that are only a head turn — §6.1's
`PatrolRhythmBreaks` and `CastsAbout` — *are* performed, and `Monster::facing` is
hashed state for that reason: a tell a replay cannot reproduce is not a tell.

§6.4 is **three sentences**, so four things had to be decided rather than read,
and each is `UPSTREAM.md` items 14–17 with a mirrored issue: the route
representation and re-join rule ([#28](https://github.com/gobha-me/gloam/issues/28)
— a full cell-by-cell path, ping-pong, validated by `valid_route` at load and not
in the tick); §5.2's unnamed movement rate
([#29](https://github.com/gobha-me/gloam/issues/29) — `monster_move_ticks = 2`,
the fastest step 10 Hz can express); the monster footfall §6.2 has no row for
([#30](https://github.com/gobha-me/gloam/issues/30) — diegetic at 14, a leather
step, and *not* a `Tuning` field, because nothing in the simulation hears it);
and what "idle variation" means
([#31](https://github.com/gobha-me/gloam/issues/31) — a bounded extension of an
*authored* dwell, so a corridor leg takes no draw and a monster with no authored
pause is a metronome).

This is the first subsystem in the tree to draw from an RNG stream, which §6.4
requires be `patrol` and "never the shared one" — asserted rather than assumed:
after 1000 patrolling ticks, only `rng_state[Stream::Patrol - 1]` has moved.
`kWorldHashVersion` goes 1 → 2 and `kTuningFieldCount` 47 → 49, so every recorded
replay is invalidated, deliberately. `advance` never calls `valid_route`; what
makes that safe is that the pump steps only through `Level::walk` **and** requires
the destination to be the waypoint the route named, so malformed data yields a
monster that stands still rather than one that drifts or teleports. Fourteen
refusals in `test/19patrol/` are what turn that from a claim into a guarantee.

**And now it comes after you.** §6.1 calls the awareness tells "the deliverable,
not the state machine", and three of the five translate a monster toward a
target — *"leaves the patrol route, walks to the last known position"*,
*"direct pursuit"*, *"walks back to the patrol route and resumes"*. §6 never
says how a monster paths, and §5.2's *"same AI, same patrols, **same
pathfinding**"* was the only mention of the word in the document: an assertion
that two play modes share a thing that did not exist.

`gloam`'s corridor trace now runs the whole §6.1 cycle rather than the first
third of it — it patrols, notices you, leaves the route, closes to arm's reach,
loses you when you back away, gives up, and walks home to resume the ping-pong:

```
  tick  monster  facing  awareness    lamp  you      sees you
  13    (3,2)    north   unaware      0     (1,2)    no   <- crossing
  15    (3,2)    west    suspicious   3     (1,2)    yes  <- crossing
  16    (2,2)    west    searching    3     (1,2)    yes  <- off its route
  17    (2,2)    west    hunting      3     (1,2)    yes  <- off its route
  ...
  27    (2,2)    west    hunting      0     (1,2)    yes  <- off its route
  ...
  33    (2,2)    east    hunting      0     (4,2)    no   <- off its route
  40    (2,2)    south   lost-track   0     (5,2)    no   <- off its route
  80    (3,2)    east    unaware      0     (5,2)    no   <- crossing
```

(Abridged — the binary prints one row per *event*, twenty-two of them, and
carries on past tick 80 to show the ping-pong resume.)

Two rows there are the mechanic rather than the plumbing. **Tick 27: the lamp is
out and it still sees you**, because §6.3 keeps an unlit party visible at an
adjacent cell — dousing does not save you from something already at arm's reach,
and getting clear is what breaks the contact. **Tick 17: it stops one cell
short.** That is the arrival rule, and it is the design decision
[#32](https://github.com/gobha-me/gloam/issues/32) said had to be made before any
code: there is no combat, no death and no cell-occupancy model at M0, so a
monster standing *on* you is a more visible dead end than one that stands next to
you and waits. When §7 lands, that halt becomes the attack and the pathing does
not change.

The primitive is a uniform-cost BFS over `Level::walk`, rooted at the **target**
so that one search serves the whole roster — and pointedly not `propagate_noise`,
which is a Dijkstra over `conducts_sound()`. A closed door is impassable and
audible at once (§12's one graph, two readings), so a pathfinder that reused the
noise search would walk a monster through a shut door. `test/21path/` pins that
pair at §6.2's own numbers, along with the property that kills the greedy
shortcut #32 rejected: every reached cell has a neighbour exactly one step
closer, so a descent always exists and no pocket can trap anything.

§6.1 also gained a row it shipped without — SEARCHING → LOST_TRACK — because
pursuit's only other terminal state is a monster stranded on a stale cell for the
rest of the session. It costs **no new tunable and no new tell**:
`hunting_lost_ticks` already means "N ticks with no perception hit", and
`Tell::CastsAbout` is already *"casts about at the last position"* — a phrase
`world.cpp` recorded as vacuous until something could leave the spot it started
on. Two more decisions were made rather than read, and both are in `UPSTREAM.md`
item 18: monsters do not collide with each other, and leaving a route discards
the pause it owed.

**The determinism impact is the rare good one.** `kTuningFieldCount` stays 49, so
`ruleset_hash` does not move and **no recorded replay is invalidated** — the
opposite of §6.4's. `kWorldHashVersion` stays **2** and the golden hash moved
anyway, which is the version byte doing its job in the direction nothing had
exercised: the *set* of hashed state is unchanged, and the *values* changed
because monsters now walk.

Measured at §11's reference scale, a tick in which sixteen monsters path to
sixteen different places costs **≈2,650 µs** against the 4 ms budget — stable
across runs to within ~3%. The same sixteen sharing one target cost **170–330 µs**
and a tick with no pathing at all **5–17 µs**; both of those are small enough to
be dominated by run-to-run noise, so the cache is worth *roughly an order of
magnitude* rather than any one figure. (The first version of this paragraph
quoted single samples of the two noisy numbers as though they were measurements.
They were the high end of a spread.)

The reason the cache usually applies is not the cache: `step` rewrites every
perceiving monster's `last_known` to the party's cell, so monsters that can see
you agree about where you are by construction. Sixteen *distinct* targets need
sixteen *stale* beliefs.

That worst case is **67% of the budget here and over it on CI** — GCC on a
GitHub runner measured 5,462 µs, and Clang on the same runner stayed under. A
row that straddles its budget by machine and by compiler is not a stable
assertion in either direction, so it is measured and printed rather than
asserted, and [#36](https://github.com/gobha-me/gloam/issues/36) carries the
escape route.

Still unstarted: the **RtAudio device** — the stream, the resident PCM and the
mixer. It is deliberately a separate change, because neither a GitHub runner nor
this project's dev box has an audio device, so it can be compiled but not
observed. §11's tick-to-first-sample row stays `PENDING` until it lands, and says
so rather than reporting a number it cannot measure.

## Design

The full specification is [`design/SPEC.md`](design/SPEC.md), vendored from the
design project that owns it — see [`design/README.md`](design/README.md) for the
provenance and for which copy wins. Section numbers throughout this codebase
(§3, §6.2, §11 …) refer to it, and every non-obvious constant carries the sentence
it came from. The rule the code follows: if a number appears here, the reason it
has that value appears next to it.

`design/` also carries the slices pulled out for specific jobs —
[`BUDGETS.md`](design/BUDGETS.md), [`SCHEMAS.md`](design/SCHEMAS.md),
[`TEST-PLAN.md`](design/TEST-PLAN.md), [`BUILD-ORDER.md`](design/BUILD-ORDER.md) —
and the rune data. `UPSTREAM.md` is where the design document's own staleness is
tracked; the snapshot is not patched in place.

Three commitments shape almost every file:

* **The lamp is the game.** Light determines what is drawn *and* what can see
  you. One `lamp_level` integer drives both the render and the perception model;
  there is no second light model.
* **Deterministic to the bit.** Seed in, world out. Fixed-tick integer
  simulation, no floats in simulation state, no wall-clock reads, no
  `std::random_device`, and no `<random>` distributions — they are not portable
  across standard libraries, and a replay must reproduce on both compilers.
* **Budgets are contracts.** Every number in §11 is a test that fails the build,
  not a target.

## Layout

```
design/           the specification the code cites — a snapshot; see design/README.md
include/gloam/    the deterministic core's public headers, plus sixteen off-umbrella
src/lib/          its implementation — standard library only, no I/O, no clock
src/bin/          the diagnostic binary, gloam_bake and gloam_replay; termforge
                  lands here too
test/             property tests (§13.3) and budget assertions (§11)
cmake/            the template's build machinery, plus check_layer_z.cmake (§4.5),
                  check_kitty_boundary.cmake (§16), check_pack_repro.cmake (§10),
                  check_replay_determinism.cmake (§12) and check_audio_mute.cmake (§9)
```

Sixteen headers in `include/gloam/` are not simulation and are deliberately left
out of the `gloam/gloam.hpp` umbrella: five render-side (`budgets.hpp`,
`layer.hpp`, `emit.hpp`, `meter.hpp`, `kitty.hpp`), `audio.hpp` for §9, and nine
for the offline pipeline and the upload path it feeds (`dither.hpp`,
`plate.hpp`, `lightfield.hpp`, `pack.hpp`, `assets.hpp`, `palette.hpp`,
`png.hpp`, `deflate.hpp`, `base64.hpp`). They live inside the
standard-library-only boundary anyway, because **producing** bytes is not the
same as needing a terminal; the `write` that puts them on one is in `src/bin/`,
and so is the only `open` in the pipeline. None of the nine owns a plate — they
take caller-owned spans and report the size they need, which is what kept image
ownership out of the library when the pack format arrived and, four slices
later, when the transmit path did too. `gloam.hpp` says all of this at the top,
so the exclusion does not read as an oversight.

(This paragraph used to say "ten", list four render-side headers, and omit
`meter.hpp` entirely. The count had been wrong since the frame classes landed;
the transmit path is what made it wrong enough to notice.)

`sha256.hpp` is the sixteenth, and a special case: still not included by
`gloam.hpp`, but reaching every consumer anyway through `replay.hpp` and
`world.hpp`, which both name `hash::Digest`. Its old reason — "pipeline-side, not
simulation" — stopped being true when `world_hash` arrived, since a digest over
simulation state is exactly what §13.2's golden replay is defined in terms of.

`gloam::lib` links nothing beyond the standard library, and that is a hard
architectural boundary rather than a coincidence: a simulation that can reach a
terminal or a clock is a simulation that cannot be replayed.

## Cheat sheet

**Configure, build, test — and picking a toolchain**

```bash
cmake -B build                                                            # $CXX, C++23, Debug
cmake -B build-clang -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain/clang.cmake   # clang
CXX=clang++ cmake -B build-asan -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain/address.cmake

cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Run these from the repo root — the toolchain paths are relative. You cannot pass
two toolchain files, so a sanitizer composes with clang via `CXX=`, as above.

**Add a dependency**

```bash
cp cmake/deps/catch2.cmake cmake/deps/<name>.cmake   # catch2.cmake is the annotated recipe template
$EDITOR cmake/deps/<name>.cmake                      # find_package first, FetchContent fallback
$EDITOR CMakeLists.txt                               # add <name> to <PROJECT>_DEPS
cmake -B build
```

The recipe file alone does nothing — the list is the switch. A name on the list
with no recipe is a hard configure error; a recipe not on the list is inert. To
remove a dependency, delete its name from the list; the file can stay.

If the **library** links the new dependency — rather than the executable or the
tests — it needs two more lines, because it becomes part of what this project
exports: `set(<DEP>_INSTALL ${${PROJECT_NAME}_INSTALL})` in the recipe, and a
`find_dependency(<dep>)` in `cmake/project-config.cmake.in`. Without the first,
installing fails during generation; without the second, consumers get a package
whose targets refer to something they cannot find. A `PRIVATE` link counts —
visibility is not the test. The annotated recipe explains both, and
`example/public-dep/` is a working example that is checked in CI.

**Add a test**

```bash
mkdir test/40myfeature
$EDITOR test/40myfeature/test.cpp    # TEST_CASEs only — test/main.cpp provides main()
cmake -B build && cmake --build build --parallel && ctest --test-dir build
```

It becomes the target and ctest name `40myfeature-test`. **Re-run `cmake -B`
after adding a directory** — discovery is a configure-time glob. The numeric
prefix only sorts that glob; it does not order the run (see the Tests bullet
above). The target links Catch2 and, when the library target exists,
`<PROJECT>::lib` — so `#include <lib.hpp>` and call into `src/lib/` directly,
with nothing to wire up. If a test needs custom build control, give it its own
`test/<dir>/CMakeLists.txt`: it inherits `TEST_NAME` and `SRCS` from the parent
scope, must define a target named exactly `${TEST_NAME}`, and must do its own
linking — the discovery loop's link lines do not reach it (see
`test/02example/`).

**Run one test**

```bash
ctest --test-dir build -N                                              # list them
ctest --test-dir build -R 20failure-testing-test --output-on-failure   # one, plus its fixtures
ctest --test-dir build -R 20failure-testing-test -FS . -FC .           # one, fixtures skipped

./build/test/20failure-testing-test --list-tests                       # Catch2 cases within it
./build/test/20failure-testing-test "[failure]"                        # one case, or a tag
./build/test/20failure-testing-test -s                                 # show successful assertions too
```

`-R` is a regex match on the test name. Every discovered test carries
`FIXTURES_REQUIRED runners`, so ctest re-adds `startup` and `shutdown` even when
you filter — `-R` alone reports **three** tests, not one. `-FS . -FC .` excludes
the fixtures. Tests with their own `CMakeLists.txt` build into
`build/test/<dir>/`; the rest land in `build/test/`.

**Consume this project from another project**

Three ways in, one target name — `<PROJECT>::lib` — so switching between them
never touches a link line.

```cmake
# 1. vendored / submoduled
add_subdirectory(third_party/<project>)

# 2. FetchContent
include(FetchContent)
FetchContent_Declare(<project>
  GIT_REPOSITORY <url>
  GIT_TAG        v1.2.3
  SOURCE_DIR     ${FETCHCONTENT_BASE_DIR}/<project>   # ← see below
)
FetchContent_MakeAvailable(<project>)

# 3. installed
find_package(<project> CONFIG REQUIRED)

target_link_libraries(app PRIVATE <project>::lib)     # all three, unchanged
```

⚠ **Pin `SOURCE_DIR` in the FetchContent case.** This project takes its name
from its directory, and FetchContent checks out into `<base>/<name>-src` — so
without that line the project comes out named `<name>-src` and the target you
have to link is `<name>-src::lib`. This applies to any directory-named project,
not just this one.

You inherit the include directory *and* C++23 as usage requirements of the
target; a consumer sets neither. `example/consumer/` is a working downstream
project that builds all three ways, and `example/consumer/verify.sh` runs them.

**Install it**

```bash
cmake -B build -DCMAKE_INSTALL_PREFIX=/opt/<project>
cmake --build build --parallel
cmake --install build
```

Installs the library, `include/*.hpp`, the executable, and a package config at
`<prefix>/lib/cmake/<project>/`. Build with `-D<PROJECT>_BUILD_BIN=OFF
-D<PROJECT>_TESTS=OFF` for a library-only install.

Three things worth knowing before you depend on it:

* The package exists **only in an install prefix**. Pointing
  `CMAKE_PREFIX_PATH` at a build directory finds the config file that was
  generated there and gets a directed refusal, not a package: this project
  exports its targets at install time only. For side-by-side development use
  `add_subdirectory` — same target name, no packaging in the way.
* The version in the package config comes from `git describe` at configure time.
  A build with no reachable tags reports `0.0.0`, and a consumer's
  `find_package(<project> 1.2.3 CONFIG REQUIRED)` is then refused — correctly,
  but the real cause is usually a shallow clone. Compatibility is
  `SameMajorVersion`.
* Headers install flat into `<prefix>/include`, generated `version.hpp`
  included. That header declares *unprefixed* constants (`PROGRAM_NAME`,
  `VERSION_MAJOR`, …). A project expecting wide consumption should move its
  headers under `include/<project>/` first.

**Cut a release tag**

```bash
git tag -a v1.2.3 -m "v1.2.3"
git push origin v1.2.3
cmake -B build          # the version is read at CONFIGURE time — re-configure or it is stale
cmake --build build --parallel
```

The format is enforced: optionally `v`- or `r`-prefixed, then exactly three
numeric components. `v1.2`, `v1.2.3.4` and `v1.2.3-rc1` are rejected by design
and fall back to `0.0.0` with a `STATUS` line naming the reason. Between tags,
`VERSION_TWEAK` counts commits since the tag and `VERSION_DIRTY` flags an unclean
tree; both land in `include/version.hpp`.

## Continuous integration

`.github/workflows/ci.yml` builds and tests on every push to `main` and every
pull request, enforcing the "both compilers, always" rule:

* **GCC and Clang** ×
* the **default** toolchain plus every sanitizer (**address**, **thread**,
  **undefined**) — 8 build/test jobs in all,
* a **library disabled** job, covering the `-D<PROJECT>_BUILD_LIB=OFF` path the
  matrix never takes — it installs as well as builds, and asserts the prefix
  gets the executable and nothing else,
* two **consumer** jobs (one per compiler) building `example/consumer/` against
  this project three ways — the only coverage of the consumed, not-top-level
  path,
* a **public dependency** job, which synthesises a fork whose library links a
  fetched dependency and checks the install/export files survive it — the one
  shape this project cannot exercise as itself,
* plus a fast, dependency-free `version-parse-selftest` job.

A change that only builds on one compiler turns that compiler's jobs red, so a
one-sided break is visible on the PR.

**Copying this into a new project:** the workflow hardcodes nothing
project-specific — the project name is derived from the checkout directory, so
copy `.github/workflows/ci.yml` verbatim, and keep `fetch-depth: 0` or
`git describe` stops finding tags. The one edit a fork owes CI is the badge URL
above; that step and everything else a new project must change live in
[NEW_PROJECT.md](NEW_PROJECT.md).
