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

Files: `generators/chassis/chassis_panels.py`, `chassis_tabs.py`.

## S4 — the jig

A base of rails on the table, and a plate at each station: front bulkhead,
nose ring, front hoop, main hoop, and the rear. Each plate is notched where
a tube crosses it, from its top edge so the tube drops IN rather than
threading through, and each carries its own mark.

It is checked the way the frame is: every plate must actually touch the
members it holds (`contact_report`), and no plate may foul a member it does
not hold.

Files: `generators/chassis/chassis_jig.py`.

## S5 — the order

`generators/chassis/chassis.py --shop <dir>` writes the directory above and
the manifest. The manifest is the deliverable that makes the rest usable: a
row per file, what it is, the material and size, and how many.

## Verified by

* Every member exports, and every file name is unique.
* Every flat blank's DXF carries its mark, and re-reading the DXF finds it.
* The jig: each plate touches the members it is there for, and contacts
  nothing else.
* The chassis still passes what it passed before — `check_geometry`,
  `check_tube`, and `check_frame` with no unattached end and no joint on a
  bend.
