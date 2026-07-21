# 3D curves — plan (fit-spline, helix, control-poly) + the features they unlock

**Status (2026-07-21):** Phases 0–2 **SHIPPED + tested**; Phase 3 **guided
sweep SHIPPED**, **guided loft = the remaining spike** (go/no-go, see below).
Supersedes [interchange/feature-gaps-plan.md](../interchange/feature-gaps-plan.md)
§3 ("3D fit-spline"), widening it from an import driver into a modeling
workstream. Scope signed off 2026-07-21: **build through Phase 3** (guided
sweeps/lofts), curve kinds **fit-spline + helix + 3D control-poly**.

### Progress
- **P0 ✅ (format v42)** — `CurveFeature` ("Curve3D"): FitSpline
  (`GeomAPI_Interpolate`), Helix (line on `Geom_CylindricalSurface`),
  ControlPoly (clamped B-spline). Non-body, caches edge + polyline; renders via
  `AppendCurve3DLines`; feature-tree icon; MCP `add_feature`; golden
  `v42-curve3d`. Tests: `tests/test_curve3d.cpp`.
- **P1 ✅** — Sweep path generalized to sketch **or** Curve3D
  (`ResolveSweepPathWire`); companion fix: `BuildOpenPathWire` now accepts
  sketch B-spline edges. Tests: helix-coil + spline-tube in `test_sweep.cpp`.
- **P2 ✅ (format v43)** — `PipeFeature` (solid/hollow circle swept along a
  path, reuses `SweepProfileAlongSpine`); Sweep `frame` option
  (corrected-Frenet default / Frenet / fixed). Golden `v43-pipe`. Tests:
  `tests/test_pipe.cpp`.
- **P3 partial ✅ (format v44)** — guided sweep: `SweepFeature.guideId` → an
  auxiliary spine (`MakePipeShell.SetMode(guide, …, BRepFill_NoContact)`)
  orients the section along a second curve. Golden `v44-sweep-guide`. Tests in
  `test_sweep.cpp`. **Graded follow-up:** `BRepFill_Contact` guide (section
  *scales* to touch the guide) — deferred; NoContact orientation guide is the
  robust v1.
- **P3 remaining — guided LOFT (SPIKE, not yet built):** OCC `ThruSections`
  has no guide support. Needs surface fitting
  (`GeomAPI_PointsToBSplineSurface` / `GeomFill`) or a multi-section
  `MakePipeShell`. Prototype first, confirm a real model needs it, then commit
  — the one genuine unknown in the workstream.

## Why

Every curve in Designer today lives inside a `SketchFeature` on a plane.
There is no free 3D curve object. That single gap blocks a whole family of
3D-curve-driven modeling:

- **Sweep along a real 3D path** — cables, tubing, mouldings that leave a plane.
- **Helix** — springs, and real *swept* threads (today `ThreadFeature` is
  cosmetic).
- **Pipe** — a hollow sweep of a circle along a path.
- **Guide curves ("two directions")** — a section shaped by a second curve as
  it travels: swept and lofted surfaces that aren't just a straight skin.
- **Import** — Onshape `fitSpline`'s non-planar case (the original driver;
  the importer currently reports it as a skip).

The keystone is one new geometry-object *category*; everything else is wiring
it into sweep/loft, which already exist.

## What already exists (reuse, don't rebuild)

- **2D B-splines** (format v40): `sketch::BSpline` (poles + degree), OCC
  `Geom_BSplineCurve` construction (`MakeBSplineCurve`), UV tessellation
  (`BSplineSampleUV`), and rendering. The 3D curve reuses this geometry
  machinery — only the container and the ambient dimension change.
- **Datum pattern for a non-body feature** — `DatumAxisFeature` caches its
  result as members (`Origin()`/`Direction()`), contributes no body, fails
  `Execute` on a degenerate definition (stopping regen), and renders through
  an overlay seam. `CurveFeature` follows this pattern verbatim.
