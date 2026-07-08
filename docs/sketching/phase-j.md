# Phase J — Arcs (detailed plan)

Adds `sketch::Arc` as a fourth alternative of `sketch::Entity`, threaded through
every subsystem a `Circle` touches today, plus the profile-loop refactor that
lets a curved edge participate in a closed boundary alongside lines.

**Build & sequencing (decided):** one bundled phase — the arc entity *and*
circle/arc trim — landing as a single commit `feat: sketch arcs + curve trim`.
The steps below are still ordered so the build stays incremental and testable
(the arc entity is solid before trim leans on its intersection helpers); only
the final commit is unified. The arc drawing tool ships **both** constructions
(center→start→end and endpoints→bulge) — see step 8.

Reference findings applied (from plan.md "What the reference repos
taught us"): keep a **CCW-always** model convention and convert to OCC only at
shape-build time (avoids FreeCAD's `emulateCCWXY` retrofit); run `ShapeFix_Wire`
(FixReorder / FixConnected / FixClosed) on every wire before face building.

PlaneGCS already exposes the full arc API we need (verified in `deps/planegcs`):
`GCS::Arc` (inherits `Circle`'s `center`+`rad`, adds `startAngle`/`endAngle` and
real `start`/`end` **Points**), `addConstraintArcRules`, `addConstraintArcRadius`,
`addConstraintArcDiameter`, `addConstraintTangent(Line&, Arc&, ccw)`,
`addConstraintTangent(Circle&, Arc&)` / `(Arc&, Arc&)`,
`addConstraintEqualRadius(Circle&, Arc&)` / `(Arc&, Arc&)`,
`addConstraintPointOnArc`.

## Locked design decisions

- **Model type** (`SketchFeature.h`):
  ```cpp
  struct Arc {
    glm::vec2 center;
    double radius;
    double startAngle;  // radians, CCW from +U axis
    double endAngle;    // radians; sweep is CCW from startAngle to endAngle
    uint32_t id = 0;
    bool construction = false;
  };
  ```
  CCW-always: the arc runs counter-clockwise from `startAngle` to `endAngle`
  (normalize so `endAngle > startAngle`, sweep = `endAngle - startAngle` in
  `(0, 2π)`). No CW flag anywhere in the model.
- **`PointRef::which` for an arc: `0 = center`, `1 = start`, `2 = end`.** Center
  stays `which == 0` to match `Circle`, so all center-based logic (Concentric,
  radius dimension leader) is uniform across circles and arcs; the two
  trimmable endpoints are `1`/`2`.
- **Endpoints are solver points** (`GCS::Arc::start`/`end`), so Coincident /
  Distance / Fixed on an arc endpoint work with **no new constraint code** —
  they resolve through the existing `(entity, which)` point map.
- **`addConstraintArcRules` is definitional, not a user constraint.** It is
  emitted once per arc at load with a sentinel tag and must be excluded from the
  `constraintFirstTag` → conflict-index mapping (Phase I), or a redundant arc
  rule would show up as a phantom conflicting constraint.
- **Profile `Loop` becomes an ordered edge list** (see step 4). This is the one
  structural change; everything else is an added `if constexpr`/`case` branch
  next to the existing `Circle` one.
- **New shared helper `EntityPointUV(const Entity&, uint8_t which)`** in
  `SketchFeature.h` returning the UV of a ref's point for any alternative
  (Point→p, Line→a/b, Circle→center, Arc→center/start/end). `RefUV`,
  `EntityPoints`, `EntityDots` collapse onto it to avoid four parallel switch
  statements drifting.

---

# Part A — Arc entity, end to end

Each step compiles on its own; run tests after 4, 9, and 12.

### 1. `src/core/SketchFeature.h` — the type
- Add the `Arc` struct (above) and extend `using Entity = std::variant<Point,
  Line, Circle, Arc>;`.
- `EntityId`/`SetEntityId`/`IsConstruction`/`SetConstruction` are `std::visit`
  generic → **no change** (Arc has `id`+`construction`).
- Add free helpers: `glm::vec2 ArcPointUV(const Arc&, uint8_t which)` (0 center,
  1 start = `center + r·{cos,sin}(startAngle)`, 2 end), and the umbrella
  `EntityPointUV(const Entity&, uint8_t)`.
- Doc-comment the `which` convention on `PointRef` (center 0 / start 1 / end 2
  for arcs).

### 2. `src/core/Serialization.cpp` — persistence (bump `kVersion` 6 → 7)
- `EntityToJson`: add the Arc visit branch →
  `{"type":"arc","center":…,"radius":…,"start_angle":…,"end_angle":…}` (`id`
  and `construction` are stamped generically, unchanged).
- `EntityFromJson`: parse `type == "arc"` **before** the throwing `else`.
- New type = additive; old files simply contain no arcs, so no migration pass —
  note the bump in the version comment.

### 3. `src/remote/RemoteServer.cpp` — API
- Inherited through `FeatureFromJson` (arcs deserialize via step 2); the
  `entity_ids` echo already covers them. **No handler change.** (Confirm
  `add_feature` round-trips an arc in the smoke test, step 12.)

### 4. `src/core/SketchSolver.cpp` / `.h` — solving  → **run solver tests**
- `Impl`: add `std::vector<GCS::Arc> arcs;`, `std::map<uint32_t,int> arcOf;`,
  and a stable-address store for the two angles (`std::deque<double> angles;`).
- Param pre-pass: `holds_alternative<Arc>` adds **9** unknowns (center 2 +
  start 2 + end 2 + radius 1 + startAngle 1 + endAngle 1).
- Load branch (`std::get_if<Arc>`): build `GCS::Arc` — `center = addPoint(id, 0,
  a->center)`, `start = addPoint(id, 1, ArcPointUV(*a,1))`, `end = addPoint(id,
  2, ArcPointUV(*a,2))`, `rad` from `radii`, `startAngle`/`endAngle` from
  `angles`; register in `arcs`/`arcOf`; push the arc shape into
  `entityShapes`/`shapeIndexOf` (Tangent side needs it). Then emit
  `addConstraintArcRules(arcs.back(), kDefinitionalTag)` — **not** recorded in
  `constraintFirstTag`.
- Extend the constraint switch to admit arcs alongside circles:
  - `Radius` → `arcOf.count` → `addConstraintArcRadius(arc, value)`.
  - `Diameter` → `addConstraintArcDiameter(arc, value)` (or arc radius `value/2`).
  - `Equal` → the mixed overloads: `addConstraintEqualRadius(circle, arc)` /
    `(arc, arc)`.
  - `Tangent` → `addConstraintTangent(line, arc, ccw)` (compute `ccw` from the
    loaded arc shape exactly like the circle path); also `(circle, arc)` / `(arc,
    arc)` if both curved.
  - `Concentric` → already resolves center via `{entity, 0}` (uniform which) →
    works once `arcOf` centers are registered at which 0.
  - `Coincident`/`Distance`/`DistanceX/Y`/`Fixed` on `{arcId, 1|2}` → free
    (endpoints are solver points).
- `WriteBack`: `holds_alternative<Arc>` branch writes back `center` (from the
  center point), `radius` (from `arc.rad`), and `startAngle`/`endAngle` (from the
  angle params). Renormalize CCW after write-back.
- Stub build path (no PlaneGCS) stays generic.

### 5. `src/core/SketchProfile.{h,cpp}` — loops as edge lists  → the risk step
- `Loop` gains an ordered edge list; keep `circle` for pure-circle loops:
  ```cpp
  struct LoopEdge { bool arc; glm::vec2 a, b;      // endpoints, in order
                    glm::vec2 center; double radius; bool ccw; }; // arc-only
  struct Loop { std::optional<Circle> circle;
                std::vector<LoopEdge> edges; double SignedArea() const; };
  ```
  (Drop the raw `polygon` field; `MakeLoopWire`/`SignedArea` are its only
  readers and both move to `edges`.)
- `BuildLoops`: chain **lines and arcs** by endpoint proximity — an arc is an
  edge with `a = ArcPointUV(1)`, `b = ArcPointUV(2)` carrying its curvature;
  circles stay standalone `circle` loops. Chain by matching endpoints (reuse the
  `Near`/`used[]` walk, generalized to `LoopEdge`).
- `SignedArea`: tessellate each edge (arcs into `kCircleSegments·sweep/2π`
  chords) and shoelace — keeps outer-vs-hole selection correct.
- `MakeLoopWire`: per edge, line → `GC_MakeSegment`; arc → `gp_Circ(ax2 at
  center, radius)` then `BRepBuilderAPI_MakeEdge(circ, gpStart, gpEnd)` with the
  CCW trim range on the plane. **After building the wire, run `ShapeFix_Wire`
  (SetPrecision, FixReorder, FixConnected, FixClosed)** before `MakeFace` — for
  *every* loop, not just arc loops (cheap, and it fixes greedy-order edge cases).
- `TessellateEntity`: add the Arc case (startAngle→endAngle CCW, segment count
  ∝ sweep).

### 6. `src/ui/AppActions.cpp` — rendering
- `EntityDots`: Arc case → center + start + end dots (`ArcPointUV` 0/1/2).
- `BuildActiveSketchPrimitives` / `AppendSketchLines` flow through
  `TessellateEntity`+`EntityDots` → automatic once step 5 lands; construction
  dashing and conflict coloring are already entity-generic.

### 7. `src/ui/SketchInteraction.cpp` — picking
- `RefUV` and `EntityPoints`: Arc case via `EntityPointUV` (center/start/end
  pickable).
- `PickSketchEntity` body hit for an arc: `|dist(mouse, centerPx) − rPx|` within
  tol **and** the cursor's angle lies within `[startAngle, endAngle]` (CCW sweep,
  angle-wrap aware). Endpoints still win first.
- `DrawDimensions`: Radius/Diameter leader for an arc reuses the circle rim-point
  math (center at which 0).

### 8. Arc drawing tool — both constructions
Ship two arc constructions (both reduce to the same CCW-always `Arc` on commit):
- `src/ui/AppState.h`: add `SketchTool::Arc` (center-based) and
  `SketchTool::ArcThreePoint` (endpoint/bulge-based).
- `src/ui/SketchInteraction.cpp`:
  - **Center → start → end** (`SketchTool::Arc`): click 1 center; click 2 sets
    radius + `startAngle`; move previews the arc from start to the cursor's angle
    (CCW); click 3 sets `endAngle`.
  - **Start → end → bulge** (`SketchTool::ArcThreePoint`): click 1 start; click 2
    end; move drags a mid/through point — derive `center`, `radius`,
    `startAngle`, `endAngle` from the three points (circumcircle of the two
    endpoints + through-point; pick the CCW sweep that passes through it).
  - Both commit via the generic `CommitEntity` after renormalizing CCW.
  - `BuildSketchOverlay`: center mode previews a ring after click 1 then a live
    arc after click 2; three-point mode previews the chord after click 1 then the
    live arc through the cursor after click 2. Escape / tool-switch clears the
    pending state (reuses the existing `hasPendingPoint`-style handling).
- `src/ui/SketchToolbar.cpp`: two buttons next to Circle —
  `toolButton("Arc", SketchTool::Arc)` and
  `toolButton("3-Pt Arc", SketchTool::ArcThreePoint)`.
- Shared helper `Arc ArcFromThreePoints(start, through, end)` (circumcircle +
  CCW sweep selection) in an anon namespace, unit-tested in step 12.

### 9. Constraint validators admit arcs  → **run solver + feature tests**
- `SketchInteraction.cpp` `ApplySketchConstraint` and `SketchToolbar.cpp`: add an
  `isArc` lambda and let arcs satisfy **Radius, Diameter, Equal, Tangent,
  Concentric** (treat "circle-or-arc" wherever `isCircle` gates a radius/tangent
  today). Coincident/Distance/Fixed already accept any endpoint ref.

### 10. Dragging (minimal)
- Arc **endpoint** drag works through the existing point-drag path (endpoints are
  solver points; WriteBack updates the arc). Ship that.
- Arc **rim/center** body-drag → **deferred refinement** (note it; add a
  `SketchDragSession::Mode::ArcRim` later, mirroring `CircleRim`).

### 11. `src/core/SketchInference.cpp` — snapping (light)
- Add arc **center** and **endpoints** to the coincident snap-vertex pass
  (mirror the circle-center line). Rim/tangent snapping → deferred to a later
  inference pass; not required for J1.

### 12. Tests  → **full deterministic suite**
- `tests/test_sketch_feature.cpp`: arc JSON round-trip (center/radius/angles +
  construction + id stability) in the existing round-trip cases.
- `tests/test_sketch_solver.cpp`: (a) `addConstraintArcRadius` drives an arc's
  radius; (b) an arc endpoint made Coincident with a line endpoint solves and
  stays on the arc; (c) Tangent line↔arc keeps the arc on its side.
- `tests/test_extrude.cpp`: a **filleted-corner** profile — a rectangle with one
  corner replaced by a quarter-arc (line, arc, line, …) — closes, extrudes, and
  matches the analytic volume `(rect − corner + quarter-disk)·h`; also a
  half-disk (diameter line + semicircle arc) loop.
- `tests/remote_smoke.py`: `add_feature` an arc entity; assert it round-trips in
  `get_state`.
- `ArcFromThreePoints`: unit-test that three sample points yield the expected
  center/radius and a CCW sweep through the middle point.

---

# Part B — Circle & arc trim → arcs

Unlocks the Trim tool on curves (today `SketchInteraction.cpp` refuses circles
with a log line) using Phase B/D split machinery. Builds on Part A's `Arc` type
and its line↔circle / line↔arc intersection helpers.

- `src/core/SketchFeature.cpp`: extend `SplitEntity`/`TrimSpanAt` (currently
  `std::get_if<Line>` only):
  - **Split a circle** at two intersection angles → replace with an `Arc`
    covering the *kept* span (the clicked span is removed), preserving the
    entity id on the survivor per the split-keeps-piece-1 discipline.
  - **Split an arc** at an interior point → two arcs that share a new center-less
    joint point, tied by a **Coincident** on the split point (FreeCAD policy).
  - Intersections: reuse/extend the trim intersection finder to line↔circle and
    line↔arc (circle: solve `|p−c| = r` along the segment; arc: same, then keep
    hits whose angle is within the sweep).
- `src/ui/SketchInteraction.cpp`: remove the circle refusal at the Trim branch;
  route curve hits through the extended split; pick the clicked span via the
  cursor angle.
- Tests (`test_sketch_feature.cpp`): trim a circle crossed by two lines → the
  clicked span is removed leaving an arc (correct start/end angles, id
  preserved); trim an arc mid-span → two arcs + a Coincident; the resulting
  profile still closes and extrudes.

---

## Done-when (whole phase, one commit)
Deterministic suite green, and via the remote screenshot path: both arc tools
draw arcs, a filleted rectangle extrudes to a smooth solid, an arc-radius
dimension edits live, and a circle crossed by two lines trims to an arc. Commit
`feat: sketch arcs + curve trim`.

## Risk & sequencing
- **Highest risk: step 5 (loop edge list + wire orientation).** Mitigation is
  the mandated `ShapeFix_Wire` pass and the filleted-corner extrude test as the
  canary. Get one arc-bearing profile extruding (steps 1–5) before wiring the
  tools/UI, and before Part B leans on the intersection helpers.
- The build stays incremental (test checkpoints after steps 4, 9, 12, and after
  Part B) even though it lands as a single commit.
- Est: ~3.5–4 days total.

## Plan-file bookkeeping
- After the commit: plan.md — mark Phase J shipped (arc entity +
  curve trim). The `Deferred` "Circle & arc trim" line resolves. Phase K's Fillet
  ("needs Phase J arcs") is unblocked.
