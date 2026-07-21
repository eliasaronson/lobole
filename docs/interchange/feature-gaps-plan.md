# Feature-gaps plan — Split, 2D B-splines, 3D fit-splines

Three genuine Designer modeling gaps surfaced by the Onshape importer
(`import-plan.md` §"Future modeling features"). Each is real parametric
feature work, not importer glue — the importer already reports each as a
clean skip, so the rest of a model still comes across. This doc is the
authoritative build plan for all three; the sketch-spline detail also lives
in [`../sketching/phase-l.md`](../sketching/phase-l.md) (Phase L, B-spline
half) and is not duplicated here beyond the delta.

They are independent and ship in separate commits. Recommended order and
effort:

| # | Feature | Onshape source | Effort | Dominant risk |
|---|---------|----------------|--------|---------------|
| 1 | Split feature | `splitPart` | ✅ **v1 SHIPPED** (robust tier deferred) | body-identity (`createdBy`) collision |
| 2 | 2D sketch B-spline | spline sketch entities | ✅ **v1 SHIPPED** (rigid net; solved poles deferred) | OCC watertight profile edge |
| 3 | 3D fit-spline | `fitSpline` | ~1 session | new non-sketch curve object type |

Grounding facts (verified 2026-07-20): format `kVersion = 38`
(`src/core/io/SerializationInternal.h:19`); sketch entities are
`std::variant<Point, Line, Circle, Arc, Ellipse>`
(`SketchFeature.h:67`); `PointRef::which` is `uint8_t`
(`SketchFeature.h:307`) — a 255-pole ceiling; `Body::createdBy` is one
producing-feature id per body (`Feature.h:25`); `CADShape::SplitSolids()`
exists (`CADKernel.h:59`); `BRepAlgoAPI_Splitter` is in the OCC 7.9.3
install. There is **no feature factory registry** — dispatch is a hand-
written if/else on `TypeName()` in `SerializeFeatures.cpp`, and every
`.cpp` is listed explicitly in `CMakeLists.txt`.

---

## 1. Split feature — ✅ v1 SHIPPED

Divide one solid body into N pieces with a plane / planar face / sketch as
the knife, keeping **all** pieces as separate bodies. Designer has Boolean
subtract (removes material) but nothing that keeps both sides.

**Shipped (format v39):** `SplitFeature` (`src/core/features/SplitFeature.{h,cpp}`),
knife = a base/datum plane offset along its normal, or one of the body's own
planar faces (extended to an unbounded plane); sketch-as-knife deferred.
`BRepAlgoAPI_Splitter` → `CADShape::SplitSolids()`; each piece becomes a body
named `"<feature> N"` with `"<id>/bN/…"` element names so its faces stay
selectable. Serialization + v39 golden (`tests/golden/v39-split.*`) +
`tests/test_split.cpp` (10 cases) + RPC exposure via the generic
`add_feature`. Verified live: a box halved by XY@z=1 → two vol-4 bodies,
volume conserved, correct centres of mass. The `createdBy` collision (below)
is accepted for v1 — the robust tier is deferred. **OCC gotcha recorded:**
OCC 7.9's `TopoDS_Shape` has a templated move-ctor that is SFINAE'd out for
const lvalues, so `TopTools_ListOfShape::Append(mf.Face())` and even a
`const TopoDS_Shape&` bind from `mf.Face()` fail to compile — copy through a
non-const `TopoDS_Face` local first (as `SketchProfile.cpp` does), then to
`TopoDS_Shape`.

### Geometry — mostly already there
Pipeline: build the knife tool-shape → `BRepAlgoAPI_Splitter` (argument =
target solid, tool = knife) → `CADShape::SplitSolids()` on the result →
append each connected solid as a `Body`. `SplitSolids()` already exists for
exactly this shape ("turn a fused pattern back into per-instance bodies").
The knife:
- **Datum plane** → an unbounded planar face (`BRepBuilderAPI_MakeFace` from
  a `gp_Pln`, oversized).
- **Planar model face** (`ElementRef`) → extend to an unbounded plane, same
  as above; matches how draft/shell resolve a picked face.
- **Sketch** → the sketch's profile wire/face as the cutter (later; ship
  plane + face first).

