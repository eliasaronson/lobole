# Print follow-ups: the Strandbeest's labels, order, look-alikes and press row

The round's first seven items (the counter, the planetary, the seal, the
table law, the assembly's printed mass, stacking at face boundaries,
`--fresh`) are built and in the docs. Four more follow from the Strandbeest
stage-1 leg pair as printed and assembled: it moves as designed and binds a
little.

## The problem

- **The pieces cannot be told apart.** c and f are 39.3 and 39.4 mm hole to
  hole; nothing on a printed piece says what it is or which hole is which
  joint.
- **The assembly order was worked out by hand.** The front and back legs
  interleave on the crank pin, and a pin's head must go in under its lowest
  bar and its cap on over its highest.
- **Nothing flags look-alikes.** Two different pieces nearly the same size
  are found by the person at the bench.
- **The press row could not be read.** Ø3.22 pins, cap bores stepped
  0.05 mm on the diameter, 1 the tightest: cap 1 went on tight and cap 5,
  the loosest as modelled, would not go on at all. The scatter between caps
  is bigger than the step at that size.

## The shape of the fix

A. **Engraved labels, on the Strandbeest only.** Every printed piece carries
   its name, engraved on a face that is neither on the bed nor a bearing
   face: the bars and triangles (j, k, c, f, BCD, EFG, with F/B for front
   and back, and the throw), the pins by joint, the discs, plates, crank
   pins and handle. Each hole on a bar or triangle has its joint's letter
   beside it where there is room. The depth is whole layers, the size the
   face's. Labels must not break `check_print` (overhang, bridges), the
   clash study or the layers' clearance.
B. **A generated assembly order.** The generator writes the build sequence
   into the test card (`strandbeest_card.md` and the README's card), derived
   from the layer table and the joints: bottom-up, every pin's head under
   its lowest bar and its cap on after its highest, the legs interleaving
   on the crank pin (back j L1, front j L2, front k L4, back k L5), "turn
   before capping" before the caps. What it would take to do this from an
   assembly's mates and layers in `_lib` for the engine and the clock is
   reported, not built.
C. **A look-alike check.** Any two different printed pieces whose box and
   volume agree within a stated tolerance (1 %, or 1 mm on the box) and
   that are not interchangeable in the assembly are named in the receipt
   and the card. With labels in place it is a report, not a stop. Expected:
   c and f.
D. **A press row that can be read.** `print_coupon.press_row` steps at
   least 0.1 mm on the diameter and prints at least two caps per step, so a
   step is larger than the print's scatter and the scatter shows. Whether a
   larger reference pin is needed is decided and said. `--record
   --press-side` stays meaningful. The Strandbeest stage-1 press row and the
   standalone `--press-only` coupon use it; both jobs are re-exported and
   sliced.

## Files

- `generators/strandbeest/build.py`, `README.md`, `test.json`.
- `generators/print_coupon.py` (`press_row`), `generators/_lib.py` if a
  shared helper earns its place.
- `docs/tolerances/README.md` (the coupon's press row).

## Verified by

- a check that fails before each: every piece's label found engraved;
  the order puts every head before its bars and every cap after; the
  look-alike report names c and f; the press row has two caps a step and
  steps of at least 0.1;
- `check_print` on both stages and the coupon, the clash studies and the
  layers' clearance, as before;
- `generator_strandbeest` and the coupon's test; the jobs re-exported and
  sliced clean in the Bambu CLI.
