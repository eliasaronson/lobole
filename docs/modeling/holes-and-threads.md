# Holes and threads

A hole records what it is **for**, not only how wide it is, so the drawing, the
fastener derivation and the interference check can read the intent instead of
reverse-engineering a diameter. A thread is an annotation on a cylindrical
face, cosmetic by default; a modelled one cuts the real helical groove.

## Hole

`HoleFeature` (`src/core/features/HoleFeature.{h,cpp}`), JSON type `Hole`.

- **Centres** come from a sketch (`sketch_id`): its points, and the centres of
  its circles and arcs; coincident centres merge. Circles count so a hole can be
  placed on another body's hole without copying coordinates — project the
  other body's hole edge into the sketch (`project_geometry`, or `external` on
  the Sketch) and the projected circle is the centre. The hole drills along the
  sketch plane's −normal.
- **Kinds** (`hole_kind`): `simple`, `counterbore` (`cb_diameter`, `cb_depth`),
  `countersink` (`cs_diameter`, `cs_depth`). `depth`, or `through` to pierce
  every body below. `diameter` and `depth` are expressions.
- **Fit** (`fit: {thread, kind, pitch?}`, kinds `close|normal|loose|tapped`):
  when present, `diameter` is **derived** in `Execute` from the vendored tables
  (`parts::HoleDiameter`, `src/core/parts/FastenerTable.{h,cpp}`) — a
  clearance from `clearance_hole_sizes.csv`, a tap drill from
  `tap_hole_sizes.csv` keyed by thread and pitch (`M6-1`), because the drill
  depends on the pitch. `pitch` 0 means coarse, read from the ISO 4762 table
  (`parts::CoarsePitch`) rather than tabulated twice. Deriving in `Execute`
  rather than at the call site is what makes it survive undo, reload, a
  configuration override and an expression on the thread. A thread or fit the
  tables do not carry is refused; nothing is interpolated or rounded to a
  neighbour. A hole without a fit behaves and serializes as a plain diameter.
