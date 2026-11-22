# Known bugs

Defects present in the code now. Each entry gives the symptom, the cause (or
what is known of it), where it lives, and the likely fix. Delete an entry in
the commit that fixes it, with a regression test. Missing features are not
bugs: they go in [roadmap.md](roadmap.md) or the area README's "does not do
yet".

Entries are grouped by area. Add a new one under its area's heading, creating
the heading if it is not here yet.

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

### A blend along a curved edge is off its rolling-ball volume

- **Symptom:** a Fillet or Chamfer whose edge is an intersection curve or
  ends on a curved face measures up to 6e-4 off the closed form the case
  derives, where one along a straight or circular edge agrees to 1e-9: the
  rim of a tube bent through an arc (5e-5), a plane cutting a cylinder or
  a cone obliquely (2e-5, 4e-6), a round laid over a hole (3e-4), a tray's
  floor and three of its corners (1e-4), a chamfer where a pipe meets a
  plate (6e-4), a tapered extrude of arcs meeting at corners (one face
  more).
- **Cause:** not established case by case, and not the measure:
  [the face-by-face integrator](inspect/README.md#the-face-by-face-integrator),
  which checks every face the fixed Gauss rule can get wrong, moves none of
  these cases by more than 1e-8 of its volume (the pipe chamfer 40264.4913,
  the round over a hole 7582.6448, the tray 145524.7077, the bent tube's
  rim 131361.8716). OpenCASCADE's blend surface on
  such an edge is an approximation to its own tolerance, and where rounds
  meet at a corner it builds a patch of its own; neither is the rolling
  ball's envelope.
- **Where:** `src/core/features/FilletFeature.cpp`;
  `ExtrudeFeature::TaperPrism` for the tapered extrude.
- **Likely fix:** per case: tighten the blend's approximation tolerance
  where that closes the gap, and write down where the corner patch differs
  by construction.
- **Cases:** `solidcraft/fillets/1213-round-end-circle-of-bent-tube`,
  `cov-fillet-exact-oblique-plane-cylinder-rim-r2`,
  `cov-fillet-exact-oblique-plane-cone-rim-r1-5`,
  `324-fillet-over-hole-overlap`,
  `324-fillet-over-hole-cut-by-overlong-cylinder`,
  `1008-fillet-tray-floor-and-three-of-four-corners`,
  `cov-fillet-section-chamfer-pipe-on-plate-2`,
  `solidcraft/booleans/2810-taper-extrude-arcs-meeting-at-corners`,
  `solidcore/fillets/round-slot-back-wall-across-a-rounded-groove-r0-9`
  (3e-8 of 7931.9, a round along a tangent chain over a groove's round),
  `solidcraft/threads/3368-faceted-prism-with-bore-and-cross-hole` (8e-5,
  a faceted prism with a bore: no blend, kept here until it is looked at).

### A blend beside a plane an add left in two pieces is refused at every size

- **Symptom:** a Fillet or Chamfer on an edge that ends on, or runs beside,
  a plane an `add` left as two coplanar faces is refused whatever its size,
  and the size its refusal names does not fit either. An L joined from two
  boxes, its wall's edge alone: "no radius builds on these edges, down to
  0.00257 mm", where the same L extruded from one profile rounds R2.5 and
  R3.5 to their closed forms (9463.66702, 9409.58736). A cylinder joined
  flush with a bar along its bottom, its bottom rim chamfered: "the largest
  distance that fits is 0.121 mm", where 7.5 builds once the bottom is one
  face. An L bracket's front inner outline rounded after its bend round
  builds 13 faces where the case counts 12.
- **Cause:** an `add` keeps the coplanar faces it joined apart until a
  Refine (the operation table's `Refine` row), and OpenCASCADE's blend
  builder fails, or builds a patch more, where the blend's end or its run
  meets the edge between two pieces of one plane. The size search then
  reads the builder's failure at every size as the size's. Every case
  below passes when a Refine follows each `add` (an experiment on the
  corpus; the translator does not do it), except where noted.
- **Where:** `ApplyBodyMode` (`src/core/BodyMode.cpp`) leaves the faces;
  `EdgeBlendNamed` (`src/core/ElementHistory.cpp`) blends on them.
- **Likely fix:** merge the coplanar faces next to the picked edges on the
  blend's copy before the builder (`ShapeUpgrade_UnifySameDomain`, names
  through its history as `RefineFeature` keeps them), or merge them in the
  `add` itself. Either moves element names, so it is a change of its own.
- **Cases:** `solidcore/fillets/12342-round-wall-and-ledge-edges-r2-5`,
  `solidcore/fillets/12342-round-wall-edge-r5-past-the-wall-refused` (at
  R5 still [not built](modeling/README.md#not-built) once merged: past
  its 4 mm wall), `solidcore/fillets/14755-round-between-cone-and-plane-chamfers`
  (its rim chamfer; the bar's chamfers after it take two distances and are
  not translated), `solidcraft/fillets/556-fillet-edge-ending-on-curved-boss`,
  `851-chamfer-arc-and-top-front-2`, `851-chamfer-arc-top-edge-9-5`,
  `851-fillet-top-front-and-front-left-upright`,
  `851-fillet-top-front-then-front-left-upright`,
  `851-fillet-top-front-then-upright`,
  `every-l-bracket-both-bends-r3-r8-concentric`,
  `every-l-bracket-outside-bend-r5`,
  `every-l-bracket-front-inner-outline-after-bend-round-r1` (the face more).

### A round runs on across a crease of a few degrees

- **Symptom:** a Fillet of one edge also rounds the edge beyond a crease
  of up to about five degrees, which nobody picked. A block whose bottom
  turns up 3° at x = 0.8, its bottom side edge's long piece rounded R2.6,
  comes back with both pieces round: 459.17945 mm³ where the short piece
  left sharp gives 459.94891. On the same block a crease of 1° or 3° is
  crossed, one of 6°, 10° or 20° is not.
- **Cause:** OpenCASCADE's blend builder adds an edge's tangent chain, and
  it reads a crease under its own angular tolerance as tangent; nothing of
  ours compares the contour it made with the edges picked.
- **Where:** `AddBlendEdges` (`src/core/ElementHistory.cpp`).
- **Likely fix:** after `Add`, walk the builder's contour
  (`NbEdges`/`Edge`) and refuse, naming the crease and its angle, a
  contour that crossed a vertex where the faces meet at more than the
  modeller's own tangency (`ChFi3d::IsTangentFaces` at a tighter angle), or
  give the builder that tolerance (`SetParams`).
- **Case:**
  `solidcore/fillets/15437-fillet-round-ends-at-crease-beside-drafted-side`.

### A blend the validity check passes can be far off in volume

- **Symptom:** the size a refused Fillet names as the one that fits builds
  a body `check_geometry` passes with a volume nowhere near a round's. Two
  equal tubes in a tee, their whole joint at R2: refused, "the largest
  radius that fits is 0.143 mm"; R0.143 builds 76595.87 mm³ on a union of
  75823.00, 773 mm³ added where a round of that size adds under 1. One of
  the joint's half ellipses alone at R1 and R0.143 builds about 750 mm³
  too many, also valid; at R2 and R0.1 it adds 16.1 and 0.042, as it
  should.
- **Cause:** not established. OpenCASCADE's builder returns a solid whose
  faces pass BRepCheck but enclose more than the union and the round; the
  validity check (`Measure::InvalidReasons`) is all the size search and the
  feature ask of a result.
- **Where:** `FilletFeature`; the size search
  ([the size that fits](modeling/README.md#the-size-that-fits)).
- **Likely fix:** hold a blend's result to the volume a blend of its size
  can move (the union's volume plus or minus the edges' length times the
  section r² (1 - pi/4), doubled for curvature) before it is kept or named
  as the size that fits.
- **Case:** `solidcore/fillets/16654-fillet-equal-tube-tee-pinched-at-the-sides-r2`
  (its row is the refusal at R2; the wrong volume is at the size named).

### A cavity touching the outer skin along a line is one shell with it

- **Symptom:** a box less a cylinder that lies inside it and touches a wall
  along a line (a cavity) has the right volume and passes
  `check_geometry`, and its mesh is one connected piece where the case
  counts two shells: the cavity's wall and the box's share the line's
  edge.
- **Cause:** the boolean keeps the tangent line as an edge both shells
  use; nothing splits the result into an outer shell and a void that only
  touch.
- **Where:** `ApplyBodyMode`, `src/core/BodyMode.cpp`.
- **Likely fix:** none needed for volume; a consumer that needs manifold
  shells (a mesh export, a print check) has to split at such an edge. A
  split has to do two things to count as two shells here: give the cavity
  a shell of its own, and take the tangent line out of the outer wall's
  face, where it is an edge the wall's triangles end on, so the wall's mesh
  and the cavity's no longer share it.
- **Cases:** `solidcore/booleans/17439-cylinder-cavity-touching-a-wall-along-a-line`,
  `17439-cylinder-cavity-along-x-touching-the-floor`,
  `17439-cylinder-cavity-touching-two-walls-along-lines`.

## Interchange

### An IGES box whose faces miss by more than 1/10000 of its size comes in open

- **Symptom:** a box of six trimmed planes whose edge stops 0.01 mm short
  of its corner in both faces that use it comes in as a shell with free
  edges (named by the import's check), not the closed 6000 mm³ box; an
  IGES surface of revolution trimmed at its rim over a flat disc (a dome)
  comes in with one free edge and is skipped as a surface.
- **Cause:** the faces are sewn at the file's resolution and then at
  1/10000 of the part's diagonal (`SewProduct`); a 0.01 mm gap on a 37 mm
  box is past both. The dome's one free edge is not traced yet: its rim
  and the disc's circle are not sewn into one, or the revolved face's seam
  is used once.
- **Where:** `SewProduct`, `src/cad/CADKernel.cpp`.
- **Likely fix:** a third sewing at the gap the open shell's free edges
  actually leave, when every free edge can close at it and that is still
  small against the part; the dome first needs its free edge named.
- **Cases:** `solidcraft/import/3845-iges-trim-pieces-miss-by-a-gap`,
  `solidcraft/import/3806-iges-revolved-arc-spherical-cap-dome`.

## Sketching

### A huge length edit refuses a shape that rides the line's end

- **Symptom:** a regular hexagon inscribed in a construction circle centred
  on the end of a line 100 long follows the line to 400, but driven to
  10000 the edit is refused, "would collapse line N to a point", where the
  hexagon could simply translate with the line's end.
- **Cause:** every solve starts from the pre-edit geometry, and the cheapest
  least-squares answer for points left 9900 mm behind is to slide them all
  to the circle's nearest point. Continuation steps a sixty-fourth at a
  time within 160 solves, and each step is still too far for the hexagon;
  the collapse guard then refuses it rather than keep it collapsed
  (docs/sketching/README.md#solving).
- **Where:** `Solver::SolveValueEdit`, `src/core/sketch/SketchSolver.cpp`.
- **Likely fix:** seed each step better: move a point held on a circle (or
  coincident with a moving point) with the circle's centre before the
  solve, as SolidCraft's 20cf1644f does; or finer steps when the budget
  allows.

## Performance

### The bench's big mesh rows are priced as faceted imports

- **Symptom:** `tools/bench.py run --set mesh` plans the 300k and 2M rows
  at an hour each (`estimate_s`, `setup_s` 3600), so `--dry-run` overstates
  the set's cost many times over.
- **Cause:** the rows were priced when a mesh imported as a face a
  triangle; under `auto` they are mesh bodies now, which import a 2M
  figure in seconds ([mesh bodies](interchange/README.md#mesh-bodies)).
- **Where:** `_mesh_rows` in `tools/bench_workloads.py`.
- **Likely fix:** price them from a measured run of each row as a mesh
  body instead of the faceted estimate.

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
  (`$XDG_STATE_HOME/lobole/gui-failures/<run>/reproduce.sh`, or a worktree's
  `build/gui-failures/<run>/`) and bisect
  it; the test that precedes the failure is the suspect.
