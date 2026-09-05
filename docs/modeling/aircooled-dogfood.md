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
