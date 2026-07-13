# Assemblies A2 — assembly document + occurrences — design

> Status: **signed off 2026-07-13** (all six decisions below). A2 is where the
> app first becomes a real assembly tool: place parts in space, move/rotate
> them, eyeball a fit. Its renderer-instancing gate shipped 2026-07-12 (commit
> 6a99c38); every other prerequisite was already done. This doc leads with the
> **save format** — the decision most likely to box in A3 (mates) — then covers
> rendering, picking, the gizmo, and commands. Phase 1 (the format spine) is
> ready to build.

## Gate status (2026-07-13)

| Gate | Status |
|---|---|
| Incremental regeneration | **Done** (R3 prefix cache). |
| Renderer instancing | **Done** (6a99c38) — `MeshAsset`/`MeshInstance`; `AddInstance`/`SetInstanceTransform` are the API A2 renders through. |
| Toponaming | **Done** (T1–T5) — needed by A3 mates, not A2. |
| Diffable format | **Done** (F3 `.dsgn`) — the assembly layout below extends it. |

A2 is fully unblocked.

## What A2 is

An **assembly** is a new document kind that sits alongside parts in the same
project. Where a part is an ordered feature list that *builds* B-Rep bodies, an
assembly is an ordered list of **occurrences** that *place* the bodies of other
parts by explicit transform. No mates yet (A3); no cross-document face refs
yet (A3). Just: reference a part, position a copy of it, repeat.

This alone is a usable v1 assembly — many real workflows are "drop the parts in
and nudge them into place."

### Why it is a distinct document kind, not a feature list

The tempting shortcut — model an occurrence as a `Feature` in a `Document` —
does not work against the current regen model. `RegenContext`
(`src/core/Feature.h:39-49`) exposes only *earlier features of the same
document* (`FindById` searches `[0, currentIndex)`). An occurrence references
the bodies of a **different** part document, which no `Feature::Execute` can
reach. So an assembly is its own kind, with a placement pass instead of a
feature regen. It reuses the project container, the directory format, the
command/RPC layer, and the instancing renderer — just not the `Feature`
abstraction.

Convenient consequence of A1: the parts an assembly references are **already
loaded** in the project's `PartSlot`s (`src/ui/AppState.h:199-204`). Same-
project occurrence resolution is a lookup of a sibling part's `Document`, not a
file load. External-file part references stay an A4/F5 concern.

## The save format (centerpiece)

### Project envelope — generalize `parts` to `documents`

Today `SaveProject` (`src/ui/AppActions.cpp:557-576`) writes:

```json
{ "format": "designer-project", "version": 1, "active": 0,
  "parts": [ {"name": "base", "dir": "parts/base"} ] }
```

A2 generalizes the flat part list to a typed document list and bumps the
project envelope to **version 2**:

```json
{ "format": "designer-project", "version": 2, "active": 2,
  "documents": [
    {"id": 1, "name": "base",  "dir": "parts/base",       "type": "part"},
    {"id": 2, "name": "arm",   "dir": "parts/arm",        "type": "part"},
    {"id": 3, "name": "robot", "dir": "assemblies/robot", "type": "assembly"}
  ] }
```

- **`id`** — a stable per-document id (uint64), unique and never reused, the
  identity a part is referenced by (decision 5). `name` and `dir` are
  cosmetic/derived and may change freely; the id never does, so renaming or
  moving a part cannot break an occurrence that references it.
- **Back-compat, no migration code:** a v1 project has `parts` and no
  `documents`; the loader reads `documents` if present, else falls back to
  `parts`, defaults every entry's `type` to `"part"`, and **assigns ids in
  list order** — the same defaulted-read + assign-on-load pattern the codebase
  already uses (v1 feature files get ids stamped in list order the same way;
  `Serialization.cpp` `j.value("key", default)`).
- `active` indexes `documents` (the bottom tab bar already switches active
  document; an assembly is just another tab).
