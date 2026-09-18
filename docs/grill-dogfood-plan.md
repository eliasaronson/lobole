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
| `grill_assemble.py` | occurrences, the 39 mates that hold them, exploded view |
| `grill_shop.py` | flat patterns, drawings with bend tables, DXF out, the checks |
| `grill.py` | the driver that runs them in order |

The model lands in `scratch/grill.dsgn`; the DXFs, the drawing and the
renders in `scratch/grill/`.

## How it is held together

One occurrence is fixed — the left-hand cart side — and every other part is
placed by a MATE to a named socket on the part that carries it: the firebox
to the cart, the lid to the firebox's hinge, each valve to its own place on
the manifold, each wheel to its end of the axle. No occurrence carries a
transform typed into the assembly, so a dimension change moves the sockets
and the parts follow. The lid, both doors, both wheels and the three knobs
are revolute; the other 32 joints are fastened.

The parts themselves are drawn, not assembled out of primitives: a propane
bottle is a profile revolved about its axis, a knob is a turned profile, and
the lid handle is a tube swept along its own bent centreline (a `Curve3D`
through the bends, then a `Pipe`) — which is what a handle IS. Two cylinders
unioned at a square corner is a picture of something nobody can make.

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

### Found in the second pass (the assembly)

Mating everything, rather than typing transforms, found four placement
faults that the eye had let through — each one visible only once the solver
put the part where the mate said it belonged:

- both wheels collapsed onto the axle's single centre frame;
- the lid hinged about the VERTICAL, because a frame's z is the axis a
  revolute turns on and both hinge frames pointed up;
- one part used on both sides of the cart carries sockets that point the
  wrong way on the second one — the right-hand shelf went through the
  firebox — so those sockets are handed;
- a valve mated to its manifold the way round that put it inside the
  firebox: `flipped` decides which of the eight ways two frames meet, and
  the default is not always the one meant.

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

## Third pass: a grill a shop could build

The second pass MATED every part and still left the grill in sixteen
pieces: `contact_report` found the firebox 1.6 mm above the cart, the back
2.5 mm off the sides, and the burners, tents, grates, rack and tray touching
nothing at all. A mate says where a part goes; it does not say what holds it
there. This pass designs the joints.

### The rule

Every pair of parts that touch is joined by something a shop would do, and
every part touches the structure:

- **Bolted** where it has to come apart or where two pieces of finished sheet
  meet: matching clearance holes in both, and the fastener DERIVED from the
  hole by `fit_fastener` (size from the hole, length from the stack), with a
  washer and nut where there is room for one.
- **Welded** where it is permanent — a ledge inside the firebox, an ear on
  the lid: the two parts are mated FLUSH, face to face, so they touch.
- **Pinned** where it turns — the lid, the doors, the wheels: a pin in a
  line-to-line bore, so the moving part touches what it turns on.
- **Resting** where gravity does it — a grate on its ledge, the bottle on
  the shelf: flush, and nothing more.

### What changes

- The cart is made square and handed: a right-hand side panel, mirrored, so
  both sides' flanges turn IN; the cart narrows until the firebox sits on the
  sides' top flanges, and deepens so the gas train fits in front of it.
- Back, shelf, control panel, side shelves and feet each bolt through a
  flange of the side panels.
- The gas train: each valve bolts to the back of the control panel; the
  burner runs from the valve's orifice to the firebox's back wall and is
  screwed there; the manifold rides on the valves.
- Inside the firebox: welded ledges front and back carry the grates and the
  heat tents, welded brackets carry the warming rack, and welded rails under
  the firebox carry the grease tray.
- The lid turns on two pins through welded ears on the rim and on the lid.
  The handle and the thermometer bolt through the lid.
- The doors hang on pin hinges bolted to the sides; the wheels are held on
  the axle by washers and nuts.

### How it is verified

- `contact_report`: ONE connected component, over every occurrence.
- `interference`: no clash except a fastener in its own clearance hole.
- `check_fasteners`: every derived fastener long enough and not bottoming.
- A bend audit: every bend's inside radius against the material's minimum,
  and every relief's style and size, from the same rules the check uses.

### What the third pass found

Six more defects, five fixed. Every one of them came from asking the model a
question a shop would ask — what holds this on, and what is it bolted to —
and none was visible while the parts were merely near each other.

10. **A normal cut landed on whichever sheet was NEAREST the sketch plane,
    either side of it** (`src/core/sheet/Unfold.cpp`). The feature carries
    each point of its outline along the sketch's normal — that is what S10
    says it does — but the hit test searched the line in BOTH directions and
    took the nearest crossing. The firebox's burner ports, sketched inside
    the box facing its front wall, came out in the BACK wall, which happened
    to be 1.5 mm nearer the plane; the screws for the burners' tails came out
    in the front. Nothing said a word, and the part looked right in every
    view. A panel in front of the plane now wins over a nearer one behind it,
    and a plane with sheet on one side only still cuts it.
11. **A part the axis crosses twice read as one solid block**
    (`src/core/parts/AxisStack.cpp`). `TraverseAxis` reported one segment per
    body, from the first entry to the last exit — deliberately, so a chamfer
    on one of its four rays could not shorten a plate. A folded part is
    crossed twice: the cart's side panel has a flange under the firebox and
    another 650 mm below it at the floor. Ten correct nuts were reported as
    "buried in Cart side left: that part runs from 1.5 mm to 651.5 mm down
    the axis". Intervals now merge only when they overlap or abut.
