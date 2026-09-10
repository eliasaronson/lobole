# A four-cylinder diesel, built complete

Current reference corrections and accuracy limits are recorded in
[om615-sources.md](om615-sources.md). The historical account below describes
the earlier model and should not be read as a claim of factory accuracy.
For iteration, `--build-only` saves an explicitly unchecked project, and
`--no-export` skips GLB export while retaining the requested checks. The
standing gate now uses `Asm.check_clashes()` to avoid computing distances.

`generators/om615/om615.py` builds an OM615-shaped engine — 1988 cc, 87 x 83.6,
five main bearings, a single overhead camshaft on a duplex chain working the
valves through rocker arms, and a pre-chamber per cylinder — down to the
bearing shells, the seals, the oil galleries and the water jacket.

    python3 generators/om615/om615.py --port <port> [--samples N] [--mesh]

Three modules: `om615_spec.py` is every shared number, `om615_bottom.py` and
`om615_top.py` build the parts, `om615.py` assembles, drives and checks them.

## What it is

70 parts, 199 occurrences, 202 mates, one degree of freedom. Four
slider-crank loops off one crankshaft; a chain relation halving crank speed
to the camshaft; per valve a cam relation for its lift and a rack relation
that makes its rocker follow. The gate is two crank revolutions with every
occurrence pair checked under EXACT booleans.

Measured, not asserted: the stroke off the piston poses (83.600 against a
design 83.6), the squish at TDC (3.20), each valve's lift (8.0 on its own
lobe), and the firing order read off the exhaust events (1-3-4-2).

## The two app defects it found

