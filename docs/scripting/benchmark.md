# The agent benchmark

`benchmarks/` answers one question repeatably: given only Loböle's MCP
surface and a description of a part, can an agent build the part — and does a
change to the API make that better or worse? The full description, how to
run it and how to read a result are in
[benchmarks/README.md](../../benchmarks/README.md); this page is the summary
and the rules that bind changes elsewhere in the repo.

## Status

**Unfinished.** It does not yet measure what it is for, so it neither gates a
change to a tool, a description, a refusal message, `help()`, `describe` or the
`modeling-workflow` prompt, nor backs a claim about one. When a sweep is run,
its results are committed under `benchmarks/results/<timestamp>-<model>/` and
never overwritten. A sweep costs money: `python3 benchmarks/run.py --dry-run`
prints the plan, the run count and the cost ceiling for free.

## What it grades

- **Tasks** (`benchmarks/tasks/*.json`) — a part described in prose (axes,
  origin and millimetres pinned; no unrequested fillets) plus the checks its
  geometry must pass. Static parts in tiers T0–T3, mechanisms at T4/T5 (three
  parts, mates, and at T5 a motion answer), and a Fit task graded against a
  host part the agent never sees. `reference.py` prints the current task and
  check counts; do not copy them into docs.
- **Arms** (`benchmarks/arms/*.json`) — Loböle configurations under test:
  `full` (every tool), `rpc-only` (six tools; `rpc` still reaches every RPC,
  so it loses only schema bulk), `script-first` (every tool, told to write a
  script), `no-inspect` (the floor: no tool that can ask an independent
  question about the part), and `document` (no RPC surface; the agent emits a
  whole project through `benchmarks/arms/document_mcp.py`). Arms narrow the
  server with `designer_mcp.py --only-tools`, which refuses rather than
  silently serving the full surface.
- **The agent** runs with no built-in tools (`--tools ""`), no project
  context (`--setting-sources ""`), from an empty directory, against its own
  `--headless` instance on a kernel-chosen port. The runner, not the agent,
  exports the STEP.
- **The oracle** (`benchmarks/oracle/oracle.cpp`, target `bench_oracle`)
  links OpenCASCADE and none of `designer_core`. It reads the exported STEP
  back and reports only facts a feature name cannot fake: solid count, bbox,
  volume, area, centre of mass (whole and per solid), cylindrical faces
  grouped into holes and bosses by outward normal, inclined planes, and a
  face census by surface kind. `grade.py` applies the check kinds
  (`CHECKS` there). App-reported checks (a variable drives the geometry)
  carry `"source": "app"` and are never scored.
- **Over-claims** — a failed run whose final message claims success — are
  counted separately. **pass^k** counts a task only if every rep of it passed.

## The controls

- `runners/reference.py` builds every task by hand and must score every
  check. It is free and runs first: if it fails, the harness or the app is
  broken, not the agent.
- `runners/negative_controls.py` builds the construction- and fit-sensitive
  parts the wrong way (a spring as flat rings, a taper as stacked discs, a
  cap that does not seat) and fails if any of them passes. A check nothing
  fails is not a check.
- `runners/villain.py` extrudes each task's own bbox as a plain block and
  reports every check that survives it. A task the villain passes outright is
  broken, unless it declares `villain_passable` with a reason.

## Construction checks

Coarse checks (bbox, volume, centre of mass) cannot see HOW a part was built:
a taper faked from sixty stacked discs passes all of them. The face census
can. `{"kind": "faces", "of": "cone", "min": 1}` counts faces of a surface
kind; `of` may be a list, summed, for a shape with several legitimate
constructions; `got` reports the whole census; an unknown kind fails naming
the census keys.

What each operation leaves after build, STEP export and re-read is measured,
not predicted — `runners/surface_probe.py` builds one probe per operation and
prints the census. Current results that task authors rely on:

| construction | census |
|---|---|
| a frustum by Revolve, or by Loft between two circles | `cone:1 plane:2` |
| the same frustum as N stacked discs | `cylinder:N plane:N+1`, no cone |
| a circle revolved off-axis | `torus:1` |
| a profile swept along a helix (Sweep or Pipe) | `bspline` + end `plane`s |
| a Sweep along a sketch arc | `torus` |
| a Pipe along a fit spline | `bspline` |
| a shelled box | `plane:11` (a solid box is `plane:6`) — shell changes counts, not kinds |

So a bent-tube check must accept `[torus, bspline]`, and wall thickness stays
a volume check. Construction checks go only where a wrong construction is a
real defect: a stepped shaft built from stacked extrudes is correct work.
