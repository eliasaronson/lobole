# Known bugs

Defects present in the code now. Each entry gives the symptom, the cause (or
what is known of it), where it lives, and the likely fix. Delete an entry in
the commit that fixes it, with a regression test. Missing features are not
bugs: they go in [roadmap.md](roadmap.md) or the area README's "does not do
yet".

Entries are grouped by area. Add a new one under its area's heading, creating
the heading if it is not here yet.

## Interchange

### An ASCII STL of whole-number coordinates is recognised at 4 mm

- **Symptom:** an ASCII STL that prints its corners as integers (`vertex 2 3
  4`) is read at a precision of 1 mm, so `recognize` fits its surfaces to
  four quanta, 4 mm: a 2 x 3 x 4 box rebuilds with the right volume
  (24 mm³) but tolerances that make `get_state`'s box 10 x 11 x 12.
- **Cause:** `mesh::ReadStlBytes` takes the place value of the last printed
  digit as the precision (`10^(floor(log10(largest)) + 1 - digits)`), which
  for one significant digit is 1.
- **Where:** `ReadStlBytes` in `src/core/mesh/MeshReaders.cpp`.
- **Likely fix:** floor the precision a whole-number file implies at what
  its exporter could mean, or take the coarser of the printed digit and the
  part's own scale; a regression test of an integer-cornered box
  recognised with its box within a micron.

## Inspection

### `interference` reports a phantom clash at an exactly symmetric pose

- **Symptom:** two meshing gears read *touching* at every pose of a tooth
  pitch except the one where a tooth sits exactly centred in the gap, flanks
  tangent, where they read *clash* with about a whole tooth's volume
  (162 mm³ on an m2 pair); 0.005 rad either side they read touching again. A
  script that sweeps poses and takes the worst case reports a clash.
- **Cause:** the common boolean already runs with the contact fuzz
  (`kContactFuzz`), but at coincident tangent faces it still returns a solid;
  `ContactState` calls any common volume above 1e-9 a clash, and `touch_tol`
  is compared only against distances, never against the common solid.
- **Where:** `InterferenceVolume` and `ContactBetween` in
  `src/core/Measure.cpp`; `ContactState` in `src/remote/RemoteInspection.cpp`.
- **Likely fix:** before calling a common solid a clash, check it is real:
  every face of it within `touch_tol` of a face of both inputs is a touch; or
  re-run with the pose nudged by ±ε and treat a volume that vanishes as
  touching.

### `get_state` and the digest report a STEP's box several times its size

- **Symptom:** the SO-101 wrist bracket (`STEP/SO101/Wrist_Roll_Pitch_SO101.step`
  in TheRobotStudio/SO-ARM100), imported with `keep_origin`, is 62.3 x 35.7 x
  77.8 mm by `mass_properties`; `get_state`'s `bodies[].bbox_*` and the
  `digest` every mutating reply carries say 164 x 345 x 156. A script reading
  either for the part's size or a placement is off by a factor of five; the
  help says the bound "can read ~60% over".
- **Cause:** both are `CADShape::GetBoundingBox` without `tight`, which
  bounds a B-spline face by its control points; an Autodesk export's faces
  are B-splines.
- **Where:** `BodyDigestJson` in `src/remote/RemoteHelpers.h`, the body rows
  of `get_state` (`src/remote/RemoteDispatch.cpp`).
- **Likely fix:** report the tight box (memoized per shape, so paid once per
  regeneration), or name the field what it is (`bound_*`) and point at
  `mass_properties`. The camera fit had the same cause and takes the tight
  box now.

### `check_print` passes a long bridge on a raw STL import

- **Symptom:** the SO-101 bracket exported as STL and imported again, end
  face down: `check_print`'s `bridges` passes, its worst span 7.78 mm. The
  same body after a `Refine` fails with a 15.38 mm span, as the B-Rep it was
  exported from does.
