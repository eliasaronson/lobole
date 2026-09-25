# The FS chassis

A Formula Student steel space frame, and the order to make it: every member
coped or mitred to its neighbours, a cut list, a bend schedule, the number of
six-metre sticks to buy, and — with `--shop` — a STEP per member, a DXF per
flat blank and a weld jig. Its point is the ORDER as much as the model. It
uses the tube system throughout ([docs/tube/README.md](../../docs/tube/README.md)).

## Running it

```sh
python3 generators/chassis/chassis.py --port <port> [--out DIR] [--shop DIR] [--save PROJECT] [--contact]
```

- Without options it builds the frame, asks `check_geometry`, `check_frame`,
  `check_tube`, `tube_schedule` and `mass_properties`, and writes
  `chassis_cut_list.csv` (packed into 6 m stock with a 2 mm kerf) and
  `chassis_bends.csv` to `--out` (default `scratch/`).
- `--shop DIR` also builds the floor, the firewall, the suspension pickups and
  the jig plates, and writes the whole order into `DIR`:
  `members/*.step`, `cut_list.csv`, `bend_schedule.csv`, `panels/*.dxf`,
  `tabs/*.dxf`, `jig/*.dxf` (each blank marked with its name) and
  `manifest.csv` — one row per file: what it is, the part, material, size and
  quantity.
- `--save PROJECT` keeps the project to open and look at.
- `--contact` runs the exact `contact_report` over the whole frame, which
  answers whether it is ONE connected thing; it takes minutes, so it is
  opt-in. Without it the cheaper half of the question still holds: every
  cope refuses unless it actually cut metal.

## Where the frame comes from

The frame is a team's own SolidWorks CAD, read without SolidWorks.
`sldprt_frame.py` does it once and writes `fs_chassis_frame.json`, which is
committed so the generator runs without the CAD and a changed frame shows as
a diff:

```sh
python3 generators/chassis/sldprt_frame.py FRAME.SLDPRT [--out fs_chassis_frame.json] [--preview preview.png]
```

A SolidWorks 2015+ part is a container of RAW-deflate streams (no zlib
header). Two of them are Parasolid transmit files, each itself zlib-wrapped;
the extractor reads the largest and takes only what a tube frame is made of:

- CYLINDER records (centre, axis, radius, reference — big-endian doubles in
  that order). An outer and an inner cylinder on one axis are a tube: its
  axis, OD and wall.
- TORUS records: the bends and their radius.
- The points Parasolid samples along the curves where tubes meet, which bound
  each member's extent (isolated clusters of three or fewer are discarded).

From those it builds a NODE GRAPH — where two axes pass within the sum of
their radii is a joint, joints a few millimetres apart are one node — makes
it exactly mirror-symmetric about y = 0, and writes straight members as node
pairs and each hoop as ONE bent member through its corners on the CAD's own
bend radius. It refuses what does not add up: a member with no mirror twin, a
bend with no two legs, a joint only one member reaches. It reads geometry,
not the feature tree, so there are no names and no 3D sketch; tabs and
brackets (separate parts in the assembly) and short sleeves are not read.

The JSON holds `source`, `units` (mm; x rearward, y across, z up), `nodes`,
`members` (nodes, OD, wall, which nodes each runs `through`), `bent` (feet,
corners, radius, OD, wall) and `set_aside`.

## How it is built

| file | what |
|---|---|
| `chassis_spec.py` | the frame from the JSON, and the SHOP: material, the bender's formers, stock length, kerf, weld gap, panel and tab gauges. The stations the jig and panels use are DERIVED from the node table |
| `chassis.py` | names and ranks every member, builds the frame, asks the questions, writes the order |
| `chassis_frame.py` | `Frame`: a member is a `Curve3D` `bent_path` and a `Pipe`; joints are `Cope`s |
| `chassis_panels.py` | floor, firewall and suspension pickups as sheet parts with DXF blanks |
| `chassis_jig.py` | the weld jig's station plates |
| `sldprt_frame.py` | the SolidWorks reader above |

**Stock.** Sizes are the CAD's, read from each member's outer and inner
radius. The CAD's imperial weldment profiles are rounded to 0.01 mm, so
`chassis_spec.NOMINAL` maps each to the tube it stands for (28.57 × 1.47 is
1-1/8" × 0.058"); anything unmapped is ordered as drawn and `check_tube` says
if nobody stocks it. The hoops are 30 × 2, which the spec checks against the
FS rules' alternative-tubing allowance (a wall of at least 2.0 and the same
bending stiffness). Material is steel; the bender owns 76.2, 100, 101.6 and
152.4 mm formers.

**Names.** A weldment's members are nameless, so each is named from where it
is and which way it runs (zone: nose, footwell, cockpit, engine bay, rear;
kind: rail, lower rail, cross, upright, diagonal; side L/R), numbered front to
back where names repeat. Members the rules name get those names (main hoop,
front hoop, their braces, shoulder harness bar).

**Rank.** Which member copes to which is read off the node graph: a member a
node sits in the MIDDLE of runs through it and is tacked before any member
that ENDS there; where several end at one node the heavier, then the longer,
goes first. Every member gets its own rank, so every pair that meets is cut
by exactly one Cope, from the member of higher rank.

**Joints.** For each pair that meets, `Frame.fit` asks the nodes: an end that
lands clear of the neighbour's own ends is on its SIDE (`against`, a
fishmouth); an end at one of the neighbour's ends is a corner (`mitre`).
Joints are tested against the centreline the METAL follows — a hoop turns
short of its corner, 63 mm on a 152 mm former — not the corners that were
typed. Only mitred ends are swept a diameter long. Every cut is opened by
`WELD_GAP` (0.25 mm), which also keeps equal-diameter joints drawn to their
node off an exact tangency.

**Panels and tabs.** The floor and firewall are 1.5 mm steel. A suspension
pickup is a 4 mm plate standing in a plane THROUGH its tube's axis, so the
rod end pulls along the tube; its seat is a cylinder cut across the plate's
thickness, so its flat pattern's edge is straight and the blank is cut to
the far side of the seat, leaving a crescent at the near face as the weld's
root gap. Pickups sit on the nose's lower rails.

**Jig.** A 5 mm plate at five stations (bulkhead, nose ring, front hoop, main
hoop, rear), each notched from its top edge so a tube drops IN: a round seat
0.5 mm clear of the tube, and above it a slot 0.25 mm clear a side. The slot
is narrower than the seat on purpose: one as wide or wider, floored on the
seat's axis, came back from the kernel uncut. A plate seats
only members that CROSS its plane — at least a quarter of the member's
direction across it, so a tube lying in the plane is not cut out of it — and
only those within 220 mm of the lowest, so the plate stays low and its seats
shallow. The hoop stations are offset 60 mm behind the hoop so the hoop itself
is not notched. An angled seat is an ellipse, which R12 DXF cannot name, so
it is written as line segments.

## What a run reports

`check_frame` on the real car reports engine-bay diagonals landing on the main
hoop's knee bend and bays that are not triangulated (panels close several);
both are as the CAD has them. The frame is 91 members in five tube sizes.
Mass is printed; the CAD's own mass is not among the streams read, so there
is no second number to hold it against.

## Not modelled

Welds as objects (an overlap between two members is the weld), the team's
tabs and brackets beyond the pickups, engine mounts, anything outside the
frame part.
