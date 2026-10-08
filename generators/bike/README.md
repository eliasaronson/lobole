# Roadster bicycle

The owner's bicycle, modelled from three photographs and no measurements: a
green Scandinavian gents' roadster with a lugged diamond frame, 28" wheels
laced three-cross, a 3-speed internal-gear rear hub, one chainring under a
teardrop alloy chainguard, full mudguards, a tubular rear carrier and
north-road bars. Twenty parts, twenty-two occurrences, and it moves: the
wheels turn, the bars steer, and the cranks drive the rear wheel through a
chain relation at 46/18.

## Running it

```sh
python3 generators/bike/bike.py [--port N] [--out DIR] [--stage parts|assembly|all] [--keep]
```

`--out` defaults to `scratch/bike/`; `--keep` builds into the open project.
`all` checks, saves `bike.dsgn` and `bike.step`, and renders. The run refuses
to save what it has not checked. It is not in the CI suite: `check_geometry`
on two 36-spoke wheels alone is longer than the headless suite should take.

## Where the numbers come from

`bike_spec.py` keeps two kinds of number apart and marks every line:

- **READ** — what the photographs establish: the frame type, 28" wheels with
  36 spokes, a trigger shifter on the right bar (so a wide internal-gear hub
  shell, which is why the rear wheel is not the front one mirrored), a
  five-arm crank, one chainring, alloy guards, the carrier, the bars.
- **ASSUMED** — every length, angle and tooth count: standard roadster
  numbers (a 58 frame, 71° head angle, 465 mm chainstays, 46/18 gearing, …).

Every dimension is declared as a project variable (`declare(app)`), so
correcting one is an edit, not a rebuild; tooth counts stay integers in the
spec. The layout is derived from them — wheelbase from the chainstay, head
angle and rake; trail; development per crank turn — and REFUSED at import if
it cannot be a bicycle: trail under 10 mm, the bottom bracket below the axles,
a crank that strikes the ground, a rear tyre through the seat tube, a
chainring bigger than the bottom bracket is high, or a gear outside 2–10 m
per crank revolution. The rims are called aluminium
(`bike_wheels.RIM_MATERIAL`), a judgement from their finish in the photos.

The origin is the bottom bracket, Z up, X forward, Y left; the ground is at
z = BB_DROP − wheel radius.

## Structure

| file | what |
|---|---|
| `bike_spec.py` | every dimension, READ or ASSUMED, the derived layout and its refusals |
| `bike_shop.py` | the moves a bicycle is made of: tubes between two points, bowed and arc tubes, partial revolves, sprocket and tangent-hull outlines, `Weldment`, `verified` |
| `bike_frame.py` | frame and fork |
| `bike_wheels.py` | rim, tyre, hub and spokes, as three bodies of three materials per wheel |
| `bike_drive.py` | crankset, pedal, chain, chainguard |
| `bike_cockpit.py` | stem, handlebar, grip, brake lever, bell, seatpost, saddle |
| `bike_fittings.py` | mudguards, carrier, reflector, front brake |
| `bike_verify.py` | what the run refuses to save without, and the portraits |
| `bike.py` | the run |

The frame is the one fixed occurrence. Everything that steers hangs off the
FORK, so the one revolute at the head tube takes the whole front end. Parts
that do not move relative to frame or fork are drawn in the bicycle's own
coordinates and fastened by a single `seat` frame. The joints are six
revolutes (steering, two wheels, cranks, two pedals), all `flipped=False`:
two frames mated with the RPC's default `flipped: true` assemble into a
different bicycle. The drive is a CHAIN relation, which turns both sprockets
the same way.

**Frames are welded SOLID and drilled afterwards** (`bike_shop.Weldment`).
Hollow tubes unioned at an angle put an end cap inside another tube's void,
and whether that builds depends on tangencies; solid rods union at any angle,
and the bores are then cylinders subtracted from one solid. The tubes come out
hollow between joints and solid through them, which is what a brazed lug is.
A BOWED rod (a stay, a fork blade) is swept along a `Curve3D` fit-spline, not
a sketch arc: a few millimetres of bow over half a metre is a torus metres
round and millimetres thick, and the seat stay's union into the seat tube
came out wrong (a patch of seat-tube wall kept inside the stay) at every
offset tried. Straight rods stay on sketches.

**A part is finished when `check_geometry` says `verified`** (`bike_shop.verified`
gates every part on the receipt). An invalid body — one axle bore drilled
through both dropouts in one pass is enough — poisons every later question,
including phantom clashes in the assembly sweep. The bore is drilled one
dropout at a time.

A handlebar is one pipe with two bends: a line–arc–line sketch in the plane
the bar is bent in, with the bend angle solved from width, sweep and rise.
Anything that clamps on the bar is built on the bar's axis.

## What a run checks

| check | |
|---|---|
| `asm.check` | no clash, nothing floating, no near miss under 1 mm, with intended contacts in its `allow` map |
| geometry, on the assembled bicycle | wheelbase, both axles at one height, straight chainline, positive trail, pedal clear of the ground |
| mass | every part on its material, total between 11 and 24 kg |
| a motion study, run | one crank revolution, the rear wheel turning 46/18 of it, nothing new touching |
| steering | clear through ±38° |

The crank is 64 mm off the centre line because of a chain of clearances:
chain, then chainguard just outside it, then the crank arm sweeping outside
the guard, then the pedal. The chainguard's tail is a strip over the chain's
TOP run (a guard shaped like the whole loop would pass through the
chainstay). The brake caliper's arms go outboard of the mudguard, which has a
hole for its bolt.

A study sampled every 15° steps a pedal 44 mm between samples, wider than
the chainguard's tail: for thin parts, sweep finer or check exact volumes at
intermediate angles.

## Not drawn

Cables, the hub's internal gears (the chain relation models its direct
gear), spoke nipples, individual chain plates (the chain is a band on the two
pitch circles), decals and the tricolour band (a material).
