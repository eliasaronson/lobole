# The air-cooled single: sketches only

A second pass at the single-cylinder engine
(docs/modeling/single-cylinder-dogfood.md) with one rule: **no Box, Sphere or
Cylinder**. Every body starts as a drawn profile that is extruded or revolved.
Same mechanism — 86 × 86 bore and stroke, 150 mm rod, two valves on a 2:1
chain — so the kinematics were already proven and the round is about shapes.

    ./tools/headless-preview.sh <scratch-port>
    python3 generators/aircooled_engine.py --port <scratch-port>

| | v1 (primitives) | v2 (sketches) |
|---|---|---|
| build | 224 calls, 2.0 s | 317 calls, 1.6 s |
| motion study | 735 checks, 3.9 s | 550 checks, **0.69 s** |
| animated GLB | 10.8 MB (1.6 after the spring fix) | 2.0 MB |
| stroke | 86.000 | 86.000 |
| mass | — | 13.93 kg, every part (21.47 with the bottom end) |

More calls, as expected — a profile is more drawing than a primitive. The
motion study got **5.7× faster** because the parts are the shapes they should
be: a revolved piston has one lateral face where a cylinder-minus-three-grooves
has a dozen, and the collision mesh follows.

## What the round found

**A sketch refuses loops that overlap.** Drawing a rod as "big-end circle +
small-end circle + shank polyline" in one sketch is refused — *"profile loops
overlap or nest inconsistently"*. So a silhouette made of overlapping shapes
becomes one extrude per shape, each `mode="add"`. That is more calls and no
guessing about which loop was meant to be a hole; it is the right refusal, and
it is worth knowing before you design a profile around it.

**A slot's end cap is half its width past the centre it ends on.** The crank
web is a `slot` from the main journal to the pin, which is what a web actually
is. At 90 wide round a pin 43 up it reached z=85 — and the barrel's underside
is at 82. The motion study called `barrel x crank` at seven of 49 samples.
A disc centred on the axis (what v1 used) hides this class of error, because
the shape only grows where you are looking.

**Fins put material on a bolt axis.** The head bolts are on a 65 mm circle,
inside the deck flange but outside the barrel wall, so the first blind hole
bottomed out 3 mm into the topmost fin: *"the axis runs into barrel 31 mm
in"*. Adding a boss was not enough — the fins still cross the bolt line all
the way down. A real finned barrel is cast exactly the way the fix models it:
the fins stop either side of each through-bolt spigot.

**Four bolt bosses hanging in mid-air, and nothing complained.** The first
version relieved the fins with a d=38 cut at each bolt and then added a d=30
boss. The relief reaches r=46 and the boss only r=50, so it ate four
millimetres of the very wall the boss had to land on: four pillars hanging off
the deck flange with daylight behind them. Every check passed — one solid
body (the flange joins them at the top), the study clean, the bolts fitted —
because none of them asks "is this shape sensible". It took a person looking
at the part. The boss now runs the barrel's full height and overlaps the wall
by two millimetres, which is what a through-bolt spigot actually is, and the
fins run into it.

**A blind hole at the end of a truncated joint was refused** — and this one
was a defect, not geometry. The head bolts stop in a blind hole 30 mm into the
barrel, and their axis carries on 158 mm further into a crank web they do not
touch. `far_end` is the documented way to say where a joint stops, but the
blind-hole check consulted the raw stack's LAST segment rather than the last
one inside the joint, so it refused even with a far end given. The stack stays
untruncated on purpose (the washers and the nut live in it); the check now
asks the right question, and the refusal names `far_end` when there isn't one.
Fixed with a case in `tests/fastener_stack_smoke.py`. Same family as W1 in
docs/agent-scale-followup-plan.md: two pieces of code disagreeing about where
a joint ends.

## Round two: a bottom end and a timing drive

The engine had no crankcase — the barrel floated above a crank held by nothing
— and the camshaft was driven by a relation with no geometry behind it. Both
are now real: a crankcase with a rounded sump that the crank runs in and the
barrel bolts to, a crankshaft with mains that span the case's end walls and a
nose for the drive, and a chain running up the side between an 18-tooth crank
sprocket and a 36-tooth cam sprocket.

