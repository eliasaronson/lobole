# Roadmap

What Loböle does not do yet, grouped by what it unblocks and ordered within
each group by what a real part needs first. Each item is absent from the code
as it stands; how the built parts work is in the area READMEs
([README.md](README.md)). A plan for an item goes in `docs/plans/` before
work starts.

## What the program is for

An open-source parametric CAD application with five goals that set it apart:

1. **Checkpoints** — named points in a model's history that can be inspected,
   rolled back to and branched from.
2. **Variables and expressions** — dimensions defined as expressions over
   parameters.
3. **Scripting** — an API that can do everything the GUI can.
4. **Git-based versioning** — models in a diffable, mergeable text format.
5. **The same interface for people and agents** — an AI assistant uses the
   same command layer, document format and feedback as a person.

All five rest on one design: a headless core with a serializable
command/document layer, of which the GUI is one client
([architecture.md](architecture.md)).

## 1. What a shop needs from a drawing

A drawing today has projected, section and detail views, driven
dimensions, callouts, notes, a parts list with balloons, datum features and
feature control frames, a title block and SVG/PDF export
([drawings](drawings/README.md)). Missing, in the order a shop asks for
them:

- **More view kinds:** broken, crop, offset- and half-section views; a
  section plane off the seven orientations; a detail boundary other than a
  circle.
- **Title-block fields** for finish and a structured general tolerance
  (`TitleBlock` holds project, document, material, scale, author, date and
  free-text notes), and a revision block.
- **Multi-sheet drawings** (a drawing document has one sheet).
- **Symbols:** surface finish and welds; datum targets, composite frames,
  projected zones and datum modifiers (datum features and single frames
  exist).
- Balloons laid out round several views as one problem (each view's ring
  is placed in turn).
- **DXF export of a drawing** (`export_drawing` writes SVG and PDF; DXF exists
  only for a sheet-metal flat pattern).
- Exploded views on a drawing.

## 2. Making what the modeller designs

- **Threads:** a modelled groove cut to its class rather than the basic
  profile; modelled threads on generated fasteners; inch fasteners beyond
  socket head cap screws (nuts, washers, hex heads, UNF) and deriving one
  from its hole ([holes and threads](modeling/holes-and-threads.md),
  [parts](parts/README.md)).
