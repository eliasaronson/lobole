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
([drawings](../../docs/drawings/README.md#section-views)), and then paid for
a friction round of its own; what both taught is
[the last section](#what-the-rounds-taught).

    tools/headless-preview.sh --start PORT        # a scratch instance
    generators/pantilt/run_all.sh PORT            # everything, ~13 min
                                                  # (the yoke's studies, 9;
                                                  # the motion studies, 2;
                                                  # the CAM job, 1; the
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
    PANTILT_SAMPLES=6 generators/pantilt/run_all.sh PORT --from assembly
    PANTILT_PAN_BEARING=tilt generators/pantilt/run_all.sh PORT
                                                  # the pan journal sized as
                                                  # the head was first built
                                                  # (below: the pan bearing)

Each stage opens the project the one before saved; all of it lands in
`scratch/pantilt/` (projects, renders, `assembly_report.json`, the BOM,
`drawings/`, `renders/`, one log per stage, and `run_all.log`, which ends in
`done` or `failed`). A study whose inputs a previous run already solved is
answered from its result, not run again
([studies as jobs](../../docs/simulation/README.md#studies-as-jobs)), so a
second run against the same instance spends seconds where the first spent
minutes.

| script | makes | saves |
|---|---|---|
| `build_base.py` | the project variables (`spec.py`), the base plate (6082-T6 aluminium, `aluminum_6082_t6`: the lower pan bearing's pocket, the tapped 3/8-16 tripod hole with its Thread, four M4 tapped holes, 0.5 mm chamfers on the rims the 6090 breaks) and the pan wheel (PETG, 40 teeth, hobbed; the upper pan bearing's counterbore, four counterbored screw holes) | `base.dsgn` |
| `build_worms.py` | the worm (one part for both axes: ZA, one start, 0.25 mm backlash, spacer collars and journals), the bearing block (one part for all four bearings) and the knob | `worms.dsgn` |
| `build_tilt.py` | the tilt wheel with its shaft, the cradle (camera plate, 1/4-20 slot) and the camera (the load: a box and a lens of 1.5 kg) | `tilt.dsgn` |
| `build_yoke.py` | the yoke, modelled where it stands, its frames the layout: the design re-modelled over its topology result (A-frame arms, a web under the tilt worm, a windowed base on a keel grillage, foot flanges bolted to the base), and the phase-5 solid and the topology envelope as variants `build_yoke_topo.py` builds | `yoke.dsgn` |
| `build_assembly.py` | the assembly: mates, the camera screw as the ASME B18.3 1/4-20 it is bought as (`add_fastener`), bearings by `fit_bearing`, the two DIN 471 rings by `fit_retaining_ring`, both rings of each bearing by `check_seat` (all eight must pass), screws by `fit_fastener` and `check_fasteners`, two worm relations, the standing exact sweep, tolerances and `check_fits`, BOM and masses, pan 360 and tilt +-90 motion studies checked exactly at every sample, clearance assertions verified at both ends of the tilt, renders | `pantilt.dsgn` |
| `build_yoke_topo.py` | the yoke under the camera's load at 3 g (`yoke_loads.py`: three cases, followed from the camera to the seats and the ledge): a static study of the solid yoke and one of the head's, each with the three as load cases, the lens carried from the seats (`probe_study`), and the pan journal's own bend, which no study sees, by beam; the yoke cut for the printer (`split_for_printing`) with the pieces' STLs; stills; the envelope's topology study (the three cases at once, the worms' blocks as keep-outs), its pictures and its check (`check_topology`) -- kept in `yoke_envelope.dsgn`, reused while the envelope, its cases and keep-outs are the same and the result is not stale, `--fresh-topology` to run it again; `yoke_topo_report.json` | `yoke_topo.dsgn`, `yoke_envelope.dsgn`, `yoke_print_*.stl` |
| `cam.py` | the base plate's job on the 6090 (Mach3): a vise jaw part, the machine, two setups (Top, Underside), each verified at 0.1 mm and required ok AND complete with no toolpath warning and the thread judged in its band, posted, the K7 seat judged in its band and the uncut features checked to be only the hand-tapped M4s, and a setup sheet for each; `--shots` plays each back for screenshots (needs `headless-preview.sh --gui`) | `cam.dsgn`, `nc/base_plate - Top.nc`, `nc/base_plate - Underside.nc`, `nc/base_plate - {Top,Underside} setup sheet.pdf`, `cam_report.json` |
| `build_drawings.py` | three A3 sheets ([below](#drawings-and-renders)): the base plate's shop drawing, the head's assembly drawing with balloons and the parts list, and the print sheet; each exported and `check_drawing`ed, and the stage stops on a sheet that is not clean. It only draws: what the sheets order by is the model's | `drawings.dsgn`, `drawings/{base_plate,assembly,print}.{pdf,svg}`, `drawings_report.json` |
| `build_renders.py` | path-traced stills of the head: hero, tilted 35 degrees by its worm, from below, exploded along its own axes | `renders/{hero,tilted,low,exploded}.png` |

`spec.py` holds the derivation; `common.py` the connection, the print
allowance lookup, the frame and plane helpers, `named(S)`, the
variables as `Param`s, and the bearing seat.

**The bearing seats are one template**
([templates](../../docs/modeling/templates.md)). The base plate's pocket,
the pan wheel's counterbore and the yoke's two tilt seats are each a
counterbore the bearing's outside wide and its width deep, cut into the
face it is pressed in from. `build_base.py` draws the first on the plate's
top, saves it to the project's library as `bearing seat` (parameters `on`,
`D`, `B` and `v`, the centre's offset along the face's v) and takes it out
again; every seat is then an instance (`common.bearing_seat`): the pocket
`D = pan_brg_D`, the counterbore and the tilt seats the printed sizes the
print allowance asks for, the tilt seats standing on the arms' outer
planes at `v = tilt_z`. The geometry is the four cuts' it replaced, to the
last ulp of every volume.

**Every part is lint-clean** (`check_literals`,
[literals](../../docs/variables/README.md#literals-that-should-be-expressions)):
no feature stands at a world height a face is already at, and no number
equals a variable. A sketch goes ON the face it belongs to where that face
is a disc on the axis its sketch is drawn about (the plate's and the pan
wheel's tops, the knob's, the collars' ends the journals and the tilt
shaft stand on); everywhere else -- the worm's ends, which its thread
moves off the axis, and the yoke and cradle, modelled where they stand --
on a plane `common.plane` places by its variable (a Frame with a `Param`
origin). Coordinates, distances and frames are `Param` arithmetic over
`named(S)`, so the document carries the formula. `base_t` 2 mm more
regenerates every part without an error: the plate's pocket, relief and
`seat` frame rise with its top, the yoke -- base, arms, bearing bores,
ledge and its eight frames -- 2 mm up its own Z, and the assembly solves
with the pan wheel, cradle and camera 2 mm higher. The bought hardware
follows too: every one of the 40 pieces `fit_fastener` placed rides in its
hole, by the hole's named seat face and bore
([attached, and mated](../../docs/parts/README.md#attached-and-mated)), so
the yoke's joint screws and the pan blocks' nuts and washers rise with the
yoke's faces, and the exact clash sweep stays clean. The two retaining
rings ride their grooves the same way
([a ring in its groove](../../docs/parts/README.md#a-ring-in-its-groove)).

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
- **The tilt shaft and its bearings.** A PETG shaft carrying the 3 g tilt
  torque needs 14.4 mm, so the arms' bearings are the first 60-series row
  over it, 6002, asked of the app (`list_bearings`). The worm journal (the
  tooth force's couple and the separating force) needs 8.5 mm: 609.
- **Shoulders, from the abutment table.** `list_bearings` gives each row's
  SKF abutment limits, declared beside its d, D and B (`*_brg_da_min`,
  `_da_max`, `_Da_max`), and every shoulder round a bearing is an
  expression over them: a collar on an inner ring mid-way in d_a
  (`pan_collar_d` 30.05, `main_collar_d` 18.7, `worm_collar_d` 12.65), a
  housing's shoulder round an outer ring between d_a max and D_a max, so it
  supports the ring and clears the turning inner one (`pan_relief_d`
  37.85, `relief_d` 25.2, `worm_relief_d` 18.15; printed ones modelled at
  their allowance).
- **The retaining rings.** Two inner rings have no shoulder on one side --
  the lower 6005's on the yoke's journal (the yoke could lift off it) and
  the -Y 6002's on the tilt shaft -- so each is held by a DIN 471 ring in
  a groove at the bearing's side face (`list_retaining_rings`: 25x1.2 in a
  Ø23.9 x 1.3 groove, 15x1 in Ø14.3 x 1.1). Each journal runs on past its
  bearing by the groove and the edge distance beyond it (`*_ring_land`,
  m + n: 3.0 and 2.2 mm): the tilt shaft now stands 2.2 mm proud of the
  -Y arm, and the plate's relief under the lower 6005 is that much deeper
  (`pan_relief_depth`), the plate that much thicker so the web over the
  tripod hole stays what it was. The grooves are cut in the printed
  journals at what they print, and the rings are placed by
  `fit_retaining_ring`. A steel circlip in a PETG groove takes the yoke's
  lift and the tilt shaft's float, not the 3 g loads, which the shoulders
  and the +Y bearing carry; a printed nut on a thread was the alternative,
  and a Ø25 or Ø15 printed thread is coarse and weaker than the groove's
  land.
- **The camera screw** is sized from the socket it threads into. A camera's
  1/4-20 tripod socket is shallow: camera makers' manuals cap a tripod
  screw at 5.5 mm into the body (`cam_socket`, ASSUMED from them; measure
  the camera). The screw engages a pitch less (`cam_screw_eng`, 4.23 mm) so
  it cannot bottom out, and the cradle's plate is as thick as puts a
  stocked length there: ASME B18.3 1/4-20 x 1/2 (`cam_screw_l`) through an
  8.47 mm plate (`plate_t`, more than its eight perimeters).
- **The pan journal and its bearings.** The yoke's journal is a cantilever
  from its collar under the head's overturning moment: the load at 3 g
  sideways, its CoM at its highest 275.5 mm over the collar (`pan_lever`:
  the yoke's base over the pan blocks, the camera's 160 mm swing and its
  clearance under the tilt axis, and the CoM's 89.4 mm lever over it), is
  12.2 N m, three times the tilt torque. A PETG journal carrying it at a
  third of the layer strength needs 21.0 mm (`pan_shaft_d_min`), so the pan
  pair is 6005 (Ø25 x Ø47 x 12). The camera's sizes are declared in
  `spec.py` for this, in the first stage, and `build_yoke.py` checks that
  the layout it builds stands the load where `pan_lever` says. The head was
  first built with the pan pair the tilt shaft's 6002: below,
  [the pan bearing](#the-pan-bearing).
- **The arms** are as tall as the camera reaches below the tilt axis in
  +-90 degrees of tilt (`cam_swing`, 160 mm, computed from the camera's and
  cradle's variables) plus a clearance; their spacing is the camera's width
  plus a running gap.
- **The blocks** stand as far out along the worm as the wheel's tip needs
  to clear them, which is why the worm carries spacer collars.
- **Printed fits** come from the print allowance table, asked of the app
  (`print_allowance`), never typed: a Ø47 bearing bore is modelled Ø47.3,
  a Ø25 journal Ø24.9. `fit_bearing` and `check_seat` read a printed seat
  at what it prints.
- The knob is sized so an 8 N fingertip pull holds the load at 1 g.

## Layout

World Z up, the base plate's underside at z = 0, the pan axis on Z, the
camera looking +X at zero pan, the tilt axis along Y. The pan wheel is
screwed to the plate and does not turn: the yoke turns on the two 6005s
(one in the plate, one in the wheel) and carries the pan worm, which walks
round the wheel. The tilt wheel and its shaft turn in the arms' 6002s; the
tilt worm stands under it on a ledge off the +Y arm.

A worm pair meshes at zero turn when the worm's blank X points at the
wheel and the wheel's at the worm, mid-span on mid-face
(docs/modeling/worm-gears.md#phase); every frame here is explicit for that
reason (a frame built from a cylindrical face takes the face's seam as X:
[connector frames](../../docs/assemblies/solver.md#connector-frames)).

## The head, assembled

`build_assembly.py`; the numbers are `assembly_report.json`'s.

- **Masses.** Without the camera 2.14 kg (the base plate 292 g of
  aluminium, the re-modelled yoke 1063 g, the tilt wheel and shaft 126 g,
  the cradle 116 g, the pan wheel 56 g, each worm 23 g, the bearings 283 g:
  two 6005s 163, two 6002s 61, four 609s 59; the two rings 3 g); with the
  1.5 kg camera 3.64 kg (the parts list in `pantilt_bom.csv`).
- **Motion.** Pan 360 degrees (40 worm turns) and tilt +-90 (+-40 turns),
  driven from the worm shafts, 48 samples each by default, every pair
  checked exactly; regenerated at `PANTILT_SAMPLES=6` (255 and 236 pair
  checks), both clean, no sample unsolved. Each ring turns with its shaft
  against its bearing's side, touching and not clashing.
- **Clearances.** The camera clears the yoke by 4.8 mm at -90 degrees (the
  designed `cam_clear`) and passes the A-frames 1.2 mm off (the running
  gap); the cradle runs 1.2 mm off the arms. The standing exact sweep is
  clean (168 pairs), each ring in its groove included: drawn at its free
  diameter it overlaps the floor (26.6 and 8.8 mm3), and that grip is
  relieved as designed, nothing else
  ([a ring in its groove](../../docs/parts/README.md#a-ring-in-its-groove)).
- **Fits.** The eight bearing seats are transition fits: K7 in the
  aluminium pocket, +-0.15 on the printed bores (modelled 0.3 over and read
  at what they print).
- **Both rings of every bearing** are checked on the seat each sits on
  (`check_seat`): the outer ring on the housing its `Bearing seat` mate
  names, the inner on the journal read through the coaxial cylinder under
  it ([both rings](../../docs/parts/README.md#both-rings); not `mate_other`,
  which would close a second joint through each bearing beside the yoke's
  and the shaft's own). Every ring's diameter passes; every ring that sits
  on a shoulder is seated on it, and every shoulder and fillet is inside
  SKF's abutment limits (d_a, D_a, r_a;
  [abutment limits](../../docs/parts/README.md#abutment-limits)). The two
  inner rings no shoulder locates -- the lower 6005's and the -Y 6002's --
  are located by their retaining rings, each beside the ring's face and
  reaching, as installed, inside the same d_a (Ø29.9 in 28.2-31.9, Ø18.7
  in 17-20.4). All eight bearings PASS, both rings each; a ring with no
  seat, or any check that fails or stays unverified, stops the stage.
- **Bought parts.** 2 x 6005, 2 x 6002, 4 x 609; 4 x ISO 4762 M4x14 into
  the plate; 8 x M4x25 with ISO 4032 nuts and ISO 7089 washers through the
  worm blocks; 2 x M4x30 and 2 x M4x40 through the yoke's foot flanges;
  DIN 471 25x1.2 and 15x1; and the camera screw, an ASME B18.3 1/4-20 x 1/2
  socket head cap screw (above, sizes).
- **`check_fasteners`**: 17 screws examined, one finding, expected and
  explained: the camera screw has 4.23 mm of thread in the camera where the
  app's rule asks one diameter (6.35). No camera socket meets that rule --
  the socket is 5.5 mm by design -- and the load is 1.5 kg at 3 g, 44 N, on
  a thread whose 4 mm strips at well over a kilonewton. The stage stops on
  any other finding.

## The base plate on the 6090

`cam.py`. The machine is the chess set's 6090 profile
(`generators/chess/cam.py`), 3-axis here.

- **The blank:** saw-cut 90 x 90 from 35 mm 6082/6061-T6 plate. 35 is the
  thinnest EN 485 plate that faces both sides of the 31.7 mm part by a
  millimetre; 90 is the Ø73.6 disc plus the 10 mm clearing cutter and 2 mm
  either side, to the next 5 mm. Margins: 8.2 mm round, 1.3 on top, 2.0
  under.
- **Top** (as modelled; the vise grips the blank's lower 8 mm on
  parallels): face (two passes), clear the waste round the disc as deep as
  the 10 mm cutter's 22 mm of flute reaches from the blank's top, a
  millimetre short (adaptive, 1 mm engagement, 10 mm deep), the 6005's
  seat (bored with the 10 mm leaving 0.2, finished with a fresh 6 mm in one
  full-depth pass, then a spring pass: the same lap again, both under
  cutter compensation), the relief, the
  four M4 tap holes (3.3 mm 118 degree drill, pecked, to the point the
  model has; tap them M4 by hand, 5.9 deep), the seat-mouth and rim
  chamfers. 26.8 min.
- **Underside** (flipped; the vise on the finished disc's top 10 mm): face
  to 31.7, clear the rest of the waste 0.5 mm into what Top cleared, the
  5/16 tap bore (helical, 6 mm), the 3/8-16 UNC-2B thread (3 passes, pitch
  diameter 8.569 in 2B's 8.494-8.645, judged by the check), the tripod-mouth
  and rim chamfers. 17.8 min.
- **44.6 min** of program at the stated feeds and the 1500 mm/min traverse,
  plus six manual tool changes, each with a Z touch-off (the post stops on
  `M0`), and the flip. Both setups verify ok and complete at 0.1 mm, with
  no toolpath warning: when the plate grew to 35 mm stock the Top clearing
  reached 25 mm down on 22 mm of flute, the feeds check said so, and the
  depth Top cannot reach is now the Underside's.
- **Tools and feeds** (`TOOLS` in `cam.py`, each with its arithmetic): chip
  loads about half of Harvey Tool's and Helical Solutions' 6061 charts for a
  VMC, every feed under the 1500 mm/min the machine states. 10 mm 3-flute at
  15000 rpm and 0.03 mm/tooth (1350 mm/min); 6 mm 3-flute finisher at 20000
  and 0.02 (1200); 3.3 mm 118 degree carbide drill at 12000 and 0.05/rev;
  6 mm 60 degree single-form thread mill (head 6, neck 3.6 x 18) at 15000
  and 0.02 (900 at the rim); 6 mm 90 degree chamfer mill at 18000 (1000).
  ER20 holders, 35 mm nuts, each tool's stickout stated so the holder check
  runs.

**The K7 seat.** Ø47 K7 is +7/-18 µm: 46.982 to 47.007, 25 µm wide. The
program aims the finisher's rim, and the spring pass's, at the band's
middle, 46.9945, and says so in its header; the check reads the posted
bore at 46.9946, in its band
([tolerance bands](../../docs/cam/operations.md#tolerance-bands)). A
router does not hold 25 µm unaided: spindle runout (an ER20 collet, 10-20
µm), the finisher's deflection and the steppers' reversal at the quadrants
each take a part of it, and all push the bore over size, which is the loose
side. What the band needs from the 6090: the finish and spring passes (in
the program), then a measured bore, corrected at the machine. Both passes
are posted under cutter compensation in wear mode (`G41 D6`,
[cutter compensation](../../docs/cam/post.md#cutter-compensation)): the
file is still the tool centre the check read, and tool 6's diameter in
Mach3's tool table holds only the wear. A bore cannot be made smaller
again, and this one comes out over, so the first run leaves it small on
purpose: +0.04 in the table, cut the seat, gauge it before the setup is
broken, take the shortfall off the entry and run the finish and the spring
pass again (the setup sheet gives their lines). Expect about +0.01 to
+0.04 of oversize as cut. If it will not come into the band, a bearing
retaining compound (Loctite 641/638-class) on a slip fit is the honest
fallback, not a pressed K7.

**Setup sheets.** Each setup posts with its sheet beside the program
(`nc/base_plate - <setup> setup sheet.pdf`,
[the setup sheet](../../docs/cam/setup.md#the-setup-sheet)): the blank,
the zero and where to touch off, the jaws, the tools in the order they are
loaded with their stickout, the operations with the line each starts on,
the K7 band and what the tool table holds, and the M4s left to tap.

**Not measured -- measure these at the machine** (`UNMEASURED` in `cam.py`):
the traverse rate and the travel (the chess job's numbers); the spindle's
power, usable speed range and runout; the vise's jaw height and width; the
parallels' height under the blank (27 mm proud of the jaws for the 8 mm
grip); the ER20 nuts' diameter and each tool's stickout; and the machine's
real bore size error, which is the K7 question above.

## Known

- The camera screw engages 4.23 mm of thread in the camera, under the
  one diameter `check_fasteners` asks; the camera's socket is 5.5 mm deep
  by design and the depth is ASSUMED from makers' manuals (above, sizes).
  Measure the camera and set `cam_socket`.
- The circlips' grooves are cut in printed PETG journals. They hold the
  yoke against lifting and the tilt shaft against float, not a working
  load; a groove's land in PETG is the weak part, and a pull that matters
  wants a steel sleeve or a machined journal.
- The M4 threads are tapped by hand: an M4 thread mill would be a 2.4 mm
  head on a router. The check lists them as not cut by the job, and
  `cam.py` stops on anything else it lists.
- The flipped setup's playback shows the waste Top already removed from
  below it as a solid slab: a height map seen from above cannot hold an
  undercut. The check reads it from Top's program and is right.
- The yoke prints as three pieces bolted together (below); the studies
  model it as one solid, so the joint is taken as bonded.
- The yoke's statics fix its journal and collar, so they are of the yoke on
  a rigid journal; the journal's own bend is a beam estimate beside them
  (below), not a study.
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

**The journal.** A study fixes the journal's face and the collar's, so it
cannot see the journal bend. `yoke_loads.journal` estimates it by beam: the
moment of the loads on the yoke about the collar (less its pan component,
which the worm holds), carried by the journal from the collar to the upper
bearing's centre and dying out to the lower one, the two `wheel_face`
apart; the yoke turns with the journal's end and the lens with it.

| 3 g | solid yoke, 607 g | the head's yoke, 1063 g |
|---|---|---|
| level: lens / camera pitch | 4.7 mm / 81' | 1.51 mm / 25' |
| tilted 63 deg: lens / pitch | 9.4 mm / 184' | 2.90 mm / 57' |
| sideways: lens | 10.6 mm | 3.20 mm |
| largest von Mises (sideways) | 12.7 MPa | 7.3 MPa |
| factor of safety, PETG yield 50 / across layers 40 | 3.9 / 3.1 | 6.8 / 5.5 |
| the Ø25 journal, by beam: lens more, level / tilted / sideways | 0.18 / 0.21 / 0.81 mm | the same |
| the journal's bending stress, sideways | 7.7 MPa | the same |

The solid yoke is phase 5's with the pan journal fixed (below). Mesh
arm_t / 3, about 61k and 91k tet10s. Each yoke is one study with the three
cases in it (one factorisation): 100 s and 260 s.

**The topology study** is of `yoke_envelope`: the base out to the pan
blocks' far edge on both sides, a hub under it down to the wheel, and each
arm a whole plate across that width up to the tilt axis. The worms' blocks
are keep-outs, boxes from their own sizes and frames with the running gap
round them (`keep_outs`): the tilt blocks standing in the +Y arm's slab
above the ledge took 593 and 614 tets, their nuts' room under it 1408 and
1415, the pan blocks none (they hang under the base, which they touch).
The seats, shoulders, reliefs and the pan blocks' screw holes are kept; the
tilt blocks' screws, under a keep-out, are not. All three cases at once,
each weighted by the inverse of its compliance on the uniform start (4.9,
1.1 and 0.5: at equal weights the sideways case is nearly the whole
objective and the level one, the working pose, is left what the others
leave). 30 iterations at arm_t / 2 (6.1 mm), 37k tets: 120 s on the GPU for
the three, at the head's yoke's mass (0.79 of the domain, 1063 g).

At the solid yoke's volume (0.45 of the domain) the check
(`check_topology`, a static of the shape before modelling from it) said
the result cannot be built as it stands: the -Y arm is one strut a tet or
two wide, the volume-matched level reaches the -Y seat only through tet
edges, and at the highest level that carries it the lens moved 18.7 / 13.7
/ 6.9 mm, worse than the solid yoke in two cases (measured with the Ø15
journal; the study runs at the head's yoke's mass since). The shape checked
case by case, the same seats carried the same way (`yoke_loads.lens`):

| 3 g, lens / max von Mises | topology shape, 1063 g | the hand re-model, 1063 g | the solid domain, 1338 g |
|---|---|---|---|
| level | 1.20 mm / 2.4 MPa | 1.51 mm / 2.0 MPa | 1.19 mm |
| tilted 63 deg | 2.33 mm / 5.1 MPa | 2.90 mm / 4.8 MPa | 2.28 mm |
| sideways | 3.51 mm / 8.7 MPa | 3.20 mm / 7.3 MPa | 3.45 mm |

The shape's lens moves 20 % less than the re-model's level and tilted and
10 % more sideways, and within 3 % of the whole domain's at 79 % of its
mass.
Read critically (`yoke_topo_{density,shape}_*.png`): the -Y arm is an
A-frame with a cross strut, the re-model's arm; the +Y arm stays a whole
plate with the blocks' and nuts' pockets cut through it; the base stays
whole. The envelope has no room for the re-model's web under the tilt worm
or its keels under the base, so the shape cannot have either.

**Re-modelling closer to it was tried and not adopted.** Each variant of
the re-model studied as the head's yoke is (`build_yoke.lighten`, the same
three cases and mesh):

| re-model | mass | lens level / tilted / sideways |
|---|---|---|
| as built: +Y arm windowed below the ledge, web, keels | 1062 g | 1.50 / 2.88 / 3.17 mm |
| the +Y arm a whole plate, as the shape keeps it | 1123 g | 1.30 / 2.49 / 3.03 mm |
| the same, without the web (the shape's +Y side) | 1048 g | 1.44 / 2.77 / 4.44 mm |
| the same, without the keels | 1015 g | 1.73 / 3.39 / 4.09 mm |

The whole +Y plate is stiffer in every case, but 61 g heavier; paid for by
dropping the web it loses 40 % sideways, by dropping the keels it loses in
every case. No variant that can be built beats the current yoke at its
mass in all three cases, so the yoke stays as it is; the topology study's
level and tilted gain lives in the +Y plate, and its sideways loss in the
web the envelope cannot hold.

**The re-model** (`build_yoke.lighten`): A-frame arms with windows, the +Y
arm solid above the ledge; a web under the tilt worm, from the base to the
ledge, which with the arm closes a box (the worm's thrust 24 mm outboard of
a 12 mm plate twisted it); a windowed base on a keel grillage down to the
wheel; foot flanges, two ISO 4762 M4 each side through the base with ISO
4032 nuts.

**Printing.** On a 256 mm cube (it also fits a 250 x 210 x 220 bed) as
three pieces, `split_for_printing` with one cut at the base's top (the
arms above it are two solids, so two pieces), `yoke_print_*.stl`: the base
(575 g, 159 x 206 x 46), the +Y arm with the web and the ledge (339 g, 159
x 60 x 187) and the -Y arm (148 g, 159 x 24 x 187), each a Derived copy of
the yoke cut by a box, in PETG. Whole, the yoke fits the volume but has no
orientation without supports under a 220 mm U.

## The pan bearing

Phase 6 found that held at its underside instead of its journal, the
re-modelled yoke's lens moved a quarter of what it did on the journal: the
Ø15 PETG pan journal and the base round it, the pan pair sized like the
tilt shaft, were most of the deflection. Sized for the moment it carries
instead ([sizes](#sizes)), the pan pair is a 6005. Both are built by the
generator (`PANTILT_PAN_BEARING=moment`, the default, and `tilt`), on one
build, studied the same way:

| 3 g | 6002 pan pair (Ø15 journal) | 6005 pan pair (Ø25 journal) |
|---|---|---|
| level: lens, study | 1.79 mm | 1.50 mm |
| tilted 63 deg: lens, study | 3.17 mm | 2.88 mm |
| sideways: lens, study | 4.22 mm | 3.17 mm |
| largest von Mises in the yoke (sideways) | 20.8 MPa | 7.7 MPa |
| the journal, by beam: lens more, level / tilted / sideways | 1.18 / 1.39 / 5.37 mm | 0.18 / 0.21 / 0.81 mm |
| the journal's bending stress (sideways); against 40 across the layers | 35.5 MPa (1.1) | 7.7 MPa (5.2) |
| the yoke | 1051 g | 1062 g |
| the head without its camera | 1987 g | 2090 g |

"Clearly better" was set before the numbers: lower in every case by more
than the 2 % the mesh alone moves them (phase 6's same-mesh comparison), no
stress worse, for a mass the head can carry. The 6005 is 16, 9 and 25 %
lower in the study, the yoke's largest stress falls by two thirds, and the
6002's journal -- which the study cannot see -- would have bent at 3 g at
35 MPa against PETG's 40 across the layers it is printed in, adding more
to the lens sideways than the whole yoke does. It costs 104 g (5 % of the
head, 3 % with its camera: the bearings 102 g of it) and a thicker plate
(28.7 against 24.7 mm, cut from 35 mm stock: 11.6 min more on the 6090).
Adopted; `PANTILT_PAN_BEARING=tilt` builds the 6002 head. (These figures are
from before the retaining rings: the journal's ring land has made the plate
31.7 mm and the head 2.14 kg since.)

## Drawings and renders

`build_drawings.py` and `build_renders.py`; the sheets are in
`scratch/pantilt/drawings/` (PDF and SVG), the stills in
`scratch/pantilt/renders/`. Every sheet is `check_drawing`ed clean (the
stage stops otherwise), and its dimensions are measured off the projection
and its callouts are the model's own text, so a change upstream re-draws
rather than going stale -- the pan bearing went from a 6002 to a 6005 and
every sheet followed.

- **`base_plate`** (A3, 2:1): the top (the disc Ø73.6, the relief Ø37.85
  seen through the seat, `4× M4 DEEP 5.9, DRILL Ø3.3 DEEP 8` and their
  42.64 spacing), SECTION A-A folded under it through the seat and the
  tripod hole (31.7 thick, the seat `Ø47 K7 (+0.007/-0.018)` 12 deep, the
  relief 16.2, the drill 12.7, the 3/8-16 thread drawn to ISO 6410 in the
  section, the seat's Ø placed by the app above the section), DETAIL C of
  the seat's shoulder at 5:1 (its width 4.58 and the 4.2 step down to the
  relief), the underside at 1:1 (`3/8-16 UNC-2B DEEP 12.7, DRILL Ø7.94
  DEEP 12.7`, the chamfers), datum features A (the face it stands on) and
  B (the seat) and the feature control frames that name them
  ([feature control frames](../../docs/drawings/README.md#feature-control-frames)):
  the seat perpendicular to A within Ø0.02 over its Ø, its shoulder's
  circular runout 0.03 to B in the detail, the M4s' position Ø0.2 to A and
  B under their callout; the notes the frames cannot carry (the stock, the
  CAM job's blank; tapping the M4s by hand; gauging the seat), and the
  title block's grade (6082-T6, the plate's material,
  [grades](../../docs/rendering/materials.md#grades)) and general tolerance
  (ISO 2768-mK). The shoulder's runout reads the old note's "runout of B
  to A" as the face the bearing's outer ring is squared by. Geometry is
  picked by what it is (a circle of the seat's diameter, the face at z = 0
  in the section), from the project's variables, never by toponame
  strings; the section stands under the top view by the plate's height.
- **`assembly`** (A3, 1:2.5): the head from the front without its camera,
  SECTION C-C through the pan worm's axis (the worm in its 609s, the wheel's
  teeth, the screws that hold the wheel down) and SECTION B-B on both axes
  (the pan journal in its 6005s, the tilt shaft in its 6002s, the yoke in
  section), a balloon on every one of the 20 parts-list rows laid out by
  the app ([balloon layout](../../docs/drawings/README.md#balloon-layout):
  each on the view that draws the most of it, round that view, no two
  leaders crossing); the parts list on the title block (item, quantity,
  part, made/printed/bought, material grade, unit mass), the camera screw
  in it as `ASME B18.3 1/4-20 x 1/2`, BOUGHT
  ([inch screws](../../docs/parts/README.md#inch-screws-asme-b183)).
- **`print`**: the PETG parts, how each goes on the bed and why, an iso of
  each, and the printed-fit allowances the model was built with, asked of
  `print_allowance` for each bearing's seat, relief and journal (hole +0.3,
  shaft -0.1, counterbore +0.2).

The stills are path-traced with the parts' materials (the plate aluminium,
the printed parts PETG, the bearings and screws steel): `hero` (the head
carrying its camera), `tilted` (35 degrees up, posed by turning the tilt
worm 35 x 40 turns' worth: the worm relation turns the wheel), `low` (from
under the pan worm) and `exploded` (along the head's own axes: the pan stack
down the pan axis, the tilt shaft out along its own, each worm out of its
blocks, screws out of their holes and their nuts the other way, each
bearing told by where it stands; the app's `auto_explode` pushes parts
radially from the centre, which scatters an axial stack, so the steps are
written out).

**At the machine** (the user): cut the plate, gauge the seat before the
setup is broken, print the head, assemble it on a tripod with a camera.

## What the rounds taught

### The head's round

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
verification rebuilt from nothing; and the topology result itself, which was
a picture and an STL, so the re-model was read off screenshots and only
known to meet its target once built.

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

### The friction list, and what the friction round made of it

The head's round ranked what had cost the user most; the friction round
that followed fixed it, two workstreams at a time, each with tests that
failed before the fix. Measured on this head:

1. **Studies were not jobs.** An exact motion study was minutes (pan 12 to
   18 min) and `leave_at` ran the whole study again to pose the end; a
   study kept running after its client died; nothing keyed a result to its
   inputs, so a rebuild re-ran it (the second 2 h topology run). Now a
   study is a job with a handle, cancelled and stopped by id, its result
   kept and reused by an input hash and saved with the project
   ([studies as jobs](../../docs/simulation/README.md#studies-as-jobs),
   [motion studies](../../docs/assemblies/README.md#motion-studies)). The
   exact pan study's checks run every sample's pairs in one batch on all
   the cores
   ([what a check costs](../../docs/assemblies/README.md#clash-sweeps-what-a-check-costs)):
   48 samples, 1294 pair checks, 144 s before and 54 s after; the same
   inputs again answer in under 0.05 s.
2. **A topology result was a picture.** One load case per study (the layout
   followed that case: a single strut in an arm), no keep-out bodies (the
   first envelope overlapped the tilt worm's blocks, found only in the
   result), islands kept where a keep face froze unconnected tets, no
   static study of the optimised shape, no split into printable parts:
   about 4 h of re-modelling by hand. Now load cases, keep-outs, the
   connectivity filter, `check_topology`, `split_for_printing` and
   `topo_to_body`
   ([topology you can build from](../../docs/topopt/README.md#topology-you-can-build-from)):
   the yoke's envelope with all three cases at once and the blocks as
   keep-outs in 80 s on the GPU, its shape checked before anything is
   modelled from it, the yoke split for the bed by one call. Snapping a
   re-model to the shape is not built.
3. **CAM trusted the model where the model was silent.** It ignored part
   tolerances, a profile with tabs never verified complete, a Hole had no
   drill point and a Thread no length short of its bore, a thread the job
   did not cut was ignored silently, and adaptive clearing was slow. Now
   the seat is aimed into its K7 band and judged against it
   ([tolerance bands](../../docs/cam/operations.md#tolerance-bands),
   [their check](../../docs/cam/simulation.md#tolerance-bands)), tabs and
   offcuts verify ([tabs and offcuts](../../docs/cam/simulation.md#tabs-and-offcuts)),
   a Hole has a drill point and a thread depth
   ([drill](../../docs/cam/operations.md#drill),
   [Hole](../../docs/modeling/holes-and-threads.md#hole)), the M4s are
   listed as uncut
   ([what the job does not cut](../../docs/cam/simulation.md#what-the-job-does-not-cut)),
   and adaptive's lead paths are cheaper (libarea patch 0002,
   `deps/libarea/README.md`): on the plate's Top setup the same 78036 moves
   in 26-28 s against 34-40 s.
4. **Fits stopped at one ring.** `check_seat`/`fit_bearing` verified the
   outer ring only; ISO 286's J classes were missing; a Hole's clearance fit
   ignored the print allowance; `set_tolerance` reached only the active
   part. Now both rings
   ([both rings](../../docs/parts/README.md#both-rings)), J and js
   ([limits and fits](../../docs/tolerances/README.md#iso-286-limits-and-fits)),
   the printed [Hole fit](../../docs/modeling/holes-and-threads.md#hole)
   and `set_tolerance {part}`
   ([tolerance RPCs](../../docs/tolerances/README.md#rpcs)).
5. **Drawings.** No feature control frames, no rotated dimension text, no
   detail views, no auto-layout of balloons or auto-placement of a linear
   Ø, no material grade ("ALUMINUM" in the parts list), a 1/4-20 camera
   screw that could not be a standard part. Now
   [feature control frames](../../docs/drawings/README.md#feature-control-frames),
   aligned values and a placed Ø
   ([dimensions and annotation](../../docs/drawings/README.md#dimensions-and-annotation)),
   [detail views](../../docs/drawings/README.md#detail-views),
   [balloon layout](../../docs/drawings/README.md#balloon-layout),
   [inch screws](../../docs/parts/README.md#inch-screws-asme-b183) and
   [grades](../../docs/rendering/materials.md#grades).
6. **Modelling.** A frame built from a cylindrical face had an arbitrary X,
   and the expression language had no table lookup. Now a round face's X is
   its seam ([connector frames](../../docs/assemblies/solver.md#connector-frames))
   and a project has [tables](../../docs/variables/README.md#tables).
7. **Tooling.** `headless-preview.sh --stop` found the instance only under
   the `XDG_STATE_HOME` it was started with; a fresh worktree's submodules
   cloned from the network. Both fixed ([scripting](../../docs/scripting/README.md)).

And the big generators, this one included, became tests
([the big generators as tests](../README.md#the-big-generators-as-tests)).

### Closing the round

Regenerated from nothing on the round's last build, the head found what the
round had not:

- **The pan journal was under-sized.** Sized like the tilt shaft, by the
  tilt torque, it carried three times that as a bending moment; the yoke's
  statics could not see it, since they fix the journal. Sized for the
  moment, the pan pair is a 6005 ([the pan bearing](#the-pan-bearing)).
- **Bigger parts move drawings and programs.** The 6005's thicker plate
  pushed the base plate's section into its top view (the section now stands
  under it by the plate's height) and its stock to 35 mm, where the Top
  clearing reached past its cutter's flutes -- a warning the job's report
  carried and `cam.py` did not stop on (it does now, as the drawing stage
  stops on a sheet that is not clean); the sheet's notes named the old
  stock and seat (they are the CAM job's blank and the model's seat now).
- **What the sheets order by is the model's.** The plate's grade and the
  ASME B18.3 camera screw were patched in by the drawing stage; they are
  made where the parts are, and the drawing stage only draws.
- **`check_seat` read "unverified" for every bearing**, which looked like
  the inner rings going unread. They were read; what stayed unverified was
  the shoulders' diameters, which need a supplier's catalog, and two inner
  rings nothing locates. The report now gives each ring's seat and checks.