Twelve parts, 21.47 kg, 895 pair checks over 720° in **1.45 s**, clean.

Everything below was found by the study or by a refusal. Not one of them was
found by reading the script.

**The drive frames were on the wrong axis.** `origin=(0, 0, DRIVE_Y)` puts the
timing drive a hundred millimetres BELOW the engine instead of outboard of it,
and the study reported the chain hitting the valves — a true statement about
where the chain was. `(0, DRIVE_Y, 0)`.

**An extrude from `Plane.xz(y)` travels toward −Y**, which the file already
said, and the mains were still drawn from the wrong end: `Plane.xz(-74)` with
a length of 42 puts the left main at y −74..−116, entirely outside a case that
ends at −78.

**The case has to clear the rod's SWING, not the crank.** The big end orbits at
43 and is 35 in radius, so it sweeps a circle of 78 — and at a half-depth of
82 with 14 mm walls the void was 68 and the rod ground the wall at 20 of 49
samples. `CASE_X` is now derived from that sum rather than typed.

**Clearances tighter than the collision mesh's own deflection read as
contact.** A main bearing 0.5 mm over its journal is 0.25 mm radial; the mesh
deflection on a part this size is 0.39 mm, so `crankcase x crank` fired at all
49 samples. That is the mesh being honest about its resolution rather than
about the engine. Mains, sprocket bores and the chain's inner face are all
opened to 1.5–3 mm, which the comments now say outright.

**A slot's reach is what sweeps.** The crank webs and counterweights are slots,
and what matters is how far they reach from the AXIS, because that circle is
swept once a turn. At 64 and 90 wide they reached 75 and 79 into a case whose
void stops at 68, and ground through the top plate at 22 samples. The widths
are now set by that arithmetic — 44 and 60, clearing by 3 and 4 — and written
down beside it.

**A `Pattern` repeats a feature that MAKES A BODY.** Patterning a sprocket
tooth added to the rim is refused ("source body not found"), which is fair.
The teeth are drawn instead, in one sketch from the tooth count: no less
parametric, one extrude rather than a body per tooth and a Boolean to put them
back.

**A cut has to go all the way through the thing it is cutting.** The sprocket
rim spans −6..+6 about its own origin and the bore was cut from +14 for 16,
stopping at −2 and leaving a solid back for the shaft to run into.

The new `islands()` was used to check the new parts rather than trusting them:
the crankcase reads as one region at the deck and TWO at mid-height (its two
side walls, correctly separated by the void), and the chain reads as two at
every height — its two runs. Nothing unexpected, which is the answer you want
from a check.

The chain is frozen geometry, drawn with running clearance on the teeth rather
than riding them. A chain modelled where it really sits would be reported as a
clash at every sample — a true statement about a deliberate simplification, and
belt paths that follow their sprockets are explicitly out of scope.

## Round three: valve gear that works, and breathing

The camshaft ran along Y while the valves sat across it, so no lobe was ever
over a stem. Two constraints settle the layout together: a cam can only press
what is under it, and the cam must be parallel to the crank for a flat chain
to reach both sprockets — so the valves moved fore and aft to (0, +-22).
Added spring retainers, cam bearing towers with OPEN saddles and the caps that
close them, and then an intake and an exhaust.

Fourteen parts, 22.97 kg, 977 pair checks over 720 degrees in 1.8 s, clean.

**The lobe is drawn from the relation's own lift function**, and it took three
goes. As a base circle plus a slot it disagreed with the table twice: a slot
reaches `offset + width/2`, so its tip stood at 20 against a 15 base circle —
5 mm of drawn lift where the relation lifts 9 — and even sized right, a slot's
flanks are arcs where the relation's lift is a cosine, so the drawn cam pushed
lower than the valve had moved at 16 of 49 samples. Now each point sits at
`CAM_BASE_R + lift(a)` placed at `180 - a`, and measured against the relation
the two agree to 0.01 mm at every angle. The sign of that phase was settled by
reading the study, not by deriving it.

