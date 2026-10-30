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

### An extrusion of a lone closed spline weighs wrong

- **Symptom:** a profile that is one B-spline closed on itself (first pole
  equal to last) extrudes to a solid whose volume `BRepGProp` reads about
  1.5% low or high, and differently with an adaptive tolerance: an
  ellipse-like closed spline of 1131 mm² read 1113 mm³ a millimetre tall
  (fixed Gauss) and 1013 (adaptive). The same curve split into two splines
  that meet at both ends reads 1131.
- **Cause:** not pinned down. The face is built from one clamped,
  non-periodic edge closed on its own vertex (`MakeLoopWire`'s lone-spline
  branch, then `ShapeFix_Wire`); the integration over that face's domain
  is what goes wrong, not the curve (its shoelace area is right).
- **Where:** `MakeLoopWire` in `src/core/sketch/SketchProfile.cpp`.
- **Likely fix:** build the lone closed spline as two edges split at its
  middle parameter, sharing both vertices; a regression test extruding a
  closed spline against its shoelace area. A section's spline fit already
  draws a closed cut as two splines for this reason
  (`sketch::FitSplines`). Changing it renames the side faces of every
  saved extrusion of a closed spline (one edge becomes two), so it needs
  a look at what refers to them.

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
