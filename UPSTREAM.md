# Upstream — what GLOAM needs from termforge

GLOAM consumes [termforge](https://github.com/gobha-me/termforge). Where termforge
cannot yet do something GLOAM needs, the question asked is always the same:

> **Would a different TUI project want this too?**

If yes, it is a termforge feature request and GLOAM waits or works around it in
the open. If it is genuinely a one-off with no recurring utility, GLOAM owns it
and termforge never learns about it. The goal is a robust framework for TUI
projects of all shapes and sizes; GLOAM is a demanding first consumer, not a
special case.

## Filed

Twelve requests, 2026-07-30, all against `gobha-me/termforge`. Each carries a
named bug class, an acceptance test, and the mutation that proves the test bites
— termforge's own register (`AGENTS.md`).

### GL-A · Application-owned resident images — blocks the compositor

| | Need | Issue |
| --- | --- | --- |
| GL-A1 | Pin an image against LRU eviction | [#109](https://github.com/gobha-me/termforge/issues/109) |
| GL-A2 | Application-allocated image ids, or a reserved range | [#110](https://github.com/gobha-me/termforge/issues/110) |
| GL-A3 | Shared-memory transfer path for the bulk startup upload | [#111](https://github.com/gobha-me/termforge/issues/111) |
| GL-A4 | Residency accounting and quota query | [#112](https://github.com/gobha-me/termforge/issues/112) |
| GL-A5 | Documented image lifecycle across alt-screen / resize / reattach | [#113](https://github.com/gobha-me/termforge/issues/113) |

**GL-A1 and [#137](https://github.com/gobha-me/termforge/issues/137) are the hard blockers.** See "What #83 landing changed"
below — the cell rect arrived, and with it a stretch-to-fill contract that a
pre-dithered plate cannot survive.

**GL-A1's own detail:** `KittyDriver` tracks 16 region slots keyed on
`(x, y, w, h)` and evicts the least-recently-drawn past that, recycling ids
(`src/lib/drivers/kitty_driver.cpp:173-201`). Moving a sprite one cell allocates
a new slot; `gc_regions()` drops anything not redrawn in the last frame. For a
dashboard that is correct. For a set of plates transmitted once at startup it
means every eviction is a silent re-upload — the exact cost the resident-image
design exists to avoid.

### GL-B · Placement and layering

| | Need | Issue |
| --- | --- | --- |
| GL-B1 | `draw_image` takes a cell rect | **[#83](https://github.com/gobha-me/termforge/issues/83) — LANDED in v0.3.0** |
| GL-B2 | Named layer API over raw z-index | [#114](https://github.com/gobha-me/termforge/issues/114) |
| GL-B3 | Sub-cell pixel offset (`X=`/`Y=`) and source crop | [#115](https://github.com/gobha-me/termforge/issues/115) |
| GL-B4 | Query the terminal's cell pixel size | **[#100](https://github.com/gobha-me/termforge/issues/100) — LANDED in v0.3.0** |
| GL-B5 | **Opt out of stretch-to-fill — place a pre-rendered image at 1:1** | [#137](https://github.com/gobha-me/termforge/issues/137) — **the compositor's blocker** |

**GL-B2 now exists GLOAM-side and is available to lift.** `include/gloam/layer.hpp`
is the named layer API §4.5 asks for: `gloam::layer::Band` over the six
compositing bands, `image_z(band, rank)` as the only route to a z-index, and the
below-background threshold behind a name rather than hand-written at a call site.
The whole header depends on `<cstdint>` and `<optional>` and nothing else — it is
deliberately a leaf, so #114 can take it whole. `cmake/check_layer_z.cmake` and
`test/06layer/` are the acceptance test and the mutation that proves it bites.

**GL-B3 and GL-B5 are what `gloam::kitty::Placement` is written against.** It
carries a source crop and a sub-cell `X=`/`Y=` offset — #115's shape — and it has
no destination cell-rect field at all, so it structurally cannot emit the `c=`/`r=`
that §3.2 rules out and #137 asks to opt out of. When both land, this module
should **shrink** rather than change shape. Until then GLOAM constructs the
placement bytes itself, behind the boundary `cmake/check_kitty_boundary.cmake`
enforces.

### GL-C · Terminal-side animation

| | Need | Issue |
| --- | --- | --- |
| GL-C1 | Register a pre-baked frame sequence at startup, by name | [#116](https://github.com/gobha-me/termforge/issues/116) |
| GL-C2 | Trigger by name, query completion, interrupt cleanly | [#117](https://github.com/gobha-me/termforge/issues/117) |

### GL-D · Input and startup

| | Need | Issue |
| --- | --- | --- |
| GL-D1 | Kitty keyboard protocol — key release and repeat | **[#60](https://github.com/gobha-me/termforge/issues/60) — LANDED in v0.2.2** |
| GL-D2 | Declare a capability floor and refuse to start below it | **[#91](https://github.com/gobha-me/termforge/issues/91)** — commented as a consumer |
| GL-D3 | Virtual `setup`/`teardown` hooks for app-owned resources | **[#97](https://github.com/gobha-me/termforge/issues/97)** — in progress; lands as `on_start`/`on_stop` |

### GL-E · Determinism and replay

| | Need | Issue |
| --- | --- | --- |
| GL-E1 | Promote the three headless-pump seams to supported API | [#118](https://github.com/gobha-me/termforge/issues/118) |
| GL-E2 | Synthetic clock injection as public API | [#119](https://github.com/gobha-me/termforge/issues/119) |
| GL-E3 | Record and playback of the event stream | [#120](https://github.com/gobha-me/termforge/issues/120) |
| GL-E4 | `App::post_event` | **[#28](https://github.com/gobha-me/termforge/issues/28)** — commented as a consumer |

## Never filed upstream

- **Audio (RtAudio).** termforge ships standard library only. An audio subsystem
  cannot land there without breaking the library's central promise, so GLOAM owns
  it permanently and the two never learn about each other. The design's earlier
  draft cited a termforge issue "#212" for this; there is no #212 and there
  should not be one.
- **Game content.** The depth ladder, the trapezoid plate geometry, the
  light-field plates, the noise and perception models, the rune grammar, the
  offline dither pipeline, the `replay.gloam` container. None of these are a TUI
  framework's business.

## What #83 landing changed

Read against termforge **v0.6.2** (2026-07-31). #83 and #100 both closed in
v0.3.0, and the compositor is **no closer**.

`draw_image` now takes a cell rect and cell pixel size is queryable and
re-pushed on every `SIGWINCH` — both real wins. But #83's resolution made
scaling the written contract:

> Stretch-to-fill, nearest neighbour. No letterbox or fit modes… Scaling is the
> contract, so it is not a degradation and raises no event.
> — `include/termforge/drivers/terminal_driver.hpp`

`place_classic` emits `c=`/`r=` on every placement, with no path that omits
them. That is in direct opposition to SPEC §3.2:

> **Kitty's `c=`/`r=` cell scaling is never used** — it resamples, and
> resampling a pre-dithered plate is exactly the dither crawl §4.3 exists to
> avoid.

The arithmetic makes it unavoidable rather than merely likely. `image_cell_extent`
uses ceiling division, so at the 9×18 px cells termforge measured on real
hardware a 480 px plate is 53.3 cells → 54 → stretched to 486 px. At the 8×16 px
nominal fallback, 360 px becomes 368 px vertically. No plate size survives,
because the cell geometry belongs to the terminal.

**Scoring the compositor's four needs against v0.6.2: none are met.** Exact
placement ([#137](https://github.com/gobha-me/termforge/issues/137)), sub-cell
offsets ([#115](https://github.com/gobha-me/termforge/issues/115)), z-order
([#114](https://github.com/gobha-me/termforge/issues/114)), residency
([#109](https://github.com/gobha-me/termforge/issues/109) — still 16 slots,
still keyed on the destination rect, so moving one cell is still a full
re-upload).

#137 asks for an **opt-out, not a reversal**: stretch-to-fill stays the default
and stays right for widgets, which generate their images and can re-rasterize at
`preferred_pixel_extent()`. A pre-rendered image cannot, and that is the
distinction the API is missing.

## Corrections to the design document

Each of these was also a GLOAM issue, because they needed a decision from the
design owner rather than a code change:
[#9](https://github.com/gobha-me/gloam/issues/9),
[#10](https://github.com/gobha-me/gloam/issues/10),
[#11](https://github.com/gobha-me/gloam/issues/11),
[#12](https://github.com/gobha-me/gloam/issues/12),
[#13](https://github.com/gobha-me/gloam/issues/13).

**All five are now decided** — see "Decisions taken" at the end of this section.
`design/SPEC.md` is deliberately NOT edited to match: it is a snapshot of the
design project that owns it, and patching it in place would fork the two copies.
This section is the amendment record, and it wins over the snapshot wherever they
disagree.

The specification was checked against termforge **v0.1.18**. The library is at
**v0.6.2** and four claims no longer hold:

1. **#60 has landed.** The design gates §7.4's hold-a-number-to-aim on the kitty
   keyboard protocol and says M2 combat cannot start without it. It shipped in
   v0.2.2 — `KeyAction::{Press, Repeat, Release}` on `KeyEvent::action`, behind
   `KeyboardMode::Enhanced`. The fallback plan in §7.4 is no longer needed.
2. **The CMake target is `termforge::lib`,** not `termforge::termforge`.
3. **`App::post_event` does not exist** — the design's §14.1 implies GL-E4 is
   partly available. It is not; #28 is still open. `dispatch_event` is public but
   synchronous and not thread-safe.
4. **The loop seams are `private virtual`, not `protected`** (`app.hpp:422-429`).
   Still overridable — access control does not affect overriding, and termforge's
   own suite relies on that — but not a supported extension point, which is what
   GL-E1 asks for.

Two more, which are design questions rather than factual errors:

5. **§4.2's slot inventory excludes transition sequences from the resident image
   count.** Summing every row of the table gives 71 + 3 = 74 for M0 and
   246 + 6 = 252 for the full game, against stated totals of 71 and 246. The
   difference is the transition row both times — consistent with its own note
   that these are "animation registrations, not placements". Encoded that way in
   `include/gloam/budgets.hpp`, with the arithmetic asserted, because the naive
   reading does not blow the 256 cap and would therefore fail silently.

6. **`SCHEMAS.md` §1 does not fully specify `pack.manifest`, and the pack as
   built deviates from it in one place.** Mirrored as
   [#16](https://github.com/gobha-me/gloam/issues/16). Four items, three of them
   gaps rather than disagreements:

   - **A `codec:u8` field is ADDED to the record.** This is the deviation.
     termforge [#163](https://github.com/gobha-me/termforge/issues/163) landed a
     verbatim transmit path that takes pre-encoded bytes, and §11's cold-start
     budget will eventually need it (see the next item but one). A one-byte
     discriminant now means PNG lands as a new codec value rather than a format
     version bump. `Codec::Png` already parses and is refused, so the door is a
     door and not a hole. It sits at record offset +6 beside the other `u8`s, so
     the record is still 52 bytes.
   - **Endianness and packing were unstated.** Little-endian, fields serialized
     one at a time, never a `memcpy` of a compiler struct — so ABI padding cannot
     reach the digest. Asserted by literal in `test/12pack/`.
   - **Where the pixels live was unstated.** `offset`/`length` imply a blob
     region; SCHEMAS.md never says whether it is the same file. It is: two files
     lets the manifest be fresher than the pixels, and §10's "a mismatched hash
     refuses to launch" needs one atomic object to hash.
   - **"Four colours plus transparent" is five states and does not fit in two
     bits.** Resolved as two planes — a 2-bit index plane and a 1-bit stencil —
     rather than by spending a palette entry, which would cost 25% of a palette
     that is the whole art direction. See `include/gloam/plate.hpp`.

   Also noted there and now **answered** by item 8 below: `replay.gloam` carries
   `pack_hash:u64` while the manifest carries `pack_sha256:[32]u8`.

8. **`SCHEMAS.md` §3 does not fully specify `replay.gloam`, and the replay as
   built deviates from it in two places.** Mirrored as
   [#19](https://github.com/gobha-me/gloam/issues/19), with the `pack_hash` half
   answered on [#16](https://github.com/gobha-me/gloam/issues/16) where it was
   raised. Settled in `include/gloam/replay.hpp`. Five items:

   - **`pack_hash:u64` is the FIRST EIGHT BYTES of the manifest's
     `pack_sha256`, in the order they appear on disk.** This is the question
     item 6 left open. The eight bytes `replay.gloam` carries at its offset 56
     are byte-for-byte the eight bytes `pack.gloam` carries at its offset 8, so
     the two files can be held up against each other in a hex dump. A fold, a
     re-hash, or the last eight bytes would be equally uniform and none of them
     would let someone confirm an unexpected warning by eye — which, for a field
     that only ever warns, outweighs everything else. `kNoPackHash = 0` means
     "no pack was loaded", which is every replay until the compositor exists.
   - **Endianness and packing were unstated**, exactly as for the pack.
     Little-endian, fields serialized one at a time. The helpers both formats
     use now live in `src/lib/bytes.hpp` rather than being copied. A record is
     **seven** bytes: §3 says "packed", and writing fields one at a time means
     honouring that costs nothing.
   - **Four fields are ADDED to §3's six**: `file_sha256`, `record_count` and
     `total_bytes` are the pack's own header fields doing the pack's own job.
     `final_world_hash` is the deviation that matters — see below.
   - **The file carries the world hash it is supposed to reproduce.**
     TEST-PLAN.md §2 defines a golden replay as "seed + input log → world hash
     at tick N" and then says "keep at least one replay per bug ever filed".
     Those compose only if the file holds all three parts; a replay carrying
     just the seed and the inputs is half a regression test, and a bug report
     mailed in from outside arrives with no way to say what it should have done.

   - **§3 states no ceiling on `tick`, and it needs one.** `play` reaches a
     record's tick by advancing one tick at a time, so `tick` is the field that
     decides how much CPU a file costs — and it is a `u32`. A well-formed
     111-byte replay whose single record sits at `0xFFFFFFFF` costs 4.29 billion
     full-level ticks, about twenty minutes of a pinned core, on a file that
     passes every other check including the digest. Reproduced against the real
     binary during review. `replay::kMaxTick` is 10,000,000 (~11.5 days of game
     time at 10 Hz) and anything past it is refused as `TickOutOfRange`. A
     format documented to accept files "mailed in from outside" cannot leave its
     own work bound unstated.

   Two places where the simulation had to decide something §6.2 is silent about,
   recorded here and on [#19](https://github.com/gobha-me/gloam/issues/19)
   because they are design questions rather than code. Both are now baked into
   `final_world_hash`, so the golden replay pins them:

   - **Two emitters in one tick take the MAX, not the sum.** §3 permits two
     records to share a tick. Summing would let an input stream manufacture
     arbitrary loudness out of two footfalls 100 ms apart.
   - **`creep_tick_cost` is not charged by `advance`.** §6.2 makes creeping cost
     ticks as well as halving the noise, but there is no movement-rate model to
     charge them against yet, so how often a `Step` may legally appear is the
     driver's rule. `step_noise` already halves.

7. **§11's cold-start payload row measures the base64 TRANSMIT payload, not the
   pack — and on the current numbers it is unreachable.** Mirrored as
   [#17](https://github.com/gobha-me/gloam/issues/17). `BUDGETS.md` says
   "Cold-start payload, base64 | ≤ 1.2 MB | manifest test", but a manifest test
   can only measure the pack. Those differ by more than an order of magnitude:
   kitty is handed pixels, so a 480×360 plate stored at 2 bits per pixel expands
   to 691,200 B at `f=32` before base64 adds a third. **The six light fields
   alone are 388,800 B in the pack and roughly 5.5 MB on the wire — about 4.6×
   over budget before a single wall plate exists.**

   The escapes are `pack::Codec::Png` (termforge #163's `f=100`), kitty's `o=z`,
   or GL-A3 / [#111](https://github.com/gobha-me/termforge/issues/111)'s
   shared-memory transfer. `test/10budgets/` asserts the pack size as an
   explicitly-labelled *necessary condition* rather than as the row, because a
   green test measuring the wrong quantity is what `budgets.hpp`'s opening
   paragraph exists to prevent.

9. **§9.3 does not say how loud a sting is, or whether it is diegetic at all.**
   Mirrored as [#22](https://github.com/gobha-me/gloam/issues/22). §9.3 derives
   gain from `propagate_noise`, which needs an emission magnitude; §6.2's table
   has rows for a step, a door, a swing, a hit, a fall and a cast, and **none for
   a monster vocalising** — nothing in the simulation has ever needed one,
   because monsters do not listen to each other.

   Settled as **diegetic and attenuated**, at `noise_melee_hit` (90). §6.1 only
   reaches SEARCHING → HUNTING with clear line of sight, a lit party and range
   within sight distance, so a stinging monster is by construction visible and
   close — which removes the one argument for a non-diegetic sting ("the player
   misses the tell at range") by making that case unreachable. A non-diegetic
   sting would also survive dousing the lamp at full volume, turning the tell
   into a UI notification in a game whose fourth pillar forbids exactly that.

   `audio::kStingEmission` lives in `audio.hpp` and **not** in `Tuning`, which is
   the part worth disagreeing with. A tunable is a promise that the number
   changes the simulation, and `ruleset_hash` makes a mismatch a hard rejection
   at load. Nothing hears the sting, so it cannot reach `world_hash` — and a
   number no replay could have observed must not be able to reject one. The
   coupling §9.3 asks for is unaffected: the *attenuation* the sting propagates
   through is already in `Tuning` and reaches the mix through `propagate_noise`.

10. **§9.2 requires a resident audio arena in the pack, and `SCHEMAS.md` §1 has
    no record that can describe one.** Mirrored as
    [#23](https://github.com/gobha-me/gloam/issues/23). §9.2 says "all PCM is
    decoded at startup into the pack's resident audio arena … the audio pack
    shares §10's manifest hash", but §1's `role` enum is
    `wall | floor | ceiling | light_field | monster | item | ui | rune` with no
    audio value, and the record is shaped for plates — `w`, `h` and a palette
    codec, with nowhere to put a sample rate, a channel count or a frame count.

    **Deliberately unresolved.** Build-order step 9 is "the audio sink, *rough*",
    and its acceptance criterion is about determinism rather than fidelity, so
    the arena is not on the critical path. Extending the pack moves
    `pack_sha256` — a launch gate (§10) and a `ctest` case — and would bake a
    schema around content nobody has authored. The device PR synthesises its
    arena at startup instead, drawing from `Stream::Ambience`, which
    `world.cpp` excludes from `world_hash` precisely so that a change to what a
    sting sounds like cannot be reported as a determinism regression. What is
    lost meanwhile: the PCM is not covered by the manifest digest, so §9.2's
    "shares §10's manifest hash" is unmet.

11. **§11 judges a frame against a per-frame budget and never says how a frame's
    CLASS is determined.** Mirrored as
    [#24](https://github.com/gobha-me/gloam/issues/24). `BUDGETS.md`'s per-frame
    table has three byte rows — "Idle — nothing changed", "Animation-only frame
    (monster pose, lamp flicker)" and "Full recomposition (a step or a turn)" —
    and §4.6 restates them. Every class is named by SITUATION. Nothing anywhere
    gives a rule code can evaluate, which is precisely what a budget assertion
    needs in order to pick which of 0 B / 400 B / 2 KB a frame is judged against.
    §4.6's one procedural sentence — "builds a placement list each tick and diffs
    it against the one on screen. Identical list, zero bytes emitted" — defines
    idle as an OUTCOME of the diff, not as an input classification.

    **Decided: classify on the state delta, not on the input event.** A tick is
    `Recomposition` if `party` or `facing` changed, else `Animation` if
    `lamp_level` changed or any monster's awareness state changed, else `Idle`.
    Encoded as `gloam::meter::FrameClass` and, for the harness,
    `classify_frame(before, after)` in `test/10budgets/`.

    The alternative — read the class off the `replay::Event` — is worse in a way
    that matters. `apply()` already refuses a `Step` into an impassable edge, so
    classifying on the event charges a blocked step as a 2 KB recomposition, and
    an input log can then manufacture its own budget class: spam `Step` into a
    wall and every tick draws a 2 KB allowance while nothing moves. The delta
    inherits `apply`'s rules rather than restating them somewhere they can drift
    apart — the same argument `world.cpp` makes for deriving the footfall from
    `pending_noise` instead of intercepting the event.

    **What the rule cannot see, and the code says so out loud:** monster *pose*
    and lamp *flicker* are both named in the animation row and neither exists in
    `World`. Awareness state is the nearest thing that does. This is an
    under-approximation of the animation class, not the final rule, and it gets
    revisited when §6.4's patrols land and monsters acquire a pose.

12. **`TEST-PLAN.md` §4 asks for a total and §11 states a percentile, and the
    windowing rule between them is unwritten.** Mirrored as
    [#25](https://github.com/gobha-me/gloam/issues/25). §4's headline budget test
    is "a **200-tick scripted replay** whose total emitted bytes are counted at
    the emit path and compared against the p95 budget". A total is not a
    percentile; §11's unit is bytes **per second** while the measurement is per
    tick; and `emit::ByteSink` keeps three scalars and no per-frame history, so
    no percentile of any kind was computable from the instrument that existed.

    **Decided: nearest-rank p95 over SLIDING one-second windows.** Form every
    window of `W` consecutive frames where `W` is one second of ticks
    (`replay::kTickHz`); sum each; sort the `N − W + 1` sums; take the sum at
    1-indexed rank `(95·M + 99) / 100`. Integer arithmetic throughout — no
    interpolation, no floating point, per AGENTS.md rule 2. For N=200, W=10 that
    is `sorted[181]` of 191 windows.

    **The rejected reading is the tempting one:** p95 of per-TICK bytes, scaled
    by the tick rate. A recomposition tick appears in exactly one per-tick sample
    but in `W` sliding windows, so the per-tick form lets any script with under
    5% recomposition ticks report the *animation* cost as its sustained rate — a
    budget that reports the cost of standing still as the cost of walking.

    **One unit trap, pinned in the code rather than in this paragraph:**
    `kMaxSustainedBytesPerSecond` is per second and the comparison is against a
    window SUM, so the units agree only while `W == kTickHz`. The harness derives
    `W` from `replay::kTickHz` and asserts the identity; otherwise a change to
    the tick rate silently rescales the budget.

13. **§4.7's 140 ms step transition and §11's 8 KB/s sustained p95 are mutually
    unreachable at §4.1's own placement count.** Mirrored as
    [#26](https://github.com/gobha-me/gloam/issues/26). Unlike 11 and 12 this is
    not a gap — it is a contradiction between two rows of the same document, and
    it is measured rather than argued.

    `test/10budgets/` now runs TEST-PLAN.md §4's 200-tick scripted replay through
    the real `kitty::emit_placement`, at the fastest walk the simulation can
    express (a step every two ticks — §4.7's 140 ms is faster still, and 10 Hz
    cannot express it). Measured:

    | | |
    | --- | --- |
    | One placement, reference cell | ~66 B |
    | Full recomposition, 24 placements | **1,596 B** vs 2,048 — passes |
    | Animation-only frame | **137 B** vs 400 — passes |
    | Idle frame | **0 B** — passes |
    | Sustained p95, sliding 1 s windows | **8,321 B/s** vs 8,192 — **1.6% over** |

    At §4.7's own 140 ms rate rather than the tick-quantised 5 steps/s the script
    can reach, it is roughly **1.4x** over.

    The p95 is an **upper bound**: the model places §4.2's M0 slot inventory with
    no diff, and §4.6's diff can only remove placements from a full list. That
    bound holds under one assumption, which is now a constraint on
    [#7](https://github.com/gobha-me/gloam/issues/7): the compositor must
    allocate **one placement id per slot and reuse it**. §4.6's diff is
    place-and-delete, so a compositor that allocates ids per frame pays ~25 B per
    vacated slot and the measurement stops bounding anything.

    **The named escape.** kitty defaults `x=0,y=0` and reads `w=0,h=0` as "to the
    right/bottom edge", so a full-plate placement can omit
    `,x=0,y=0,w=480,h=360` — 20 of the ~66 bytes, which brings even §4.7's rate
    inside budget with room. Deliberately not taken in this slice: it breaks
    `test/07emit/`'s golden literal, the crop fields are load-bearing for any
    future atlas, and `kitty.cpp`'s `validate()` refuses a zero crop for a
    documented and correct reason. It belongs to whoever builds the compositor.

    `test/10budgets/` asserts the overrun **inverted**, the way #17's cold-start
    row is asserted, so the row goes red the day a diff or a shorter wire form
    fixes it and whoever lands that has to come back and flip it. A second
    assertion pins the overrun at ≤ 5%: the margin being this thin is itself part
    of the finding, because a budget whose satisfaction turns on the third
    significant figure of an assumed wire form is not yet a budget.

### Decisions taken

Amendments to `design/SPEC.md`, decided 2026-07-31. The snapshot is not edited;
these rows are the amendment, and the code implements them.

| | Decision | Effect |
| --- | --- | --- |
| [#9](https://github.com/gobha-me/gloam/issues/9) | **§7.4's fallback is dropped.** #60 landed, so bind 1–4, hold to surface options, release to commit — as originally designed, no timing heuristic. | §14.2's GL-D1 row is landed. **M2 has no upstream blocker.** No code today; M2 combat has not started. |
| [#11](https://github.com/gobha-me/gloam/issues/11) | **YRN is renamed URN.** §8.1 lists Y among the liquids and never among the vowels, so a Modifier opening on Y was a vocabulary bug, not a rule with an exception. | `Modifier::Urn`, inscription `"URN"`. `slot_from_inscription` now returns `Form` for Y, and the grammar is **exact over all 24 runes** — `test/05spells/` asserts it exhaustively. Enum value unchanged (5), so `ruleset_hash` is untouched and no replay is invalidated. |
| [#12](https://github.com/gobha-me/gloam/issues/12) | **§6.1 gains a LOST_TRACK → HUNTING row, with its own tell.** Condition is the same `saw` as SEARCHING → HUNTING. The tell is the head snapping mid-cast-about and an immediate close, with **no audio sting**. | `Tell::SnapsBack`. The sting is reserved for a first sighting: it means "found you", and its absence here means "never lost you". Tell selection is now keyed on the `(before, next)` pair, because two transitions land on HUNTING and §6.1 requires them to read differently. |
| [#10](https://github.com/gobha-me/gloam/issues/10) | **Confirmed:** transition sequences are animation registrations, not resident images. | Already encoded; no change. |
| [#13](https://github.com/gobha-me/gloam/issues/13) | **Recorded, deliberately not built.** Capacity from (carry − equipped), over-capacity as a noise penalty, multiplicative rather than subtractive noise reduction, armour **classes** not slots, enchantments derived from URN bindings, and the bag is **silent, not weightless**. | No code. §19 is explicit that nothing in §7 or §8 makes M0's question easier to answer and building either first makes it more expensive to act on the answer. Implement at M1, when the party exists. |

Two notes worth carrying forward from #12's implementation:

- **Re-acquisition is checked before the forget timer.** On the exact tick the
  LOST_TRACK timer expires, a monster that can see the party hunts rather than
  forgets. Losing someone on the frame they walked into your light is the worst
  available reading.
- **The condition is `saw`, not `hit`.** Re-acquiring by ear alone would put a
  hole in §6.3's pillar exactly where it matters most — you have just broken
  line of sight and doused. Note that §6.3 makes a doused party visible **at an
  adjacent cell**; dousing buys invisibility *beyond* adjacent cells, which is
  what keeps it a decision rather than a dominant strategy. Both sides of that
  boundary are pinned in `test/04perception/`.

## Working around an open request

GLOAM does not build elaborate shims for blocked APIs. Where a request blocks
real progress, the work stops and the blocker is escalated to termforge rather
than routed around — a workaround that survives long enough becomes the reason
the upstream fix never lands.

What that means today: the entire §4 compositor is still blocked on GL-A1 and
GL-B5 (#137). The deterministic core underneath it — geometry, noise, perception,
light, runes, budgets — has no terminal dependency by design and is complete and
tested.

**One correction to an earlier version of this paragraph, because it is the
sentence someone would cite to undo build-order step 2.** It used to read
"`gloam::lib` therefore contains no rendering code at all". That is no longer
true, and the distinction matters more than the fact:

- The library contains the code that **constructs** escape sequences —
  `layer.hpp`, `emit.hpp`, `kitty.hpp`/`kitty.cpp`. Those are pure functions from
  integers to bytes in a caller-owned buffer. No clock, no file descriptor, no
  global, nothing beyond the standard library, and therefore no threat to §5.1's
  replayability, which is what the rule is actually protecting.
- The code that **writes** those bytes to a terminal stays in `src/bin/`.

§16 is the reason this was built before its blocker cleared: "keep all kitty calls
behind GLOAM's own layer API from day one, so a vendored driver is a swap and not
a rewrite". A boundary built after the driver arrives is not insurance.
