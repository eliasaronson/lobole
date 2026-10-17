# A printed Strandbeest

Theo Jansen's walker, printed. Twelve legs are driven by one hand crank:
three crank phases 120° apart, each with a front and a back leg on one crank
pin, and that set again on the other side. Every joint is a printed pin with
a printed cap pressed on. It is built in two stages: one leg pair to test the
pins and the foot, then the walker.

    tools/headless-preview.sh --start <port>
    python3 generators/print_coupon.py --port <port> --seed   # the P1S's bed
    python3 generators/strandbeest/build.py --port <port> --profile "K5 PLA on P1S"
    python3 generators/strandbeest/build.py --port <port> --profile "K5 PLA on P1S" --size 12 --stage 1

The walker packs its plates into the printer's bed, the profile's
`bed_mm`, so it needs a profile that states one: `print_coupon.py --seed`
gives the P1S profiles theirs (256 x 256 x 250, and the 18 x 28 mm
keep-out zone in its front-left corner, which the packer keeps clear of),
and Settings, Print profiles sets anyone's. Without one it refuses, saying
so.

Output goes to `scratch/strandbeest/` (or `--out`):

- `stage1/stage1_plate.stl`: one leg pair on its crank and the press row,
  one plate;
- `stage2/stage2_plate_N.stl`: the walker, as plates for the profile's
  bed;
- the `.dsgn` of each stage, holding the parts, the plates, the assemblies
  and their studies;
- `stage1/strandbeest_stage1_job.3mf` and `stage2/strandbeest_job.3mf`,
  each stage's [print job](../../docs/tolerances/README.md#print-jobs):
  every piece its own object where the packer put it, the standing pins at
  three walls (`PIN_PRINT`: a standing shaft is all wall, and the preset's
  two lines are thin round a pin a bar turns on), the rest at the preset,
  no supports and no brim. The walker's is a Bambu project of its plates
  when `$LOBOLE_SLICER_CONFIGS` holds `pla.json` (`tools/bambu-config.py`),
  and is not written without one, the receipt saying why;
- `strandbeest.json`, the receipt: every size, the layers, the
  allowances it was built with, every check, the controls, the
  predictions and the jobs.

`linkage.py` is the leg as plain numbers: the lengths, the joints at a crank
angle by circle intersections, the foot curve and the layer search. The
generator models the parts from it and then holds the app's motion study to
it.

## Why it exists

It asks of the app what no earlier dogfood did:

