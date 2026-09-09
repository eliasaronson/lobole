# Building this quadcopter

A 220 mm 5-inch freestyle quad, 6S, analog video. 435 g dry, 654 g with a
6S 1300 pack, about 9:1 thrust to weight.

Everything here comes out of the model in this directory
(`generators/fpv_drone.py` built it; `docs/modeling/fpv-drone.md` says how
and what was checked). Dimensions on the sheets are the authority — the
lists below are what to order and the order to put it together in.

---

## 1. Have these cut

Send the SVG or the STEP. Any carbon-plate cutting service takes either;
so does a hobby CNC router with a 1 mm single-flute cutter. **Cut carbon
wet or with extraction, and wear a mask.**

| sheet | part | material | qty |
|---|---|---|---|
| `bottom-plate.svg` | Bottom plate | 3 mm 3K carbon | 1 |
| `mid-plate.svg` | Mid plate | 2 mm 3K carbon | 1 |
| `top-plate.svg` | Top plate | 2 mm 3K carbon | 1 |
| `arm.svg` | Arm | **5 mm** 3K carbon | 4 |
| `camera-plate.svg` | Camera plate | 2 mm 3K carbon | 2 |

Each sheet carries its own hole table (centres from the part origin), so a
shop that would rather type coordinates than trace a profile can.

**Or cut them yourself.** `<sheet> - T<n>.nc` is G-code for a 600 x 400 grbl
router, one file per cutter, nested on a 320 x 320 sheet: 3 mm (bottom
plate), 2 mm (mid, top and both camera plates) and 5 mm (all four arms). Set
X0 Y0 at the near corner of the sheet and Z0 on its top face, and run each
sheet's files in this order — **T2, T3, T4, T1**, which is the order the
cutters are wanted in and not their numbers: the drills first, then the 2 mm
for the slots, and the 3.175 mm last because its final pass is what cuts the
parts free. About 5, 13 and 11 minutes of cutting.

The parts come out on four 4 x 0.6 mm tabs each; part them with a knife and
sand the stub. The shaft bore in each arm drops a slug — catch it, or it
goes under the cutter. **Cut carbon wet or with extraction either way.**

The two camera plates are one part, not a mirrored pair: a flat plate
mirrored through a plane parallel to itself is the same plate.

## 2. Print these

0.2 mm layers, 4 perimeters. TPU 95A for both — they are there to give.

| file | part | qty |
|---|---|---|
| `antenna-mount.stl` | Antenna mount | 1 |
| `battery-pad.stl` | Battery pad | 1 |

## 3. Buy these

**Powertrain**

- 4 x 2207 motor, 1750 KV, M5 shaft, 16 x 16 M3 mounting (e.g. EMAX ECO II,
  iFlight XING2). They must have **5 mm of usable thread** in the base —
  the M3x10 screws below are sized for it.
- 2 x propeller 5.1x4.3x3 CW, 2 x CCW (5 mm bore)
- 4 x M5 prop nut (usually ships with the motors)
- 1 x 4-in-1 ESC, 55 A, 6S, 30.5 x 30.5 mounting, 36 x 36 board
- 1 x flight controller, F7, 30.5 x 30.5, 36 x 36 board
- 1 x 1000 uF 35 V low-ESR capacitor (ø10 x 20)
- 1 x 6S 1300 mAh pack, 72 x 36 x 42 max, XT60

**Video and radio**

- 1 x 19 mm micro FPV camera with M2 side mounting (e.g. Caddx Ratel 2,
  Foxeer Razer Micro)
- 1 x 5.8 GHz VTX on the 20 x 20 pattern, 25 x 25 board
- 1 x RHCP antenna, SMA, straight
- 1 x ELRS receiver, ~22 x 12 mm
- 1 x 5 V buzzer

**Hardware** — all stainless or steel socket cap unless noted

