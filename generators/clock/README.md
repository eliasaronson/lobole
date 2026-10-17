# A printed pendulum clock

A weight-driven wall clock in which everything is printed but the cord: a
Graham deadbeat escapement, a two-stage cycloidal going train to the centre
wheel (one turn an hour), 12:1 motion works, a cord drum with a printed
click, two plates on four threaded pillars closed by printed nuts, a dial
ring, hands, and a pendulum with a rating nut on a printed thread. The
weight is the [jar](../jar/README.md) full of sand.

It is the first print dogfood that has to run on its own. The planetary and
the counter were turned by a hand that could push through a tight mesh; a
clock train sees grams at the escape wheel, and its rate is a number a phone
stopwatch checks. So the design states its period, its rate and its run per
winding before anything is printed, and the print tests the design, as the
[hook](../hook/README.md)'s breaking load did.

    tools/headless-preview.sh --start <port>
    python3 generators/clock/build.py --port <port> --profile "K5 PLA on P1S"
    python3 generators/clock/build.py --port <port> --period 0.8

Output, untracked, in `scratch/clock/` (or `$LOBOLE_SCRATCH/clock`, or
`--out`): `clock.dsgn`, the receipt `clock.json`, the test card
`clock_card.md`, the stills, and `stage1/` and `stage2/`, one STL per
print plate and each stage's [print job](../../docs/tolerances/README.md#print-jobs)
(`clock_stage1_job.3mf`, `clock_stage2_job.3mf`: every piece its own
object with its slicer settings, a Bambu project when
`$LOBOLE_SLICER_CONFIGS` holds `pla.json`, else not written and the
receipt says why).

| file | what |
|---|---|
| `clock_spec.py` | every size from the period and the profile; the escapement's geometry, its stand-in motion and a 2-D sweep of its own polygons; the compound pendulum's sums |
| `clock_parts.py` | one function per part, each in the movement frame |
| `build.py` | the parts, the pendulum's length, both stages' assemblies, every check and control, the predictions, the FEM, the print plates, the card |
| [`../cycloidal.py`](../cycloidal.py) | the cycloidal teeth, shared with the [watch](../watch/README.md) |

## Where every size comes from

Two inputs size the clock: `--period` (the pendulum's, there and back;
default 1 s, so a beat every half second) and the print profile, through
`print_allowance`. One more is a measured input, not a size of the clock:
`--cord-mm`, the cord's diameter, which sets the drum's capacity.

- **The play.** Every clearance that a chain of printed faces and arbors in
  their holes must absorb is one number: twice the gap a side at a wheel's
  size plus twice the gap at an arbor's (0.5 mm on K5 PLA on P1S: 0.1 and
  0.15 a side). It is the escapement's drop and its lock, the backlash in
  the teeth, and the end shake.
