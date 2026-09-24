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

A drawing today has projected views, driven dimensions, callouts, notes, a
title block and SVG/PDF export. Missing, in the order a shop asks for them:

- **Section and detail views.** A `DrawingView` is an orientation (plus a
  projected child); nothing cuts or magnifies.
- **A parts list and balloons** on an assembly drawing.
- **Title-block fields** for material, finish and general tolerance
  (`TitleBlock` holds project, document, author, date and notes), and a
  revision block.
- **Multi-sheet drawings** (a drawing document has one sheet).
- **Symbols:** GD&T and datum symbols, surface finish, welds.
- **DXF export of a drawing** (`export_drawing` writes SVG and PDF; DXF exists
  only for a sheet-metal flat pattern).
- Exploded views on a drawing.

## 2. Making what the modeller designs

- **Threads fully specified:** a thread class that is more than display text,
  imperial (UNC/UNF) sizes, left-hand threads, and modelled helical thread
  geometry (the `modeled` flag on a thread is reserved and unimplemented).
- **CAM operations for holes:** canned drilling cycles (pecking is expanded
  into plain moves), tapping and thread milling — a hole the app derives as
  tapped has no operation that taps it.
- **Cutter radius compensation** (G41/G42) in the posts.
- **Workholding follow-ons:** generated vises and toe clamps, the spindle nose
  as a collision body, a chuck and tailstock for rotary setups, holding the
  part down on 3D operations (tabs are profile-only), and a setup sheet.
- **Other processes:** turning; laser, plasma and waterjet posts (the posts
  are milling dialects); nesting sheet blanks.
- **Procurement data:** a material has appearance, density and FEM constants
  but no cost, stock form, temper or finish, and a BOM row has no price,
  supplier, part number or lead time.

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
  designed interference reads as a clash.
- **A weld as a record** (length, size, symbol, a weld table). Welded joins
  are currently flush mates or unioned solids.
- **Fasteners:** property classes, torque and preload (`check_fasteners`
  checks length and engagement, not load); Phillips drives; a countersunk
  screw in a plain bore (refused today); inserts, rivets, dowel pins,
  circlips and spring washers.
- **Relations with two inputs:** a relation reads one mate; a delivery valve
  whose timing follows both the camshaft and the rack cannot be expressed.
- **Springs** that stretch between two occurrences and follow the motion.

## 4. Analysis

- **FEM:** thermal, buckling and nonlinear studies; modal analysis without
  supports (free-free); VTU export of results.
- **Topology optimization:** a slender bending part falls back from the fp32
  GPU solver to the CPU and runs at CPU speed; fp64 iterative refinement
  around the GPU solve is designed and unbuilt (see
  [topopt/README.md](topopt/README.md)).
- **Tolerances:** an inferred stack-up chain (`stack_up` sums a chain the
  caller states), and general-tolerance tables (ISO 2768).
- **Beam elements** for tube frames: `check_frame` checks the member graph,
  not stiffness, and FEM has only tet10 elements.

## 5. Interchange

- **PMI in STEP** (AP242 GD&T and tolerances); tolerances stay in the app.
- **IGES**, **mesh import beyond STL** (OBJ), and **3MF**.

## 6. The agent interface

- **Feature templates:** a saved feature list with parameters, instantiated as
  one composite feature. Feature types are a compiled table (`kFeatureTypes`),
  so an agent can drive every feature and define none. Logic that computes
  geometry already works from outside the app as a generator
  (`generators/gear.py`), with no determinism cost; a template is the
  in-document form, for a composite that recurs across projects. An
  interpreter inside regeneration is not the answer: it brings a sandbox, a
  pinned runtime version and a new determinism gate.
- `extract_variables`: turn a model's literals into named variables in one
  verb ([plans/extract-variables.md](plans/extract-variables.md)).
- **Design requirements stored with the project.** A generator's acceptance
  criteria live in its own script, so a later edit cannot be checked against
  them and a check run before an edit looks like one run after it. The
  direction: a persisted `Requirement` (target by stable id, bounds, source,
  revision), one result envelope for every check (`fits`, assertions,
  contact, motion) bound to the model revision it read, with `stale` and
  `unresolved` as real outcomes, and requirement edits that are explicit and
  invalidate old evidence.
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
- Benchmark tasks graded on mechanisms and relations, not only on static
  geometry and fit.

## 7. Platform and presentation

- An **MSAA setting** (the sample count is chosen automatically, capped at
  4x, and is baked into every pipeline).
- **Real-time hybrid ray tracing** in the viewport; ray tracing is the offline
  `render_still` path today. It needs `main.cpp`'s Vulkan setup factored into
  a shared context first.
- Shaded fill of sketch regions.
- Glass in the viewport: a per-body transmission override, and panes sorted
  back to front.
- **Packaging:** deb, rpm, Snap and macOS builds, and a Flathub submission
  for the existing Flatpak.

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

- Sketch text.
- NURBS surface modelling (boundary, fill, sew, thicken).
