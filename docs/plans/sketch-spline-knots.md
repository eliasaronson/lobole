# A sketch spline that carries its own knots

A spline cut in two should be two splines that still move as the one curve
they were, and a dimension on it should survive the cut wherever the thing
it measured still exists.

## The problem

A sketch spline stores poles and a degree; its knots are implied, clamped
and uniform (`BSplineKnots`, src/core/features/SketchFeature.h). The exact
cut (`sketch::CutSpline`) therefore cannot store "the left part" as one
spline: it raises three knots to full multiplicity and stores a uniform
spline, a Bézier, a Bézier and a uniform spline.

- **One click makes four entities.** A cubic cut mid-span gains 7 poles,
  three joints of Coincident + Curvature, and 8 degrees of freedom.
- **The pieces are not the curve they were.** A joint is G2: the two sides
  agree in direction and curvature, not in speed. Dragging a pole beside a
  joint bends the curve in ways the uncut spline could not.
- **More is dropped than need be.** Every pole replaced by the three raised
  knots loses its constraints, dimensions and expressions; only the poles
  of the cut's own span have to go.
- **A fillet on a spline** leaves two pieces of it where one would do.
- **A DXF spline with non-uniform knots** is imported as one loose Bézier
  per span, with nothing joining them (`SplineToSketch`).
- **The Dimension tool on a spline's body** draws its ghost from the last
  pole and then fails with "[id, 255] is no point of a spline".

## The shape of the fix

### 1. Knots on the entity

`sketch::BSpline` gains `knots`: the full, non-decreasing vector of
`poles + degree + 1` values, clamped at both ends, interior multiplicity at
most the degree. Empty means clamped-uniform, as today, so every existing
file, golden and generator reads and writes unchanged; the writer omits
`knots` when they are clamped-uniform. The field goes last in the struct
(positional initialisers exist).

One accessor, `SplineKnots(b)`, returns the stored or the implied vector,
and `SplineDomain(b)` its first and last value. Nothing else derives knots
or a domain: every `BSplineKnots(n, p)` and `n - p` outside those two is
replaced, and a convention test keeps a new one from arriving.

Weights are not added. A rational spline stays refused on import.

### 2. A cut gives two pieces

`CutSpline` inserts the cut's parameter to multiplicity `degree` and
divides the poles there: two splines, each with the parent's knots on its
side (not renormalised). A cut on an existing knot inserts fewer. The
longer piece keeps the id; `source` still says which pole each new pole
was.

### 3. The joint holds the curve

A new constraint, **Smooth** (`ConstraintKind::SplineJoint`, badge `C2`),
between the meeting ends of two splines of the same degree: the first and
second derivatives agree at the joint, each scaled by its side's knot
spacing. It is linear in the poles and is written on our own
`GCS::Constraint`, beside `ConstraintEndCurvature`.

With Coincident + Smooth, the two pieces of a cubic have exactly the
freedom of the uncut spline with one knot inserted: 2 more degrees of
freedom than before the cut, against 8 today. Deleting the Smooth badge
frees the joint, as deleting any constraint does; Tangent or Curvature can
be put there instead.

A cut where the spline was only C1 or C0 (a repeated knot) gets what the
curve had: the first derivative only, or Coincident alone.

### 4. What a dimension does across a cut

The rule: **a dimension stays on the thing it measured if that thing still
exists.**

- **On an end** (the first or last pole): it follows that end to its piece.
- **On a pole away from the cut:** it follows the pole to its piece. Only
  the `degree − 1` poles of the cut's own span are replaced (two, for a
  cubic); today it is up to seven.
- **On a replaced pole:** dropped and named, as now ("distance (constraint
  6) on pole 4 of spline 9"). Before the cut, the Split tool's hover lists
  what the cut at the cursor would drop, so it is seen before the click
  and a cut a little further along can be chosen.
- **Point on the spline:** follows to the piece it lies on.
- **The new joint is a point** like any other end: Distance, X/Y and
  Coincident take it, so "cut here, 20 mm from the edge" is one cut and
  one dimension.
- **The Dimension tool on a spline's body** refuses in the tool, before a
  ghost is drawn: "a spline has no size of its own: dimension its ends, a
  pole, or a point placed on it". A spline's length and its radius at a
  point are not in this plan.

