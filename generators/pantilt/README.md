# pantilt

A geared pan/tilt head for a 1.5 kg camera: a worm pair on each axis, a
machined aluminium base plate with a 3/8-16 tripod thread, everything else
printed in PETG. The plan, and the phases after this one (FEM and topology of
the yoke, the base plate on the 6090, drawings):
[docs/plans/pan-tilt-head.md](../../docs/plans/pan-tilt-head.md).

    tools/headless-preview.sh --start PORT        # a scratch instance
    generators/pantilt/run_all.sh PORT            # everything, ~45 min
                                                  # (the yoke's statics, 25;
                                                  # the motion studies, 9;
                                                  # the CAM job, 1.5)
    generators/pantilt/run_all.sh PORT --from yoke_topo  # its studies only
    python3 generators/pantilt/build_yoke_topo.py --port PORT --fresh-topology
                                                  # the topology study run
                                                  # again: 2 h on the CPU
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
| `build_yoke_topo.py` | the yoke under the camera's load at 3 g (`yoke_loads.py`: three cases, followed from the camera to the seats and the ledge): static studies of the solid yoke and the head's, the lens carried from the seats (`probe_study`); the yoke as the three pieces it prints in, each checked against a 256 mm bed, with their STLs; stills; the envelope's topology study and its pictures -- run once and kept in `yoke_envelope.dsgn`, reused while the envelope is the same (its volume), `--fresh-topology` to run it again; `yoke_topo_report.json` | `yoke_topo.dsgn`, `yoke_envelope.dsgn`, `yoke_print_*.stl` |
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
- The yoke prints as three pieces bolted together (below); the studies
  model it as one solid, so the joint is taken as bonded.

## The yoke under load

`build_yoke_topo.py`; the numbers are `yoke_topo_report.json`'s.

**The load.** The camera at 3 g, followed down its path (`yoke_loads.py`):
the camera, cradle, wheel and shaft are one body on two bearings (radial at
both, axial at +Y); the wheel's tooth force at the worm holds the tilt, so
it cancels the load's moment about the axis, plus a separating force of
tan 20 deg of it; the worm hands its thrust to the ledge. The yoke is fixed
on its pan journal and collar. Three cases: 3 g down, level (the working
pose); 3 g down at 63.4 deg of tilt, where the CoM's lever is longest (the
tooth force 98.7 N, the spec's `wheel_ft_3g`); 3 g sideways. The lens is
not in the study: its motion is carried from what the two bearing bores
and the ledge did (`probe_study`): the shaft's line through the bores, its
turn from the wheel's teeth following the worm, the lens 160 mm in front of
and 80 above the axis.

| 3 g | solid yoke, 591 g | the head's yoke, 1051 g |
|---|---|---|
| level: lens / camera pitch | 5.5 mm / 92' | 1.8 mm / 29' |
| tilted 63 deg: lens / pitch | 10.3 mm / 210' | 3.2 mm / 65' |
| sideways: lens | 13.1 mm | 4.2 mm |
| largest von Mises (sideways) | 18.3 MPa | 21.2 MPa |
| factor of safety, PETG yield 50 / across layers 40 | 2.7 / 2.2 | 2.4 / 1.9 |

The solid yoke is phase 5's with the pan journal fixed (below). Mesh
arm_t / 3, about 54k and 112k tet10s.

**The topology study** is of `yoke_envelope`: the base out to the pan
blocks' far edge on both sides, a hub under it down to the wheel, each arm a
plate across that width up to the tilt axis -- inside the tilt worm's
blocks and their nuts above and under the ledge. The seats, shoulders,
reliefs and screw holes are kept. Its volume fraction is the solid yoke's
volume in the envelope (0.48): the stiffest layout of the same material,
and the target asked of the re-model is the lens at most a third of the
solid yoke's at 3 g level. The study's case is the tilted one (the gear's
worst); 30 iterations at an 8 mm mesh, 33k tets, two hours on the CPU (the
GPU solver fails on it at the first solve, docs/topopt/README.md). Its
compliance settled from iteration 15 (139, from 171 at iteration 5); it did
not reach the change tolerance.

What it says (`yoke_topo_{density,shape}_{iso,arm_pos,arm_neg,base_top,
base_under}.png`), read critically:

- The +Y arm (the gear's side) stays a solid plate; the -Y arm becomes a
  single strut from the bearing to the -X end of its foot, along that
  case's bearing load. One load case, so one strut: under the level or the
  sideways load it would bend. The re-model makes both arms A-frames.
- The base stays solid on -X and +Y and empties the +X/-Y quadrant --
  where the pan worm's blocks bolt on, which no case loads. The re-model
  keeps it.
- The screw holes' kept rings float free under the base: a keep face
  freezes the tets it touches, connected or not.

**The re-model** (`build_yoke.lighten`): A-frame arms with windows, the +Y
arm solid above the ledge; a web under the tilt worm, from the base to the
ledge, which with the arm closes a box (the worm's thrust 24 mm outboard of
a 12 mm plate twisted it); a windowed base on a keel grillage down to the
wheel; foot flanges, two ISO 4762 M4 each side through the base with ISO
4032 nuts. Held at its underside instead of its journal, the re-modelled
yoke's lens moves 0.46 mm at 3 g level: three quarters of the 1.8 mm is the
Ø15 PETG journal and the base around it -- the pan bearing pair, sized for
strength, is what limits the stiffness, and topology cannot reach it.

**Printing.** On a 256 mm cube (it also fits a 250 x 210 x 220 bed) as
three pieces split at the base's top, `yoke_print_*.stl`: the base (564 g,
159 x 206, 43 high, top face down), the +Y arm with the web and the ledge
(339 g, upright, 187 high; the ledge outboard of the web on support) and
the -Y arm (148 g, on its inner face, 24 high). Whole, the yoke fits the
volume but has no orientation without supports under a 220 mm U.

The assembly re-runs with it: the standing sweep and both motion studies
are clean; the camera now passes the A-frames 1.2 mm off at full tilt (the
designed running gap), where it passed empty space beside the old arms.
