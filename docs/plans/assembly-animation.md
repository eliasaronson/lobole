# Assembly animation: the Strandbeest's film

What is built is in docs/assemblies/README.md#playing-and-filming-a-sequence:
a sequence's timeline (groups and poses included) laid out from its check,
Play / scrub / Stop in the Motion window, `export_assembly_video`, and the
clock's films (`--video`). One generator is left.

## The problem

The Strandbeest states a sequence with a pose step (its back pivot cap goes
on with the crank at 15°) and has no film of it, so a person building it
still has to picture the order and the turn of the crank.

## The shape of the fix

`generators/strandbeest/build.py` takes `--video`, as the clock does: after
each stage's sequence is checked, the camera set from the linkage's own
size and `fit_view` at the film's size, `export_assembly_video` writes
`stage<N>_sequence.mp4` beside the card. Off by default, so the
generator test (36 minutes already) does not grow; refused with a
`--headless` instance (no renderer), as the clock refuses `--no-render`.

## Files

- `generators/strandbeest/build.py`, `generators/strandbeest/README.md`.

## Verified by

- The film made with `--video` and looked at: the frames show the parts in
  the sequence's order, the crank turning to 15° before the cap goes on.
- `generator_strandbeest` still passes without the flag.
