# Import workstream — plan

> Using their API has terms and an annual call budget attached:
> [onshape-terms.md](onshape-terms.md).

Status: SIGNED OFF 2026-07-19 — I1+I2 approved to build now; I3 follows
after a real-world STEP checkpoint.
Scope: moving existing work INTO Designer. Three milestones: structured STEP
for parts (I1), assembly STEP (I2), and a parametric Onshape importer (I3).
Companion to docs/interchange/plan.md (export, v1 complete).

## Why

Interchange-in is the biggest gap between Designer and "I can use it for my
real projects." Today's STEP import produces anonymous, colorless dumb solids
in a single part — an assembly STEP collapses into a pile of shapes. And
there is no parametric path at all: everything imported is frozen geometry.

The SolidWorks reality check (from the next-step discussion): native
.SLDPRT/.SLDASM import is off the table — Parasolid geometry plus an
undocumented feature tree, readable only through commercial translator
licenses (HOOPS/Datakit/CADEx), and even those recover geometry, not
features. The supported answer is the one FreeCAD and Fusion give: **export
STEP AP242 from SolidWorks** — which makes I1/I2 the de-facto SolidWorks
import. Onshape is the opposite: a public REST API that hands over the real
feature list, so a genuinely *parametric* import is feasible (I3).

## Current state

- `ImportFeature` (`src/core/Feature.h:155`) wraps
  `CADKernel::LoadSTEP` = plain `STEPControl_Reader`: shapes only — no
  names, colors, or product structure. Centers the combined bbox at the
  origin for determinism; caps at 20 shapes; one body per shape in ONE part.
- Entry points exist end-to-end: File > Import STEP (Ctrl+I), `import_step`
  RPC + MCP tool. Whatever we improve lands in all three for free.
- The XCAF bridge patterns already exist on the EXPORT side
  (`src/MeshExport.cpp`: `XCAFDoc_ShapeTool`/`ColorTool` labels) — I1/I2
  read through the same API (`STEPCAFControl_Reader`). OCC 7.9.3 has it.
- `Body` already carries `name` (and `material`); assemblies reference parts
  by stable slot id; `AddPart`/`AddAssembly`/`AddOccurrence` +
  position/quaternion occurrences are all in place from A2.
- The project format is a directory (F3) — a natural home for extracted
  per-component geometry files.
- The RPC/MCP semantic surface accepts the full feature grammar
  (`add_feature`, `add_constraint`, expressions, mates) — an external
  translator can build a parametric model without new C++.

## I1 — Structured STEP import for parts (~0.5 session)

Replace the reader in `LoadSTEP` with `STEPCAFControl_Reader` (XCAF) and
surface what STEP actually carries:

- **Names**: XCAF label names → `Body::name` (falls back to "Import N" as
  today). The tree's bodies rows finally show "Housing", not "Body 3".
- **Colors**: XCAF color labels → a new optional per-body display color,
  stamped by the import feature (alongside the existing material slug;
  a color is NOT a simulation material — FEM keeps requiring an explicit
  material assignment). Renderer sync prefers: material > display color >
  palette.
- Same determinism rule (bbox centering), same free-solids-only behavior,
  same 20-shape cap (raised only if a real file needs it — log when capped).
- Serialization: display color rides in the Import feature's JSON (format
  bump only if Body serialization itself must change — expected: no bump,
  colors re-derive from the file at regen like everything else about
  ImportFeature).
- Tests: a small STEP fixture with named, colored solids (we can generate it
  with our own X1 exporter + XCAF names) → assert body names + colors after
  import; existing import tests keep passing (reader swap is behavior-adding,
  not behavior-changing).

## I2 — Assembly STEP import (~1 session)

The payoff milestone: an assembly STEP becomes a Designer **assembly**, not
a pile of shapes.

- Read the XCAF product structure: unique products (referred labels) →
  **one part document each**; instance labels with locations →
  **occurrences with transforms**. 100 identical screws = 1 part doc +
  100 occurrences, exactly the A2 model.
- **Geometry storage per part**: extract each unique component's shape and
  write it as a `.brep` file under the project directory (e.g.
  `imported/<product>.brep`), referenced by that part's ImportFeature. This
  keeps every part regenerable on its own, works with F5 git versioning,
  and avoids the fragile "index into a shared source STEP" alternative.
  (ImportFeature learns to load `.brep` by extension — `BRepTools::Read`,
  trivial.)
- **Transforms**: XCAF locations → occurrence position + quaternion.
  Rigid-only expected from real CAD; a sheared/scaled location imports at
  identity with a warning rather than corrupting the A2 model.
