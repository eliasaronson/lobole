# Assemblies A1 — multi-part projects — design (for sign-off)

> Status: design for Elias's sign-off. A1 is mostly an architectural
> refactor (untangling the one-document assumption) with real UX decisions —
> not something to land blind in an autonomous run. This doc fixes the
> direction and lists the decisions that need you.

## Gate status (2026-07-11)

The assemblies plan lists four gates. Three are now cleared:

| Gate | Status |
|---|---|
| 1. Incremental regeneration | **Done** (R3 prefix cache) — an assembly edit re-runs only the changed part's changed suffix. |
| 2. Renderer instancing | **Remains** — needed for A2 (many occurrences of one part), NOT for A1 (each part still one tessellation). |
| 3. Toponaming | **Done** (T1–T5) — needed for A3 mates, not A1/A2. |
| 4. Diffable directory format | **Done** (F3 `.dsgn`) — the `parts/<name>/` layout below extends it. |

So **A1 is unblocked**; A2 waits only on renderer instancing; A3 waits on
nothing (toponaming is done) but is the largest UX piece.

## What A1 is

A **project** holds several **part documents** instead of one. No cross-part
references yet — just "open several parts in one file, switch between them."
Most of the work is untangling the singleton `one document` assumption, not
new geometry.

### On-disk layout (extends F3)

```
robot.dsgn/                 # today: one part. A1: a project of parts.
  project.json              # {format, version, parts: [{name, dir}], active}
  parts/
    base/                   # each = today's .dsgn part directory (index.json + features/)
    arm/
    gripper/
```

A single-part `.dsgn` (today's format) stays valid: loading one detects "no
`project.json`, just an `index.json`" and wraps it as a one-part project.
`DocumentIO` gains `SaveProject`/`LoadProject` over the part-level
Save/LoadDir it already has.

### Code shape

- A `Project` owns `std::vector<std::unique_ptr<Document>>` + an active
  index. `AppState.document` becomes `state.project->active()`.
- The **singleton untangling** is the bulk: every `state.document->` site
  (there are ~many across ui/, remote/, main.cpp) keeps working via
  `active()`, but ownership, undo stacks (one `CommandStack` per part), and
  the regen cache (one per part — already per-Document) move under `Project`.
- Autosave/checkpoints/git (F2/F4/F5) operate at the **project** level (the
  `.dsgn` dir is already the git repo; a project is the same repo with more
  parts).

### UI

- **Part tabs** above the viewport (or a project section at the top of the
  feature tree); the tree shows the active part's features. Switching parts
  swaps the active document + camera framing.
- File > New Part / Delete Part / Rename Part.

### RPC / MCP

- `list_parts`, `add_part {name}`, `set_active_part {name}`,
  `delete_part {name}`. Every existing document RPC keeps acting on the
  active part — so agents get multi-part for free once they can switch.

## Decisions that need you

1. **Project container vs. document type.** Is a project a thin container of
   independent part documents (this doc's assumption — simplest, and matches
   Onshape's "tabs"), or a first-class document that can itself hold assembly
   occurrences (folding A2 in earlier)? I recommend the thin container: it
   makes A1 a pure refactor and keeps A2 a clean addition.
2. **Tabs vs. tree-section UI** for part switching. Tabs read as more
   Onshape-like; a tree section is less chrome. Your call on the workbench
   feel.
3. **When to do A2 (occurrences).** A2 is where an assembly becomes *useful*
   (place parts in space, eyeball a fit) but needs renderer instancing first.
   Do you want the instancing + A2 push right after A1, or interleave other
   work? Renderer instancing is itself a meaty rendering change (one
   tessellation per part, per-instance transform/material — the 10–100×
   memory lever), worth its own design pass.

## Why not implement A1 now

The singleton untangling touches ownership across the whole UI/RPC surface
and has the two UX decisions above baked in — exactly the kind of change that
wants a human in the loop, not a blind autonomous pass. Everything it depends
on is shipped, so it's ready to build the moment you weigh in on (1)–(2).
