# pantilt

A geared pan/tilt head for a 1.5 kg camera: a worm pair on each axis, a
machined aluminium base plate with a 3/8-16 tripod thread, everything else
printed in PETG. The plan, and the phases after this one (FEM and topology of
the yoke, the base plate on the 6090, drawings):
[docs/plans/pan-tilt-head.md](../../docs/plans/pan-tilt-head.md).

    tools/headless-preview.sh --start PORT        # a scratch instance
    generators/pantilt/run_all.sh PORT            # everything, ~2.5 h
                                                  # (the yoke's topology
                                                  # study, 2 h on the CPU;
                                                  # the motion studies, 9;
                                                  # the CAM job, 1.5)
    generators/pantilt/run_all.sh PORT --from yoke_topo  # its studies only
    generators/pantilt/run_all.sh PORT --from cam # the plate's CAM job only
    generators/pantilt/run_all.sh PORT --from assembly
    PANTILT_SAMPLES=6 generators/pantilt/run_all.sh PORT --from assembly  # ~4 min

Each stage opens the project the one before saved; all of it lands in
`scratch/pantilt/` (projects, renders, `assembly_report.json`, the BOM, one
log per stage, and `run_all.log`, which ends in `done` or `failed`):

| script | makes | saves |
|---|---|---|
| `build_base.py` | the project variables (`spec.py`), the base plate (aluminium: the lower pan bearing's pocket, the tapped 3/8-16 tripod hole with its Thread, four M4 tapped holes, 0.5 mm chamfers on the rims the 6090 breaks) and the pan wheel (PETG, 40 teeth, hobbed; the upper pan bearing's counterbore, four counterbored screw holes) | `base.dsgn` |
| `build_worms.py` | the worm (one part for both axes: ZA, one start, 0.25 mm backlash, spacer collars and journals), the bearing block (one part for all four bearings) and the knob | `worms.dsgn` |
| `build_tilt.py` | the tilt wheel with its shaft, the cradle (camera plate, 1/4-20 slot), the camera (the load: a box and a lens of 1.5 kg) and the 1/4-20 camera screw (a plain part with a cosmetic thread) | `tilt.dsgn` |
| `build_yoke.py` | the yoke, modelled where it stands, its frames the layout: the design re-modelled over its topology result (A-frame arms, a web under the tilt worm, a windowed base on a keel grillage, foot flanges bolted to the base), and the phase-5 solid and the topology envelope as variants `build_yoke_topo.py` builds | `yoke.dsgn` |
| `build_assembly.py` | the assembly: mates, bearings by `fit_bearing` and `check_seat`, screws by `fit_fastener`, two worm relations, the standing exact sweep, tolerances and `check_fits`, BOM and masses, pan 360 and tilt +-90 motion studies checked exactly at every sample, clearance assertions verified at both ends of the tilt, renders | `pantilt.dsgn` |
| `build_yoke_topo.py` | the yoke under the camera's load at 3 g (`yoke_loads.py`: three cases, followed from the camera to the seats and the ledge): static studies of the solid yoke and the head's, the lens carried from the seats (`probe_study`); the envelope's topology study and its pictures; the yoke as the three pieces it prints in, each checked against a 256 mm bed, with their STLs; stills; `yoke_topo_report.json` | `yoke_topo.dsgn`, `yoke_print_*.stl` |
| `cam.py` | the base plate's job on the 6090 (Mach3): a vise jaw part, the machine, two setups (Top, Underside), each verified at 0.1 mm and required ok AND complete with the thread judged in its band, posted; `--shots` plays each back for screenshots (needs `headless-preview.sh --gui`) | `cam.dsgn`, `nc/base_plate - Top.nc`, `nc/base_plate - Underside.nc`, `cam_report.json` |

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

## The base plate on the 6090

`cam.py`, phase 7. The machine is the chess set's 6090 profile
(`generators/chess/cam.py`), 3-axis here.

- **The blank:** saw-cut 90 x 90 from 30 mm 6082/6061-T6 plate. 30 is the
  thinnest EN 485 plate that faces both sides of the 24.7 mm part by a
  millimetre; 90 is the Ø73.6 disc plus the 10 mm clearing cutter and 2 mm
  either side, to the next 5 mm. Margins: 8.2 mm round, 3.3 on top, 2.0
  under.
