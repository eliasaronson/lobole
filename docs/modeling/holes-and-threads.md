# Holes and threads

A hole records what it is **for**, not only how wide it is, so the drawing, the
fastener derivation and the interference check can read the intent instead of
reverse-engineering a diameter. Threads are cosmetic: an annotation on a
cylindrical face that changes no geometry.

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
- **A tapped fit threads the bore.** `RecordTapping` adds an internal thread
  region on every bore the hole cut, so a screw in it engages rather than
  reading as interference (see [Thread engagement](#thread-engagement)), and a
  fastener joint ends at the end of that thread.
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
`M8x1`, or free text), `pitch`, `thread_class` (display only), `length` (0 =
the whole face), `modeled`, `allow_engagement`, `minimum_engagement`.

- **Cosmetic.** The body is untouched; volume, STEP, mass and CAM see no
  thread. Each regeneration resolves the face, checks it is cylindrical,
  infers internal or external from the face normal, checks a known
  designation fits the face's diameter, and computes an overlay the UI
  draws: dashed circles at the thread depth (minor diameter on a boss, major
  inside a hole, from the pitch by the ISO 68 profile) and crest lines. A
  missing or non-cylindrical face is a feature error.
- **Standards** (`src/core/ThreadStandards.{h,cpp}`, namespace `threads`): the
  ISO metric coarse and fine series, `designation → {majorDia, pitch}`, with
  `MinorDiameter` (D − 1.0825·P; M8 → 6.647) and `TapDrill` (D − P). A known
  designation fills the pitch; a custom one keeps the typed pitch.
- `modeled: true` is reserved and errors ("modeled threads are not implemented
  yet") while the cosmetic overlay stays drawn —
  [../plans/modeled-thread.md](../plans/modeled-thread.md).
- GUI: `###tb_thread`, then pick a cylindrical face.
- Drawing notes can read a thread's `feature_<id>_pitch`, `_major` and
  `_tap_drill` (and a hole's `_diameter`, `_depth`) as note variables.

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
empty-space warning), `tests/test_thread.cpp` (table lookups, cosmetic volume
unchanged, bad faces, engagement relief, a tapped hole is threaded),
`tests/test_parts.cpp` (the hole tables, fits and their refusals),
`tests/test_derived.cpp` (a hole on a projected derived hole edge),
`tests/test_drawing.cpp` (hole callouts), goldens `v36-thread-rib` and
`v71-hole-fit`.
