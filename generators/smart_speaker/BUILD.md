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

## What the CNC job does not cut

`cam.py` posts fourteen operations, all on the BAFFLE: two flange recesses,
two through openings, ten screw holes. It does not cut

- any panel outline, or the plinth's foot profile;
- the divider's dados;
- the T-nut holes in any other panel;
- **the dowel holes** — and those are the accuracy-critical feature. The
  whole carcass locates on them, `joinery.py` exists because they did not
  line up, and there is no CAM for them. Jig or a second job, before
  glue-up day.

The baffle is 1122 mm against the 6090's 900 mm of travel, so the plinth end
hangs off the bed; every machined feature is in the upper two thirds and
within reach, which is what `reach_offset="auto"` settles.

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
