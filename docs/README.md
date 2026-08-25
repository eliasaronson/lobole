# Designer docs

A map of what lives where. **[roadmap.md](roadmap.md) is authoritative for
status** — it holds the vision, the numbered workstream table and the
sequencing. This file says where to look; it deliberately does not repeat the
statuses, because two lists of the same thing drift apart.

Each area directory has a `plan.md` (the design and its decisions) plus
per-milestone designs beside it. A plan is written before the work and updated
after it with what actually happened — including what was tried and rejected,
which is usually the part worth reading later.

**Update the status banner in the same commit as the work.** A doc audit on
2026-08-25 found three banners contradicting the roadmap — SSAO listed as
"remaining" six weeks after it shipped, the C++23 work as "not started" below
its own table of completed rows, and the roadmap naming git branching as the
last unbuilt thing four days after F7 shipped it. One of those cost something
real: `interchange/import-plan.md` still described 3D splines as a gap, so
nobody re-read the Onshape importer's `fitSpline` skip, which had been dead
code since format v42. A stale line does not just misinform — it makes the
thing behind it invisible to the next person who looks.

## Workstreams

| Area | Start at | Also inside |
|---|---|---|
| [sketching/](sketching/plan.md) | the 2D constraint sketcher | `phase-*.md` per phase; `copy-links.md`, `reference-image-plan.md` |
| [modeling/](modeling/plan.md) | 3D features | `toponaming.md` + `toponaming-design.md` (persistent face/edge refs), `m3-extrude.md`, `curves-3d-plan.md`, `thread-rib.md`, `regeneration.md` |
| [variables/](variables/plan.md) | parameters + expressions | |
| [scripting/](scripting/plan.md) | the LLM/automation interface | `mcp.md`, `s1-mcp.md`, `s2-headless.md`, `error-style.md` (what a failed call must tell an agent) |
| [versioning/](versioning/plan.md) | format, autosave, git | `f1-f2.md`, `f3-directory-format.md`, `f6-history-ui.md`, `f7-merge-and-conflicts.md`, `f8-external-changes.md`, `f9-collaboration.md`, `f10-autosave-the-project.md`, `f11-seeing-what-changed.md`, `f7-handoff.md`, `collab-dogfood.md`, `baked-geometry.md`, `format.md` |
| [ui/](ui/plan.md) | selection, tools, layout | `u1`, `u7`–`u13` rounds (`u12` keybindings, `u13` chrome + units), `settings.md` (user preferences), `viewport-handles.md` (drag arrows + hover), `direct-manipulation.md` (preview drag handles), `terminal-panel.md` (an embedded terminal for Claude Code), `gui-coverage.md`, `gui-test-isolation.md` (why any shard partition must be valid), `dims-and-assembly-measure.md` |
| [inspect/](inspect/plan.md) | measure, mass properties, sections | `measure-v2.md` (measure reopened, in flight), `section-tool-design.md`, `review-checklist.md`, `agent-validation-plan.md` |
| [interchange/](interchange/plan.md) | import + export | `import-plan.md`, `feature-gaps-plan.md`, `onshape-terms.md` |
| [rendering/](rendering/plan.md) | visual fidelity | `instancing-design.md`, `ssao-soft-shadows.md`, `r4-offline-render.md`, `appearance-plan.md`, `frames-in-flight.md`, `frame-timing.md`, `outline-perf.md` |
| [assemblies/](assemblies/plan.md) | multi-part, mates, sub-assemblies | `a1`–`a5` designs |
| [simulation/](simulation/plan.md) | FEM | see also `ui/u11-simulation-setup.md` |
| [drawings/](drawings/plan.md) | 2D drafting | `guide.md` (the workflow) |
| [configurations/](configurations/plan.md) | part + assembly families | |
| [topopt/](topopt/plan.md) | topology optimization | `guide.md`, `gpu-plan.md`, `gpu3-plan.md`, `gpu4-plan.md`, `gpu5-plan.md` (**designed, not built** — the fp32 slender-part limit) |
| [packaging/](packaging/plan.md) | shipping it to other machines | |
| [cam/](cam/plan.md) | milling toolpaths + G-code (built) | `gui.md` (verbs, the picture of the cut, the verdict — built), `dogfood-round.md` (three rounds on one real coupon; ends with what to watch on the first actual cut) |

## Cross-cutting

- **[known-bugs.md](known-bugs.md)** — confirmed, unfixed defects (symptom +
  root cause + likely fix). Cleared as fixes land.
- **[dogfooding.md](dogfooding.md)** — every self-driven round that built a
  real object end-to-end, what each found, and what was done about it. The
  richest source of real bugs in this repo.
- **[performance/scale-round.md](performance/scale-round.md)** — the
  measured-at-size round (open 259 s → 0.04 s, and what was slow and why).
- **[tier1-closeout.md](tier1-closeout.md)** — the 2026-07-14 sign-off that
  closed out five workstreams' remainders at once.
- **[loose-ends-2026-08-25.md](loose-ends-2026-08-25.md)** — the same idea a
  month on: an audit of all 105 docs against the source, the six items it
  found that were started and never finished, and what a stale status line
  cost. **Awaiting sign-off.**
- **[architecture-review-2026-08-24.md](architecture-review-2026-08-24.md)** —
  a structural pass over the whole tree (layering, duplication, unit sizes,
  dead code), measured rather than estimated. All five stages shipped; the
  follow-ups at the bottom include one struck because its premise was wrong,
  which is the part worth reading.
- **[cpp23-modernization.md](cpp23-modernization.md)** — the toolchain bump
  and what it bought.
- **[ai-cad-survey-2026-08-18.md](ai-cad-survey-2026-08-18.md)** — how other
  AI-facing CAD tools and the research literature approach this.

Project-level files (`README.md`, `CONTRIBUTING.md`, `THIRD_PARTY.md`) stay at
the repo root, as does `CLAUDE.md` — the working notes loaded into every
assistant session.
