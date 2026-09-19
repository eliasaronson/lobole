# The chassis round

A Formula Student space frame is the part this project has never been able to
make: forty-odd tubes, most of them straight, several bent on a former, every
one coped to its neighbours, welded into one frame, and then ORDERED — a cut
list to a supplier, a bend schedule to a bender, a solid per member to a tube
laser. The modelling half exists. The shop half does not.

This round builds the shop half, then builds the chassis with it and fixes
what that finds. Two smaller items come first because they are in the way.

## 1. W6, re-decided

`docs/peer-survey-plan.md` closed W6 — a DFM check — on four pieces of
evidence, three days before the grill round produced exactly the defect class
its first reason said had never occurred. The verdict is re-taken there, in
place. It stays closed, on different grounds, and those grounds are what
shapes stage 3: **a manufacturability check belongs to a declared process,
beside the feature that declares it.** Sheet metal declares material and
gauge, so its check can state a minimum radius; tube bending declares nothing,
so its one rule is a hard-coded literal.

## 2. The bore with no name

`docs/grill-friction-plan.md` finding 15: a bore drilled in one end of a
pipe-and-plug body had no cylindrical face in `list_elements`, while the other
end — same feature, same call — did. A face with no name cannot be threaded,
picked, filleted or fitted, and the listing looks complete. Only the COUNT
shipped (`unnamed_faces`); the defect itself was never reduced.

It is first because every stage after this one leans on naming: a cope picks a
face, a bracket sits on one, and a chassis has hundreds.

Reduce it from the handle's construction (a `Pipe`, two `Extrude` unions, one
`Extrude` cut) to the smallest case that reproduces, and fix what drops the
name — without inventing one that looks stable and is not, which is the
substitution this project refuses.

What the reduction found: the handle's own case is GONE. Its centreline was a
29-point `fit_spline` when finding 15 was written and is a `bent_path` now,
and the part reports `unnamed_faces: 0`. But the defect class is alive and
much larger than one bore — a probe across the constructions a frame uses
found that **every patterned or mirrored body is entirely unnamed**, which
`docs/modeling/toponaming.md` had recorded as deliberate scope. On the grill
that is 105 faces: every bar of the cooking grate and the warming rack, three
of three faces each. So the fix is to name pattern instances, with the
suffix convention patterned CUTS have used since R14.

Verified by: two `[toponaming]` cases (a linear pattern's instances each fully
named and mutually distinct, a mirror's reflection suffixed `#m`), and the
whole grill audited — 80 bodies, not one unnamed face.

## 3. The tube system

Its own plan: [tube/plan.md](tube/plan.md). In short — a frame's members as
first-class things that survive into a cut list, a declared bender the way
sheet metal declares a brake, and files that leave the building.

## 4. The chassis

`generators/chassis/` — a Formula Student frame to the 2024 rules' shape:
front and main hoops bent from one tube each, bulkhead, side impact structure,
front and rear bays triangulated, suspension brackets, floor and firewall
panels. Built with the stage-3 system, not around it.

It is the acceptance test for stage 3 and the friction round for everything
else: every place the chassis needs a move the app cannot make, or makes
awkwardly, is a finding — recorded as it happens, fixed if it is small, and
written down if it is not.

Verified by: the frame builds clean (`check_geometry`, one connected
component, no clashes at body scope beyond the welds), and the order it
produces is complete — every member in the cut list exactly once, with a
length that matches the metal, and a bend schedule a bender could run.

## As built

All four stages landed; the commits are on `wtchassis`.

**1. W6, re-decided** in `docs/peer-survey-plan.md`. It stays closed, on
different grounds: two of its four reasons are now false (the grill DID
produce DFM-shaped defects, and sheet metal does measure what matters in its
place), but they were found BY a check of that kind rather than missed for
want of one. What replaces it is the rule the tube system was then built on —
a manufacturability check belongs to a declared process, beside the feature
that declares it, and a check that has to GUESS the process is the one not
worth building.

**2. The bore with no name** did not reproduce: the handle's centreline was a
29-point spline when finding 15 was written and is a `bent_path` now, and the
part reports `unnamed_faces: 0`. The defect CLASS was alive and larger — a
probe found that every patterned or mirrored body was entirely unnamed, 105
faces on the grill alone, recorded in the toponaming memo as deliberate
scope. Pattern instances now carry the seed's names with an instance suffix,
the convention patterned CUTS had used since R14. The grill audits clean: 80
bodies, 1145 named faces, none unnamed.

**3. The tube system** — [tube/plan.md](tube/plan.md), format v119 — and its
own "as built" section, which is where the measurement bugs are recorded.

**4. The chassis** — `generators/chassis/`. 32 members, 19 coped, built in
2.1 s; one connected component of all 32, no invalid bodies, `check_tube`
clean against a bender with three formers; 18 cut-list rows for 32 members,
10 bends, seven six-metre sticks, 27.4 kg.

### What the chassis found that the tests could not

Every one of these needed a real frame rather than a four-member fixture, and
each is fixed:

* The stock is per MEMBER. Copying sheet metal's per-document size meant 22 of
  32 members were ordered in the wrong tube.
* Stock was packed across sizes, buying a 19.05 member off a 25.4 stick.
* A cut list grouped by exact equality, so a mirrored pair that differed by
  microns was two rows of one instead of one row of two.
* The cut list named the CENTRELINE ("Lower rail L CL"), not the member.
* A cope that cut nothing blamed the wrong cause: at an equal-diameter joint
  drawn to its node the surfaces are tangent, and the members do meet.
* `contact_report` took 84.5 s on 32 bodies, over the client's default
  patience. An oriented-box prefilter makes it 68.7 s; the rest is a
  performance round of its own.

### Still open

* A weld is not a thing in the model. A cope now says two members MEET, which
  is what a weld record would hang on ([manufacturing-gaps.md](manufacturing-gaps.md)).
* No sheet panels on this chassis yet — the floor and firewall of the real
  car are the obvious next piece, and they would put the sheet system and the
  tube system in one part.
* No suspension brackets, and no tabs: the laser-cut plates welded to the
  tubes in every photograph of a real frame.
* Nothing has been cut. The order this produces has never met a saw
  ([roadmap.md](roadmap.md): "Nothing has cut a real part yet").
