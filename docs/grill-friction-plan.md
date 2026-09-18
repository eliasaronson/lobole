# What the grill's joints cost, and what to do about it

Building `generators/grill/` to a real bill of materials — 35 parts, 43
derived fasteners, one connected structure — took three passes. The third
one, which designed the JOINTS rather than the placements, is where both the
defects and the friction were: five app bugs (fixed, see
`docs/grill-dogfood-plan.md`) and a build loop that costs **two minutes a
turn** for a model whose geometry is a few dozen flanges and a hundred holes.

Measured, on this machine, over a full build of the grill:

| | |
|---|---|
| whole build, part tabs + assembly + fasteners | **117 s** |
| of that, `NormalCut` — 30 calls, 2.87 s each | **86 s (73%)** |
| `FlatPattern` — 16 calls | 9.8 s (8%) |
| `fit_fastener` — 43 calls | 5.4 s |
| `check_fasteners` + `contact_report`, once each | 8.0 s |
| every other RPC — 855 calls, flanges and sketches and mates included | **7.9 s** |

Reproduce with `python3 scratch/time_build.py` (the harness wraps
`Designer.request` and aggregates by method and feature type) and
`tools/profile.sh --app scratch/profile_firebox.py` for the C++ attribution.

Eight stages, in the order of what they cost.

## 1. A hole should not unfold the part

`sheet::NormalCut` is 33% of the app's CPU during a firebox build, and the
profile says where it goes:

```
44.78%  sheet::UnfoldImpl                 <- the whole part, laid flat
  └ 24.16% of it called from sheet::NormalCut
13.06%    sheet::Fuse          (boolean per bend)
11.95%    sheet::StripFor
 8.22%    sheet::Common
 9.13%  IntCurvesFace_Intersector::Intersector()   <- one per sample point
 5.35%  IntCurvesFace_Intersector::Perform
 3.52%  IntCurvesFace_ShapeIntersector::Load
```

Two costs, both paid for every hole, and neither needed by most of them:

- **The outline is sampled every 0.25 mm and each sample gets its own ray
  intersector.** `Hit()` constructs `IntCurvesFace_ShapeIntersector`, `Load`s
  the WHOLE body into it, performs one ray, and throws the face tree away.
  A Ø30 port is 377 samples; the firebox's five cuts are ~2,300 ray casts,
  each rebuilding a tree over a thousand-face solid.
- **The part is unfolded — completely — to answer "where does this outline
  land?"**, and the unfold is itself a chain of booleans (Fuse, StripFor,
  Common: 33% of the app between them). For the common case the answer is
  thrown away three lines later: *"One panel, no bend: lay the cutter out in
  the blank and move it straight back onto the panel. Nothing is unfolded at
  all."* — except that it was, already, unconditionally.

And both are paid **per REGION**: `NormalCutFeature::Execute` loops over the
sketch's profiles and calls `sheet::NormalCut` once each, so the firebox's
13 holes unfold the firebox 13 times.

**The fix**, in three moves, smallest first:

1. **One intersector per cut.** `Hit` takes a loaded
   `IntCurvesFace_ShapeIntersector&` instead of a body; `NormalCut` loads it
   once. Pure mechanics, no behaviour change.
2. **The unfold becomes lazy.** The sample hits already name the FACE each
   one lands on. If every hit is on the same planar face, the outline is on
   one panel and crosses no bend — which is the question `pieces` and
   `crossed` were unfolding to answer. Unfold only when the faces disagree,
   which is exactly the across-a-bend case the machinery exists for.
3. **The region loop shares the work.** `sheet::NormalCut` takes the profile
   faces as a set (or a small context holding the intersector and the
   lazily-built `Placement`), so a four-hole sketch pays at most once for
   whichever of the two it turns out to need.

Files: `src/core/sheet/Unfold.cpp` (`Hit`, `NormalCut`, a `CutContext`),
`src/core/sheet/Unfold.h` (the entry point takes several profiles),
`src/core/features/NormalCutFeature.cpp` (hand it every region).

**Verified by**: the five existing `[normalcut]` cases unchanged — the exact
circle, every hole its sketch holds, across a bend, the oblique landing, and
the sheet its sketch looks at — plus a new one that cuts a twelve-hole sketch
into a folded pan and asserts the body is identical (volume, face count,
named cylinders) to cutting the same twelve one feature at a time. The number
to beat is the firebox: **26 s today**, and the arithmetic says ~4 s once a
plain hole stops unfolding anything.