- **Top** (as modelled; the vise grips the blank's lower 8 mm on
  parallels): face (2 x 1.65), clear the waste round the disc down to 2 mm
  over the jaws (adaptive, 1 mm engagement, 10 mm deep), the 6002's seat
  (bored with the 10 mm leaving 0.2, finished with a fresh 6 mm in one
  full-depth pass, then a spring pass: the same lap again), the relief, the
  four M4 tap holes (3.3 flat-bottom drill, pecked; tap them M4 by hand), the
  seat-mouth and rim chamfers. 20.1 min.
- **Underside** (flipped; the vise on the finished disc's top 10 mm): face
  to 24.7, clear the rest of the waste 0.5 mm into what Top cleared, the
  5/16 tap bore (helical, 6 mm), the 3/8-16 UNC-2B thread (3 passes, pitch
  diameter 8.569 in 2B's 8.494-8.645, judged by the check), the tripod-mouth
  and rim chamfers. 17.7 min.
- **37.8 min** of program at the stated feeds and the 1500 mm/min traverse,
  plus six manual tool changes, each with a Z touch-off (the post stops on
  `M0`), and the flip. Both setups verify ok and complete at 0.1 mm.
- **Tools and feeds** (`TOOLS` in `cam.py`, each with its arithmetic): chip
  loads about half of Harvey Tool's and Helical Solutions' 6061 charts for a
  VMC, every feed under the 1500 mm/min the machine states. 10 mm 3-flute at
  15000 rpm and 0.03 mm/tooth (1350 mm/min); 6 mm 3-flute finisher at 20000
  and 0.02 (1200); 3.3 mm flat-bottom carbide drill at 12000 and 0.05/rev;
  6 mm 60 degree single-form thread mill (head 6, neck 3.6 x 18) at 15000
  and 0.02 (900 at the rim); 6 mm 90 degree chamfer mill at 18000 (1000).
  ER20 holders, 35 mm nuts, each tool's stickout stated so the holder check
  runs.

**The K7 seat.** Ø32 K7 is +7/-18 µm: 31.982 to 32.007, 25 µm wide. The
program puts the finisher's rim on 32.000, inside the band and 7 µm under
its top; the CAM reads no tolerance, so that is where any bore goes. A
router does not hold 25 µm unaided: spindle runout (an ER20 collet, 10-20
µm), the finisher's deflection and the steppers' reversal at the quadrants
each take a part of it, and all push the bore over size, which is the loose
side. What the band needs from the 6090: the finish and spring passes (in
the program), then a measured bore -- cut the seat, gauge it before the
setup is broken, and if it is over, re-post the finish with the finisher's
diameter raised by the excess (the path is tool-centre; there is no G41 to
trim) and cut it again. Expect about +0.01 to +0.04 as cut. If it will not
come into the band, a bearing retaining compound (Loctite 641/638-class)
on a slip fit is the honest fallback, not a pressed K7.

**Not measured -- measure these at the machine** (`UNMEASURED` in `cam.py`):
the traverse rate and the travel (the chess job's numbers); the spindle's
power, usable speed range and runout; the vise's jaw height and width; the
parallels' height under the blank (22 mm proud of the jaws for the 8 mm
grip); the ER20 nuts' diameter and each tool's stickout; and the machine's
real bore size error, which is the K7 question above.

## Known

- Bearing inner rings are not mated (one solid bearing cannot be fastened to
  both rings in a mechanism), so `check_seat` verifies each outer ring and
  reports the inner ring unverified.
- Nothing retains the yoke against lifting off (gravity holds it) or the
  tilt shaft axially at the -Y arm; a nut or circlip is not modelled.
- The M4 threads are tapped by hand: an M4 thread mill would be a 2.4 mm
  head on a router, and nothing in the check says a thread on the part is
  left uncut.
- The flipped setup's playback shows the waste Top already removed from
  below it as a solid slab: a height map seen from above cannot hold an
  undercut. The check reads it from Top's program and is right.
- The yoke is about 250 mm tall, more than many printers' Z: phase 6
  (topology) is where it gets lighter, and it may have to be split.
