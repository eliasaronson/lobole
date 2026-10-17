# A printed pendulum clock

## The problem

The print dogfoods so far are mechanisms that turn when pushed by hand. A
clock has to run on its own, on a few hundred grams of falling weight, and
keep a rate that a phone stopwatch can check. That makes three demands
nothing in Loböle has met yet:

- **an escapement.** The app animates one (the watch's Swiss lever, two cam
  relations from a turning wheel) but has no anchor, no Graham geometry and
  no pendulum;
- **a train that runs on its own power.** The printed gear sets so far
  (planetary, counter) were turned by a hand that can push through a tight
  mesh. A clock train sees grams at the escape wheel;
- **a prediction about time.** The design states a period, a rate and a
  run per winding before it is printed, so the print tests the design, as
  the hook's breaking load did.

Everything is printed, as with every print dogfood: plates, arbors, pillars,
nuts, the cord barrel's click. The weight is the jar
(generators/jar/) filled with sand or coins, on a cord from the drawer.

## The shape of the fix

`generators/clock/build.py`, output in `scratch/clock/`, sizes derived from
`--period` (the pendulum's, default 1 s) and the print profile, as the
counter and hinge box derive theirs.

### The movement

- **A Graham deadbeat escapement.** A 30-tooth escape wheel and an anchor
  spanning 7½ teeth, with locking and impulse faces. The escape wheel steps
  forward only (no recoil), which the `intermittent` relation can express
  and a recoil anchor could not. Every face is sized from the tooth pitch,
  the drop from the profile's gap a side.
- **The pendulum** is a printed rod and bob on a suspension the anchor's
  crutch drives. Its length comes from the period as a compound pendulum:
  `T = 2π √(I / (m g d))`, with `I`, `m` and `d` taken from
  `mass_properties` of the rod and bob as modelled, not from the simple
  `L = g (T/2π)²`. At 1 s the rod is about 250 mm and prints diagonally on
  the P1S bed.
- **A rating nut** on a printed thread under the bob (threads are proven by
  the jar). The generator states the rate change per turn of the nut in
  seconds a day.
- **The going train.** Escape wheel → third wheel → centre wheel (one turn
  an hour, the minute hand), with a 12:1 motion work to the hour hand.
  The tooth counts are chosen by the generator for the period (at 1 s the
  escape wheel turns every 30 s, so escape to centre is 120:1).
  - **Cycloidal clock gearing** for the 6–10 leaf pinions, from
    generators/watch/watch_gearing.py (moved to a shared module), at a
    module the profile can print: the tip land at least two lines wide, as
    `tip_land` checks in the planetary and counter.
- **The drive.** A cord barrel on the centre arbor with a printed click
  (the `click` relation) so the clock winds backwards without turning the
  hands. The run per winding is the drop height over the cord per turn.

### How it is held together

Two frame plates on four printed pillars, closed by printed nuts on printed
threads. Every arbor runs in plain holes in the plates at the profile's
small-size gap (0.15 mm a side on K5). Each wheel and its pinion is one
body printed flat. So every piece starts on the bed, and nothing prints
onto anything else. No print-in-place is needed.

### In Loböle

- **The assembly**: revolutes for every arbor, `gear` relations through the
  train and the motion work, the `click` on the barrel.
- **The escapement, as the watch does it**: a hidden "beat" revolute
  drives the escape wheel through `intermittent` (period 360° per tooth,
  the drop as its window) and the anchor and pendulum through a `cam` whose
  table is the anchor's swing. This is a stand-in, not a simulation. The
  motion study's clash check is what proves the pallets clear the teeth
  over the swing and that the locking faces meet a tooth at the end of each
  drop. The README says so.
- **FEM**: a static study of the pendulum rod under its bob, and of the
  barrel arbor under the weight's torque, from the weight the generator
  asks for.

### Checked every run

- `check_geometry` verified on every part;
- `check_print` passes every item on every plate;
- the ratios: escape wheel to minute hand to hour hand, read from a motion
  study, as `watch.py`'s `check_ratios` does;
- the escapement study: one escape-wheel turn at 4 samples per beat, no
  clash, the wheel locked at the end of every beat;
- **negative controls that must fail**: a pallet set too deep clashes with
  the wheel; a train with one tooth count wrong fails the ratio check.

### What it predicts

Written to the receipt and the README's test card before printing:

- the period and the rate in seconds a day, and the change per turn of the
  rating nut;
- the run per winding;
- the torque at the escape wheel per 100 g of weight. Friction is not
  modelled, so the weight needed is found on the bench and recorded.

### Staged on the printer

1. **The escapement alone**: escape wheel, anchor, pendulum and a weight on
   the escape arbor, in a short frame. It must tick unaided for five
   minutes. One plate.
2. **The full movement**, with hands and a dial ring.

## Files

- `generators/clock/` (the generator and its README), a row in
  `generators/README.md`, a `generator_clock` test.
- `generators/watch/watch_gearing.py` → a shared module the watch and the
  clock both import.

## Verified by

- the generator's own checks and negative controls, at `--period 1` and at
  one other period;
- `generator_clock` and `generator_watch` in the generators suite (the
  watch still builds after its gearing moves);
- the headless suite;
- on the printer: stage 1 ticks for five minutes; the full clock runs an
  hour and its rate is read against a phone over a day.