### 5. Everything that reads a spline

| consumer | change |
|---|---|
| evaluation, derivatives, nearest parameter, sampling | through `SplineKnots` / `SplineDomain` |
| the solver's binding (already hands GCS knots and multiplicities) | the stored ones |
| `EndCurvatureFactor` (already reads only the end knots) | the stored ones |
| `MakeBSplineCurve` (the profile's OpenCASCADE curve) | the stored ones |
| **profile loops and path wires that reverse a spline's poles** | a `ReverseSpline` that mirrors the knots too; a missed one is a silently wrong curve |
| `ContentHash`, `DerivedFeature`'s `HashSketch` | hash the knots; a missed one is a stale cache |
| Split, Crossings, Fillet's walk and trim | the domain helper; Fillet keeps one piece per spline |
| DXF import | a clamped spline is stored as it is; an unclamped one is clamped by insertion; cut only past 255 poles or at a full-multiplicity interior knot |
| overlay, picking, previews, CAM, mirror, pattern, paste, pole expressions, text glyphs, SVG import, section and offset fits | none |

## Stages

- **A.** Knots on the entity (§1, §5): the accessors, every consumer, the
  format (`vNEXT`), DXF import. Nothing a user does changes except that an
  imported non-uniform spline is one entity.
- **B.** The two-piece cut and Smooth (§2, §3), in Split and Fillet.
- **C.** Dimensions across a cut (§4): the hover's list of what would
  drop, and the Dimension tool's refusal on a body.

## Files

- `src/core/features/SketchFeature.{h,cpp}`, `src/core/sketch/SketchSpline.{h,cpp}`,
  `SketchSplit.cpp`, `SketchFillet.cpp`, `SketchSolver.cpp`,
  `SketchProfile.cpp`, `SketchImport.cpp`; new
  `src/core/sketch/ConstraintSplineJoint.{h,cpp}`
- `src/core/features/DerivedFeature.cpp` (the hash)
- `src/core/io/SerializeFeatures.cpp`, `SerializationInternal.h`,
  `tests/golden/vNEXT-spline-knots.json`
- `src/ui/SketchDimensions.cpp`, `SketchToolOverlay.cpp` (the hover's list),
  `SketchOverlay.cpp` (the badge)
- `src/remote/RemoteModeling.cpp` (`split`, `fillet`, `add_constraint` rows)
- `tests/test_conventions.cpp`
- docs/sketching/README.md, docs/versioning/format.md

## Verified by

- **Same curve.** For degrees 1 to 3, uniform and non-uniform, open and
  closed: a spline with stored knots evaluates, samples, and builds its
  OpenCASCADE edge on the same points to 1e-9; reversed in a profile loop
  and in a path wire it is still the same curve (the test fails with the
  knots left unmirrored).
- **Old files.** Every existing golden and `kernel_cases/selftest.json`
  round-trips byte-identical; a file with `knots` round-trips; malformed
  knots (wrong count, decreasing, unclamped, over-multiple) are refused by
  name.
- **The cut.** One cut gives two entities whose union is the original to
  1e-9; a cubic gains 2 degrees of freedom; with one pole dragged, the two
  pieces stay on the curve the uncut spline takes with the same knot
  inserted and the same drag. With Smooth deleted they part.
- **Dimensions.** A distance on an end, on a far pole and on a
  point-on-spline each survive a cut with their value; one on a pole of
  the cut's span is dropped and named, and the hover named it first; a
  distance to the new joint drives where the joint lies along the curve.
- **The Dimension tool** on a spline body refuses with its sentence and
  adds nothing (GUI test).
- **Fillet** of a line and a spline leaves one spline.
- **DXF:** a non-uniform spline imports as one entity on the file's curve;
  an unclamped one likewise; a rational one is still refused.
- **Through the pipeline:** the owner's case (a spline crossed by a line,
  split, one piece made construction) extrudes to the same volume as
  before the split; `refused_calls_smoke` and `remote_smoke` updated;
  the action matrix and kernel cases unchanged.
- An OpenCASCADE edge with a full-multiplicity interior knot (a degree-1
  polyline, which exists today) is checked through profile, extrude and
  mass properties, since that is the one case the kernel may treat
  differently from our sampler.