- **A unified thread taps too.** `fit: {thread: "3/8-16 UNC", kind:
  "tapped"}` drills the unified table's own tap drill (5/16" here, see
  [Thread standards](#thread-standards)), because the vendored tap table is
  metric only. The TPI is part of the designation, so a `pitch` that differs
  from it is refused. A clearance fit for a unified size is refused: the
  clearance table is metric.
- **A tapped fit threads the bore.** `RecordTapping` adds an internal thread
  region on every bore the hole cut, so a screw in it engages rather than
  reading as interference (see [Thread engagement](#thread-engagement)), and a
  fastener joint ends at the end of that thread. It also writes the thread on
  the body (a `ThreadMark`: the table's thread at the fit's pitch,
  right-handed, no class), which is what a CAM thread mill reads
  ([thread mill](../cam/operations.md#thread-mill)).
- **Empty-space warning.** A hole checks, per centre, whether any point down
  the bore (clipped to the bodies' bounding box) is inside a non-reference
  body; centres that meet no material are listed in the feature's warning.
  Some centres missing is the case nothing else reports — the cut still
  removes material elsewhere. The warning reminds that sketch coordinates are
  measured from the sketch plane's origin.
- Reference (Derived ghost) bodies are never drilled.

GUI: the Hole tool (`###tb_hole`: pick a flat face, click the centres). The
editor's **Drilled for** combo lists the clearance table's own sizes (so it
cannot offer a size the lookup would refuse) and a **Fit** combo; under a fit
the diameter row is read-only, since the next regeneration would overwrite it.

**Drawings.** `drawing::FormatHoleCallout` (`src/core/drawing/DrawingCallout.cpp`)
writes `Ø4.5 THRU` / `Ø4.5 DEEP 6` plus counterbore/countersink. A clearance
fit appends its intent (`Ø3.4 THRU, M3 NORMAL CLEARANCE`, named even when it
is normal); a tapped fit leads with the thread instead of the tap drill
(`M3 THRU`), because the drill is not what gets made. `drawing::FindHoleForRef`
finds the Hole behind a projected circle from the feature ids in its element
name, cross-checked against the bore, counterbore and countersink radii.

**Fasteners** read the fit: `fit_fastener` asks the hole for its thread before
falling back to reading the diameter backwards out of the clearance table, a
blind clearance hole is refused as having nothing for a screw to bite, and a
countersunk screw needs a picked countersink rim deep enough for its head (the
parts area).

## Thread

`ThreadFeature` (`src/core/features/ThreadFeature.{h,cpp}`), JSON type
`Thread`: `face` (a cylindrical face by `ElementRef`), `designation` (`M8`,
`M8x1`, `3/8-16 UNC`, optionally with a class suffix, or free text), `pitch`,
`thread_class`, `length` (0 = the whole face), `modeled`, `hand` (`right` or
`left`, absent = right), `allow_engagement`, `minimum_engagement`.

- **Cosmetic** (the default). The body is untouched; volume, STEP, mass and
  CAM see no thread. Each regeneration resolves the face, checks it is
  cylindrical, infers internal or external from the face normal, checks a
  known designation fits the face's diameter, and computes an overlay the
  UI draws: dashed circles at the thread depth (minor diameter on a boss,
  major inside a hole, from the pitch by the ISO 68 profile) and crest
  lines. A missing or non-cylindrical face is a feature error.
- **The designation** is resolved against the tables below. One that names
  a series we carry — an `M` size, a size-TPI pair such as `1/4-20`, or a
  `UNC`/`UNF` suffix — and is not a row is **refused**, naming the rows that
  are (`1/4-20` alone is refused with "did you mean '1/4-20 UNC'?": the
  series is not guessed). Any other text is a custom thread that keeps its
  typed pitch.
- **The class** (`thread_class`, or the designation's suffix: `M8-6g`,
  `3/8-16 UNC-2B`) must be one the series has on the face's side: ISO 965
  `6g`, `4h`, `6h`, `4g`, `8g`, `6e`, `6f` external and `6H`, `4H`, `5H`,
  `7H`, `8H`, `5G`, `6G`, `7G` internal; unified `1A`/`2A`/`3A` external
  and `1B`/`2B`/`3B` internal. `2B` on a boss is refused as the internal
  class it is, and a suffix and a `thread_class` that disagree are refused
  rather than one winning. A class drives no geometry: a modelled thread is
  the basic profile. On a custom thread it is not checked, and the feature
  warns so.
- **On the body.** Every regeneration writes the thread on the body it is
  on (`Body::threads`, a `ThreadMark`: the face, the canonical designation,
  major diameter, pitch, class, hand, side, length and whether it is
  modelled), transient like a bend. It is what a consumer that has only the
  bodies reads -- the CAM thread mill -- rather than guessing a thread back
  from a diameter. A Thread on a tapped Hole's bore comes after the Hole's
  own mark and is the one read. Unlike a `ThreadRegion` it claims nothing
  about engagement.
- GUI: `###tb_thread`, then pick a cylindrical face. The editor has the
  designation combo (both tables, then Custom), the class combo (the
  series' classes for the face's side), **Modeled** and **Hand**.
- Drawing notes can read a thread's `feature_<id>_pitch`, `_major` and
  `_tap_drill` (and a hole's `_diameter`, `_depth`) as note variables.

## Thread standards

`src/core/ThreadStandards.{h,cpp}`, namespace `threads`: rows of
`{designation, majorDia, pitch, series, tapDrill}` in millimetres.

- **ISO metric** (ISO 262): the coarse series M1.6 to M30 and the common fine
  pitches. `TapDrill` is D − P (M8 → 6.75).
- **Unified** (ASME B1.1): UNC `#1-64` to `1-8` and UNF `#0-80` to `1-12`,
  entered in the units their standard uses (inches, TPI) and converted once.
  Each row carries the 75%-thread tap drill a shop chart gives — a number,
  letter or fractional drill (1/4-20 → #7, 5.105 mm; 3/8-16 → 5/16",
  7.938 mm) — which D − P only approximates.
- Both series share the 60° basic profile (ISO 68-1 = the UTS profile), so
  `MinorDiameter` (D − 1.0825·P; M8 → 6.647, 3/8-16 → 7.807) and everything
  derived from D and P serve either.
- `Resolve` accepts each row's own spelling, a metric coarse thread with its
  pitch written out (`M8x1.25` is `M8`), a number size without its `#`
  (`10-32 UNF`), and any of these with a class suffix. `Lookup` is the same
  without the error, for callers that only ask "is this a known thread".

## Pitch-diameter bands

A class is a band of pitch diameters the thread may be made to
(`threads::PitchDiameterBand`, `src/core/ThreadStandards.cpp`): what a thread
mill aims its last pass at (the middle) and what the verifier holds the cut
thread to. Computed from the standards' own formulas, so every row and every
class has one, rather than tabulated:

- **ISO 965-1** (metric): `Td2(6) = 90 P^0.4 d^0.1` um, `d` the geometric
  mean of the nominal diameter's range (5.6 to 11.2 mm for an M8);
  `TD2(6) = 1.32 Td2(6)`; grades 3 to 8 scale both by 0.5, 0.63, 0.8, 1,
  1.25, 1.6. The fundamental deviation is 0 for H and h, `+(15 + 11P)` um for
  G and `-(15 | 30 | 50 + 11P)` for g, f, e. Internal: `[D2 + EI, + TD2]`;
  external: `[d2 + es - Td2, d2 + es]`. M8 6H is 7.188 to 7.348.
- **ASME B1.1** (unified), in inches: the 2A tolerance
  `0.0015 D^1/3 + 0.0015 (9P)^1/2 + 0.015 P^2/3`; 1A x 1.5, 3A x 0.75, 1B
  x 1.95, 2B x 1.3, 3B x 0.975. Every B class starts at basic; 1A and 2A sit
  an allowance of 0.3 of the 2A tolerance below it, 3A at it. 3/8-16 UNC-2B
  is 8.494 to 8.645 mm.

Basic pitch diameter is `D - 0.649519 P`. The published tables round each
term, and these land within a few hundredths of a millimetre of them at worst
(3/8-16 2B's maximum is 8.639 in the table; M8 6g's minimum 7.042 against
7.038). An empty class is not a class: a caller that has none says which it
aims at (`DefaultClass`: 6H/6g, 2B/2A), and a class of the other side or
series is refused.

## Modelled threads

`modeled: true` cuts the thread's groove from the body. It stays opt-in: a
helical sweep-cut is the slowest boolean the kernel does, and most parts
only need the callout.

- **The groove** is the space between two teeth of the basic profile
  (H = √3/2·P), drawn in the half-plane through the axis. External: the root
  flat P/4 wide at the basic minor D/2 − 5H/8, the flanks opening outward to
  P/10 past the face. Internal: the root flat P/8 at the major D/2, the
  flanks opening inward to P/10 inside the wall (a tapped hole's wall is the
  tap drill, so the crests are the drill's). D is the table's; a custom
  thread takes the face as the major on a boss and as the basic minor in a
  hole. The groove is capped at 0.95 P wide so neighbouring turns never
  touch.
- **The helix** (`src/core/HelicalSweep.{h,cpp}`) is one edge per
  turn on a cylinder: one B-spline over many turns loses accuracy turn by
  turn. It starts a quarter turn off the face's seam. `HelixEdge` is also
  the Curve3D helix's builder.
- **The sweep** is `MakePipeShell` with the axis as a fixed binormal
  (`SweepFrame::Binormal` in `SweepProfileAlongSpine`): the exact screw
  motion, which keeps the section in the axial plane. Frenet on OCC's
  approximated helix tilts the section as it goes; it is only the fallback.
  A solid that comes out inside-out is reversed.
- **The ends.** An end that is the face's own end and opens onto air (asked
  of the body over two pitches past it, at four angles) is left open, and
  the groove runs out through it. An end against material — a boss's
  shoulder, a blind hole's floor — or a `length` that stops mid-face is cut
  square: the tool is clipped there by a cylinder. There is no runout. A
  square end on a shoulder (or a blind hole's floor) has the tool's end
  lying on that face, so the cut draws the groove's footprint there: arcs
  that split the plane into coplanar pieces and bound no volume. The cut
  merges them away (`MergeFootprint`): a `ShapeUpgrade_UnifySameDomain`
  restricted to the planar faces the cut made, across edges with such a
  face on both sides, every other edge kept. Its history is composed onto
  the boolean's (`topo::RefineResult`) before anything is named, so the
  shoulder is one face named as the shoulder (`<shoulder>;M<thread>`) and
  every other face is named exactly as the bare cut names it. A whole-body
  `Refine` is not needed, and would name every face afresh.
- **The cut** is one checked subtract on the face's own body
  (`CheckedBooleanNamed`), with the tool's faces named as a primitive's
  (per-flank naming is not done). Any failure — a groove whose root would be
  outside the boss, a sweep or a boolean the kernel refuses — is the
  feature's error: the body stays as it was and the overlay stays drawn. A
  cut that succeeds sets `overlay.cut`, and the UI draws no convention over
  the real flanks.
- **`HelicalSweep`** takes an axis frame, a span, a lead, a number of
  starts, a hand and any closed axial profile, and returns the swept tool,
  so a worm is the same call with a gear's axial profile
  ([worm-gears.md](worm-gears.md)). `pinPhase` pins the phase by a height
  at the frame's X instead of a start angle, which is how a worm's tooth
  is put where its wheel expects it.
- **Cost**, on this machine: an M8×1.25 through a free 20 mm boss (16
  turns, both ends open) about 2.6 s; a 3/8-16 UNC through a 10 mm plate
  about 0.6 s; an M8 12 mm on a shoulder (one end squared) about 1.7 s. The
  sweep itself takes tens of milliseconds; the time is the checked cut, and
  the clip where an end is squared.
- **Printed.** On a part with a print process the basic profile prints
  tight, so `add_feature`/`modify_feature` on a Thread return `print`: what
  it should be modelled at, as a custom thread on the face that gives it
  ([../tolerances/README.md](../tolerances/README.md#printed-parts)).
- **Checked by volume.** A cutter that is screw-invariant and passes through
  both ends of a length L removes exactly 2π·r̄·A·L/P (Pappus holds for the
  screw motion), with A and r̄ the groove section's area and centroid radius
  inside the blank. The tests assert that to 0.5%.

## Thread engagement

Exact interference between two bodies normally counts every overlap. A pair of
matching threads is allowed to overlap over their common span, when both sides
declare it:

- an **internal** thread region comes from a `Thread` with `allow_engagement`
  on a hole wall, or from a Hole with a tapped fit;
- an **external** one from a `Thread` with `allow_engagement` on a boss, or
  from a generated fastener's shank.

`Measure` (`src/core/Measure.cpp`, `ThreadSpans` / `RemoveThreadEngagement`)
relieves only the common cylindrical span of an internal/external pair with a
known designation, the same major diameter and pitch, the face diameter the
standard expects (major for external, tap drill for internal), coaxial axes,
and an overlap of at least the larger of each side's `minimum_engagement` and
one pitch. Anything else — an unknown designation, a stale face, a wrong axis,
diameter or pitch, too little overlap — gets no relief, and any other
intersection between the same two bodies is still a collision. Interference
reports expose `engaged_thread_pairs` and `thread_engagement_volume`. Mesh
collision is geometric only and applies no relief.

## Tests

`tests/test_hole.cpp` (kinds, a bolt pattern from one sketch, the
empty-space warning), `tests/test_thread.cpp` (metric and unified table
lookups and their refusals, cosmetic volume unchanged, bad faces, engagement
relief, a tapped hole is threaded and takes a unified thread, modelled
threads: an M8 in either hand by the Pappus volume and by following the
groove, a shoulder left whole and left one face with no stray edges and
its other faces' names kept, a 3/8-16 UNC-2B in a tapped wall, a failure
that keeps the body), the GUI case `thread/modeled_checkbox_cuts_the_groove`,
`tests/test_parts.cpp` (the hole tables, fits and their refusals),
`tests/test_cam_thread.cpp` (the pitch-diameter bands against the tables,
and a tapped hole's thread on its body),
`tests/test_derived.cpp` (a hole on a projected derived hole edge),
`tests/test_drawing.cpp` (hole callouts), goldens `v36-thread-rib` and
`v71-hole-fit` and `v140-modeled-thread`.
