# SO-101 PG-52 wrist gripper

A parallel-jaw gripper with a wrist camera for the SO-ARM100/101 follower
arm: eight printed parts (chassis, left and right drive plates, coupler
plates, two fingers, jaw pad, camera pod), a 1:1 involute sector-gear pair
coupling two parallelogram fingers so the pads close along X and meet at
X = 0 (52 mm fully open), a 32 × 32 UVC camera module aimed at the grasp
point, and the STS3215 servo imported from the SO-ARM100 repository's own
STEP.

## Running it

In order, against one instance:

```sh
python3 generators/so101_gripper/so101_gripper.py --port N            # every part, and the servo import
python3 generators/so101_gripper/so101_gripper_assembly.py --port N --create [--theta 0]
python3 generators/so101_gripper/so101_gripper_motion.py --port N [--samples 49]
python3 generators/so101_gripper/so101_gripper_export.py --port N
```

- The parts script declares the mechanism's governing numbers as project
  variables (drive pivot offset and height, link length, parallelogram base,
  maximum opening, plate thickness) and imports the servo from the path in
  `SERVO_STEP`, which must point at `STEP/SO100/STS3215_03a.step` from a
  local clone of the SO-ARM100 repository. The import is left centred (the
  default): every servo dimension in the spec was measured off that import.
- `so101_gripper_assembly.py` places the parts at a link angle `--theta`
  (from +Z, positive closing); re-running with a new angle moves the existing
  occurrences.
- `so101_gripper_motion.py` gives every pivot a revolute between named datum
  frames, couples the two drive pivots with a GEAR relation (not a slider,
  which has a second solution with both fingers sliding the same way),
  drives one joint through the stroke in a motion study and checks every
  pair at every sample.
- `so101_gripper_export.py` writes STLs to print (with quantities), STEPs
  and renders into `scratch/so101-pg52-gripper/`.

## Where the numbers come from

`so101_gripper_spec.py` holds them in the TOOL frame: Z the wrist-roll axis
(+Z away from the arm), Z = 0 the wrist-roll servo's horn top face; X the
closing direction; Y the gripper servo's shaft axis. Every interface number
was measured off the SO-ARM100 repository's STEP files (the STS3215 servo and
the SO-101 assembly), not a datasheet: the servo case, horn face and its
9.9 mm M3 square, the connectors on its back.

The arm's wrist bracket reaches past the horn face into the tool's space —
up to 4.73 mm at radii 12–25 mm, measured off the SO-101 assembly posed by
its URDF — and the roll joint turns ±160°, so every sector of that ring
passes under the tool. Only a hub no wider than the horn comes down to the
mounting face; everything else starts 6 mm above it (`MOUNT["stack_z"]`).