**The camshaft stopped 2 mm inside its sprocket**, so the bore showed daylight
and the drive read as unconnected however correct the mate was — spotted in a
render while every measurement said fine.

**Two OCC booleans failed by returning nothing.** Neither raised an error;
both just made the head cease to exist, leaving a lone cam tower as the only
body. A port throat cut after the guide bore, meeting it at right angles
through the same point, was the first — cutting the throats before the valve
gear fixes it. The second was subtler: both throats at the same height are
COAXIAL cylinders with a 20 mm plug of head between them, and the second
subtract failed. Staggering them (26 and 36) makes two independent cuts. A
boolean that fails loudly is a bad afternoon; one that succeeds and returns
emptiness is a worse one.

**A part grows along its frame's Z.** The carburettor's flange frame pointed
back down its own bore, so aligning it to the head's port mated the whole
carburettor INTO the head — `carburettor x valve` at every sample, quite
correctly.

**The timing drive has to clear the intake, not just the case.** At y = -104
it was outboard of the crankcase wall and straight through the carburettor's
float bowl. `DRIVE_Y` is -150 now.

Two defects from this round were fixed in the app rather than modelled around:
a mesh clash where an exact boolean said clear (the containment fallback
tested a bounding box's CENTRE, which for a cap that straddles a shaft lies
inside the shaft), and a bounding box 60% too big on swept surfaces. Both in
`fix(collide,cad)`.

## Round four: the carburettor as a SUBASSEMBLY

Seven parts — body, throttle spindle, butterfly, lever, return spring, float,
main jet — in an assembly of their own that the engine places as ONE
occurrence. A carburettor is the natural shape for the feature: nobody thinks
of a float bowl as part of a cylinder head, and the throttle turns inside the
body whether or not the engine is running. Styled on a Solex H-type: bell
mouth, fixed venturi, butterfly below it, integral bowl beside the barrel.

Twenty-one BOM rows, 23.14 kg, still clean over 720 degrees.

**A subassembly mates by its own ORIGIN.** It has no frames or faces of its
own — the app says exactly that — and a mate connector will not take a path
into one either. So the thing has to be BUILT around the origin you mean to
mate: the carb body is fixed at its assembly's origin with the flange face
there, and the engine then needs nothing but its own port frame.

**The three base planes do not extrude the same way.** `xy` goes +Z, `yz` goes
+X, `xz` goes −Y. Assuming the two positives behaved like the third put both
of the body's cross-drillings outside the part — the spindle bore ran from
x=44 to x=132 and touched nothing, so the spindle was mated into solid metal,
and the jet duct said so out loud ("cut removed no material"). Measured on a
box rather than guessed a second time.

**A butterfly is slotted for its spindle, not drilled** — the spindle lies
across the disc's face. And its mating frame's Z runs ALONG the spindle, which
lies *in* the disc's plane; giving it the disc's own normal mates it broadside
across the barrel.

**The float bowl was floating**, 12 mm clear of the barrel it belongs to, with
`check()` green because a float legitimately touches nothing. `islands()`
found it in one call — two regions at every height, the barrel and the bowl —
which is the same shape of error as the bolt bosses and the reason that check
exists. A cast web joins them now.

**A float floats and a spring hangs.** The assembly check is run here with
`connected=False` and `clearances=False`, and both are deliberate: nothing in
a bowl is supposed to touch, and every near miss left is a fit that was chosen
— the butterfly clears its bore by 0.6 mm, the spindle by 1.0. What stays
checked is clashes, which is the half with something to say about a
carburettor.

The throttle really turns: driving its mate from 0 to 90 degrees takes the
metal across a horizontal section through the bore from 2701.8 mm² to 2194 —
the butterfly going from lying across the bore to edge-on in it.

## Round five: a throttle mechanism, not a throttle

Four more parts — bellcrank, link rod, idle stop screw, mixture screw — plus
a linkage bracket, a pivot boss and a gusset on the casting. Eleven parts,
25 BOM rows. The point is the loop: body → spindle → lever → link rod →
bellcrank → body, four revolutes, so the carburettor stops being a tree with
one turning joint and becomes a four-bar the A6 solver has to close. It
closes to 1.6e-07 mm.

