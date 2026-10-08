# Loböle docs

The current description of the program: how each part of it works, the rules
it follows, and what it does not do yet. The history of how it got here is in
git, not here.

## Start here

- [architecture.md](architecture.md) — how the code is organised: the source
  layout, the libraries, the regeneration model, the actions layer, the RPC
  dispatcher, threading, tests and C++ usage.
- [conventions.md](conventions.md) — the rules every subsystem follows (world
  axes, units and angles, derived defaults, refuse-don't-substitute, honest
  responses and receipts) and the gate that enforces each.
- [known-bugs.md](known-bugs.md) — defects that are present now.
- [roadmap.md](roadmap.md) — what is not built yet, and in what order.

## Areas

Each area has a `README.md` that describes how it works now. A topic page
sits beside it only where the README would be too long.

| area | covers |
|---|---|
| [sketching](sketching/README.md) | the 2D constraint sketcher |
| [modeling](modeling/README.md) | 3D features, topological naming, regeneration |
| [variables](variables/README.md) | parameters and expressions |
| [configurations](configurations/README.md) | part and assembly families |
| [parts](parts/README.md) | standard parts (fasteners) |
| [tolerances](tolerances/README.md) | limits and fits, stack-ups |
| [tube](tube/README.md) | bent tube, copes, cut lists |
| [sheetmetal](sheetmetal/README.md) | flanges, bends, flat patterns, DXF |
| [assemblies](assemblies/README.md) | occurrences, mates, relations, sub-assemblies |
| [cam](cam/README.md) | milling toolpaths, posting, verification, simulation |
| [ui](ui/README.md) | selection, tools, panels, preferences, the GUI suite |
| [rendering](rendering/README.md) | the renderers, offscreen mode, stills |
| [versioning](versioning/README.md) | the file format, autosave, checkpoints, git |
| [interchange](interchange/README.md) | import and export |
| [packaging](packaging/README.md) | building for other machines |
| [scripting](scripting/README.md) | the RPC, MCP server, Python SDK |
| [inspect](inspect/README.md) | measure, mass properties, sections, checks |
| [requirements](requirements/README.md) | what a design is for, stated in the project and checked against it |
| [simulation](simulation/README.md) | FEM studies |
| [topopt](topopt/README.md) | topology optimization |
| [drawings](drawings/README.md) | 2D drafting |

Plans for work that is not built yet live in [plans/](plans/), one file per
plan. Each generated model has its own page beside its code, indexed in
[generators/README.md](../generators/README.md): what it is, how to run it,
and where its numbers come from.

## Writing docs

**A doc describes the program as it is.** Write it for a developer or an agent
opening the area cold.

An area README holds: the model and its concepts; the invariants and the
reasons for them, as present-tense facts ("an OpenCASCADE boolean modifies its
arguments, so every query boolean runs non-destructive"); where the code is;
the RPCs and GUI entry points; how it is tested; and what it does not do yet.

It does not hold: dates, status or "shipped" banners, milestone numbers used
as status, who decided what and when, approval notes, rejected alternatives,
measurements of what something used to cost, or bug stories. A number stays
only if someone needs it now (a budget, a limit).

**Keep the docs true in the same commit as the code.** When a change alters
behaviour, update every page that describes it, the RPC `describe` rows, and
the version history in [versioning/format.md](versioning/format.md) if the
format moved. A
stale line does not just misinform: it makes the thing behind it invisible to
the next person who looks.

**Every statement is checked against the code.** One that cannot be confirmed
is fixed or dropped, never kept to be safe.

**Plans.** Anything multi-step starts with a plan in `docs/plans/`: the
problem, the shape of the fix, the files it touches and how it is verified —
no dates, no decision log, no status. When the work lands, what is still
true moves into the area docs and the plan is deleted.

**Known bugs** are defects present now, each with its symptom, cause, where it
lives and the likely fix. An entry is deleted when its fix lands with a
regression test.

**References.** Code comments cite docs to explain why the code is shaped the
way it is (`docs/<area>/README.md#heading`). The cited page must state that
reason. `tests/test_conventions.cpp` fails on a cited `docs/` or
`generators/` path that does not exist, on an `#anchor` with no matching
heading, and on a relative link inside `docs/` that does not resolve — so
renaming a heading means updating what cites it.

Project-level files (`README.md`, `CONTRIBUTING.md`, `THIRD_PARTY.md`,
`CLAUDE.md`) stay at the repository root.