If `FlatPattern`'s 9.8 s still stands out afterwards, the same `Placement` is
what it builds: caching one per (body shape, rules) serves both, and is a
separate change with its own risk.

## 2. A tapped hole should be ONE thing

Today a tapped hole is three coordinated things, and two of them are
invisible: the hole must be at the TAP DRILL diameter, carry a `Thread`
feature, and that thread must set `allow_engagement`. Miss any one and the
screw in it is a clash — 40 mm³ of steel and fastener in the same place, with
`engaged_thread_pairs: 0` the only hint, and no refusal anywhere.

`Hole` already accepts what the caller means: `fit: {thread: "M6", kind:
"tapped"}`, which derives the 5.0 mm drill. It just does not stamp the thread
region, so nothing downstream knows what the hole is FOR. Only
`ThreadFeature` and `FastenerFeature` ever populate `Body::threadRegions`.

**The fix.** A `Hole` whose fit kind is `tapped` records the internal thread
itself — designation and pitch from the fit it was drilled for, engagement
allowed, minimum engagement 0 — which is what `ThreadFeature` does, minus the
second feature and the face pick. The explicit route stays for a thread on a
face that no Hole cut.

Files: `src/core/features/HoleFeature.{h,cpp}`, the `Hole` row in
`SerializeFeatures.cpp`'s type table (documentation only — `fit` is already
serialized), `docs/parts/plan.md`.

**Verified by**: a block with an M6 tapped `Hole`, an ISO 7380 M6x12 fitted
into it from the rim, and `interference` reporting **no clash** with
`thread_engagement_volume > 0`; `check_fasteners` ok on the same pair; and
the negative — the same hole with `kind: "normal"` still clashes, because a
clearance hole is not a thread.

## 3. A rim must resolve in the part it belongs to

`fit_fastener`'s `rim` is `{feature_id, element}`, and a feature id is unique
within a part, not across the project. Its own documentation says the
occurrence is "needed only when the part is placed more than once". It is
needed always: handed the right-hand cart side's hole without one, the app
answered with the LEFT side's — the same feature id in a different part, 624
mm away. It surfaced only because a second pick disagreed about the axis.

**The fix.** Resolve the rim by finding the occurrences whose part owns that
feature id AND resolves that element name. One candidate: use it. Several:
refuse, naming them and asking for `occurrence` — the same shape as every
other ambiguity refusal here. Then correct the `MethodDoc` row, which is
currently wrong in a way that reads as permission.

Files: `src/ui/FastenerActions.cpp` (the rim resolution), the `fit_fastener`
row in `src/remote/RemoteAssembly.cpp`, `tests/test_actions.cpp`.

**Verified by**: two parts that both carry a hole at the same feature id,
placed once each; fitting without an occurrence is REFUSED naming both, and
with one is correct. The grill is the regression: its 43 fasteners all pass
the occurrence today, and the refusal must not fire on them.

## 4. The joint a bolt is in has to survive re-derivation

`check_fasteners` stores nothing — a placed fastener's rim and axis are its
own position and orientation, which is the right design and is what lets it
check a screw nobody derived. But `far_end` is not recorded, so a joint that
legitimately ends before its axis does can only be re-derived when a NUT
bounds it. A screw into a tapped boss with anything beyond it re-derives as a
different, longer joint and reports a finding against a correct model: on the
grill, "0 mm of thread in Lid handle" for two M6s that are fine.

**The fix**, cheapest first: **a thread bounds a joint the way a nut does.**
The model already knows — `Measure::ThreadSpans` reads the internal thread
regions for the clash check — so a screw whose axis enters a thread of its
own designation ends where that engagement ends. That covers every tapped
joint without storing anything. Where a joint genuinely ends in open air with
material beyond and no nut and no thread, nothing can re-derive it, and THAT
is the case worth a recorded far end (a format bump) rather than the general
one.

Files: `src/core/parts/AxisStack.{h,cpp}` (`JointEnd` takes the threads it
finds), `src/core/Measure.{h,cpp}` (`ThreadSpans` moves somewhere both can
reach it), `src/ui/Bom.cpp`.

**Verified by**: the grill with its tappings NOT artificially blind — the lid
handle's bore open into the tube, where the finding came from — reporting no
findings; plus a unit case with a boss bored right through into open air.

## 5. Say what a bend IS

There is no way to ask the app about a bend. `check_sheet_metal` returns
counts, `get_sheet_metal_rules` returns the rules a part was built under, and
the only per-bend data anywhere is the TEXT of a drawing note. The bend audit
this round needed — every bend's radius against the material's minimum, its
relief, its allowance — was assembled out of those three by hand.