- **CAM for holes:** canned drilling cycles (pecking is expanded into plain
  moves); tapping (G84) for a machine with a spindle encoder (threads are
  milled today, [thread mill](cam/operations.md#thread-mill)); multi-tooth
  thread mills.
- **Distance tolerances between walls** in CAM: a floor or shoulder is cut
  to its band ([tolerance bands](cam/operations.md#tolerance-bands)), a
  slot's width between two walls as modelled.
- **Workholding follow-ons:** generated vises and toe clamps, the spindle nose
  as a collision body, and holding the
  part down on 3D operations (tabs are profile-only).
- **Other processes:** lathe turning (4th-axis milling of a turned part
  exists, [rotary](cam/rotary.md)); laser, plasma and waterjet posts (the
  posts are milling dialects); nesting sheet blanks.
- **Driving an installed slicer.** Print time, filament and the slicer's
  own warnings belong to the slicer, and Loböle is not one: `export_print_job`
  writes the job, and a generator that needs those numbers runs the Bambu
  CLI itself and reads `result.json`. A call that hands the job to the
  slicer the user has installed (Bambu Studio, Orca, PrusaSlicer, through
  their command lines) and returns what it reports would make that one
  step, without estimating anything ourselves.
- **Procurement data:** a material has a grade (alloy and temper), density
  and FEM constants but no cost, stock form or finish, and a BOM row has a
  standard and a designation but no price, supplier, part number or lead
  time.

## 3. Assemblies and cross-part design

- **Assembly-level features:** a cut made in the assembly that goes through
  several parts. Regeneration never crosses a document boundary, and the
  `Derived` feature pulls geometry *into* a part; this case pushes a cut *out*
  to several, and needs its own design.
- **Cross-part linked sketch copies.** A linked copy follows a sketch in the
  same part; across parts it falls back to a plain paste.
- **Other designed overlaps.** The contact checks relieve an engaged
  thread, a retaining ring's grip and a press fit whose size bands allow it
  ([designed overlaps](inspect/README.md#designed-overlaps)); any other
  overlap meant by design (a snap-fit hook, a crush rib, a press fit on
  faces with no size tolerance) reads as a clash.
- **A weld as a record** (length, size, symbol, a weld table). Welded joins
  are currently flush mates or unioned solids.
- **Fasteners:** property classes, torque and preload (`check_fasteners`
  checks length and engagement, not load); Phillips drives; a countersunk
  screw in a plain bore (refused today); inserts, rivets, dowel pins and
  spring washers.
- **Relations with two inputs:** a relation reads one mate; a delivery valve
  whose timing follows both the camshaft and the rack cannot be expressed.
- **Springs** that stretch between two occurrences and follow the motion.
- **Contact beyond a disc cam.** The `contact` relation solves a follower
  from a cam's shape in the planar case only, with the cam's axle and the
  follower's guide rigid to each other: a barrel or face cam, a follower
  driven by another follower, and a roller that spins on its own pin are
  not built ([relations.md](assemblies/relations.md#contact)).
- **The space a moving part sweeps, as geometry.** A motion study says
  whether two parts clash at each sample; designing a part to clear a
  housing over a whole stroke needs the region the housing leaves free in
  the moving part's own frame (the intersection over the stroke), as a solid
  or a section to sketch inside. A generator today samples the poses and
  intersects the polygons itself.
- **Undo for project-level edits.** Units, tube rules, the rollback marker,
  part renames and the folder verbs change the document with no undo step,
  so none of them can be undone, in the GUI or over RPC. A batch
  refuses them for that reason
  ([rpc.md](scripting/rpc.md#a-refused-call-changes-nothing)); giving each a
  command would let a batch take them too.

## 4. Analysis

- **FEM:** thermal, buckling and nonlinear studies; modal analysis without
  supports (free-free); VTU export of results.
- **Tolerances:** an inferred stack-up chain (`stack_up` sums a chain the
  caller states), and general-tolerance tables (ISO 2768).
- **Beam elements** for tube frames: `check_frame` checks the member graph,
  not stiffness, and FEM has only tet10 elements.

## 5. Interchange

- **PMI in STEP** (AP242 GD&T and tolerances); tolerances stay in the app.
- **IGES export**, **PLY and glTF import**, and **3MF** import and a
  general 3MF export (a print job is written as
  one, [print jobs](tolerances/README.md#print-jobs)).
- **Healing a STEP only when it needs it.** OpenCASCADE's read-time
  repair (`ShapeProcess`) runs on every import and is most of a large file's
  read time (about 0.6 s of 1.5 s on a 4.3 MB module). Running it only when a
  validity check of the raw shapes fails could cut that, if the check is much
  cheaper than the repair; that needs measuring first.

## 6. The agent interface

- **Typed contracts for the rest of the surface.** The most-used methods
  have typed rows — validated before the handler, published as
  `request_schema`, typed in the SDK stubs — and part writes take a revision
  guard ([scripting/rpc.md](scripting/rpc.md#typed-contracts)). The other
  rows, CAM's among them, still name keys only, so their wrong types are
  caught inside the handler; only the two inspection methods publish a
  result schema; and the guard covers the active part's writes, not an
  assembly's or a job's.
- Putting the current selection into an agent's context unasked: the
  workflow prompt has the agent read `get_state`'s `selection` before it
  acts on "this" ([mcp.md](scripting/mcp.md#the-workflow-prompt)).
- `design_sweep` with no `variables`: every variable at a default spread
  about its value, so a part's fragile parameters show without ranges stated
  first ([design sweep](requirements/README.md#design-sweep)).
- Benchmark tasks graded on relations (gears, racks, cams); mechanisms are
  graded only on their mates and a motion answer.
- **Benchmark tasks that edit an existing part.** Every task builds from
  nothing, so nothing measures whether an agent reads a part it did not build
  correctly. If those tasks show it misreads them, a read-only
  `describe_document` follows: the part as a short outline (its variables,
  one line per feature saying what it does, references by intent such as
  "the top face of Block"), with no promise that it reruns.
- **The prompt and the design sweep.** The prompt does not mention
  `expression` requirements or `design_sweep`. Adding them changes what the
  benchmark measures, so it waits on the benchmark.

## 7. Platform and presentation

- An **MSAA setting** (the sample count is chosen automatically, capped at
  4x, and is baked into every pipeline).
- **Real-time hybrid ray tracing** in the viewport; ray tracing is the offline
  `render_still` path today. The device and renderers are one owner table
  now (`app/GpuOwners.cpp`); it needs a TLAS kept current across edits.
- Shaded fill of sketch regions.
- Glass in the viewport: a per-body transmission override, and panes sorted
  back to front.
- **Packaging:** deb, rpm, AUR and macOS builds, Windows signing and
  Netgen, and a Flathub submission for the existing Flatpak.

## 8. Code structure

Split what the next feature extends, not as a standalone sweep:

- `HandleSketchViewport` (`ui/SketchInteraction.cpp`) is one ~850-line
  function. It is a pipeline, not a switch over tools, so it splits by phase
  (plane pick, escape ladder, snap inference, dimension-pill gate, per-tool
  handling, click) — one phase per commit, with the shuffled GUI suite between
  each, because its ordering is load-bearing and it has no headless coverage.
- The per-tool state structs inside `ui/AppState.h` (sketch edit, measure,
  simulation, gizmo drag, …) could move beside the actions that own them.
- `app/main.cpp` still holds the window, the frame loop and the captures in
  one file with no unit tests (the device is `app/GpuOwners.cpp`'s); split
  it on its own, against the pixel gate.
- `tests/rpc_gate_smoke.py` still regex-parses handler bodies; it could read
  the `MethodDoc` flags alone.

## Not planned

- NURBS surface modelling (boundary, fill, sew, thicken).
- **An interpreter inside regeneration.** It would bring a sandbox, a
  pinned runtime version and a determinism gate of its own. Feature types
  are a compiled table (`kFeatureTypes`); a composite that recurs is a
  [feature template](modeling/templates.md), which is data, and logic that
  computes geometry runs outside the app as a generator
  (`generators/gear.py`).
- **Exporting a part's history as a replayable script.** The document is
  already its history, and it rebuilds from it; a session replays with
  `lobole.replay`; a part meant to be code is a generator. A script that
  reran would have to translate every feature type and turn history names
  back into intent, and rerunning it would renumber every element that
  assemblies, drawings and jobs point at. What an agent would gain from it,
  reading a part, is the outline above.
- From the comparison with SolidCraft, deliberately not copied: a FreeCAD
  oracle (it shares OpenCASCADE, so it agrees with our kernel's mistakes),
  mutation testing of the code, heuristics that nudge a failing boolean,
  contributor and merge-queue machinery, i18n, themes and a web build, and a
  per-launch token on the control port.
