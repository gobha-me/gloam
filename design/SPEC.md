# GLOAM — design document

**Status:** draft v2 — implementation specification
**Reads against:** gobha-me/termforge @ v0.1.18 (2026-07-30)
**Platform:** kitty graphics protocol only. No sixel fallback, no degraded mode.
**Language:** C++23, termforge, RtAudio

---

## 0 · One-line pitch, and what changed

A 1-bit grid dungeon crawler in the Dungeon Master / Eye of the Beholder lineage,
where **your lamp is the thing that lets you see and the thing that gets you
seen**, and where the renderer transmits zero pixels during play.

This revision is the v1 draft checked line by line against the termforge source.
Six things in v1 do not survive contact with what the library actually is. They are
listed here rather than quietly patched, because each is a decision someone will
otherwise re-litigate.

| v1 said | What the source says | Resolution |
| --- | --- | --- |
| Audio sink is termforge issue **#212** | There is no #212 — the tracker tops out around #102. And termforge's dependency policy is **standard library only in the shipped library**. RtAudio is third-party. | Audio lives in GLOAM. Never file it upstream. §9 |
| Deterministic replay in the **coroutine event system** | There is no coroutine event system. The pump is an escape-sequence state machine feeding a frame loop with three protected test seams. | Replay is built on those seams, promoted to public API. §14, GL-E |
| Four dither-density masks composite over **any** wall plate | A rectangular mask cannot darken a trapezoidal side-wall plate without spilling. Per-slot masks cost 4 × 12 = 48 plates, not 4. | Replaced by six **full-frame radial light fields**. §4.4 |
| Sim 10 Hz, **render 60 fps** | 60 fps × ~2 KB of placements is ~1 Mbit/s sustained. That breaks the ssh promise outright. | Emit-on-change. An idle frame costs **zero bytes**. §4.6, §11 |
| Order of **150** resident images | An honest slot count for four wall types, three lateral positions and four depths is ~246. | Budget raised to 256 and made a test. §4.2 |
| Hold-a-number-to-aim uses kitty's **press/repeat/release** | The kitty keyboard protocol is filed as **#60** and unimplemented. Today's decoder reports presses only. | §7.4 is gated on #60. M0 does not need it; M2 does. §14 |

Everything else in v1 stands, and two things turn out to be better supported than
it knew: the tick pump it asks for already exists as `App::on_tick` /
`set_tick_hz`, and the sprite-slicing primitives landed in v0.1.18.

---

## 1 · Pillars

Load-bearing commitments. A feature that violates one gets cut, not negotiated.

1. **The lamp is the game.** Light determines what is drawn and what can see you.
   Rendering and stealth are one system, not two that cooperate.
2. **Resident art, zero streaming.** Every plate is transmitted once at startup.
   Gameplay emits placement and animation-control commands only. A session must be
   comfortable over ssh.
3. **Deterministic to the bit.** Seed in, world out. Fixed-tick integer
   simulation, recorded input, replayable sessions.
4. **Diegetic interface.** No floating state labels. Monster awareness is read from
   behaviour and sound, never from a UI element.
5. **Discoverable, not enumerated.** Magic is composed from runes the player finds
   and reasons about. Nothing is presented as a menu of known spells.
6. **Budgets are contracts.** Every number in §11 is a test that fails the build,
   not a target. Permadeath; no save scumming.

---

## 2 · Core loop

```
  DESCEND  →  EXPLORE  →  AVOID / ENGAGE  →  RECOVER  →  DESCEND
                  ↑                                          │
                  └──────────  party wiped: run ends  ───────┘
```

The party moves cell to cell through a dungeon level, managing light, noise, weight
and mana. Monsters patrol on their own schedules and perceive the party through
sight and hearing. Combat is expensive and usually worse than avoidance. Descent is
one-way.

---

## 3 · Geometry — frozen

Everything downstream depends on this and it is expensive to change later, so it is
decided here rather than in M0.

The rule that makes it cheap: **each depth is 1/√2 of the one before it, so every
second depth is exactly half.** A depth-2 plate is the depth-0 plate downsampled
2:1; depth-3 is depth-1 downsampled 2:1. The pipeline authors two rings and derives
the rest.

### 3.1 The depth ladder

