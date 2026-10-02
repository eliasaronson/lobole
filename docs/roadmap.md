# Roadmap

What Designer does not do yet, grouped by what it unblocks and ordered within
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
- **Deliberate contact.** `add_assertion` declares a minimum clearance and
  `check_fits` classifies ISO 286 fits, but `interference` and
  `contact_report` cannot be told that an overlap is a press fit, so a
  designed interference reads as a clash. (A retaining ring's grip in its
  groove is the one designed overlap they relieve,
  [a ring in its groove](parts/README.md#a-ring-in-its-groove).)
- **A weld as a record** (length, size, symbol, a weld table). Welded joins
  are currently flush mates or unioned solids.
- **Fasteners:** property classes, torque and preload (`check_fasteners`
  checks length and engagement, not load); Phillips drives; a countersunk
  screw in a plain bore (refused today); inserts, rivets, dowel pins and
  spring washers.
- **Relations with two inputs:** a relation reads one mate; a delivery valve
  whose timing follows both the camshaft and the rack cannot be expressed.
- **Springs** that stretch between two occurrences and follow the motion.

## 4. Analysis

- **FEM:** thermal, buckling and nonlinear studies; modal analysis without
  supports (free-free); VTU export of results.
- **Tolerances:** an inferred stack-up chain (`stack_up` sums a chain the
  caller states), and general-tolerance tables (ISO 2768).
- **Beam elements** for tube frames: `check_frame` checks the member graph,
  not stiffness, and FEM has only tet10 elements.

## 5. Interchange

- **PMI in STEP** (AP242 GD&T and tolerances); tolerances stay in the app.
- **IGES**, **mesh import beyond STL** (OBJ, PLY and glTF are export-only),
  and **3MF**.

## 6. The agent interface

- **Feature templates:** a saved feature list with parameters, instantiated as
  one composite feature. Feature types are a compiled table (`kFeatureTypes`),
  so an agent can drive every feature and define none. Logic that computes
  geometry already works from outside the app as a generator
  (`generators/gear.py`), with no determinism cost; a template is the
  in-document form, for a composite that recurs across projects. An
  interpreter inside regeneration is not the answer: it brings a sandbox, a
  pinned runtime version and a new determinism gate.
- **Typed contracts for more RPCs.** Only `geometry_snapshot` and
  `validate_snapshot` have machine-readable request and result schemas
  ([scripting/rpc.md](scripting/rpc.md#typed-inspection-contracts)); every
  other method is a `MethodDoc` row, so a wrong type or enum is caught only
  inside the handler and the SDK is untyped. The direction: extend the
  contracts to the most-used methods, with structured errors and generated
  typed SDK stubs, and let a write carry the snapshot revision it was built
  from so a stale reference is refused.
- Putting the current selection into an agent's prompt as context.
- `design_audit`: perturb each parameter and report which perturbations break
  regeneration.
- Benchmark tasks graded on relations (gears, racks, cams); mechanisms are
  graded only on their mates and a motion answer.

## 7. Platform and presentation

- An **MSAA setting** (the sample count is chosen automatically, capped at
  4x, and is baked into every pipeline).
- **Real-time hybrid ray tracing** in the viewport; ray tracing is the offline
  `render_still` path today. It needs `main.cpp`'s Vulkan setup factored into
  a shared context first.
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
- `app/main.cpp` holds the window, Vulkan setup and frame loop in one file
  with no unit tests; split it on its own, against the pixel gate.
- `tests/rpc_gate_smoke.py` still regex-parses handler bodies; it could read
  the `MethodDoc` flags alone.

## Not planned

- NURBS surface modelling (boundary, fill, sew, thicken).