**Four wrong diagnoses before the right one, and they were all geometry.**
The loop would not close, and each residual looked like a different bug: "off
by 68 mm" (link rod pointing away), "off by 6 mm" (the two crank pins in
different planes), "off by 387.9" then "418.1" (the bellcrank arm reversed).
Every one of those was a real error and fixing it moved the number. The cause
of the *last* one was none of them: the closing mate's two axes pointed
OPPOSITE ways, so the solver had to spin the rod end for end to satisfy the
axis, which swung its far eye off the crank. `flipped=True` on that one mate
took 418.1 mm to 5e-06. A coupler in a four-bar is turned end for end
relative to the crank it drives — the flip is the normal case, not the
exception, and nothing in the residual says "axis" rather than "position".

**Seeding the poses made it worse.** While the loop was failing I placed the
new occurrences near where they belonged with `at=`. That is exactly wrong:
the mates already put every pin where it belongs — measured, both crank pins
land coincident with the link rod's eyes — and an `at=` only moves the
starting pose away from the answer. A loop that will not close is a geometry
error every time; it has never once been a solver wanting a hint. The seeds
are gone.

**A four-bar that closes is not yet a throttle.** Every mate can be right,
the loop can solve, and the linkage can still transmit nothing. So the
generator drives the far end and reads the near end: turn the CABLE arm 60
degrees and section the assembly horizontally at spindle height. The metal
that plane crosses falls from 3053.2 to 2480.0 mm² as the butterfly goes from
lying across the bore to edge-on in it. That is the check — the residual only
says the bars fit together.

**The intended contacts had to be named.** Four pairs are supposed to touch:
a crank running on its pivot boss, a link rod lying against the lever it
drives, two screws entering the mouths of their tapped holes. `check()` has
no way to be told a contact is deliberate, so the script lists those four
pairs and still fails on anything else. Worth noticing as a gap: "these two
are meant to touch" is a property of the model, and it currently lives in the
script that built it.

**Render before believing.** The return spring passed every check — a spring
is supposed to touch nothing — and rendered hanging in clear air beside the
casting, because its post was a frame on the barrel rather than on the new
bracket. Same shape of error as the floating bolt bosses and the floating
float bowl, and again only a picture found it. Three rounds, three of these.

## Round six: what looking at it found

Four defects, all of them reported by the person looking at the renders
rather than by any check the model runs.

**The exhaust had no hole through it, and this was the third time.** The
flange is a revolved profile, and the profile started at r=0 — so it swept
a solid disc with a pipe standing on top of it. Nothing complained, and
nothing could: a revolve to the axis is a legal shape, there is no cut to
report "removed no material", no clash, no island, and the part is one
clean solid. Every check the project owns is about material that should not
be there or should be joined; none of them is about a VOID that should be
there.

So there is now a check for it, and it is the dual of `islands()` and the
same one call: a plane CONTAINING a passage's axis cuts an open duct into
two lumps, one wall either side of the bore, and a blocked one into one.
`part.bored_through()`. The exhaust and the silencer both assert it.

**The mixture screw floated above its own hole, with its head inside the
bell mouth.** It was seated 6 mm above the web it screws into, which put
its head where the barrel flares. Both numbers came from writing the seat
height and the tapped hole's mouth separately; they are one number now.

**The linkage went through itself.** Three parts shared the plane x = -29
and interpenetrated by 130, 162 and 4 mm³ — while the loop closed to
1.6e-07 and `check()` passed. Two separate things were wrong:

- The four-bar was a parallelogram with both arms pointing ALONG the ground
  link, which puts the coupler on the line through both pivots. There was
  nowhere to put the bellcrank's pivot boss, so every attempt to space the
  parts apart drove one into another. Both arms point perpendicular to the
  ground now and the coupler runs 17 mm clear of both pivots.
