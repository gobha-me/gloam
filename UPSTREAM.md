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

Each of these is also a GLOAM issue, because they need a decision from the
design owner rather than a code change:
[#9](https://github.com/gobha-me/gloam/issues/9),
[#10](https://github.com/gobha-me/gloam/issues/10),
[#11](https://github.com/gobha-me/gloam/issues/11),
[#12](https://github.com/gobha-me/gloam/issues/12).

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

## Working around an open request

GLOAM does not build elaborate shims for blocked APIs. Where a request blocks
real progress, the work stops and the blocker is escalated to termforge rather
than routed around — a workaround that survives long enough becomes the reason
the upstream fix never lands.

What that means today: the entire §4 compositor is blocked on GL-A1 and #83, and
`gloam::lib` therefore contains no rendering code at all. The deterministic core
underneath it — geometry, noise, perception, light, runes, budgets — has no
terminal dependency by design and is complete and tested.
