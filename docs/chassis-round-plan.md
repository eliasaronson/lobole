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
