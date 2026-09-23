# The LFS19 chassis, recreated

> **Superseded (2026-09-23):** the frame is now READ from the team's CAD, not
> recreated from a photograph — the team's SolidWorks frame part turned out
> to be readable after all (its streams are raw deflate, and two are Parasolid
> models). See [chassis-cad-plan.md](chassis-cad-plan.md). What
> follows is the photograph-era plan, kept for its reasoning about jigs,
> tabs and hoops.

The frame the generator builds now is *a* Formula Student chassis: rectangular
hoops, a boxed rear, a bulkhead and five stations, every bay triangulated
because the rules of a space frame say so. The car Lund Formula Student
actually built is a different shape, and this round replaces the textbook
frame with a recreation of it.

It is a RECREATION, not an import: the frame stays parametric — a node table
and a member list that a generator builds — so that every dimension is a
number somebody can argue with, and the shop bundle keeps working.

## What the evidence is

1. **One photograph of the car on its weld jig.** It carries the TOPOLOGY
   almost completely: which tubes exist, where they meet, how the hoops are
   shaped, where the tabs are, how the jig is built.
2. **The FS rules**, which fix several dimensions absolutely — main hoop
   height above the seat, the cockpit template, the minimum tube sizes for
   each named member — so a member's size is not a guess.
3. **The team's SolidWorks CAD** (`LFS19_Main_Assembly_20`, 959 files), which
   holds the real numbers and which nothing in this toolchain can read: the
   files are SolidWorks' own container, not OLE, with no Parasolid, STEP or
   zlib stream in them. It becomes usable the moment
   `19031101_Frame_19_SES.SLDPRT` is exported as STEP.

Every dimension in the spec is therefore tagged in the source, the way the
bicycle round did it:

* `# READ` — measured off the photograph or the CAD, with what it was
  measured against.
* `# RULE` — fixed by the FS rules, with the rule's own number.
* `# ASSUMED` — a number nobody has yet. It is a variable, it is named, and
  it is the list to walk when the STEP arrives.

## What the photograph shows that the current frame does not

**The hoops are arches, not rectangles.** Both the main hoop and the front
hoop have legs that lean INWARD as they rise and a large-radius arched top —
the main hoop is markedly narrower at the top than at its feet. The current
build bends a hoop's two top corners and runs a straight across, which is a
different part and a different bend schedule.

**The rear is a converging space truss, not a box.** Behind the main hoop the
tubes taper in from both sides and from above to a pair of rear node
clusters, where four or five members and the suspension links meet. The
current build's rear is a rectangular bay with diagonals.

**The main hoop is braced both ways.** A long tube runs rearward and down
from the top of the hoop into the rear truss, and the bracing between the
two braces is itself triangulated.

**Suspension tabs are PAIRS in double shear.** Every pickup in the photo is
two parallel plates straddling a rod end, not the single plate the generator
makes. That is a different part, a different DXF (two of them per pickup) and
a different weld.

**The bulkhead carries perforated mounting strips** — flat bars with a row of
holes, welded to the bulkhead uprights, for the impact attenuator and the
nose. The current bulkhead is bare tube.

**The jig is a bolted ladder with one small plate per tube.** Square tube
rails bolted to the table in a ladder, and on them individual low plates,
each notched for ONE tube, bolted down. The current build makes one
full-width plate per station — which cannot be removed once the frame is
welded around it. A per-tube plate lifts out.

## L1 — the node table

`chassis_spec.py` becomes the LFS car: the stations along the car, the half
widths and heights at each, and the hoop profiles as node lists rather than
as corner radii. A hoop is a bent path through its own nodes, so an arched
hoop is a spec change and not a code change.

## L2 — the members

`chassis.py`'s `members()` is rewritten against the photograph, in the same
rank order the frame is tacked up in. The rear truss and the hoop bracing are
the two parts that change shape completely.

## L3 — the tabs

A pickup becomes a PAIR of plates on a shared bolt axis, spaced for the rod
end that goes between them. `chassis_panels.py` grows the spacing and the
second plate; the bundle gains a DXF per plate.

The bulkhead's perforated strips are the same flat-blank move with a row of
holes.

## L4 — the jig

One plate per TUBE rather than one per station, sized to the tube it holds
and to the ladder it bolts to, plus the ladder itself as a cut list of square
tube. The band rule that made the plates low stays; the transversality test
stays.

## L5 — the bundle

Unchanged in shape: `--shop <dir>` still writes members, panels, tabs, jig
and a manifest. The counts move.

## Verified by

* `check_geometry`: no invalid body.
* `check_tube`: every bend inside the bender's dies, every hoop one piece.
* `check_frame`: no unattached end, no joint on a bend, and FEWER
  untriangulated bays than the frame it replaces — the real car is more
  triangulated than the textbook one, so this number is a check on the
  recreation, not just on the frame.
* The render against the photograph, from the same angle.
