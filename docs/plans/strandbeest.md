# A printed Strandbeest

## The problem

A Theo Jansen walker is a planar linkage of two coupled loops per leg,
several legs on one crankshaft, and a hundred printed pins. It walks only
if the linkage is right over a whole crank turn and its joints turn freely.
It asks of Loböle what no dogfood has asked yet:

- **coupled loops.** The loop solver closes any number of loops, but every
  test is a single four-bar or slider. Nothing has driven a Jansen-type
  linkage, and nothing guards a sweep from flipping to the mirror branch;
  the carburettor's general four-bar did exactly that and was replaced by a
  parallelogram;
- **a path as the output.** The foot's path is what the linkage is for, and
  a motion study cannot trace a point. Each generator transforms poses by
  hand (hinge_box, carburettor);
- **pins that are assembled, not printed in place.** Every pin so far was
  print-in-place. A walker's pins must turn freely and stay in, which is a
  press or a snap, and the print profile has no press allowance to size one.

## The shape of the fix

### 1. Motion studies trace points

`run_motion_study` gains `trace: [{name, occurrence, point_mm}]`. The
reply gives each named point's world position at every sample, and its
extent (x, y, z ranges). `MotionStudy` computes it from the poses it
already solves, and the motion panel can draw the path. The hinge box,
carburettor and both new generators read it instead of transforming poses
themselves.

### 2. A coupled-loop linkage in the solver's tests

A Jansen leg goes into `tests/test_assembly_loops.cpp`: DOF 1 by rank, a
full crank turn that stays on the built branch (each knee's signed angle
keeps its sign), and the foot path against Jansen's own published curve.
If the solver flips, the test pins it and the fix goes in
`AssemblyLoopSolve.cpp`. Per-sample cost is measured on the full walker
(about 100 revolute columns in one component). Analytic Jacobians are
built only if that cost makes the study unusable.

### 3. A press allowance

`printfit::Printed` gains `Press`, the interference a side for a printed
cap pressed onto a printed pin, as `Gap` is the clearance. It has a
built-in default (to confirm with a coupon) and a coupon row. The coupon
row is pins with caps, stepped in interference. The answer is the
smallest step that holds by hand and does not split. `--record --press-side`
saves it. Docs and RPC rows follow `Gap`'s.

### 4. The walker

`generators/strandbeest/build.py`, output in `scratch/strandbeest/`.

- **The leg** is Jansen's eleven lengths scaled from `--size` (the crank
  radius): eight bars, one crank pin, two frame pivots.
- **Legs.** Three crank phases 120° apart, each with a front and back leg
  sharing its crank pin, on each side of the frame (12 legs). `--pairs`
  sets the number of phases.
- **Layers.** Every bar gets a layer across the crank axis so that no two
  bars that cross over the turn share one. The generator assigns them, and
  the motion study's clash check plus a minimum clearance between adjacent
  layers over the study prove it.
- **Joints.** A pin with a head printed on, through plain holes at the
  profile's small-size gap, closed by a printed cap pressed on at the press
  allowance. All pins and caps print flat on the bed.
- **The crankshaft** is keyed segments, one per phase, so each prints flat
  and the phases are set by the keys, not by hand.
- **Hand crank** on one end.

### Checked every run

- `check_geometry` verified, `check_print` every item, on every plate;
- a 360° study at 2° per sample: solved at every sample, on the built
  branch at every sample (the knee signs), no clash;
- the foot trace: the stride (the flat part's length), the foot lift, and
  the flatness of the stride, all against the leg's own scaled Jansen curve;
- **static stability**: at every sample, the centre of mass (from
  `mass_properties`) lies inside the polygon of the feet on the ground;
- **negative controls that must fail**: one bar 10 % long breaks the foot
  curve's flatness; two crossing bars on one layer clash.

### What it predicts

The distance walked per crank turn, and how high a step it clears. A ruler
and ten turns check it.

### Staged on the printer

1. **One leg pair on its crank**, plus the press coupon row. It tests the
   pins, the friction and the foot path before 12 legs. One plate.
2. **The full walker.** About three plates.

## Files

- `src/core/MotionStudy.{h,cpp}`, `src/remote/RemoteMotion.cpp` (trace),
  the motion panel (draw the path).
- `src/core/PrintAllowance.{h,cpp}`, the Settings window,
  `src/remote/RemoteInspection.cpp` rows (press).
- `tests/test_assembly_loops.cpp`, `tests/test_motion*.cpp`,
  a motion smoke for `trace`, `tests/print_allowance_smoke.py`.
- `generators/print_coupon.py` (press row), `generators/strandbeest/`,
  `generators/hinge_box/build.py` and `generators/carburettor.py` (read
  `trace`), `generators/README.md`.
- `docs/assemblies/README.md` (trace), `docs/assemblies/solver.md`
  (coupled loops, branches), `docs/tolerances/README.md` (press).

## Verified by

- the unit tests above, the smokes, and the headless and GUI suites;
- the walker generator's checks and negative controls at two sizes;
- the generators suite (the hinge box and carburettor after moving to
  `trace`);
- on the printer: the leg pair walks its foot through the predicted path;
  the full walker's distance per crank turn against the prediction.