- Single-part projects still save as a flat `.dsgn` (A1's rule, unchanged); a
  project acquires `project.json` the moment it holds more than one document.

### Assembly document directory — mirrors the F3 part layout

```
assemblies/robot/
  index.json                 # {format, version, kind:"assembly", occurrences:[ids]}
  occurrences/
    1-base.json              # <occ-id>-<label>.json — one file per occurrence
    2-arm.json               # label is cosmetic (the occ id is identity, per F3)
    3-arm.json               # a second occurrence of the same part
```

The index reuses the **one** `designer-document` format string and the **one**
`kVersion` (bumped 22 → **23**), with a `kind` discriminator so `LoadDir`
branches on part-vs-assembly:

```json
{ "format": "designer-document", "version": 23, "kind": "assembly",
  "occurrences": [1, 2, 3] }
```

Keeping a single format + single version means the existing migration policy,
golden corpus, and round-trip machinery (`docs/versioning/format.md`) cover
assemblies with no parallel contract — we add one golden assembly file at the
v23 bump and it rides the same `test_golden.cpp` harness. A part index is
unchanged (no `kind` key ⇒ implicitly `"part"`, another defaulted read).

File-per-occurrence keeps the F3 property that matters for git: moving
occurrence A and occurrence B touch different files, so disjoint edits merge
conflict-free. Occurrences are tiny, so this is more about consistency than
size — see decision 4.

### Occurrence schema

```json
{
  "type": "Occurrence",
  "id": 2,
  "name": "arm",
  "part_id": 2,
  "part_version": null,
  "transform": { "position": [10, 0, 0], "orientation": [0, 0, 0, 1] },
  "visible": true,
  "fixed": false
}
```

- **`id`** — stable uint64, unique-and-never-reused, exactly like feature ids
  (A3 mates reference occurrences by this id; undo/redo and stable file naming
  need it).
- **`name`** — the occurrence's own display label (e.g. "arm", "arm (2)"),
  cosmetic; independent of the referenced part's name.
- **`part_id`** — the stable id of the referenced document (decision 5:
  by-id, not by-name). Renaming or moving the part changes its `name`/`dir` in
  `project.json` but not its id, so occurrences never break and no
  rename-propagation is needed — the same rename-safety features already enjoy
  by referencing each other by id, not display name.
- **`part_version`** — reserved for the git-commit pin the assemblies plan
  calls for (`plan.md` cross-cutting); `null` until F5 wires "reference a
  specific part version." Present now so adding it later is not a format bump.
- **`transform`** — translation + **quaternion** (`orientation` is xyzw). Seven
  meaningful numbers that diff readably and admit no shear/scale, versus an
  opaque 16-float `mat4`. It matches the existing plane convention of storing
  placement as vec3s rather than matrices (`Serialization.cpp:373-375`), and
  it is how mate frames will think. Needs a new `TrsfToJson`/`TrsfFromJson`
  pair built on the existing `Vec3ToJson` (`Serialization.cpp:35`); no
  quaternion serializer exists yet.
- **`visible`**, **`fixed`** — booleans; `fixed` is the Onshape-style grounded
  flag (a fixed occurrence is immovable and, in A3, the anchor the mate solver
  places others against).

### The principle that keeps A2 from boxing in A3

**An occurrence always stores an explicit `transform` — its last solved
placement — and is always loadable on its own. A3 mates become a separate
`mates/` list that, on load, re-solves and overwrites the transforms of
non-`fixed`, mated occurrences.**

This mirrors the format's existing literal-plus-`_expr`-twin rule for
dimensions (`docs/versioning/format.md` guarantee 4): the stored value always
loads meaningfully, and a higher-level pass (there, expression eval; here, the
mate solve) wins when present. So an A2 file stays valid and openable forever,
mates are purely additive at v-next, and a file whose mates were deleted still
opens at the last good positions instead of collapsing to the origin. Locking
this now is the whole reason to design A2's format with A3 in view.

## Rendering (migrate to the instancing API)

`SyncDocumentRenderers` (`src/ui/AppActions.cpp:682-739`) currently calls the
transitional `AddMesh` once per body. When the active document is an assembly:

- For each **distinct** referenced part: regenerate the sibling part's
  `Document`, tessellate its bodies once → `CreateAsset` (`MeshRenderer.h:91`).
- For each **occurrence**: `AddInstance(assetId, occurrenceTransform)`
  (`MeshRenderer.h:96`) with the occurrence's `mat4` (built from its
  translation+quaternion) and its material/visibility.

