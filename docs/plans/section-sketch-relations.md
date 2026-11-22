# A section into a sketch: real geometry, with the relations the scan supports

A scan cut by a sketch plane should become a sketch a designer would have
drawn: lines and arcs that meet, are level, square, tangent and equal where
the object is, at the round sizes it was made to. Each of those is offered
with the evidence for it, and none is applied past what the scan allows.

## The problem

`project_section` (docs/sketching/README.md#a-section-into-a-sketch) draws
the cut as solver-fixed construction geometry. That is right for a profile
that should follow the scan exactly, and wrong for reverse engineering a
made part:

- **The pieces are where a greedy walk put them.** `sketch::FitSection`
  takes, from each point, the longest line or three-point arc within the
  tolerance. A corner round is swallowed by the line before it, a break
  falls wherever the tolerance ran out, and a wall that wanders by more
  than a round does is cut into several lines.
- **Nothing relates the pieces.** A slot's sides are 0.2° from parallel, its
  ends 0.03 mm from equal, a boss 11.97 mm across. The designer has to see
  each of these, redraw the outline by hand over the construction geometry
  and constrain it, with no way to know whether "12" is inside the scan's
  noise or a guess.
- **A dimension has no error bar.** The reply gives one `deviation_mm` for
  the whole section.

## The shape of the fix

### 1. Pieces that fit the cut

A new fit, `fit: "pieces"`, in `sketch::FitSection`:

- **Corners first.** Each chain is split by Douglas-Peucker at the chain's
  own measured scatter (the median stand-off of a point from the line
  through its neighbours, as a robust sigma).
- **Runs are merged best first** while one line, or failing that one arc,
  explains the merged run. "Explains" is three tests: the residuals' sigma;
  the worst *short stretch* (the mean miss over a few consecutive points),
  so a long run cannot average a fillet away; and the heading at the piece's
  ends agreeing with the points there.
- **Walls wander, rounds do not.** A line is allowed several times the
  arc's tolerance, from the chain's own low-frequency scatter.
- **Breaks slide** to where the residuals of the two neighbours cross; a
  leftover shorter than a few points is given to a neighbour; each piece is
  tried as the other kind and kept as the simpler unless the other is
  better by an F-test.
- `detail` (0 to 1, default 0.5) scales the tolerances: fewer, longer pieces
  or more, shorter ones.

Each piece keeps its points' residual sigma and the uncertainty of its own
parameters (a line's angle and offset, an arc's centre and radius) from the
fit's normal equations.

### 2. Real geometry beside the section

`trace_section {sketch_id, section, detail?}` (and **Trace** in the Section
tool's To sketch) adds real, non-construction lines and arcs for the
pieces, coincident where consecutive pieces meet, over a section that stays
as it is underneath. The traced entities are ordinary sketch geometry; they
record which section and which span of it each came from
(`traced_from {section, chain, from, to}`), nothing more.

The section still follows a re-scan. The traced geometry does not move by
itself: it is the designer's drawing. `section_fit {sketch_id}` reports, for
each traced entity, how far the current cut stands from it (sigma, worst
stretch, and where), so a re-scan or a held relation that pulled a piece off
the object is seen.

### 3. Candidates, with their evidence

`sketch_candidates {sketch_id}` lists what the traced geometry nearly is.
Each row: the relation or dimension, the entities, `off` (how far the fit is
from it, in its unit), `sigmas` (that, over the fit's own uncertainty) and
for a size the `nominal`.

- **Relations:** horizontal, vertical, parallel, perpendicular, collinear,
  equal length, equal radius, concentric, tangent, on a datum plane's
  trace or the sketch's axes, symmetric about an axis.
- **Tangent, equal and a round angle are offered only between neighbours**
  (pieces that meet, or face each other across a gap): between distant
  pieces they hold by chance.
- **Sizes:** a length, distance, radius or angle near a round figure. The
  steps are 0.5 mm below 10 mm and 1 mm above, radii to 0.5 mm, angles to
  5°; a document in inches steps in its own unit. The step table is one
  table in the source.
- Nothing is offered past `max_sigmas` (default 3).

### 4. Accepting

`accept_candidates {sketch_id, max_sigmas?, max_move_mm?, only?}` adds the
candidates as ordinary constraints and dimensions, nearest (fewest sigmas)
first, solving after each:

- one that the solver cannot meet, or reports redundant, is taken back;
- one that moves any traced piece further from its section points than
  `max_move_mm` (default three times the piece's sigma, measured on the
  curve, not at an arc's centre) is taken back;
- the reply lists `accepted` and `left`, each with its reason, and
  `section_fit` after: what holding the relations cost, per piece.

It is one undo step. In the tool, the candidates are a list with their
sigmas, each row lighting its entities; rows are ticked and **Apply** runs
the same acceptance; a row the solve took back says why.

PlaneGCS has no data term, so the points do not pull on the solve. The
order of acceptance, the move limit and the fit report afterwards are what
keep the sketch on the scan; a dimension accepted at its nominal is a
driving dimension the designer can change.

### 5. Reading a round and a thread off a scan

Two readers that give a feature its size, used as the default when the tool
is opened on a scan-backed body (docs/ui/tool-sessions.md#defaults-and-refusals):

- `read_round {feature_id, point_mm}`: a section square to the edge at the
  point; the two faces as lines; a circle through the points below both,
  tangent to both. The reply is `radius_mm`, `sigma_mm`, or a verdict that
  is not a radius: `sharp`, `chamfer` (with its two distances), `no_scan`.
- `read_thread {feature_id, point_mm}`: on a fitted cylinder, radius
  against axial position in angular sectors; the pitch from the
  autocorrelation most windows of a few turns agree on; the hand from the
  phase between sectors. The reply is `pitch_mm`, `major_diameter_mm`,
  `hand`, and the nearest standard size with how far off it is.

## Stages

- **A.** The pieces fit (§1) as a `fit` of `project_section`.
- **B.** Trace and `section_fit` (§2).
- **C.** Candidates and accepting (§3, §4), the RPCs, then the tool.
- **D.** The two readers (§5).

## Files

- `src/core/sketch/SketchSection.{h,cpp}` (the pieces), new
  `src/core/sketch/SectionTrace.{h,cpp}`, `SketchCandidates.{h,cpp}`
- `src/core/features/SketchFeature.cpp` (`traced_from`, kept through
  regeneration), the sketch serialization (`vNEXT`: `fit: "pieces"`,
  `detail`, `traced_from`)
- new `src/core/mesh/ReadRound.{h,cpp}`, `ReadThread.{h,cpp}`
- `src/remote/RemoteModeling.cpp` (`project_section`, `trace_section`,
  `section_fit`, `sketch_candidates`, `accept_candidates`),
  `src/remote/RemoteInspection.cpp` (the readers)
- `src/ui/SectionToSketch.{h,cpp}`
- docs/sketching/README.md, docs/modeling/README.md,
  docs/versioning/format.md

## Verified by

Fixtures are outlines generated from known lines and arcs, tilted in the
plane, sampled with noise as a scan's cut is, and checked against the
generator's numbers.

- **Pieces.** A rounded rectangle, a slot, a keyhole and a D-shape come
  back as their own count of lines and arcs, each break within a few point
  spacings of the true tangent point, each radius within its stated
  uncertainty. A 1 mm corner round on a 60 mm side is found, not averaged
  into the side. A wall with a 0.05 mm low wave is one line.
- **Candidates.** On a slot generated 0.2° off parallel with 0.05 mm noise,
  parallel is offered at under 3 sigmas and its ends' equal radius with it;
  on one generated 3° off, parallel is not offered. Two unrelated arcs of
  equal radius at opposite ends of a part are not offered as equal. A boss
  of 11.97 mm offers 12; one of 11.7 mm with the same noise does not.
- **Accepting.** The slot ends with its relations, its fit report within
  the noise, and its dimensions at their nominals. A candidate that would
  drag a side 0.5 mm off the points is left, naming the move. A redundant
  one is left as redundant. Undo removes all of it in one step.
- **A re-scan.** The section moves, the traced sketch does not, and
  `section_fit` reports the piece that no longer lies on the cut.
- **Through the pipeline:** the traced, constrained profile is extruded and
  the body's deviation from the scan
  (`compare_scan`,
  [how far a body is from a scan](../inspect/README.md#how-far-a-body-is-from-a-scan))
  is within the tolerance.
- **The readers.** Rounds of 0.5, 2 and 6 mm between faces at 90° and at
  60° read within their sigma; a sharp edge and a chamfer are named as
  such. A scanned M8 × 1.25 and a left-hand trapezoidal thread give their
  pitch, diameter and hand; a plain bore is refused as no thread.
- `refused_calls_smoke` covers each new call's refusals changing nothing,
  and `rpc_units_gate` its keys.
