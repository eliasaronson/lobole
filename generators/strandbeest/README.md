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
- `strandbeest_card.md`, the test card: the look-alikes, every piece's
  label and each stage's assembly order, made from the layer table and the
  joints ([below](#labels-look-alikes-and-the-assembly-order));
- `strandbeest.json`, the receipt: every size, the layers, the
  allowances it was built with, every check, the controls, the
  predictions and the jobs.
- with `--video`, `stage1_sequence.mp4`: the leg pair's
  [assembly sequence](../../docs/assemblies/README.md#playing-and-filming-a-sequence)
  filmed (1280 x 720, 30 fps), every piece appearing at its card step and
  lit while it goes home along the path the check took, the crank turned
  to 15° by the pose step before the caps that go on there. Stage 2 has no
  sequence. Off by default; it needs an instance with a renderer and
  ffmpeg, and is refused on a `--headless` one before anything is built.

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
- **The crankshaft** is a disc per throw and a crank pin per throw. Each
  disc turns in its plate. A crank pin is a part of its own, **printed
  lying down** on a D-flat (`r/√2` under its axis, so nothing under it
  looks down past 45°): the torque bends it between two discs ALONG its
  layers, never across them. It spans its slab and is pressed into a
  D-shaped socket in the disc on each side, its flat toward the crank
  axis, so the D keys it. A disc's top socket takes its own throw's pin
  and a middle disc's underside socket, one phase behind, the last
  throw's: every middle disc is the same part, and the sockets set the
  phases rather than a hand. The first disc stands on the hand crank's
  flange and arm, and the handle is pressed into the arm, its stub a crank
  pin's size. The last disc has only a socket and a flange, and prints
  flange down. The two flanges hold the crankshaft in the frame.
  A through shaft along the crank axis is not possible: the crank bars j
  and k pass the axis itself over the turn.
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
  flange reaches a bar beyond the disc. A socket is 0.6 of a plate deep;
  a crank pin spans the slab and reaches into both sockets, a gap's play
  short of their floors, and its sockets and the handle's bore are a pin
  at the press allowance at its size. The handle is 2.5 bars long, on an
  arm reaching three bars past the flange, its stub a pin across.

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

First, before any assembly, **every pin in a load path, under the
walker's own loads** (`strength()`, `_lib.pin_strength`,
docs/simulation/README.md#printed-layers):

- **The loads** come from the leg's statics (`linkage.statics`: the bars
  j, k, c, f as two-force members, the triangles and the crank in
  equilibrium; checked against virtual work), over a turn, with the
  walker's weight shared by the feet on the ground (within 2 % of the
  crank radius of the lowest) and at every foot the most friction the
  floor gives, mu times its share, the worse way (`linkage.walking_loads`).
  The weight is every printed piece's `mass_properties` (solid where a
  part states no print settings: more, so the loads err high). mu is 0.5
  (`--mu`, `FOOT_MU`): a stated upper value for dry printed PLA on a table
  -- the published measurements of printed PLA are against steel
  (Stoimenov et al., Tribology in Industry 46(1), 2024) -- to confirm by
  tilting the walker on its floor until it slides.
- **The design torque** is the larger of the walk's own (the weight alone,
  doubled for the joints' friction and a rough floor, `WALK_MARGIN`) and
  the slip's: the most torque the feet let a hand put through the legs
  before they slide. At 15 mm the walker is 500 g; walking takes 5.1 N mm
  and slipping 60.4, so **the slip governs**: 60.4 N mm, 1 N on the knob.
  `--hand-n` states a push on the knob instead.
- **The studies**, each as the part prints, with its layers' direction,
  each needing a factor of 2 against the interlayer strength AND against
  the yield, or the run stops: a crank pin carrying the design torque
  between its discs (held in its sockets, which turn together, so a beam
  fixed at both ends and offset at one: held in one socket, the force at
  mid-span), and its crank bars' pull; the handle under the push; every
  joint pin of a leg pair (the band of its top body loaded, the others'
  bands holding it); both frame pivots on their plate. At 15 mm the least
  factor is **4.15**, a frame pivot across its layers; the crank pin's is
  9.4 across and 12.2 against the yield. The receipt keeps every study
  (`strength`).

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

## Labels, look-alikes and the assembly order

