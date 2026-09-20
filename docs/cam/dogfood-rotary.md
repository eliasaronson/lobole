# Rotary dogfood — the first continuous 4-axis job

`generators/rotary/handle.py` is a file handle in oak: Ø36 at the swell,
130 mm long, with five dimples spiralling down it at 43° apart. It is chosen
because **it cannot be made on a lathe** — the radius varies with θ as well as
with x — which is the only honest way to exercise an axis whose whole purpose
is to present the work at an angle.

`handle_job.py` cuts it from a Ø40 dowel with one 6 mm two-flute ball:

```
  part         Phi 36 x 130 mm from a Phi 40 dowel
  cutting      42.1 m, 28 min at feed
  program      25013 lines, 513 kB
  checked as   ROTARY r(x, theta)
  max gouge    56 um
  max leftover 1.04 mm over 201.8 mm2
  rapids in    0
  holder       0 collisions
```

56 µm of gouge through the real post and the real verifier, on a part whose
surface is nowhere flat. That is the number the round was for.

## What it found

Four things, three of them fixed in the same round because they were wrong
rather than missing.

### 1. The machine warning measured the part with the wrong bounding box

> *the parts need **310 mm in Z** and 6090 travels 150*

On a handle 38 mm across. `PlacedPartsBounds` took OCC's default bounding
box, which bounds a B-spline by its CONTROL POINTS, while `StockBoundsFramed`
had been switched to the tight bound when a 200 mm waveguide asked for a
216 mm blank. So the same part answered two different sizes depending on
which check asked — and the loose one is the check that tells an operator
their job does not fit the machine. **A warning that wrong is a warning
nobody reads.** Fixed: tight, like the stock.

### 2. The reach check read a rotary pass's Z range as a cut depth

> *T1 cuts **38.3 mm deep** and has 25.0 mm of flute, so the last 13.3 mm is
> cut by the shank*

The deepest radial cut in this job is about four millimetres. A rotary op's
`top_z`/`bottom_z` span the blank's DIAMETER — they are filled in so the panel
has something honest to show and the strategy never reads them — and treating
that as a plunge condemns a correct job in the same sentence the check uses
for a real one.

Skipped for `surface_rotary` rather than approximated. What a rotary pass
actually asks of a cutter's length is how far it must reach past the chuck to
the far end of the part, and that is a fixturing question this check does not
have the fixture to answer. Saying nothing is honest; saying 38.3 mm is not.

### 3. "0.0 mm2 in 0 places", beside a millimetre of leftover

`CompareRotary` measured the depth of the leftover and not its extent, while
`Explain` printed both unconditionally. The area is now summed cell by cell —
a rotary cell's area is its axial size by its ARC at the radius it sits at,
so it is not a count times a constant — and the "in N places" clause appears
only when regions were actually grouped. **Say what was counted.**

### 4. A second run built a second part into the first job

The waveguide script claimed a guard for this and did not have one: it picked
the first PART tab and rebuilt the model into it, which renamed the elements
the first job's operations pointed at. Nothing said so — the operations simply
produced no toolpath, and the report printed only the post's warnings, so it
looked like a clean run. This script refuses by name instead.

## What it confirmed, and what it cannot do

**The end faces are leftover, and they always will be.** The 1.04 mm over
201.8 mm² is the butt and the ferrule face: a rotary pass cuts radially, so a
face perpendicular to its own axis is the one thing it can never reach. A real
job parts those off or faces them in a second, 3-axis setup. The verdict is
right to report it and a reader should know what it is.

**The chip load is under the band and cannot be brought into it.**

> *T1 cuts hardwood at 0.062 mm/tooth, under 0.09-0.21 … it would need
> 2160 mm/min at 12000 rpm and the machine tops out at 1500*

The same wall the waveguide hit: the 6090's feed ceiling, stated. The finding
names one flute as what would fit inside it, which is a real answer for a
finishing pass in wood.

## Still missing, in the order the job wanted them

