# Phase L — Ellipses + B-splines (plan)

Two new curve types threaded through the same pipeline every other entity uses
(model → serialization → solver → profile → render → pick → tool). Ellipses are
the tractable half (fixed parameters, OCC `gp_Elips`, closed loop) and are being
built first; B-splines are larger (variable poles/knots, control-point UX) and
are planned here for review.

## Ellipse (building now)

- **Model** (SketchFeature.h): `struct Ellipse { glm::vec2 center; double
  rMajor, rMinor; double rotation; uint32_t id; bool construction; }` — the
  major axis is `rotation` radians from +U. Add to the `Entity` variant.
  `PointRef::which` 0 = center. `EntityPointUV` returns the centre.
- **Serialization** (v8 → v9): `{"type":"ellipse", center, r_major, r_minor,
  rotation}`.
- **Solver** (v1 — minimal, safe): register the ellipse *centre* as a solver
  point (like a circle centre) so Coincident/Distance/Fixed on the centre work
  and the ellipse translates when its centre is dragged; radii/rotation are
  **rigid** (not solved). WriteBack reads the centre back. No PlaneGCS ellipse
  rules yet — see follow-up.
- **Profile** (SketchProfile.cpp): a pure-ellipse loop builds a `gp_Elips`
  (major/minor on the plane basis rotated by `rotation`) → one wire edge, like
  the circle path. `TessellateEntity` samples the ellipse into a polyline.
- **Render / pick**: `EntityDots`/`EntityPoints` expose the centre; the
  tessellated polyline drives body-pick and rendering (arcs already do this).
- **Tool**: `SketchTool::Ellipse` — 3-click: centre → major-axis end (sets
  `rMajor` + `rotation`) → a point giving the minor radius (perpendicular
  distance). Toolbar icon.
- **Remote**: `add_feature` ellipse round-trips via serialization; no new
  handler.
- **Tests**: ellipse JSON round-trip; ellipse extrudes to the analytic area
  (`π·a·b·h`); centre Coincident/Fixed solves and moves the ellipse.

### Ellipse solver follow-up (deferred)
Full constrainability (Radius-major/minor, Tangent, Point-on-ellipse) needs
PlaneGCS's `GCS::Ellipse` (centre + focus1 + radmin) and the
`addConstraintInternalAlignmentEllipse*` rules (`GCS.h:507-534`). Convert our
centre/rMajor/rMinor/rotation ⇄ focus1 (`c = √(a²−b²)` along the rotation) +
radmin, add the alignment/rules, and expose Equal/Radius/Tangent to ellipses in
the constraint validators. Sizeable; do when ellipse constraints are wanted.

## B-spline — ✅ v1 SHIPPED (format v40)

Built as the rigid-net v1: `sketch::BSpline{poles, degree}` threaded through
the full pipeline (model → serialize → solver → profile → render → pick →
tool). Non-rational, clamped, uniform; weights/explicit knots are the
defaults (not stored). Header de Boor evaluator agrees with the OCC
`Geom_BSplineCurve` profile edge (same knot vector → watertight). Each pole
is a free solver point (Coincident/Distance/Fixed work; drag reshapes);
poles capped at 255 (`PointRef::which` is `uint8_t`). `SketchTool::Spline`
(click poles, Enter/double-click finish), toolbar `ICON_FA_PEN_NIB`, key
`S`. Tests in `tests/test_spline.cpp` + v40 golden. See the shipped summary
in [../interchange/feature-gaps-plan.md](../interchange/feature-gaps-plan.md).
**Deferred:** fully-solved poles (`GCS::BSpline` + point-on-spline/tangent-
at-knot), expression-driven poles, splines in sweep paths, 3D fit-splines.

The original build plan (retained for the deferred items):

## B-spline (planned — larger, review before building)

- **Model**: `struct BSpline { std::vector<glm::vec2> poles; std::vector<double>
  weights; std::vector<double> knots; int degree; bool periodic; uint32_t id;
  bool construction; }`. `PointRef::which` addresses a pole (so poles are
  draggable/constrainable points). Non-trivial: `which` is `uint8_t` today — a
  spline with >255 poles would overflow; widen `which` or cap poles.
- **Solver**: PlaneGCS has `GCS::BSpline` + `addConstraint*` for control points;
  each pole is a solver point (Coincident/Distance on poles work). Knot/weight
  handling per the FreeCAD Sketcher BSpline model.
- **Profile**: OCC `Geom_BSplineCurve` (poles + weights + knots + mults) →
  `BRepBuilderAPI_MakeEdge` → wire. Closed/periodic splines form a loop; open
  splines need a closing edge to extrude (same as an open arc chain).
- **Render**: tessellate the OCC/self-evaluated curve; draw the control polygon
  (dashed) + poles as draggable dots.
- **Tool**: click to place poles; double-click / Enter to finish; drag poles to
  reshape. Degree default 3.
- **Serialization**: poles/weights/knots/degree/periodic; a v-bump.
- **Tests**: spline round-trip; a closed spline extrudes; dragging a pole
  reshapes and re-solves; the control-point count survives save/load.

Risk: medium-large — the OCC B-spline construction (knot vector + multiplicities)
and the pole-count vs `PointRef::which` width are the fiddly parts.

## Sequencing
Ellipse v1 first (this session), committed as `feat: sketch ellipses`. B-splines
and the ellipse-solver-rules follow-up are separate commits after review.