- **coupled loops**: a leg is three loops in one component, and twelve
  of them share the frame and the crankshaft. That is 109 revolute columns
  in one solve ([coupled loops](../../docs/assemblies/solver.md#coupled-loops)).
- **a path as the output**: what the linkage is for is the foot's path. The
  study traces it with `run_motion_study`'s `trace`
  ([tracing a point](../../docs/assemblies/README.md#tracing-a-point)).
- **pins that are assembled**: a walker's pins must turn freely and stay
  in. Each pin is closed by a cap pressed on at the profile's press
  allowance ([press allowance](../../docs/tolerances/README.md#press-allowance)),
  a separate occurrence whose press the exact clash check relieves
  ([printed press fits](../../docs/tolerances/README.md#printed-press-fits)).

## The walker

- **The leg** uses Jansen's eleven lengths scaled by `--size / 15`
  (`--size` is the crank radius). Each leg has six bodies: the bars j, k,
  c and f, and the triangles BCD and EFG. The triangles are frames of
  bars, open in the middle. The leg also has a crank pin (A) and a frame
  pivot (B). The back leg is the front one mirrored about the crank's
  vertical, so its foot also pushes the walker back on the ground.
- **Layers.** Each throw's bodies stand between two frame plates in layers
  across the crank axis. `linkage.assign_layers` searches for the fewest
  layers, then the shortest pins, under these rules:
  - no two bodies in one layer come within a quarter of a bar's width of
    each other over the turn;
  - a pin's head sits in the layer under its lowest bar, and its cap in the
    layer over its highest. No other bar may pass the joint in any layer
    from the head to the cap;
  - nothing but the crank bars may pass the crank pin, which spans the whole
    slab.

  The k bar passes 6 mm from the frame pivot B, so k must stand above B's
  cap. Front and back legs share layers wherever they never meet: they meet
  only at the crank pin. The search gives five bar layers, a head layer and
  a cap layer, the same at both sizes:

  | layer | front | back |
  |---|---|---|
  | 1 | BCD | j |
  | 2 | j, c, f | BCD |
  | 3 | EFG | c, f |
  | 4 | k | EFG |
  | 5 | - | k |

- **Joints.** A pin has its head printed on, with the head on the bed, and
  passes through plain holes at the profile's gap for its size. A shoulder
  over the top bar leaves a gap of play. A cap is pressed onto the stub
  above the shoulder, at the press allowance. The frame pivots B are pins
  printed on the plates and capped the same way. The crank pin is part of
  the crankshaft.
- **The crankshaft** is a disc per throw. Each disc turns in its plate, its
  crank pin stands on it, and the pin's stub is pressed into the next
  disc's socket. A middle disc's socket is one phase behind its own pin, so
  every middle disc is the same part, and the keys set the phases rather
  than a hand. The first disc stands on the hand crank's flange and arm,
  and the handle is pressed into the arm. The last disc has only a socket
  and a flange, and prints flange down. The two flanges hold the crankshaft
  in the frame.
- **The frame** is a plate per disc. The plates are tied by two standoffs
  each, pressed into the next plate's sockets. The standoffs stand on the
  crank's vertical, at the nearest spots above and below the disc that no
  bar, head or cap ever comes near over the turn.

The assembly has one occurrence for the frame (its plates), one for the
crankshaft, one for each pin and one for each cap, the frame pivots' caps
too. Each cap is fastened to its pin, or to the frame, and its bore and
the stub under it carry a printed press
([printed press fits](../../docs/tolerances/README.md#printed-press-fits)):
the part's print profile bands them, so the exact clash check relieves
the press and lists it. The discs' keys, the standoffs and the handle are
pressed too, but inside the crankshaft's and the frame's one part, where
no check looks.

## Where every size comes from

The inputs are:

- `--size`, the crank radius;
- the profile's gap (`print_allowance` kind `gap` at each diameter: the
  pin, the disc), and its press (kind `press` at the stub);
- the profile's layer height;
- the profile's bed (`bed_mm`), which the plates are packed into;
- the P1S's 0.4 mm nozzle, which belongs to the machine and is not
  derived.

`solve()` derives the rest. It refuses a wall under four nozzle widths and
a stub under four nozzle widths.

- **Pins:** the pin is 0.33 of the crank radius. Its hole is the pin plus
  the gap. The wall round a hole is half a pin, so a bar is the hole plus
  two walls wide.
- **Stubs, heads and caps:** the stub a cap is pressed on is 0.65 of a pin.
  A head and a cap are a bar's width less a gap.
- **Thicknesses:** a bar is 0.2 of the crank radius thick, a head 0.6 of a
  bar, a cap one bar, and a plate and a disc 1.5 bars. Each is rounded to
  whole layers.
- **Layers and slab:** layer to layer is a bar plus the gap a side, and the
  slab is every layer plus that gap.
- **The crankshaft:** a disc reaches the crank pin plus half a bar, and a
  flange reaches a bar beyond the disc. A socket is 0.6 of a plate deep.
  The handle is 2.5 bars long, on an arm reaching three bars past the
  flange.

At 15 mm with K5 PLA, the profile's numbers give:

| part | size |
|---|---|
| pins | Ø4.95 in Ø5.25 holes |
| bars | 10.2 mm wide, 3 mm thick |
| caps | Ø9.9 on Ø3.22 stubs |
| one slab | 7 layers of 3.15 mm, 22.2 mm in all |
| plates and discs | 4.4 mm thick |
| discs | Ø45.15, in Ø45.35 holes |
| standoffs | 38.7 mm above and below the crank axis |

The profile has no press row, so the caps' bores are at the built-in
default, Ø3.22: the stub's own size.

## Checked every run

First, before any assembly, **the crank stub across its layers**
(`_lib.pin_strength`, docs/simulation/README.md#printed-layers): the first
disc as it prints, flat, so its crank pin and key stub stand across their
layers, held on its underside, with the whole hand crank's torque on the
key stub as torque / crank radius, tangential. The torque is `--hand-n`
(30 N, a firm push) on the knob at the arm's length. The factor against the
interlayer strength goes in the receipt (`crank_stub`), and **under 2 the
run stops**: the default walker does, at a factor of 0.06 (127 N on the
stub, 276 MPa across the layers at the crank pin's root, the pin a 23 mm
cantilever of Ø4.95 from its disc to its key). It would take under 1 N on
the knob to pass. See [Not built](#not-built).

On each assembly, a leg pair and the walker:

- **Solved:** with the crank driven, nothing is left free (DOF 0) and every
  loop closes.
- **The study:** a 360° turn at 2°, 181 samples, with an EXACT clash check.
  Every sample must solve and be clash free, and every cap on its pin, or
  on its frame pivot, must be listed as a printed press fit
  (`press_fits`), relieved and not a clash: 10 on the leg pair, 60 on the
  walker.
- **On the built branch**, twice. The study watches every joint for a
  branch jump between samples
  ([branch jumps](../../docs/assemblies/README.md#branch-jumps)) and must
  report none. And, as a second witness, at every sample each knee of every
  leg (C, E and F, linkage.KNEES) is on the side it was built on. The
  joints are read off the study's trace.
- **The layers' air:** at every 15th sample, slab 0's bars in adjacent
  layers that overlap seen along the axis are measured exactly with the
  assembly left there (168 pairs). None may be closer than the gap a side.
  Slab 0 stands for every throw: the throws are the same leg pair a phase
  apart, and the samples cover the turn.
- **The feet**, traced at the foot triangle's vertex less its rounded
  corner, against the scaled Jansen curve from `linkage.pose`. Each must
  stay within 1 % of the crank radius of the curve at every sample. Its
  stride, lift and flatness must match the curve's:
  - the stride is the run of the turn within a band of the lowest point
    (2 % of the crank radius);
  - the flatness is over a stride's length.
- **Static stability (the walker):** the centre of mass at every sample
  must lie inside the polygon of the feet on the ground. The centre of
  mass is each occurrence's own (`mass_properties` of its part), traced
  and weighted; at the first sample it agrees with the assembly's
  `mass_properties` to 1e-13 mm. A foot is on the ground within the
  stride's band of the lowest. The worst margin is reported.

On every print plate:

- `check_geometry` verified;
- every `check_print` item passes, `bed_fit` among them: the plate in the
  profile's bed, every piece clear of its keep-out zone;
- every piece starts on the bed;
- the plate's box (`mass_properties`' `bbox_mm`) starts at the bed's
  corner, where the packer put it.

### Negative controls, which must fail

Both are run on a leg pair of their own every time, and the run stops if
either passes.

- **The front f bar 10 % long.** It assembles and turns. Its foot runs
  15.5 mm off the curve, and over the stride's 44.8 mm it rises 2.06 mm
  against the curve's 0.27 (at 15 mm).
- **The front k bar in the BCD triangle's layer.** The clash check finds k
  against BCD at 25 samples, from 168°.

## What it predicts, and the test card

The walker stands on its lowest feet, and they carry it. At every sample it
moves by the lowest foot's travel along the ground, so over a turn it
walks:

| `--size` | distance per crank turn | step it clears (the foot's lift) | stride | centre of mass inside the feet, worst |
|---|---|---|---|---|
| 15 (Jansen's) | **141.8 mm** | **22.5 mm** | 44.8 mm | 15.6 mm at 240° |
| 12 | 113.5 mm | 18.0 mm | 35.9 mm | 12.7 mm at 240° |

It walks front first: toward −x, where the front legs reach. At 15 the
walker weighs 500 g in PLA.

**Test card (stage 1, then stage 2):**

1. Print `stage1_plate.stl`. Press each of the press row's five caps (the
   notches count which) onto its pin. Keep the smallest interference that
   holds by hand and does not split:
   `print_coupon.py --record --profile "K5 PLA on P1S" --press-side MM`.
   Then rebuild: the caps are modelled at the recorded press.
2. Assemble the leg pair. Turn the crank by hand: every joint must turn
   freely, and no cap may come off.
3. Hold the frame level over a ruler and turn the crank. The front foot
   should touch down, run flat for **44.8 mm**, and lift **22.5 mm**.
4. Print stage 2. Assemble it and set it on a table. Ten crank turns should
   walk it **1418 mm**, give or take what the feet slip. It should step
   onto a **22 mm** ledge and not onto a 25 mm one.

## The plates

For K5 PLA on a P1S at 15 mm, every plate passes every `check_print` item:

| plate | pieces |
|---|---|
| stage 1 | one leg pair (12 bodies), 8 pins, 10 caps, the first and last plates and discs, the handle, and the press row (a plate of five pins and five caps): 36 pieces, 41 solids |
| stage 2, 1 | first, last and 3 middle plates; first and last discs; 4 front k, 2 front EFG |
| stage 2, 2 | 2 middle plates; 6 back k, 6 back EFG, 2 front k, 4 front EFG |
| stage 2, 3 | 4 middle discs, the handle; 6 front and 6 back BCD, 6 front j, 2 back j, 2 back f; 1 pin |
| stage 2, 4 | 1 middle disc; 6 front and 6 back c, 6 front f, 4 back f, 4 back j; 47 pins, 60 caps |

The assignment comes from a MaxRects packing of each piece's box, turned to
its tightest, half a bar apart. At 12 mm, three plates hold the walker.

## Not built

- **No path in the Motion window.** A trace is a request parameter, and
  nothing in the document holds it for the window to draw.
- **A crankshaft that carries a hand's torque.** The crank stub is now
  checked across its layers, and it fails: the torque between two discs
  goes through one Ø4.95 crank pin standing 23 mm as a cantilever between
  them, printed across its layers, which holds about 1 N on the knob at a
  factor of 2. The run stops there until the crankshaft is redesigned (a
  through shaft, the discs keyed to it; or pins printed lying down) or
  the torque it must carry is stated from the walker's own need. The
  joint pins and the frame pivots print standing too, and are not yet
  studied.
- **Packing goes by boxes.** Triangles nest poorly by their boxes, so four
  plates hold the 15 mm walker where three might.
