# Sketching

A sketch is a 2D constraint-solved drawing on a plane. Extrude, revolve and
loft consume its closed profiles, sweep its open paths, and the Hole tool its
points. It is a
`Feature` in the history (`SketchFeature`), contributes no bodies itself, and
is solved by a vendored copy of FreeCAD's PlaneGCS (`deps/planegcs/`).
Coordinates are plane-UV millimetres. Angles are radians in the model and
in the file; the UI and expressions show degrees.

## Where the code is

| what | where |
|---|---|
| data model, entity ops (add/delete/split/trim/mirror/fillet/pattern/offset, derived ops, images) | `src/core/features/SketchFeature.{h,cpp}` |
| PlaneGCS adapter, drag sessions, DOF diagnosis, value edits (continuation), `JudgeAddition` | `src/core/sketch/SketchSolver.{h,cpp}` |
| orientations a value edit must keep, `MeasureDimension` (pure geometry) | `src/core/sketch/SketchOrientation.{h,cpp}` |
| loops, OCC wires/faces/prisms/revolves, offset, projection of model faces/edges | `src/core/sketch/SketchProfile.{h,cpp}` |
| text labels: fonts, glyph layout, box corners | `src/core/sketch/SketchText.{h,cpp}`, the Hershey data `src/core/sketch/HersheyRomanSimplex.inc` |
| snap/auto-constraint inference (pure, camera-free) | `src/core/sketch/SketchInference.{h,cpp}` |
| box-select hit test (pure) | `src/core/sketch/SketchSelection.{h,cpp}` |
| serialization | `src/core/io/SerializeFeatures.cpp` (`EntityFromJson`, `ConstraintToJson`, `DerivedOpToJson`, the sketch arm of `FeatureToJson/FromJson`) |
| viewport interaction, Escape ladder | `src/ui/SketchInteraction.cpp` (`HandleSketchViewport`) |
| drawing tools, commits, drag sessions, copy/paste | `src/ui/SketchTools.cpp` |
| picking | `src/ui/SketchPick.cpp` |
| constraint/construction/delete ops, the Driven toggle, DOF status | `src/ui/SketchOps.cpp` |
| dimension tool, dimension drawing and inline editor, glyphs | `src/ui/SketchDimensions.cpp`, `src/ui/SketchDimensionDraw.cpp` |
| entity colours and line primitives | `src/ui/SketchOverlay.cpp` (`BuildActiveSketchPrimitives`) |
| prompt pills | `src/ui/SketchToolOverlay.cpp` (`DrawToolPrompt`, `SketchToolPrompt`) |
| tool-window sessions, Operations list | `src/ui/SketchToolbar.cpp` |
| key bindings | `src/core/io/Keymap.cpp` (`kDefaults`), `src/ui/Commands.cpp` |
| RPCs | `src/remote/RemoteModeling.cpp` |
| reference-image import/actions, renderer | `src/ui/ImportActions.{h,cpp}`, `src/render/UnderlayRenderer.{h,cpp}`, `src/shaders/underlay.{vert,frag}` |

## Data model

### Plane