| Depth | Front-face width | Height | Ratio | Derived from |
| --- | --- | --- | --- | --- |
| 0 — own cell | 480 px | 360 px | — | authored |
| 1 | 336 px | 252 px | 0.700 | authored |
| 2 | 240 px | 180 px | 0.714 | depth 0 ÷ 2 |
| 3 | 168 px | 126 px | 0.700 | depth 1 ÷ 2 |
| 4 — far cap | 120 px | 90 px | 0.714 | depth 2 ÷ 2 |

Sight distance is four cells; depth 4 is a cap plate, not a navigable ring. Every
width divides by 8 and by 24, so the dither cell never lands on a fractional
boundary.

### 3.2 Screen layout

The viewport is defined in **pixels** and never resampled. Everything else is
defined in **cells** and reflows. The reference terminal is an 80 × 24 grid at a
10 × 20 px cell — an 800 × 480 px window, the universal floor.

| Region | Cols | Rows | At reference cell | Contents |
| --- | --- | --- | --- | --- |
| Viewport | 1–48 | 1–18 | 480 × 360 px | the compositor; plates only |
| Rail | 50–80 | 1–18 | 31 × 18 cells | messages, item labels, rune entry |
| Party strip | 1–80 | 19–23 | four 20-col panels | persistent, never modal (§7.2) |
| Status line | 1–80 | 24 | 1 row | hand cursor contents, fuel, weight |