- Names → part slot names + occurrence names; colors per I1.
- **Flow**: Import STEP on an assembly tab (or a "file has N components —
  import as assembly?" prompt from a part tab) builds parts + assembly in
  one undo-less action (like File > Open; snapshot the project first).
  Single-component files keep today's into-the-active-part behavior.
- RPC/MCP: `import_step` gains `{as_assembly?: bool}` and returns the
  created part/occurrence ids.
- Tests: **round-trip against our own exporter** — build an assembly
  (2 parts, 3 occurrences, rotations), export STEP (X2 XCAF writer), import
  into a fresh project: assert part count, occurrence transforms (to
  tolerance), volumes, names. This tests both directions at once and needs
  no binary fixtures.

## I3 — Onshape parametric import (2–3 sessions, own mini-workstream)

A real feature-level import: the model arrives *editable*.

**Architecture decision: external translator first, not C++.** A Python tool
(`tools/onshape_import.py`) reads the Onshape REST API and replays the
document into Designer through the existing RPC/MCP semantic surface
(`add_feature`, `add_constraint`, `add_mate`, variables). Rationale: zero
new C++ for v1, the mapping is inspectable/patchable per-failure, partial
imports are visible live in the GUI, and the scripting surface is exactly
the "LLM models like a human" interface the project already bets on. An
in-app "File > Import from Onshape" wrapper can come later if the tool
proves itself.

- **O1 — client + fetch**: API-key auth (env vars), resolve a document URL →
  `GET /partstudios/.../features` (feature list JSON: BTMSketch entities +
  constraints, feature parameters), `/assemblies/.../definition`
  (occurrences + mates), variable tables, mass properties (for
  verification). Units: Onshape wire format is meters — convert to mm at
  the boundary, once.
- **O2 — the mapping layer** (the real work):
  - Sketches: line/circle/arc/point/ellipse map 1:1; constraint vocabulary
    is near-identical (coincident/tangent/equal/distance/angle/...);
    splines: import as construction polyline + warning (v1).
  - Features: extrude (incl. up-to-face/symmetric/thin), revolve, fillet,
    chamfer, shell, hole, linear/circular/mirror patterns, datum planes,
    boolean, loft, sweep — all have direct Designer equivalents. Onshape
    variables → document variables, expressions passed through where the
    grammar overlaps (else evaluated to literals + comment).
  - Assemblies: occurrences map to A2; Onshape mate types map cleanly onto
    A3 (fastened/revolute/slider/cylindrical/planar/ball — same six).
  - **Fallback rule**: any unmappable feature (FeatureScript custom
    features, surfacing) → import that feature's *resulting* bodies as
    dumb geometry (Onshape part-export endpoint) + mark it clearly in the
    report; downstream mappable features continue on top when Onshape's
    rollback-state export allows, else the remainder of the tree imports
    dumb. Never silently drop.
- **O3 — verification + report**: per-feature table (mapped / fallback /
  skipped + why) printed at the end; automatic mass-properties comparison
  (Onshape API vs our `mass_properties`) as the correctness metric.
  Tests: the mapping layer gets unit tests against recorded API fixtures
  (JSON checked into tests/), so CI needs no network or account.

Risks, stated up front: real-world docs lean on FeatureScript custom
features (fallback path is load-bearing, not an edge case); in-context /
multi-part studios with booleans across parts need care; configurations are
out of scope v1 (import the active one).

## Future modeling features (surfaced by the Onshape importer)

These are genuine Designer feature gaps — not importer work — that real
Onshape documents lean on. Until built, the importer reports each as a
skip (the rest of the model still comes across). Each is its own small
workstream — **full build plan in [feature-gaps-plan.md](feature-gaps-plan.md)**:

- **Split feature** — ✅ **v1 SHIPPED (format v39)**: `SplitFeature`
  (OCC `BRepAlgoAPI_Splitter` + `SplitSolids`), knife = a base/datum plane or
  a planar face of the body, keeping all pieces. Sketch-as-knife and the
  robust downstream body-identity tier are deferred — see
  [feature-gaps-plan.md](feature-gaps-plan.md). The importer's `splitPart`
  skip can now replay a Split.
- **Splines** — ✅ **BOTH SHIPPED.** Sketch B-splines landed in sketching
  phase L (format v40, rigid-net v1): `BSpline` is a member of the sketch
  entity variant alongside Point/Line/Circle/Arc/Ellipse/EllipseArc. The 3D
  side landed as its own workstream — `CurveFeature` / `Curve3D` (format
  v42, fit-spline + helix + control-poly), which grew out of this gap and is
  planned in [../modeling/curves-3d-plan.md](../modeling/curves-3d-plan.md).
  Fully-solved poles (point-on-spline / tangent-at-knot) remain deferred.
  **The importer caught up 2026-08-25:** `map_fit_spline` + `import_fit_spline`
  replay a `Curve3D`, re-fitted through a dense sample of the curve Onshape
  built rather than through its through-point *query* — for the same reason
  `up_to_verts`, `consumed_edges` and `face_diffs` all measure geometry:
  Onshape queries do not resolve at rollback.

(**up-to-face extrude bounds are already supported** — `ExtrudeEnd::UpToFace`
— and are mapped by the importer; listed here only to note they are NOT a
gap.)

## Out of scope (documented, deliberate)

- **Native SolidWorks files** — see Why. The user-facing answer, in docs and
  the import dialog's format filter: "From SolidWorks: File > Save As >
  STEP AP242, then import that."
- IGES / DXF / mesh(STL-as-geometry) import — stays demand-driven (X3 in
  the export plan).
- Fusion 360 — no server-side API for feature extraction (desktop add-in
  only); revisit on demand.
- STEP PMI/GD&T annotations, drawings interchange.

## Order & effort

I1 → I2 back-to-back (~1.5 sessions; I2 is the payoff, I1 is its plumbing
made independently shippable). I3 after, gated on its own O1→O3 order —
O1+O2 core mapping first (sketch/extrude/revolve/fillet covers most real
parts), breadth grows by fixture. Suggested checkpoint after I2: import a
real SolidWorks-exported STEP assembly and file whatever breaks before
starting I3.

## What the first real file broke (2026-08-30)

The I2 checkpoint above — "import a real SolidWorks-exported STEP assembly and
file whatever breaks" — run against a 48 MB, 299-product, 314-occurrence
speaker model. Nothing was wrong with the *result*; everything was wrong with
what it cost. Measured against an `--offscreen` instance and a harness linking
`libdesigner_core.a`:

| | before | after |
|---|---|---|
| import (File > Import, frozen window) | ~38 s | ~14 s, off the UI thread behind a modal |
| switching back to the assembly tab | 5.5–5.9 s, every time | 30 ms |
| hiding one occurrence | 5.9 s | 39 ms |

Four causes, none of them the one that looked obvious (the picker — 20 ms for
all 314 instances):

- **The file was parsed twice.** `ImportCADFile` read the whole STEP to decide
  part-vs-assembly, dropped the model, and `BuildAssemblyImportPlan` read it
  again. The comment justifying that — "STEP reads are cheap next to the
  extraction that follows" — is backwards at this size: extraction is 0.9 s,
  the read 12 s. The plan builder now takes an already-parsed model.
- **Nothing was cached between resyncs.** `SyncAssemblyRenderers` built its
  per-part map as a local, and both renderers destroy an asset when its last
  instance goes — so the teardown at the head of every resync threw away every
  tessellation and every upload. `AppState::partRenderCache` keeps them, keyed
  on (part slot id, part document version), and `RetainAsset`/`ReleaseAsset`
  hold them across the teardown. **The eviction rule matters as much as the
  cache**: a strict "release anything not drawn this sync" made two assembly
  tabs thrash (alternating two 299-part assemblies re-tessellated all 299 every
  switch, 4.2 s each), so it is an LRU against a device-memory budget,
  never evicting what the current sync draws.
- **Every buffer upload drained the queue.** `vkutil::CopyBuffer` ends in a
  `vkQueueWaitIdle`; a cold build of this file creates 1196 buffers.
  `vkutil::StagingBatch` puts them behind one submit (−1.1 s cold).
- **Progress needs care.** `STEPCAFControl_Reader::Transfer` reports through a
  `Message_ProgressIndicator`, but it fires ~200k times on this file — calling
  `GetPosition()` in every callback took the transfer from 8.3 s to 20.1 s.
  Sample it, do not read it per tick.

Hiding occurrences was never the answer, which is worth stating because it is
the obvious workaround: the work is per UNIQUE PART, and a part is tessellated
if even one of its occurrences is visible. 274 visible vs 314 visible was
5.5 s vs 5.9 s.

### Follow-ups, in the order they are worth doing

1. **The ray-traced still re-tessellates per OCCURRENCE.**
   `SyncRayTracingScene` (AppActions.cpp) builds its item list from
   `FlattenActiveAssembly` and calls `GenerateMesh(0.1)` on each — 314
   tessellations rather than 299, and all of them again on every Render, even
   though `partRenderCache` is holding exactly that data. Roughly 4 s per
   Render on this file, and now a small change.
2. **Tessellate a cold import in parallel.** The remaining ~4 s of the ~6.7 s
   first view is OCC meshing 299 parts one after another.
   `BRepMesh_IncrementalMesh` already parallelises WITHIN a shape; nothing
   parallelises across them. The parts are independent, so this is the largest
   remaining win — gated on confirming OCC's thread-safety for concurrent
   meshing of distinct shapes.
3. **A single-product STEP is still read twice** — once by the import worker
   to decide part-vs-assembly, then again by `ImportFeature::Execute` on
   regen, since the feature stores a path. Unchanged from before this round
   (the assembly path is the one that got the fix), and it only bites on files
   that are one big solid.
4. **A memory suballocator.** Measured and deliberately skipped: batching the
   submits took 1.15 s off a cold build, and the ~1200 remaining
   `vkAllocateMemory` calls are worth far less than the tessellation above.
   Do (2) first.
5. **`MeshRenderer::CreateAsset` never reuses a released slot** — ids come
   from `m_assets.size()` after a `push_back`, so the vector grows for the
   life of the process. The cache makes this far rarer than it was; it is a
   null `unique_ptr` per hole, so it is a tidiness issue, not a leak.