- `check()` had an allowance for "intended contacts" that was no check at
  all: it raised only if NEITHER name in a pair appeared in the message, and
  every pair named the body, so nothing ever raised. It hid all six
  clashes. The allowance is gone; clashes are a failure, all of them. An
  escape hatch written to admit two known cases admitted everything, which
  is exactly the failure mode the project's own "refuse, don't substitute"
  rule is about — and it was in the script doing the checking.

The axial stack is now written down in one block of constants, as faces
measured outward from the barrel, instead of being deduced at each sketch.
A solved loop says the pins line up. It says nothing whatever about
thickness.

**A frozen spring is only as honest as where it is frozen.** The return
spring was hung off the bracket, 115 mm³ inside the plate at one end and in
clear air at the other. It is a coil round the spindle now, in the gap
between the barrel and the lever's hub, which is where a throttle return
spring lives. At radius 8 the bottom of every turn was 0.8 mm inside the
mounting flange — 594 mm³ of spring in solid aluminium, in the one place a
side view does not show it. Radius 6.5 clears.

**And the linkage is proved through a MOTION STUDY, not `set_mate`.**
Driving the loop mate a step at a time returned a clean solve and a
different configuration nearly every time — 2590, 2559, 2719, 2534 mm² of
bore at ten-degree intervals, one of them with the coupler lying across the
pivot it turns about. A study over the same range tracks it exactly: 13
poses, residual 0.0000 throughout, and a clash verdict monotone in the
angle, which is how the remaining interference (the bellcrank swinging down
into the flange) got found and fixed by reversing the swing. In
docs/known-bugs.md; the walk exists in `MotionStudy.cpp` and `set_mate` is
not using it.

The butterfly turns 66.1° for 60° of cable arm, and the linkage is clear at
all thirteen poses.

**Seeds, corrected.** An earlier note here said never to seed a loop's
starting pose. That was drawn from a loop with NO solution, where the
residual is a geometry error and a hint only starts the solve further from
an answer that does not exist. This loop has two solutions at every angle —
the linkage and its mirror — and both close it exactly. Unseeded, it
assembled crossed: residual 1e-06, dof 1, and the butterfly sitting edge-on
in the bore at rest. A seed is how the branch is chosen.

## A silencer, and folders

The header runs 100 mm past its bend into a silencer that slips over it —
one revolved profile, spigot, two cones, a can and a tailpipe, and
`bored_through()` on that too.

And the part tabs go into folders: Bottom end, Top end, Valve train,
Carburettor, Exhaust, and whatever is left into Fasteners. Twenty-seven
parts in six folders. `list_parts` returns every TAB, assemblies included,
so the first sweep filed the engine and the carburettor under Fasteners —
the two things in the project that are neither.


## Materials

Every part carries one, so the BOM has masses rather than nulls and the
viewport has something to shade:

| part | material | mass |
|---|---|---|
| crank | steel | 6348 g |
| barrel | aluminum | 3044 g |
| head | aluminum | 1913 g |
| rod | steel | 1129 g |
| camshaft | steel | 651 g |
| piston | aluminum | 401 g |
| valve ×2 | stainless steel | 146 g each |
| ISO 4762 M10x40 ×4 | steel | 38 g each |

Aluminium for the barrel and head is not decoration: the fins are there to
move heat into the air and the alloy is half the reason that works. The piston
is aluminium because it is the one reciprocating part whose mass costs you at
both ends of every stroke, and the valves are stainless because they sit in
the exhaust stream and have to stay hard hot. 13.93 kg is the core only — no
crankcase, no flywheel.

`Part.material(slug)` was added to the SDK for this. It is the one verb here
that MUST activate its tab: a material is written to the feature that created
the body, and writes stay on the active tab (W2).

## What it looks like

A finned barrel and head, four bolt bosses running the barrel's full height
with the fins running into them, shaped crank webs and counterweights, a turned
piston with a dished underside, a waisted rod, and egg-shaped cam lobes. Every
one of those came from a profile, and every dimension is still a variable —
change `FIN_PITCH` and the barrel re-fins itself.

The drawing is deliberately not part of this generator: an exact sheet of this
engine is dominated by its two valve springs, which is a known bug with its
own measurements (docs/known-bugs.md).
