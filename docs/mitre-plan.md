# The cut a corner joint needs

A Formula Student chassis built with `Cope` came out with **27 of its 47
members square-cut at one end or both**. Not a bug in coping: those ends are
at CORNER joints, where two tubes both stop at the node and neither runs past
the other, so there is no round side to fishmouth against. A corner wants a
MITRE — both tubes cut on the plane that bisects them — and this build has
no mitre, so it left the ends square and said nothing.

A square end at a corner is a tube that does not sit down on its neighbour. A
welder can grind one to fit; a tube laser cannot, and the cut list is silent
about which ends need it.

## M1 — `Cope` gains `mitre`, and members are drawn LONG

Two things had to change together, and the first attempt found out why.

**A mitre needs material past the cut.** A tube whose axis stops at the node
and is then cut on the bisector does not come out mitred: the plane only
shaves a wedge off the end, leaving part of the original square face behind.
Measured on a 90° corner — 2 cylinders and THREE planes on a member that
should have two. So members are drawn LONG, a diameter past each node, and
the cut trims them back. That is how a frame is built: tubes are cut
oversize and trimmed to fit.

**Which cut a joint wants is topology, not geometry.** Once members overrun
their nodes, nothing in the geometry distinguishes a corner from a tee — both
look like two tubes crossing. The frame knows: at a corner both members END
at the node, and at a tee one runs through. So `Cope` is TOLD rather than
left to guess:

```
Cope {member_id, against: [...], mitre: [...], gap?}
```

* `against` — a neighbour whose side this member lands on: FISHMOUTH, by
  subtracting its outer solid, as today.
* `mitre` — a neighbour this member meets END TO END: cut on the plane
  through the joint whose normal is `u - v`, the unit vectors along the two
  members away from it. Two tubes at 90° are each cut at 45°; two in line are
  cut square, which is what a straight butt is.

The joint is the closest approach of the two CENTRELINES, so it is the node
whether or not either member overruns it.

`gap` opens a mitre the way it opens a cope: the plane moves back along the
member.

Files: `src/core/features/CopeFeature.{h,cpp}`, `src/core/io/SerializeFeatures.cpp`,
`tools/designer_mcp.py`, `tests/test_cope.cpp`. **Format change** — a new
field, so a bump, a golden at that version and a row in the history.

## M2 — the frame says which joint is which

The generator already knows: it holds every member's NODES, so it can ask
whether a neighbour runs past a given end or stops at it — which is exactly
the test it used to decide whether to cope at all. That test now chooses
between `against` and `mitre` instead of between coping and doing nothing,
and the rank rule still decides who cuts whom.

It also sweeps each member along a path a diameter longer at each end than
its nodes, while keeping the nodes for the topology.

Files: `generators/chassis/chassis_frame.py`.

## M3 — the two things the frame is actually missing

Found by looking at it rather than by a check:

* **no bottom tube in the front bulkhead.** It has two uprights, a top and a
  diagonal, and the bottom corners are joined by nothing. It was there before
  the triangulation round and was dropped by accident.
* **the rear bay is thin.** It carries the engine and the rear suspension and
  has one diagonal per side plus a bulkhead diagonal; the real car's is a
  fully triangulated box.

Files: `generators/chassis/chassis.py`.

## Verified by

* A mitred corner has NO planar face left on either tube, and the two cuts
  are coplanar — which is what makes them meet.
* A tee joint still fishmouths: the test that a neighbour running past is
  coped, not mitred, keeps its current shape.
* The chassis: **no member square-cut at a joint**, measured the way the
  fault was found — by counting planar faces per member.
