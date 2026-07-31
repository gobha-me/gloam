# GLOAM — build order

Numbered, gated, in this order. Each step's acceptance criterion is a test, not an
opinion.

**Steps 1–9 exist only to reach step 10 as cheaply as possible.** Nothing in the
party or magic sections makes the M0 question easier to answer, and building either
first makes it more expensive to act on the answer.

### 1. File the upstream epics

File GL-A and GL-B in termforge's own register (a named bug class, an acceptance
test, and the mutation that proves the test bites). Comment on the existing #83,
#100, #60, #91 and #97 naming GLOAM as a consumer.

*Accept:* the issues exist, and GL-A1 has an acceptance test written.

### 2. The layer API and the kitty call boundary

Every escape sequence GLOAM emits goes through one module. This is the insurance
policy against GL-A never landing: a vendored driver must be a swap, not a rewrite.

*Accept:* `-1073741825` appears exactly once in the tree; a grep test enforces it.

### 3. Image lifecycle tests — before any game logic

All six cases in `TEST-PLAN.md` §1, against a real pty.

*Accept:* all six red before the residency fix, green after.

### 4. Byte and cold-start instruments

Counters on the emit path and the upload path, printed every run.

*Accept:* every row of `BUDGETS.md` exists as an assertion, even where the number is
trivially met.

### 5. The asset pipeline and the pack

Two authored depth rings, three derived by exact 2:1 downsample, six light fields,
one manifest.

*Accept:* two pipeline runs produce **byte-identical** packs.

### 6. The compositor

Slot placement, the placement-list diff, one step transition.

*Accept:* an idle frame emits **zero** bytes; a step emits under 2 KB.

### 7. The sim core and the replay format

Integer state, named RNG streams, the golden-replay harness.

*Accept:* a recorded session replays to an identical world hash on both compilers.

### 8. Perception, noise, light — the whole model, one monster

Property tests from `TEST-PLAN.md` §3.

*Accept:* a doused party is provably unseen beyond an adjacent cell.

### 9. The audio sink, rough

RtAudio stream, the SPSC command ring, footfalls and one sting.

*Accept:* `--mute` and unmuted runs produce identical replays.

### 10. Play it. Toggle the pump. Answer the M0 gate.

Does glimpsing that monster cross the intersection produce genuine tension?

Then, and only then, open the party or magic sections.
