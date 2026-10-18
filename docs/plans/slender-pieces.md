# Slender pieces: a check that sees them, and shafts that print lying

## The problem

The clock's stage-1 plate 1 failed on the printer: all four pillars (123 mm
tall on a Ø5.0 end, 24.5:1) and both arbors (79 and 95 mm on Ø4.8, 19.8:1)
came off the bed part-way up, and the nozzle strung the rest of their layers
into the air. `check_print` passed that plate on every item, because no item
looks at how tall a standing piece is against what it stands on. The engine's
camshaft (204 mm on an 8 mm bar end, with a brim) is the same case and has
not been printed yet.

## The shape of the fix

1. **A `check_print` item, `slender`.** For each piece (shell) on a plate:
   its height over the least width of its first-layer footprint (the convex
   hull of its contact with the bed, read the way `bed` reads it), and the
   same ratio at every height over the footprint below — a piece that
   necks down is as weak there as at the bed. It fails a piece past the
   limit, naming the piece, its height, its footprint and the ratio.
   - The limit comes from the print profile, `slender_ratio` (with its
     source, like every measured row), else a built-in default of 8,
     stated as unmeasured in the item's text.
   - A brim on the piece (the per-object settings `export_print_job`
     already writes) widens the footprint by the brim's width; the item
     says when a brim is what makes it pass.
   - Added to `PRINT_ITEMS`, the RPC row, the print checks' docs.
   - Tests: a Ø5 × 123 mm pin fails, the same pin at 30 mm passes, a
     bottle neck (wide base, thin stem) fails at the neck, a brim moves a
     marginal one to a pass. The clock's old plate 1 is the negative
     control and fails `slender` on exactly the six shafts.
2. **The clock's shafts print lying, on stage 1's plate 2.** The plate-1
   pieces the user has already printed stay where they are, so plate 1
   loses only the four pillars and two arbors; they join the pendulum on
   plate 2 (the pendulum's pieces keep their solid settings, the shafts
   take the frame's).
   - Each arbor and pillar gets a flat along its whole length, about two
     layers deep, to stand on; the D-flats where parts press on face up
     or sideways, never down. The flat is derived from the layer height,
     not a literal.
   - The pillar's threads print sideways with the flat at the bottom; its
     thread fits and the pillar-nut fit are re-checked (the flat must not
     cut a thread's engagement below what its requirement asks).
   - Stage 2's arbors (escape, third, centre) are laid down the same way,
     on stage 2's plates.
   - Both stages' `check_print` pass every item including `slender`, the
     arbor strength study still passes (now with the layers along the
     shaft), and the motion studies are unchanged.
   - The card says which pieces moved, so a half-printed stage 1 can be
     finished from plate 2 alone.
3. **The engine's camshaft lies down too**, on the same flat, if 204 mm
   fits the bed lying (it does on a 256 mm bed); its lobes' 45° undersides
   are redesigned for the new up direction or the shaft splits into two
   keyed halves if the lobes cannot print lying. Its standing brim goes.
   Any other engine piece that fails `slender` is fixed the same way. Both
   engine jobs re-exported and sliced.

## Files

- `src/core/PrintCheck.cpp` (+ header, its tests in `tests/`), the print
  profile reader and `docs/` print-check and tolerances pages, the
  `check_print` RPC row, `generators/_lib.py` (`PRINT_ITEMS`).
- `generators/clock/` (clock_parts.py, build.py, README, card).
- `generators/engine/` (engine_parts.py, engine_spec.py, build.py, README).

## Verified by

- the new item's tests, each failing before it;
- the clock's old plate 1 failing `slender` on its six shafts;
- the clock and engine generator tests, every plate passing every item;
- the print jobs re-exported and sliced clean in the Bambu CLI;
- the headless, GUI and generators suites.
