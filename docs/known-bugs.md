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

### OpenCASCADE reported "premature end of file" in `headless_smoke`

- **Symptom:** once, during the import-formats work, `headless_smoke`'s
  import phase printed OCC's "Error: premature end of file". Not reproduced
  since.
- **Where:** the import phase of `tests/headless_smoke.py`.
- **Likely fix:** first reproduce it (the phase in a loop under load); a
  file read while still being written is the first suspect.

### A thin Shell of the worm runs past the matrix's minute

- **Symptom:** on `golden/v146-worm`, a Shell opening the flank face
  `1/face1;M2` with the Shell tool's default wall (a tenth of the body's
  thinnest wall: 0.02 mm) fails "shell gave up after 46 s" -- after about
  96 s. The [action matrix](modeling/README.md#action-matrix) gives an
  action a minute and records it as a hang.
- **Case:** `tests/action_matrix/open/worm-shell-slow.json`
  (`tools/action_matrix.py replay` it; with `--timeout 200` it answers).
- **Cause:** the offset builder stops at its progress checks once the
  budget (`directedit::BuilderBudgetS`, 46 s for 33 faces) is spent, and on
  this body one step between two checks takes about 50 s more.
- **Where:** `ShellFeature::Execute` (`src/core/features/ShellFeature.cpp`);
  the builder is `BRepOffsetAPI_MakeThickSolid`.
- **Likely fix:** find the step (a profile of the replay) and either give
  it a progress check in our OCC patches or refuse a wall that thin for a
  worm's flank before the builder starts.

### A Shell's answer can still follow memory addresses on the patched kernel

- **Symptom:** a Shell of 0.1 mm opening `9/cap1;M12[12/g3;M12]` of the
  toolmaker's clamp's M3 retaining screw (`generator/toolmakers_clamp`)
  builds in a full action-matrix sweep and is refused ("shell produced an
  invalid solid (invalid geometry)") in that sweep's shard 3 and in a fresh
  replay. In a fresh instance two, and five, draws on copies all fail; ten
  build. `action_matrix_full_3` reads it as a pass that became a fail.
- **Cause:** the offset builder still orders its work by shape address
  where `occt-shell-cap-order.patch` does not reach (it fixes which way the
  shell's faces point, not the order: upstream's OCCT#1600), and on this
  body the order decides whether the result is valid.
- **Where:** `ShellFeature::Execute`
  (`src/core/features/ShellFeature.cpp`), `BRepOffset_MakeOffset`.
- **Likely fix:** carry OCCT#1600 in `nix/patches/`, then measure whether
  any draw beyond the first still changes an answer.

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
  moves with the process's allocation history. A fresh instance is not a
  cure, only another draw: the addresses are the allocator's, and differ
  from one process to the next as from one build to the next. The same
  mechanism moved a whole shape, not only names: the offset builder shelled
  `tests/fuzz/cases/rebuild-139.jsonl` into an invalid solid about one build
  in seven, in fresh instances as in reused ones, which is what made
  `fuzz_corpus_smoke` fail at random. That one was OpenCASCADE's quilt
  order, now patched in every build we make
  ([modeling](modeling/README.md#a-reshaping-feature-keeps-only-a-valid-solid)).
  The blend and draft builders' shapes do not: Fillet, Chamfer and Draft
  gave one volume and one set of names over 60 copies of a drilled box, ten
  cases between them. The offset builder still returns its faces in an
  address order, which made a later Draft's validity a coin toss until the
  check stopped sampling one point per cavity
  ([inspect](inspect/README.md#geometry-checks)).
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

## Windows

### A fault in system code on a worker thread leaves no crash report

- **Symptom:** a fault inside a system DLL (msvcrt's `memcpy` handed a bad
  pointer, a driver) on a `std::thread` ends the process with no `.crash`,
  and the next start reports a session that "ended from outside".
- **Cause:** mcfgthread's thread entry (`__MCF_seh_top`) ends any exception
  it does not know in `RaiseFailFastException`, below which no filter runs.
  The crash handler reports faults in code we ship at first chance, but
  leaves faults in the system's own code to the unhandled-exception filter,
  because that code may be probing memory under its own `__try`.
- **Where:** `OnFaultInShippedCode` in `src/app/CrashHandler.cpp`.
- **Likely fix:** report a first-chance fault in system code when the
  thread's next handler is mcfgthread's (walk the faulting thread's frames
  for an exception handler before reporting).
