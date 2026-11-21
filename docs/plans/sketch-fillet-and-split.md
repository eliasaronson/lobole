# Sketch fillet on any corner, and a Split tool

A sketch fillet should round any corner two curves make at a shared end
(lines, arcs, elliptical arcs and splines), and the corner the user points
at when more than two meet there. A curve should split in two where it is
clicked, and the two pieces should stay one curve until one is changed.

## The problem

- **Lines only.** `SketchFeature::FilletCorner` refuses anything but two
  `Line`s, so a slot end, a line running into an arc or a traced outline
  with spline sides cannot be rounded.
- **The first two found.** The Fillet tool collects every line with an end at
  the clicked vertex and fillets `lines[0]` and `lines[1]`. Where a spline, a
  vertical line and two collinear horizontal lines meet, it can take the
  collinear pair and refuse a corner nobody meant.
- **A refusal that does not say why.** Every failure reads "radius 0.500000
  doesn't fit this corner", whatever `whyNot` said. A click finding fewer
  than two lines repeats the prompt pill's sentence as its warning, and the
  tool window prints the same sentence a third time.
- **A literal default.** `filletRadius = 0.5` (src/ui/AppState.h) is a
  length that knows nothing of the sketch.
- **A corner that repeats itself.** Where three ends are joined pairwise,
  re-pointing both filleted ends onto the corner point makes two coincidents
  one and the same, which reads as redundant.
- **No Split.** `SplitEntity` takes lines only, adds no collinear, and has no
  tool or RPC. It moves constraints by ref alone: a midpoint or an equal
  length on the line then holds only the first piece, and a tangent naming
  `{line, 1}` moves to piece 2 whether it meant the end or the line.
- **Trimming an arc's middle loses its far end.** `TrimCurveAt` rebuilds the
  survivors with no transfer: refs to the arc's end stay on the first
  survivor, whose end has moved, and the second joins nothing.

## The shape of the fix

### 1. Which corner

