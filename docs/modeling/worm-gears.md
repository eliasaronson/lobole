# Worm gears

A worm and the wheel it drives, each cut from a cylindrical blank the way a
Thread cuts a boss: pick the blank's outside face and the feature cuts the
teeth into its body. `Worm` is a thread with a gear's axial profile, built by
the same helical sweep as a modelled thread
([holes-and-threads.md](holes-and-threads.md#modelled-threads)). `WormWheel`
is built the way a wheel is cut: by a hob, the worm oversized, rolled through
the mesh. The two are coupled in an assembly by the `worm` relation
([../assemblies/relations.md](../assemblies/relations.md#worm)).

Code: `src/core/WormGear.{h,cpp}` (sizes, the worm's tooth space, the hob's
envelope, the relation's sign), `src/core/features/WormFeature.{h,cpp}` (both
features). Tests: `tests/test_worm.cpp`.

## Why two features, not a generator

The pan/tilt head plan started this as a generator next to `gear.py`, to
become a feature only if it had to. It had to: a generator can only reach
what features already build, and none builds a helical sweep of a custom
profile or cuts a wheel by a moving tool. A spur gear is a sketch extruded,
so it can stay a script. A worm cannot.

It is two features rather than one with a kind, for three reasons:

- They take different numbers. The wheel needs its tooth count and the
  mating worm's diameter. The worm needs a backlash and a length.
- A worm is useful alone, as a lead screw or a feed worm.
- Each is its own part in an assembly.

## The worm

`Worm {face, module?, starts?, pressure_angle_deg?, hand?, backlash?,
length?}`. The face is the blank's cylindrical outside, and it is the worm's
tip cylinder:

- **pitch diameter** d1 = tip − 2m.
- **module** omitted = tip / 12, which is a diameter quotient q = d1/m of 10.
  A 24 mm blank is therefore module 2 on a 20 mm pitch diameter.
- **Profile:** ZA, straight-sided in the axial section. The flanks are at the
  pressure angle (20° by default), the addendum is m and the dedendum is 1.25m.
- **Backlash** (mm, axial) thins the tooth by half of it on each flank. A
  printed pair runs at 0.2–0.3 mm.
- **Ends:** an end that opens onto air is run out through it, and one against
  material (or a `length` stopping mid-face) is cut square, as a modelled
  thread's ends are.

The removed volume is exact. The cut is screw-invariant, so it removes
2π·r̄·A per axial pitch (Pappus for a screw motion), and the test holds it to
that within 0.2 %.

Refused, naming what is accepted: an unknown hand; starts outside 1–8; a
pressure angle outside 10–30°; a pitch diameter that leaves no root; a lead
angle over 45° (that is a crossed helical gear); a backlash that brings the
tooth to a point; a bore instead of a boss.

## The wheel

`WormWheel {face, teeth?, module?, starts?, worm_diameter?,
pressure_angle_deg?, hand?}`. The face is the blank's rim, and its length is
the face width. The worm's `starts`, `worm_diameter` (its pitch diameter)
and `hand` are given again, because the worm is in another part.

- **module** omitted = rim / (teeth + 3), the usual outside diameter.
- **worm_diameter** omitted = 10 modules.
- **Sizes** (KHK / DIN 3975, axial module): r2 = z2·m/2, the centre distance
  a = r1 + r2, and the throat radius ri = r1 − m about the worm's axis. The
  throat diameter is dt = 2(a − ri) = d2 + 2m. The root is r2 − 1.25m.
- **Tip:** r2 + 1.5m, held to where the throat's arc stays within 45° of the
  mid-plane. It is then lowered, if needed, until the thinnest tooth (at a
  face end, where the throat does not reach) keeps a crest a quarter of a
  module wide. The head's pair keeps the full r2 + 1.5m, with a 2.2 mm crest.
- **Face width:** a face wider than the worm can reach is refused, and the
  message says how wide it can be. Past the effective limit of 0.67·d1, the
  report notes that the ends carry no load.

Printing: the wheel prints axis-up. Its throat arc is at most 90° (58° on
the head's pair), so the overhang stays within 45°.

### How the wheel is cut

A hob is the worm with its tip raised by the root clearance and each flank
thickened by 0.05 of a module. That clearance comes before any backlash, so
the worm clears its own wheel.

The mesh motion is this: the wheel turns θ, and the worm turns the angle
that screws its thread r2·θ along its axis. The hob is screw-invariant, so
that turn is the same as a slide. The survey planned to cut the wheel with
the hob placed at 6–8 steps per wheel pitch: one gap envelope built by one
multi-tool boolean, patterned z2 times. **Measured, the booleans do not
scale.** One multi-tool common of a sector with 1, 2, 4 and 8 hob placements
took 0.16, 2.4, 14 and 105 s, about ×7 per doubling. Sequential cuts grow
about 0.65 s per placement already cut. A space needs about 60 placements at
n = 8, which is hours either way. The placements are nearly coincident
helical B-spline surfaces, and intersecting those is what the kernel pays
for.

So the envelope is computed point by point instead. It is the same hob and
the same motion, which makes it the limit that the stepped loop converges
to:

- **Is a point cut?** A point of the wheel is cut when some position of the
  hob contains it. Taking the point back through both turns into the hob's
  frame is a closed-form test (`Hob::Margin`). The best position is found by
  sampling the wheel's turn and refining with a golden-section search.
- **Each section's space.** In each transverse section, the tooth space is
  bounded by the two angles where the hob stops cutting. They are found by
  stepping out from the space's middle and bisecting, at 12 radii from the
  root to just past the tip.
- **The solid.** The two flank curves (B-splines approximated to 0.002m),
  the root arc and a top arc close each section. The sections are lofted into
  one solid, which is turned z2 times. The wheel is the blank less the throat
  (a torus about its axis, radius ri at the centre distance) and those z2
  spaces.

The stepped loop is still there as `HobOptions::stepsPerPitch`. The survey's
convergence measure, one space's mid-plane area, reads:

| hob steps per pitch | 8 | 16 | 32 | continuous |
|---|---|---|---|---|
| space area, mm² | 20.6486 | 20.6571 | 20.6595 | 20.6607 |

The gap to the continuous envelope falls by about 3.4 then 3.0 per doubling.
The scallops go as 1/n², as the survey predicted, and are 0.006 % of the area
by n = 32.

Measured facts about the loft and the cut:

- Building the envelope and the loft takes about 0.3 s.
- The cut of the forty spaces takes 15–23 s on the head's pair, under the
  30 s budget.
- The sections are an **even** count, so none lies on the mid-plane. A
  section there lies along the throat torus's equator, and every odd count
  measured gave an invalid wheel.
- **Twelve points** a flank. With 24, the booleans that follow failed on some
  face widths, and interpolating through the points instead of approximating
  cost eight times as long.
- Volume against the number of sections: 10, 20 and 36 sections agree to
  0.03 %.
- **How the teeth's cut is checked.** It is not checked by
  `BooleanRefusal` ([README.md](README.md#a-boolean-is-checked-before-it-is-kept)).
  That check's point-in-tool test misjudges a compound of lofted B-spline
  solids. Measured, it named a point on a tooth's crest as inside a space, on
  a result that passed BRepCheck and whose volume matched a plain cut's to
  the last digit. Its fuzzy retry then cost a further minute.
- **What is checked instead** is what every wrong result measured did get
  wrong: BRepCheck (each one failed it), one solid, and a removed volume
  between nothing and every space whole. A cut that fails this is tried again
  with the sections moved (two more), then with 16 points a flank, before the
  wheel reports the failure as its error. The throat's cut is an ordinary
  checked boolean.

The fallbacks the survey lists are the BOSL2-style bent tooth and a helical
involute at the lead angle. Neither was needed. The computed envelope is
throated and has line contact, which both fallbacks give up.

## Phase

Where the teeth stand, so that a pair meshes at zero turn:

- **Wheel:** a tooth is centred on the blank's cylinder X direction (its
  seam), at the face's mid-plane. Space 0 is half a pitch round from it.
  When a space was centred on the seam instead, a sliver of the seam's face
  stayed inside the cut and the exact boolean got the wheel wrong.
- **Worm:** a tooth space is centred at the middle of its span, on the
  blank's X side.

Put the worm's X towards the wheel's X side at the mesh, with its mid-span
on the wheel's mid-plane, and the wheel's tooth stands in the worm's space.
`tests/test_worm.cpp` mates the pair this way with a relation phase of 0.

## Self-locking

Both features report it. `get_state` and the `add_feature` response carry
`report.locking` with two rows, dry printed polymer (μ 0.2) and greased
(μ 0.1). Each row gives the friction angle atan μ, whether the lead angle γ
is under it (static self-locking), and the efficiency tan γ / tan(γ + ρ)
with the worm driving.

The head's pair has γ = atan(1/10) = 5.71°:

- **Dry:** locked, with a 2× margin (the friction angle is 11.3°).
- **Greased:** exactly at the edge, since μ = 0.1 is tan γ.

Whether it stays locked under vibration is not claimed. Static self-locking
is all the rule gives.

The wheel's report also carries the centre distance to mount the worm at,
the ratio, the throat and tip diameters, the tip crest, the throat arc, the
number of sections and how long the hob took.

## Tests

`tests/test_worm.cpp`:

- the sizes against KHK's worked example;
- the refusals;
- the worm's removed volume to Pappus;
- the envelope converging as the hob's steps shrink;
- the relation's sign for each hand, side and mate orientation;
- `[slow]`: a wheel hobbed from its blank, and the pair meshing through
  1¼ worm turns (more than a wheel pitch) in a motion study checked exactly
  at every sample, both hands, with the opposite sign as the control that
  must clash.

GUI: `worm/editor_picks_the_blank_and_sets_hand` and
`worm/wheel_editor_reports_the_pair`, plus the `defaults/worm` and
`defaults/worm_wheel` tool cases.
