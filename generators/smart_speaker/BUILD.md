# Building this cabinet

What the model cannot enforce. Each of these is something the run checks, or
cannot check, and that the finished box depends on.

Run `python3 generators/smart_speaker/smart_speaker.py --port <n>` for the
parts and the G-code; `modal.py` for the panel modes. Design intent lives in
`~/Documents/speakers/PLAN.md` — where this disagrees with that, this is
newer.

## The glue lines ARE the design

**Bond the plywood skin over its whole face.** 3.6 mm birch ply over 16 mm
MDF is 1.94x the bending stiffness of the 18 mm MDF the plan called for, and
panel modes land 35% higher — but only bonded. The neutral axis moves into
the ply and the gain is entirely shear transfer through the glue:

| | bending stiffness | panel modes |
|---|---|---|
| 16 mm MDF bare | 1229 | 1.00 |
| 18 mm MDF bare | 1750 | 1.13 |
| 16 + 3.6 ply, bonded | 3395 | 1.52 |
| 16 + 3.6 ply, **unbonded** | 1268 | 1.02 |

Unbonded the skin is worth 3%. Roller or notched spreader, full coverage,
weighted or bagged. A bead round the edges buys nothing structural. (A
deliberately flexible adhesive would give constrained-layer damping instead
— you get one or the other, and with this bracing the stiffness is worth
more.)

**Glue the braces to the panels they butt against.** Every brace in this
cabinet meets a continuous face, so nothing in the model can hold them: the
ring brace's four edges, the ladder's perimeter, and the three studs. The
studs are the only members that shorten the side walls' governing span, and
they were 2.5 mm clear of the walls until a modal study went looking. With
everything bonded the side walls' worst region is ~550 Hz; with the skin as
well, ~830. Unbonded, 311.

## The port

**Cut the tube long and trim to a measurement.** The table says 221 mm,
which is where the computed fb lands on its 33 Hz target — but the flares'
effective length (`port_end_correction`, 183 mm) is derived from PLAN.md's
"25 cm doing the work of 36 cm", not measured off the part. The impedance
saddle between the two peaks on the finished box is the only thing that
settles it. Cut at 250, measure, trim.

`verify` reports the tube at 0.884 of the reference STEP and that is
correct: the reference is the 250 mm as-drawn tube.

**The flares are a sketch of a part you are buying.** Measured off the
model: an R25 arc, 84 degrees, tangent to the bore, d100 opening to d145
over 24.9 mm. The tangency is right and R25 is about what the 210 mm
internal width allows (the reference optimum is nearer 0.5D, so R50, which
needs a d200 mouth). It does not matter for turbulence — PLAN.md computes
8 m/s peak where even an unflared port survives 17 — but it sets the TUNING,
which is why the paragraph above exists. When the real Precision Port
arrives, measure it and update `port_end_correction`.

**The tube is a line-to-line d105 fit into both flares.** Zero clearance by
design; two real parts at one nominal diameter do not assemble. Sand, or
plan 0.2-0.3 mm.

## A guide for the edge dowels

The deep dowels go into the uprights' 16 mm END faces, which no flat setup
reaches, so they are drilled by hand. The numbers make a single guide serve
every one of them:

- they sit at v +/-461 on a panel spanning -611..511, so **50 mm from the
  top end and 150 mm from the bottom end**;
- and at 8 mm from the inside face, which is dead centre of a 16 mm panel.

So: a strip **200 mm long with one hole 50 mm from one end**, centred across
its width. Butt it against the top end and the hole lands on the top dowel;
turn it round and butt it against the bottom end, and 200 - 50 = 150 lands
on the bottom one. One guide, four holes per upright, eight for the pair,
and the same guide for both panels.

Cut it from 16 mm offcut for 16 mm of drill guidance, with a groove in the
underside to straddle the panel's edge and register on the inside face.
Drill the guide hole on the machine, not by hand — the whole point is that
it is more accurate than what it guides.

## What the CNC job does not cut

`cam.py` posts fourteen operations, all on the BAFFLE's front: two flange
recesses, two through openings, ten screw holes. `cam_back.py` adds four
more programs — each upright's back face, in two setups apiece — carrying
the divider dado and the face dowels.

Still not cut, by anything:

- any panel outline, or the plinth's foot profile;
- the side walls' and the divider's own dados and face dowels;
- the T-nut holes in any panel but the baffle;
- **the edge dowels**, which is a drill's job — see the guide above.

The baffle is 1122 mm against the 6090's 900 mm of travel, so the plinth end
hangs off the bed; every machined feature is in the upper two thirds and
within reach, which is what `reach_offset="auto"` settles.

## The one CAM warning still worth a decision

Four of the job's warnings are about chip load, and three of those are now
answered by using the speeds the shop settled on (6000 rpm on the 6 mm,
9000 and 1100 mm/min on the 3 mm — the generated job had 12000 on both,
which is half the chip load). The remaining one is not a number, it is a
cutter:

> T1 (6 mm, 4 flute) cuts a slot 16 mm deep in sheet goods — over 2.8
> diameters, full width. More than two flutes leaves little room to carry
> that dust out, and packed flutes burn and grab.

That is the through-opening profile, and the advice is sound: a 1- or 2-flute
cutter is what sheet goods want, and on a machine that tops out at 1500
mm/min a 4-flute 6 mm tool cannot reach its chip-load band at any feed
(it would need 5760 mm/min at 12000 rpm). The tool table declares 4 flutes
because the ported job did. If the cutter actually in the collet is a 1- or
2-flute spiral — which is what the ported job's own spare "8 mm flat 2F
(flange recesses)" suggests you were already thinking about — then change
`flutes` in `cam.py` to match it and the warning goes with it.

The reach warning is real and already handled: the baffle needs 1122 mm of
Y and the 6090 travels 900, so the plinth end hangs off the bed. Every
machined feature is in the upper two thirds and within reach, which is what
`reach_offset="auto"` settles — but check the clamping before pressing
start.

## Damping

**Stuff the Alpair chamber properly, not lightly.** Its volume does not
matter — the driver is high-passed at 250-400 Hz, so 8 L gross (Qtc 0.73,
fc 96 Hz) and the plan's 4.5 L (Qtc 0.84, fc 110 Hz) are both far below the
crossover. What matters is the chamber's DEPTH mode: 320 mm gives a
half-wave at 536 Hz, inside the Alpair's passband, and no change to the
chamber's height moves it.

PLAN.md's 3 cm of foam on the rear wall behind the woofer stands: 854 mm of
chamber puts the height-axis mode at 201 Hz. It also adds ~5% effective
volume, which nudges fb down a fraction.

## Numbers worth having at the bench

| | |
|---|---|
| woofer chamber, gross | 57.39 L |
| ...without the port hardware | 55.57 L |
| ...net Vb, what a simulator wants | 53.3 L |
| port | d100, 221 mm tube, down-firing |
| fb | 32.9 Hz (checked by `verify.tuning`) |
| W170S bolt circle | **175.8** — Visaton's drawing, not 175.0 |
| Alpair bolt circle | 112, five holes, d3.1 |
| panel modes, bare 16 mm MDF | side wall 311 Hz, baffle skirt 850 Hz |

The bolt circle is called out because it was wrong in this model until the
datasheet was read: the driver's own holes are d4.4 on M4, 0.2 mm of radial
slack, and a 0.4 mm error in the circle binds all five bolts.
