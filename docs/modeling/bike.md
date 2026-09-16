# A roadster bicycle, from photographs

`generators/bike/` builds the owner's bicycle: a green Scandinavian gents'
roadster, 28" wheels, 3-speed hub, full mudguards, teardrop chainguard,
tubular carrier, north-road bars. Twenty parts, twenty-two occurrences, six
joints, and it moves — the wheels turn, the bars steer, and the cranks drive
the rear wheel through a chain relation.

    python3 generators/bike/bike.py [--port N] [--out DIR]
                                    [--stage parts|assembly|all]

About four minutes, of which three are the parts and most of that is the
validity check on two 36-spoke wheels.

## The thing this model is actually about

Every other generator here starts from numbers: a datasheet, a plan, a CAD
export. This one starts from three snapshots of a bicycle leaning on a hedge.
The question was whether the API lets an agent turn that into a document
whose numbers are all visible, all editable, and all honest about where they
came from.

The answer is `bike_spec.py`, which keeps two kinds of number apart and says
which is which on every line:

- **READ** — what the photographs establish. A gents' diamond frame with a
  horizontal top tube. 28" wheels, 36 spokes. **A 3-speed trigger shifter on
  the right bar**, which is why the rear hub is a wide internal-gear shell and
  the rear wheel is not the front one mirrored. One chainring under a full
  teardrop alloy guard. Alloy mudguards front and rear. A tubular carrier with
  a slatted top and a red reflector. Swept-back bars, white grips, a bell.
  Rubber-block pedals. A lot of seatpost showing.
- **ASSUMED** — every length, angle and tooth count, standard Scandinavian
  roadster numbers. Each is a project variable, so correcting one is an edit
  and not a rebuild.

One judgement worth recording because it is worth 1.4 kg: **the rims are
called aluminium**, not chrome steel. In the photographs they are a dull,
slightly rough silver, the same finish as the chainguard beside them, rather
than the mirror of chrome plate. A roadster of this vintage could have
either; `bike_wheels.RIM_MATERIAL` is one word.

## What the run refuses to save without

All of it measured on the assembled bicycle, not recomputed from the spec:

| check | what it caught |
|---|---|
| `asm.check` — no clash, nothing floating, no near miss under a millimetre | the chainguard bolted to nothing; the fork floating in a head tube 1.7 mm larger than its steerer; the saddle in the air above its post |
| wheelbase, axles level, chainline | — |
| **trail is positive** | the sign of its own formula: 61.7 mm of trail came out as -61.7 and read as unrideable |
| pedal clears the ground | — |
| mass between 11 and 24 kg | a solid-modelled tyre at 3.3 kg, a solid rim at 1.9 kg, a solid bell at 320 g |
| **a motion study, RUN** | the left pedal striking the chainstay 16 degrees past top dead centre — clear standing still |
| steering through ±38° | — |

The bicycle comes out at **16.9 kg**, which is what a steel roadster with a
carrier and full guards weighs.

## Three things worth carrying to the next frame-shaped part

**Weld solid, drill after** (`bike_shop.Weldment`). Three constructions were
tried for a mitred tube joint:

1. Hollow pipes, unioned. A tube whose end cap lands inside another tube's
   VOID is a cap between two surfaces, and whether it builds is a tangency
   lottery: a seat stay 11 mm off the seat tube's axis failed where 10 and 12
   passed.
2. Hollow pipes with a solid plug in each welded end. Better, and the plug's
   own lateral face leaves a sliver inside the wall — that body then fails the
   NEXT boolean it is an operand of. A chainstay at a dropout spacing of
   65.5 mm failed at a bow of 7 and 11 mm while 5 and 9 passed.
3. **Solid rods, unioned, and the bores drilled afterwards.** Solid rods union
   at any angle — there is no void for a cap to land in — and a bore is a
   cylinder subtracted from a big solid, which is the easiest thing a kernel
   does. The tubes come out hollow between their joints and solid through
   them, which is what a brazed lug is, and the frame builds in 3.4 s instead
   of 12.3.

**A part is not finished until `check_geometry` says `verified`.** The frame
passed "no feature errored" and "one solid" while carrying a body
OpenCASCADE calls *badly nested* — one axle bore drilled through both
dropouts in a single pass, crossing 130 mm of open air and entering material
four times. An invalid body poisons everything asked of it afterwards: the
assembly's interference sweep reported 17,297 mm³ of the rear cog buried in a
frame whose bounding box does not reach it, and a 78 mm probe disc 40 mm
outside the frame read as 98% inside it. The hunt for that phantom clash cost
more than the rear triangle did to build. `bike_shop.verified` now gates every
part on the receipt, and the bore is drilled one dropout at a time.