`SketchFeature::plane` is stored as a baked value. When `faceRef` is set the
sketch is attached to a body face and `Execute` re-derives the plane from the
resolved face every regeneration; when `planeRef` names a datum plane,
another sketch (its plane) or a Frame (its XY plane, X as U) earlier in the
history the plane is refreshed from it. A reference that is gone,
suppressed or itself in error is a regeneration error naming it, never a
fall-back to the last plane; an Extrude or Revolve of a failed sketch fails
with it. The last two kinds are what a primitive placed on a sketch or a
frame attaches by ([../modeling/README.md](../modeling/README.md#primitives)).

The plane is an origin, a normal and a `uAxis`; V is normal × uAxis, and
sketch UV is measured from the origin. A positive extrude runs along the
normal, so which way the plane faces decides which way a feature grows. The
SDK's `Plane.xy/xz/yz` helpers take `flip=True` for the opposite-facing plane;
their axes are tabled in
[the SDK guide](../scripting/sdk.md#the-principal-planes).

**The plane and everything on it are double precision**, like the rest of the
model ([../conventions.md](../conventions.md#the-model-is-double)), from the
document through the solver to OpenCASCADE: `SketchPlane` holds `glm::dvec3`,
every entity coordinate is a `glm::dvec2`, and JSON reads and writes them as
the doubles they are. A feature starts on its plane, so a value rounded here
becomes a face microns off the one it was meant to meet: through float, a
plane typed at z = 13.4 was 13.39999962, a pocket cut from it kept a 0.4 µm
skin over itself, and an expression-driven sketch re-solved from a rounded
seed on every regeneration. Float appears only where a value is handed to the
renderers (`GetPlaneMatrix`, the overlays) or read off the screen (a pick
widens into the sketch). A plane built from a face takes the B-rep face's
frame (`CADShape::FacePlane`), never the render mesh's. A frame that is
already orthonormal is kept bit for bit, so a plane saves and loads unchanged.

### Entities

`sketch::Entity` is a variant of:

| type | fields | JSON `type` | solver treatment |
|---|---|---|---|
| `Point` | `p` | `point` | one free point |
| `Line` | `a, b` | `line` | two free points |
| `Circle` | `center, radius` | `circle` | centre point + radius |
| `Arc` | `center, radius, startAngle, endAngle` | `arc` | centre, start, end points + radius + angles, tied by `addConstraintArcRules` |
| `Ellipse` | `center, rMajor, rMinor, rotation` | `ellipse` | `GCS::Ellipse` (centre, focus, minor radius); axis ends are points tied to it ([below](#ellipses-and-splines)) |
| `EllipseArc` | ellipse + `startParam, endParam` | `ellipse_arc` | `GCS::ArcOfEllipse`: the ellipse plus start/end points and params, tied by `addConstraintArcOfEllipseRules` |
| `BSpline` | `poles, degree` | `spline` | `GCS::BSpline` over the poles (each a free point), knots and weights fixed |
| `Text` | `anchor, text, font, height, angle, halign, valign, spacing` | `text` | the anchor a free point; the glyphs rigid; the box corners ride the anchor ([Text](#text)) |

Every entity also has `id` and `construction`. Rules that hold everywhere:

- **Arcs (and elliptical arcs) are counter-clockwise always.** `endAngle` is
  normalized so the CCW sweep lies in (0, 2π). OCC's form is produced only
  when a wire is built, so no CW flag exists anywhere in the model; every
  transform (`ReflectEntity`, which flips winding and swaps the ends) keeps
  the invariant.
- **A B-spline is non-rational, clamped and uniform.** Weights and knots are
  not stored — `BSplineKnots` produces exactly the knot vector OCC builds for
  the same poles, so the header evaluator (`BSplineEvalUV`) and the OCC
  profile edge agree. Degree is cubic, reduced to poles−1 for short splines.
  Poles are capped at 255 because `PointRef::which` is a `uint8_t`.
- **Rectangles are not a primitive.** `AddRectangle` emits four lines, four
  corner coincidents, two horizontal and two vertical constraints, so every
  side is editable and trimmable. `AddRectangleCentered` adds a centre point.
  A legacy `rect` entity in an old file is expanded the same way on load.
- **Construction** geometry drives constraints and snapping but is excluded
  from profiles and paths, and draws dashed.

### Entity ids

Every entity carries a stable **id**; constraints, derived ops and selections
refer to ids, never list indices (which shift on insert/delete).
`AddEntity` mints the id with `MintEntityId`: random, non-zero, and retried
against the sketch's own entities. Random rather than a per-sketch counter
because a face's stable name is `<feature>/g<entityId>` (see
`BuildPrismNamed`): two people editing one sketch separately would both mint
the next counter value, and a duplicated id makes a face name — and every
fillet, mate or drawing dimension keyed on it — ambiguous while the document
rebuilds without complaint. A sketch that arrives with two entities sharing
an id is **refused on load**. `IndexOfEntity(id)` is a linear scan (sketches
are small, and a cached map is a desync risk).

`PointRef {entity, which}` addresses a point: Line 0 = `a`, 1 = `b`; Circle
and Ellipse 0 = centre; Arc and EllipseArc 0 = centre, 1 = start, 2 = end; Ellipse
and EllipseArc 3/4 = the major-axis ends (+û, −û), 5/6 = the minor-axis
ends (+v̂, −v̂); BSpline k = pole k; Text 0 = anchor, 1..4 = the box corners (bottom-left,
bottom-right, top-right, top-left). `kWholeEntity` (0xFF) marks a *selection* of the whole
entity; constraints never use it.

### Text

A `Text` entity is a one-line label (v137): ONE entity whose glyphs are
regenerated from its fields by `sketch::BuildTextGeometry` and never saved.

| field | meaning |
|---|---|
| `text` | UTF-8, one line: a newline is refused |
| `font` | a bundled font by name: `sans` (outline, Liberation Sans) or `hershey` (single stroke, Hershey Roman Simplex); any other name is refused, listing these |
| `height` | the CAP height, mm, measured once per font from 'H' |
| `anchor` | the point the label is placed by, and its only solver unknowns |
| `angle` | radians CCW from +U (the baseline's direction); `angle_deg` in JSON and expressions |
| `halign` / `valign` | `left`/`center`/`right`; `baseline`/`bottom`/`middle`/`top`, from the FONT's ascender and descender, never the string's ink, so a row of labels lines up |
| `spacing` | extra mm between characters |

The label's **box** runs from the pen's start to its end (the advance of the
whole string, kerning and spacing included) and from the font's descender to
its ascender. Its corners are `which` 1..4: solver points tied to the anchor
by fixed offsets, so the solver reads them but never moves one relative to
the others. Dimensioning a corner, or making it coincident with something,
places the label. The expression fields are `x`, `y` (the anchor), `height`,
`angle_deg` and `spacing`.

**Fonts are bundled, never the system's**, so a document builds the same on
every machine:

- **Outline** glyphs come from OpenCASCADE's `StdPrs_BRepFont` (FreeType,
  `TKService`, linked explicitly), initialised on the TTF under
  `share/designer/fonts/` (`deps/fonts/` in the tree; `DataDir()`), never by
  a font-manager name. OCC's unicode-subset fallback is switched OFF (it
  would borrow a missing glyph from a SYSTEM font), and every character is
  checked with `HasSymbol` first: a missing glyph is refused, naming it.
  Composite-curve mode stays off, so every segment is a line or a 3/4-pole
  Bezier, which is exactly the sketch's clamped B-spline of degree 2/3:
  nothing is flattened. Glyphs are placed by their advance, with kerning from
  the font's `kern` table; there is no shaping (no ligatures, no complex
  scripts). The bundled Liberation Sans has no overlapping contours
  (profiles refuse partial overlap), which a test checks for every printable
  ASCII glyph.
- **Single stroke**: the original Hershey Roman Simplex data (the
  `rowmans.jhf` records, attribution in `LICENSES/Hershey.txt`), embedded and
  decoded from its `'R'`-offset pairs (Y down). Printable ASCII only. Its
  contours are OPEN polylines, for engraving.

A label that cannot be laid out (unknown font, missing glyph, empty text) is
its sketch's regeneration error, named. Mirroring a label mirrors its anchor
and direction but not its letters: a mirrored label still reads.

### Datum geometry

The origin and the two axes are always available as references without being
entities: `kOrigin`, `kAxisX`, `kAxisY` live in a reserved id block
(`kDatumBase` = 0xFFFFFF00) above any minted id. The solver registers them as
fixed points and lines on every load, so a constraint to a datum id (coincident
to origin, point on an axis, symmetric about an axis, two-point H/V to origin)
needs no special case, and they serialize as ordinary ids. `IndexOfEntity`
returns `SIZE_MAX` for them, so **any code resolving a ref by index must test
`IsDatum` first** and use `DatumPointUV` / `DatumAxisEndpoints`.

### Constraints

`sketch::Constraint {kind, a, b, c, value, labelOffset, valueExpr, driven}`.
Kinds (JSON strings in brackets):

- Relations: Coincident [`coincident`], Horizontal [`horizontal`], Vertical
  [`vertical`], Parallel [`parallel`], Perpendicular [`perpendicular`], Equal
  [`equal`], Tangent [`tangent`], PointOnLine [`point_on_line`], Symmetric
  [`symmetric`], Midpoint [`midpoint`], Fixed [`fixed`], Concentric
  [`concentric`], Collinear [`collinear`].
- Dimensions (`IsDimension`; carry `value`, an optional expression, and a
  label position): Distance [`distance`], DistanceX [`distance_x`], DistanceY
  [`distance_y`], DistancePL [`distance_pl`, point-to-line], Radius
  [`radius`], Diameter [`diameter`], Angle [`angle`].

Notes on how they map to PlaneGCS (`Solver::Load`):

- Horizontal/Vertical take one line, an ellipse or elliptical arc (its major
  axis, ends 3–4), or two points (`a`, `b`) — the two-point form is what
  alignment inference emits.
- PointOnLine is point-on-CURVE: `b` may be a line, circle, arc, ellipse,
  elliptical arc (its whole ellipse, as an arc's is its circle) or spline.
- Symmetric uses `c` as the symmetry line (or a point).
- Fixed holds a point's parameters out of the unknowns; it adds no equation.
- Concentric is a coincident of the two centres; Collinear is Parallel plus
  a point-on-line.
- Tangent's side (which side of the line the curve sits on) is taken from the
  current geometry at load, so a solve never flips a circle across its line.
- Two curves that meet at an end (a Coincident joins an end of one to an end
  of the other — every fillet, every slot, a spline running into a line;
  ends are a line's 0/1, an arc's or elliptical arc's 1/2, a spline's first
  and last pole) are tangent THERE, as an angle at that point
  (`addConstraintAngleViaPoint`, 0 or π as the geometry already is), not
  the centre-to-line distance. Held exactly, the distance form and "the
  shared end lies on the arc" are the same equation to first order, so every
  fillet read as one redundant constraint and a phantom DOF. A spline's
  tangent at an end is its end leg (pole 0→1, or the last two).
- Without a shared end, Tangent takes a line and a circle/arc (signed
  distance), a line and an ellipse/elliptical arc
  (`addConstraintTangent(Line&, Ellipse&)`, unsigned: a line cannot cross
  an ellipse without cutting it, so it keeps its side), or two circles/arcs
  (inside or outside as they are at load).
- Angle is stored in **radians**; its expression is written in degrees
  (`VisitExprs` converts).
- Radius/Diameter accept circles and arcs; Equal two lines, two
  circles/arcs, or two ellipses/elliptical arcs (both radii); Concentric any
  two of circle, arc, ellipse, elliptical arc.

An unknown kind string is refused on load (`ConstraintKindFromString`), never
read as some default kind.

**Driven (reference) dimensions.** A dimension with `driven` set measures the
sketch instead of driving it. It adds no equation: `Solver::Load` dispatches
it like a driving one (so it is refused for exactly the same refs) and then
takes its equations out again, leaving its tag range empty, so it never
appears in the conflict lists or the DOF, and `FindOrientations` does not
count it as an edge. It has no input: its value is
`sketch::MeasureDimension` of the current geometry (`SketchOrientation.h`),
computed where it is read — the label, the serializer, `get_state`,
`add_constraint`'s `measured` — so it cannot go stale; the stored `value` is
not its truth. In JSON it is `"driven": true` (omitted when false) with
`value` written as the measure. The loader refuses `driven` on a kind that
is not a dimension (naming the dimension kinds), on a dimension with an
`expr`, and when it is not a boolean. Making a driven dimension driving
takes its measured value and is judged like an addition (below).

Since it has no input, no path gives it one: it is no `VisitExprs` slot, so
`check_literals` never calls its measure a literal and `extract_variables`
never offers it a variable, and a configuration — which overrides variables,
not dimensions ([configurations](../configurations/README.md)) — cannot
reach it. A dimension an expression drives cannot be made driven with the
expression kept (the loader, `add_constraint` and the GUI toggle all refuse
it), so an override never lands on a driven dimension. Drawings never read
sketch dimensions; their dimensions are measured from the projected view
and are all driven anyway ([drawings](../drawings/README.md)).

#### Refusals

A constraint the solver cannot represent — a radius on an ellipse, a
tangent between a circle and a spline, a ref to a point an entity does not
have — is never added to nothing. `Solver::Load` records each one with the
reason and what its kind takes (`UnsupportedConstraints()`); the dispatch IS
the validator, so what is refused and what is solved cannot drift apart.
`add_constraint(s)` refuses with that text and leaves the sketch as it was;
the GUI's constraint commands show it ("Not added: …") and add nothing; a
sketch LOADED with one (hand-written JSON, an older build) carries a
regeneration warning naming the constraint, because it solves as if absent
while the sketch looks constrained.

**A constraint that constrains nothing is refused.** Every add goes through
`sketch::JudgeAddition(before, after)` (`SketchSolver.h`): one solve of
each, and the addition is accepted when the solve after it succeeds (unless
the one before already failed), no constraint is conflicting or redundant
after that was not before, and — when anything new drives — the DOF is
strictly lower. So a sketch that is already over-constrained still takes a
constraint that touches none of its broken ones, and adding only driven
dimensions need not lower the DOF. A refusal names what the addition runs
into: for a contradiction, the conflicting group PlaneGCS names; for a
repeat, which PlaneGCS names on one side only (usually the new constraint),
every driving constraint without which it would constrain, found by solving
once with each left out (only on a refusal, and not past 200 constraints):
`the sketch already fixes that (constraint 5)`, `that contradicts what the
sketch already fixes (constraints 0, 1, 6)`. Indices are into
`constraints`. `add_constraint(s)` refuses with it ("not added: …", adding
"pass driven: true (and no value) to add it as a reference dimension" for a
dimension kind) and leaves the sketch as it was; `add_constraints` judges
the whole list in one solve and, on a refusal, steps through it to name the
first refused spec's index (`constraints[1]: …`). The GUI's relation
commands and a dimension typed right after drawing say "Not added: …" and
add nothing; the Dimension tool adds the dimension driven instead
([GUI](#gui)); inference (`ApplyInferred`) drops it silently, as it always
did. A sketch already saved with redundant constraints loads and solves as
before, painted red; nothing converts it.

A dimension VALUE the sketch cannot take is refused too, in the same words
on every path ([Solving](#solving) says how it is found):
`radius (constraint 4) = 49 cannot be met`, `distance (constraint 7) = 51
would mirror the triangle of constraints 5, 6, 7`, `radius (constraint 4) =
5000 would turn arc 3 inside out` (arc 3 is the entity id). The GUI's
inline editor shows it as "Not changed: …" and keeps the old value,
expression and geometry with no undo record; `modify_feature` throws it and
leaves the document unchanged; a regeneration (`set_variable` on a variable
a dimension reads) keeps the previous geometry and carries it as the
sketch's warning, "…, keeping the previous geometry", in the `regen` of the
call that caused it.

### Ellipses and splines

An ellipse is PlaneGCS's own `GCS::Ellipse`: centre, `focus1` (at
`√(a² − b²)` from the centre along `rotation`) and the minor radius b —
exactly its five degrees of freedom, so nothing holds it together. Its
**axis ends** (`which` 3/4 major, 5/6 minor) are solver points tied to it by
`addConstraintInternalAlignmentPoint2Ellipse` (tag 0, definitional): two
unknowns and two equations each, so the DOF stays 5, and radii and
orientation become ordinary point constraints:

- a radius is a `distance` from the centre (0) to an axis end (3 or 5) —
  `radius` on an ellipse is refused, since picking one of its two radii for
  the caller would be a substitute;
- Horizontal/Vertical on an ellipse levels its major axis;
- an axis end can be made coincident, fixed, dimensioned or dragged.

The focus is an unknown like any point, so the smallest step in the
unknowns is NOT the smallest change in the shape: a coincident that moved
the centre left the focus behind and reshaped the ellipse (1.5 × 0.5 came
out 0.85 × 0.68, turned 100°), and levelling one shrank it. So each ellipse
carries **soft holds** on its focal distance, minor radius and turn (and an
elliptical arc on its two end parameters): constraints with a negative tag,
which PlaneGCS solves as a second priority — minimised within what the real
constraints allow, never counted in the DOF or the diagnosis. Their
minimisation stops near 1e-9, so `Solver::Load` first solves with the holds
HARD (tag 0, in a throwaway system): when the constraints can be met by
moving ellipses whole, that answer keeps their shape exactly, and the real
system starts already solved. Dragging the centre therefore translates an
ellipse; dragging an axis end resizes and turns it.

The write-back is `rMajor = √(|F−C|² + b²)`, `rotation = atan2(F − C)`
(kept within half a turn of the stored one). When the focus offset (to
1e-12 of the size) and b come back unmoved, the stored `rMajor`/`rotation`
are kept bit for bit: re-deriving them through the focus rounds by an ulp,
and a sketch re-solved on every regeneration would walk. An ellipse is
`rMajor >= rMinor > 0` (refused on load otherwise; `EllipseFromClicks`
swaps the axes and turns a quarter when the third click is the longer); a
**circle-shaped** one (rMajor = rMinor) has no focus direction, so it stays
rigid — its centre free, every other point riding it.

That last bit has to be the same wherever the sketch solves, because a
project's parts load on worker threads and the chain hash reads it.
FreeCAD's PlaneGCS ordered a subsystem's unknowns, and the two-priority
solve's, by their **addresses**, and which of the solver's parameter blocks
sits lower is the heap's choice — a worker's arena chose differently from
the main one, so a held ellipse solved to a different ulp about one load in
five. The vendored copy keeps the declared order (`SubSystem::initialize`,
`System::solve(subsysA, subsysB)`); `[parallel]` in
`tests/test_sketch_curves.cpp` solves with the heap's small blocks handed
out in shuffled orders, and on eight threads at once, and requires the same
bits.

An **elliptical arc** is a `GCS::ArcOfEllipse` with real start/end points
(1/2) and its end parameters as unknowns. PlaneGCS measures the parameter
from the major axis, as the model stores `startParam`/`endParam`, so they
write back directly, renormalized CCW like an `Arc`'s. DOF 7.

A **spline** is a `GCS::BSpline` over its pole points, with the
clamped-uniform knots (`BSplineKnots`) and unit weights, both fixed. A
clamped spline's ends ARE poles 0 and n−1, so they need no points of their
own. A spline whose first and last poles are equal is closed, and a
definitional coincident keeps it closed (skipped when both are held — say
expression-driven — since it would have no unknown). Point-on-spline adds
one unknown, the point's curve parameter, seeded at the nearest point. DOF
2 per pole (2 fewer when closed).

In the GUI the axis ends are dots and pick points (inference snaps to them
too), so dragging one resizes and turns the ellipse. Dragging an ellipse or
elliptical arc by its BODY drags its centre (the holds carry the rest);
dragging a spline by its body moves every pole by the cursor's delta — it
translates rather than reshaping around one pulled pole. The Dimension tool on an ellipse's body places its major radius
(centre → major end); the minor radius is the centre and a minor end. Tests:
`tests/test_sketch_curves.cpp` `[ellipse]` `[spline]`, golden
`v155-ellipse-axis-ends`.

### Derived ops (linked generative geometry)

`SketchFeature::derived` holds `DerivedOp`s that regenerate their outputs
from their sources. `instances` maps each `(sourceId, k)` to the id of the
generated entity, and `RegenerateDerived` **reuses that entity in place**
rather than recreating it, so ids — and constraints and downstream face names
that reference them — survive every replay; growing a count adds entities,
shrinking deletes the surplus. Kinds:

| kind | JSON | parameters |
|---|---|---|
| `LinearPattern` | `linear_pattern` | `vec` step, `count`; optional second direction `vec2`, `count2` (row-major grid) |
| `CircularPattern` | `circular_pattern` | `vec` centre, `scalar` **per-step** angle (radians), `count`; optional `centerEntity`/`centerWhich` to track an entity point |
| `Mirror` | `mirror` | `axisEntity` (a line or `kAxisX`/`kAxisY`), resolved fresh each replay |
| `LinkedCopy` | `linked_copy` | `vec` paste offset, `sourceSketchId` (0 = same sketch) |

`count` includes the seed. A vanished source prunes its instances (for a
same-sketch op); an unresolvable mirror axis prunes all of them. The replay
runs after every edit commit, and after `Execute` re-solves an
expression-driven sketch.

Baked (unlinked) mirrors and patterns leave no op — just plain entities.
Fillet and offset always bake.

### Linked copies

`Ctrl+C` in a sketch copies the selection; `Ctrl+V` pastes plain copies with
fresh ids; `Ctrl+Shift+V` pastes **linked** copies — a `LinkedCopy` op whose
outputs re-sync from the source until the link is broken.

- The sync **rewrites geometry at regeneration**; it is not a constraint.
  PlaneGCS solves one sketch at a time, so constraints cannot span sketches.
- Outputs are **solver-fixed**: their parameters are held out of the unknowns
  (`Solver::Load`), so the target sketch's constraints may reference them but
  never move them — the sync owns their position. They draw violet.
- A same-sketch source replays in `RegenerateDerived`. A cross-sketch source
  must be **earlier in the history** (so regeneration stays one forward pass)
  and replays in `Execute` (`ReplayLinkedCopyFrom`), after the source has
  executed. A vanished source sketch or source entity **fails the sketch's
  regeneration** with a break-link hint; it never desyncs silently.
- `BreakLink(opIndex)` removes the op and keeps its outputs as ordinary
  geometry with ids intact, so profile regions and constraints on them survive.
- Paste-linked falls back to a plain paste, with a message saying why, when
  the source is in another part, is not earlier in the history, or has lost
  one of the copied entities.

The same solver-fixed mechanism holds **expression-driven coordinates**
(`entityExprs`, see [../variables/README.md](../variables/README.md)) and
**external geometry** (below) in place: the parameter table or the source
body owns them, not the solver.

### External (projected) geometry

`AddExternalGeometry(ElementRef, Face|Edge)` references a face or edge of a
body earlier in the history. `Execute` re-projects it onto the plane every
regeneration (`ProjectElement`) as solver-fixed construction entities with
stable ids (`ExternalGeom::genIds`), so geometry constrained to it follows the
source. A reference that no longer resolves fails the sketch. Planar curves
parallel to the sketch keep their type; anything else is sampled into a spline.

### Reference images (underlays)

A `sketch::ReferenceImage` is a raster placed on the plane to trace over. It is
**not an entity**: it has no curve and takes no part in the profile, the solve
or the regeneration hash (`Document.cpp` erases `images` before hashing, so
toggling one never rebuilds downstream features). Placement is a rigid 2D
affine: pixel `(px, py)` maps to `origin + R(rotation)·(px, py)·unitsPerPixel`.

- **Bytes live on the document, not the feature** (`Document::SetAsset/
  GetAsset`, keyed by `asset`). Undo rebuilds features from JSON, which never
  carries the bytes, so storing them on the feature would lose them; and the
  renderer decodes each asset into one texture however many images use it.
  Only referenced assets are saved: as `assets/<name>` in a directory
  project, as base64 under a root `"assets"` map in a single-file document.
- `widthPx`/`heightPx` are cached so sizing and calibration never need a
  decoder (headless builds have none).
- **Calibration** (`CalibrateImage(id, p1, p2, distance, alignAxis)`) rescales
  so two picked plane points are `distance` apart, holding `p1` fixed; an
  `alignAxis` of 0/1 also rotates the p1→p2 line onto the U/V axis.
- **What draws.** `UnderlayRenderer` draws textured quads **before** the
  sketch lines (depth test on, write off). `ui::SyncUnderlays`
  (`SketchOverlay.cpp`) hands it the list: while a sketch is edited, all of
  that sketch's visible images; and at any time, every image marked
  **`show_always`** (v166) on every sketch that is part of the model on
  screen — not suppressed (configurations included), not past the rollback
  bar. `hidden` still wins. A show-always underlay is how a model is checked
  against the plan it was traced from: it stays on screen once the sketch is
  closed, and it is in **quick stills** and motion videos, which hand the
  renderer the show-always images only (never the edited sketch's own, since
  a capture draws no sketch;
  [still-render.md#quick-mode](../rendering/still-render.md#quick-mode)).
  Path-traced stills show no underlays.
- The renderer owns persistent device-local textures, which is why it is
  separate from `SketchRenderer`; `ReleaseTextures` waits for the device to
  idle before freeing, since a frame in flight may still sample them. The
  cache is keyed by asset name, which is per document, so it is dropped when
  the active document changes.
- **One placement convention for both RPCs.** `add_reference_image` and
  `set_reference_image` take the position as `at`, the image's centre, or
  `origin`, its (0, 0) pixel corner (what is stored), never both. A turn
  (`rotation` in radians or `rotation_deg`, never both) is about the centre
  in both: a `set` that turns or rescales the image with neither keeps the
  centre where it was. Both answer with the placement as `at` and `origin`.
  Add once took only the centre and set only the corner, and the house
  generator's plan landed half an image away from where it was put.

## Operations on the entity list

All are `SketchFeature` methods; UI and RPC commits wrap them in one
`ModifyFeatureCommand` (one undo step).

- **DeleteEntity(id)** — collects the survivors directly coincident with each
  dying point and re-links them pairwise (A~B~C, delete B ⇒ A~C), drops every
  constraint that reads the dead id, and returns how many *non-trivial*
  constraints (anything but Coincident/Horizontal/Vertical) were dropped, for
  a warning. Dimensions on the dead entity are dropped, not transferred.
- **SplitEntity(lineId, P)** — piece 1 keeps the id (`a`→P), piece 2 is new
  (P→`b`); refs to the old far end move to piece 2 (so a whole-edge distance
  re-anchors to span both pieces); H/V/Parallel/Perpendicular are copied onto
  piece 2; a coincident joins the pieces. No collinear is added.
- **TrimLineAt / TrimCurveAt(id, click)** — remove the span between the
  crossings (with lines and circles) that bracket the click (via split + delete). With no bracketing
  crossing the whole entity goes. A trimmed circle becomes the complementary
  arc (keeping the id); a trimmed arc becomes up to two arcs. `TrimSpanAt`
  gives the span for the hover preview.
- **MirrorEntities(sources, axis, link)** — bake: reflected copies. Linked: a
  `Mirror` derived op.
- **FilletCorner(line1, line2, r, whyNot)** — trims both lines to the tangent
  points and inserts the arc, with two Coincident and two Tangent constraints.
  The old corner becomes a **construction point**: constraints that held the
  corner are re-pointed onto it, and a point-on-line from each line to it
  keeps both lines aimed as before. Only a slot that names a POINT is
  re-pointed: a line's corner end and the whole line are both `{line, 0}` or
  `{line, 1}`, so a tangent, a point-on-line's curve or a one-line
  horizontal naming the line keeps it. If that web over-determines the corner, one
  constraint *added by the fillet* that is redundant (same DOF without it) is
  dropped, so a good fillet does not read as conflicting; user constraints are
  never removed. Failure reasons go to `whyNot`.
- **AddLinearPattern / AddCircularPattern(..., link)** — bake or a derived op.
  The circular angle is the per-step angle.
- **OffsetChain(chain, distance, joinArc)** — offsets one closed loop through
  OCC `BRepOffsetAPI_MakeOffset` (arc or intersection joins) and converts the
  result back to entities. Always baked.

## Solving

`sketch::Solver` (`SketchSolver.h`) loads a sketch into PlaneGCS, solves with
DogLeg, and writes positions back. `SolveResult` carries `dof` and the
conflicting/redundant constraints mapped back to **indices into
`constraints`** (each constraint records the tags it used; the arc rules use
tag 0 so they are never reported as a user constraint).
`UnderDefinedEntities()` maps PlaneGCS's dependent-parameter diagnosis back to
entity ids.

Held out of the unknowns (fixed): datum geometry, `Fixed` points, linked-copy
outputs, external geometry, and expression-driven coordinates.

**A value edit never turns the sketch over.** A sketch has more than one
solution for most values -- a triangle and its mirror image, an arc and its
complement -- and one solve lands in whichever basin is nearest. The side
guards in `Load` hold what a constraint can see (line–circle tangency and
`DistancePL` pin their side at load, a tangent at a shared end keeps 0 or π,
circle–circle tangency keeps inside/outside, `DistanceX/Y` and `Angle` are
signed). What no constraint sees is listed by `FindOrientations`
(`SketchOrientation.h`): every triangle of `Distance` edges between corners
(the points `Coincident` joins; two held corners count as an edge) with a
free corner, its sign the cross product of its corners; and every arc the
solver moves, its sign which side of its chord the centre is on (sweep
against π). Capped at 256 triangles and a work limit, so a large sketch
pays nothing noticeable. An orientation counts only where it is clear: a
triangle's height over 1/1000 of its longest side, an arc's sweep more than
1e-3 rad from π.

`Solver::SolveValueEdit(changes)` runs after `Load` on the sketch carrying
the new values, seeded with the pre-edit geometry; each `ValueChange` is a
constraint's old value (`MeasureDimension` on the seed: what the geometry
measures, so a ref swap or a reflected seed reads as the flip it is) and
its new one. It solves in one jump; if that solves and no clear orientation
turned, that is the answer (one solve plus the check). Otherwise it
**continues**: the changed dimensions go back to their old values and step
toward the new ones in sixty-fourths, starting half-way; a step that solves
and keeps every orientation clear where it started is taken and the next
doubles, one that does not is undone and halves, down to a sixty-fourth,
within 160 solves. Steps compare with the state they start from, so a sweep
that passes 180° continuously is accepted (near π it is not clear); a
triangle cannot be carried across, because side lengths stepped linearly
stay inside the triangle inequalities and never pass a flat one. An angle
steps in its stored signed value from the turn nearest its new one. A
refused edit leaves the solver at the pre-edit geometry with `refusal` and
`flipped` (the constraints of the orientation that turned) set. Each step
re-solves without reloading: PlaneGCS starts every solve from the reference
`initSolution` stored, so a taken step re-stores it and an undone one goes
back to it. The cost is small beside the load's own diagnosis.

Every value edit goes through it: the GUI inline editor
(`ApplyDimensionEdit`, `SketchDimensionDraw.cpp`), `modify_feature`'s
re-solve of a sketch whose dimension values changed (`RemoteModeling.cpp`),
and `SketchFeature::Execute`'s re-solve of expression-driven dimensions,
whose old values are what the stored geometry measures. Expression-driven
coordinates are written into the entities before `Execute`, so their edit
is the one jump -- still checked. `add_constraint(s)` is not a value edit
(the geometry was never solved for the new constraint) and keeps its single
solve. PlaneGCS's own numerical reach is the limit: an arc on a 100 mm chord
converges up to a radius of about 1e6 and not at 1e7, in one jump or in
steps, so 1e7 is refused as not met.

**Dragging** builds the solver once per drag: `BeginDrag(points)` (or
`BeginRadiusDrag(circleId)`) pins the dragged points to targets with a
temporary coincident **down-weighted by `rescaleConstraint(…, 0.01)`**, so real
constraints dominate and a heavily constrained sketch does not oscillate; each
frame `SetDragTargets`/`SetRadiusTarget` then `SolveDrag` — no rebuild.
`Begin*` returns false when the sketch is already conflicting and the UI
refuses the drag. Every frame solves from the drag's starting geometry (the
reference PlaneGCS stored at `Begin*`), not from the last frame. `SolveDrag`
checks each frame's result against the last frame's orientations; on a flip
it steps the target from the last frame's toward the cursor, halving as a
value edit does, and keeps the furthest step that holds, so the sketch stops
short of mirroring and follows again when the cursor comes back
(`DragGuard::None` is the unguarded solve, for tests). UI modes (`SketchDragSession::Mode`): `Point`, `LineBody`
(both endpoints, rigid delta), `CircleRim` (radius), and `SketchTranslate`
(dragging a line that carries a driving dimension moves the whole sketch
instead of fighting the dimension).

`SketchFeature::Execute` re-solves only when a dimension or coordinate is
expression-driven or external geometry exists (after `VisitExprs` has
refreshed the values), as a value edit (above). A refused one keeps the last
good positions and sets the sketch's warning rather than failing the
history.

## Profiles

`BuildLoops` (`SketchProfile.cpp`) turns non-construction entities into closed
`Loop`s: circles and ellipses are loops on their own; lines, arcs, elliptical
arcs and splines chain by endpoint proximity (1e-4) into ordered `LoopEdge`
lists; open chains are discarded with a warning. An outline text label adds
each glyph contour as a loop of its own, never chained with other geometry;
single-stroke text is skipped silently (an extrude or revolve whose region
picks it, or whose sketch has nothing else, is refused naming the label and
its font). Every wire goes through `ShapeFix_Wire` (reorder, reconnect,
close) before face building. Loops are grouped by **containment depth**
(`ClassifyLoops`): a loop inside an even number of others is an outer
boundary, inside an odd number a hole of its innermost container, so a
letter's counter is a hole and an island inside a hole is an outer again.
Partially overlapping loops are refused. Side faces are named
`<feature>/g<entityId>` after the generating entity; a text segment's face is
`<feature>/g<textId>.<glyph>.<contour>.<seg>`, the glyph counted over
rendered characters only, so a space renumbers nothing
([naming](../modeling/toponaming.md)).

A **region** is referenced by the sorted entity ids of its outer loop
(`LoopEntityIds`). `SiblingRegions` expands a picked loop that belongs to a
linked pattern or mirror to every member, since a cut through one hole of a
bolt circle but not its copies is almost never the intent. A label is one
entity, so every glyph's loop has the same id set `{textId}`, and picking any
letter picks the whole label (`FilterLoopsToRegions` keeps every loop that
matches a region). CAM likewise targets the entity (`g<textId>`), which
survives editing the string.
`BuildOpenPathWire` gives open chains for sweep paths.

## GUI

Sketch editing is a mode of the part tab. `SketchEditState` (`AppState.h`)
holds the session; a new session starts in **Select** (a Hole sketch starts
in Point). While a sketch is edited, the document's view rollback is set at
the sketch's first consumer so solids built from it are out of the way
(`SyncSketchViewRollback`, `ProjectActions.cpp`) — transient, never saved,
never dirties the document.

**Tools** (`SketchTool`): Select, Point, Line, Circle, Rectangle,
RectangleCenter, Arc (centre→start→end), ArcThreePoint (start→end→through),
Ellipse (centre→major end→minor), EllipseArc (5 clicks), Spline (click poles,
Enter/double-click finishes), Trim, Mirror, Fillet, Pattern, Offset,
Dimension, Project (live external geometry), CalibrateImage, Image, Text
(each click places a label; the Tool window edits its text, cap height,
font and alignment, and a label selected when the tool is armed is edited
instead. The default cap height is five cells of the sketch's grid, which is
sized from the sketch and the part — never a literal).

**Default keys** (sketch context; the global binding of the same chord is
shadowed while sketching; rebindable — `src/core/io/Keymap.cpp`):

| draw | modify | constrain | other |
|---|---|---|---|
| L line, C circle, Shift+C ellipse, G rectangle, Shift+G centre rectangle, A 3-point arc, Shift+A centre arc, Ctrl+Shift+A elliptic arc, S spline, `.` point, Shift+T text, X project, Shift+I reference image | M trim, O offset, Shift+F fillet, Shift+M mirror, P pattern, D dimension, Q construction, Delete delete | H horizontal, V vertical, N coincident, B parallel, Shift+B perpendicular, T tangent, E equal, Shift+N concentric | Space clear selection, Ctrl+C/V/Shift+V copy/paste/paste linked, Shift+S auto-constrain, Y constraint glyphs, F7 slice, Shift+E extrude, Shift+R revolve, Shift+H holes |

Point-on-line, symmetric, midpoint, fixed and collinear are commands with no
default key.

**Escape** backs out one level per press: an open dimension field → dimension
placement → spline poles → pending clicks or a box → mirror axis phase →
op re-pick → op edit session → dimension targets → the tool (back to Select)
→ the selected constraint → the entity selection → a Hole session → leave the
sketch.

**Selection**: click picks the nearest endpoint first, then an entity body;
Ctrl adds; repeated clicks on one spot cycle candidates. A drag from empty
space box-selects (`sketch::BoxSelect`): left→right is window (entity wholly
inside), right→left is crossing (any sample inside or any edge through the
box); Ctrl adds. Datum hits rank below real geometry.

**Auto-constraints while drawing** (`sketch::InferPoint`, on by default,
Shift+S toggles, Ctrl suppresses for one click): the point snaps and, on
commit, gains the implied constraint — coincident (10 px, including the
origin), point-on-line (8 px), X/Y alignment with another vertex or the origin
(8 px, a two-point Vertical/Horizontal, drawn as a dashed guide), and for a
line segment horizontal/vertical or parallel/perpendicular to a nearby line
(2°). Geometry is placed at the snapped position first so the solve starts
satisfied; an inferred constraint that `JudgeAddition` refuses (it makes the
sketch conflict, repeats what it says, or removes no DOF) is dropped.

**Status and colour** (`UpdateSketchStatus`, `BuildActiveSketchPrimitives`):
blue = under-defined, green = fully defined entity (all neutral when
`dof == 0`), violet = solver-fixed (linked copy, expression-driven), red =
touched by a conflicting/redundant constraint, dashed grey = construction.
A prompt pill shows "Fully defined" or the DOF count plus the active tool's
step (`SketchToolPrompt`: "Name (Step k/n): …"). Constraint glyphs (Y) mark
committed constraints; the Model panel's Constraints section lists them,
flags conflicts, deletes one, and switches a dimension between driving and
driven (its **Driven** checkbox; a driven row reads "(distance)", muted).

**Dimensions**: the Dimension tool accumulates targets and infers the kind;
while placing, the cursor zone picks horizontal/vertical/aligned. Typing a
digit right after drawing opens a dimension on the new entity
(`DrawDimensionOnCreate`). Double-click a label to edit it inline
(`ApplyDimensionEdit`); anything that is not a plain number is an expression,
with variable completion; angles are typed in degrees; a negative value flips
direction. Labels drag to reposition (`labelOffset`).

**Driven dimensions in the GUI.** Dimensioning geometry the sketch already
fixes reads it, as in every CAD tool: the Dimension tool
(`CommitInferredDimension`, `OverConstrained::AddDriven`) adds a dimension
that would over-constrain the sketch as a driven one, with the status "That
dimension would over-constrain the sketch, so it was added as a driven
(reference) dimension", and opens no inline editor. Its label reads the
measure in parentheses, "(40)", in `DrivenDimensionColor` (`Theme.h`, the
dimension colour muted), and is never painted as conflicting.
Double-clicking it opens nothing and says "A driven dimension measures the
sketch; make it driving to edit it". A dimension typed right after drawing
carries a value, which a driven one cannot, so that one is refused instead
("Not added: the sketch already fixes that (constraint 3)"). Right-clicking
a label (a click that does not orbit the view) opens its menu, whose
**Driven** item — like the Model panel's checkbox — toggles it
(`SetConstraintDriven`, one undo step): made driving, it takes its measured
value and goes through `JudgeAddition`, so one that would over-constrain the
sketch stays driven with "Still driven: …"; made driven, it cannot
over-constrain anything (a dimension driven by an expression is refused:
clear the expression first).

**Generative tools** open a session in the floating Tool window
(`DrawSketchToolWindow`): Mirror collects geometry then an axis (a line,
construction line, or X/Y datum axis) with a live preview; Pattern and Offset
show a ghost preview with their parameters; the Linked toggle chooses bake or
linked. The Model panel's **Operations** section (`DrawSketchModelSections`)
lists images, patterns, mirrors and linked copies; double-click opens the op's
edit session (pattern parameters, sources/axis re-pick, Break link for a
linked copy).

**Reference images**: File > Reference Image… or Shift+I opens a file dialog
and inserts one. Its session (Tool window) has opacity, hide, Show always,
Calibrate… and Delete; the image drags to move. Opacity, hide and show-always
are persisted fields and are undoable — the opacity baseline is taken when the slider is grabbed and
one command pushed on release, never one per frame. Calibration picks two
points (markers show them and the span), then takes the real distance and an
optional axis alignment.

## RPCs

In `src/remote/RemoteModeling.cpp` (`describe` lists `sketch_entity_types`,
`constraint_kinds`, `constraint_value_units`):

| method | params | notes |
|---|---|---|
| `add_feature` | `feature` | a Sketch with `plane`, `entities`, `constraints`; returns `entity_ids` |
| `modify_feature` | `feature, feature_id \| index` | replaces the whole sketch JSON; changed dimension values are re-solved from the sent geometry, and a value that cannot be met or would flip the sketch is refused ([Refusals](#refusals)); a driven dimension's value is ignored, and one switched to driving is judged like an addition and refused when it would over-constrain the sketch |
| `add_constraint` | `sketch_id, kind, refs, value \| value_deg \| expr \| driven` | `refs` = `[[entity_id, which], …]` (a, b, c); `value` is mm, or **radians** for an angle (`value_deg` instead, never both; more than a full turn is refused as degrees); `expr` is in base units, angles in degrees. A constraint that constrains nothing is refused ([Refusals](#refusals)). `driven: true` on a dimension kind, with none of `value`/`value_deg`/`expr`, adds a [driven dimension](#constraints) and answers `measured` (mm; radians for an angle). Every answer carries the sketch's `dof` after the add. One solve, one undo step |
| `add_constraints` | `sketch_id, constraints[]` | many at once, each spec as `add_constraint`'s: one judgement, solve, regen, undo step; a refused spec adds nothing and is named by index; `measured` lists each spec's measure (null for a driving one) when any is driven |
| `delete_entity` | `sketch_id, entity_id` | with the constraint cleanup above |
| `fillet` | `sketch_id, line1, line2, radius` | returns `arc_id`; failures say why |
| `offset` | `sketch_id, chain, distance, join_arc?` | sign of `distance` picks the side |
| `mirror` | `sketch_id, sources, axis, link?` | `axis` may be a datum axis id |
| `pattern` | `sketch_id, sources, count, link?` and `step` or `circular, center, angle \| angle_deg` | circular angle is **per step** |
| `project_geometry` | `sketch_id, feature_id, element, kind` | live external geometry; returns `gen_ids` |
| `add_reference_image` | `sketch_id, path, at? \| origin?, opacity?, units_per_pixel?, rotation \| rotation_deg, show_always?` | returns `image_id`, `at`, `origin`; turned about its centre |
| `set_reference_image` | `sketch_id, image_id, opacity?, hidden?, show_always?, at? \| origin?, units_per_pixel?, rotation \| rotation_deg` | returns `at`, `origin`; a turn keeps the centre |
| `calibrate_reference_image` | `sketch_id, image_id, p1, p2, distance, align?` | `distance` in sketch mm |
| `delete_reference_image` | `sketch_id, image_id` | |

In entity JSON an arc's angles are `start_angle`/`end_angle` (radians) or the
`_deg` pair; `ellipse` takes `rotation` or `rotation_deg`; an `ellipse_arc`'s
`start_param`/`end_param` are ellipse parameters, not angles, so they have no
`_deg` twin. An arc sweeping more than a full turn is refused (almost always
degrees in a radian field). A `text` takes `angle` or `angle_deg`, and
refuses an unknown `font`, `halign` or `valign` naming the accepted words. Any entity may carry `construction` and
`<field>_expr`.

There is no RPC to delete a single constraint, trim, split, paste or break a
link; `modify_feature` with an edited sketch JSON covers them.

## Tests

- `tests/test_sketch_solver.cpp` `[solver]` `[arc]` `[drag]` `[delete]`
  `[flip]` — constraint kinds, arcs, datum references, drag sessions,
  conflict mapping, delete-time coincidence repair; value edits that one
  jump flips or fails (an arc's radius to 5000 and 1e6, a triangle's sides
  10000 → 51, an unreachable 49), each first shown flipping or failing
  without the guard, a sweep passing 180° accepted, one solve when nothing
  can turn, and a drag that would mirror a hinged triangle. `[driven]`: a
  driven distance on a fully defined triangle leaves DOF 0 and reads the new
  measure after another dimension changes it; `JudgeAddition` refuses a
  repeated horizontal and a contradicting distance, naming what they run
  into, judges a driven-to-driving switch, and accepts an addition to an
  over-constrained sketch that touches none of its broken constraints.
- `tests/sketch_constraint_refusal_smoke.py` — over RPC, on a fully defined
  triangle: a repeated `distance_x` and a contradicting `distance` are
  refused with the sketch unchanged; the same `distance_x` with `driven:
  true` is added and answers `measured`; `driven` with an input or on a
  relation is refused; `add_constraints` with one bad spec in three adds
  nothing and names index 1; `modify_feature` switching a driven dimension
  to driving where it would over-constrain is refused; and a dimension a
  configuration drives through its variable cannot be made driven with the
  expression kept. `tests/test_literals.cpp` `[driven]`: a driven dimension
  is no literal and takes no variable.
- `tests/regen_honesty_smoke.py` — a variable driving a radius out of
  reach warns in `set_variable`'s `regen`, naming the sketch and the
  dimension; `modify_feature` to 5000 keeps the minor arc, to 49 is
  refused with the document unchanged.
- `tests/test_sketch_feature.cpp` `[sketch]` `[ids]` `[io]` `[pattern]`
  `[undo]` — id stability, split/trim, mirror, fillet, pattern replay and id
  stability, linked copies (sync, break link, vanished source), legacy-rect
  migration, serialization (`[driven]`: a driven dimension round-trips
  written as its measure; `driven` on a horizontal, with an `expr`, or not
  a boolean is refused at load). Offset profiles are in
  `tests/test_extrude.cpp`.
- `tests/test_sketch_inference.cpp` `[inference]` `[datum]`,
  `tests/test_sketch_selection.cpp` `[selection]` — the pure passes.
- `tests/test_spline.cpp` `[spline]` `[profile]` — evaluator vs OCC, profiles.
- `tests/test_reference_image.cpp` — placement, calibration, asset storage in
  both formats, regen-hash exclusion, `show_always` round trip.
- `tests/underlay_show_always_smoke.py` — a show-always underlay in a quick
  still with no sketch in edit, by pixel colour; none without the flag;
  `rotation_deg` on add turns it about its centre; `at` and `origin` name
  one placement in add and set, and a turn on set keeps the centre.
- `tests/test_sketch_text.cpp` `[text]` — cap height, advance and kerning
  against the fonts' own numbers, alignment, save/reload, refusals, every
  ASCII glyph of the outline font as a profile, a cut label's face names,
  stroke text in profiles, a dimensioned box corner.
- GUI: `src/app/guitests/GuiTestsSketch.cpp` (`sketch/*`, `constraints/*`,
  `trim/*`, `mirror/*`, `infer/*`, `drag/*`, `region/*`, `dims/*` (among
  them `dims/edit_refuses_flip`, `dims/over_constraining_becomes_driven`,
  `dims/toggle_driven`; and `constraints/repeat_is_refused`), reference
  image panel and drag), `GuiTestsViewport.cpp` (`pick/*`, `select/*`),
  `GuiTestsShell.cpp` (`keys/*`), `GuiTestsModeling.cpp` (`fillet/*`,
  `pattern/*`).
- Goldens: `tests/golden/v03-legacy-sketch` (index refs and rigid rects
  migrate), `v35-linked-copy`, `v38-entity-exprs`, `v40-spline`,
  `v46-ellipse-arc`, `v49-reference-image`, `v50-linear-pattern-2d`,
  `v51-mirror-and-center-ref`, `v137-text`, `v155-ellipse-axis-ends`,
  `v166-underlay-show-always`, `vNEXT-driven-dimension`.
- `tests/test_sketch_curves.cpp` `[ellipse]` `[spline]` `[refusal]` —
  ellipses, elliptical arcs and splines solved: DOF, radii by axis-end
  distance through an extrude and a revolve, tangency checked as directions
  at the joint, points on the curves, drags, bit-identical re-solves (also
  across heap layouts and threads, `[parallel]`), and the refusals.

## Not built

- Tangency to a spline away from its ends, or between an ellipse and a
  circle or another ellipse without a shared end; an ellipse's radii as
  expression-driven entity fields (drive the distance to an axis end).
- Spline weights and knots as unknowns (rational or non-uniform splines);
  continuity at a closed spline's seam beyond C0.
- Linked copies across parts: a cross-part paste is always a plain copy.
- Offset as a regenerating op (it bakes).
- Transferring a dimension from a deleted entity onto a coincident survivor.
- Extend tool, numeric move/rotate/scale of a selection, slot and polygon
  primitives.
- Text: multi-line labels, text on a path, shaping (ligatures, complex
  scripts), fonts beyond the two bundled ones.