- **Sweep** — `SweepFeature{profileSketchId, pathSketchId}` →
  `BRepOffsetAPI_MakePipeShell` with `RoundCorner` transitions. The spine is
  built by `sketch::BuildOpenPathWire`, which **walks only lines/arcs and
  silently ignores spline edges** (feature-gaps-plan.md:149).
- **Loft** — `LoftFeature{sketchIds}` → `BRepOffsetAPI_ThruSections`; sections
  only, no guides. (An in-flight smooth/ruled skinning fix lives in
  `LoftFeature.cpp` — this plan does not touch that logic.)
- **Render seam** — `UI.cpp:~425`: `AppendDatumAxisLines(state, overlay)`
  emits `LineVertex3D` segments for datum axes. `AppendCurve3DLines` sits
  right beside it.
- **Format version** — currently `kVersion = 41`
  (`SerializationInternal.h`). This workstream bumps it (see each phase).

---

## Phase 0 — `CurveFeature`: the new geometry-object category

The one architectural addition. Do this first; it gates everything below.

### The object
- **`class CurveFeature : public Feature`**, `TypeName() == "Curve3D"`, in the
  regen list — parametric, participates in rollback, variable-driven fit
  points. Contributes **no body**.
- **Cached transient result** (rebuilt every `Execute`, never serialized),
  exposed like a datum:
  - `const TopoDS_Edge& Edge() const;` (or a `TopoDS_Wire` for composites)
  - `Handle(Geom_Curve) Curve() const;`
- **`Execute`** builds the curve for the active kind, validates it (non-null,
  finite, >2 points for a fit), and `SetError` + `return false` on a
  degenerate definition — same contract as datums.

### Curve kinds (tagged variant, extensible)
Store kind as an enum + a small params struct so new kinds don't reshape the
class:

1. **`FitSpline3D`** — `GeomAPI_Interpolate(points, periodic, tol)` with
   optional start/end tangents → `Geom_BSplineCurve`. Points are 3D world
   coordinates (or resolved from referenced vertices — see UX). **The import
   driver.**
2. **`Helix`** — procedural. Params: axis frame (origin + direction, or a
   `DatumAxisRef`), radius, pitch, turns *or* height (one drives the other),
   handedness (RH/LH), optional linear taper (cone helix). Built the canonical
   OCC way — a straight `Geom_Line` in the parameter space of a
   `Geom_CylindricalSurface`, `BRepBuilderAPI_MakeEdge(line2d, cyl, u0, u1)` —
   then optionally `Approximate`d to a `Geom_BSplineCurve` for a uniform
   downstream representation. Taper uses a `Geom_ConicalSurface`.
3. **`ControlPolyBSpline3D`** — poles in space + degree, direct
   `Geom_BSplineCurve` (clamped-uniform knots, mirroring the 2D
   `MakeBSplineCurve`). Sibling to the 2D control-poly spline.

### Render
`AppendCurve3DLines(state, overlay)` beside `AppendDatumAxisLines`
(`UI.cpp`): tessellate `Edge()` (OCC `BRepMesh` / `GCPnts_TangentialDeflection`
along the curve) into `LineVertex3D`. No mesh, no new pipeline. Curve has its
own visibility toggle (datum convention).

### Serialize
New `"Curve3D"` branch in `SerializeFeatures.cpp` (write + read), kind string
+ params. **Bump `kVersion` → v42**, add a golden file. Fit points and
control poles are the serialized state; the OCC curve is always rebuilt.

### Pick / UI
- Feature-tree entry (icon), selectable, error surfaced like any feature.
- Pickable as a downstream reference (sweep path, loft guide).
- **Tool UX** (the fiddly part): picking non-coplanar 3D points. v1 tool can
  take points from existing vertices / sketch points / typed coordinates;
  free in-space picking is a later polish. Helix tool is a param dialog
  (axis + radius + pitch + turns), no point-picking needed.
- **Headless/MCP works immediately** via `add_feature` regardless of the GUI
  tool maturity.

### RPC / MCP / tests
- `add_feature` round-trips the `Curve3D` JSON; `measure` / `fit_view`
  account for it.
