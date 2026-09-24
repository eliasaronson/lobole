# PES pump on the OM615

## The problem

The OM615 and its Bosch PES4M injection pump are two separate generators,
each moving and clash-checked on its own. The engine's block has an
`injection_pump_mount` frame, but nothing is mounted on it, nothing drives
the pump, and no fuel reaches the injectors. Put together as they stand, they
do not fit:

- **The drive is missing.** The timing chain runs crank to camshaft only.
  The pump's 26-tooth sprocket cannot turn at half crank speed from the
  22-tooth crank sprocket.
- **The interface disagrees with itself.** The block's three M8 holes put
  one hole at the top. The pump flange's matching three holes put one at the
  bottom. The flange face has no frame to mate to.
- **At the mount frame, the pump sits inside the engine.** The sprocket and
  flyweights land inside the solid mounting boss, 70 mm behind the chain
  plane. The feed pump is inside the block. The element row overlaps the
  fuel-filter canister and its bracket. The governor overlaps the oil-filter
  housing.
- **Names collide.** Both generators have a part called `camshaft`, and
  `app.part(name)` opens the existing tab. Built into the engine project, the
  pump would add its features to the engine's camshaft.
- **Sub-assemblies are rigid.** The parent's solve does not go into them, so a
  pump placed as a sub-assembly would not move in the engine's study. The
  pump goes into the engine assembly flat.

- **The engine is a mirror image of the real one.** The model's front is -Y
  and its up is +Z, and it puts the pump side at -X, which is the car's RIGHT.
  On the real OM615 the pump, injectors, filters and vacuum pump are on the
  car's LEFT, and the manifolds are on the right. Seen from the pump side, the
  real fan is on your left; in the model it is on your right. The pump model
  is the right way round (it matches the owner's pump), so on the mirrored
  engine its feed pump faces the block instead of facing out.

## The layout, from the photographs

- The pump lies along the pump side of the block, below the injectors. Its
  drive end is in the timing case at the front and the governor points to the
  rear. The feed pump and its hand primer face outward. The delivery-valve
  row is on top.
- The injection lines rise from the four outlets and curve up to the
  injectors. They are held together by clamps.
- The fuel filter is a tall canister on a bracket at the front of the head,
  on the pump side, above the vacuum pump.
- The oil filter is a spin-on canister on a housing below the pump. It hangs
  down and outward at an angle.
- The dipstick rises ahead of the pump. Its ring is at the front, on the pump
  side.

## The shape of the fix

0. **Put the engine the right way round.** Re-lay the OM615 so that its pump
   side is +X, the car's left, with front -Y and up +Z. This is not a
   mirrored copy: threads stay right-handed, and the crank turns clockwise
   seen from the front (from cylinder 1's TDC, the pin moves toward the pump
   side). Every check keeps its numbers: strokes, lifts, and firing order
   1-3-4-2.
1. **The pump looks like the pump.** It gets the Z cover, the clamp plates
   on the delivery valves, the primer wheel, the FP/K feed pump, the red
   filler cap and the governor chamber, as the owner's pump has them. Its
   motion and its interface do not change.
2. **One interface, stated once.** The pump flange (face, register, bolt
   circle and hole clocking) lives in `pes_spec.py`, because it is the pump's
   design and the block is machined to fit it. `om615_spec.py` reads those
   numbers, so the block boss and the pump flange are drawn from the same
   values. Add a `flange` frame on the pump housing. Place the mount where the
   pump's sprocket lies in the chain plane and the feed pump stands clear of
   the block wall. That puts it on the front of the block, beside the chain
   case, as on the real engine. The block boss moves with it.
3. **The pump builds into any assembly.** `pes_pump.assemble` takes the rig
   and a placement. The standalone generator passes its own rig and fixes the
   housing; the engine fastens the housing's `flange` to the block's
   `injection_pump_mount`. The pump's parts are renamed so none collides with
   an engine part. The standalone generator keeps its four studies and still
   passes them.
4. **The drive.** Give the pump sprocket the camshaft sprocket's 44 teeth, and
   add a chain relation crank to pump camshaft at 22:44, like the camshaft's.
   The vacuum pump's stand-in "injection timer" at the timing cover is now
   the real pump's job: drive the vacuum pump's face cam from the pump
   camshaft and delete the stand-in. The control rack is held at full load
   in the engine.
5. **Make room, as the photographs show.** Move the fuel filter to the front
   of the head. Rebuild the oil filter as a housing below the pump with a
   spin-on canister hanging down and outward. Move the dipstick ahead of the
   pump. Take positions from the pump's envelope and the block, not from
   fixed numbers. The mounting checks already cover all three.
6. **Fuel lines.** Add an `outlet` frame on each delivery-valve holder.
   Build four steel injection lines, `bent_tube` Ø6 × 2, each running from
   element k's outlet to cylinder k's injector `fuel_inlet` frame. Pump
   element k feeds cylinder k, so the pump's 1-3-4-2 matches the engine's.
7. **The checks.** The engine's `--stage full` motion study now moves the
   pump, and these become part of the gate:
   - The pump camshaft makes one turn per 720° of crank.
   - Every plunger strokes 7.0 mm.
   - The plungers peak in firing order 1-3-4-2.
   - **Spill timing:** each element's port closing, where plunger lift crosses
     the port height, falls a fixed angle before its own cylinder's
     compression TDC. The angle is the same for all four, within a sample.
     This check is what shows the pump is timed to the engine, not only turning.

## Dogfood

This is a dogfood build, so every point where the app or API gets in the way is
written down as it happens. The ones already known:

- A sub-assembly cannot move with its parent. A relation cannot cross into one.
- `app.part(name)` reuses an existing part without saying so. A second
  generator writing into the same project adds features to a part it did not
  create.
- The pump's studies are built for its own project. The engine's
  `run_engine` takes `studies[0]`.

They are fixed in a round after this one, each with its own plan.

## Files

- `generators/om615/om615.py`, `om615_spec.py`, `om615_bottom.py` (the block
  boss), `om615_accessories.py` (the filters, the vacuum pump drive, the
  injection lines).
- `generators/pes_pump/pes_pump.py`, `pes_parts.py`, `pes_spec.py`.
- Both READMEs. `tests/om615_*_smoke.py` if they build the pump side.

## Verification

- `python3 generators/pes_pump/pes_pump.py` still passes its four studies,
  exact and clean.
- OM615 `--stage full` passes:
  - The standing and motion sweeps are exact and clean with the pump in.
  - Strokes are 83.6 mm, valve lift 8.0 mm, firing order 1-3-4-2.
  - The pump checks above pass.
- `ctest -R om615` passes.
- Renders from the pump side, quick and path traced, with a section through
  one element at port closing.