**The fix.** `list_bends` (or `bends` on `list_elements`): per bend, its id,
angle, inside radius, the K and allowance used, its length, which way it
folds for a given side, and the relief cut at each end with its style and
size. `drawing::BendTableText` already computes most of this for the table;
this is the same record, as data.

Files: `src/remote/RemoteInspection.cpp` (+ its `MethodDoc` row),
`src/core/sheet/Unfold.h` if the per-bend record needs widening,
`tests/sheetmetal_smoke.py`.

**Verified by**: `generators/grill/grill_shop.py`'s audit reading the RPC
instead of three sources; a smoke assertion that the row count matches
`check_sheet_metal`'s `bends_checked` and the drawing table's rows, so the
three cannot drift.

## 6. A face with no name is invisible, and says nothing

`list_elements` walks `elementMap.names`, so a face the map never named does
not appear — it cannot be threaded, picked, filleted or fitted, and the
listing looks complete. That is how finding 15 presented: a bore drilled in
one end of a pipe-and-plug body had no cylinder to thread, while the other
end, same feature, same call, did.

**The fix**, two parts. The naming defect itself needs a reproduction reduced
from the handle (a `Pipe`, two `Extrude` unions, one `Extrude` cut) — and
until it is understood, the listing must at least COUNT what it is not
showing: `unnamed_faces: N` per body, so "nothing to thread here" is
distinguishable from "no such face".

Files: `src/remote/RemoteInspection.cpp` (the listing),
`src/core/ElementMap.h` (the count), then wherever the reduction lands.

**Verified by**: the reduced repro as a `[toponaming]` case asserting every
cylinder of the cut is named; and the listing's count asserted on a body with
a deliberately unnamed face.

## 7. A bent tube is a part, not a picture

The lid handle is the one bent tube on the grill, and the pipe system made a
real one: a `Curve3D` (`kind: fit_spline`) through the centreline, then a
`Pipe` with an outer diameter and a wall. It is accurate — the swept surfaces
measure 34,017 mm² and 28,914 mm² against π·D·L of 34,015 and 28,913 for a
541.4 mm path, six thousandths of a percent — it takes a boolean afterwards
(the plugs welded in its ends) and a tapped hole in those, and it is visibly
the part rather than two cylinders meeting at a corner, which is what it
replaced. That part of it works.

What is missing is everything a SHOP needs, and the path itself:

- **There is no line–arc–line path.** A tube is bent by a former of radius R,
  through an angle, at a distance from the last bend, rotated about the tube
  — that is literally what a bender is programmed with. `fit_spline` takes
  points, so the handle's centreline is 29 of them, generated in Python: extra
  points ALONG the straights, because a spline through two ends bows and the
  tube trumpets where it does, and eight per 90° corner to approximate a 45 mm
  former. The bend radius is therefore an approximation that nothing in the
  model knows exists.
- **There is no DFM and no shop output for it.** Every sheet part here got a
  minimum bend radius per material, a relief rule, a flat pattern, a bend
  table and a DXF. The one tube got none: no minimum centreline radius (the
  1.5–3 × OD rule that decides wall thinning and ovality), no cut length, no
  bend schedule — distance, rotation, angle per bend. `grill_shop.bend_audit`
  could audit sixteen sheet parts and not the handle.
- **A swept body's `bbox` is its control hull, not its extent.** The handle
  measures ±227.6 × ±19.0 × 118.0 for a tube whose real extent is ±210 × ±10
  × 100: a rational circle's poles sit at 2R, and the fitted spline's poles
  overshoot at the bends. Revolved and prismatic bodies are exact (the wheel
  is ±75.0, the bottle ±155.0), so this bites only NURBS — but generators
  measure with `bbox` constantly. Three placements in this round were landed
  by it (the lid onto the rim, the tray onto its rail, the tent onto its
  ledge), and on a swept body every one of them would have been 17 mm out
  with nothing to say so.

**The fix.** A `Curve3D` kind that IS a bent tube's path: a polyline plus a
bend radius (per corner, or one for the part), built as exact line–arc–line,
which makes `Pipe` exact and gives every bend a radius and an angle that can
be read back. Then the tube's own shop record beside the sheet one: cut
length, and per bend its radius, angle and rotation, checked against a
minimum centreline radius for the material — `check_tube` to `check_sheet_
metal` as a bend table is to a blank. And `bbox` either computed optimally
for spline bodies or reported as the conservative box it is.

