# Known bugs

Defects present in the code now. Each entry gives the symptom, the cause (or
what is known of it), where it lives, and the likely fix. Delete an entry in
the commit that fixes it, with a regression test. Missing features are not
bugs: they go in [roadmap.md](roadmap.md) or the area README's "does not do
yet".

Entries are grouped by area. Add a new one under its area's heading, creating
the heading if it is not here yet.

## Sheet metal

### A circular corner relief on a fold that is not square leaves the flat pattern thick

- **Symptom:** at a 60° fold the flat blank comes out 1.67 mm thick on 1 mm
  sheet (2.27 mm at 45°); a rectangular relief is fine. The part passes
  `check_sheet_metal` and cannot be cut.
- **Cause:** the circular relief tool is an upright cylinder of height
  `t + radius` along the panel normal. Only the plan angle between the two
  bend lines is checked (and refused when not square); the fold angle is not.
- **Where:** `src/core/features/CornerReliefFeature.cpp`.
- **Likely fix:** refuse the circular style when a bend's fold angle is not
  90°, or size and place the tool from the bend region's real extent (or cut
  it in the flat state).

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

## Simulation

### `fem_async_smoke` fails when a solve finishes between the poll and the cancel

- **Symptom:** the smoke fails intermittently with `AssertionError: a static
  solve must refuse to cancel`; every archived failure is this assertion.
- **Cause:** the test polls `study_status`, sees `running: true`, then calls
  `cancel_study`. If the solve finishes in between, `cancel_study` finds
  nothing running and answers `{"cancelled": false}` instead of refusing. It
  also checks only the solve's status, not the mesh job's.
- **Where:** `tests/fem_async_smoke.py`; `cancel_study` in
  `src/remote/RemoteSimulation.cpp`.
- **Likely fix:** have `cancel_study` say plainly when nothing is running
  (and consider the mesh phase), and let the test accept that answer when the
  study has since finished.

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

`describe` is what an agent reads instead of the code, so a stale row is a
wrong contract.

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
  (`$XDG_STATE_HOME/designer/gui-failures/<run>/reproduce.sh`) and bisect
  it; the test that precedes the failure is the suspect.
