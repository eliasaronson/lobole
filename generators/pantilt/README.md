# pantilt

A geared pan/tilt head for a 1.5 kg camera: a worm pair on each axis, a
machined aluminium base plate with a 3/8-16 tripod thread, everything else
printed in PETG. The plan, and the phases after this one (FEM and topology of
the yoke, the base plate on the 6090, drawings):
[docs/plans/pan-tilt-head.md](../../docs/plans/pan-tilt-head.md).

    tools/headless-preview.sh --start PORT        # a scratch instance
    generators/pantilt/run_all.sh PORT            # everything, ~20 min
                                                  # (the two studies, 18)
    generators/pantilt/run_all.sh PORT --from assembly
    PANTILT_SAMPLES=6 generators/pantilt/run_all.sh PORT --from assembly  # ~4 min

Each stage opens the project the one before saved; all of it lands in
`scratch/pantilt/` (projects, renders, `assembly_report.json`, the BOM, one
log per stage, and `run_all.log`, which ends in `done` or `failed`):

| script | makes | saves |
|---|---|---|
| `build_base.py` | the project variables (`spec.py`), the base plate (aluminium: the lower pan bearing's pocket, the tapped 3/8-16 tripod hole with its Thread, four M4 tapped holes) and the pan wheel (PETG, 40 teeth, hobbed; the upper pan bearing's counterbore, four counterbored screw holes) | `base.dsgn` |
| `build_worms.py` | the worm (one part for both axes: ZA, one start, 0.25 mm backlash, spacer collars and journals), the bearing block (one part for all four bearings) and the knob | `worms.dsgn` |
| `build_tilt.py` | the tilt wheel with its shaft, the cradle (camera plate, 1/4-20 slot), the camera (the load: a box and a lens of 1.5 kg) and the 1/4-20 camera screw (a plain part with a cosmetic thread) | `tilt.dsgn` |
| `build_yoke.py` | the yoke, modelled where it stands, its frames the layout | `yoke.dsgn` |
| `build_assembly.py` | the assembly: mates, bearings by `fit_bearing` and `check_seat`, screws by `fit_fastener`, two worm relations, the standing exact sweep, tolerances and `check_fits`, BOM and masses, pan 360 and tilt +-90 motion studies checked exactly at every sample, clearance assertions verified at both ends of the tilt, renders | `pantilt.dsgn` |

`spec.py` holds the derivation; `common.py` the connection, the print
allowance lookup and the frame helper.

## Sizes

Every size is a project variable, most of them expressions over the load:
1.5 kg, its CoM 80 mm up and 40 mm forward of the tilt axis, sized for 3 g.

- **The worm pair.** Lewis on the wheel's teeth at 3 g against a third of
  PETG's layer strength gives the module (1.95, taken up to 2); then
  d1 = 20, d2 = 80, centre distance 50, face 13.4 (AGMA's 0.67 d1), worm
  length 33.3. Self-locking is the Worm feature's report: dry yes (5.71
  against 11.3 degrees), greased at the edge.
- **The shafts and bearings.** A PETG shaft carrying the 3 g moment needs
  14.4 mm, so the main bearings are the first 60-series row over it,
  6002, asked of the app (`list_bearings`). The worm journal (the tooth
  force's couple and the separating force) needs 8.5 mm: 609.
- **The arms** are as tall as the camera reaches below the tilt axis in
  +-90 degrees of tilt (`cam_swing`, 160 mm, computed from the camera's and
  cradle's variables) plus a clearance; their spacing is the camera's width
  plus a running gap.
- **The blocks** stand as far out along the worm as the wheel's tip needs
  to clear them, which is why the worm carries spacer collars.
- **Printed fits** come from the print allowance table, asked of the app
  (`print_allowance`), never typed: a Ø32 bearing bore is modelled Ø32.3,
  a Ø15 journal Ø14.9. `fit_bearing` and `check_seat` read a printed seat
  at what it prints.
- The knob is sized so an 8 N fingertip pull holds the load at 1 g.

## Layout

World Z up, the base plate's underside at z = 0, the pan axis on Z, the
camera looking +X at zero pan, the tilt axis along Y. The pan wheel is
screwed to the plate and does not turn: the yoke turns on the two 6002s
(one in the plate, one in the wheel) and carries the pan worm, which walks
round the wheel. The tilt wheel and its shaft turn in the arms' 6002s; the
tilt worm stands under it on a ledge off the +Y arm.

A worm pair meshes at zero turn when the worm's blank X points at the
wheel and the wheel's at the worm, mid-span on mid-face
(docs/modeling/worm-gears.md#phase); every frame here is explicit for that
reason (a frame built from a cylindrical face does not carry the blank's X).

## Known

- Bearing inner rings are not mated (one solid bearing cannot be fastened to
  both rings in a mechanism), so `check_seat` verifies each outer ring and
  reports the inner ring unverified.
- Nothing retains the yoke against lifting off (gravity holds it) or the
  tilt shaft axially at the -Y arm; a nut or circlip is not modelled.
- The yoke is about 250 mm tall, more than many printers' Z: phase 6
  (topology) is where it gets lighter, and it may have to be split.