**`flipped` defaults to TRUE, and frame-to-frame mates want FALSE.** The RPC
says so; this round is what it feels like not to have read it. Left at its
default, every mate assembled and the result was a different bicycle: the fork
turned end for end about its own steering axis, carrying the front wheel up to
z = 771; the seatpost, saddle and carrier hanging under the frame; the chain
and its guard mirrored onto the wrong side.

## What the checks made the model tell the truth about

Three shapes in this bicycle are what they are because a check refused the
simpler version, and all three are what the real part looks like.

**The crank is 64 mm off the centre line, and that number is not free.** It
follows from a chain of clearances that the interference sweep and the motion
study worked out between them, and it is why a bicycle with a full chainguard
has a wide crank:

    chain           43 – 51 mm off the centre line (chainline 47)
    chainguard    53.5 – 55      just outside the chain
    crank arm       57 – 70      sweeping OUTSIDE the guard
    pedal           70 – 180     everything of it outboard of the arm's face

Drawn the other way round — the guard outboard of a 46.5 mm crank, which is
where this started — every one of those pairs is a collision, and they arrive
one at a time as each is fixed: the crank's arm through the guard's rim, then
the pedal's spindle through the guard's tail, then the pedal's spindle
through the chain. The guard's eye had to grow from 96 to 140 mm for the same
reason: the spider reaches from the ring's plane out to the arm's, so it
passes THROUGH the eye.

**The chainguard's tail is a strip over the chain's TOP run**, not the other
half of the chain's outline. A guard shaped like the whole chain loop has to
pass through the chainstay, because the stay runs between the chain's two
runs for its whole length.

**The brake caliper's arms go outboard before they go down**, straddling the
mudguard, and the mudguard has a hole for the caliper's bolt. Every roadster
guard has that hole for the same reason.

**A handlebar is one pipe with two bends in it**, and building it as three
tubes welded end to end shows every seam. Drawn as a chain — line, arc, line,
arc, line — in the plane the bar is bent in, it is one `Pipe` and one sketch,
and the bend angle is solved for rather than guessed: the spec keeps width,
sweep and rise, which is what a rider would measure, and the angle is
whatever puts the grips there. That is also the shape that found the sketch
bug below.

Two more from looking at the render rather than the checks: a grip bored
right through is a piece of pipe and looks like one (it wants a closed outer
end), and anything that clamps ON the bar — a lever, a bell — must be built
on the BAR's axis, or its clamp is a ring standing beside the bar rather than
round it.

One measurement worth keeping: **24 samples is not a fine enough sweep for a
crank.** At 15 degrees apart, the pedal moves 44 mm of arc between samples,
which is wider than the chainguard's tail — the study stepped over it. What
found it was sweeping the crank by hand at 5 degrees and asking `interference`
for an exact volume at each: the pedal's spindle through the chain, 621 mm³ at
215 degrees, and through the guard, 170 mm³ at 210. Worth doing whenever a
study says a mechanism is clean and the parts are thin.

## What it found in the app

**An open sketch path's edges did not share vertices, and a closed loop's
did.** The chain walker accepts a joint within `kChainTol` (1e-4), and
`BRepBuilderAPI_MakeWire` then merges at OCC's confusion (1e-7) and refuses
the edge — so a path whose lines meet its arcs at points no float32 holds
exactly was rejected as "not an open sketch chain", which it was. A handlebar
is exactly that path. `MakeLoopWire` had solved this years earlier for closed
profiles, with measured per-joint vertex tolerances;
`BuildOpenPathWire` now does the same (`tests/test_pipe.cpp`, "a line-arc-line
path whose joints are irrational").

## Not drawn

Brake and gear cables and their routing. The internal gear hub's innards — the
shell is a shell, and the chain relation models its direct-drive gear, the one
where the cog and the wheel turn together. Spoke nipples. The chain's
individual side plates (it is a band on the two pitch circles, with the right
section on the right path). Decals, and the tricolour band on the seat tube —
they are a material, and the lettering is not measurable from these
photographs.

## Correcting it

Every dimension is a project variable. Measure the real bicycle and set them:
`wheelbase` follows from `chainstay_len`, `head_angle_deg` and `fork_rake`;
`seat_tube_len` and `top_tube_len` are the frame's size; `chainring_t` and
`cog_t` are integers in `bike_spec.py` rather than variables, because a count
is not a dimension. The layout refuses at import anything that cannot be a
bicycle — negative trail, a bottom bracket below the axles, a crank that
strikes the ground, a tyre through the seat tube, a gear that is not one.
