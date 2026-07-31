# termforge — what exists, what is missing

Read against **v0.1.18 (2026-07-30)**. Five epics with fine-grained children,
because "file six issues" is not a plan a contributor can pick up. Where an
upstream issue already exists it is **named, not duplicated** — comment on it
naming GLOAM as a consumer instead.

File these in termforge's own register: a named bug class, an acceptance test, and
the mutation that proves the test bites.

---

## Already there — use it, do not rebuild it

| GLOAM needs | termforge has |
| --- | --- |
| Fixed-tick pump with remainder carry | `App::on_tick(dt)` + `set_tick_hz(n)` (v0.1.8) |
| Clamp disabled for a synthetic clock | `set_max_tick_dt(0)` — documented for exactly this |
| Kitty transmit and placement | KittyDriver: base64 + APC, classic cursor placement, Unicode placeholders for tmux |
| Sprite-sheet slicing, alpha compositing | `Image::sub/blit/blend/fill` (v0.1.18), straight alpha, bit-exact blend contract |
| Terminal restored on every exit path | raw-mode RAII + async-signal-safe restore + the `run_loop` exception guard |
| Mouse out of the way | `MouseMode::None` (v0.1.15) |
| Dim / reverse text for the rail | `Cell::attrs` bitmask (v0.1.16) |
| Frames, dialogs, lists for the rail and party strip | the full widget set, `FocusRing`, modal overlay stack |

**An earlier draft asked for a swappable pump policy object. It already exists.**
Do not build a second one.

---

## EPIC GL-A — Application-owned resident images

**Blocks the entire project.** Nothing downstream is safe until this is settled.

### GL-A1 — Pin an image against LRU eviction
The KittyDriver assigns stable **per-region** image IDs and **LRU-evicts** them.
That policy is right for a dashboard, where regions come and go, and fatal for a
resident plate set, where an eviction is a silent full re-upload mid-session.
Proposed: an `ImageHandle` with `pin()`/`unpin()`, pinned images exempt from eviction,
and an `ErrorEvent` when a pin cannot be honoured under quota.

*Acceptance:* place 256 pinned images, churn 256 unpinned regions, assert zero
re-transmission of the pinned set. Red before, green after.

### GL-A2 — Application-allocated image IDs
Or a reserved ID range the driver will not allocate into. An application that owns
a manifest of plate IDs needs those IDs to mean what the manifest says.

### GL-A3 — Shared-memory transfer path for the bulk upload
Kitty's `t=s` transmission medium instead of base64 through the tty, for the startup
upload only. This directly sets cold-start time, which is a GLOAM budget.

### GL-A4 — Residency accounting and quota query
How many images are resident, how many bytes, what is the terminal's limit.
Without it, quota exhaustion is discovered by failure.

### GL-A5 — Documented image lifecycle
Across alt-screen enter/exit, SIGWINCH, suspend/resume and reattach — with the
`TEST-PLAN.md` §1 cases contributed upstream. This is library-wide value, not
GLOAM-specific: any app with a persistent image has it.

---

## EPIC GL-B — Placement and layering

### GL-B1 — `draw_image` takes a cell rect
**Already filed upstream as #83.** It gates the MapWidget sprite tier too. Comment,
do not duplicate.

### GL-B2 — Named layer API over raw z-index
Including the below-background threshold. **No application should hand-write
`-1073741825`.** GLOAM ships one internally (per its own CLAUDE.md) and offers it up.

### GL-B3 — Sub-cell pixel offset and source crop on placement
Kitty's `X=`/`Y=` and the source-rect parameters, exposed. GLOAM needs them because
its viewport is pixel-defined and cell boundaries do not divide it evenly.

### GL-B4 — Query the terminal's cell pixel size
**Already filed upstream as #100** — "both examples guess, and one guessed wrong".
Until it lands, GLOAM reads `TIOCGWINSZ`'s `ws_xpixel`/`ws_ypixel` itself, behind one
function, with a deletion date pointing at #100.

---

## EPIC GL-C — Terminal-side animation

### GL-C1 — Register a pre-baked frame sequence at startup, by name
ROADMAP 6.3 ("KittyDriver animation — frame-based animation via image ID
replacement, `std::jthread`") is the unstarted stub.

### GL-C2 — Trigger by name, query completion, interrupt cleanly
Interruption is the part a naive implementation gets wrong. GLOAM's step
transitions must honour inputs queued during them, so a running sequence has to be
cancellable mid-flight without leaving a half-composed frame.

---

## EPIC GL-D — Input and startup

### GL-D1 — Kitty keyboard protocol: key release and repeat
**Already filed upstream as #60.** Blocks hold-to-aim / release-to-commit combat
input. Not needed for M0 or M1; **must land before M2 combat starts.** The
fallback if it slips is press-to-open / press-to-commit, which costs one keystroke
and no ambiguity.

### GL-D2 — Declare a capability floor and refuse to start below it
**Already filed upstream as #91.** GLOAM is its first real consumer: it is
kitty-only by design and opts out of degradation entirely, which is a deliberate
contradiction of the library's own degradation-as-events pitch — and exactly what
#91 exists to make legitimate.

### GL-D3 — Virtual `setup`/`teardown` hooks
**Already filed upstream as #97.** GLOAM needs them for the plate upload and the
RtAudio stream lifecycle: both must be torn down on every exit path, including the
exception path the `run_loop` guard now covers.

---

## EPIC GL-E — Determinism and replay

### GL-E1 — Promote the headless pump to supported API
`App::test_run_frames` and the three protected seams (`now_steady`, `wait_readable`,
`read_available`) are **exactly the right shape** and are currently test-only. A
consumer with a golden-replay corpus needs them supported.

### GL-E2 — Synthetic clock injection as public API
The natural companion to GL-E1. `set_max_tick_dt(0)` already anticipates it.

### GL-E3 — Record and playback of the event stream
A testing primitive for the whole library, not just for GLOAM. Every terminal app
has the "reproduce that input sequence" problem.

### GL-E4 — `App::post_event`
**Already filed upstream as #28** (TF-05).

---

## Not a termforge issue: audio

An earlier draft asked for an audio sink upstream, citing issue **#212**. That
issue does not exist — the tracker tops out around #102 — and more importantly
termforge's stated dependency policy is **standard library only in the shipped
library**. RtAudio is a third-party dependency and cannot land there without
breaking the library's central promise.

**Do not file it.** GLOAM owns audio permanently. See `SPEC.md` §9.
