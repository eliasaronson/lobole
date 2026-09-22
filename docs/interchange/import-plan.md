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
  origin for determinism (v97: `keep_origin` keeps the file's own
  coordinates instead, and `add_feature` reports the shift it applied as
  `recentred_by` — an import whose coordinates MEAN something, a neighbour
  part or a re-import of this app's own export, needs it); caps at 20
  shapes; one body per shape in ONE part. (v103: `units` says what length
  unit an STL or BREP was drawn in, since neither format carries one — a
  STEP refuses it; docs/mesh-units-plan.md.)
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
- Same determinism rule (bbox centering, unless `keep_origin`), same
  free-solids-only behavior,
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
  no binary fixtures. (2026-09-22: the tests were built on
  `SaveSTEPStructured`, which no export path called; since X4,
  docs/interchange/step-export-plan.md, `export_step` itself writes named
  products and placed instances.)

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
  switch, 4.2 s each), so it is an LRU against a device-memory budget
  (`kPartRenderCacheBudgetBytes`, 256 MB), never evicting what the current
  sync draws.
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

### Follow-ups

**1 and 2 are DONE** (2026-08-30). The ray-traced still now reads the part
render cache instead of re-meshing per OCCURRENCE ("314 reused, 0 tessellated
here"), and a cold import tessellates across threads. First view of the
speaker import: **6.7 s -> 3.1 s**, of which the tessellation phase went
5375 ms -> ~1500 ms; a Render went from ~4 s of duplicate meshing to ~0.
The `Assembly synced:` log line carries the breakdown, parallel phase included.

**On the safety of concurrent meshing** — established by measurement, since
OCC's own documentation is not specific enough to rely on:

- Meshing 299 distinct products serially and across 8 threads produced
  IDENTICAL output (vertex counts, triangle counts, summed positions and
  normals). 3.2x at 8 threads; only 2.3x at 16, because
  `BRepMesh_IncrementalMesh` is already internally parallel and
  oversubscribing loses.
- The only shared mutable state a mesher touches is the triangulation it
  writes into each TFace, so the hazard is two threads meshing shapes that
  SHARE a TShape. A scan of the real import found **zero** TFaces shared
  between parts, on both the reader's products and the .brep sidecars the app
  actually loads. (A first version of that scan reported 50 shared faces — it
  was comparing pointers to shapes it had already destroyed. Worth the
  warning: a sharing check must keep every shape alive.)
- Zero sharing today is a property of today's features, not an invariant
  anything enforces: a future "duplicate part" would copy a `CADShape`, which
  shares its TShape. So the work is **grouped by TShape** and a group always
  runs on one thread — the race is impossible rather than merely absent.

3. **DONE.** A single-product STEP used to be read twice — once by the import
   worker to decide part-vs-assembly, then again by `ImportFeature::Execute`.
   The worker's parsed model now travels with the feature
   (`ImportFeature::SeedGeometry`) and is consumed by the first Execute.
   Deliberately not serialized and not sticky: a later re-execution re-reads
   the path exactly as before, so editing the source file still updates the
   part. The seed removes a duplicate parse; it does not change what an import
   means.
4. **The first frame after an import is still an uncovered stall.** The
   progress modal covers the parse, the extraction and the tab creation, then
   closes — and the cold assembly sync that follows is 2.6 s with nothing on
   screen explaining it. The original plan for this round said the same pump
   would pre-warm the cache a few parts per frame so the modal spanned it;
   that was not built, because parallel tessellation took the same stall from
   9.7 s to 3.1 s and the remaining case is far weaker. It is the honest gap
   in "the import is behind a modal".
5. **A memory suballocator — smaller than it looks.** A cold sync now reads
   `2556 ms [build 438, picking 28, parallel tessellation 1496]`, so the
   tessellation is STILL the largest single item and the whole GPU asset path
   (create + upload, batched) is 438 ms. About 590 ms of that total is not
   attributed to any phase yet — teardown, the tree walk and the eviction pass
   are the candidates, and nobody has measured which. Attribute that before
   spending anything on allocation strategy: batching the submits already took
   1.15 s off, and what is left is not obviously worth the lifetime complexity
   in `DestroyAssetBuffers`.
6. **Renderer id slots are never reused — and should stay that way.**
   `CreateAsset` and `AddInstance` both return `size()` after a `push_back`,
   so a released slot is left null forever and the vectors grow for the life
   of the process. Instances churn far more than assets (314 per sync on the
   speaker import), which made this look like the obvious next cleanup. It is
   not, on both counts:

   - **The cost is nil.** A null slot is one 8-byte `unique_ptr`. A thousand
     assembly syncs on a 314-instance import is 314k slots — 2.5 MB, for a
     session far longer than anyone has.
   - **The fix is a hazard.** Reusing an id turns a stale one from a harmless
     no-op (it finds null) into a silent write to a DIFFERENT object.
     `AppState::simulation.hiddenMeshes` holds instance ids across frames, and
     it is not the only such holder — so reuse would risk hiding the wrong
     geometry, which is precisely the class of correct-looking wrong answer
     this codebase keeps paying for.

   Doing it safely means generation-tagging ids so a stale one is *detected*,
   which is a real robustness win but a much larger change than "reuse the
   slot". Worth it only if something else makes it worth it — not for 2.5 MB.
