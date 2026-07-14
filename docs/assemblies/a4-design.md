# Assemblies A4 — assembly features & beyond — design

> Status: **signed off 2026-07-13** (all six decisions, as recommended).
> A1–A3 + A2.5 are complete: multi-part
> projects, instanced occurrences, all six mate types with the
> direct-frame-alignment solve, and interactive manipulation. A4 is the plan's
> grab-bag milestone — "in-context editing, assembly-level features,
> interference check, BOM, exploded views, configurations". This doc's first
> job is to **scope that honestly**: interference already shipped (inspect I4),
> two of the items are workstreams of their own that deserve separate designs,
> and one item nobody listed — part→assembly change propagation — is actually
> the foundation the rest stands on. What remains is a coherent, buildable A4.

## What's in and what's not

**A4 v1 (this doc):**

1. **Part→assembly change propagation** — an assembly notices when a
   referenced part's geometry changed and re-solves/re-syncs automatically.
   Today it doesn't (see below); every other A4 feature assumes it does.
2. **Bill of materials** — a derived part/quantity/mass table, panel + RPC +
   CSV export.
3. **In-context editing** — edit a part with the assembly ghosted around it,
   v1 visual-only (the ghost is a reference for the eye, not pickable).
4. **Exploded views** — named, persistent explode states authored with the
   A2.5 gizmo, animated by a factor slider. The one format bump (v27).

**Explicitly deferred to their own designs (decisions, not gaps):**

- **Assembly-level features** (a hole cut through multiple parts at assembly
  time). This breaks the two load-bearing A2 abstractions at once: instancing
  (an occurrence with an assembly cut no longer shares its part's mesh — it
  needs per-occurrence B-Rep + tessellation) and the document model (an
  assembly has no feature list or regeneration). It is a real milestone —
  SolidWorks has it, Onshape deliberately doesn't — but it is *its own*
  design with its own format implications, and none of A4 v1 precludes it.
- **Configurations** — named variants (suppressed occurrences, variable
  overrides, part configs). Wants three prerequisites that don't exist:
  occurrence suppression, an assembly variable table, and a story for how an
  occurrence pins a part config (interacts with the unused `partVersion`
  F5 pin). Own design.
- **Cross-part references in-context** (project another part's edge into the
  edited sketch — the full Onshape "edit in context"). Needs
  occurrence-qualified `ElementRef`s (`{occurrence, feature, element}`) with
  staleness semantics — a toponaming extension. In-context v1 is visual-only;
  the entry/exit/ghost machinery it builds is exactly what cross-part refs
  will sit on later.
- **Subassemblies** (an assembly occurrencing an assembly) — never listed for
  A4, noted here because it shapes the BOM: v1's BOM is flat, and a tree BOM
  is an additive change when nesting arrives.
- **Interference check** — already shipped (inspect I4). Nothing to do.

## 1. Part→assembly change propagation

**The gap (surveyed 2026-07-13):** the only resync gate is
`SyncDocumentRenderers` comparing the *active* document's version
(`AppActions.cpp:1428-1434`); a tab switch forces a full rebuild by setting
`syncedDocumentVersion = UINT64_MAX` (`AppActions.cpp:502`). So an assembly
re-*tessellates* changed parts on tab-activate by accident of the full
rebuild — but it never re-*solves* mates (the known "solve-on-tab-switch"
polish item), and nothing at all propagates while the assembly tab is
inactive or — fatally for in-context editing — while a part is edited with
the assembly on screen.

**The fix — per-part version stamps:**

- `SolveActiveAssembly` already visits every referenced part (the
  `PartResolver`); it now records `{partId → Document::Version()}` into
  AppState (`solvedPartVersions`, stashed per-slot on tab switch). Stamping
  at *solve* rather than render sync means propagation works headless too
  (headless has no renderer, so sync never runs there).
- A cheap staleness check (`AssemblyPartsStale(state)`: walk occurrences,
  compare two ints per part) runs:
  - on **assembly tab activation** — if stale, `SolveActiveAssembly` (solve →
    `BumpVersion` → existing resync). This *is* solve-on-tab-switch, now
    load-bearing rather than polish.
  - after **every part regeneration while in-context** (phase 3) — the edit
    loop that makes ghosts track live.
- Mate connector frames re-derive during the solve (they are never stored),
  so a moved hole moves the mated bolt with zero extra machinery — this is
  the A3 payoff loop finally firing *automatically* instead of via explicit
  `solve_assembly`.

No format change; `partVersion` (the F5 git pin) remains reserved and
untouched — these stamps are in-memory session state, not persistence.

## 2. Bill of materials

Purely **derived** data — computed from the document every time, never
stored (same rule as element names, format guarantee 3).

**Model:** group occurrences by `partId` →

```
BomRow { partId, partName, quantity,
         material,      // slug; "mixed" when a part's bodies differ
         unitMass, totalMass, unitVolume }   // mass nullable (no density)
```

