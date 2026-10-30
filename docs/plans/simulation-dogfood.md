# Simulation dogfood: the tea alarm measured

The tea-ready alarm is designed and checked in the app
([generators/tea_alarm](../../generators/tea_alarm/README.md)): its strip's
travel, its trigger and its scatter, its heat path and its clip are all
predictions. What is left needs the parts in hand: KANTHAL 94S strip, the
1.4301 rod and sheet, the ball, the screw.

## The problem

Every number the alarm rests on is a maker's constant or a bound:

- the strip's specific deflection is Kanthal's catalogue value, ± 4 % on
  its curvature, for strip as Kanthal rolls it, not as cut and clamped;
- the trigger's scatter is predicted from that tolerance and from a
  friction (K per mN) nobody has measured;
- the printed block's margin covers conduction only, under an adiabatic
  mount in still air, with no steam.

## The shape of the work

Build one alarm from the drawings (the clip's flat pattern and bend table,
the rod's bend table, the knob's dial) and the plate, then:

- **The strip's travel**: the strip as cut, clamped as the frame clamps it,
  in a water bath stepped from 20 to 95 °C against a thermometer; its tip
  height against A = a ΔT L² / s, which measures the 94S constant on the
  part as cut.
- **The trigger**: the alarm on a mug of the `--vessel-mm` it was built
  for, tea poured at the boil, a probe thermometer in the tea, ten
  cool-downs at the knob's 55 °C mark; the mean against the receipt's
  trigger and the spread against its scatter (the strip's ± 4 % and the
  K per mN).
- **The block's temperature**: a probe on the block's mount face through a
  cool-down from the pour, against the fin's still-air bound.
- **The ball**: weighed, and the probe's force recomputed from its mass.

## Files

`generators/tea_alarm/README.md` gains the measurements beside the
predictions; `ta_spec.py`'s constants stay the handbook's, and the
measured ones are recorded beside them, not substituted.

## Verification

The measurements sit beside the predictions in the generator's README; a
trigger outside the predicted scatter is a finding for the round that
follows.