| item | qty | where |
|---|---|---|
| M3 x 16 | 4 | outer arm bolt, into the standoffs |
| M3 x 14 | 4 | inner arm bolt, with a nyloc |
| M3 x 10 | 16 | motors |
| M3 x 8 | 6 | top plate into the standoffs (4), antenna mount (2) |
| M3 x 25 | 4 | the stack |
| M3 nyloc nut | 10 | inner arm bolts, stack, antenna mount |
| M2 x 12 | 4 | video transmitter |
| M2 x 8 | 2 | camera cage lock |
| M2 x 6 | 2 | camera tilt |
| M2 nyloc nut | 4 | video transmitter |
| M3 x 15 aluminium standoff, female-female | 4 | between mid and top plate |
| M3 x 4 nylon spacer | 4 | bottom plate to ESC |
| M3 x 8 nylon spacer | 4 | ESC to flight controller |
| M2 x 5 nylon spacer | 4 | mid plate to VTX |
| M2 x 19.5 tapped aluminium spacer | 1 | camera cage |
| 20 mm battery strap | 1 | through the top plate |

The M2 x 19.5 spacer is the one odd item: an ordinary M2 female-female
standoff cut to length, or two shorter ones. It sets the camera cage's
width, so cut it to **19.5 mm**, not "about 20".

---

## 4. Put it together

Everything assembles top-down. Nothing needs to be sprung, twisted or
persuaded.

1. **Arms.** Lay the bottom plate flat, foot side down. Put the four arms
   on it, pointing out along the diagonals, and the mid plate on top of
   them. From underneath: an M3x16 up through each outer hole into a
   standoff, and an M3x14 up through each inner hole with a nyloc on top.
   Do not tighten yet.
2. **Square it.** With all eight bolts loose the arms can still rotate.
   Set the wheelbase equal on both diagonals (220 mm motor centre to motor
   centre) and then tighten, opposite corners first. Carbon does not need
   much: 0.6-0.8 Nm, or firm with a short key.
3. **Motors.** Four M3x10 up through each arm's pad into the motor. Check
   each screw enters freely for the first two turns — a cross-threaded
   motor mount is the usual way to lose one. Leave a little slack, spin the
   bell, then tighten.
4. **Stack.** M3x25 up from under the bottom plate, an M3x4 nylon spacer,
   the ESC (component side up), an M3x8 spacer, the flight controller, a
   nyloc on top. The stack stands up through the mid plate's opening; it
   should not touch its edges.
5. **Solder** before anything else goes on: motor phases to the ESC (any
   order — direction is set in software), the capacitor across the ESC's
   battery pads with the shortest legs you can manage, the XT60 lead out
   the back.
6. **Camera cage.** Drop each camera plate through its slot in the mid
   plate until its shoulder rests on the plate and its foot lands on the
   bottom plate. Camera between them on the two M2x6 screws — those thread
   into the camera's own bosses, so they are short on purpose. Then the
   M2x8 lock screws through both plates into the 19.5 mm spacer, which is
   what stops the plates lifting back out.
7. **Top plate** over the standoffs, four M3x8. Its front notch clears the
   camera; if the camera fouls it, the cage is not fully down.
8. **Video transmitter** on the mid plate's tail: four M2x5 nylon spacers,
   four M2x12 through the board and the plate into nylocs underneath.
   Antenna mount beside it, two M3x8 down through it into the plate with
   nylocs under. Receiver and buzzer on the mid plate beside the stack.
9. **Battery pad** on the top plate, strap through the four slots.

## 5. Before you fly it

- **Prop directions.** Front-left and rear-right take CW props; front-right
  and rear-left take CCW. The frame is a true X, so "front" is whichever
  way the camera looks.
- Check the props turn the right way with props OFF first, then fit them.
- Prop nuts are self-tightening in the direction of rotation; still check
  them between packs for the first few.
- Balance the camera tilt to the speed you fly at — 20 degrees as drawn.

## 6. Clearances, for when you change something

Measured on the model, not calculated:

| between | mm |
|---|---|
| adjacent propeller discs | 30.7 |
| propeller to top plate | 37.1 |
| propeller to battery | 33.8 |
| propeller to antenna | 14.3 |
| battery to camera | 7.4 |

The propellers were also turned through a full blade pitch in the model
with every occurrence pair checked; nothing new touches. If you fit
different props, a longer pack, or move the antenna, that check is the one
to run again — it is what the standoff length and the battery position are
set by.
