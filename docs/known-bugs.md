# Known bugs

Defects present in the code now. Each entry gives the symptom, the cause (or
what is known of it), where it lives, and the likely fix. Delete an entry in
the commit that fixes it, with a regression test. Missing features are not
bugs: they go in [roadmap.md](roadmap.md) or the area README's "does not do
yet".

Entries are grouped by area. Add a new one under its area's heading, creating
the heading if it is not here yet.

## Inspection

### The band-cache window may miss a reach that its margin now covers

- **Symptom:** none visible today. Dropping the cached insertion branch's
  "as much again either side" margin made the clock's bob step ~13x cheaper,
  but changed the stage-2 "centre spacer" step's answer from a 5.7 mm travel
  with a confirm to travel 0 and no confirm. A correct window would not change
  an answer, so the margin is likely hiding an edge case.
- **Cause:** not known; somewhere in the band cache's "covers" / window logic.
- **Where:** the cached branch of `Measure::Insertion` in
  `src/core/MeasureInsertion.cpp`.
- **Likely fix:** reproduce on the clock's saved stage-2 project with the
  margin removed, find which bands the spacer step loses, fix the window, then
  drop the margin (the bob step goes from ~13 s to ~1 s).

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

### A very thin Shell of random design 17 faults in the offset builder

- **Symptom:** a 0.002 mm Shell of random design 17 (fuzz generator 1),
  opening the blend face `100/g5;U0;G110;M120`, fails "shell failed inside
  OpenCASCADE (OSD_SIGSEGV ...)". The signal is caught and the edit refused,
  so the app lives; the action matrix records it as a `signal` finding
  (`tests/action_matrix/open/fuzz17-shell-signal.json`).
- **Cause:** a null dereference in `BOPDS_DS::ShapeInfo`, called from
  `BRepOffset_Tool::Inter3D` under `BRepOffset_Inter3d::ConnexIntByArc`
  (`BRepOffset_MakeOffset::Intersection3D`, `MakeThickSolidByJoin`, from
  `ShellOnce`). That is the pave filler our
  `nix/patches/occt-offset-inter3d-progress.patch` hands a progress range;
  whether stock OCCT faults the same way is not checked.
- **Where:** OCCT's `BRepOffset_Tool::Inter3D`; `ShellFeature::Execute`.
- **Likely fix:** check the pave filler's errors before `Inter3D` reads its
  data structure, in the patch if it is ours, else upstream.

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
