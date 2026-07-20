# Import workstream — plan

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
- **Splines** — sketch B-splines (fit / control-polygon) and 3D fit
  splines. Onshape: spline sketch entities + `fitSpline`. Designer's sketch
  entity set is Point/Line/Circle/Arc/Ellipse only; a spline is a new
  entity type (solver params, OCC `Geom_BSplineCurve`, rendering) — the
  largest of the three. 3D fit-spline additionally needs a non-sketch
  curve object.

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