12. **A second pick AT the floor of a blind hole made the joint a through
    one** (`JointEnd`). Picking the bottom rim of a tapping is how a caller
    says where the joint ends, and that pick lands on the plane the on-axis
    ray found, to a rounding error — read as a truncation, it turned a blind
    hole into a through joint, and a through joint is sized UP to a stocked
    length. The burner screws came back M5x14 in a 13.5 mm hole, bottoming
    out on the floor and clamping nothing.
13. **`check_fasteners` bounded a joint at the LAST nut on the axis**
    (`src/ui/Bom.cpp`). The side-shelf bolts' axis runs across the cart and
    out the other side, where the opposite shelf's bolt has its own nut:
    646 mm away, past the joint and past the grease tray's rail in between.
    Four correct bolts were reported as having no thread in a part they do
    not touch. The joint ends at the FIRST nut, and at a jam nut tight
    against it.
14. **The loop solver moved degrees of freedom it was not solving for**
    (`src/core/AssemblyLoopSolve.cpp`). Every bolt a fastener fits across an
    interface is a mate, so bolting two parts that were already mated closes
    a small loop — and everything hanging off that component then sits in the
    loop problem. Marquardt's damping is RELATIVE to the diagonal, and an
    unconstrained column has none, so the solve handed those DOFs whatever
    satisfied 0 = 0: the lid stood 443 degrees open, with the loop closed to
    a ten-thousandth of a millimetre and every mate "satisfied". A column
    that cannot move the loop by the tolerance it has to close to, moved a
    whole unit, is now held where it was posed — which is what `set_mate`
    already promised.

#### Open

15. **A drilled hole can come out with no NAME on its cylindrical face.** On
    the lid handle — a `Pipe` with a plug unioned into each end — the bore
    drilled in one end had a cylinder no `list_elements` would show, though
    the hole is plainly in the solid and a screw sits in it. A face with no
    name cannot be threaded, picked or fitted, so the screw in it read as
    76 mm3 of clash. The other end, same feature, same call, came out named.
    The handle now gets its tapping as the inner loop of the plug's own
    sketch, which has a name and needs no drilling.

### The joints, and what holds them

51 bends, 43 derived fasteners, 67 placed parts, and the gate the pass was
for: `contact_report` finds **one connected component and no clashes**, and
`check_fasteners` examines all 43 with no findings.

- **Bolted** (43): the firebox to the cart's top flanges; the back, the
  shelf, the control panel and the side shelves to the sides' flanges; the
  panel's top lip to the firebox's front wall; each valve to the panel; a
  screw through the back wall into each burner's tapped tail; the handle
  through the lid into the plugs welded in its ends; a foot under each front
  corner; the doors' hinge leaves to the side panels.
- **Welded** (flush, face on face): the ledges the grates, the heat tents and
  the warming rack rest on; the rails the grease tray slides on; the lid's
  hinge ears, one on the firebox's back wall and one on the lid's back facet
  — the second welded to the plane the app reports for that facet, not to a
  number worked out here.
- **Pinned**: the lid on two clevis pins through its ears; each door on a
  corner hinge whose two leaves are the same part turned over, interleaved
  on one pin; each wheel on a shoulder bolt with a nut inside the panel.
- **Resting**: grates, heat tents and the warming rack on their ledges, the
  tray on its rails, the bottle on the shelf.

Not modelled, and named rather than left to be noticed: the hose from the
regulator to the manifold, an igniter, a door catch, and the split pins that
retain the hinge pins (their cross holes are there).

### Are the bends right for the metal?

`grill_shop.bend_audit` reads it out of the document rather than restating
it: for every sheet part, the material and gauge it was built under, the
inside radius against that material's minimum, the K-factor and where it came
from, and the relief cut where a bend line stops inside a panel.

| part | material | t | R | R min | K | relief |
|---|---|---|---|---|---|---|
| firebox, ledges, rails | steel | 1.5 | 1.5 | 1.5 | 0.41 | rect 2.25 x 3.0 |
| hood | steel | 1.2 | 1.2 | 1.2 | 0.41 | rect 1.8 x 2.4 |
| cart sides, back, shelf, doors | steel | 1.0 | 1.0 | 1.0 | 0.41 | rect 1.5 x 2.0 |
| grease tray | steel | 0.8 | 0.8 | 0.8 | 0.41 | rect 1.2 x 1.6 |
| control panel, heat tents | stainless | 1.0 | 1.5 | 1.5 | 0.45 | rect 1.5 x 2.5 |
| side shelves | steel | 1.2 | 1.2 | 1.2 | 0.41 | rect 1.8 x 2.4 |

Every radius is the table's own minimum for that material and thickness —
R/t = 1 in mild steel, R/t = 1.5 in stainless, which is the reason the
stainless parts bend on a 1.5 mm radius in 1 mm sheet. K follows the same
table (0.41 for steel at R/t < 2, 0.45 for stainless). Relief is rectangular
everywhere, 1.5t wide and 2t deep, measured PAST the tangent line — the thing
finding 2 fixed. `check_sheet_metal` reports no violation on any part, over
51 bends and 146 holes; the ten hems are crushed rather than bent round a
die, and are exempt from the minimum radius for the reason finding 5 gives.