### Files to touch (standard new-solid-feature template)
Model on `BooleanFeature.{h,cpp}` (consumes bodies by `createdBy`) +
`ShellFeature` (face/plane picking).
- **New** `src/core/features/SplitFeature.{h,cpp}` — fields: target `bodyId`
  (`Body::createdBy`), knife kind (datum-id | `ElementRef` face | sketch id),
  optional "keep only side N" selector. `TypeName()` = `"Split"`,
  `Execute(RegenContext&)`.
- `CMakeLists.txt` — add the `.cpp` to the explicit source list (and the
  tests target).
- `src/core/io/SerializeFeatures.cpp` — a `dynamic_cast` arm in
  `FeatureToJson` and a `type == "Split"` arm in `FeatureFromJson`; include
  the header.
- `src/core/io/SerializationInternal.h` — bump `kVersion` 38 → 39 with a
  comment.
- `tests/golden/` — one frozen `v39-split-*.json` + `.expect.json` pair (the
  golden convention adds a pair only on a `kVersion` bump).
- **New** `tests/test_split.cpp` + register in `tests/CMakeLists.txt` — split
  a box by its midplane → two half-volume bodies; volumes sum to the original.
- **RPC/MCP: none.** `add_feature` (`src/remote/RemoteModeling.cpp`) is
  generic JSON round-trip, so `mcp__designer__add_feature` exposes Split for
  free once serialization knows it.
- **UI (for clickability, not scripting):** `src/ui/AppActions.cpp`
  (`CreateSplit` + preview, like `CreateThread`/`CreateRib`),
  `src/ui/FeatureEditor.{h,cpp}` (a `PickSlot` entry for the target body +
  knife, reusing the face/body pick machinery), toolbar/menu entry.

### The landmine — body identity
Split is the **first feature that turns one body into N kept bodies.** Every
downstream consumer finds bodies by `Body::createdBy == <feature id>` and
takes the **first** match (`BooleanFeature.cpp`, `ShellFeature.cpp`,
`DraftFeature.cpp`, `FilletFeature.cpp`, `ThreadFeature.cpp`,
`RibFeature.cpp`). If every Split piece is stamped with the same
`createdBy = GetId()`, the pieces are indistinguishable and a later
fillet/boolean/shell silently binds to whichever piece is found first.
Import already hit this exact wall and worked around it **only for face
names** (body-ordinal prefix `"<id>/b<N>"`, `Feature.cpp:161`) — the
`createdBy` field itself still collides there.

Two-tier scope:
- **v1 (~0.5 session):** ship Split as documented; accept that downstream
  parametric ops on the *pieces* are ambiguous. Fine for the importer's need
  (splitPart is usually terminal) and for "cut a part in two and export."
  Golden + volume test prove the geometry. Document the limitation.
- **Robust (~+0.5 session):** give each piece a stable sub-identity. Add a
  `uint32_t bodyOrdinal` (or a derived id `createdBy·k + n`) to `Body`; make
  the downstream `find_if(createdBy == id)` consumers accept an optional
  ordinal in their body reference. This is a small change to the body-
  identity model, not a localized feature add — do it when a real model needs
  to fillet a split piece, not speculatively. Note: Pattern sidesteps this by
  re-fusing into one body (`FuseInstancesReplacingSource`); Split cannot,
  since keeping both sides is the point.

**Recommendation:** ship v1, file the robust tier as a follow-up gated on
demand. Update the importer's `splitPart` handler to replay Split instead of
skipping.

---

## 2. 2D sketch B-spline entity — ✅ v1 SHIPPED