- **The end shake** is the axial play between every two parts that turn
  against each other and overlap seen from the front: the centre wheel an
  end shake above the escape pinion, the hour wheel over the cannon pinion.
  It was first added to get the mesh-mode motion study past a face seated
  on a face, which that check called a clash; it now calls that touching
  ([inspect](../../docs/inspect/README.md#touching-is-not-a-clash)). It
  stays, because it is not only a workaround: two printed faces turning at
  different rates rub, and a print's layers need the play. The geometry is
  unchanged.
- **The escape wheel** has 30 teeth (a tooth a period, a turn in 30
  periods). Half its tooth pitch must hold a tooth's tip (two lines), the
  rest gap (the gap a side between a locked tooth and its locking face), the
  drop, and a pallet four lines wide: that sizes the wheel's radius, to the
  millimetre. The anchor spans 7½ teeth with its pivot on the tangents
  through the pallets, so a tooth runs straight at a locking face that is
  an arc about the pivot: dead-beat. The lift is the pallet's width over the
  radius (a 45° impulse face), the lock the drop over the radius, and the
  swing the impulse and lock plus the fewest quarter-locks of
  supplementary arc whose escapement sweeps clean (below): 1.5 at K5,
  ±3.59°. Fewer than 1.5
  leave the exit pallet too shallow for the tooth to land on its locking
  face within the beat.
- **The teeth** are as deep as it takes for the tooth ahead's back to clear
  the pallet's deep corner by the gap at the pallet's deepest, and 15 % more.
- **The train**: the smallest two wheels, with pinions of 8 leaves or
  more, whose product is exactly the hour over the escape wheel's turn
  (`3600 / 30T`): 80/8 and 96/8 at 1 s. A period whose ratio is not one of
  whole numbers is refused, naming periods that work.
- **The module** is the smallest, in tenths, at which every tooth's tip is
  two lines across (`cycloidal.tip_land`, the counter's and planetary's
  rule for printed teeth), every pair's contact ratio is at least 1.1, and
  every pinion's root leaves two lines round its pressed bore.
- **Arbors** are twelve lines across, printed standing, D-flatted where a
  wheel is pressed on (half the hole allowance over the arbor: a light
  press, no angular play) and round in the plates' holes (the gap at the
  arbor's size). The cannon pinion is pressed on a ROUND end: that is the
  setting clutch.
- **The plate spacing** is set by the weight: the jar must pass the bob,
  which hangs behind the back plate. The cord's rearmost turn stands the
  jar's radius, the bob's half-depth and that half-depth again in front of
  the bob, and the drum reaches forward to it on a sleeve.
- **The pendulum's length** is found, not computed: the rod, bob, rating nut
  and the anchor with its arbor are built, `mass_properties` gives each
  body's mass, centre and inertia, and the rod is lengthened or shortened
  until `T = 2π √(I / (M g d))`, with the circular error of the design swing,
  is the period. The bob is a quarter of the equivalent length wide and a
  fifth tall; the rating nut travels 2 % of the length either way.
- **Threads** are 45° flanks (the jar's), M8's major (the smallest printed
  thread that is reliable), pitches in whole layers: 10 for the pillars, 8
  for the rating nut. The rod is printed flat, so its threaded end is
  D-flatted on the bed below the axis, which cuts away every face that would
  look down more than 45°.

## The escapement is animated, not simulated

Nothing in the app simulates an escapement: there is no contact, no
impulse, no pendulum dynamics. As the watch does, the motion is a stand-in.
A hidden "beat" revolute turns once a period. Through an `intermittent`
relation it steps the escape wheel half a tooth a beat, inside a window that
opens where the locking corner passes the resting tooth; through a `cam`
whose table is `A cos(beat)` it swings the anchor, and the pendulum
fastened to it. The step's law is a **table**
([`motion: table`](../../docs/assemblies/relations.md#intermittent)):
`clock_spec.window()` walks the beat in 60 samples and at each puts the
wheel as far on as it can go without coming nearer either pallet than the
rest gap does across a 45° face (rest gap/√2), on both beats at once, so
the tooth follows the impulse face, crosses its deep corner on the tip
land and drops onto the other locking face; the window closes where the
wheel has gone its half tooth (33 points at the default). In the model a
tooth lags its face by that rest gap; in the print the tooth pushes on
it.

What the stand-in can prove is geometry: that the pallets clear every tooth
through the whole swing, that the wheel stands locked, the rest gap off a
locking face, at the end of every beat, and that the train turns at the
ratios its teeth say. It says nothing about whether the clock runs. Whether
the weight overcomes the friction, how far the pendulum swings and how the
rate drifts are the printer's to answer, which is what the two stages are
for.

## What is checked, every run

- the escapement's own polygons swept through two beats before anything
  is built (`clock_spec.escapement_sweep`), and the same sweep with the
  pallets set two locks too deep, which must overlap;
- `check_geometry` verified on every part;
- stage 1: a motion study over one period at 2° of beat, and one over a
  whole escape-wheel turn at 4 samples a beat, both clean, the wheel on its
  step at every beat's end, and the resting tooth measured the rest gap off
  each locking face;
- **negative control**: stage 1 with an anchor whose pallets are two locks
  too deep must clash, anchor against escape wheel;
- stage 2: the same turn with the whole movement, clean, locked at every
  beat's end, and every arbor's turn first sample to last against its
  tooth counts (`watch.py`'s `check_ratios`); every mesh through a pitch of
  its smaller gear, exactly (`watch.py`'s `check_meshes`); the click: wound
  backwards the drum turns and the hands do not, pushed the going way it
  holds;
- **negative control**: a third wheel of one tooth more must fail the ratio
  check;
- every pillar nut and the rating nut turned until its thread clears its
  pillar's (the jar's phase search);
- static studies of the pendulum rod under its bob and nut, and of the
  centre arbor in its pivots under the jar full of sand, each once, with a
  timeout. The arbor is a printed pin standing on its back end, so its
  study is `_lib.pin_strength` (docs/simulation/README.md#printed-layers):
  the cord's pull on the band of its round the drum turns on and the
  pivots held on the bands in the plates, each a region of the one round
  face (docs/simulation/README.md#loads-on-part-of-a-face), read across its
  layers. Its interlayer factor goes in the receipt, and under 2 the run
  stops;
- every print plate's `check_print`, every item.

`generator_clock` (generators/README.md#the-big-generators-as-tests) runs
the whole of it but the stills, on the seeded P1S profile: the period is
the PRINTED pendulum's, which needs a profile's layer and line width.

## The default clock, and its test card

`--period 1 --profile "K5 PLA on P1S"`: a 30-tooth escape wheel 58 mm
across with teeth 6.3 deep, pallets 1.64 mm wide, lift 3.23°, lock 0.99°,
a swing of ±3.59°; a train of 8/80 and 8/96 at module 1.3 (escape to
centre 120:1) and motion works 10/30 and 8/32; plates 61.2 mm apart;
a rod of 296.4 mm from the anchor arbor to its lower end and a bob
63 × 49.8 × 20.4 mm. `clock_card.md`, as that run wrote it:

| what | predicted |
|---|---|
| period at the design swing (±3.59°), printed as set | 1.00000 s (0.99975 s at a small swing) |
| the same parts read solid | 1.00000 s |
| rate, as built | 0.0 s/day |
| a turn of the rating nut that lowers the bob | −246 s/day (−10.2 a notch of 24) |
| a degree more swing either way | −13.5 s/day (circular error) |
| run per metre of drop (the first layer of cord) | 28.2 h |
| torque at the escape wheel per 100 g, stage 2 | 0.046 N·mm |
| torque at the escape wheel per 100 g, stage 1 | 5.54 N·mm |

Equivalent length 248.3 mm, 98 g swinging (rod, bob, nut, anchor and
arbor, weighed as they PRINT: `mass_properties`' printed estimate). The
pendulum prints solid, 100 % rectilinear (`PENDULUM_PRINT`; grid cannot
print 100 %), so its rod, bob and nut are their solid selves exactly; the
anchor and its arbor print at the frame's 15 %, which is why the solid
reading differs in the sixth figure. A swinging part with no printed mass
that is not printed solid is refused rather than weighed solid. The jar full of sand is about 320 g: 0.15 N·mm at
the escape wheel in stage 2. Whether that runs the train is what stage 1
answers: the least weight that keeps stage 1 ticking, times 120 and over
the train's friction, is what stage 2 needs. A bigger drum would give more
torque and less run; the drum is the smallest that holds the cord.

FEM, each study once: the rod under the bob and nut peaks at 0.08 MPa
(0.04 mm of stretch); the centre arbor under the jar full of sand (3.1 N
on the drum's band) at 2.2 MPa von Mises, a factor of safety of 22,
0.04 mm of bending, and 2.24 MPa across its layers: **a factor of 7.6**
against PLA's interlayer strength.

### Print plates

Every piece prints the way it is modelled, its back face (the face towards
the wall) on the bed, except the rod (flat, along the bed's diagonal from
the front right to the back left, clear of the P1S's keep-out corner, its
D-flatted thread on the bed) and the bob and rating nut (stood on end).
Every part but the pendulum states Bambu Studio's 0.20 mm Standard (2
walls, 5/3 shells, 15 % grid; `clock_parts.FRAME_PRINT`). No supports, no
brim; every plate passes every `check_print` item, its keep-out zone
included. Printed, the clock weighs 329 g against 560 g solid (59 %);
Bambu's own slice of the two jobs, 340 g.

| plate | pieces |
|---|---|
| `stage1/plate1.stl` | the stage-1 back and front plates, escape wheel, anchor, stage-1 drum, 4 pillars and 8 nuts (stage 2 uses them too), the anchor arbor and the stage-1 escape arbor |
| `stage1/plate2.stl` | the pendulum rod, bob and rating nut |
| `stage2/plate1.stl` | the back plate, third, hour and minute wheels, drum, cannon pinion, the two dial spacers, the third and centre spacers, the escape, third and centre arbors |
| `stage2/plate2.stl` | the front plate and both hands |
| `stage2/plate3.stl` | the centre wheel |
| `stage2/plate4.stl` | the dial ring |

### Test

1. **Stage 1** (`stage1/`): hang it level by the keyhole, wind a cord
   round the drum on the escape arbor, set it in beat (tilt until the ticks
   are even) and find the least weight that keeps it ticking five minutes.
2. **Stage 2** (`stage2/`, with stage 1's pillars, nuts, escape wheel,
   anchor, arbor and pendulum): the weight on a cord round the drum. Wind it
   by turning the drum backwards; the click lets it go and the hands stay.
   Set the hands by the minute hand: the cannon pinion slips on its arbor.
3. Run it an hour against a phone, then a day, and read the rate; a turn of
   the rating nut moves it by the card's figure.

| stage | weight (g) | ran for | rate (s/day) | swing (°) | notes |
|---|---|---|---|---|---|
| 1 | | | | | |
| 2 | | | | | |

## Not modelled

- **Dynamics.** No contact, no friction, no impulse, no pendulum: the
  motion is the stand-in above. The weight needed and the swing it keeps
  are the bench's.
- **The flexure pawls do not flex.** The click relation says the drum winds
  back and holds the going way; riding the pawls over the ratchet is not
  clash-checked, since the model cannot bend them.
- **Fits that hold by friction** (every press on an arbor's D, the cannon
  pinion's clutch, the hour hand on its pipe) are modelled half the hole
  allowance over the shaft, so the model shows a hair of air where the
  print is tight.
- **The movement is modelled in its own frame**, Y up; the app's world is
  Z-up, so in the app the clock lies on its back. The stills set the camera.
- Two wheels overhang the plates' outline (the third wheel to the left,
  the centre wheel at the bottom): the plates are the hull of the pillars
  and the arbors, not of the wheels.
