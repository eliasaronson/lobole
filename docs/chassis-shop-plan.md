# The chassis a shop could build

The frame is 52 members that come out of the app as 52 laser-ready solids
with their ends coped and mitred, a cut list and a bend schedule. What it is
not yet is a CAR that somebody could weld: there is no floor, no firewall, no
suspension pickup, no engine mount, and — the piece this round is named for —
nothing to hold the tubes while they are tacked.

A weld jig is what makes a space frame repeatable. It is a flat table, rails
bolted to it, and a plate at every station notched so a tube drops into it
and cannot move. Every one of those plates is a laser-cut blank, and a real
team sends them out in the same order as the tubes.

Nothing new is strictly required to build one, which a spike confirmed:
sheet rules, a `BaseFlange`, a boolean against the tube, `export_dxf` —
187,200 mm³ of plate became 185,558 with a 26.4 mm notch through it, and the
DXF came out as four lines and a circle. What is missing is everything that
makes it a workflow rather than an afternoon of Python.

## What the shop gets at the end

One directory, and nothing else needed to make the car:

```
members/*.step        52 solids, ends coped and mitred, named per member
cut_list.csv          what to buy and cut, per size, packed into stock
bend_schedule.csv     what to set the bender to, per bend
panels/*.dxf          floor, firewall, side panels — flat, with bend lines
tabs/*.dxf            suspension pickups and engine mounts
jig/*.dxf             every plate of the fixture, notched and labelled
manifest.csv          every file, what it is, and what it is made of
```

## S1 — a DXF can carry a MARK

Fifty-two tubes and thirty flat blanks arrive at a welding table looking
alike. A laser marks parts as it cuts them, and it is told to by a TEXT
entity on its own layer — which R12 DXF has and this writer does not emit.

`export_dxf` gains `mark`: a string written as a `TEXT` entity on a `MARK`
layer, placed inside the blank's outline. Deliberately an EXPORT parameter
and not a feature: a `Mark` feature would be a new type, a format bump and a
golden, to carry a string whose only consumer is this writer — and the
string a generator wants is the part's own name, which it already holds.

That is also the limit worth stating: it marks a FLAT blank, because a DXF is
what a flat blank is cut from. A tube arrives as a STEP and carries its name
in the file name; etching it needs glyph outlines the build has no font for,
and that is not this round.

Files: `src/core/io/ExportDxf.{h,cpp}`, `src/remote/RemoteSheetMetal.cpp`.

## S2 — `export_members`

One STEP per member is what a tube laser takes, and it already works: 52
files in 0.4 s. It is 52 calls, which is fine from Python and clumsy from
anywhere else, and each file is named by the caller rather than by the part.

`export_members {dir}` writes them in one call, each named for its member,
and returns the manifest rows. The name matters more than the convenience: a
STEP carries no part name inside it, so the file name IS the identity, and
one place deciding it is one spelling.

Files: `src/remote/RemoteTube.cpp`.

## S3 — the car the frame is missing

* **Panels**: floor, firewall, and the side-impact panels the rules ask for —
  sheet parts, flat-patterned, cut from a DXF.
* **Tabs**: suspension pickups and engine mounts. A tab is a flat plate
  SADDLED to its tube: the same boolean the jig plate uses, with the tube as
  the cutter, so the tab sits down on the tube instead of bridging it.

Files: `generators/chassis/chassis_panels.py` (a tab is a panel that is
saddled, so they share the blank).

## S4 — the jig

A base of rails on the table, and a plate at each station: front bulkhead,
nose ring, front hoop, main hoop, and the rear. Each plate is notched where
a tube crosses it, from its top edge so the tube drops IN rather than
threading through, and each carries its own mark.

Which members a plate holds is derived, not listed: a plate seats the tubes
that actually CROSS its station plane, and only those within a band of the
lowest, so the plate is low and the seats are shallow.

Files: `generators/chassis/chassis_jig.py`.

## S5 — the order

`generators/chassis/chassis.py --shop <dir>` writes the directory above and
the manifest. The manifest is the deliverable that makes the rest usable: a
row per file, what it is, the material and size, and how many.

## Verified by

* Every member exports, and every file name is unique.
* Every flat blank's DXF carries its mark, and re-reading the DXF finds it.
* The jig: each plate is notched only where a member actually CROSSES it —
  the seats are computed from the member's own metal polyline, and a member
  running along the plate is refused rather than cut out of it. The exact
  contact sweep is not run: it is 60+ bodies and minutes of fuzzy booleans,
  and it would answer a question the construction already answers.
* The chassis still passes what it passed before — `check_geometry`,
  `check_tube`, and `check_frame` with no unattached end and no joint on a
  bend.

## As built — the three things the geometry decided

**A jig plate is LOW, and its notches are shallow.** The first plates were
cut to the full height of the tubes that crossed them, so a station plate
stood 1053 mm tall with 1053 mm slots in it and the tubes threaded through
fingers of plate rather than dropping into seats. A plate holds only the
members within 220 mm of the lowest one it crosses, which takes the main
hoop station from 645 x 1053 to 645 x 143 — a plate you can bolt to a table
and lift a tube out of. It also has to REFUSE a member that runs along it:
a tube lying in the station's plane is not crossing it, and cutting its full
length out sawed the main hoop plate nearly in half. The test is
transversality — at least a quarter of the member's direction must be across
the plate — and the two hoop stations are offset 60 mm off the hoop so the
hoop itself is not the thing being notched.

**A tab's seat does not appear in its flat pattern, and that is correct.** A
suspension pickup stands in a plane THROUGH its tube's axis, so its seat is a
cylinder cut across the 4 mm thickness rather than a scallop in the outline.
The blank is cut to the far side of the seat — 12.264 mm off the axis of a
25.4 mm tube rather than 12.9 — so it never fouls the tube, and the 0.64 mm
crescent at the near face is the weld's root gap. A plate ACROSS a tube is
the other case and does scallop.

**R12 has no ELLIPSE, so an angled seat tessellates.** A tube crossing a jig
plate square leaves a true `CIRCLE`; a tube crossing at an angle leaves an
ellipse, which R12 (1987) cannot name — DXF gained `ELLIPSE` in R14. The
seats are built from `Cylinder` primitives and the solid IS analytic (26
planes and 14 cylinders on one plate); it is the WRITER's dialect that
cannot say so, and the flat pattern comes out as line segments. A laser cuts
them fine. It is a limit of the format the exporter targets, not of the
model.

### What it produced

66 files, 8.9 MB: 52 member STEPs, 11 DXFs (2 panels, 4 tabs, 5 jig plates),
the cut list, the bend schedule and a 65-row manifest. The frame is
unchanged — 52 members, 0 invalid bodies, 35.7 kg, no unattached end and no
joint on a bend, 12 untriangulated bays, most of them faces a panel closes.
`--save` writes the project so the frame, the panels, the tabs and the jig
can be opened and looked at.
