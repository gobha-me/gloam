# GLOAM — test plan

## 1. Image lifecycle — written before any game logic exists

Per the kitty spec, clear-screen and main↔alt-screen transitions clear images.
With ~246 resident plates referenced by every frame, a lifecycle bug means a full
re-upload mid-session and a visible stall.

Each case drives a **real pty**, then re-places every image ID and asserts no
re-transmission occurred.

| Case | Assert |
| --- | --- |
| Alt-screen enter → leave → re-enter | all IDs still resident |
| SIGWINCH resize | all IDs still resident; viewport cell rect recomputed |
| SIGWINCH that changes the **cell pixel size** | viewport re-placed, plates unscaled |
| SIGTSTP suspend → resume | all IDs still resident |
| Detach / reattach (tmux, and a bare ssh reconnect) | all IDs still resident |
| Exception thrown out of a frame | terminal lands cooked; no wedge |

There is a second hazard beyond the spec: **termforge's KittyDriver LRU-evicts its
per-region image IDs.** That policy is correct for a dashboard and actively hostile
to a resident plate set — an eviction is a silent re-upload. Write a test that
places 256 plates and asserts none is evicted; expect it red until GL-A1 lands.

## 2. Determinism harness

A golden replay is `seed + input log → world hash at tick N`. The hash covers every
byte of simulation state and **nothing** from the render layer.

- Run the corpus under a **synthetic clock with the stall clamp disabled**
  (`set_max_tick_dt(0)` — termforge documents this as being for exactly this), so the
  suite never sleeps and CI wall-time stays flat as the corpus grows.
- Run on both compilers in the matrix. A cross-compiler hash divergence is a
  floating-point leak into sim state, every time.
- **There is no flaky-test triage path for this suite.** A nondeterministic
  simulation has no correct behaviour to fall back to. A mismatch is a regression.
- Keep at least one replay per bug ever filed.

## 3. Property tests — perception

| Property | Why it matters |
| --- | --- |
| Attenuated noise is monotone non-increasing in graph distance | the model is legible only if it is monotone |
| Line of sight is symmetric | asymmetric LOS is the classic "it saw me through a wall" bug |
| A doused party is never seen beyond an adjacent cell by a `sees_unlit = false` monster | **the pillar, asserted** |
| Awareness never advances two states in one tick, and never regresses except by the §6.1 timers | a skipped state has no authored tell, so it reads as arbitrary |
| Every one of the 1,764 rune sequences resolves without throwing | the resolver is a pure function; exhaust it |
| Every row in `spells.data` is reachable from some placeable inscription set | a spell nobody can learn is content that does not exist |

## 4. Budget tests

Every row of `BUDGETS.md` is a CI assertion, including the ones trivially met at M0 —
the point is that the assertion exists before the number gets close.

The headline one: a **200-tick scripted replay** whose total emitted bytes are
counted at the emit path and compared against the p95 budget.

## 5. What the suite cannot answer

Matching termforge's own discipline: a green suite is not a working terminal app.
These need a real emulator and a human:

- Do the §6.1 awareness tells actually read at a glance? (This is the M0 gate.)
- Does the 140 ms transition feel right against the real-time pump?
- Does the plate set look right on the user's kitty at their cell size?
- Does a doused corridor feel disorienting, or just annoying?