N occurrences of one part ⇒ one tessellation, N instances — exactly the lever
instancing was built for. A part document renders as today (one asset + one
identity instance per body).

## Picking → occurrence

Extend the hit→identity join. `DocumentRenderMesh` (`src/ui/AppState.h:30-37`)
currently maps `meshId → bodyIndex` under the one-active-document assumption; it
gains an **occurrence id**. The `meshId→bodyIndex` lookups
(`AppActions.cpp:427-436, 1122-1136`) resolve, in an assembly, to
`instanceId → occurrence`. A single click selects the occurrence; double-click
enters the referenced part (opens its tab). This is where the **FaceSelector /
OutlineRenderer instance-space split deferred from instancing phase 1** lands —
A2 is its first real multi-occurrence consumer, so it is built and tested here.

## Move/rotate gizmo

Net-new — there is no interactive manipulator today (`AppendOriginGizmo`,
`AppActions.cpp:147-168`, only draws non-interactive lines). The gizmo builds
on two existing pieces: the `LineVertex3D` overlay path for drawing the handles
and the `FaceSelector`/ray infrastructure for hit-testing them. Drag updates
the occurrence transform live (the renderer instance follows each frame); on
release, one snapshot command is pushed for a single clean undo step. This is
the largest non-format piece of A2 and its own implementation sub-plan.

## Commands / RPC (scripting parity)

Occurrences are commands like everything else, so the LLM builds assemblies the
same way. Copy the existing templates:

- `add_occurrence {assembly, part, transform?}` — mirrors `add_feature`
  (`RemoteServer.cpp:334-351`).
- `set_occurrence_transform {id, transform}` — mirrors the snapshot-based
  `modify_feature` (`RemoteServer.cpp:352-363`); live-drag mutates, release
  pushes one `ModifyOccurrenceCommand`.
- `remove_occurrence {id}`, `set_occurrence_fixed {id, bool}`,
  `set_occurrence_visible {id, bool}`, plus `add_assembly {name}` alongside the
  existing `add_part`.

Undo/redo reuses the `CommandStack` (snapshot before/after, like
`ModifyFeatureCommand`).

## Decisions — SIGNED OFF (2026-07-13)

All six settled with Elias; the format above reflects them.

1. **Assembly is a distinct document kind** (a `kind:"assembly"` directory of
   occurrences, *not* a `Feature` list) — `RegenContext` can't reach other
   parts, so this is forced.
2. **`project.json` generalizes `parts` → typed `documents`** with one `active`
   index across all tabs; v1 files load via defaulted reads.
3. **Transform = translation + quaternion** (`position` + `orientation` xyzw).
4. **File-per-occurrence** (git-conflict-free, consistent with F3).
5. **Parts get a stable id; occurrences reference `part_id`, not name.** Prompted
   by Elias: by-name would break an assembly on any part rename that didn't run
   propagation (git merge, manual edit, file move). Referencing by stable id
   makes renames free and matches how features already reference each other
   (id = identity, name = cosmetic). Cost is a small `id` field on each
   `project.json` document entry, stamped on load for old files. Variables stay
   name-keyed (they're expression symbols); parts are structural refs, so they
   follow the feature/id model.
6. **A3-proofing confirmed:** occurrences always store an explicit transform;
   A3 mates re-solve over the non-`fixed` ones on load.

## Phasing

1. **Format + model** — `AssemblyDocument` kind, occurrence schema, project
   `documents` list, `TrsfToJson`/`FromJson`, v23 bump + golden assembly +
   round-trip test. Headless/RPC-testable with no rendering.
2. **Rendering + picking** — assembly `SyncDocumentRenderers` path
   (`CreateAsset`/`AddInstance`), FaceSelector/OutlineRenderer instance-space
   split, hit→occurrence, double-click-to-enter.
3. **Gizmo + commands** — move/rotate manipulator, the `*_occurrence` RPCs,
   snapshot undo. The interactive UX layer.

Phase 1 is the format spine and is the piece that most needs sign-off before
code; 2 and 3 are additive on top of it.
