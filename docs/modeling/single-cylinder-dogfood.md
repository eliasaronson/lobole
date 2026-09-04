# The single-cylinder engine

The acceptance test for the agent-scale plan (docs/agent-scale-plan.md): one
running engine, built by a script through the RPC surface, checked by the app
rather than by eye. `generators/single_cylinder.py` builds it.

    ./tools/headless-preview.sh <scratch-port>
    python3 generators/single_cylinder.py --port <scratch-port>

Seven parts — block, crank, rod, piston, head, valve, camshaft — in one
assembly, on a real bore and stroke (86 × 86 mm, 150 mm rod). The valve is
placed twice, and four ISO 4762 cap screws hold the head down, each derived
from the hole it goes in rather than specified. Every dimension is a variable, every mate is between named
frames, and no coordinate in the script was transcribed from a measurement of
something the script had already built.

## What it produces

| | |
|---|---|
| build | 224 RPC calls, 2.0 s |
| motion study | 49 samples over 720°, 735 pair checks, 3.9 s |
| piston stroke | 86.000 mm against 2R = 86.000 |
| valve lift | 8.97 mm from a 9.0 mm lobe, both valves (the peak falls between samples: 15° of crank is 7.5° of cam, and the nose is at 95°) |
| exact re-check | clean, 27.9 s (the mesh pass said clean in 3.9 s) |
| animation | 12 nodes × 49 keyframes, a 10.6 MB GLB |
| head bolts | M10 × 80, derived from a grip of 85 (head 60, block 147) |

The crank turns 720° because the camshaft runs at half crank speed on a chain
relation; two revolutions is one full valve cycle, and a study that stopped at
360° would exercise each lobe once instead of confirming it repeats.

## What the gates caught

Everything below was found by the app, not by reading the render. That is the
claim the acceptance test exists to support, so each one names its reporter.

**The loop solver was not holding the driven DOF.** Setting the crank to 90°
read back 67.4°, the piston wandered up to 15.7 mm off the analytic
slider-crank, and 360° did not return to 0°. In a closed loop the driven
value is just another unknown in an underdetermined least-squares, so the
solver was free to trade it away. Fixed by pinning it as a residual row
(`LoopPin`, `assembly::Driven`). Worst deviation afterwards: 0.000008 mm.

**Every bolted joint read as a clash**, which made `clean` unreachable. The
sweep was checking pairs that cannot move relative to each other. The study
now records placements in its first pass and skips rigid pairs, reporting how
many it skipped (49 of 784 here).

**A 9 mm cam lobe produced 10.86 mm of valve travel.** The lift table was
interpolated with a Catmull-Rom spline, which dipped 1.86 mm below zero in the
dwell and lifted the same amount past the nose. Replaced with a periodic
monotone cubic, which cannot overshoot a table it interpolates. Both valves
now read 8.97 mm.

**A scrub could land on the mirror image of the mechanism.** A slider-crank
at 180° has the crank and rod collinear, and both branches close the loop
exactly: the piston at 107 mm and the piston at −193 mm are equally valid
answers to "crank at 180". Jumping a quarter turn straight into that
singularity put the piston out through the bottom of the crankcase and
reported a clean solve, because it is one. The run never had this problem —
it walks its samples in order and each solve starts from the last — so the
scrub now walks to its target at the study's own spacing. It costs one run
sample per step and is bounded by the study's sample count.
`tests/scrub_smoke.py` holds ten poses, jumps and reversals included, to a
micrometre of the closed-form slider-crank.

**A fastener could not be derived from an ordinary M10 clearance hole.** An 11
mm hole came back as "ISO4762def does not stock M9". Reading a diameter
backwards through the clearance table is ambiguous — 11 mm is an M9 Loose and
an M10 Normal — and the lookup returned whichever the table listed first,
which is always the smaller thread on the looser fit. 46 of the table's 111
diameters are ambiguous, so this was a coin toss dressed as a measurement on
41% of holes. It now refuses naming every candidate, and `fit_fastener` takes
a `size` to say which; a size given is checked against the hole rather than
believed. Drilling with a Hole feature, which records the fit it was made
for, needs no `size` at all.

**A cosine lobe starting at 200° for 150° refused itself**, because the
generated angles ran past 360. Every generated angle is now wrapped.

**Finding the bolt rims switched the tab out from under the fastener.**
Querying a part's edges activates that part, so the `fit_fastener` call right
after was refused with "a fastener goes in an ASSEMBLY" although the caller
had never left one. The SDK's assembly handle grew a self-activating `bolt`,
like its other verbs, and `_id` learned to accept an occurrence.

**The SDK did not re-activate its tab.** Building a part after the assembly
existed, then placing it, failed with "the active document is not an
assembly" — the handle knew which tab it belonged to and never said so.

And eight geometry errors of my own, each caught as an interference rather
than by looking: a cut extruded away from the part (`Plane.xz`'s normal is
−Y), bolts outside the rod's width, the big end hitting a full-length main
journal, the rod passing through the piston crown, a valve mounted upside
down, a valve cone wider than its own guide (exactly 161 mm³), the rod
grazing the bore mouth, and a valve spring buried in the head's material.

The last of those is worth its own line, because the fix is a real feature and
not a fudge: the spring compresses by the lift, and with nowhere to go its
bottom coils were inside the head's top face at every angle. A head has a
spring seat recess, so the part got one. The first attempt cut 20 mm of fresh
air above the top face and the app said so — `cut removed no material` — which
is the same plane-orientation mistake as the first one in the list.

## What it does not do

The block and head are plain boxes. No ports beyond a bore, no water jacket,
no fins, no bearing shells. The rod's bolt holes are holes, not bolts —
only the head is actually bolted. The point of the exercise was whether the app can
hold a running mechanism together and say honestly when it cannot, and the
massing is what the motion study needs to be true about.

The drawing was the one deliverable this test could not produce: a three-view
sheet had not finished after 21 minutes. It does now, in about three seconds.

The cause is worth knowing rather than treating as "assemblies are slow",
which is what I assumed and what the measurement then contradicted. Exact HLR
of this engine is 85.1 s a view; hide the two valves and it is **0.0 s**. The
two helical valve springs are the entire bill, and the other seventeen bodies
— block, crank, rod, piston, head, camshaft, four cap screws — project
instantly. Sixteen bored boxes project in 3.1 ms. `Hide()` intersects every
projected edge against every face, and a swept B-spline surface is orders of
magnitude dearer to intersect than a plane.

One of those springs was also modelled wrong: `Pipe` takes a `wall_thickness`
and the generator passed one, so each spring was TUBING rather than round bar.
The bore is a second swept B-spline surface, invisible and paid for on every
view. Making them solid took the engine's exact sheet from 85.1 s to 47.4 s
and its animated GLB from 10.8 MB to 1.6 MB.

So the sheet is projected with `quality="fast"` (polygonal HLR — a look, not
a dimensionable deliverable), and `--exact-drawing` takes the time. The other
way out, for a real drawing of this engine, is to leave the springs off the
sheet. See docs/agent-scale-followup-plan.md, W3.
