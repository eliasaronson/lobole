# Dogfood round — a gas grill

## Why this object

The sheet-metal workstream (S1–S11) shipped against unit tests and a smoke.
Nothing has yet built a whole product out of it. A three-burner propane grill
is almost entirely brake-formed sheet: a firebox pan, a faceted hood with end
caps, heat tents, a grease tray, a control panel, and a cart made of folded
panels. It also has the parts sheet metal has to live with — burner tubes, a
manifold, valves and knobs, grates, wheels, a tank — so the assembly is a real
one rather than a folder of plates.

It exercises, in one object, the things that were only ever tested apart:

- a pan with **corner relief** at four corners, and **closed corners** where
  the hood's facets meet its end caps;
- **hems** on every edge a person touches (tray, doors, shelves) — the reason
  hems exist;
- **jogs** where a panel steps to lap over another;
- a **normal cut** through a bend: the burner-tube ports through the firebox
  end wall, and the valve-stem holes through the control panel's face;
- **flat patterns** on drawings with **bend tables**, and a **DXF per sheet
  part** — the deliverable a shop would actually cut.

## What gets built

`generators/grill/` — one module per subassembly, the layout `generators/bike`
already uses, driven against a scratch instance:

| module | what it makes |
|---|---|
| `grill_spec.py` | every dimension, in one table, derived from the cooking area |
| `grill_lib.py` | sheet parts, edge picks by geometry, blanks, the checks |
| `grill_firebox.py` | firebox pan, hood facets + end caps, heat tents, grease tray |
| `grill_cart.py` | side panels, back, bottom shelf, doors, side shelves, control panel |
| `grill_hardware.py` | burner tubes, manifold, valves, knobs, grates, warming rack, wheels, tank, regulator, handle, thermometer |
| `grill_assemble.py` | occurrences and mates, exploded view |
| `grill_shop.py` | flat patterns, drawings with bend tables, DXF out, the checks |
| `grill.py` | the driver that runs them in order |

The model lands in `scratch/grill.dsgn`; the DXFs, the drawing and the
renders in `scratch/grill/`.

## How it is verified

Nothing here is verified by looking at a render alone.

- Every sheet part passes `check_sheet_metal` with no violations, and its flat
  pattern is **exactly one gauge thick** — the check that catches a bend that
  did not rotate.
- Every part passes `check_geometry`.
- The assembly's exact `interference` sweep is empty, apart from the joints
  meant to be interference fits.
- Each sheet part exports a DXF that contains its outline, its holes and a
  bend line per bend, on `BEND_UP`/`BEND_DOWN`.
- The firebox's blank is checked against hand arithmetic: panel + bend
  allowance + wall, the same sum `test_unfold.cpp` asserts, so a wrong
  K-factor cannot hide behind a plausible-looking part.
- A drawing sheet carries the folded part and its blank side by side, with a
  bend table, and exports to PDF.

## What this round is for

Defects. Every place the app fights back is fixed as it is found, each as its
own commit, and listed in "Findings" at the bottom of this file. A round that
reports no findings has not been run properly.

## Findings

Nine defects, eight of them fixed as they were found. Every one came from a
part a shop would actually make, and none of them was visible from the
feature tests that shipped with S1–S11.

### Fixed

1. **A pan with a rim would not flatten** (`cfd299f7`). Any pan whose rim runs
   the full length of its wall meets the wall beside it at the corner and
   fuses to it — a corner of continuous metal, which folds from no blank. The
   refusal was right; what it SAID was "bend 2 has no wall past it to lay
   flat", about a wall plainly visible in the viewport. It now names the
   fusion and both ways out. (The firebox and the grease tray both need this:
   their rims are inset by a radius and a thickness.)
2. **Bend relief relieved nothing on the default mold line** (`05221f58`). The
   notch's depth was measured from the setback line rather than past the
   tangent line, and on a `material_inside` flange the two are the same
   number — so the relief came out exactly as deep as the setback, stopped
   dead at the tangent line, and left the metal to tear. `check_sheet_metal`
   had been right all along, and the part claimed a relief it did not have.
3. **A hem could not stop short of its edge** (`d966b8fb`, format v116). Which
   made a hemmed tray — most trays — impossible: the hem fused to the wall
   beside it, as in (1), and the only fix was a field `EdgeFlange` had and
   `Hem` did not, though they share a builder that already took it.
4. **The unknown-field refusal named a vocabulary that was wrong**
   (`728347c6`). It listed the fields the feature had just serialized, so a
   Hem built with defaults answered "Accepted: type, edges, length, name,
   type" — omitting `kind`, `gap` and `outside`, which are all accepted, and
   saying `type` twice.
5. **Every hem failed the minimum-radius check** (`033cf209`). A hem's radius
   is the gap its return leaves, a twentieth of the sheet, because it is
   crushed rather than bent round a die — the feature said so in its own
   documentation and nothing downstream could tell. Four correct folds on the
   grease tray were called violations. The same tray showed the other half:
   an inset hem stops its fold inside the panel exactly as an inset flange
   does, and now takes the document's relief for it.
6. **Corner relief and the DFM check disagreed about what a corner is**
   (`8c33c686`). On a heat tent — a ridge folded up, end tabs folded down —
   `check_sheet_metal` called the corners out as tearing and `CornerRelief`
   refused to cut them ("no two bends of this body meet at a corner"). The
   pairing wanted both walls off the same FACE; two bend lines that cross
   tear the corner whichever way each wall turns.
7. **A normal cut through a flat panel was a 380-sided polygon** (`1074f1a2`).
   Every normal cut laid its outline down as a polygon sampled every quarter
   millimetre — the machinery a cut ACROSS A BEND needs, applied to cuts that
   land on one flat panel. The firebox's four holes left it with 1004 faces;
   a Ø30 hole came out as 383 faces and 766 straight edges, within a tenth of
   a percent by volume and wrong in the DXF, in the file, and in the cost of
   every boolean after. It is now 7 faces and an exact circle.
8. **A normal cut made one hole out of three** (`ba11315b`). The feature took
   the first region of its profile and dropped the rest without a word: the
   firebox got one burner port, and two of its burners ran through solid
   steel. Only the clash sweep caught it.

### Open

9. **A circular corner relief on a fold that is not square leaves the flat
   pattern non-planar.** On the heat tent's 60° ridge the blank comes out
   1.67 mm thick on 1 mm sheet (2.27 mm at 45°); a rectangular relief of any
   size on the same part is exactly 1.00. 90° and 120° folds are fine either
   way. Reproduction: a blank, a flange at 60°, two tabs folded the other way
   on the adjacent edges, `CornerRelief` with the default circular style,
   then a flat pattern — and measure the blank's thickness. The grill's tents
   use a rectangular relief, which is what a shop would punch there anyway.

### What held up

- The exact clash sweep: 780 pairs of 40 occurrences in 3.7 s, and every
  clash it reported was a real modelling mistake of mine — a burner through a
  wall, a lid hinged upside down, two wheels collapsed onto one axle frame,
  a bottle taller than the cart it stands in.
- `check_sheet_metal` found four genuine faults in parts that looked right.
- The blank of the firebox matches the bend-allowance arithmetic —
  987.3 × 807.3 mm against panel + 2 × (BA + wall + BA + rim), BA 3.322 mm —
  computed here from the formula and the document's own K, not read back from
  the unfolder.
