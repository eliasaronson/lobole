# Known bugs

Defects present in the code now. Each entry gives the symptom, the cause (or
what is known of it), where it lives, and the likely fix. Delete an entry in
the commit that fixes it, with a regression test. Missing features are not
bugs: they go in [roadmap.md](roadmap.md) or the area README's "does not do
yet".

Entries are grouped by area. Add a new one under its area's heading, creating
the heading if it is not here yet.

## Modeling

### Three loaders substitute a default for a value they do not know

- **Symptom:** a document loads and looks right while meaning something else.
- **Cases:**
  - a sketch derived op with an unknown `kind` loads as a linear pattern
    (`DerivedOpFromJson`, `src/core/io/SerializeFeatures.cpp`);
  - a sketch external reference whose `kind` is not `edge` loads as a face
    (the `external` loop in the Sketch branch, same file);
  - `set_result_display` turns an unknown `field` into `von_mises`
    (`src/remote/RemoteSimulation.cpp`).
- **Likely fix:** refuse, naming the accepted values
  ([conventions.md](conventions.md#refuse-dont-substitute)).

### The datum-plane refusal does not say what is accepted

- `DatumPlaneKindFromString` accepts `offset|angled|through_points` and
  refuses anything else with "Unknown datum plane kind: X", without naming
  them; the DatumPlane `describe` row says `offset|angle`
  (`src/core/io/SerializeFeatures.cpp`).

### `JogFeature` asks for a non-destructive boolean too late

- `BRepAlgoAPI_Common op(a, b)` builds in its constructor, so the
  `SetNonDestructive` call after it does nothing and the boolean may write
  p-curves onto its input's edges (`src/core/features/JogFeature.cpp`). Build
  it default-constructed, as `BuildQuery` does in `Measure.cpp`.

### A sketch entity id can land in the reserved datum block

- `SketchFeature::MintEntityId` draws from 1..UINT32_MAX and checks only the
  sketch's own entities, so it can mint an id ≥ `0xFFFFFF00`, which
  `IsDatum()` reads as the origin or an axis (about 6e-8 per mint). Cap the
  range below `kDatumBase` (`src/core/features/SketchFeature.cpp`).

### An unknown `hole_kind` becomes a simple hole

- **Symptom:** a Hole feature whose `hole_kind` is misspelled
  (`"counterbored"`) loads and builds as a plain drilled hole, with no error.
  Counterbore and countersink fields (`cb_*`, `cs_*`) given to a kind that
  does not use them are accepted and silently dropped.
- **Cause:** the loader maps `counterbore` and `countersink` and sends every
  other string to `HoleKind::Simple`; the RPC's unknown-field check accepts
  any key the Hole row documents, whichever kind it belongs to, and
  serialization writes `cb_*` only for counterbores.
- **Where:** `FeatureFromJson`'s `Hole` branch in
  `src/core/io/SerializeFeatures.cpp`; `RejectUnknownFeatureFields` in
  `src/remote/RemoteModeling.cpp`.
- **Likely fix:** refuse an unknown `hole_kind`, naming `simple`,
  `counterbore` and `countersink`; refuse `cb_*`/`cs_*` on a kind that does
  not take them, naming the kind that does
  ([conventions.md](conventions.md#refuse-dont-substitute)).

## Sheet metal

### A flat pattern made in the GUI is always flat

- **Symptom:** after Create › Flat pattern the part is flat in every
  configuration, and the Sheet Metal panel's "Show the blank" toggle cannot
  bring the folded part back.
- **Cause:** `CreateFlatPattern` does not suppress the FlatPattern feature it
  adds, and nothing else does; the toggle's `Flat` configuration only
  unsuppresses it. The GUI test `sheetmetal/flat_view_on_a_sheet` suppresses
  it by hand, and scripts suppress it explicitly, which hides this.
- **Where:** `src/ui/CreateActions.cpp` (`CreateFlatPattern`),
  `src/ui/SheetMetalPanel.cpp`.
- **Likely fix:** create the feature suppressed in the base configuration.

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

## CAM

### The rapid-feed setting never produces a capped G1

- Preferences › Machines labels the field "Rapid feed (mm/min, 0 = G0)", and
  `Setup::rapidFeed` says some machines want a capped G1, but `CamPost.cpp`
  always writes rapids as G0; the value only clocks rapids and caps axis
  speed on turning cuts (`src/ui/SettingsWindow.cpp`,
  `src/core/cam/CamJob.h`, `src/core/cam/CamPost.cpp`). Either emit the
  capped G1 or relabel the field.

### A Z-travel overrun is reported "first at line 0"

- **Symptom:** when a program's Z span exceeds the machine's Z travel,
  `cam_verify` reports it as outside travel but says "first at line 0".
- **Cause:** the Z check is a span over the whole program (deepest point to
  highest retract), and it never sets `firstOutsideLine`; the explanation
  prints that field regardless.
- **Where:** the travel check and `Explain` in `src/core/cam/CamVerify.cpp`.
- **Likely fix:** record the line of the move that set the extreme (or say
  "over the whole program") for a Z-only overrun.

## Simulation

### Static and modal results name the wrong solver

- Results are labelled `cpu (Eigen CG-IC)`, but the solve is a direct
  `SimplicialLDLT` (modal: LDLT shift-invert). The label reaches
  `study_status`, `study_results` and the receipt's oracle version
  (`src/ui/SimulationActions.cpp`).

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

### `describe` rows that say something the handler does not do

- `fit_fastener`: says one mate per interface and an empty `mates` when the
  interface is already joined; placement adds a fastened mate per bolt per
  interface and the solver reports the extras as `redundant_mates`
  (`src/remote/RemoteAssembly.cpp`, `src/ui/FastenerActions.cpp`).
- `add_reference_image` says "path or base64 bytes" (only `path` is read);
  `set_reference_image` mentions a lock that does not exist
  (`src/remote/RemoteModeling.cpp`).
- `remove_configuration` says "by name"; it takes `id`
  (`src/remote/RemoteConfigurations.cpp`).
- `study_results` says there is no bonded contact between parts, which is
  untrue for an assembly study (`src/remote/RemoteSimulation.cpp`).
- `background_jobs` says the kinds are `cam`; the registry also has
  cam-verify, sync, topo, study, mesh, import, motion and open
  (`src/remote/RemoteDispatch.cpp`).
- The `feature_types` rows omit Pattern `mirror`, Extrude `thin`, Loft
  `path_id` and Rib `body_id` (`src/core/io/SerializeFeatures.cpp`).
- `import_step` on a part does not return `recentred_by`, which
  `add_feature` with an Import does.

### The Python tools

- `help('execute_script')` answers "no such method": `_EXPOSED` in
  `tools/designer_mcp.py` leaves it out. The MCP `describe` tool description
  hard-codes a method count.
- `designer_sdk.connect()` lists every instance as "(no project)": it reads a
  `project` field the instance registry never writes (`tools/designer_sdk.py`,
  `src/remote/InstanceRegistry.h`).
- SDK `Occurrence.frame()` does not activate its assembly, and the `frame` RPC
  resolves occurrences against the active one (`tools/designer_sdk.py`).

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

### `ResetAppForTest` leaves some windows open

- It does not reset `simulationToolOpen`, `bomWindowOpen`, `camGenerateOpen`,
  `drawingAddViewOpen` or the `asmEdit*` fields, so a window left floating by
  one test can eat the viewport clicks of the next
  (`src/app/guitests/GuiTestsCommon.cpp`;
  [testing.md](ui/testing.md#independence-any-partition-is-valid)).

## Rendering

### Four overlay renderers alias frames on a three-image swapchain

- `SketchRenderer`, `StockRenderer`, `FieldRenderer` and `UnderlayRenderer`
  keep `kFramesInFlight = 2` and index `currentFrame % 2`, while the app
  requests three swapchain images (`g_MinImageCount = 3`). Images 0 and 2
  share a UBO and vertex slot, so the CPU can overwrite one the GPU is still
  reading: sketch, stock, field and underlay overlays can draw with another
  frame's camera while the view moves. Only Mesh, Outline, SSAO and GpuTimer
  use `render::FrameSlot`; move the other four onto it
  ([frame-pacing.md](rendering/frame-pacing.md#frames-in-flight)).

## Configurations

### The feature tree does not dim a feature the configuration suppresses

- `FeatureTreePanel.cpp` checks only the feature's own suppressed flag, so a
  feature suppressed by the active configuration looks live.

## Versioning

### `pull` into a duplicate id reports the loader's raw refusal

- The merge succeeds, then `OpenProject` throws the loader's refusal instead
  of reporting a conflict (`src/remote/RemoteProject.cpp`, the pull handler);
  `conflict_list` still answers.