**On non-reference cell sizes** the viewport keeps its 480 × 360 px extent and
covers a different number of cells. Its cell rect is `ceil()`'d and the overhang
painted as background; sub-cell alignment uses kitty's `X=`/`Y=` pixel offsets.
**Kitty's `c=`/`r=` cell scaling is never used** — it resamples, and resampling a
pre-dithered plate is exactly the dither crawl §4.3 exists to avoid. If the window
cannot give 480 × 360 px to the viewport plus 6 cell rows of chrome, GLOAM refuses
to start (§14, upstream #91).

Reading the terminal's cell pixel size is **not currently possible through
termforge** — filed upstream as #100. Until it lands, GLOAM parses `TIOCGWINSZ`'s
`ws_xpixel`/`ws_ypixel` itself, behind one function, with a deletion date pointing
at #100.

---

## 4 · Rendering architecture

This is the centrepiece. Build it first.

### 4.1 The compositor thesis

**Do not raycast.** Eye of the Beholder and Dungeon Master did not render 3D — they
were sprite compositors. There is a fixed set of wall positions relative to the
viewer, and each position has a pre-drawn, pre-scaled plate. Rendering a frame
means placing roughly two dozen sprites into known slots.

Applied to kitty this inverts the bandwidth problem. Every plate is transmitted
once at startup, each with its own image ID, and stays resident. During play a
frame is a list of placement commands — image ID, cell position, z-index, crop. No
PNG payloads, no base64 through the tty during gameplay. A whole level's worth of
movement is a few hundred KB of *commands*.

### 4.2 Slot inventory

Twelve wall slots: left-side and right-side trapezoids at depths 0–3, plus front
faces at depths 1–4. Floor and ceiling bands are one pair per depth.

| Class | M0 | Full game | Note |
| --- | --- | --- | --- |
| Wall slots × wall types | 24 | 48 | 2 types in M0 (plain, door); 4 at M3 |
| Floor / ceiling bands | 8 | 8 | one pair per depth |
| Light fields (full-frame) | 6 | 6 | §4.4 |
| Monster poses | 27 | ~120 | 3 depths × 3 lateral × 3 poses × types |
| Items, decorations, floor features | 0 | 34 | |
| UI frames, rune glyphs, portraits | 6 | 30 | 24 rune glyphs at M2 |
| Transition frame sequences | 3 | 6 | animation registrations, not placements |
| **Total resident images** | **71** | **~246** | hard cap 256, tested (§11) |

### 4.3 Dither stability

Dither crawl — the quantization pattern shimmering as the viewpoint moves — is the
classic hard problem when you dither a live 3D render per frame. Pre-baked plates
sidestep it completely: the dither is burned in offline, so stepping forward does
not make the pattern swim, because the pixels are literally the same pixels. You
get the look without solving the problem.

**Requirement: all quantization happens in the offline pipeline (§10). Nothing is
dithered, scaled or resampled at runtime, ever.**

### 4.4 Light as a full-frame field

v1 proposed per-slot dither-density masks. They do not work: a rectangular mask
cannot darken a trapezoidal side-wall plate without spilling over its neighbours,
and shaping one per slot costs 48 plates.

The fix is to notice that **in a fixed-slot compositor, screen position *is*
depth.** The centre of the frame is far; the edges are near. So light falloff is a
single full-frame image: a 480 × 360 screen-door transparency field, pre-baked,
whose hole density grows outward from the vanishing point. Six of them — one per
lamp level — composited above the geometry band and below the sprite and text
bands.

| Lamp level | Lit radius (cells) | Field plate | Fuel / 100 ticks | Reads as |
| --- | --- | --- | --- | --- |
| 5 — bright | 5 | L5 | 4 | wasteful, loud, safe |
| 4 | 4 | L4 | 3 | |
| 3 — default | 3 | L3 | 2 | the working setting |
| 2 | 2 | L2 | 1 | |
| 1 — guttering | 1 | L1 | 1 | adjacent cells only |
| 0 — doused | 0 | L0 (opaque) | 0 | navigate from memory and sound |

This is screen-door transparency, exactly what period engines did without an alpha
channel. Period-correct and cheapest simultaneously — and because the same integer
`lamp_level` drives both the field plate and the perception model in §6.3, dousing
the lamp makes the render go dark *because* the party became invisible. One
variable, both systems.

### 4.5 Compositing bands

| Band | z | Content |
| --- | --- | --- |
| Overlay images | z ≥ 0 | party panel overlays, hand cursor, rune inscriptions |
| Text cells | — | readouts, item labels, rune entry |
| Light + sprites, upper | z < 0 | light field, then monster sprites beneath it |
| Geometry, lower | z < 0 | wall / floor / ceiling slot plates |
| Cell backgrounds | — | ambient tint |
| Below background | −1073741825 | static frame furniture |

That threshold integer must appear **exactly once in the codebase**, inside a named
layer API. termforge has no layer API today; GLOAM ships one and offers it upstream
(§14, GL-B2).

### 4.6 Emit-on-change

v1's "sim 10 Hz, render 60 fps" is a bandwidth failure: 60 frames × ~2 KB of
placement commands is roughly 1 Mbit/s sustained, which is not an ssh session, it
is a video stream.

The compositor instead builds a **placement list** each tick and diffs it against
the one on screen. Identical list, zero bytes emitted. In a discrete-cell game with
90° turns, an idle frame is genuinely idle — nothing moves between commits except
monster animation poses and lamp flicker, and those are two or three placements,
not twenty-four.

| Situation | Emitted |
| --- | --- |
| Idle — nothing changed | **0 bytes** |
| Monster pose advance, lamp flicker | ≤ 400 B |
| Full recomposition (a step or a turn) | ≤ 2 KB |
| A 140 ms transition | one animation-control command |

### 4.7 Step transitions

Pre-bake "step forward", "step back" and "turn left/right" as kitty animation frame
sequences, registered at startup. Trigger with one animation-control command and
the terminal plays the sequence: you are not streaming motion, you are firing a cue.

Transition budget **140 ms** (v1 guessed 120–180; 140 is the value to tune against,
and it is a named constant, not a literal). Must be interruptible — inputs queued
during a transition are honoured, not dropped. termforge has no animation-sequence
API; ROADMAP 6.3 is the stub. GLOAM builds it (§14, GL-C).

### 4.8 Image lifecycle

Per spec, clear-screen and main↔alt-screen transitions clear images. With ~246
resident plates referenced by every frame, a lifecycle bug means a full re-upload
mid-session and a visible stall.

**Write the lifecycle tests before any game logic exists.** Cover alt-screen
enter/exit, SIGWINCH resize, terminal reattach, and SIGTSTP suspend/resume.

There is a second hazard v1 did not know about: **termforge's KittyDriver LRU-evicts
its per-region image IDs.** That policy is correct for a dashboard and actively
hostile to a resident plate set — an eviction is a silent re-upload. Pinning is the
first thing GLOAM needs from the library (§14, GL-A1).

---

## 5 · Determinism and the tick pump

### 5.1 Simulation model

- Fixed-tick integer simulation. **No floating-point in simulation state.** Floats
  are permitted in the render layer only, and render never feeds back into
  simulation.
- Seed is `uint64_t`. Every subsystem draws from its own named stream —
  `rng(seed, subsystem_id)` — so adding a subsystem later does not perturb existing
  ones and old replays stay valid.
- No wall-clock reads in sim, no `std::random_device`, no order-dependent iteration
  over unordered containers.
- Replay format: seed plus ordered `(tick, key, event_type)`. Bug reports arrive as
  playable artifacts, and the replay log doubles as the regression test format.

### 5.2 The pump is a policy, and termforge already has it

v1 asked for a swappable pump policy object. The library shipped one in v0.1.8:
`App::on_tick(dt)` is called between the input pump and `on_render`, and
`set_tick_hz(n)` switches from a variable delta to an accumulator delivering an
integer number of ticks at a constant dt of exactly 1/n, carrying the remainder.

| Mode | Pump | Implementation |
| --- | --- | --- |
| Real-time | ticks advance on a wall-clock schedule | `set_tick_hz(10)`, `set_frame_ms(33)` |
| Step-timed | ticks advance only on a committed player action; monsters consume N ticks per action | `set_tick_hz(0)`, GLOAM drives its own tick counter from `on_tick` |
| Replay | ticks advance from a synthetic clock, as fast as the CPU allows | `set_max_tick_dt(0)` disables the stall clamp — documented upstream as being for exactly this |

Both play modes share **all** game code — same AI, same patrols, same perception,
same pathfinding. Only the pump differs. That makes real-time vs step-timed an
empirical question answerable in an afternoon and toggleable mid-session by a
playtester, which is far more informative than two builds. **Shipping both
permanently as a mode selector is a legitimate outcome, not a failure to decide.**

One caveat the library states and GLOAM must honour: ticks keep firing while a
modal overlay is up. A game pauses; check `modal()` and skip.

---

## 6 · Perception, noise and light

The target experience: you glimpse a monster crossing a distant intersection, left
to right, and you do not know whether it registered you. Everything in this section
serves that moment. Every number below is an integer, tunable from one data file,
and none of them are floats.

### 6.1 Awareness

```
UNAWARE → SUSPICIOUS → SEARCHING → HUNTING → LOST_TRACK → UNAWARE
```

**The player never sees a state label.** Awareness is read entirely from behaviour,
so every transition has an authored tell — and the tell is the deliverable, not the
state machine.

| Transition | Condition | The tell (this is the work) |
| --- | --- | --- |
| UNAWARE → SUSPICIOUS | one perception hit, heard or seen | patrol rhythm breaks — halt for one tick, head turns toward the source |
| SUSPICIOUS → SEARCHING | second hit within 30 ticks, or 3 ticks of unbroken LOS | leaves the patrol route, walks to the last known position |
| SEARCHING → HUNTING | LOS clear **and** party lit **and** range ≤ sight | gait changes, direct pursuit, one audio sting |
| HUNTING → LOST_TRACK | 8 ticks with no perception hit | casts about at the last position, turning in place |
| LOST_TRACK → UNAWARE | 40 ticks | walks back to the patrol route and resumes |

### 6.2 Noise

Every action emits an integer noise value at a grid position. Noise propagates
through the cell graph with per-edge attenuation. A monster hears if attenuated
noise at its own position exceeds its threshold.

| Emitter | Value | | Attenuation per edge | Δ |
| --- | --- | --- | --- | --- |
| Step — unarmoured | 8 | | Open corridor cell | −2 |
| Step — leather | 14 | | Corner | −6 |
| Step — mail | 26 | | Open doorway | −8 |
| Step — plate | 44 | | Closed door | −40 |
| Creep (costs 2 ticks) | × 0.5 | | | |
| Door open / close | 60 | | **Hearing threshold** | |
| Melee swing / hit | 70 / 90 | | keen / normal / dull | 20 / 40 / 70 |
| Cast | 10 × power cost | | while HUNTING | −10 |
| Fall | 120 | | | |

**Armour weight is the key coupling.** Plate is protection bought with audibility:
a plated character steps at 44 and is heard by a normal-threshold monster through
five open cells; unarmoured at 8 is inaudible past the fourth. That makes the
loadout decision tactical rather than a stat-block comparison, and it is why §7.1
keeps per-character equipped slots when it throws away everything else.

### 6.3 Light

A monster sees the party if — and only if — line of sight is clear, **and** the
party's cell illumination is greater than zero, **and** range is within its sight
distance (4 / 6 / 8 cells by type). Illumination is the same `lamp_level` integer
that selects the light-field plate in §4.4. There is no second light model.

A doused lamp means unlit, which means effectively invisible beyond adjacent cells
— and also means **the render goes dark**. Moving unlit should be genuinely
disorienting: you navigate from memory and sound. This is the mechanic that makes
the game, and it costs almost nothing to implement, because the renderer already
keys off light level. Fuel is a strict consumable (§18 Q3) and managing it is a
real strategic layer.

One monster type at M2 carries `sees_unlit = true`. Its existence is the reason
dousing is a decision rather than a dominant strategy, and it should not appear
before the player has learned to trust the dark.

### 6.4 Patrols

Monsters follow authored or generated patrol routes with idle variation, so the
world feels alive before it feels threatening. Patrol schedules are
seed-deterministic and drawn from the `patrol` RNG stream, never the shared one.

---

## 7 · Party and inventory

Four characters. The known failure mode is that moving items between four
inventories becomes a slog, because comparison and transfer compete for the same
screen space. termforge's widget set solves rendering an inventory; it does not
solve this.

### 7.1 Shared party pack, per-character equipped slots only

Everything unequipped lives in a single pool with a combined weight limit. The only
per-character inventory is what is wielded and worn. This eliminates the great
majority of cross-member shuffling — no moving torches between backpacks — while
preserving the decision that matters: what each character carries into a fight. The
loadout layer survives; the bookkeeping does not.

### 7.2 Persistent party strip

Four compact panels always on screen (rows 19–23), with a detail overlay for one
character at a time. Comparison context never fully disappears, which is what modal
per-character screens normally destroy.

### 7.3 The hand cursor

An item picked up rides in the cursor across panel switches and screens, and is
reported on the status line. Physical, obvious, no modal state to explain. This is
the single best affordance the genre produced — take it directly.

### 7.4 Combat input — gated on upstream #60

Bind `1`–`4` to character actions. Hold a number to surface that character's
options, release to commit. This needs true press/repeat/release with disambiguated
modifiers, which is the kitty keyboard protocol — **filed upstream as #60 and not
implemented.** Today's decoder reports presses only, and a timing heuristic to fake
release is exactly the thing this design is avoiding.

M0 and M1 do not need it. **#60 must land before M2 combat starts.** If it slips,
the fallback is press-to-open / press-to-commit, which costs one keystroke and no
ambiguity — and step-timed mode makes even that acceptable.

---

## 8 · Magic — rune composition

Spells are composed, not selected. The player finds rune inscriptions in the
dungeon and works out what combines with what.

### 8.1 The grammar is the hint

A spell is an ordered sequence of up to four runes: Power, Element, Form, Modifier.
Power and Element are required. Combination space is 6 × 6 × 7 × 7 = **1,764**.
Valid spells number 60–80. Discovery is the content.

**The syllables encode their own slot, and nothing tells the player so.** Power
runes begin with a **stop** (k t p d b g). Elements begin with a **fricative**
(s sh f v th h). Forms begin with a **liquid or nasal** (l r w y m n). Modifiers
begin with a **vowel**. So an inscription that opens on a vowel is a modifier, and
a player who notices that has inferred the grammar from four wall carvings —
without a spell list, a tutorial, or a UI element. That is pillar five doing real
work.

### 8.2 The twenty-four runes

**Slot 1 · Power** — stops, required. Sets mana cost and magnitude.

| Rune | Mana | Magnitude |
| --- | --- | --- |
| KAI | 1 | a flicker |
| TOR | 2 | slight |
| PEL | 3 | ordinary |
| DUN | 5 | strong |
| BRAM | 8 | severe |
| GOTH | 13 | ruinous |

**Slot 2 · Element** — fricatives, required. The substance of the effect.

| Rune | Substance | Note |
| --- | --- | --- |
| SIL | light | also refuels a lamp |
| SHOR | cold | slows, and quiets |
| FEN | rot | over time, not at once |
| VAST | stone | walls, doors, weight |
| THULE | void | silence — a noise sink |
| HESH | flame | loud, bright, obvious |

**Slot 3 · Form** — liquids and nasals, optional. Shapes it.

| Rune | Shape | Note |
| --- | --- | --- |
| LOM | bolt | travels the corridor |
| REND | touch | adjacent cell only |
| WYRD | ward | persistent barrier on a cell edge |
| YARN | field | fills a cell and its neighbours |
| MOTE | mote | a placed object that persists |
| NIL | self | the caster |

**Slot 4 · Modifier** — vowels, optional. Alters delivery or duration.

| Rune | Alters | Note |
| --- | --- | --- |
| ARR | extend | duration × 3, cost × 2 |
| ESK | quieten | cast noise halved |
| ITH | split | two targets, half magnitude |
| OTH | delay | fires 10 ticks later |
| UMBRA | invert | the effect runs the other way |
| YRN | bind | attaches to an object, not a place |

Original vocabulary. Do not import syllables from an existing game, and do not let
the generator print the class of a rune on its inscription. Machine-readable form:
`data/runes.json`.

### 8.3 Rules

- Invalid combinations fail loudly and **still cost mana**. Experimentation has a
  price.
- A subset of invalid combinations are actively dangerous rather than merely inert.
  `GOTH·HESH` with no form is the canonical example: all that power, nowhere to go.
- No spell list, ever. No known-spells screen, no autocomplete. Players keep notes —
  that is the intended experience.
- Runes are learned by finding physical inscriptions, placed by the level generator
  from the same valid-combination table the resolver reads, so a level never
  teaches a rune whose partners are unreachable.
- Casting emits noise (§6.2) at 10 × the power rune's cost. `GOTH` in a quiet
  corridor is 130 — audible to a dull-eared monster through a closed door.

Implementation is a pure function: `(RuneSeq, CasterState, WorldState) → SpellOutcome`.
Deterministic, exhaustively unit-testable across all 1,764 inputs, and the valid
table is a data file (§12).

---

## 9 · Audio — RtAudio

**Audio is a mechanic here, not decoration.** If the player can be heard, the
player must be able to hear. Monster footfalls, door noise and distance attenuation
are how §6 is read when the corridor is dark. Silent stealth is a materially
different and worse game, so this cannot be deferred the way a typical nice-to-have
can. **Target a working sink by M2, prototyped in M0.**

### 9.1 It lives in GLOAM, permanently

termforge's stated dependency policy is standard library only in the shipped
library. RtAudio is a third-party dependency, so an audio subsystem cannot land
upstream without breaking the library's central promise. **Do not file it.** GLOAM
owns audio; termforge stays audio-free; the two never learn about each other.

### 9.2 The threading contract

RtAudio hands you a real-time callback thread. The simulation is a deterministic
integer machine. The entire design of this subsystem is the wall between them.

| Rule | Why |
| --- | --- |
| One output stream. 48 kHz, float32, 256-frame buffer (≈ 5.3 ms). | Small enough that a footfall feels co-incident with its frame; large enough not to xrun on a loaded laptop. |
| The callback never allocates, never locks, never touches sim state. | Any of the three is a dropout, and a dropout in a stealth game is a lie about the world. |
| Sim → audio is one SPSC ring of fixed-size `VoiceCommand`. | Lock-free in one direction only. The ring is the whole interface. |
| A full ring **drops the command and increments a counter**. It never blocks. | A blocked sim is a stalled tick, which is worse than a missing footstep. The counter is a budget assertion, not a log line. |
| **Audio → sim is nothing. Ever.** | No feedback path means determinism holds by construction, and `--mute` produces a bit-identical replay. This is the property that makes the whole subsystem safe. |
| All PCM is decoded at startup into the pack's resident audio arena. | No decoding on the callback thread; the audio pack shares §10's manifest hash. |
| Device loss is a degradation, not a crash: sink goes silent, game continues, message line reports it. | Failing to open a device is **not** fatal — unlike a missing kitty protocol, which is (§17). |

### 9.3 The interface, and the one clever part

The game only ever calls `Sink::play(SoundId, Gain, Pan)`. A null sink satisfies it,
so core code compiles and tests with no audio at all, and RtAudio stays behind one
translation unit.

Gain and pan are **computed by the same noise-propagation function monsters hear
through** (§6.2), evaluated from the party's position instead of the monster's.
What the player hears and what the monster hears are one system read from two
positions. That is pillar one applied to sound — and it means tuning attenuation
tunes the stealth model and the mix together, which is the only way they stay
honest with each other.

Latency budget: **≤ 20 ms** from the tick that emits a sound to its first sample.

---

## 10 · Asset pipeline

All assets generated offline. Nothing generated at runtime, no model in the shipped
binary.

| Asset | Source | Processing |
| --- | --- | --- |
| Wall / monster / item plates | generated image | Compose at the depth-0 and depth-1 rings only, derive depths 2–4 by exact 2:1 downsample, then deterministic dither to the fixed palette |
| Light fields | procedural | Six radial screen-door masks at 480 × 360, baked from the vanishing point |
| Ambience, footfalls, stings | generated SFX | Normalise, trim, tag with an attenuation class, decode to raw PCM into the pack |
| Trailer | generated video | Marketing only — never played in the grid |

**Palette:** four colours plus transparent, 1-bit dithered. Aesthetic discipline and
payload discipline in one decision. The dither kernel is a fixed ordered matrix
with a fixed seed — the pipeline run twice on the same input must produce
byte-identical plates, because the pack hash is a build gate.

Bake into a versioned pack with a manifest hash. The build verifies it. Because
plates are transmitted once at startup, pack integrity is a hard startup
requirement: a mismatched hash refuses to launch rather than half-uploading a
corrupt plate set.

---

## 11 · Budgets

See `BUDGETS.md` for the full table. The headline commitments:

- ≤ 256 resident images, ≤ 1.2 MB cold-start payload
- **0 bytes on an idle frame**, ≤ 2 KB on a full recomposition, ≤ 8 KB/s p95
- ≤ 4 ms per simulation tick at 10 Hz, ≤ 2 ms to compose and emit
- ≤ 20 ms tick-to-sample audio latency, 0 dropped voice commands per 1000 ticks

These are not optimisation targets. Each is a CI assertion wired from the first
commit.

---

## 12 · Data schemas

See `SCHEMAS.md`. Four formats: `pack.manifest`, `level.gloam`, `replay.gloam`,
`spells.data`. All versioned, all with a magic number.

The load-bearing detail: `replay.gloam` carries a `ruleset_hash` covering every
tunable integer in §6 and §8, and a replay recorded against different tuning is
**rejected at load** rather than silently mis-played — otherwise a golden test
starts passing for the wrong reason.

---

## 13 · Test plan

See `TEST-PLAN.md`. Four suites: image lifecycle (written before any game logic),
the determinism harness (golden replays under a synthetic clock), property tests on
perception, and the budget assertions.

Matching termforge's own discipline: every fix ships with a test verified to fail
without it, and terminal-protocol behaviour is checked in a real pty, not only in
the suite.

---

## 14 · termforge — what exists, what is missing

See `UPSTREAM-ISSUES.md` for the full epic breakdown. The summary:

The library is **further along than v1 assumed** in the places that matter for
simulation — the tick pump, the exception-safe teardown, image sub-rect ops, mouse
mode, cell attributes — and **further behind** in the places that matter for a
resident image set.

Five epics: **GL-A** application-owned resident images (blocks everything; GL-A1
pinning against LRU eviction is the single hardest blocker), **GL-B** placement and
layering (#83 and #100 already filed), **GL-C** terminal-side animation, **GL-D**
input and startup (#60, #91, #97 already filed), **GL-E** determinism and replay
(#28 already filed).

**Audio is not a termforge issue** and never will be — §9.1.

### Working with the repo

Conventions live in termforge's `AGENTS.md`; live state in its `STATUS.md` and the
tracker. The project's culture is worth matching rather than working around: every
fix ships with a test verified to fail without it, terminal-protocol changes are
checked in a real pty, and mutation checks are routine. **GLOAM's upstream issues
should arrive in that register** — a named bug class, an acceptance test, and the
mutation that proves the test bites.

---

## 15 · Milestones

Each milestone is a gate. Do not begin the next until the current one is honestly
passed.

### M0 — the corridor slice

One corridor. Four cells long, one intersection. One patrolling monster with the
**full** perception model. One character. No inventory, no combat, no magic. You
can move, carry a lamp, and douse it.

Also in scope: the compositor, plate residency, light fields, one step transition,
the image lifecycle tests, the byte and cold-start instruments, and a rough audio
sink — because the corridor question cannot be answered in silence.

**Gate:** does the corridor moment land? Does glimpsing that monster cross the
intersection produce genuine tension?

This slice is deliberately tiny and it answers the most expensive question in the
project. It also settles real-time vs step-timed definitively, because the pump
toggles against the same scene. Party vs solo is a UI and content question that can
wait; feel cannot.

### M1 — a level

Procedural single level. Doors, keys, fuel, one item class. Four-character party
with the §7 inventory model. Save/quit, replay recording.

**Gate:** is a full level's traversal still tense at minute thirty, or does the
perception model become predictable?

### M2 — systems

Combat. Rune magic with the full valid table. Audio sink working end to end. Three
monster types with distinct perception profiles. Weight and noise coupling tuned.
**Requires upstream #60.**

**Gate:** are avoidance and engagement both viable, or does one dominate?

### M3 — depth · M4 — release

M3: multiple levels, descent progression, monster and item variety, permadeath run
structure, difficulty pacing. M4: polish, accessibility, terminal compatibility
matrix, packaging.

---

## 16 · Risks

| Risk | Severity | Mitigation |
| --- | --- | --- |
| Perception model reads as arbitrary rather than tense | **High** | M0 exists solely to test this. Invest in the §6.1 tells over AI sophistication. |
| Image eviction or lifecycle bugs force mid-session re-upload | **High** | GL-A1 first, lifecycle tests before game logic, cold start measured from commit one. |
| Upstream GL-A never lands and GLOAM must fork or vendor the driver | **High** | Keep all kitty calls behind GLOAM's own layer API from day one, so a vendored driver is a swap and not a rewrite. |
| Startup upload is slow over ssh | Medium | GL-A3 shared memory; the §11 budget is a test, not a hope. |
| Art volume exceeds solo capacity | Medium | The 1/√2 ladder means depths 2–4 are derived, not authored. Monster breadth is the lever — cut breadth before quality. |
| Real-time combat with four characters becomes unreadable | Medium | §7.4's input model. If #60 slips or it fails, step-timed mode is already built and shipping-ready. |
| Rune system frustrates rather than intrigues | Medium | Seed level one with enough inscriptions to make the §8.1 grammar inferable. Test with players who have not seen the table. |

---

## 17 · Non-goals

Written down so they do not get helpfully added later.

- **No raycasting, no free-look, no continuous rotation.** Discrete cells, 90°
  turns — the premise, not a limitation.
- **No runtime dithering, scaling or image generation.**
- **No sixel fallback, no half-block tier, no ASCII tier.** Kitty only; refuse to
  start otherwise.
- **No multiplayer of any kind.**
- **No spell list UI, no known-spells screen, no rune autocomplete.**
- **No mouse-required interaction.** Mouse may be supported; keyboard must suffice.
- **No complex script or text shaping.**
- **No save scumming.** One run, one outcome.

The third is worth a sentence, because it contradicts termforge's own central
value: the library's pitch is degradation-as-events, and GLOAM opts out entirely.
That is the point of upstream #91 — an app declaring a floor rather than the
library guessing one for it.

---

## 18 · Open questions

The ones that block M0 are decided here, with the reasoning, so they can be
overruled deliberately. The rest stay open.

| | Question | Decision |
| --- | --- | --- |
| **Q3** | Lamp fuel: strict consumable, or rechargeable at safe points? | **Strict, for M0.** The tension is the deliverable and rechargeable fuel removes it before it has been tested. Revisit at M1 with real session-length data. |
| **Q4** | One shared lamp, or one per character? | **Shared.** Simpler, and it makes dousing a collective decision rather than four independent ones. Also halves the light state the compositor tracks. |
| **Q5** | Do sight and hearing vary by awareness state? | **Yes.** HUNTING subtracts 10 from the hearing threshold (§6.2). Nothing else varies until M0 says otherwise. |
| **Q6** | Viewport dimensions and plate resolutions | **Frozen in §3.** 480 × 360, five-step 1/√2 ladder, 80 × 24 reference grid. |
| **Q7** | Transition length | **140 ms**, as a named constant. Tune against the real-time pump, where it competes with the monster tick rate. |
| *Q1* | Character classes, or skill growth by use? | *Still open.* Growth-by-use fits the discovery pillar and avoids a creation screen. Does not block M0 — M0 has one character with no progression. |
| *Q2* | Party recruited in-dungeon, or assembled up front? | *Still open.* In-dungeon recruitment makes permadeath survivable and pairs with descent progression. Decide at M1, when the party exists. |

---

## 19 · Build order

See `BUILD-ORDER.md` for the gated steps and acceptance criteria.

**Steps 1–9 exist only to reach step 10 as cheaply as possible.** The whole project
has one expensive question in it. Nothing in §7 or §8 makes that question easier to
answer, and building either first makes it more expensive to act on the answer.
