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
| [modeling/](modeling/plan.md) | 3D features | `toponaming.md` + `toponaming-design.md` (persistent face/edge refs), `m3-extrude.md`, `curves-3d-plan.md`, `thread-rib.md`, `regeneration.md`, `hole-standards.md` (**planned** — a hole that knows it is an M6 clearance, from 298 rows of vendored table) |
| [variables/](variables/plan.md) | parameters + expressions | |
| [scripting/](scripting/plan.md) | the LLM/automation interface | `mcp.md`, `s1-mcp.md`, `s2-headless.md`, `error-style.md` (what a failed call must tell an agent) |
| [versioning/](versioning/plan.md) | format, autosave, git | `f1-f2.md`, `f3-directory-format.md`, `f6-history-ui.md`, `f7-merge-and-conflicts.md`, `f8-external-changes.md`, `f9-collaboration.md`, `f10-autosave-the-project.md`, `f11-seeing-what-changed.md`, `f7-handoff.md`, `collab-dogfood.md`, `baked-geometry.md`, `format.md` |
| [ui/](ui/plan.md) | selection, tools, layout | `u1`, `u7`–`u13` rounds (`u12` keybindings, `u13` chrome + units), `settings.md` (user preferences), `viewport-handles.md` (drag arrows + hover), `direct-manipulation.md` (preview drag handles), `terminal-panel.md` (an embedded terminal for Claude Code), `gui-coverage.md`, `coverage-matrix.md` (**generated** — interactive tools x tab kinds, the asymmetry that keeps costing bugs), `gui-test-isolation.md` (why any shard partition must be valid), `dims-and-assembly-measure.md`, `long-operations.md` (**planned** — one job registry for the five long operations, and the two poll bugs reading them found) |
| [inspect/](inspect/plan.md) | measure, mass properties, sections | `measure-v2.md` (measure reopened, in flight), `section-tool-design.md`, `review-checklist.md`, `agent-validation-plan.md` |
| [interchange/](interchange/plan.md) | import + export | `import-plan.md`, `feature-gaps-plan.md`, `onshape-terms.md` |
| [rendering/](rendering/plan.md) | visual fidelity | `instancing-design.md`, `ssao-soft-shadows.md`, `r4-offline-render.md`, `appearance-plan.md`, `frames-in-flight.md`, `frame-timing.md`, `outline-perf.md`, `gpu-debugging.md` (validation layers vs. a frame capture — which question each answers), `renderer-wiring.md` |
| [assemblies/](assemblies/plan.md) | multi-part, mates, sub-assemblies | `a1`–`a5` designs |
| [simulation/](simulation/plan.md) | FEM | see also `ui/u11-simulation-setup.md` |
| [drawings/](drawings/plan.md) | 2D drafting | `guide.md` (the workflow) |
| [configurations/](configurations/plan.md) | part + assembly families | |
| [topopt/](topopt/plan.md) | topology optimization | `guide.md`, `gpu-plan.md`, `gpu3-plan.md`, `gpu4-plan.md`, `gpu5-plan.md` (**designed, not built** — the fp32 slender-part limit) |
| [parts/](parts/plan.md) | standard parts (fasteners) | `gui-plan.md` (placing one by clicking — **built**), `preview-plan.md` (seeing what you are about to place — **built**), `f1-loose-ends.md` (attachment, the countersunk case, the pick-slot registry — **built**). **complete v1 (2026-09-01)** — seven vendored ISO standards, a fastener as a PART placed as an occurrence, size and length derived from the hole you pick (washers and nut included), a BOM that tells bought from made, `check_fasteners` for a stack that changed under a screw |
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
  cost. **Complete.** Three of the seven turned out to be already fixed and
  still listed as open; two "polish" items were user-facing bugs.
- **[ui/coverage-matrix.md](ui/coverage-matrix.md)** — **generated** by
  `tools/coverage_matrix.py`: interactive tools × tab kinds, the asymmetry
  that has cost three bugs. Re-run it after adding GUI tests.

### Test-harness conventions worth knowing before writing one

- `tests/smoke_app.py` is the **only** way a test launches the app
  (`launch_app()`): it binds a kernel-chosen port in the process that keeps
  it, so there is no pick-then-hand-off race, and it keeps the app's log and
  prints it on failure. `config_isolation_lint.py` fails any script that
  spawns the binary directly. On a failure it also **archives** the log, the
  traceback and a `reproduce.sh` to
  `$XDG_STATE_HOME/designer/smoke-failures/<stamp>-<test>/` — ctest's own
  `LastTest.log` is overwritten by the next run, so re-running an intermittent
  failure used to destroy the only record of it.
- `tests/rpc_params_gate.py` keeps `describe`'s parameter schema honest
  against what each handler actually reads (`tools/rpc_params_scan.py`).
- `tests/test_actions.cpp` is where UI *logic* is pinned headlessly — read its
  header before adding to it. Anything about widgets belongs in the GUI suite.
- **[architecture-review-2026-08-24.md](architecture-review-2026-08-24.md)** —
  a structural pass over the whole tree (layering, duplication, unit sizes,
  dead code), measured rather than estimated. All five stages shipped; the
  follow-ups at the bottom include one struck because its premise was wrong,
  which is the part worth reading.
- **[capability-review-2026-08-31.md](capability-review-2026-08-31.md)** — the
  outward-facing counterpart: capability walls, absent feature families, and
  what we do worse (and better) than FreeCAD and Onshape. **Findings only,
  nothing scheduled.** The three that are architecture rather than backlog:
  the assembly solve has no numeric 3D solver so closed loops are
  unassemblable, regeneration cannot cross a document boundary, and there is
  no general mechanism for running a long operation off the frame. Its second
  half is **a pass over the testing system** — what the harness does unusually
  well, and seven gaps, of which the loudest is that `tools/test-fast.sh` now
  takes 48.7 s against the ~8 s its own header claims (a by-name exclusion
  list that never learned about the CAM gates).
- **[coverage-2026-08-31.md](coverage-2026-08-31.md)** — the first recorded
  `tools/coverage.sh` run: 75% of `src/core`, and the 133 functions nothing
  executed — including the no-git checkpoint fallback, which no test machine
  reaches. (A caveat about gcov never flushing was attached and retracted on
  2026-09-01; the numbers are accurate.)
- **[cpp23-modernization.md](cpp23-modernization.md)** — the toolchain bump
  and what it bought.
- **[ai-cad-survey-2026-08-18.md](ai-cad-survey-2026-08-18.md)** — how other
  AI-facing CAD tools and the research literature approach this.

Project-level files (`README.md`, `CONTRIBUTING.md`, `THIRD_PARTY.md`) stay at
the repo root, as does `CLAUDE.md` — the working notes loaded into every
assistant session.
