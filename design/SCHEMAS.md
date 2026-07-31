# GLOAM — data schemas

Four formats. All versioned, all with a magic number, none of them ad hoc.

## 1. `pack.manifest`

Header, then one fixed-size record per plate.

```
header  { magic:"GLPK", version:u16, pack_sha256:[32]u8,
          plate_count:u16, total_bytes:u32 }
record  { plate_id:u16, role:u8, depth:u8, lateral:u8, wall_type:u8,
          w:u16, h:u16, offset:u32, length:u32, sha256:[32]u8 }
```

- `role` — wall | floor | ceiling | light_field | monster | item | ui | rune
- `lateral` — left | centre | right | full_frame
- `depth` — 0–4, or 255 for full-frame plates

The build verifies `pack_sha256`. Because plates are transmitted once at startup,
pack integrity is a **hard startup requirement**: a mismatch refuses to launch
rather than half-uploading a corrupt plate set.

## 2. `level.gloam`

For authored test levels and M0's corridor only. Generated levels never touch
disk — the seed *is* the level.

```
header  { magic:"GLLV", version:u16, w:u16, h:u16, seed:u64 }
cell    { kind:u8, edges:[4]Edge, contents:[]u16, inscription_id:u16 }
Edge    { kind:u8 (open|wall|door|doorway), state:u8 (open|closed|locked),
          key_id:u16, attenuation:i8 }
patrol  { monster_type:u8, route:[]CellIndex, dwell:[]u8 }
```

`attenuation` on the edge is the noise loss for crossing it (§6.2 of SPEC). It
lives on the edge rather than the cell so that a closed door and its doorway are
one object, and so the noise graph and the render graph are the same graph.

## 3. `replay.gloam`

```
header  { magic:"GLRP", version:u16, seed:u64, ruleset_hash:u64,
          pack_hash:u64, tick_hz:u16 }
record  { tick:u32, event:u8, payload:u16 }   // packed, ordered by tick
```

- `ruleset_hash` covers **every tunable integer** in SPEC §6 and §8. A replay
  recorded against different tuning is **rejected at load**, never silently
  mis-played — otherwise a golden test starts passing for the wrong reason.
- `pack_hash` is advisory: art changes do not affect simulation, so a mismatch
  warns rather than rejects.
- Bug reports arrive as playable artifacts. Keep one replay per bug ever filed.

## 4. `spells.data`

One row per **valid** combination. Absent rows are invalid by definition.

```
row  { power:u8, element:u8, form:u8|NONE, modifier:u8|NONE,
       effect_id:u16, params:[4]i16, danger_class:u8 }
```

- `danger_class` — 0 inert, 1 backfires on the caster, 2 backfires on the party,
  3 backfires on the level (a collapsed wall, a woken floor).
- Read by **both** the resolver and the generator's inscription placement, from one
  file, so a level can never teach a rune whose partners are unreachable.
- Combination space is 6 × 6 × 7 × 7 = 1,764. Valid rows: 60–80.

See `data/runes.json` for the rune definitions and
`data/spells.schema.json` for the JSON authoring form the build compiles into
`spells.data`.
