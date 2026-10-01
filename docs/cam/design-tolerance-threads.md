# CAM honest about tolerance and threads (design note)

Workstream B of [the pan/tilt friction round](../plans/pantilt-friction-round.md).
CAM trusts the model where the model is silent: the K7 seat is cut at
nominal, a profile on tabs never verifies complete, a blind tapped hole is
threaded to the bottom of a flat drill, a thread nobody cuts is not
mentioned, and adaptive clearing is slow on the base plate.

## 1. Tolerance bands

- **What CAM reads.** A part's size tolerances (`Document::GetTolerances`,
  a class or a deviation pair on a round face) are stamped on the job's
  bodies when the job assembles them (`ui::JobBodies`), as
  `Body::bands {face, lower, upper, label}` -- diameter deviations, the
  band measured at the face's own size. Transient like `jobPartId`; a
  tolerance edit moves the job's body key, so the job regenerates.
- **Where a finish pass aims.** Any pass that follows a toleranced round
  wall puts that wall at the middle of its band: the material boundary of
  the circle is moved radially by `(lower + upper) / 4` before the tool is
  offset from it. Honoured by drill (a bore's rings), profile (a wall's
  contour, inside or outside), pocket and adaptive (a round outline or a
  round island), and so by every rough-then-finish pass of those. Not by a
  plunge (the tool is the hole's size), face, edge break, surfacing or
  engraving. A distance tolerance is not read.
- **Told to the verifier.** Each toolpath that aimed at a band declares it
  (`Toolpath::Fit`: centre, side, nominal, band, aim, the wall's Z span).
  The verifier measures the diameter the posted file cut there -- the
  widest (inside) or narrowest (outside) the tool's rim reached over the
  wall's span, from the declaring operations' moves -- and judges it
  against the band (`fits` in `cam_verify`). Cut past the band on the scrap
  side (a bore too big, a boss too small) counts against `ok`; short of it
  against `complete`. The band's ring is excused from gouge and leftover,
  so a band wider than the grid's quantisation does not read as either.
- **Posted.** The program's header states each band and its aim:
  `(fit op 7: bore D32 K7 31.982-32.007, aimed at 31.995)`.
- Untoleranced faces are cut exactly as before.

## 2. Tabs and offcuts

The verifier's map is split, after the cut, into pieces of material that
through cuts separate (cells cut to the stock's floor separate; a tab's
cells in the kerf join nothing). A piece with none of the part in it is an
**offcut** and is never leftover -- the slug of an opening, the frame round
a part. A piece held neither by the stock's edge nor by a tab is **cut
free**: an offcut cut free or a part cut free is a finding (`cut_free`, a
Warn line naming where), not a failure, since a vacuum table or screws the
job does not model may hold it. A profile on tabs then verifies complete.

## 3. Drill point and thread depth (format)

- `Hole.drill_point_deg`: the included angle of the drill's point at a
  blind hole's floor, modelled as a cone (role `point`). Absent, derived: a
  hole with a fit is drilled to a drill table's size, and those tables are
  ISO 235 jobber drills, whose point is 118 degrees; a plain diameter says
  nothing about how the hole is made and keeps a flat floor. 180 is a flat
  floor said out loud. `depth` stays the full-diameter depth.
- `Hole.thread_depth` (+ `_expr`, tapped fits only): how deep the tapped
  thread runs from the mouth. Absent, derived: the drilled depth less three
  pitches -- the lead of the common blind-hole tap (ISO 8830 form C, 2-3
  threads), which cuts no full thread -- and the whole bore for a through
  hole. The Hole's `ThreadMark` and `ThreadRegion` carry that length.
- Each Hole also writes a `HoleMark` on its bodies (bore face, diameter,
  depth, through, point angle, its thread), as a Thread writes a
  `ThreadMark`: what CAM reads to know the floor is a drill's point and to
  list uncut holes.
- **CAM.** Drill derivation on a bore whose floor is a point lowers the tip
  by the tool's own point height, so a drill of the hole's angle leaves the
  modelled cone; another angle, or a flat tool, is said in the notes. The
  thread mill already stops a thread at its `length`; the verifier judges
  it against that span.

## 4. Uncut holes and threads

`cam_verify` lists every hole and thread of the job's parts that the job
does not cut (`uncut`): a hole when no setup leaves its bore in air (this
setup's machined map, or another setup's, `SetupStock::Removes`), a thread
when no enabled thread-mill operation of the job targets it. A finding, not
a failure: a job routinely leaves features to other processes (the M4s are
tapped by hand), `complete` stays the program's own statement about what
it set out to cut, and a generator that requires a feature gates on the
list itself.

## 5. Adaptive

Profile the pan/tilt base plate's Top setup (`tools/profile.sh`) and fix
what it shows; report before and after.

## Files

`src/core/Feature.h` (HoleMark, Body::bands), `src/core/features/HoleFeature.*`,
`src/core/ElementOrigin.*` (role `point`), `src/core/io/SerializeFeatures.cpp`
(and `kVersion`, a golden, format.md), `src/core/cam/CamJob.h`
(`Toolpath::Fit`), `CamStrategies.cpp`, `CamDerive.cpp`, `CamVerify.*`,
`CamPost.cpp`, `CamAdaptive.cpp`, `src/ui/JobActions.cpp`, `CamTool.cpp`,
`src/remote/RemoteCam.cpp`, the docs in `docs/cam/` and
`docs/modeling/holes-and-threads.md`.

## Verified by

`tests/test_cam_fit.cpp` (a K7 bore aimed, posted, judged; an out-of-band
file failed), the tab test passing complete with an unheld offcut flagged,
a blind tapped-hole coupon with a drill point verified clean,
`test_hole.cpp` (point, thread depth, round trip), and the pan/tilt base
plate job re-run on a copy: the seat aimed into K7, the M4 threads listed
as uncut, both setups clean; adaptive timed before and after.