**A relation whose A is a free mate could beat the driver to a mate.**
`Solve` propagated relations from every relation's source, discarding the
walk the motion study had just made from the driver. A cam relation drives
the valve's slide; the rack that makes the rocker follow has the ROCKER as
its A. Reached from the rocker instead, the rack wrote the valve from last
sample's rocker angle, and the pair fed back to 60 mm of lift on an 8 mm
cam. `PropagateRelations` now walks the driven chain to completion first and
seeds the rest afterwards, and `Solve` passes the driven mate it already
knows (`AssemblySolve.cpp`; `tests/test_assembly_relations.cpp`, "The DRIVEN
mate's chain settles before any other source").

**A cam profile that did not begin at zero degrees extrapolated backwards.**
`CamAt` located the span by scanning forward from the first point, so an
angle BELOW the table's first angle left the index at 0 and the Hermite
cubic ran backwards out of the interval its endpoints define. A lobe phased
to open at 74 degrees answered 60 mm of lift at 0. The profile is periodic:
below the first angle is inside the span that wraps past the end of the
turn, and that is the span it now uses (`Occurrence.cpp`; same test file, "a
lobe that does not begin at zero is still shut before it").

Both were invisible to the single-cylinder engine, which has one lobe
starting at 20 degrees and no rockers.

## What the model itself got wrong, and how it was caught

Every one of these was found by a check, not by looking:

- **One stale frame, 88 clashes.** The block's `cap_i` frame was `z = -Z,
  x = +Y` on the reasoning that the parting face "points down". A mate
  ALIGNS axes, so every main cap arrived rotated a half turn about (1,1,0):
  120 mm long along the crank, standing above the parting line instead of
  under it, and dragging its shells and thrust washers with it. Mounting
  frames are world-aligned translations now; only what something turns or
  slides on carries an axis.
- **The crankcase voids extruded the wrong way.** `Plane.xz(y)`'s normal is
  -Y, so a cut travels forward; placed at the void's own front face, all
  four landed a bay ahead — the middle one straight through the centre main
  web. Nothing outside the block changed, so the render looked right. The
  thrust recesses cutting nothing is what surfaced it.
- **The bays were open to the sky.** The crankcase is 172 mm wide inside
  (the big end sweeps 83.8 from the axis) and the deck is 140, so a block
  that stepped in at the bays' roof left a 16 mm slot down each ledge. The
  step is above the roof now.
- **The rod was as wide at its small end as at its big one**, so it was
  1.3 cm3 inside the piston at every angle, between two pin bosses that had
  been one boss across the whole pin.
- **The piston dropped out of its bore into a bay narrower than itself.**
  Each bore is carried down through the webs beside it.
- **The oil pump did not fit its own chain case** — its cover inside the
  cover's front wall, its sprocket inside the pump, its gears through the
  case floor, its pickup in the sump's wall and its strainer out through the
  pan's nose. The case is open at the bottom now (a chain case drains into
  the sump), and the pump is derived from the space between the crank's
  sprocket and the pan rail.
- **The belt stood on its pulleys' rims** rather than in the vee, being
  wider at the radius it rides than the groove is.
- **The valve gear took nine rounds of the exact check to clear**, and every
  one of them was a real thing:
  - the cam's lobes cut into the rocker ARMS, because an arm running
    straight to its contact point is nearly tangent to the lobe the whole
    way along. The cam pad stands 18 mm proud now, so the arm's top drops
    away under it and only the pad is ever in reach;
  - the pivot bosses were in the head's own side wall (the shafts moved
    inboard to 60);
  - the cam cover had a 6 mm floor across its whole width, at exactly the
    height of the camshaft and every rocker boss — a cover sits ON its rail,
    so its cavity is open at the bottom, like the chain case's;
  - the cam cover's gasket was UNDER the head's top face, which is inside
    the head, and its rim was over the rocker bosses;
  - the camshaft ran through both end rims of the cover and its gasket;
  - the head was 21 mm too short: the rocker's own stack eats 60 mm between
    the cam and the valve's tip, and the spring retainer ended up below the
    chamber floor and on the stem seal;
  - the spring pocket left a plug of solid metal above each guide, which the
    stems ran through;
  - a spring drawn at its free length is inside its retainer as soon as the
    valve opens — a rigid spring has to be drawn at its SHORTEST;
  - and the last one, three tenths of a millimetre: a rocker's pad slides
    8 mm sideways off the valve as the rocker turns, so at full lift it is
    the ARM'S UNDERSIDE, not the pad, that comes down over the stem.

## Things worth knowing before doing this again

- **The collision mesh's deflection scales with the model.** On this engine
  it is 1.17 mm, so every running fit in it reads as contact and the mesh
  check cannot be the gate — `--mesh` is a fast look, exact is the answer
  (25 samples, ~420 s). On the single-cylinder engine the same number was
  0.39 and mesh was enough.
- **A firing order is not a property of the crank.** Cylinders 1 and 4 share
  a throw and reach TDC together, so the pistons cannot say which is firing.
  It is read off the camshaft: an exhaust lobe peaks a fixed 260 crank
  degrees after its own cylinder's firing TDC. And a valve opens DOWNWARD,
  so its greatest lift is its lowest z — taking the max read a shut valve
  and gave the right cycle started at the wrong cylinder.
- **A head is built in one order and only one.** The water jacket is a void;
  everything that must stay dry needs its wall put back BEFORE it is
  drilled. It is drawn as a ladder — a channel up each side and a column
  between each pair of cylinders — rather than the slab it looks like in
  section, because a slab crosses every port, guide, bolt and plug, and a
  sleeve round an INCLINED port is a disc that pokes out of both faces it
  passes near.
- **Primitives are always new bodies.** Box, Sphere and Cylinder take no
  mode, so a part built from them is a bag of overlapping solids that counts
  its overlaps twice in its own volume. Everything here is a drawn profile;
  `slab`, `disc`, `axis_solid` and `bore` in `om615_bottom.py` are the whole
  vocabulary.
- **Do not pose the assembly by hand before a study.** `set_mate` +
  `solve_assembly` solves without saying what is driven, and a slider-crank
  loop can settle on the mirrored branch — pistons at -103 instead of +187.
  A study started from there inherits it. Every measurement here comes from
  a study on a freshly built document.
- **An occurrence's reported position is its part ORIGIN.** The camshaft's
  origin is 372 mm below its own axis, so it "travels" 745 mm in a study
  that only rotates it. Measure a piston at its crown, not at its pin.

## Round two: the manifolds, the fasteners, and what the user saw

A look at the built engine turned up eight things, and every one of them was
real: the intake manifold was a set of straight pipes that did not meet their
ports, both castings were boxes, there were two holes straight into the
engine, the injectors were buried inside the intake log, and there were no
fasteners and no engine mounts at all — the sump was "just sitting there".

What it is now: 76 parts, **241 occurrences**, 40 real fasteners derived from
the holes they go in, and the same gate — 10491 exact-boolean pair checks over
two crank revolutions, clean.

**Both manifolds are LOGS with curved branches**, swept as pipes along fitted
splines rather than straight cylinders between two flanges: a runner leaves
its port on the port's own axis and arrives at the log on the log's, and
everything between is a curve. Each is built solid and bored afterwards,
because two hollow tubes unioned leave each one's wall across the other's
throat.

### A third app defect, and the two-hour version of it

**`add` on a body the new material does not touch does not union.** It keeps
the connected lump and drops the rest — so four flange pads added before the
log that joins them left one flange and one runner, silently. Everything here
is built log-first now, each step touching what is already there.

Worse, and now fixed: **a fuse that OCC could not do came back valid, smaller,
and silent.** A swept tube whose end cap lands *inside* the block it is being
added to made a union that returned an EMPTY solid — the whole manifold
vanished, with no error on any feature. A union can only add, so
`ApplyBodyMode` now refuses a fuse whose result has less volume than the body
it started from and says which body and how much (`BodyMode.cpp`). The
modelling fix is in the same spirit: a runner starts flush ON the flange face
with a short straight run out of it, so it passes through the pad rather than
ending inside it.

### Fitting forty fasteners taught five things

- **A threaded hole's major diameter IS the bolt's nominal.** Drawn at a
  tapping-drill size, all forty bolts read as steel inside steel for their
  whole engagement — twenty clashes that were nothing but that.
- **A hole that is not one of the standard's own clearances is refused**, and
  rightly: 8.5 is not an M8 clearance (8.4 / 9 / 10 are) and 12.5 is not an
  M12 one. "The bolt plus a half" is not a size.
- **A rim SPLIT into two arcs is not a rim** a fastener can be fitted to, and
  three different things caused one: a cam-cover bolt drilled through the
  outermost head bolt's own boss, a boss that ran past the head's end face,
  and a sump bolt that broke out of the flange's inner edge. All three are
  "something crosses the face this hole seats on".
- **Matching a rim to its own hole floor is a match in WORLD space.** The
  flywheel's bolt circle is in its local XY and the crank's is in its local
  XZ; comparing (x, y) to (x, y) put every flywheel bolt 64 mm off its axis.
- **A bolt circle is bounded on both sides.** The flywheel's has to clear the
  spigot it centres on and stay inside the crank's rear seal land — at 74 mm
  the bolts ran out through the land and into the seal that runs on it.

### The two holes into the engine

- **The top one was the head/cover joint.** The cover's rail, its gasket and
  the head's own 8 mm wall all ended on the same line. The cover OVERHANGS
  the head now — but in x only: lipped in y as well, the gasket ran past the
  notch cut for the cam nose and the camshaft came through 5 mm of gasket at
  every crank angle.
- **The bottom one was the chain case meeting the sump.** The case's floor is
  open on purpose (it drains into the sump, and the oil pump's sprocket has
  to hang through it), so the pan's flange has to run forward underneath it.
  Stopped at the block's front face, the two left a slot into the crankcase.

### Less boxy

The crankcase and the head are drawn as SECTIONS with chamfered outer edges
and extruded along the engine, not as boxes; the sump tapers to a narrower
floor; the cam cover is a rail, a step and a raised roof. A casting has draft
where a box has a corner, and that is most of what makes one look like the
other. It also moved the sump bolts twice: they have to sit outboard of the
pan flange's inner edge and inboard of the block's now-chamfered rail.
