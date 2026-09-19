# CAM hardening — the nine the waveguide round left behind

Everything named as open at the end of the waveguide dogfood
(docs/cam/dogfood-waveguide.md), in the order it should be done. Four are
small, three are medium, two are workstreams that happen to be on the list.

Sequenced so the cheap, high-value ones land first and the two structural
ones last — the structural pair change the shape of `Operation`, and doing
them before the rest would make every other diff argue with them.

## 1. A material on the waveguide job — BUILT

`generators/waveguide_job.py` sets none, so `FeedsFor` reports "nobody
looked" and the chip-load half of the check never runs on the one job we
actually drive. One call, and it exercises a path nothing else does.

## 2. `outsideTravel` checks X and Y only — BUILT

`CamVerify.cpp:687` loops `for (int a = 0; a < 2; a++)`. A Z move past the
machine's travel is unflagged — and Z is the axis a router has least of
(150 mm on the 6090 against 600 and 900). `MachineWarning` does check three,
but against the PARTS' bounding box rather than the program's moves, which is
a different question.

Built, and then corrected by the first real job it ran on. Z is a SPAN and
not a position. In X and Y the bed's near corner is assumed to sit on the
stock's near corner plus `reach_offset` — a stated assumption a person can
correct — and there is no equivalent in Z, because where the machine's zero
sits relative to the workpiece is what the operator sets when they touch off,
AFTER the file is written. Anchoring the box's floor to the stock's underside
anyway reported the waveguide's five profile moves at Z-0.5 — a through cut
into the spoilboard, the most ordinary thing a router does — as "1.0 mm past
its Z travel". So the Z question is the one that needs nothing typed: a
program whose deepest point and highest retract are further apart than the Z
travel cannot run at any touch-off height at all.

## 3. The V-bit refusal asks the table — BUILT

`CamDerive.cpp:126` refuses a cone for `Pocket || Face || Adaptive` by
enumeration — the last op-type predicate in the CAM core the capability table
does not answer. A `NeedsFlatFloor` column.

## 4. Stepover against scallop — BUILT

Named in `surfacing.md` and never written: halving `stepover` must halve the
cusp the verifier measures, not merely change the path count. The one property
of a finishing pass that no current test pins.

## 5. Retract into cut material — BUILT

The waveguide's 121 rapids through material are worst 0.787 mm at r=57.4, and
the cause is known: a raster's passes alternate, so at the end of a pass one
neighbour is cut and one is not, and a vertical climb puts the ball's rim in
the uncut one. Worst where the surface is steep.

The fix is to step one stepover TOWARD the cut side before climbing. With a
zigzag that side is always known — it is the pass just finished. Verified by
the count going to zero on the waveguide, which is a number we now have.

## 6. The simulation re-meshes its stock every frame — BUILT

`update` is 95.8 ms of a 126 ms frame while a simulation plays. `CamPlayback`
already gates the remesh on `CamStockSim::Revision()`, and that gate correctly
never fires: during playback the stock really does change every frame. A
210 mm blank on a 0.25 mm grid is a ~700,000-cell height map and
`BuildStockMesh` walks all of it.

Three ways out, cheapest first: rebuild at a capped rate while scrubbing
(the eye cannot use 60 Hz of stock surface); remesh only the region the cursor
crossed; or coarsen the grid while playing and refine on pause. The first is
a dozen lines and probably enough.

## 7. `CamToolpath.h` is five headers — BUILT

522 lines: sections and depth passes, generation, stock bounds and origin,
three kinds of pick, derivation, and the stepover/stepdown derivations. A
header-only reorganisation, cheap but wide — batch it with other header work,
because it costs a full rebuild.

## 8. The swept shank and holder envelope — BUILT (v121)

`docs/cam/simulation.md` S6, designed and unbuilt. Today's reach check
compares a cut's DEPTH against the flute length, which catches a cutter too
short; it cannot see a holder fouling a feature the tool never cut into — a
boss beside a pocket, a clamp, a tall neighbour on the same sheet.

`TipHeight` generalises to a radius-above-tip profile for the whole assembly,
and a cell whose stock stands above that envelope is a collision, reported
with its G-code line — the same shape as `rapidsThroughStock`. It needs
holder geometry the `Tool` record does not carry: an overall length and a
collar diameter, both of which a person knows and can type.

Built as written, with one narrowing worth stating: the envelope is checked
only OUTSIDE the cutter's own radius. Inside it the material is either cut —
and the height map records that it was — or it is a plunge deeper than the
cutter is long, which the reach finding already states at the setup. Two
findings for one fault, in two vocabularies, is how a check stops being read.
So this is the holder check and not the shank check; the shank's question was
already answered.

Silent unless both numbers are stated, and refused if only one is: half a
holder disables the check while looking like it enabled it. The glyph draws
the stated collar, so it is watchable and not only a line number. See
[simulation.md](simulation.md) S6.

## 9. `Operation` is a union with no discriminant

~10 of its 25 fields exist for one op type each. The capability table added in
phase 0 answers "does this type have this field" in one place, but the FIELDS
are still all there, on every op, and a surfacing op carries tabs it refuses.

FreeCAD's answer is the cleanest idea in that workbench: tabs, dogbones,
boundaries and lead-ins are separate wrapper objects an operation is decorated
with, so an op that cannot take tabs is simply never wrapped and there is
nothing to gate. That is the shape to move toward, and it is a format change.

## Order and cost

1-4 are small. 5 and 6 are medium and both have a measured number to hit.
7 is cheap but wide. 8 and 9 are workstreams: 8 wants new `Tool` fields and a
new pass over the height map, 9 wants a format change and touches every op
type. They are last because the rest should not have to argue with them.

1-8 are done. 9 is the only one left, and it is the one that is genuinely a
workstream rather than a task with a workstream's shape.
