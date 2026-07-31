# GLOAM — budgets

Not optimisation targets. Each row is a CI assertion, wired from the first commit,
that fails the build when exceeded. Instrument cold-start upload time and per-frame
command bytes **before there is a game to measure**.

## Payload and residency

| Budget | Limit | Enforced by |
| --- | --- | --- |
| Resident images | ≤ 256 | manifest test |
| Cold-start payload, base64 | ≤ 1.2 MB | manifest test |
| Cold start, local terminal | ≤ 800 ms | startup instrument, logged every run |
| Cold start, 1 Mbit/s link | ≤ 12 s | synthetic-throttle test |

## Per-frame emission

The compositor builds a placement list each tick and diffs it against what is on
screen. Identical list, nothing emitted.

| Situation | Limit |
| --- | --- |
| Idle — nothing changed | **0 bytes** |
| Animation-only frame (monster pose, lamp flicker) | ≤ 400 B |
| Full recomposition (a step or a turn) | ≤ 2 KB |
| A 140 ms step transition | one animation-control command |
| Sustained gameplay, p95 | ≤ 8 KB/s |

The counters wrap the **emit path**, so they measure what actually left the
process, not what the compositor believed it produced.

> Why this matters: an earlier draft specified "sim 10 Hz, render 60 fps". 60
> frames × ~2 KB of placement commands is roughly 1 Mbit/s sustained. That is not
> an ssh session, it is a video stream. Emit-on-change is what keeps the pillar-2
> promise true.

## Timing

| Budget | Limit | Enforced by |
| --- | --- | --- |
| Simulation tick @ 10 Hz | ≤ 4 ms | tick timer assertion |
| Compose + diff + emit | ≤ 2 ms | frame timer assertion |
| Step transition | 140 ms (named constant, tunable) | — |
| Audio, tick → first sample | ≤ 20 ms | sink instrumentation |
| Dropped voice commands | 0 per 1000-tick replay | ring counter assertion |

## Plate inventory

| Class | M0 | Full game |
| --- | --- | --- |
| Wall slots × wall types | 24 | 48 |
| Floor / ceiling bands | 8 | 8 |
| Light fields (full-frame) | 6 | 6 |
| Monster poses | 27 | ~120 |
| Items, decorations, floor features | 0 | 34 |
| UI frames, rune glyphs, portraits | 6 | 30 |
| Transition frame sequences | 3 | 6 |
| **Total resident images** | **71** | **~246** |