**Shipped (format v40):** `BSpline` is now a sketch `Entity` variant — a
non-rational, clamped, uniform B-spline through its control `poles` (degree
cubic, reduced to poles-1 when few; weights/knots are the clamped-uniform
defaults, not stored). A header de Boor evaluator (`BSplineEvalUV` /
`BSplineSampleUV`) drives tessellation/render/pick; the OCC profile edge
(`Geom_BSplineCurve` in `SketchProfile.cpp`) uses the identical knot vector
so the two agree (watertight). **Rigid-net solver:** each pole is a free
solver point addressed by `which`, so Coincident/Distance/Fixed on a pole
work and dragging a pole reshapes the curve (poles capped at 255 — `which`
is `uint8_t`). Profiles: a self-closed spline is its own loop; an open
spline chains with lines/arcs (endpoints = first/last pole; poles reversed
on chain-swap). A `SketchTool::Spline` (click poles, Enter/double-click to
finish, Esc cancels) + toolbar (`ICON_FA_PEN_NIB`, key `S`) + pole
dots/control-polygon preview. Drawing projection already samples B-spline
edges via its generic path — no change. Serialize + v40 golden
(`tests/golden/v40-spline.*`) + `tests/test_spline.cpp` (14 cases:
de Boor endpoints/hull, translate/reflect, JSON round-trip, pole-as-solver-
point, closed + spline-plus-line extrude volume-vs-area). Verified live over
headless RPC (spline sketch → extrude → 31.2 mm³ → save/reload identical).
**Deferred:** fully-solved poles (`GCS::BSpline` + point-on-spline/tangent-
at-knot constraints), expression-driven poles, >255-pole splines, and
splines in sweep *paths* (`BuildOpenPathWire` ignores them). The drawing
*tool* itself was not visually driven (assistant can't launch windowed
Vulkan); its click logic is thin glue over the unit-tested core.

Historical scope notes below (kept for the deferred follow-ups):

Add `BSpline` as a sketch `Entity` variant (current set is
Point/Line/Circle/Arc/Ellipse). This is the second half of sketching
**Phase L** — already reviewed in [`phase-l.md`](../sketching/phase-l.md);
that file has the model/solver/profile/tool breakdown. Deltas and the
grounding that matters:

- **Good news — PlaneGCS ships spline support out of the box.** The vendored
  FreeCAD solver has `class BSpline : public Curve` (`deps/planegcs/Geo.h`,
  poles/weights/knots/degree/periodic) plus `addConstraintPointOnBSpline`,
  `addConstraintTangentAtBSplineKnot`,
  `addConstraintInternalAlignmentBSplineControlPoint`. The **constraint
  solver is not the blocker.**
