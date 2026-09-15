# A roadster bicycle, from the owner's photographs

Build the owner's bicycle — a green Scandinavian gents' roadster — as a
parametric assembly that moves: the wheels turn, the bars steer, and the
cranks drive the rear wheel through a chain relation.

## Why this part

Everything modelled here so far is either a machine with a datasheet (the
watch, the engine, the drone) or a part somebody else had already drawn (the
speaker). A bicycle from three snapshots is neither. It is the case a user
actually arrives with: **no dimensions at all, only pictures**, and the
question is whether the API lets an agent turn that into a document whose
numbers are all visible, all editable, and all honest about where they came
from.

It also exercises three things nothing else here does at once:

- **Tubes that meet at arbitrary angles.** A frame is seven tubes mitred into
  five joints, none of them square to an axis. Everything built so far is
  prismatic or turned.
- **A closed drivetrain loop with two ratios.** Cranks → chainring → chain →
  cog → hub → wheel. `asm.chain` exists and one generator uses it; nothing
  has yet driven a *road speed* out of a crank rate through it.
- **Spoked wheels.** 36 spokes laced three-cross, twice, on a rim that is a
  revolve. The interesting question is what that costs to regenerate, because
  a naive answer is 72 bodies of thin cylinder.

## Where the dimensions come from

Only what the photographs show is a fact. **Everything dimensional is an
assumption**, so every one of them is a declared project variable with the
assumption recorded beside it, in the shape `cncest6090` established: the
model is correct-looking on day one and correctable in one edit on day two.

What the photographs actually establish, and that the build must honour:

| read off the photographs | where it lands |
|---|---|
| gents' diamond frame, horizontal top tube, lugged | `frame.py` |
| 28" (622) wheels, chrome rims, 36 spokes, black tyres | `wheels.py` |
| **3-speed trigger shifter on the right bar** → internal-gear rear hub | `wheels.py`, `drivetrain.py` |
| single chainring, single cog, full teardrop alloy chainguard | `drivetrain.py` |
| full-length alloy fenders front and rear, tubular rear rack | `fittings.py` |
| swept-back north-road bars, white grips, one brake lever, bell | `cockpit.py` |
| rubber-block pedals, chrome seatpost, black saddle | `drivetrain.py`, `cockpit.py` |
| green frame, tricolour band on the seat tube, chrome fenders | materials |

The frame size is the one number worth a second look: the photographs show a
tall frame (the saddle sits well above the bars), so the seat tube starts at
**580 mm** rather than the 540 of a typical stock roadster. It is a variable
like the rest.

## Parts

Twenty parts, each its own document, placed by a mate and never by a
coordinate:

- **Frame set** — frame (7 tubes + BB shell + dropouts), fork (crown, 2
  blades, steerer, dropouts).
- **Wheels** — one part each, in three bodies: the rim (alloy), the hub with
  its 36 spokes (steel), and the tyre (rubber). Three because they are three
  materials, and the mass of a bicycle is mostly which parts are steel.
- **Drivetrain** — crank arms (left, right), chainring, pedal, chain link,
  rear cog, chainguard.
- **Cockpit** — handlebar, stem, grip, brake lever, saddle, seatpost, bell.
- **Fittings** — front fender, rear fender, fender stay, rear rack,
  reflector.

## Structure

`generators/bike/`, the layout the README prescribes for a group:

| file | what |
|---|---|
| `bike_spec.py` | every dimension, declared once at project scope, with its assumption; refuses a geometry that cannot close (negative trail, a bottom bracket below the axles, a crank that strikes the ground, a tyre through the seat tube) |
| `bike_shop.py` | the half-dozen moves a bicycle is made of: tubes between two world points, partial revolves, sprocket and tangent-hull outlines, and `Weldment` |
| `bike_frame.py` | frame and fork, welded SOLID and then bored |
| `bike_wheels.py` | rim, tyre, hub, spokes, and the two laced wheels |
| `bike_drive.py` | cranks, chainring, cog, chain, pedals, chainguard |
| `bike_cockpit.py` | bars, stem, grips, lever, saddle, post, bell |
| `bike_fittings.py` | mudguards, stays, carrier, reflector, front brake |
| `bike_verify.py` | what the run refuses to save without |
| `bike.py` | the run: the parts, the assembly and its joints, the checks, the exports |

## How it is verified

The run refuses to save what it has not checked:

- **`asm.check()`** — nothing clashing, nothing floating, no near miss under a
  millimetre, with the contacts that are meant to happen in its `allow` map
  (tyre on fender is not a contact; chain on cog is).
- **Geometry that has to be right or the bike is not a bike**: wheelbase
  within a millimetre of the spec, both axles at the same height, bottom
  bracket drop positive, the crank circle clearing the chainstay and the
  ground, the chainline straight from chainring to cog within 1 mm, the
  fender concentric with its wheel.
- **Trail is positive** — the one number that decides whether a bicycle
  steers at all, computed from head angle, rake and wheel radius, and
  asserted, not assumed.
- **A motion study** that turns the cranks one revolution and checks the rear
  wheel turned `chainring/cog` times through the chain relation, and that the
  bars steer through their lock without the front wheel touching the down
  tube.
- **Renders against the photographs** — the same three-quarter view as each
  photograph, looked at, not just written.

NOT gated in `tests/generators_smoke.py`: the run is four minutes, and three
of them are `check_geometry` on two 36-spoke wheels — far past what belongs in
a 110-second headless suite. It gates itself instead, and refuses to save.

## Not drawn

Brake cables and their routing, the internal gear hub's innards (the shell is
a shell), spoke nipples, the chain's individual side plates (the chain is a
swept loop with the roller pitch on it), decals and the tricolour band as
geometry — they are a material, and the lettering is not measurable from
these photographs.
