# The FS chassis, read from the team's own CAD

## The problem

`generators/chassis/` builds a Formula Student frame that the owner of the
real one calls neither good looking nor well
designed, and they are right: it was drawn from ONE photograph with every
length assumed, and it came out a long flat box — one floor height from nose
to tail, arched hoops dropped onto it, a boxed rear. The real car has a
raised nose box whose lower rails fall to the cockpit floor, a six-sided
front hoop, a tall main hoop whose feet splay out to a knee, a triangulated
cockpit side, a converging engine bay and an octagonal rear ring round the
differential.

The earlier plan (docs/chassis-lfs-plan.md) wrote the team's SolidWorks CAD
off as unreadable: "no Parasolid, STEP or zlib stream in them". That was
wrong in one respect. A SolidWorks 2015+ file is a container of RAW-deflate
streams (no zlib header), and inside the
frame part two of them are Parasolid transmit files —
the solid model itself — plus a 1420x806 preview render of the frame.

## The shape of the fix

1. **An extractor**, `generators/chassis/sldprt_frame.py`: SLDPRT container
   -> raw-deflate streams -> the Parasolid partition (itself zlib-wrapped)
   -> every CYLINDER and TORUS record (big-endian doubles in the schema's
   order: centre, axis, radius, reference). A tube is an outer cylinder with
   an inner one on the same axis, so each straight member comes out with its
   axis, its OD and its WALL; each bend comes out with its radius. A
   member's EXTENT is read from the points Parasolid samples along the curves
   where tubes meet (points on this tube's surface), discarding isolated
   clusters of three or fewer, which are stray numbers that merely parse as
   points.
2. **A node graph, not coordinates.** Where two members' axes pass within
   the sum of their radii, that is a joint; joints within a few mm are one
   node. Nodes are then made exactly mirror-symmetric (the frame is, to
   8-30 mm in the raw read). Each straight member is a pair of nodes. Each
   hoop is ONE bent member through its corner points (the intersections of
   its straight legs), with the CAD's own bend radius.
3. **The data is committed**, `generators/chassis/fs_chassis_frame.json`, written
   by the extractor with its source file named, so the generator does not
   need the team's CAD to run and the numbers can be diffed when the CAD
   changes.
4. **The generator reads it.** `chassis_spec.py` keeps the shop values (the
   bender's formers, stock length, weld gap, panel and tab gauges) and
   derives the stations the jig, panels and tabs use from the node table
   instead of assuming them. `members()` becomes: every member from the
   table, with its RANK computed from the topology — a member a node sits in
   the MIDDLE of is tacked before one that ends there, which is the rule the
   old generator applied by hand — so the existing Frame (copes, mitres,
   drawn-long ends) builds it unchanged.
5. **Sizes are the CAD's**, read from the outer and inner radii: 30x2 for
   the hoops (checked in the spec against FSG 2019's alternative-tubing rule)
   and the imperial chromoly the rest is drawn in.

## Files

`generators/chassis/sldprt_frame.py` (new), `fs_chassis_frame.json` (new),
`chassis_spec.py`, `chassis.py`, `chassis_jig.py`, `chassis_panels.py`,
`generators/README.md`, and a write-up replacing the assumed-number caveats
in docs/chassis-lfs-plan.md.

## How it is verified

- The extractor reports what it found and refuses what does not add up: a
  member with no mirror twin, a hoop whose legs do not meet, a joint only one
  member reaches.
- The built frame: every member builds, every cope cuts metal,
  `check_geometry` all valid, `check_frame` (no unattached ends, no joints on
  bends), `check_tube` against the shop's formers.
- Its mass and member count (the CAD's own mass property is not among the
  streams read, so there is no second number to hold it against).
- Renders from the preview's own viewpoint beside the CAD preview, and from
  the photographs' viewpoints beside the photographs.
- `--shop` still writes the whole order (members, cut list, bend schedule,
  jig, panels, tabs).

## What it came to

Read in about 9 s: 99 straight tube runs, 8 bends (two hoops), 4 short
sleeves set aside, 50 nodes, 89 straight members and 2 bent ones, mirror
symmetric to within 8-30 mm before snapping. Built by the generator in about
30 s: 91 members, 86 coped or mitred, 0 invalid bodies, 0 tube violations,
26.9 kg, seven 6 m sticks. The CAD's weldment profiles are imperial chromoly
rounded to 0.01 mm (28.57 x 1.47 is 1-1/8" x 0.058"), mapped in the spec to
their exact sizes — and the app's stock table gained those sizes, which it
lacked (it had 1" x 0.049" only as a rounded 25.4 x 1.2).

`check_frame` reports two things about the REAL car rather than about the
read: ten engine-bay diagonals land on the main hoop's knee bend, and eleven
bays are not triangulated (panels close several of them). Both are left as
the CAD has them.

Not read: the feature tree (no names, no 3D sketch), the tabs and brackets
(separate parts in the assembly), and the four short sleeves, which are
bosses rather than members.