- Tests: fit-through-points reproduces the input points (on-curve within tol);
  helix has expected arc length & turn count; control-poly matches the 2D
  builder in a plane; save/load; variable-driven fit points regen.

---

## Phase 1 — 3D curve as a **sweep path**

Biggest immediate payoff; unblocks helix-swept springs/threads and
spline-swept tubing.

- **Generalize the sweep path input.** Replace `pathSketchId` with a
  `PathRef` that resolves to a `TopoDS_Wire` from **either**:
  - a sketch (existing `BuildOpenPathWire`), or
  - a `CurveFeature` (wrap `Edge()` in a single-edge wire).
  Keep back-compat: old files with `pathSketchId` deserialize into a
  sketch-kind `PathRef`.
- **Companion fix:** teach `BuildOpenPathWire` to include **sketch `BSpline`
  edges** in the walked chain (closes the `:149` gap) — a 2D spline sketch
  becomes a legal path too, using the same `MakeBSplineCurve` edge already
  built for profiles.
- Tests: circle profile swept along a helix → plausible spring volume;
  profile along a 3D fit-spline; a 2D-spline path sweep.

---

## Phase 2 — **Pipe** + sweep orientation controls

- **`PipeFeature`** — thin, discoverable: outer Ø, optional wall thickness
  (annular profile), path = a `CurveFeature`/sketch path. Internally builds the
  ring profile and calls the Phase-1 sweep core. Dedicated feature for the
  toolbar; shares the engine, no duplicate OCC code.
- **Profile-orientation mode** on sweep — `MakePipeShell.SetMode`: keep
  (fixed trihedron), Frenet, corrected-Frenet. **Required** so a helix sweep
  doesn't self-twist and a spline sweep stays natural. Default corrected-Frenet;
  expose as a sweep param.
- Bump `kVersion` for `PipeFeature` + the sweep mode field; golden.

---

## Phase 3 — **Guide curves / "two directions"**

The advanced tail. Sequence last; ship Phases 0–2 first, then take this on.

- **Guided sweep** — `BRepOffsetAPI_MakePipeShell` with an auxiliary
  spine / guide wire (`SetMode(guideWire, keepContact)`), so the profile
  scales and rotates to follow a second curve as it travels. Inputs: profile
  sketch + spine path + one or more guide `CurveFeature`s.
- **Guided loft** — the risky one: OCC `ThruSections` has **no** guide
  support. Genuine guide-lofting needs either surface fitting
  (`GeomAPI_PointsToBSplineSurface` / `GeomFill_BSplineCurves` across
  sections+guides) or a multi-section `MakePipeShell`. Prototype-gate this:
  build the spike, confirm a real model needs it, then commit. Fall back to
  ruled/smooth (existing) when guides aren't supplied.
- Bump `kVersion`; goldens for guided sweep and (if built) guided loft.

**Phase-3 note:** guided *sweep* is well-trodden OCC and low-risk; guided
*loft* is the one genuine unknown in the whole workstream. Treat it as a
spike with an explicit go/no-go, not a foregone build.

---

## Risk

Medium-large but **contained** — matches the original feature-gaps §3
assessment. No touch to body-identity, sketch-variant, or the constraint
solver. Cost is concentrated in: the new object category (Phase 0), its
render/pick path, and accepting it as a sweep/loft input (Phases 1–3). The
only architectural unknown is guided loft (Phase 3), which is spike-gated.

## Sequencing & ship points

1. **Phase 0** — `CurveFeature` (fit-spline + helix + control-poly), rendered,
   serialized (v42), MCP. *Ship: 3D curves exist and round-trip.*
2. **Phase 1** — 3D curve + 2D spline as sweep paths. *Ship: springs, threads,
   tubing.* Update the importer to replay non-planar `fitSpline` (drop the
   skip); drop the line from `import-plan.md` §"Future modeling features".
3. **Phase 2** — Pipe + orientation modes. *Ship: pipes, clean helix sweeps.*
4. **Phase 3** — guided sweep, then spike-gated guided loft. *Ship: "two
   directions" surfaces.*

After each phase ships, update the importer where relevant and this doc's
status line.
