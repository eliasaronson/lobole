# Cutter compensation in the posts, and a setup sheet per setup

## Problem

**A fit the router cannot hold has no fix at the machine.** The pan/tilt
base plate's K7 bearing seat (31.982 to 32.007, a 25 µm band) is aimed at
the band's middle ([tolerance bands](../cam/operations.md#tolerance-bands)),
but the router's runout, the cutter's deflection and its reversals push a
bore over or under. Today the only correction is to change the tool's
diameter in the job, regenerate and re-post: a new file, carried back to
the machine, for a hundredth. Every controller in the shop has the
standard answer, cutter radius compensation (G41/G42 with a D register),
and no file we write uses it.

**Nothing tells the operator how the job is set up.** The work zero, the
blank, the jaws, which tool goes in which pocket at what stickout, which op
each cuts and for how long, which wall is held to a band and must be gauged
before the vise is opened: all of it is in the job, and none of it reaches
the machine except as G-code comments. The roadmap lists "a setup sheet"
under workholding follow-ons.

## 1. Cutter compensation, wear mode

**The mode: WEAR (the path is the tool centre; the controller adds only
the wear).** Two conventions exist: "in control" posts the part contour and
the controller offsets by the cutter's whole radius from its tool table;
"wear" posts the tool-centre path, as today, under G41/G42 so the table
holds only the deviation. Wear, only, because:

