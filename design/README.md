# design/ — the specification this repo implements

`SPEC.md` is the design document the rest of the tree cites. Every `§3.2`, `§4.5`,
`§11` marker in a header, a test comment or an issue resolves to a section number in
that file.

## Why it is vendored here

It was not, and that cost real work twice. The code cites section numbers constantly
and the sections lived only in a claude.ai design project, so answering "what are
§4.5's six compositing bands?" meant leaving the repository — and a session that
could not reach the design project could not answer it at all. A specification the
code quotes belongs under the same version control as the code.

## Provenance, and which copy wins

| | |
| --- | --- |
| Canonical source | claude.ai design project `Gloam` |
| Project id | `db76ddbe-dd72-4c39-8d17-95386c576b11` |
| Folder | `design_handoff_gloam/` |
| URL | https://claude.ai/design/p/db76ddbe-dd72-4c39-8d17-95386c576b11 |
| Snapshot taken | 2026-07-31 |
| Read with | the `DesignSync` MCP tool, `get_file` |

**The design project is canonical. This directory is a snapshot.** A correction
belongs upstream first, then re-synced down — otherwise the two copies fork
silently, and a fork is worse than the absence this vendoring was meant to fix. The
handoff's own README says the same thing about itself: it is the machine-readable
copy, and it wins over the typeset `GLOAM Design Document.dc.html` in the same
project.

## What is here

| File | What it holds |
| --- | --- |
| `SPEC.md` | The whole design, §0–§19. The source of truth. |
| `BUDGETS.md` | Every enforceable number, and the test that enforces it. Mirrored in `include/gloam/budgets.hpp`. |
| `SCHEMAS.md` | `pack.manifest`, `level.gloam`, `replay.gloam`, `spells.data`, field by field. |
| `TEST-PLAN.md` | Lifecycle tests, the determinism harness, the perception properties. |
| `BUILD-ORDER.md` | Ten gated steps, each with an acceptance criterion that is a test. |
| `UPSTREAM-ISSUES.md` | The five epics as originally drafted. |
| `data/runes.json` | The 24 runes, as shipped data. Mirrored in `include/gloam/runes.hpp`. |
| `data/spells.schema.json` | Authoring schema for the valid-combination table. |

Two things in the bundle are deliberately **not** vendored:

- `CLAUDE.md` — this repo already has `AGENTS.md`, and a second conventions file
  that has never been reconciled with the first is how they drift apart.
- `GLOAM Design Document.dc.html` — the typeset copy of `SPEC.md`, for reading and
  circulation. Same content, and `SPEC.md` wins if they ever disagree.

## Where it has already been overtaken

`SPEC.md` reads against **termforge v0.1.18**; the library is many tags past that.
`UPSTREAM-ISSUES.md` predates the issues actually being filed. Neither file is
edited to keep up — a snapshot that is quietly patched is no longer a snapshot.

`../UPSTREAM.md` is the living record: it tracks the filed issue numbers, their
current state, and the corrections the design document needs. Its "Corrections to
the design document" section is the authoritative list of places where `SPEC.md` is
now wrong, each mirrored as a GLOAM issue so the decision gets made rather than
absorbed.
