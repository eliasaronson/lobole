# A section into a sketch: real geometry, with the relations the scan supports

A scan cut by a sketch plane should become a sketch a designer would have
drawn: lines and arcs that meet, are level, square, tangent and equal where
the object is, at the round sizes it was made to. Each of those is offered
with the evidence for it, and none is applied past what the scan allows.

## The problem

A section is drawn into a sketch as solver-fixed construction
(docs/sketching/README.md#a-section-into-a-sketch); its `pieces` fit finds
the lines and arcs the cut is made of, each with its own fit and
uncertainty (docs/sketching/README.md#the-pieces-fit), and `trace_section`
draws them as real, joined geometry over it, with `section_fit` saying how
far the cut stands from each (docs/sketching/README.md#tracing-a-section).
What is left:

- **Nothing relates the traced pieces.** A slot's sides are 0.2° from
  parallel, its ends 0.03 mm from equal, a boss 11.97 mm across. The
  designer has to see each of these and constrain it, with no way to know
  whether "12" is inside the scan's noise or a guess.
- **A round's and a thread's size are read by hand.**

## The shape of the fix

What it builds on: each traced entity's `PieceFit` (its points' sigma, its
parameters' sigmas: a line's angle and offset, an arc's centre and
radius, widened by its residuals' correlation) is what an `off` is divided
by. It is computed by `sketch::FitPieces` and kept only on the section
(`ExternalGeom::pieces`, not saved) and in `trace_section`'s reply; the
candidates need it per traced entity, so they refit each traced entity's
span (`TracedFrom` records it) of the current cut, as `section_fit`
measures it (`sketch::SectionFitReport`). A small round's sigma leaves out
where its tangent points are (the docs say by how much); a candidate on
one should not be offered at a sigma it does not have -- a tangent-
constrained refit between its neighbours is the likely answer.

### 1. Candidates, with their evidence

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

### 2. Accepting

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

### 3. Reading a round and a thread off a scan

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

- **C.** Candidates and accepting (§1, §2), the RPCs, then the tool (a
  candidates list in the Section tool's Trace area, or the sketch's).
- **D.** The two readers (§3).

## Files

- new `src/core/sketch/SketchCandidates.{h,cpp}` (beside
  `SectionPieces`/`SectionTrace`)
- new `src/core/mesh/ReadRound.{h,cpp}`, `ReadThread.{h,cpp}`
- `src/remote/RemoteModeling.cpp` (`sketch_candidates`,
  `accept_candidates`), `src/remote/RemoteInspection.cpp` (the readers)
- `src/ui/SectionToSketch.{h,cpp}`
- docs/sketching/README.md, docs/modeling/README.md

## Verified by

Fixtures are outlines generated from known lines and arcs, tilted in the
plane, sampled with noise as a scan's cut is, and checked against the
generator's numbers.

- **Candidates.** On a slot generated 0.2° off parallel with 0.05 mm noise,
  parallel is offered at under 3 sigmas and its ends' equal radius with it;
  on one generated 3° off, parallel is not offered. Two unrelated arcs of
  equal radius at opposite ends of a part are not offered as equal. A boss
  of 11.97 mm offers 12; one of 11.7 mm with the same noise does not.
- **Accepting.** The slot ends with its relations, its fit report within
  the noise, and its dimensions at their nominals. A candidate that would
  drag a side 0.5 mm off the points is left, naming the move. A redundant
  one is left as redundant. Undo removes all of it in one step.
- **Through the pipeline:** the traced, constrained profile is extruded and
  the body's deviation from the scan (as `test_section_trace.cpp` does for
  the traced profile unconstrained)
  (`compare_scan`,
  [how far a body is from a scan](../inspect/README.md#how-far-a-body-is-from-a-scan))
  is within the tolerance.
- **The readers.** Rounds of 0.5, 2 and 6 mm between faces at 90° and at
  60° read within their sigma; a sharp edge and a chamfer are named as
  such. A scanned M8 × 1.25 and a left-hand trapezoidal thread give their
  pitch, diameter and hand; a plain bore is refused as no thread.
- `refused_calls_smoke` covers each new call's refusals changing nothing,
  and `rpc_units_gate` its keys.