- The mass math already exists in the `mass_properties` RPC
  (`RemoteServer.cpp:715-768`: per-body `ComputeMassProps` × material
  density, null-tainting when a body has no material). It is **refactored
  into a core helper** (`core::DocumentMassProps(const Document&)`) so the
  RPC, the BOM, and any future caller share one implementation.
- `visible=false` occurrences **count** (visibility is a display state, not
  suppression — exclusion arrives with configurations).
- Flat, one row per part — no nesting until subassemblies exist.

**Surface:** a BOM section in the Model dock (table: part / qty / material /
unit / total mass, with a mass-total footer and an Export CSV button), plus
`bom {}` (structured JSON, per-row + totals) and `export_bom {path}` RPCs,
plus MCP mirrors. LLM parity: "how many M6 bolts and what does it weigh" is
one `bom` call.

## 3. In-context editing (visual v1)

Edit a part while seeing the assembly around it — alignment by eye, sanity
checks, "does the bracket clear the motor". v1 draws the context; it does not
let you reference it.

### The frame convention: part at identity, ghosts inverse-transformed

Two options exist: render the active part at its occurrence's assembly
transform (the "true position" view), or keep the part at identity and bring
the assembly to it. **v1 keeps the part at identity** and places every ghost
at

```
G_j = OccMatrix(occ_edited)⁻¹ · OccMatrix(occ_j)
```

— the assembly rendered *relative to the edited occurrence*. Rationale: every
piece of part tooling (sketch plane derivation, face picking, datum display,
the feature preview flow, section planes) assumes the document sits at
identity; rendering the part transformed would touch all of it for zero v1
benefit. The relative view is geometrically identical — you see exactly the
same spatial relationships — and when cross-part references arrive, `G_j` is
precisely the matrix a projected edge must be pulled through anyway.

### Mode plumbing

- **Entry:** double-click an occurrence in the assembly viewport (the A2
  design reserved this), or "Edit in context" in the occurrence panel/context
  menu. Implementation: the existing `SetActivePart` tab switch to the part's
  slot, plus a new `AppState::inContext` — `{assemblySlotId, occurrenceId}` —
  that survives until exit.
- **While editing:** the part tab is fully itself — feature tree, sketcher,
  undo stack, everything unchanged. Additionally `SyncDocumentRenderers`
  (part branch) appends **ghost instances**: every *other* occurrence of the
  context assembly, tessellated via the same shared-asset path
  `SyncAssemblyRenderers` uses, transformed by `G_j`. Occurrences of the
  *edited part itself* other than the edited one also appear (ghosted) —
  editing one instance shows all its placements move together. After each
  part regeneration, the phase-1 staleness hook re-solves the context
  assembly so ghosts track mates live (shrink the base part → the ghosted
  mated bolt slides while you watch).
- **A banner** ("Editing *bracket* in context of *robot* — Return") renders
  over the viewport; Return (or clicking the assembly's bottom tab) exits.
- **Exit:** ordinary tab activation of the assembly + clearing `inContext`;
  phase-1 propagation re-solves and the full resync repaints. No special
  save/cancel semantics — part edits were real document edits with real undo,
  exactly as if made in the part's own tab.
- Ghosts are **not pickable** (never fed to `FaceSelector`), **cast no
  outlines**, and don't affect camera-fit (fit stays on the edited part;
  a "fit context" is a later nicety).

### Ghost rendering: the one renderer change

Surveyed: no transparency exists — blend is hard-disabled
(`MeshRenderer.cpp:485`), the shader writes alpha 1.0, but the per-instance
push-constant block *already carries* `baseColor` as a vec4 with an unused
alpha channel. So:

- `MeshInstance` gains a `ghost` flag; ghost instances set `baseColor.w`
  (≈0.25) and a desaturated tint.
- A **second pipeline** sharing the mesh shaders: `blendEnable = VK_TRUE`
  (standard alpha), **depth test on, depth write off**, drawn in a second
  pass after all opaque instances. The fragment shader multiplies
  `baseColor.w` into the output instead of the hard 1.0.