- **The file is still the cut the verifier checks.** Every check here runs
  on the posted file ([one engine](../cam/simulation.md#one-engine)). A
  wear-mode file is the tool-centre path the verifier, the playback, the
  holder and fixture checks and the band judgment already read; with wear 0
  it cuts exactly what was verified. An in-control file would need the
  parser to run its own cutter compensation to know where the tool goes:
  a second implementation of the controller's offsetting, whose
  disagreement with Mach3's would be invisible.
- **The failure of a forgotten table is a hundredth, not a radius.** A
  router's Mach3 tool table is usually empty. In control with an empty
  entry cuts every wall a full radius into the part; in wear mode it cuts
  the verified program.
- **It is what the machinist needs here:** gauge the bore, type the error
  into the table, re-run the finish.

The sign is the useful one: G41 when the wall is on the right of travel
(climb), G42 when it is on the left, so a POSITIVE diameter in the table
moves the tool away from the wall in both. A bore measured 0.02 over: +0.02
in the table cuts it 0.02 smaller. A negative entry takes more (RS274/NGC
controllers swap the side for a negative diameter). The D word is the tool
number.

**What gets compensated: the finishing wall pass.** A new op field
`cutter_comp` (`off` | `wear`, refused otherwise naming both) on profile,
pocket and drill (a bore), a capability bit like any other
([the capability table](../cam/operations.md#the-capability-table-and-the-field-table)).
On a rough-then-finish op it applies to the finishing toolpath only; on
any other op to every wall pass:

- profile: each pass's lap (ramp, lap, overshoot, tabs);
- pocket: each wall ring (level 0, islands included);
- drill: a bore's wall ring (its helix and the flat turn); a hole the
  tool's size has no wall to compensate and is refused.

The strategies declare each wall pass on the toolpath
(`Toolpath::Wall{first, last, materialRight}`), as a thread mill declares
its arcs. `CompensateWalls` (new, `CamComp.cpp`) then builds each one's
**lead-in and lead-out** on the waste side and declares the compensated
run (`Toolpath::Comp{first, last, left}`):

- **Lead-in:** a line half the cutter's radius long, then a quarter arc of
  half its radius tangent into the wall pass's first point. G41/G42 goes on
  the line. Every wall pass is entered after a rapid down to the rapid
  floor or by a link at depth through what the inner rings cleared, so the
  lead-in is either in the air above the stock or in cleared material; the
  approach rapids and the link are rerouted to the line's start.
- **Lead-out:** the mirror: a quarter arc away from the wall, then the
  line, G40 on the line, so the controller's taper back to the programmed
  path happens away from the wall and leaves no mark. The retract after it
  rises over the line's end.
- **Room:** every lead point lies inside the region the wall pass encloses
  (a bore, a pocket, an opening) or outside it (an outside profile), or the
  op is refused naming the wall, as a lead arc is refused in an opening
  with no room. A profile with comp starts each pass at the same point
  (the lead-arc rule), so the leads stack into the waste a stepdown each
  instead of slotting it full depth.

**What the controller can compensate, checked.** The program is checked for
a wear of up to a tenth of the cutter's diameter either way (radius d/20;
0.6 mm in the table for a 6 mm cutter), derived from the tool and stated in
the header. Offsetting by w consumes `w tan(θ/2)` of each segment at a turn
of θ toward the offset; a segment shorter than what its two ends consume,
or a reversal, is a place the controller refuses (LinuxCNC) or gouges
(Mach3). Such a place is REFUSED, as the op's error, naming it in the
file's coordinates and why: "an inside corner whose radius is within
0.3 mm of the cutter's, at X.. Y..". An inside corner sharper than the
cutter is a sharp corner of the tool path and compensates fine; one a
whisker larger than the cutter is a tiny arc that cannot.

**The post.** `kDialects` gets a `cutterComp` column: mach3 and linuxcnc
yes, grbl no (GRBL has no G41/G42). A setup whose op asks for comp on a post
without it is refused at post time naming the op and the post. The run is
written as: `G41 D6` (or `G42`) on its own line, the lead-in line, the
compensated moves (arcs fitted as usual, never across the run's ends), `G40`
on its own line, the lead-out line. The header says, per op:
`(comp op 7 T6: G41 D6 wear - tool table 6 holds the WEAR only, 0 cuts as
programmed, +0.02 leaves 0.01 more a side; checked to +-0.6)`.

**The verifier and the playback simulate the nominal tool**, which is what
the file says with wear 0 (the parser already reads past G40-G42). The
parser now records which ops are under comp; the verdict says so in its own
line ("op 7 is compensated in the controller: checked as programmed, with no
wear"), and `cam_verify`/`cam_simulate` report `cutter_comp`.

**Saved form:** `cutter_comp` in the op's fields (written only when not
`off`): kVersion bump, a job golden at that version, a format.md row.

## 2. The setup sheet

**One PDF page (or more) per setup**, from a new GUI-free writer
(`core/cam/CamSetupSheet.{h,cpp}`): a `SetupSheet` struct of rows and
an optional RGB picture, written as a minimal PDF with base-14 Helvetica
like the drawing export's (`DrawingExport.cpp`), plus an image XObject.
`ui::SetupSheetOf(state, setup)` (new, `src/ui/CamSetupSheet.cpp`) fills it
from what the job already knows:

- job and setup name, project, date, the program's file name (the one Save
  G-code writes, or the RPC's `program`);
- the stock: size, mode, the material and its grade (the material
  library's `GradeOf`, as a drawing's title block);
- the work zero: the corner (`origin_at`), the face Z zero is touched off
  on, its position, safe Z;
- workholding: each fixture by name, part and position (the jaws' gripping
  faces), and a picture of the setup: the job tab's view of this setup
  (part, stock, fixtures) through the quick still (`captureRaster`);
- every tool in cutting order: number, name, type, diameter, flutes,
  stickout and holder, rpm, feeds, the operations it cuts and its minutes;
- the operations in order: name, type, tool, depths, minutes; the cycle
  time (`CuttingMinutes`);
- notes: each band the program aims at (the header's fit lines), each comp
  op with what to type into the table, and "gauge the toleranced walls
  before breaking the setup" whenever a band is aimed at.

**RPC** `export_setup_sheet {job?, setup, path, program?, picture?}` in
`RemoteCam.cpp`, with a `MethodDoc` row; it refuses while passes are pending,
and draws the picture only where there is a renderer (`picture: false`
or `--headless` writes the sheet without it, and says so). **GUI:** a
"Setup sheet" button beside Save G-code in the Generate window, written
beside the project as `<project> - <setup> setup sheet.pdf` (an unsaved
project refused like Save G-code), pumped from the frame like the quick
render.

## Files

- `src/core/cam/CamJob.h` (CutterComp, `Operation::cutterComp`,
  `Toolpath::Wall`, `Toolpath::Comp`), `CamNames.{h,cpp}` (names, the
  capability bit), `CamOpFields.cpp` (the row), `CamComp.{h,cpp}` (new),
  `CamStrategies.cpp` (walls declared, comp applied, carried through
  multi-target and index joins), `CamGeometry.cpp`, `CamTravel.cpp` (the
  rough gets none), `CamPost.{h,cpp}`, `CamGcode.{h,cpp}`,
  `CamVerify.{h,cpp}` (the line), `CamSetupSheet.{h,cpp}` (new),
  `src/ui/CamSetupSheet.{h,cpp}` (new), `CamPanel.cpp` (the field),
  `CamGenerate.cpp` (the button), `BackgroundJobs.cpp`,
  `src/remote/RemoteCam.cpp`, `src/core/io/SerializationInternal.h`,
  CMakeLists.
- Tests: `tests/test_cam_comp.cpp` (new), `tests/test_cam_capability.cpp`
  poke, `tests/golden/job/v158-cam-cutter-comp.*`, a smoke for the RPC.
- Docs: `docs/cam/post.md`, `operations.md`, `simulation.md`, `gui.md`,
  `setup.md`, `README.md`, `docs/roadmap.md`, `docs/versioning/format.md`,
  `generators/pantilt/README.md` if the job changes.

## Verification

- G-code shape: `G41`/`G42` with the right side for climb and conventional,
  `D<tool>`, the lead-in line under it longer than the checked wear, `G40`
  before the lead-out line, no arc across either end, the header line.
- The verdict of the compensated op equals the uncompensated one's: the
  same gouge and leftover, the band's measured diameter within a cell.
- Refusals: grbl at post time; `cutter_comp` on a face or an adaptive; an
  unknown value; a wall with an inside corner a whisker larger than the
  cutter; no room for the lead in a small bore; a drill with no wall.
- The seat: on a COPY of `scratch/pantilt`, the base plate's seat finish
  and spring pass posted with comp; the excerpt read; both setups verify as
  before.
- Setup sheets for both base-plate setups, rasterised with `pdftoppm` and
  read.
- `tools/test-fast.sh`, `ctest -L cam`, the full headless suite, the GUI
  suite and the generators label.
