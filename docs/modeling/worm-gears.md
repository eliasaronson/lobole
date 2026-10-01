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
- **The solid.** The two flank curves (B-splines fitted to 0.002m), the
  root arc and a top arc close each section. The sections are lofted into one
  solid, which is turned z2 times. The wheel is the blank less the throat (a
  torus about its axis, radius ri at the centre distance) and those z2 spaces
  ([how the teeth are cut](#how-the-teeth-are-cut)).

The stepped loop is still there as `HobOptions::stepsPerPitch`. The survey's
convergence measure, one space's mid-plane area, reads:

| hob steps per pitch | 8 | 16 | 32 | continuous |
|---|---|---|---|---|
| space area, mm² | 20.6486 | 20.6571 | 20.6595 | 20.6607 |

The gap to the continuous envelope falls by about 3.4 then 3.0 per doubling.
The scallops go as 1/n², as the survey predicted, and are 0.006 % of the area
by n = 32.

Measured facts about the loft:

- Building the envelope and the loft takes about 0.2 s.
- The sections are an **even** count, so none lies on the mid-plane. A
  section there lies along the throat torus's equator, and every odd count
  measured gave an invalid wheel.
- **Twelve points** a flank. With 24, the booleans that follow failed on some
  face widths, and interpolating through the points instead of approximating
  cost eight times as long.
- **Every flank is fitted on the same knots**: a least-squares cubic on
  uniform spans, the fewest spans (six on the head's pair) that keep every
  point within 0.002m. Fitted each on knots of its own, as before, the flanks
  made the loft merge all twenty sections' knots into each one, and the flank
  faces came out 19 × 19 patches where they are now 6 × 19. The cut spent
  most of its time evaluating them, and fell from 15 s to 10 s with this
  change alone.
- Volume against the number of sections: 10, 20 and 36 sections agree to
  0.03 %. Fewer sections cut faster (8 took 7 s whole against 9 s for 20)
  but move the shape, so the count stays.

### How the teeth are cut

Measured on the head's pair, the cut of forty spaces at once cost forty
times one space's: 0.33 s a space, 13–15 s in all. The kernel's time goes to
intersecting each space's lofted faces with the throat, the tip and the end
faces. Running that boolean in parallel, giving it a fuzzy value or oriented
bounding boxes made it no faster (fuzzy made it 39 s). So the wheel cuts one
space and copies the result (`worm::CutSpacesBySectors`):

1. **The ring.** The spaces only reach a ring: from under every root to past
   the blank, and a module past both faces. A blank that is a solid of
   revolution about the wheel's axis is its own ring. Any other is split
   into the ring and the rest, and the ring must then be a solid of
   revolution: anything already cut into it would be copied round.
2. **One sector.** A pitch of the ring, from space 0's middle to space 1's,
   less spaces 0 and 1. Its boundary steps at the mid-plane, so that each
   half runs inside the space, which leans with the lead angle. A tooth
   then lies whole in one sector, and no tooth face is split. With a plain
   radial boundary at a tooth's middle, the space nicked it where the throat
   meets the tip, and gluing the sectors left eighty slivers of 0.0006 mm².
3. **Turned and glued.** The sector is turned z2 times and the copies are
   fused with OCC's full glue option, since they meet only on whole,
   identical faces. The rest of the body, if any, is fused back on, and faces
   the sectors split are merged. Space 1 is space 0 *moved*, so the two
   halves of each root lie on one surface and merge back into one face.
   The one space where the ring of copies closes keeps its root in two
   faces.
4. **Names.** The steps' histories are composed into one, from the blank and
   the spaces to the wheel, so faces are named as one boolean would name
   them ([toponaming.md](toponaming.md)). What the
   replication never touched (a bore, holes in the web) keeps its name.

The result is checked: BRepCheck, one solid, and a removed volume that is the
sector's loss z2 times over, to a part in a million. Against the cut of all
forty spaces at once, the volume agrees to 1e-7, and 400 points through the
teeth classify alike.

When replication is refused or its check fails, the forty spaces are cut at
once, as before. That cut is not checked by `BooleanRefusal`
([README.md](README.md#a-boolean-is-checked-before-it-is-kept)), whose
point-in-tool test misjudges a compound of lofted B-spline solids. Measured,
it named a point on a tooth's crest as inside a space, on a result that
passed BRepCheck and whose volume matched a plain cut's to the last digit,
and its fuzzy retry cost a further minute. What is checked instead is what
every wrong result measured did get wrong: BRepCheck (each one failed it),
one solid, and a removed volume between nothing and every space whole. A
cut that fails this is tried again with the sections moved (two more), then
with 16 points a flank, before the wheel reports the failure as its error.
The throat's cut is an ordinary checked boolean.

Timings on the head's pair (one core, an idle machine), for the whole
feature:

| | teeth's cut | wheel |
|---|---|---|
| before: own knots, cut whole | 15.0–15.3 s | 15.4 s |
| shared knots, cut whole | 10.1–10.6 s | 10.7 s |
| shared knots, replicated | 2.0–2.2 s | 2.3–2.5 s |

Of the replicated cut's 2 s, the sector takes 0.5 s, the glue 0.7 s, the
check's volume 0.6 s and BRepCheck 0.15 s. A blank drilled through its web
adds 0.8 s for fusing the rest back on. The report's `teeth_cut` says which
way the teeth were cut (`replicated` or `whole`).

**Reuse.** The last four wheels built are kept by their exact inputs: the
blank's body (the same shape, not an equal one), its names, and every
number the wheel reads. Undoing or redoing an edit to a wheel, or switching
a configuration back, gives the inputs back unchanged. The regeneration
cache has dropped the wheel by then, since it keeps only the one history it
last ran, so the wheel would otherwise be built again. The report's
`reused` says so. An edit after the wheel never rebuilds it (the cache keeps
everything before the edit), and an edit before it changes its inputs.

The wheel still regenerates on the main thread. Regeneration is synchronous
by design ([../ui/long-operations.md](../ui/long-operations.md#what-is-not-on-the-registry)), and at 2 s
the wheel no longer reaches the frame watchdog's 3 s.

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
number of sections, how long the hob took, how the teeth were cut
(`teeth_cut`) and whether the wheel was `reused`
([how the teeth are cut](#how-the-teeth-are-cut)).

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
  must clash;
- `[slow]`: teeth cut by replication against the forty spaces cut at once
  (volume, faces, 400 points classified, the history), and a blank drilled
  through its web split, replicated and its holes' volume exact;
- `[slow]`: a drilled wheel replicated with every face named once and the
  holes keeping their names; a hole through the rim falling back to the
  whole cut; an undone wheel edit coming back from its inputs.

GUI: `worm/editor_picks_the_blank_and_sets_hand` and
`worm/wheel_editor_reports_the_pair`, plus the `defaults/worm` and
`defaults/worm_wheel` tool cases.
