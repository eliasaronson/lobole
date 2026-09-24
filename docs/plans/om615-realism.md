# OM615: make it look like the engine

## The problem

The OM615 generator builds a complete, clearance-checked, moving engine, but
it reads as boxes: the block, head and timing cover are flat slabs, the sump
is a box, the intake plenum a plain tube, and every part wears the same
polished metal. Against the reference photographs (front, both sides, top,
and the factory cutaway, KTG 9810) what is missing is shape and finish, not
mechanism:

- **Finish.** The real engine is a grey-painted iron block and head, bare cast
  aluminium cam cover, intake and vacuum pump, a dark cast-iron exhaust,
  black rubber hoses and a dark fan.
- **Silhouette.** A long cam cover with full-round ends; an intake plenum that
  is a rounded cast box beside it with the throttle body in the middle and
  four short curved runners; an exhaust manifold that is a cast log merging
  four runners to one outlet flange; a sump with a deep well under the front
  cylinders and a flanged lip; a timing cover that follows the chain case
  (round over the cam sprocket, bulged for the water pump) rather than a
  rectangle; cast triangular engine-mount arms bolted to the block front on
  both sides.
- **Castings.** Block sides with a raised water-jacket panel, core plugs and
  bosses; a head with manifold bosses rather than a flat wall.
- **What hangs on it.** A vertical oil-filter housing on the pump side of the
  block, a fuel filter canister on the pump side of the head, a thermostat
  housing at the front of the head with the upper hose and the heater pipe,
  the lower hose to the water pump, a dipstick tube with its ring handle, and
  a crank pulley that reads as pulley plus vibration damper.

## The shape of the fix

Every moving part, its mates, relations and the numbers the mechanism uses
stay as they are; this changes what the outside looks like and adds the
static parts above. Each change is a builder edit iterated with
`--update-component`, then a full rebuild.

1. **Finish first.** One table of appearances in `om615.py` (a material and
   colour per part family, colours linear), applied to every part: iron
   painted mid grey, aluminium castings satin, exhaust dark oxide, rubber
   black, steel fasteners and pipes, the fan dark.
2. **Top end** (`om615_top.py`): cam cover with full-round ends, a raised
   rim, the three-pointed star boss and the filler cap near the rear;
   intake plenum as a rounded-rectangle box on four curved runners, with a
   throttle body and lever at its middle; exhaust as a cast log with a
   central downward outlet flange and studs; head side bosses; thermostat
   housing on the head front.
3. **Bottom end** (`om615_bottom.py`): block side panel, core plugs and
   bosses; sump with a front well, a shallow rear and a flanged lip;
   timing cover following the chain case; crank pulley with damper ring and
   hub bolts; cast mount arms.
4. **What hangs on it** (`om615_accessories.py`): oil-filter housing, fuel
   filter, upper and lower hoses and heater pipe as swept tubes, dipstick.
   Placed off existing bosses so the mounting checks cover them.

Dimensions come from the bore/stroke-derived spec where they can and are
read from the photographs (scaled against the 87 mm bore and 97 mm bore
pitch) where they cannot; the README says which.

## Files

`generators/om615/om615_top.py`, `om615_bottom.py`, `om615_accessories.py`,
`om615.py` (appearance table, new occurrences, component keys),
`om615_spec.py` (new dimensions), `generators/om615/README.md`.

## Verification

- `--stage full` passes: the exact standing sweep and the motion study over
  two crank revolutions are clean, and strokes, lifts and firing order are
  as before. `om615_accessories_smoke` and `om615_mounting_smoke` pass.
- Renders from the photographs' angles (front, pump side, manifold side,
  top, 3/4), quick and path traced, compared with the photographs side by
  side and looked at.
- Build time reported against the current 90 s preview.
