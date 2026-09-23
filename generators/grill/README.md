# Gas grill

A three-burner propane grill on a cart, built almost entirely from
brake-formed sheet: firebox pan, faceted hood, heat tents, grease tray,
control panel, and a cart of folded panels, doors and shelves — plus the
burners, manifold, valves, knobs, grates, wheels, tank and lid handle that
sheet metal has to live with. Every part is mated, every joint is designed
(bolted, welded, pinned or resting), and every folded part leaves as a DXF.
It exercises the sheet-metal features together
([docs/sheetmetal/README.md](../../docs/sheetmetal/README.md)): corner relief,
closed corners, hems on every edge a hand touches, jogs, normal cuts through
bends, flat patterns, bend tables.

## Running it

```sh
python3 generators/grill/grill.py --port <port> [--only firebox|cart|hardware|assembly|shop ...] [--keep]
```

It always starts from a new project unless `--keep`. `--only` may be repeated;
an unknown stage is refused naming the accepted ones. The shop stage writes a
DXF per folded part and a drawing into `scratch/grill/` (relative to where it
is run).

## How it is built

| module | what |
|---|---|
| `grill_spec.py` | every dimension, derived outward from the cooking area (620 × 440, three burners): the grate decides the firebox, the firebox decides the cart (it rests on the cart sides' top flanges, so the cart is exactly as wide), the gas train decides how much deeper the cart is. Each part's sheet stock, and every place two surfaces touch |
| `grill_lib.py` | sheet parts with their stock, edge picks by geometry (which of a rim's edges is picked decides which way a wall folds), flanges, reliefs, blanks, frames |
| `grill_firebox.py` | firebox pan, hood facets and end caps, heat tents, grease tray, the ledges and rails welded inside |
| `grill_cart.py` | side panels (handed), back, shelf, doors, side shelves, control panel |
| `grill_hardware.py` | burners, manifold, valves, knobs, grates, warming rack, wheels, axle bolts, feet, tank, regulator, lid handle, thermometer, hinge pins and leaves |
| `grill_assemble.py` | occurrences, mates, and the joints that hold each one |
| `grill_shop.py` | bend audit, tube schedule, blanks, the firebox blank checked by hand, the drawing |
| `grill.py` | runs the stages in order |

Build order matters: some parts are measured off others — the hood's end
caps off the facets that landed on the rim, the lid's hinge ear off the
facet's own plane, the grease tray off the rail it slides on — and those
numbers are read from the app rather than computed a second time.

Stock, from `grill_spec.py`: firebox, ledges, rails and brackets 1.5 mm steel;
hood and side shelves 1.2 mm steel; cart 1 mm steel; grease tray 0.8 mm
steel; control panel and heat tents 1 mm stainless. Every bend is at the
table's minimum radius for its metal, with the table's K and a rectangular
relief.

The lid handle is a tube: a `Curve3D` `bent_path` and a `Pipe`, with a plug
in each end carrying the tapping for the bolts through the lid.

## How it is held together

One occurrence (the left cart side) is fixed; everything else is mated to a
named socket on the part that carries it, so a dimension change moves the
sockets and the parts follow. Parts used on both sides carry handed sockets.

- **Bolted** — a clearance hole through both parts and `fit_fastener` on the
  rim, which derives the size from the hole and the length from the stack and
  mates what it clamps: the firebox to the cart's top flanges; back, shelf,
  control panel and side shelves to the sides' flanges; the panel's lip to
  the firebox; each valve to the panel; a screw through the back wall into
  each burner's tapped tail; the handle through the lid; the feet; the door
  hinge leaves.
- **Welded** — mated flush, face on face: the ledges for grates, tents and
  warming rack; the grease tray's rails; the lid's hinge ears.
- **Pinned** — the lid on two pins through its ears, each door on a hinge
  whose two leaves are one part turned over, each wheel on a shoulder bolt;
  these are revolute.
- **Resting** — grates, tents and rack on their ledges, the tray on its rails,
  the tank on the shelf, seated with a tenth of a millimetre of clearance.

## What a run checks

- `contact_report`: one connected component over every occurrence, and no
  clash.
- `check_fasteners` on every derived fastener.
- A bend audit per sheet part (`list_bends` beside `check_sheet_metal`, whose
  bend counts must agree): every radius against the material's minimum, the
  K and its source, the relief, the holes checked, no violation. Crushed hems
  are exempt from the minimum radius.
- The firebox blank against the base less its setbacks plus, per side,
  BA + (wall − setback) + BA + rim — computed from BA = θ(R + K·t) and the
  document's own K, not read back from the unfolder — to 0.05 mm.
- A drawing with the folded part, its blank with bend lines, and a bend table.

## Not modelled

The hose from regulator to manifold, an igniter, a door catch, and the split
pins that retain the hinge pins (their cross holes are there).