- **Every piece carries its name**, engraved two layers deep on its top
  face, a face that is neither on the bed nor one a joint turns on: a bar
  or a triangle its name (`F c`, `B BCD`: F front, B back) along its
  longest run and each hole's joint letter beside it along the bar; a
  joint pin `FC`, `BE` on its head's top, round the shaft; a plate and a
  disc their kind (`plate first`, `disc middle`); the crank pin `A` on its
  crown in the slab's one layer no crank bar turns in; the handle `H`.
  The text is as large as its face holds: 0.4 of a bar's width on a bar.
  A pocket in an upward face has no face that looks down, so the plates'
  `check_print`, the clash study and the layers' air are as before. The
  throws' pieces are the same pieces in every throw, so a label names no
  throw. The caps are all one piece and carry none. The pins and the crank
  pin are studied before their labels go on (Netgen meshes a letter's
  faces badly); the pockets leave every factor above 9.
- **Look-alikes.** Every printed piece is described by its volume and
  its principal radii of gyration as built, before its label. Two that
  agree to 1e-6 are the same piece in either place (a back bar is the front
  one turned over); two different pieces within 1 % on the volume and 1 mm
  (or 1 %) on every radius are look-alikes, named in the receipt and the
  card, a report and not a stop, since the labels tell them apart. At
  15 mm: **c and f** (0.23 % in volume, 0.03 mm in their radii; 39.3 against
  39.4 mm hole to hole), and **plate first and plate middle** (the middle
  plate's sockets).
- **The assembly order** is made from the layer table and the joints:
  bottom up, a layer at a time, front before back within a layer. A pin
  goes in with its lowest bar, its head under it -- in first, lowered to
  its place, and the bar laid on over it: pushed up from below with the
  bar laid, pin FC is forced 6.2 mm through the plate under its hole --
  and the bars over it slide on; the front and back legs interleave on
  the crank pin (back j layer 1, front j 2, front k 4, back k 5) and on
  nothing else. The crank is turned before any cap goes on. A cap goes on
  over its pin's highest bar, with the crank where no bar in a layer above
  covers it: the back frame pivot's at 15°, since the back k passes over
  it. The last plate goes on before the last disc: the disc's flange is
  over the plate, which is forced 7.6 mm going down over it.
- **The order is checked as it is done** (`check_sequence`,
  [sequences](../../docs/assemblies/README.md#assembly-sequences)): the
  leg pair put together piece by piece (`put_together`: the stage-1
  assembly with the frame and the crankshaft as the plates, discs, crank
  pin and handle they are pressed from, each fastened where it sits in
  its whole, the wholes hidden but still carrying the crank and the
  pivots), and the card's steps stated as its sequence -- each piece slid
  home on its path against every piece before it, the 15° caps after a
  pose step turning the crank there. Any step that fails stops the run.
  For the leg pair at 15 mm, as the run writes it:

1. Press the handle into the first disc's arm. Stand the first plate on the
    bench, its pivots up; put the first disc in its hole, flange under, and
    press a crank pin into the disc's top socket, its flat toward the axis.
2. layer 1: F BCD -- its B hole over the plate's front frame pivot; pin FC
    up through its C hole from below, head under; pin FD up through its D
    hole from below, head under; the pins in first, the bar laid on with
    them
3. layer 1: B j -- its A hole over the crank pin; pin BC up through its C
    hole from below, head under; the pin in first, the bar laid on with it
4. layer 2: F j -- its A hole over the crank pin; its C hole down over pin
    FC
5. layer 2: F c -- its B hole over the plate's front frame pivot; pin FE up
    through its E hole from below, head under; the pin in first, the bar
    laid on with it
6. layer 2: F f -- its D hole down over pin FD; pin FF up through its F hole
    from below, head under; the pin in first, the bar laid on with it
7. layer 2: B BCD -- its B hole over the plate's back frame pivot; its C
    hole down over pin BC; pin BD up through its D hole from below, head
    under; the pin in first, the bar laid on with it
8. layer 3: F EFG -- its E hole down over pin FE; its F hole down over pin
    FF
9. layer 3: B c -- its B hole over the plate's back frame pivot; pin BE up
    through its E hole from below, head under; the pin in first, the bar
    laid on with it
10. layer 3: B f -- its D hole down over pin BD; pin BF up through its F
    hole from below, head under; the pin in first, the bar laid on with it
11. layer 4: F k -- its A hole over the crank pin; its E hole down over pin
    FE
12. layer 4: B EFG -- its E hole down over pin BE; its F hole down over pin
    BF
13. layer 5: B k -- its A hole over the crank pin; its E hole down over pin
    BE
14. TURN the crank a full turn by hand before capping: every joint must turn
    freely, nothing may catch.
15. a cap pressed onto pin BC, over its layer 2 bar, down to its shoulder
16. a cap pressed onto pin FC, over its layer 2 bar, down to its shoulder
17. a cap pressed onto pin FD, over its layer 2 bar, down to its shoulder
18. a cap pressed onto the plate's front frame pivot B, over its layer 2
    bar, down to its shoulder
19. a cap pressed onto pin BD, over its layer 3 bar, down to its shoulder
20. a cap pressed onto pin FF, over its layer 3 bar, down to its shoulder
21. a cap pressed onto the plate's back frame pivot B, over its layer 3 bar,
    down to its shoulder, the crank turned to 15 deg so no bar is over it
22. a cap pressed onto pin BF, over its layer 4 bar, down to its shoulder
23. a cap pressed onto pin FE, over its layer 4 bar, down to its shoulder
24. a cap pressed onto pin BE, over its layer 5 bar, down to its shoulder
25. The last plate over the last crank pin and onto the standoffs' stubs;
    the last disc down through it onto the crank pin, its socket on it,
    flange up.

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

1. Print `stage1_plate.stl`. Press each of the press row's six caps (the
   notches count which) onto its pin: three steps 0.1 mm apart on the
   diameter, two caps at each, 1 and 2 the tightest. Keep the smallest
   step whose two caps both hold by hand and do not split; a pair that
   disagrees is the print's scatter, and the next step is the one to trust:
   `print_coupon.py --record --profile "K5 PLA on P1S" --press-side MM`.
   Then rebuild: the caps are modelled at the recorded press.
2. Assemble the leg pair in the card's order (above). Turn the crank by
   hand before capping: every joint must turn
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
| stage 1 | one leg pair (12 bodies), 8 pins, 10 caps, the first and last plates and discs, the crank pin, the handle, and the press row (a plate of six pins and six caps): 37 pieces, 43 solids |
| stage 2, 1 | first, last and 3 middle plates; first and last discs; the 6 crank pins; 3 front k, 2 front EFG |
| stage 2, 2 | 2 middle plates; 6 back EFG, 4 front EFG, 3 front k, 2 back k, 1 front j |
| stage 2, 3 | 4 middle discs, the handle; 6 front and 6 back BCD, 4 back k, 3 front j; 5 pins |
| stage 2, 4 | 1 middle disc; 6 front and 6 back c, 6 front and 6 back f, 6 back j, 2 front j; 43 pins, 60 caps |

Bambu's slice of the jobs: stage 1 in 2.7 h and 83 g; the walker's four
plates in 11.4 h and 324 g (2.9, 2.5, 2.6 and 3.4 h).

**Stage 1 against the leg pair printed and assembled**
(`scratch/strandbeest/old-2026-10-17d/stage1`): **every stage-1 piece
changes but the caps.** Every bar and triangle (front and back j, k, c, f,
BCD and EFG), every joint pin (front and back C, D, E and F), both plates,
both discs, the crank pin and the handle now carry their engraved labels;
their shapes are otherwise the same, and a pair already assembled works as
it is. The press row is new: six pins in three steps of 0.1 mm, two caps a
step. The caps are the same parts. Reprint the press row to find the press;
reprint the rest for the labels.

The assignment comes from a MaxRects packing of each piece's box, turned to
its tightest, half a bar apart. At 12 mm, three plates hold the walker.

## Not built

- **No path in the Motion window.** A trace is a request parameter, and
  nothing in the document holds it for the window to draw.
- **The loads are the statics'.** No inertia, no joint friction (the
  margin stands for it), a flat floor; the standoffs and the caps' stubs
  are not studied (the caps take no load across the pin, the standoffs
  only the frame's own).
- **Packing goes by boxes.** Triangles nest poorly by their boxes, so four
  plates hold the 15 mm walker where three might.