- Unsorted blending — ghost-on-ghost ordering artifacts are accepted for a
  0.25-alpha reference visual (documented; sorting is a rendering-plan
  problem if it ever matters). Ghosts respect the section clip plane like
  everything else (it's in the shared UBO).

This is deliberately also the first transparency in the renderer — glass
materials someday reuse the pipeline.

## 4. Exploded views

Named, persistent explode states. **Pure view state**: exploding never
touches occurrence transforms, mates, or the solve — it is applied at
render-sync time only, so a saved exploded view can't corrupt placements by
construction.

**Model** (on `AssemblyDocument`):

```
ExplodedView { id, name,
               steps: [ ExplodeStep { occurrenceId, offset /*vec3, world*/ } ] }
```

- Transient (AppState, not saved): `activeExplodedView` (0 = none),
  `explodeFactor` ∈ [0,1].
- Rendered instance transform = `Translate(factor · step.offset) · Solved(occ)`.
  Applied in `SyncAssemblyRenderers` + `PushOccurrenceLive`; picking follows
  rendering automatically (the `FaceSelector` gets the same matrix).
- One offset per occurrence per view (a later "step ordering / trails"
  refinement is additive).

**Authoring** — the A2.5 drag layer with a different write target: when an
exploded view is active, the occurrence gizmo (translate arrows only) writes
`step.offset` instead of the occurrence transform — grab a part, pull it out,
that *is* the explode step. Mated occurrences explode too (explode overrides
the visual pose; the solve is untouched underneath). An **Auto-explode**
button seeds offsets radially from the assembly centroid (distance scaled by
bounding-box size) as a starting point. Panel: an Exploded Views section
(list, add/rename/delete, factor slider, per-view activate).

**Format v27** (the only A4 bump), additive per the F3 discipline:

```
assemblies/robot/
  index.json                  # + "exploded": [ids]  (absent when none — v26 byte-identical)
  exploded/
    9-service-view.json       # {type:"ExplodedView", id, name, steps:[{occurrence, offset:[x,y,z]}]}
```

Ids come from the same never-reused `m_nextId` counter as occurrences and
mates. Golden + round-trip per the migration policy. Undo rides
`AssemblyHistory` snapshots for free (views live in `AssemblyDocument`).

**RPCs:** `add_exploded_view`, `set_explode_step {view, occurrence, offset}`,
`remove_exploded_view`, `list_exploded_views`, `set_explode {view?, factor}`
(the display state — LLM screenshot of an exploded assembly = two calls) +
MCP mirrors.

## Decisions for sign-off

1. **Scope: A4 v1 = propagation + BOM + in-context (visual) + exploded
   views.** Assembly-level features and configurations are deferred to their
   own designs (each has format/architecture implications A4 must not rush);
   interference is already shipped (I4). Nothing in v1 precludes either.
2. **Propagation via per-part version stamps** recorded at assembly solve,
   checked on assembly tab activation and after in-context part regens; stale
   → auto `SolveActiveAssembly`. Absorbs the solve-on-tab-switch polish item.
   In-memory only; the `partVersion` F5 pin stays reserved.
3. **In-context = part at identity, ghosts inverse-transformed**
   (`OccMatrix(edited)⁻¹ · OccMatrix(other)`), entered by occurrence
   double-click, exited by tab return; part tooling completely unchanged;
   **v1 is visual-only** — ghosts unpickable, cross-part references deferred
   (they will reuse this exact frame math).
4. **Ghost rendering = the renderer's first transparency**: second mesh
   pipeline (alpha blend, depth-test on / depth-write off, second pass),
   per-instance alpha through the already-present-but-unused `baseColor.w`
   push constant; unsorted (artifacts accepted at 0.25 alpha); no outlines,
   no picking.
5. **Exploded views are persistent named view state** — per-occurrence world
   offsets scaled by a transient factor, applied at render sync only (solve
   and stored transforms untouched by construction); authored by the A2.5
   gizmo writing `step.offset` + radial auto-explode seed. **Format v27**,
   file-per-view under `exploded/`, shared id counter, golden per bump.
6. **BOM is derived, never stored**: flat rows grouped by `partId`
   (qty / material / unit / total mass), mass via a new shared
   `core::DocumentMassProps` helper (refactored out of the `mass_properties`
   RPC), `visible=false` counts (no suppression yet); Model-dock table +
   `bom` / `export_bom` RPCs + MCP + CSV.

## Phasing

1. **Propagation** (small, foundational): part-version stamps +
   `AssemblyPartsStale` + solve-on-activate; unit test: edit part → activate
   assembly → mated occurrence moved *without* explicit `solve_assembly`.
2. **BOM**: `core::DocumentMassProps` refactor (mass_properties RPC keeps its
   contract), BOM computation + panel + RPCs + MCP + smoke test.
3. **In-context editing** (the big one): ghost pipeline in MeshRenderer
   (blend pass + `baseColor.w`), enter/exit plumbing (`AppState::inContext`,
   double-click, banner), ghost instance sync, live re-solve hook (uses
   phase 1). Verified visually via dev-watch.
4. **Exploded views**: model + v27 serialization + golden; sync/pick explode
   transform; gizmo write-target + auto-explode; panel + RPCs + MCP.

Order rationale: 1 unblocks 3's live-tracking payoff and is a real bug-class
fix on its own; 2 is independent and cheap (an early win in any order); 4 is
independent of 3 but reuses the gizmo write-target pattern last touched in
A2.5. After A4: assembly-level features and configurations each get their own
design docs (A5/A6), informed by what in-context editing teaches about
per-occurrence state.