Files: `src/core/features/Curve3DFeature.{h,cpp}` (the new kind),
`src/core/sheet/` has no business here — a tube table belongs beside the
fastener tables in `src/core/parts/` or its own `src/core/tube/`;
`src/remote/RemoteInspection.cpp` for `check_tube`; `src/core/Measure.cpp`
for the optimal box.

**Verified by**: the handle rebuilt from a polyline and a 45 mm radius,
asserting its swept area against π·D·L for the exact path length (the same
arithmetic that passes today, now against a path whose length is known
rather than sampled); a bend below the minimum radius REFUSED by name; and
the handle's bbox matching its true extent.

### Found while building stage 4

**An exact boolean can lose a bore.** A Ø6 pin standing in the Ø5 bore of the
lid handle's plug reports an overlap of 305.4 mm3 — the WHOLE pin — where the
truth is the annulus around it, 93.3. The handle's solid is valid
(`BRepCheck_Analyzer`), its volume is right to 3 mm3 in 52,190, its faces are
named and its bore measures Ø5 exactly. Reduced: an extruded tube with a ring
plug and a pin answers 93.3; a straight `Pipe` swept along a `Curve3D` with
the same plug and pin answers 93.3; only this body — the bent handle, two
plugs, a union each — answers 305.4. It is the class of failure the bicycle
round hit from the other side (an invalid body making `interference` report
17 cm3 of cog inside a frame that does not reach it), except that here every
check says the body is sound.

It matters beyond one handle: `interference` and `contact_report` are the
gates this whole round is verified by, and a boolean that silently swallows a
void turns a correct model into a clash report nobody can act on. Next step
is a reduction that runs headlessly — the shape is in `scratch/grill.dsgn`
and the pin is fifteen lines — and then either a fix or a refusal, because a
measurement that cannot be trusted has to say so.

## 8. The small ones, as one wave

Each is a papercut that cost this round minutes, not hours. Two of the six
were tried and DROPPED once the build answered back — recorded here because
a plan that only lists what worked is a worse guide than one that says which
of its own ideas were wrong:

- ~~**A zero-depth overlap is touching, not a clash.**~~ DROPPED. The case
  that motivated it — a heat tent's edge resting on its ledge, 0.002 mm³ over
  78 mm² — turned out to be a real 0.086 mm penetration once the tent's seat
  was measured properly, and a rule that calls that touching would hide the
  next one. The fix belonged in the model, and is there: a resting part is
  seated with a tenth of a millimetre of clearance.
- ~~**`new_project` on a dirty document should refuse** unless
  `force: true`.~~ DROPPED. Implemented, and it broke six smokes at once —
  because a script that resets between phases means exactly that, and this is
  the call it means it with. The accident it was meant to prevent (a probe
  pointed at the wrong instance) is answered by `ping`, which says which
  instance and which project before anything is discarded. The row now says
  plainly that the call discards unsaved work.
- **A flange's `length` is measured from the tangent line**, so it reaches
  `length + radius` past the face its edge is on. The row DID say it —
  `length_mode: flat (the wall past the bend)` — and I did not read it, which
  is its own kind of evidence: the row now spells out the number to work back
  from when a flange has to land on something.
- **`modify_feature` replaces the whole feature.** Landing the lid on the rim
  — moving one sketch plane by a measured delta — is read, strip `id` and
  `index`, mutate, send back. A patch form, or `set_sketch_plane`, makes
  measure-then-correct a two-liner.
- **Assembly placement is boilerplate.** Every generator rewrites
  `add_occurrence` + `add_mate` + socket frames. One SDK call —
  `place(part, on={occurrence, frame}, with=frame, type=...)`, returning both
  ids — removes about forty lines per generator.
- **A read-only question that NAMES its part is still gated on the active
  tab.** DONE: `list_bends`, `check_sheet_metal`, `get_sheet_metal_rules`,
  `export_dxf` and `tube_schedule` now answer from any tab and refuse for
  themselves when there is neither a name nor an active part. The grill's
  audit dropped its tab walk.
- **The staleness convention test fires on any source file**, so editing an
  unrelated subsystem in another window fails the suite with a message about
  a stale binary. Compare against the files that link into the binary.

## Order

1 first: it is the loop every other change is tested through, and 73% of it
is one feature doing work it then discards. Then 2, 3 and 4 — each removes a
class of SILENT wrongness, which is worse than slow. Then 5, 6 and 7, which
are about being able to ASK: what a bend is, what a face is called, and what
a bent tube would cost a shop. Then 8 as one wave.