`sketch::CornerAt(sketch, vertex, cursor)` (pure) lists the curve ends at a
vertex (a line's 0/1, an arc's or elliptical arc's 1/2, an open spline's end
poles), each with the direction its curve leaves in, sorted by angle. It
returns the two that bound the sector holding the cursor. The tool takes a
click inside a corner, not only on its vertex. Refused, naming why: one curve
ends there; the cursor is on a vertex where more than two end ("move into the
corner to round"); the sector is wider than half a turn. Two curves picked on
their bodies in the tool are filleted at their shared end nearest the click.
Two that share no end are refused. Entities a linked op generates or a
projection owns are refused by name, in the core as Trim refuses them.

### 2. The fillet of two curves

`FilletCorner(a, b, radius, near, whyNot)` takes any two of line, arc,
elliptical arc and spline that share an end. `near` picks the end when they
share two (an arc closing a line). The centre is where the curves' offsets by
the radius toward the inside of the corner meet nearest it. For a line or an
arc this is closed form. For an elliptical arc or a spline it is Newton on
the two curve parameters, seeded by the fillet of the two end tangents. Each
tangent point must lie on its curve, with no stretch between it and the
corner bending tighter than the radius. A radius that fails is refused naming
the largest that fits, found by bisection on the same construction: "4 mm
does not fit this corner; 2.83 mm is the largest that does". The fillet arc
is the one from T1 to T2 that faces the corner.

Each curve is trimmed to its tangent point: a line end, an arc's angle, an
elliptical arc's parameter, a spline as in §4. The corner keeps today's
construction point. Constraints on the corner ends are re-pointed onto it,
and it is held on each line's line, each arc's circle and each elliptical
arc's ellipse. A spline gets no such lock, since nothing of it runs past the
tangent point. The arc joins each curve with Coincident and Tangent (the
shared-end angle form, which the solver reads for every such pair). A
re-pointed coincident that repeats another is dropped, as self-coincidents
are, and the add-then-prune pass stays. Everything is computed and checked
before the first entity changes, so a refusal changes nothing.

### 3. The Fillet tool

The prompt pill alone says what to do: "click inside a corner (radius 5 mm)".
A refusal says only why, prefixed "Not filleted:", with the radius through
`FormatLength`. The tool window keeps the radius field and its note. Hovering
shows the arc the click would make and lights the two curves. A radius of 0
means "not chosen": arming the tool sets it to one cell of the sketch's grid
(`SketchGridFor`), as Text takes its cap height from five, and a typed radius
stays for the session.

### 4. A spline cut exactly

The sketch stores a spline as clamped-uniform poles, so a piece that starts
mid-span is not one storable spline. Knot insertion at the cut u in span
[k, k+1] changes no point of the curve. On each side it gives a Bézier for
the partial span, plus a clamped-uniform spline for the whole spans past the
next knot, once that knot is raised to full multiplicity. Both are exact
`BSpline`s, joined Coincident and Curvature (G2), or Tangent at degree 1. A
side inside one span is one Bézier, and the largest piece keeps the id. The
insertion moves into `src/core/sketch/SketchSpline.{h,cpp}`, and the DXF
reader's `SplineToSketch` uses it.

A pole the insertion leaves in place keeps its constraints, re-indexed onto
its piece. A constraint on a pole it replaces is dropped. The reply's
`dropped` and the pill name each one ("distance (constraint 6) on pole 4 of
spline 9"). The pieces carry more poles than they replace, and the pill's
DOF says by how much. No format change is needed: the results are entities
and constraint kinds the file already holds.

### 5. Split

`SplitEntity(id, at)` cuts a curve into pieces that stay one curve:

- a line: piece 2 runs at→b, joined Coincident plus Parallel to piece 1,
  or the H/V the line carried, so the pieces are collinear;
- an arc: two arcs joined Coincident and Concentric (the radius follows); a
  circle takes two cuts, joined the same way at both;
- an elliptical arc, or an ellipse in two cuts: Concentric, Equal and
  Coincident;
- a spline: cut as in §4, with G2 at the cut; a closed spline takes one cut,
  since its seam is the other.

A constraint on an end goes with the piece that holds the end, and so does a
Tangent or Curvature joined there. One on the whole curve (its direction, its
circle, symmetry about it, a point on a line or arc) stays on piece 1, and
the joints hold piece 2 to it. A point on a spline goes to the piece it lies
on. A whole-edge distance spans both pieces, as today, and a midpoint of the
line becomes a symmetric of its outer ends. An equal length on the line or
an `arc_length` on the arc has no two-piece form and is refused by name. At a
crossing, the joint is put on the crossing curve, so the DOF does not change.
At another curve's end, the joint becomes coincident with that end, replacing
the point-on-curve that held it. Anywhere else the joint slides (one DOF).
Trim keeps its own transfer, so its survivors are not left parallel.
`TrimCurveAt` cuts an arc through this split, so the far end keeps its
constraints.

### 6. The Split tool

`SketchTool::Split`, command `sketch.split` "Split a curve" (Modify),
`###tb_sk_split` after Trim, default key `K`. Its icon is `ICON_DS_SPLIT`,
part Split's glyph, so the two go into one same-idea group in
`test_conventions.cpp`. Hover snaps to a crossing with any curve (sampled,
then refined) within the coincident snap's 10 px, then to another curve's
end on it, else to the nearest point. It previews a cut mark and the two
pieces in two shades. Each click cuts, with one solve and one undo step, and
a tap on empty space or Enter finishes, as with Trim. A closed curve takes two
clicks (Step 1/2, 2/2). Refusals name why: a generated entity, a point or a
label, a cut at an end, or the constraints above.

### 7. RPCs

- `fillet {sketch_id, entity1, entity2, radius (mm), near?}` replaces
  `line1/line2` (and the rows sharing its branch). `near` is [u, v] in sketch
  mm. Without it, two curves sharing two ends are refused, naming both. It
  returns `arc_id`, `replaced` (each cut spline and its pieces) and
  `dropped`.
- `split {sketch_id, entity_id, at, crossing?}`. `at` is one [u, v] in
  sketch mm, or two for a closed curve. With `crossing`, the cut is that
  curve's crossing nearest `at` and the joint goes on it. Otherwise `at`
  moves to the nearest point of the curve, and is refused past a hundredth of
  the curve's length. It returns `entity_ids` in order along the curve, the
  first keeping the id, plus `dropped`, `dof`, `regen` and `digest`.

## Stages

Each lands with the suites green.

- **A.** Fillet of lines and arcs: §1–3, the `fillet` RPC.
- **B.** Split of lines, arcs and circles: §5–6, `split`, the Trim fix.
- **C.** Splines and elliptical arcs: §4, then in Fillet and Split.

## Not in this plan

A fillet between curves that share no end (crossing lines, two circles),
sketch chamfers, and Trim at crossings with ellipses and splines.

## Files

- `src/core/features/SketchFeature.{h,cpp}` (`FilletCorner`, `SplitEntity`,
  `TrimCurveAt`); new `src/core/sketch/SketchFillet.{h,cpp}` (`CornerAt`,
  the tangent construction) and `SketchSpline.{h,cpp}`;
  `src/core/io/SketchImport.cpp`; `CMakeLists.txt`
- `src/ui/SketchInteraction.cpp`, `SketchToolOverlay.cpp`,
  `SketchToolbar.cpp`, `SketchOverlay.cpp`, `AppState.h`, `Toolbar.cpp`,
  `Commands.cpp`, `Icons.cpp`; `src/core/io/Keymap.cpp`
- `src/remote/RemoteModeling.cpp`, `api/rpc-surface.json`
- `tests/test_sketch_feature.cpp`, `test_sketch_solver.cpp`,
  `test_sketch_curves.cpp`, `test_keymap.cpp`, `test_conventions.cpp`;
  `src/app/guitests/GuiTestsModeling.cpp` (`fillet/*`), `GuiTestsSketch.cpp`
  (`split/*`), `GuiTestsShell.cpp`; `tests/remote_smoke.py`,
  `mcp_smoke.py`, `headless_smoke.py`, `refused_calls_smoke.py`,
  `tests/reference_parts/07-rounded-rectangle/case.json`
- docs/sketching/README.md, docs/ui/tool-sessions.md,
  docs/ui/keybindings.md

## Verified by

- **Curve pairs.** Line–arc, arc–arc (convex and concave) and, in C,
  line–spline, spline–spline and line–elliptical arc corners: each tangent
  point on its curve within 1e-9 mm, the arc tangent to both, the sketch
  solving clean at the DOF predicted, extruding. A radius too large is
  refused with the largest that fits, and that radius succeeds.
- **The owner's corner.** A spline, a vertical line and two collinear
  horizontal lines at one point: a click in each sector fillets the curves
  either side of it; on the vertex it is refused; the other two curves stay
  at the corner and no constraint reads redundant.
- **Exact spline cuts.** The pieces of a cut spline lie on the original at
  every sample (1e-12 relative); constraints on untouched poles survive and
  each replaced one is named in `dropped`.
- **Split.** A line, an arc, a circle, an ellipse and a spline, each split at
  a crossing and at a free point: the geometry unchanged after the solve, DOF
  unchanged at a crossing and one higher elsewhere, and making either piece
  construction leaves the other where it was. A midpoint carried, an equal
  length refused. A trimmed arc's far end keeps its coincident.
- **Messages.** The refusal never repeats the prompt
  (`SketchToolPrompt` test), and the radius reads through `FormatLength`.
- **Default.** The armed radius tracks the grid on a 2 mm and a 300 mm
  sketch, and a click on its corner fillets it.
- **The bar.** `toolbar/every_tool_is_reachable` covers the sketch bar too
  (the row fits, every `###tb_sk_*` exists); `no_duplicate_glyph` holds; the
  keymap has no conflict.
- **Through the RPCs.** `fillet` and `split` in `remote_smoke.py`, each
  refusal changing nothing in `refused_calls_smoke`, and `rpc_units_gate`.