- **Wide-but-shallow variant fan-out.** Adding `BSpline` to the variant
  forces a small arm at every `std::visit`/`get_if` site: in
  `SketchFeature.h` (`ReflectEntity`, `TranslateEntity`, `RotateEntity`,
  `EntityPointUV`, the expr-field helpers), `SerializeFeatures.cpp`
  (`EntityToJson`/`EntityFromJson`, a `"spline"` case + v-bump — shares the
  v39 bump if landed together, else its own), `SketchProfile.cpp`
  (`TessellateEntity` + the loop→OCC-edge builder), `SketchSolver.cpp` (load
  loop + param-count switch), `SketchSelection.cpp` / `SketchInference.cpp`
  (picking/snap — mostly free via tessellation), `DrawingProjection.cpp`
  (verify the generic `GeomAbs_BSplineCurve` path isn't dropped), and the UI
  tool (`SketchInteraction.cpp` `SketchTool::Spline`, `SketchToolbar.cpp`).
  All mechanical; follow `Ellipse` everywhere it appears.
- **Pole-count vs `PointRef::which`.** `which` is `uint8_t` — a pole is
  addressed via `PointRef` so a spline with >255 poles overflows. v1: **cap
  poles at 255** and reject beyond (real sketch splines are <20 poles);
  widening `which` to `uint16_t` is a separate serialization concern, deferred.

### The two real risks (from `phase-l.md`, unchanged)
1. **Watertight OCC profile edge.** Turning spline entities into a valid
   closed `TopoDS_Wire`/`Face` mixed with lines/arcs for extrude/revolve
   (`SketchProfile.cpp` loop builder): OCC `Geom_BSplineCurve` (poles +
   weights + knots + mults) → `BRepBuilderAPI_MakeEdge` → wire. Degenerate /
   self-intersecting splines and tangent continuity at joins are the failure
   modes; guard with `ShapeFix_Wire` (already used for arc loops).
2. **Solver DOF depth.** PlaneGCS *can* solve splines, but Designer's
   `SketchSolver` wrapper models points/lines/circles/arcs and keeps
   **ellipses rigid** (`SketchSolver.cpp:223`, "only the centre is a solver
   point"). Two sub-tiers:
   - **v1 rigid net** (~cheap): each pole is a solver *point*
     (Coincident/Distance/Fixed on poles work, the spline translates with its
     poles), curve shape otherwise rigid — exactly the Ellipse treatment.
     Covers the importer's `fitSpline` (poles come from Onshape already
     placed).
   - **Fully-solved poles** (~+1 session): `GCS::BSpline` construction +
     point-on-spline / tangent-at-knot constraints wired through the
     validators. Do when sketch splines need to be dimensionally driven.

- **RPC/MCP: none** for entity creation (clients send the whole
  `SketchFeature` JSON through `add_feature`); `add_constraint`
  (`RemoteModeling.cpp`) only extends if spline-specific constraint kinds are
  added in the fully-solved tier.
- **Tests:** spline JSON round-trip; a closed spline extrudes to a plausible
  volume; a pole Coincident/Fixed solves and moves the spline; pole count
  survives save/load.

**Recommendation:** ship the **rigid-net v1** — it's what the importer needs
and it reuses the Ellipse pattern end-to-end. Fully-solved poles is a graded
follow-up.

---

## 3. 3D fit-spline (non-sketch curve object)

Onshape's `fitSpline` can also produce a **3D** curve through points not
lying on any one sketch plane. Designer has no 3D free curve object today —
every curve lives inside a `SketchFeature` on a plane. This is the largest of
the three because it introduces a **new geometry object category**, not just
a variant.

### Scope decision first
Most `fitSpline` uses in the target docs are *planar* (they fit through
points on a sketch) — those are covered by feature #2 and do **not** need
this. Build #3 only when a genuinely non-planar fit-spline blocks a real
import. Until then the importer reports the 3D case as a skip (it already
does). **Recommendation: design now, build on demand.**

### What it needs (design)
- **New object** `CurveFeature` (or a doc-level `Curve3D`) — not a sketch
  entity. Holds fit points (3D) + resulting `Geom_BSplineCurve`. Decide:
  a `Feature` in the regen list (parametric, participates in rollback) vs a
  doc-level object like `Study`/`DatumPlane`. Lean **`Feature`** so it regens
  with variable-driven fit points and can be a reference for downstream
  sweeps/lofts.
- **Geometry:** OCC `GeomAPI_Interpolate` (fit through points, optional
  tangents) → `Geom_BSplineCurve` → a free `TopoDS_Edge` carried on the
  feature (not part of a solid body).
- **Consumers:** the point of a 3D curve is to drive **Sweep** (path) and
  **Loft** (guide/section) — both already exist. Wiring a `CurveFeature` id
  as a sweep path is the integration work: sweep currently takes a sketch/
  open-path wire (`sketch::BuildOpenPathWire`), so it needs to also accept a
  `CurveFeature` edge.
- **Render:** a new render path for a standalone edge (the sketch line
  pipeline draws `LineVertex3D` from tessellated polylines — reuse it by
  tessellating the OCC edge; no solid mesh).
- **Serialization:** new `"Curve3D"` feature type, `kVersion` bump, golden.
- **Pick/UI:** pick the curve as a sweep-path reference; a "3D fit spline"
  tool that takes 3D points (from vertices / points in space) — the UX is the
  fiddly part (picking non-coplanar 3D points).
- **RPC/MCP:** `add_feature` round-trips it; `add_feature`-driven import can
  replay it once the type exists.

### Risk
Medium-large but **contained** — it doesn't touch the body-identity or sketch
variant systems. The real cost is the new object category + its render/pick
path + accepting it as a sweep/loft input. No architectural landmine.

---

## Suggested sequencing

1. **Split v1** — smallest, highest-confidence, unblocks `splitPart` import.
   Ship; file the robust body-identity tier as demand-gated.
2. **2D B-spline rigid-net v1** — unblocks planar `fitSpline` + spline sketch
   entities; reuses the Ellipse pattern; the widest but shallowest change.
3. **3D fit-spline** — design now (this doc), build when a non-planar
   fit-spline actually blocks an import.

Graded follow-ups (all demand-gated, none speculative): Split robust body
identity; B-spline fully-solved poles + point-on-spline/tangent constraints;
`PointRef::which` widening past 255 poles; sketch-as-knife for Split.

After each ships, update the importer to replay it (replace the skip) and
drop the corresponding line from `import-plan.md` §"Future modeling
features".
