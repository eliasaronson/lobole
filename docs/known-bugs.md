# Known bugs

Defects present in the code now. Each entry gives the symptom, the cause (or
what is known of it), where it lives, and the likely fix. Delete an entry in
the commit that fixes it, with a regression test. Missing features are not
bugs: they go in [roadmap.md](roadmap.md) or the area README's "does not do
yet".

Entries are grouped by area. Add a new one under its area's heading, creating
the heading if it is not here yet.

## Modeling

### A fillet on the second piece of a split does not build

- **Symptom:** split a body, pick an edge of the second piece
  (`query_geometry` hands back `{feature_id: <its created_by>, element}`)
  and fillet it: the fillet fails with "edge '…' no longer exists". The
  reference the app gave out is one it cannot resolve.
  `tests/reference_parts/41-split-then-fillet` is the case.
- **Cause:** every piece of a split keeps the split body's `createdBy`, and an
  `ElementRef` names a body by that id alone, so the feature binds to the
  first body carrying it and looks the name up there.
- **Where:** the body lookup in `src/core/features/FilletFeature.cpp`, and
  every feature that finds its body by `createdBy` the same way (Shell,
  Boolean, Draft and the rest).
- **Likely fix:** a body sub-ordinal on `Body` and `ElementRef` (a piece's
  names already differ: `…/b2/…`); short of that, resolve a named element
  against every body sharing the id and refuse a name that two of them carry.

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

### `query_geometry` falls over on a short vector

- **Symptom:** `query_geometry {normal: []}` (or `axis`, `near_point`, or
  any array shorter than three) is refused with nlohmann's "array index 2 is
  out of range", labelled `code: internal`, instead of naming the shape it
  wants; `normal: [0, 0, 0]` normalises to NaN.
- **Cause:** the handler's local `vec3` reads `a.at(0..2)` with no size or
  zero-length check.
- **Where:** `query_geometry` in `src/remote/RemoteInspection.cpp`.
- **Likely fix:** read them with `remote::Vec3Param` (`RemoteHelpers.h`) and
  refuse a zero direction. `tests/hostile_params_smoke.py` lists it under
  `KNOWN`; delete that entry with the fix.

## Modeling

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

### A blend face's edge names depend on what the process did before

- **Symptom:** the same design, run in an instance that has just built
  another one, names a fillet face's boundary edges differently from a
  fresh instance: a Fillet whose edge was picked as `100/g1;G110;U0` finds
  no such edge when the steps are replayed from nothing in the same
  process. `tools/fuzz.py` reports it as `leak` (seed 41 after seed 37, on
  generator 1).
- **Cause:** a U name (`<face>;U<k>`, `topo::ApplyFallbackPasses`) numbers a
  face's boundary edges in the order OpenCASCADE's blend builder hands the
  face back, and that order follows its maps keyed by shape address, so it
  moves with the process's allocation history.
  It is not only names: a design built a second time in one process
  (`tests/fuzz/cases/rebuild-139.jsonl`, a shell of a drilled box, replayed
  right after its own recorded session) came out with another volume and
  its next step's body selector missed, so the corpus and seed smokes give
  each run an instance of its own.
- **Where:** `FallbackPasses` in `src/core/ElementHistory.cpp`.
- **Likely fix:** number a face's unnamed edges in an order the geometry
  fixes (by each edge's midpoint, then its length), not the wire's order.
  That renames U edges in saved documents, so the goldens that cite one
  move with it.

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