1. **There is no rotary ROUGHING strategy.** This job is four passes of the
   same finishing strategy at `stock_to_leave` 4.0, 2.0, 0.8 and 0, because
   that is the only way to take a Ø40 dowel down to a Ø26 waist with what
   exists. It is not roughing: a constant-engagement clearing pass keeps the
   chip load steady, and offsetting a finishing pass keeps only the depth
   steady. It is also most of the 28 minutes.
2. **The feed is not compensated** (D2's generator half). Every pass here cuts
   at a different radius and they all carry the same F.
3. **There is no picture.** Playback draws a height map, so this job can be
   verified and not watched. Of everything on this list that is the one a
   person notices first.

   Asked directly, and the answer was worse than "no": the simulation ran it.
   It carved the block as though the part stood still and reported **5,004
   mm³ removed** against about 77,000 actually cut — a precise, confident,
   wrong number, and a picture to match. That is the one outright bug this
   round's survey found in another project (blendercam leaves its 2-D map
   reachable from a 4-axis path with no guard), arrived at from the inside.
   It refuses now, in `BuildTimeline` because that is the funnel every caller
   goes through, so the panel and the RPC inherit it rather than each
   remembering. An INDEXED file still simulates: one constant A is a part
   standing still, which is exactly what a height map represents.
4. **No origin mode means "the rotary centreline".** `stock_centre` happens to
   give it, because the blank is symmetric about the axis in Y and Z — true
   rather than designed, and it would not be true of a part held off-centre.

## Round two: a barley-twist rolling pin

`generators/rotary/rolling_pin.py` — six helical flutes down a Ø50 × 300 pin,
one 45° turn every 157 mm, from a Ø54 dowel. Chosen for the same reason the
handle was and more sharply: **a lathe spins the work against a still tool,
so every shape it can cut is a body of revolution, and a twist is precisely
not one.**

```
  cutting      113.9 m, 76 min at feed
  program      172807 lines, 3628 kB
  checked as   ROTARY r(x, theta)
  max gouge    33 um
  max leftover 2.45 mm over 350.2 mm2   (the end faces again)
  rapids in    0        holder 0        ok True
```

Two passes, not the handle's four: a Ø54 dowel is two millimetres of radius
above the pin's surface and the flutes are four more, so nothing asks a
finishing pass to take a big bite. That is what makes this the better first
thing to actually cut.

### What the MODELLING found

The CAM side behaved. The kernel and the feature set did not, and all three
are worth knowing before anyone models a twist.

**A cut that consumes every body said nothing.** `BodyMode.cpp` drops bodies
a cut fully consumes — right for one body of several — and then reports
nothing when there is nothing left. Twice the twelve-groove chain came back
with a part of 1 cm³, or none, and the next thing anyone sees is "no bodies"
from `mass_properties` with no clue which feature did it. Both times it cost
a hand bisection. It warns now: *"this cut consumed EVERY body — nothing is
left of the part."* Gated in `remote_smoke.py`, next to the no-op cut warning
that was already there — the two are the same question from opposite ends.

**A diamond lattice is not buildable here, and the reason is geometric.** Six
right-handed flutes and six left is prettier, and it asks the kernel for
thirty-six crossings where two B-spline pipes meet the cylinder
*tangentially* — a pipe whose centreline lies on the surface has its equator
IN that surface. It came back `self-intersecting wire, unorientable shape`.
Lifting the grooves clear to make the crossings transversal traded that for a
part with 0.2 cm³ left in it. A single hand has no crossings, is valid first
time, and is the shape everybody recognises as turned-and-twisted anyway.

**`Pattern` cannot repeat a `Pipe` that cuts**, and the refusal names why: a
cut consumes its tool body, so there is nothing left to repeat. Extrude is
special-cased for this (`an Extrude that cuts or adds patterns its tool
instead`); Pipe is not. So the six flutes are written out, each phased by
starting its helix a sixth of a pitch further back — a helix takes an origin
and no phase, so an axial shift is the only lever there is.

One more, unfixed and recorded rather than guessed at: **intersecting the
latticed cylinder with a domed revolve returned BOTH bodies with their
volumes added** — a boolean that did nothing and said nothing. It has not
been chased down; the part is shaped around it (cylinder, then flutes, then
the ends revolved away as waste).