- **Cause:** an STL arrives as one planar face per triangle, and the item
  takes each downward face's inscribed diameter on its own: a ceiling of
  twenty triangles is twenty short spans.
- **Where:** the bridges item in `src/core/PrintCheck.cpp`
  ([checking a print](tolerances/README.md#checking-a-print)).
- **Likely fix:** merge coplanar neighbours first (`ShapeUpgrade_UnifySameDomain`,
  what Refine runs) on the faces the item considers, or measure the span of
  each connected set of ceiling faces.

## Modeling

### Sheet metal: 6061-T6's bend radius is unsourced

- **Symptom:** a sheet of `aluminum_6061_t6` bends at 1.5 t by default and
  takes the 0.41/0.46/0.50 K bands, numbers the table carried for the
  generic aluminium row before it left the library (a 5052-class sheet).
  6061-T6 cracks sooner than 5052 at a tight radius, and its minimum grows
  with thickness, so the default may be too tight for a thick 6061-T6
  sheet.
- **Cause:** the generic row's sheet data moved to the only aluminium sheet
  grade the library has; no 6061-T6 bend table is cited.
- **Where:** `MinRadiusMultiple` and `BandsFor` in `src/core/SheetMetal.cpp`.
- **Likely fix:** cite a 6061-T6 bend-radius table by thickness (the
  Aluminum Association's), or add a 5052-H32 row with cited data and move
  the sheet table to it.

### A cylinder added flush with the tangent of another leaves a free edge

- **Symptom:** a rod (a Ø6 cylinder along Z) added to a foot (a cylinder
  along X whose end faces sit at the rod's ±3 mm, tangent to the rod's
  round) regenerates and `check_geometry` says *verified*, but
  `check_print`'s `shells` fails it: *not watertight*. Lengthening the foot
  0.4 mm past the rod each side passes. The woodcutter's rods hit it
  (generators/woodcutter/woodcutter_parts.py, `build_rod`, which now
  lengthens the foot).
- **Cause:** not yet looked at. The shells item's watertightness is a
  B-Rep free-edge scan (`Measure::AnalyzeWatertight`), so the fuse's
  result has an edge used by one face, where the plane meets the cylinder
  along a line of tangency; `check_geometry`'s checks do not count free
  edges.
- **Where:** the Extrude's `add` boolean (`src/core/features/`), and
  `check_geometry`, which should not verify a solid with a free edge.
- **Likely fix:** first make `check_geometry` report free edges, so the two
  checks agree; then see whether `ShapeFix` or a fuzzy fuse closes the
  tangent seam.

### Two Shells of the clamp's screw are refused on every draw

- **Symptom:** on the toolmaker's clamp's M3 retaining screw
  (`generator/toolmakers_clamp`), the Shell tool's 0.05 mm wall opening
  `12/g1;M12` or `9/cap1;M12[12/g3;M12]` fails "shell failed (wall too
  thick for the geometry?)", on every copy and in every process. Before
  `occt-offset-arc-bind-order.patch` both built on about a third of 100
  draws, and the action matrix's baseline had them as passes; it now has
  them as fails.
- **Cause:** the patch fixes the order the arc-join offset registers its
  offset faces in, which used to follow shape addresses; whether the
  intersections after it succeed on this body depends on that order, and
  the order now fixed is one where they fail. Upstream's OCCT#1600 reports
  the same of one input (a fuse of two boxes), and that reversing the order
  built it.
- **Where:** `BRepOffset_MakeOffset` (`nix/patches/occt-offset-arc-bind-order.patch`);
  `ShellFeature::Execute` (`src/core/features/ShellFeature.cpp`), which now
  draws once on a kernel carrying the patch.
- **Kept as it is:** the same refusal every time is better than a coin
  toss, and a second fixed order (the reverse of the binding order) would
  cost a second build of every honest refusal. The fix belongs upstream, in
  the offset's intersections, not in the order.

### A boundary edge's U name follows the result's face order

- **Symptom:** an edge both of whose faces are named takes its U name
  (`<face>;U<k>`) from whichever face comes first in the result's face
  order, and a builder can hand its faces back in shape-address order, so
  in another process the same edge can carry the other face's name. A
  reference saved with one name then finds no edge: the feature fails with
  "no longer exists" (the lookup is an exact match for a U name, and an
  edge's place in its face's order is held for it whether or not that face
  names it, so the missing name is never given to another edge). Separately,
  an edit that moves one edge's midpoint past a sibling's swaps the two
  edges' numbers, and that reference does resolve, to the other edge.
- **Cause:** the U pass numbers a face's edges by their geometry
  ([toponaming](modeling/toponaming.md#how-each-operation-names-its-output))
  but walks the faces in index order, which is the kernel's.
- **Where:** `FallbackPasses` in `src/core/ElementHistory.cpp`.
- **Likely fix:** pick the naming face by a fixed rule. Not by face name: a
  first attempt sorted the faces by name, and "A dangling edge ref errors
  the fillet with reattach info" failed: a hole's rim edge, named from the
  hole's bore before, became a U name of the cap face, and once the hole was
  deleted that cap-face name resolved to one of the cap's outer edges
  instead of failing. A rule that prefers the face with fewer boundary
  edges (the bore over the cap) keeps that case honest; it renames U edges
  in saved documents, so it is a format change.

### A small sphere cut beside a fillet keeps a face inside the sphere

- **Symptom:** a half revolve (a 10 x 20 section through 180 degrees),
  two of its edges filleted r2.79, then a d2.5 Sphere cut on its flat face
  next to the blend: the exact cut and the fuzzy retry both leave a face of
  the body INSIDE the sphere, and the boolean check refuses the cut ("made
  body 'Base' WRONG though it passes BRepCheck").
- **Case:** `tests/fuzz/open/boolean_failed-779.jsonl` (`tools/fuzz.py
  replay` it; it moves to `tests/fuzz/cases/` with the fix).
- **Cause:** OpenCASCADE's cut, where the sphere's seam meets the fillet's
  surface; the check is right to refuse it.
- **Where:** `CheckedBooleanNamed` in `src/core/BodyMode.cpp` is where a
  remedy would go (a retry with the tool turned about its axis, so the seam
  falls elsewhere).

### The measured box can stand off a cut sphere

- **Symptom:** a box cut by a Sphere and that cut patterned round the Z axis:
  `mass_properties` reports the body's `bbox_min.y` as -25.7 where every
  point of it (its mesh) is within -21.25. The box called MEASURED is not.
  `tools/fuzz.py` reports it as `wrong_way` ("a pattern circular grew the
  bbox").
- **Case:** `tests/fuzz/open/wrong_way-373.jsonl`.
- **Cause:** `BRepBndLib::AddOptimal` without a triangulation bounds a
  trimmed spherical face by more of the sphere than the face keeps.
- **Where:** `CADShape::GetBoundingBox(tight)` in `src/cad/CADKernel.cpp`.
- **Likely fix:** for a face on an elementary surface, bound the face's own
  boundary and its interior extrema, or tighten against a fine mesh.

## Rendering

### The remote is silent while a Wayland window is not on screen

- **Symptom:** a windowed instance on a Wayland session (the human's
  `dev-watch`, on sway) answers nothing while its window is minimised, in
  the scratchpad or on a workspace not on screen; every call waits until
  the window is shown. The log says `main thread stalled … (in presenting:
  compositor sends no frames to a hidden window)`, then `resumed after`.
- **Cause:** the swapchain is FIFO, and Mesa's Wayland FIFO present blocks
  for the compositor's frame callback, which a compositor does not send to
  a surface it is not showing. Wayland does not tell a client it is hidden,
  so the loop cannot skip the present as it does for a minimised X11 window.
  Reproduced on a nested headless sway (`WLR_BACKENDS=headless`, the window
  moved to the scratchpad): the stall lasts exactly as long as it is hidden.
- **Where:** `FramePresent` in `src/app/main.cpp`; the present mode in
  `src/app/GpuOwners.cpp`.
- **Likely fix:** a present the loop can bound: `VK_KHR_present_wait` with
  a timeout on the previous present before the next one, or MAILBOX paced
  by the loop itself on Wayland, skipping the frame while the last one has
  not been shown.

## Simulation

### The steel row's modulus is not its cited clause's

- **Symptom:** the library's `steel` row has E = 200 GPa; the clause it
  cites for its expansion, EN 1993-1-1:2005 §3.2.6, gives E = 210 000
  N/mm² (and ν = 0.3). Every static, modal and buckling answer on library
  steel is 5 % softer than the clause it names.
- **Cause:** the modulus was entered as a handbook value before the row was
  cited, and the [plasticity] hardening curve (H = E/99, EN 1993-1-5
  C.6(2)) and its closed forms were written from it.
- **Where:** `elastic(MaterialType::STEEL, ...)` in
  `src/cad/CADMaterials.cpp`.
- **Likely fix:** take 210 GPa with its clause, and move the [plasticity],
  [thermal] and [fem] closed forms that read 200 with it (they read the
  library, so most move by themselves; the cases that state 200 by hand do
  not).

### A contact's weak springs hold a soft part instead of its contact

- **Symptom:** a 0.5 mm 1.4301 spring clip held only by its contact with a
  rigid vessel (the tea alarm's clip, pulled onto the mug's wall by its
  inner leaf) is refused: "the weak springs that hold a part while its
  contact closes carry 62.1 % of the load". It was 11 minutes of ccx
  before the refusal.
- **Cause:** the weak springs are `1e-6 E L` summed over the part's nodes
  (`kStabilization`), a stiffness set by the modulus and the part's size,
  not by the part's own stiffness. A thin spring is far softer than that
  (the clip moves 3 mm under 0.76 N; the springs give 11 N/mm), so the
  springs, not the contact, carry the load.
- **Where:** `fem::SettleSprings` and the stabilization in
  `src/core/fem/Calculix.cpp`.
- **Likely fix:** scale the weak springs from the part's own stiffness (a
  fraction of the stiffest support-free mode's, or of the assembled
  diagonal's mean) rather than `E L`, and keep the equilibrium refusal.
  Until then the generator holds the clip's inner leaf as the rod does and
  presses the jaw onto the wall (generators/tea_alarm/README.md).

### A variable-driven force reads 0.0 N in the study panel

- **Symptom:** in a windowed instance, `new_project`, `set_variable load_n
  10000`, a box, a static study with `add_load type: force, magnitude:
  "load_n"`, then `enter_simulation`: the panel's force row, and the
  viewport's load label, read `0.0 N`, while `list_studies` reports
  `magnitude: 10000` and a run solves at 10 kN. The FIRST such project in a
  fresh instance reads 10000.0 N; a second one built the same way in the
  same instance reads 0.0 N.
- **Cause:** not found. The row prints the load's stored `magnitude`
  (`DrawStudyPanel`'s load list), which on the second project has not been
  evaluated from its expression when the panel draws.
- **Where:** the load rows in `src/ui/StudyPanel.cpp`; the evaluation of a
  load's magnitude expression.
- **Likely fix:** show the value the expression evaluates to where the row
  is drawn (as `list_studies` does), with a GUI test that builds the study
  twice in one instance.

### A diverging solve is reported as an unconstrained part

- **Symptom:** a 60×20×2 mm steel plate clamped on one end, meshed at 0.4 mm
  (about 500 k DOF), is refused as "insufficiently constrained (singular)".
  The CPU iterative solve's residual grew to 4.8 and then 21.7: it diverged,
  it did not stall. The same plate at 0.75 mm solves.
  The tea alarm's 0.5 mm stainless clip, meshed at its thickness, solved
  in-house (on the GPU) in two runs and in a third was refused the same
  way, the CPU's iterative solve stalled at a relative residual of 0.95
  after 2001 iterations. The generator now meshes the clip's bends at half
  the sheet.
- **Cause:** not found. The refusal reads a failed iterative solve as a
  singular matrix, whatever made it fail.
- **Where:** the iterative path in `src/core/fem/FemSolver.cpp`.
- **Likely fix:** tell the two apart. A diverging residual is refused as a
  solver failure that names the residual; "singular" is kept for a matrix
  that is singular.

### CalculiX does not balance the in-house stiffness on a thin bent sheet

- **Symptom:** a linear static study of the tea alarm's clip (0.5 mm
  1.4301 sheet, two 90 degree bends at R 0.75, meshed at 0.5 mm with the
  bends at 0.25) solved on CalculiX is refused: "|u . (K u - f)| / |u . f|
  = 0.000042, above 1e-6 plus the 0.000002 its printed precision accounts
  for, so the deck and the study it was written from differ". The same
  study in-house solves, and its tip moves within 1 % of beam theory.
- **Cause:** not found. The likeliest is an element ccx integrates
  differently from the in-house code: a tet10 whose curved midside nodes
  on a bend's tight cylinder distort it (the in-house code integrates
  |det J|; ccx refuses a negative one, but a small positive one it
  integrates as it is).
- **Where:** the energy check in `src/core/fem/Calculix.cpp`; the mesh
  from `src/core/fem/FemMesher.cpp`.
- **Likely fix:** report the worst element's Jacobian ratio with the
  refusal, and refuse (or re-mesh straight-sided) elements below a bound
  before either solver sees them.

### A pin on a short sleeve does not settle when the load tilts the part

- **Symptom:** the woodcutter's left bushing (a Ø14.8 x 6.2 mm sleeve
  under a 2 mm flange), on a `pin` over its sleeve and loaded through its
  bore by a remote force 22 mm off the sleeve's middle (35 N), is refused:
  "the pins' contact did not settle: it still pulls on the part" at a
  1.6 mm mesh, "did not settle in 30 iterations" at 1.2 mm. The same force
  through the sleeve's middle (no tilt) solves in 3 s, and so does the same
  statics held the other way round: a pin over its bore, the reaction on
  its sleeve (what generators/woodcutter does). The woodcutter's camshaft,
  on pins over its two bushings' bores and leaned on at the crank, solves
  at its generator's mesh and fails its first `converge` refinement the
  same way ("did not settle in 30 iterations").
- **Cause:** not found. The tilt presses the sleeve at its two ends on
  opposite sides; the facet-wise contact set seems to cycle between them.
- **Where:** `SolveWithPins` in `src/core/fem/FemStudy.cpp`.
- **Likely fix:** first a regression case (a short tube pinned on its
  outside, a remote load off its end); then see whether the set cycles or
  ends on a pull above the thousandth.

### The GPU solve's failures are hidden, and a synchronous run never tries it

- **Symptom:** on the same 500 k DOF plate:
  - a synchronous `run_study` went straight to the CPU solve, while an
    async one tried the GPU first;
  - on an identical mesh, the async GPU solve failed 2 times (plus one
    probable) in 7, on a busy machine;
  - the reason the GPU gave up is dropped from the error;
  - `study_status.iterations` reads 0 during a GPU solve.
- **Cause:** not found.
- **Where:** the solver choice in `src/core/fem/FemStudy.cpp`, and the
  femgpu solve it calls.
- **Likely fix:**
  - one solver choice for both paths;
  - carry the GPU's reason into the result's `solver` text and into the
    error;
  - report the GPU's iteration count;
  - then find the intermittent failure with the failing mesh.

## Scripting

### A remote `item_click` does not leave a text field focused

- **Symptom:** a script cannot click a text field and then send keys: the
  keys go nowhere. A transient popup that exists only while a field is
  focused (expression completion, an in-progress tool prompt) cannot be
  screenshotted.
- **Cause:** every `ui_action` is queued, run and unwound before the RPC
  returns, so focus does not survive to the next call. `key_press` sends one
  chord; `item_input` (click, type, Enter in one action) is the only way to
  type.
- **Where:** `src/remote/RemoteUiActions.cpp`.
- **Likely fix:** a `key_chars` action (the test engine's
  `KeyCharsAppend`), or an `item_click` variant that keeps the field active.

### mypy before 2.1 reads a partial feature dict as a CamProfile spec

- **Symptom:** under mypy 1.9 (Ubuntu 24.04's apt package),
  `app.add_feature(feature={"type": "Box"})` is an error naming
  `Literal['CamProfile']`, where the documented promise is that a plain dict
  passes and only a spec annotated as one is checked
  ([rpc.md](scripting/rpc.md#typed-feature-specs)). A complete spec of any
  type passes.
- **Cause:** `CamProfileSpec` is the one generated spec whose only required
  key is `type`, because a cam is placed by `face` OR `frame` and the
  contract grammar can say only that each is optional. mypy 1.9 picks a
  TypedDict from a union by keys alone, and a dict whose keys fit exactly one
  spec is held to it; mypy 2.1 also matches the `type` literal. CI's typing
  gate runs mypy 2.1, as the nix shell does.
- **Where:** CamProfile's `contract` in `kFeatureTypes`
  (`src/core/io/SerializeFeatures.cpp`); the grammar in
  `src/remote/RpcContract.cpp` and `tools/rpc_contract.py`.
- **Likely fix:** a "one of these keys" form in the contract grammar, which
  the generator emits as one TypedDict per alternative and the schema as
  `oneOf`; the server then refuses a cam with neither or both at the call.

## Reader fuzz

### Which cases the full sweep runs depends on the machine's speed

- **Symptom:** two runs of one family's every-case sweep at the same seed run
  different case sets: `part directories 4/4` ran 13 603 cases on a quiet
  machine and 13 238 under load. A case that failed in one run may not run
  in the next, so "passes alone" can mean "was not run".
- **Cause:** an input whose unmutated read costs 0.25 s of CPU or more is
  `heavy` and runs an eighth of its cases; the cost is measured once, per
  run, and `v198-cam-profile`'s rebuild measured 0.14 s to 0.38 s across
  runs on this hybrid machine, so it lands on either side.
- **Where:** `heavy` in `Sweep::Impl::RunDocument`, `tests/mutate/ReaderFuzz.cpp`.
- **Likely fix:** decide `heavy` from the input rather than a clock (the
  corpus entry's features, or a list kept with the corpus), so a seed names
  one case set.

## GUI suite

### Viewport picks resolve nothing after some neighbours in a shuffled run

- **Symptom:** `thread/face_pick_fills_the_slot` fails in some shuffled
  orders at `!th->face.name.empty()`: the click lands on the cylinder and
  resolves no face. `assembly/a_fastener_is_placed_by_picking_a_rim` fails the
  same way (`rimElement.IsSet()`). Both pass alone and in most orders.
- **Cause:** unknown. The shape — a viewport pick that resolves nothing
  after some other test has run — points at state the per-test reset does not
  cover (the viewport rectangle or projection), not at threads or fasteners.
- **Where:** `src/app/guitests/GuiTestsModeling.cpp`,
  `src/app/guitests/GuiTestsAssembly.cpp`; the per-test reset
  (`ResetAppForTest`).
- **Likely fix:** replay an archived failing order
  (`$XDG_STATE_HOME/lobole/gui-failures/<run>/reproduce.sh`) and bisect
  it; the test that precedes the failure is the suspect.
