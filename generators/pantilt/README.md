# pantilt

A geared pan/tilt head for a 1.5 kg camera: a worm pair on each axis, a
machined aluminium base plate with a 3/8-16 tripod thread, everything else
printed in PETG. It holds a camera still, turns without play, and screws
onto a tripod. It was the dogfood that built modelled and imperial threads
([holes and threads](../../docs/modeling/holes-and-threads.md)), worm pairs
([worm gears](../../docs/modeling/worm-gears.md),
[the worm relation](../../docs/assemblies/relations.md#worm)), bearings as
standard parts ([bearings](../../docs/parts/README.md#bearings)), printed
clearances ([printed parts](../../docs/tolerances/README.md#printed-parts)),
thread milling ([thread mill](../../docs/cam/operations.md#thread-mill)) and
section views, parts lists and balloons
([drawings](../../docs/drawings/README.md#section-views)); what it taught is
[the last section](#what-the-round-taught).

    tools/headless-preview.sh --start PORT        # a scratch instance
    generators/pantilt/run_all.sh PORT            # everything, ~45 min
                                                  # (the yoke's statics, 25;
                                                  # the motion studies, 9;
                                                  # the CAM job, 1.5; the
                                                  # drawings and renders, 1)
    generators/pantilt/run_all.sh PORT --from yoke_topo  # its studies only
    python3 generators/pantilt/build_yoke_topo.py --port PORT --fresh-topology
                                                  # the topology study run
                                                  # again: ~80 s on the GPU
    generators/pantilt/run_all.sh PORT --from cam # the CAM job onward
    generators/pantilt/run_all.sh PORT --from drawings  # the sheets, renders
    generators/pantilt/run_all.sh PORT --from assembly
    generators/pantilt/run_all.sh PORT --skip yoke_topo renders  # no later
                                                  # stage reads either; what
                                                  # the generator_pantilt
                                                  # test runs
    PANTILT_SAMPLES=6 generators/pantilt/run_all.sh PORT --from assembly  # ~4 min

Each stage opens the project the one before saved; all of it lands in
`scratch/pantilt/` (projects, renders, `assembly_report.json`, the BOM,
`drawings/`, `renders/`, one log per stage, and `run_all.log`, which ends in
`done` or `failed`):

| script | makes | saves |
|---|---|---|
| `build_base.py` | the project variables (`spec.py`), the base plate (aluminium: the lower pan bearing's pocket, the tapped 3/8-16 tripod hole with its Thread, four M4 tapped holes, 0.5 mm chamfers on the rims the 6090 breaks) and the pan wheel (PETG, 40 teeth, hobbed; the upper pan bearing's counterbore, four counterbored screw holes) | `base.dsgn` |
| `build_worms.py` | the worm (one part for both axes: ZA, one start, 0.25 mm backlash, spacer collars and journals), the bearing block (one part for all four bearings) and the knob | `worms.dsgn` |
| `build_tilt.py` | the tilt wheel with its shaft, the cradle (camera plate, 1/4-20 slot), the camera (the load: a box and a lens of 1.5 kg) and the 1/4-20 camera screw (a plain part with a cosmetic thread) | `tilt.dsgn` |
| `build_yoke.py` | the yoke, modelled where it stands, its frames the layout: the design re-modelled over its topology result (A-frame arms, a web under the tilt worm, a windowed base on a keel grillage, foot flanges bolted to the base), and the phase-5 solid and the topology envelope as variants `build_yoke_topo.py` builds | `yoke.dsgn` |
| `build_assembly.py` | the assembly: mates, bearings by `fit_bearing` and `check_seat`, screws by `fit_fastener`, two worm relations, the standing exact sweep, tolerances and `check_fits`, BOM and masses, pan 360 and tilt +-90 motion studies checked exactly at every sample, clearance assertions verified at both ends of the tilt, renders | `pantilt.dsgn` |
| `build_yoke_topo.py` | the yoke under the camera's load at 3 g (`yoke_loads.py`: three cases, followed from the camera to the seats and the ledge): static studies of the solid yoke and the head's, the lens carried from the seats (`probe_study`); the yoke as the three pieces it prints in, each checked against a 256 mm bed, with their STLs; stills; the envelope's topology study and its pictures -- run once and kept in `yoke_envelope.dsgn`, reused while the envelope is the same (its volume), `--fresh-topology` to run it again; `yoke_topo_report.json` | `yoke_topo.dsgn`, `yoke_envelope.dsgn`, `yoke_print_*.stl` |
| `cam.py` | the base plate's job on the 6090 (Mach3): a vise jaw part, the machine, two setups (Top, Underside), each verified at 0.1 mm and required ok AND complete with the thread judged in its band, posted, the K7 seat judged in its band and the uncut features checked to be only the hand-tapped M4s; `--shots` plays each back for screenshots (needs `headless-preview.sh --gui`) | `cam.dsgn`, `nc/base_plate - Top.nc`, `nc/base_plate - Underside.nc`, `cam_report.json` |
| `build_drawings.py` | three A3 sheets ([below](#drawings-and-renders)): the base plate's shop drawing, the head's assembly drawing with balloons and the parts list, and the print sheet; each exported and `check_drawing`ed | `drawings.dsgn`, `drawings/{base_plate,assembly,print}.{pdf,svg}`, `drawings_report.json` |
| `build_renders.py` | path-traced stills of the head: hero, tilted 35 degrees by its worm, from below, exploded along its own axes | `renders/{hero,tilted,low,exploded}.png` |

`spec.py` holds the derivation; `common.py` the connection, the print
allowance lookup and the frame helper.

## Sizes

Every size is a project variable, most of them expressions over the load:
1.5 kg, its CoM 80 mm up and 40 mm forward of the tilt axis, sized for 3 g.

- **The worm pair.** Lewis on the wheel's teeth at 3 g against a third of
  PETG's layer strength gives the module (1.95, taken up to 2); then
  d1 = 20, d2 = 80, centre distance 50, face 13.4 (AGMA's 0.67 d1), worm
  length 33.3. Self-locking is the Worm feature's report: dry yes (5.71
  against 11.3 degrees), greased at the edge (q = 12, gamma 4.76 degrees,
  would lock greased). At full tilt the lever is 89.4 mm, 1.32 N m at 1 g
  and 3.95 at 3 g; pan carries no gravity torque, so tilt sizes both pairs
  ([sizing a pair](../../docs/modeling/worm-gears.md#sizing-a-pair)):

  | m | d1 (q 10) | d2 (z 40) | a | tooth force 1 g / 3 g | face | Lewis at 3 g | tooth at pitch |
  |---|---|---|---|---|---|---|---|
  | 1.25 | 12.5 | 50 | 31.25 | 53 / 158 N | 8.4 | 50 MPa | 1.96 mm |
  | 1.5 | 15 | 60 | 37.5 | 44 / 132 N | 10.1 | 29 MPa | 2.36 mm |
  | **2** | **20** | **80** | **50** | 33 / 99 N | **13.4** | **12 MPa** | 3.14 mm |

  At m 2 the 3 g bending stress is a quarter of PETG's layer strength, a
  tooth is seven perimeters thick at the pitch line, and the worm drives
  with about 0.12 N m.
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

## The head, assembled

`build_assembly.py`; the numbers are `assembly_report.json`'s.

- **Masses.** Without the camera 2.0 kg (the base plate 260 g of
  aluminium, the re-modelled yoke 1051 g, the tilt wheel and shaft 126 g,
  the cradle 92 g, the pan wheel 73 g, each worm 23 g, the bearings 182 g);
  with the 1.5 kg camera 3.5 kg (the parts list in `pantilt_bom.csv`).
- **Motion.** Pan 360 degrees (40 worm turns) and tilt +-90 (+-40 turns),
  driven from the worm shafts, 48 samples each, every pair checked exactly:
  both clean, no sample unsolved. The exact checks are the time: the pan
  study is 12 to 18 minutes of the run.
- **Clearances.** The camera clears the yoke by 4.8 mm at -90 degrees (the
  designed `cam_clear`) and passes the A-frames 1.2 mm off (the running
  gap); the cradle runs 1.2 mm off the arms. The standing exact sweep is
  clean.
- **Fits.** The eight bearing seats are transition fits: K7 in the
  aluminium pocket, +-0.15 on the printed bores (modelled 0.3 over and read
  at what they print). `check_seat` passes every outer ring; the inner
  rings are unverified (below).
- **Bought parts.** 4 x 6002, 4 x 609, 4 x ISO 4762 M4x16 into the plate,
  8 x M4x25 with ISO 4032 nuts and ISO 7089 washers through the worm blocks,
  4 x M4x30/M4x40 through the yoke's foot flanges, and the 1/4-20 camera
  screw (modelled: imperial screws are not standard parts).

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
  four M4 tap holes (3.3 mm 118 degree drill, pecked, to the point the
  model has; tap them M4 by hand, 5.9 deep), the
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
  and 0.02 (1200); 3.3 mm 118 degree carbide drill at 12000 and 0.05/rev;
  6 mm 60 degree single-form thread mill (head 6, neck 3.6 x 18) at 15000
  and 0.02 (900 at the rim); 6 mm 90 degree chamfer mill at 18000 (1000).
  ER20 holders, 35 mm nuts, each tool's stickout stated so the holder check
  runs.

**The K7 seat.** Ø32 K7 is +7/-18 µm: 31.982 to 32.007, 25 µm wide. The
program aims the finisher's rim, and the spring pass's, at the band's
middle, 31.9945, and says so in its header (`(fit op 7: bore D32 K7
31.982-32.007, aimed at 31.9945)`); the check reads the posted bore at
31.9946, in its band. A
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
  head on a router. The check lists them as not cut by the job, and
  `cam.py` stops on anything else it lists.
- The flipped setup's playback shows the waste Top already removed from
  below it as a solid slab: a height map seen from above cannot hold an
  undercut. The check reads it from Top's program and is right.
- The yoke prints as three pieces bolted together (below); the studies
  model it as one solid, so the joint is taken as bonded.
- The M4 holes are drilled 8 deep with a 118 degree point and threaded
  5.9, three pitches short of it (a Hole's derived drill point and thread
  depth). The tripod hole is bored flat with the 6 mm end mill and states
  so (`drill_point_deg` 180): a drill's point there would break through
  the web into the relief.

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
worst); 30 iterations at an 8 mm mesh, 33k tets: about 80 s on the GPU with
fp64 refinement ([mixed-precision refinement](../../docs/topopt/README.md#mixed-precision-refinement));
the saved result was run on the CPU in two hours, when the fp32 GPU solver
could not converge on a part this slender. Its
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

## Drawings and renders

`build_drawings.py` and `build_renders.py`; the sheets are in
`scratch/pantilt/drawings/` (PDF and SVG), the stills in
`scratch/pantilt/renders/`. Every sheet is `check_drawing`ed clean, and its
dimensions are measured off the projection and its callouts are the model's
own text, so a change upstream re-draws rather than going stale.

- **`base_plate`** (A3, 2:1): the top (the disc Ø73.6, the relief Ø23.5
  seen through the seat, `4× M4 DEEP 5.9, DRILL Ø3.3 DEEP 8` and their
  37.34 spacing), SECTION A-A folded under it through the seat and the
  tripod hole (24.7 thick, the seat `Ø32 K7 (+0.007/-0.018)` 9 deep, the
  relief 10.2, the drill 12.7, the 3/8-16 thread drawn to ISO 6410 in the
  section), the underside at 1:1 (`3/8-16 UNC-2B DEEP 12.7, DRILL Ø7.94
  DEEP 12.7`, the chamfers), datums A (the face it stands on) and B (the
  seat), the notes the dimensions cannot carry (perpendicularity and runout
  of B to A, the M4 holes' position, gauging the seat), and the title block's
  material (6082-T6 or 6061-T6 plate, 30 thick) and general tolerance
  (ISO 2768-mK). Geometry is picked by what it is (a circle of the seat's
  diameter, the face at z = 0 in the section), from the project's
  variables, never by toponame strings.
- **`assembly`** (A3, 1:2.5): the head from the front without its camera,
  SECTION C-C through the pan worm's axis (the worm in its 609s, the wheel's
  teeth, the screws that hold the wheel down) and SECTION B-B on both axes
  (the pan journal in its 6002s, the tilt shaft in its pair, the yoke in
  section), a balloon on every one of the 17 parts-list rows, each placed on
  the view that draws the most of that part, in a column beside it in the
  order of its anchor so leaders do not cross; the parts list on the title
  block (item, quantity, part, made/printed/bought, material, unit mass).
- **`print`**: the PETG parts, how each goes on the bed and why, an iso of
  each, and the printed-fit allowances the model was built with, asked of
  `print_allowance` (hole +0.3 at Ø32 and Ø24, shaft -0.1, counterbore +0.2).

The stills are path-traced with the parts' materials (the plate aluminium,
the printed parts PETG, the bearings and screws steel): `hero` (the head
carrying its camera), `tilted` (35 degrees up, posed by turning the tilt
worm 35 x 40 turns' worth: the worm relation turns the wheel), `low` (from
under the pan worm) and `exploded` (along the head's own axes: the pan stack
down the pan axis, the tilt shaft out along its own, each worm out of its
blocks, screws out of their holes and their nuts the other way; the app's
`auto_explode` pushes parts radially from the centre, which scatters an
axial stack, so the steps are written out).

**At the machine** (the user): cut the plate, gauge the seat before the
setup is broken, print the head, assemble it on a tripod with a camera.

## What the round taught

From the commits' times: the round ran from the plan (30 September, 22:00)
to these drawings (1 October, about 18:00), some 20 hours of wall clock with
phases overlapping in worktrees. Of 29 fix commits in that window, 27 were
found by the head.

| phase | wall clock | what it cost |
|---|---|---|
| 0 survey, 1-4 threads, worm pair, bearings, print allowance, thread mill | 22:00-01:15, 3.3 h | four worktrees in parallel; the worm's hobbing by booleans did not scale (8 hob placements in one common took 105 s, about x7 per doubling), so the wheel is the envelope lofted point by point |
| 5 the head, assembled | 01:45-05:10, 3.4 h | 16 fixes (bearings, fasteners, baked geometry, the worm's crest, a cut at a face's height, holes, a save that read clean); the worm wheel's rebuild was 15 s (forty lofted spaces cut one by one), now 2.3 s by one replicated sector; the exact pan study 12 to 18 min a run |
| 7 the base plate on the 6090 | 05:20-07:20, 2 h | 6 fixes (clear-round, a hole that opens downward, a face's exact extent, a bore's rough, a chamfer on a round rim, thread marks through a later feature) |
| precision | 08:00-12:10, 4.2 h | the solid yoke would not mesh: 5 µm slivers from single-precision sketch planes, then the same in placements and mates; the model went to double end to end |
| 6 the yoke under load | 05:30-13:30, 8 h | the topology study ran 2 h on the CPU (the fp32 GPU solver could not converge on a part this slender; it now refines in fp64 and takes 80 s, against 7313 s), and a merge verification that rebuilt from nothing ran it again; the re-model took about 4 h, eight build-and-study iterations of 3-6 min, the density read off section screenshots by hand |
| 8 drawings, renders | 15:00-18:00, 3 h | the drawing had no sections, parts list, balloons, datums or material row: built; 12 defects fixed on the way (below) |

**What blocked.** Nothing for long on its own; three things together held
the round for most of a day. The precision of the model (no study could run
on the solid yoke until the sketch planes, and then the placements, were
double); the topology study's price on the CPU, paid twice because a
verification rebuilt from nothing; and the topology result itself, which is
a picture and an STL, so the re-model was read off screenshots and only
known to meet its target once built. The yoke stage now reuses its saved
study while the envelope is the same (`--fresh-topology` to run it again),
and `run_all.sh --from STAGE` runs what changed.

**Defects phase 8 found and fixed** (the drawing of a 25 mm plate): lengths
printed to three significant figures (37.34 read 37.3; 1051 would read
`1.05e+03`); a cylinder's seam drawn as an edge (a tick on every chamfer
from above, and a Ø across a cylinder's sides failing when one side was the
seam); leaders ending at the centre of their text; exported values sitting
on their own dimension lines; centre marks on arcs of hidden circles; an
`extreme` asked of a circle answering its centre; the validator flagging any
label in a round part's bounding box; a still of an exploded view rendered
assembled; seventeen balloons recomputing the parts list's masses on every
resolve (an export past a minute); section cuts thrashing a 64-entry cache
(146 s for the assembly sheet, 27 s now); the title block's notes field
saved and never printed; "the active document is not a part, not a part".

### The friction list, by what it costs the user

1. **Studies are not jobs.** An exact motion study is minutes (pan 12 to 18 min)
   and `leave_at` runs the whole study again to pose the end; a study keeps
   running after its client dies (stop it by pid); nothing keys a result to
   its inputs, so a rebuild re-runs it (the second 2 h topology run). Hours.
2. **A topology result is a picture.** One load case per study (the layout
   follows that case: a single strut in an arm), no keep-out bodies (the
   first envelope overlapped the tilt worm's blocks, found only in the
   result), islands kept where a keep face freezes unconnected tets, no
   static study on the optimised shape, no snapping a re-model to it, no
   split-into-printable-parts verb. About 4 h of re-modelling by hand.
3. **CAM trusts the model where the model is silent.** It ignores part
   tolerances (the K7 seat is cut at nominal; a band the router cannot hold
   unaided is the machinist's problem); a profile with tabs never verifies
   complete; a Hole has no drill point and a Thread no length short of its
   bore, so blind tapped holes are threaded to the bottom of a flat drill;
   a thread the job does not cut (the M4s) is ignored silently; adaptive
   clearing is slow. Risks a wrong part. (Fixed since, in the friction
   round: the seat is aimed into K7 and judged against it, tabs and
   offcuts verify, a Hole has a drill point and a thread depth, the M4s are
   listed as uncut, and adaptive's lead paths are cheaper.)
4. **Fits stop at one ring.** `check_seat`/`fit_bearing` verify the outer
   ring only (a one-solid bearing mates to one seat in a mechanism); ISO
   286's J classes are missing (J7); a Hole's clearance fit ignores the
   print allowance; `set_tolerance` reaches only the active part.
5. **Drawings, after this round.** No feature control frames (the datums'
   meaning is written as notes), no rotated dimension text, no detail
   views, no auto-layout of balloons or auto-placement of a linear Ø (the
   generator places them), no material grade in the material library
   ("ALUMINUM" in the parts list), a 1/4-20 camera screw that cannot be a
   standard part.
6. **Modelling.** A frame built from a cylindrical face has an arbitrary X
   (every worm frame is written out); the expression language has no table
   lookup (bearing and screw sizes are chosen in Python).
7. **Tooling.** `headless-preview.sh --stop` finds the instance only under
   the `XDG_STATE_HOME` it was started with; a fresh worktree's submodules
   clone from the network, which stalled once for minutes.

### The next friction round

1. Studies as jobs: a handle, cancel and stop by id, results kept and
   reused by an input hash (motion, static, topology), `leave_at` from the
   recorded samples, the exact motion check cached between samples.
2. Topology you can build from: several load cases, keep-out bodies, a
   connectivity filter, a static study on the optimised mesh, a split for
   the printer.
3. CAM honest about tolerance and threads -- built: see
   [tolerance bands](../../docs/cam/operations.md#tolerance-bands) and
   [what the job does not cut](../../docs/cam/simulation.md#what-the-job-does-not-cut).
4. Fits: J/js classes, both rings of a bearing checked, the print allowance
   in a Hole's clearance fit, `set_tolerance` on any part.
5. Drawings: feature control frames on datums, rotated text, balloon
   auto-layout, imperial screws as standard parts, a material grade.
