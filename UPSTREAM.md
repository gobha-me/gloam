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

One more, which is a design question rather than a factual error:

5. **§4.2's slot inventory excludes transition sequences from the resident image
   count.** Summing every row of the table gives 71 + 3 = 74 for M0 and
   246 + 6 = 252 for the full game, against stated totals of 71 and 246. The
   difference is the transition row both times — consistent with its own note
   that these are "animation registrations, not placements". Encoded that way in
   `include/gloam/budgets.hpp`, with the arithmetic asserted, because the naive
   reading does not blow the 256 cap and would therefore fail silently.

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
