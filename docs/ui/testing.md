# Testing the GUI

The GUI is tested by driving the real application with the Dear ImGui Test
Engine: real windows, real frames, real OpenCASCADE geometry, a real Vulkan
renderer on a virtual display. Aesthetics are checked by eye on screenshots;
behaviour is asserted.

## Where the tests are

`src/app/guitests/GuiTests{Assembly,Cam,Drawing,Modeling,Project,Shell,Simulation,Sketch,Terminal,Viewport}.cpp`,
registered with `IM_REGISTER_TEST(engine, "<category>", "<name>")` and
compiled into the app. Shared helpers are in `GuiTestsCommon.{h,cpp}`:
`ToolbarClick` (opens the `sheet` and then the `direct` flyout,
`**/<group>/###more`, until the button shows), `ToolbarCreateAccept`, `WorldToWindow` (a world point to window
coordinates through the viewport rect, matching the picking math),
`LensScale()` (pick tolerances are in pixels, so a test that pins a camera
distance to land on a pixel scales it by the lens), and `kToolCases`, the
table behind the `defaults/*` gate ([tool-sessions.md](tool-sessions.md#defaults-and-refusals)).

The binary's flags: `--gui-tests`, `--gui-test-filter`, `--gui-test-list`,
`--gui-test-shuffle [seed]`.

## Running them

`./tools/gui-tests-xvfb.sh` is the runner. It runs the suite on Xvfb, where
there is no compositor, occlusion or screen lock to throttle presentation.

| flag | effect |
|---|---|
| (none) | whole suite, 4 shards, **shuffled**, with the seed printed |
| `-j N` | N shards |
| `--fast` | skip tests recorded above a time threshold (a test with no recorded time always runs) |
| `-f <pattern>` | matching cases only, one process |
| `--serial` | one process: the worst case for leaked state, and the way to find it |
| `--shuffle <seed>` | replay a recorded order exactly |
| `--in-order` | declared order, no shuffle |
| `--record-times` | refresh `tools/gui-test-times.txt`, which balances the shards |
| `-L <label>` | all ctest tests with that label |
| `--slow-frames MS`, `--slow-jobs MS` | a loaded machine, simulated ([below](#wait-on-the-condition-not-a-frame-count)) |

- Each shard gets its own `XDG_STATE_HOME` and `XDG_CONFIG_HOME`, so shards
  never share the recent-projects list or `settings.json`, and no run writes
  the developer's real preferences.
- The run takes a machine-wide `flock`, so concurrent runs (parallel agents)
  serialise rather than collide, and then `build/`'s lock shared, so a build
  or a merge does not relink `designer` under the shards
  ([the development loop](../architecture.md#the-development-loop)).
- It compares the number of tests that ran with `--gui-test-list`, so a bad
  partition fails loudly instead of skipping tests.
- It reaps Xvfb servers orphaned by an earlier run.

ctest's `gui_smoke` is the same suite, serial and unsharded; it and the other
Xvfb-needing ctest entries run under `tools/xvfb-exec.sh`. CI runs the suite
serial and shuffled on software Vulkan.

## Independence: any partition is valid

The shards are balanced by recorded times, so adding a test reshuffles which
tests share a process. A suite whose tests depend on each other therefore
turns an unrelated test red whenever someone adds one. Three mechanisms make
order irrelevant:

**1. Reset at the start, enforced.** After registration every test's
`TestFunc` is wrapped (`ImGuiTestEngine_GetTestList`, `RunTestWithReset`) so
`ResetAppForTest()` runs **before** the body. At the start, because a test
that fails partway never reaches its own cleanup; enforced, because there is
no registration path that skips it. It resets, in order: tool sessions and
armed picks (pending preview, editor, measure, section, mate, dimension,
in-context, the CAM verb and editors, exploded view, motion), the CAM
transients and background jobs, handle and pick state, the selection and
sketch session, `keyCaptureId`, the shortcuts window, `settings` to defaults,
the camera; then it rebuilds the **default dock layout** unconditionally and
replaces the project with a fresh one holding a single part tab.

- The layout rebuild is unconditional because the dock state is **ImGui's**,
  not the app's: a panel that appears without `NoFocusOnAppearing` takes the
  selected tab in the shared left node, and no app field records that.
  Tracking whether a test "touched the layout" would read clean in exactly
  that case.
- The fresh project is required, not tidiness: a test ending on an assembly
  tab leaves `document` null, and the next test to touch `Doc()` crashes the
  whole shard.
- Settings are reset so the developer's own config never decides whether a
  test passes.

Deliberately **not** reset: the renderers and the ImGui context (expensive,
and rebuilt from the document anyway). A process per test would isolate more
and catch less (see 3).

`isolation/a_messy_test_leaks_nothing` opens every tool session, turns on the
terminal, selects a non-Model dock tab and dirties the document, then ends
without cleaning up, immediately before a test that reads the model tree.

**2. Shuffled by default.** `--gui-test-shuffle` randomises the order within
each shard and prints a seed derived from one base; `--shuffle <seed>`
replays a failure exactly — while the registered tests are the same (see
below). That is the difference between a randomised suite and a flaky one.
`--serial` puts every test in one process when hunting a leak; a green
sharded run is not evidence of independence.

### Replaying a failed shard

A failed shard is archived under `~/.local/state/lobole/gui-failures/`:
its log, its filter, `sequence` (the tests in the order they ran, read from
the log) and `reproduce.sh`, which runs that sequence with
`--gui-test-sequence cat/name,...` — exactly those tests, in that order, an
unknown name refused. The seed is kept as a comment only: it shuffles the
whole registered list, so after one test is added the same seed deals a
different order and the replay passes for no reason. To find the test that
leaks, delete lines from `sequence` (keep the failing one last) and rerun.

**3. A failed lookup poisons the rest of the test.** `ImGuiTestContext`'s
`WindowInfo` and `ItemInfo` log an error when they do not find their target,
and once the context is in error every later lookup in that test returns
null. So

```cpp
IM_CHECK(ctx->WindowInfo("//Terminal###TerminalPanel").Window == nullptr);
```

passes — and silently disables every later assertion that looks something
up. Whether it logs depends on whether the window ever existed, i.e. on which
test ran before, so it appears and disappears with the partition. **Ask with
`ImGuiTestOpFlags_NoError` whenever "absent" is a legitimate answer.**
`ItemExists` already does. Isolation cannot fix this one: it is inside one
test, and running each test in a fresh process would hide it.

## Wait on the condition, not a frame count

A test that gives something asynchronous a fixed number of frames —
`ctx->Yield(3)` after starting a job, `for (i < 600 && …)` — is a guess at how
long a worker takes measured in frames, and both halves move under load: the
worker is starved, and a frame can take 250 ms. Such a test passes alone and
on a quiet machine and fails, a different one each run, while another agent
is building.

- **Waiting** is `WaitFor(ctx, "what", [] { return condition; })` from
  `GuiTestsCommon.h`: a frame at a time until the condition holds, bounded
  by WALL time (`kWaitSeconds`, under the engine's 300 s watchdog), logging
  what it waited for when it gives up. `WaitForJobs(ctx, kind)`,
  `WaitForOpen` and `WaitForStudyResult` are the common ones. A short
  `Yield(n)` is still right for UI that settles synchronously (a window
  appearing the frame after a click); never for a worker, a child process
  or anything off the frame.
- **Wait for the thing that lands**, not for "some result": a motion study's
  previous verdict stays up while a new run is in flight, so a test that
  waits for `MotionResultFor != nullptr` reads the old one.
- **ImGui's clock counts frames** in a suite run (`ConfigFixedDeltaTime` =
  1/60 s, `main.cpp`). The engine injects input a frame at a time, but ImGui
  judges it by the clock: its double-click is two presses within 0.3 s, and
  the engine's are two frames apart, so with 150 ms frames every
  `ItemDoubleClick` in the suite was two single clicks and the editor it
  opens never appeared. Wall time is for what really is wall time (a
  worker's deadline, the idle pacer's cooldown): `std::chrono::steady_clock`,
  not `ImGui::GetTime()`.
- **A bound on what a frame costs** ("no frame ran the pass", "playback
  stays interactive") reads `MainThreadCpuMs()`, the main thread's CPU
  clock, not a stopwatch round `Yield()`: under load a frame that computed
  nothing still takes 250 ms of wall time waiting for a core.
  A bound on the app's own work, without what drawing the frame cost, reads
  `frameTimer.Snapshot(1).workCpuMs` after the `Yield()` — the same clock
  over the `update` and `ui` phases only
  (docs/rendering/frame-pacing.md#phases); the frame timer's phases
  themselves are wall time.

Load reproduces this badly: the burners `tools/with-cpu-load.sh` starts are
niced (`--contend` unnices them), and the app outruns them. The runner
reproduces it on a quiet machine instead, deterministically:

| flag | what it simulates |
|---|---|
| `--slow-frames MS` | every frame takes at least MS (`--gui-test-min-frame-ms`) |
| `--slow-jobs MS` | no background job is done sooner than MS after it started (`--gui-test-job-latency-ms`, `ui::SetTestJobLatency`) |

Both come before `-f`, and a failed shard's `reproduce.sh` carries them.
`--slow-frames 160` and `--slow-jobs 3000` are levels the whole suite
passes at. `--headless` honours `--gui-test-job-latency-ms` too, so a smoke
can hold its instance's jobs open the same way (`hostile_params_smoke
--self-test`, [the hostile sweep](../scripting/rpc.md#the-hostile-sweep)).

## Tests that time things

A check that compares a time with a fixed number is one of three kinds,
and a loaded machine treats each differently (one with load 24 to 61 failed
three suites that passed alone):

- **Cost** -- "this must not go quadratic", "a refusal costs a parse", "a
  frame does not run the pass". Read CPU time, which other work does not
  inflate: `std::clock()` for the process in a Catch2 case (Delete Face's
  100 holes, under 120 s of CPU; the feature-limit refusal, under 5 s;
  `FaceSnapPoints`), `MainThreadCpuMs()` in a GUI test (above), the
  reader-fuzz budget (docs/versioning/README.md#reader-fuzzing). CPU time
  still depends on the core the work lands on: a hybrid CPU's low-power
  E-core spends 3.3 times a P-core's CPU seconds on the same heal, so a
  bound is set against the slowest core, measured pinned there (`taskset
  -c`). A ratio of two times taken in the same run stays wall-clock, since
  load cancels out of it (`test_meshcollide`'s mesh against exact).
- **Responsiveness** -- "a ping answers within 1 s while a job runs", "a
  reuse answers within 1 s", "a cancel lands inside the hold", "a budget
  bounds the call". Wall time is the point, so a smoke keeps it, measures
  inside a `host_load` window, and calls `host_load.judge`: a check that
  holds passes; one that fails fails on a quiet machine, and on a machine
  busy with other work is skipped, printing why, while the rest of the
  test runs ([the probe](../performance/README.md#is-the-machine-busy)).
  These are the latency checks in `contact_smoke`, `insertion_smoke`,
  `drawing_parked_smoke`, `cam_parked_smoke`, `remote_smoke`,
  `requirements_smoke`, the reuse checks in `fem_async_smoke`,
  `topo_smoke`, `motion_smoke`, `study_jobs_smoke` and
  `study_reopen_smoke`, and `drawing_quality_smoke`'s budget.
- **Hang guard** -- `PumpUntil`, `WaitFor`, a `< 30` s wait loop, a ctest
  `TIMEOUT`, `test_occ_patches`' deadline check (ten times the longest
  silence). It ends a hang rather than judging speed, so it stays as it is,
  with room for a loaded machine.

A new timing check names its kind, and is shown to fail on a quiet machine
against a planted slowdown in the path it times: a CPU spin inside the
measured interval for a cost check (a sleep would not count, which is the
point), a delay in the method for a responsiveness one.

## Recording a scene

The test engine records video: `ctx->CaptureBeginVideo()` and
`CaptureEndVideo()` around a test's steps write an mp4 through ffmpeg (found
on `PATH`, as `export_motion_video` finds it). The frames come from the app,
not the display: the engine asks for each frame's pixels from inside
`ImGuiTestEngine_PostSwap`, and `CaptureForTestEngine` (`app/main.cpp`)
re-renders that frame, scene and interface from the same draw data, through
`CaptureScreenshot`, the remote screenshot's path. The swapchain is never
read, and under Xvfb nothing outside the app could read it: the driver
presents without passing through the X framebuffer, so `x11grab` and
`xwd -root` both see black. The engine calls the hook only while it
captures, so an ordinary frame costs nothing.

- **The clock.** A recording is one video frame per frame of the suite's
  fixed 1/60 s clock, at 60 fps, however long each frame took to render.
  Ending a video sets the engine's `ConfigFixedDeltaTime` to 0 (wall clock),
  so `EndRecording` (`guitests/GuiTestsDemo.cpp`) puts the suite's back --
  every test after it would otherwise judge double-clicks by machine load.
  Motion and assembly-sequence playback tick on that clock
  (`ImGui::GetTime()`), so Play in a scene runs at its own pace in the
  clip; CAM playback still ticks on the wall clock.
- **The speed.** While it records, the engine switches a Fast run to
  Cinematic: a mouse move glides and then pauses. Step a timed sequence with
  `ctx->Yield()`, one frame each, not with mouse moves --
  `capture/records_a_clip` once recorded two minutes where it meant two
  seconds. `--gui-test-speed fast|normal|cinematic` sets the speed for a
  whole run.
- **The size.** The engine rounds a video's size to even sides and then,
  on a window already even, rounds it down by two (1280x720 became
  1278x718); `BeginRecording` asks an even window for no rounding.
- `capture/records_a_clip` records two seconds of a turning camera, decodes
  them and checks the clip: every frame, the window's size, not black,
  moving. CI's GUI job installs ffmpeg for it.

`--capture-dir <dir>` is where a scene writes its clip
(`GuiTestCaptureDir()`); a scene given none runs the same steps and records
nothing, which is how it stays in the suite. The `demo` category is the
release video's scenes, recorded by `tools/demo/record.sh`
([tools/demo/README.md](../../tools/demo/README.md)) at Cinematic speed in a
window `--window-size` makes 3840x2160, with `--ui-scale 2` drawing a
1920x1080 layout at HiDPI density -- a size and scale only a run that asks
gets, since the suite's layout tests assert what fits at 1280x720 and 1.0.
Mouse and scroll speeds scale with `--ui-scale`, and the recording keeps
every window where it is (`ImGuiCaptureFlags_IncludeOtherWindows`): without
it the capture tool hides and moves windows while it records, and a tool
window's button had nothing under the mouse.

## Making a widget addressable

Tests, `ui_items` and remote UI driving all address widgets by path. What
makes a widget unreachable by name:

- **`##label`** hides the label *and* leaves the widget with no name, so only
  the numeric id `ui_items` reports reaches it. Use `"Visible###stable_id"`:
  same appearance, addressable. `LabeledCombo` uses `"###" + label`.
- **A bare `ImGui::Combo("Label", …)`** gathers with an empty label; give it
  `"Label###id"`.
- **`PushID(int)`** makes a scope that cannot be a path segment;
  `PushID(const char*)` can (`//###ToolWindow/Distance/##expr` works). For a
  repeated widget, put the unique part in the id
  (`"###material_<id>"`) rather than wrapping it in a scope.
- **An unnamed table** (`BeginTable("##…")`) is an unaddressable scope, and a
  `**` wildcard crawl over one can outrun the engine's 300 s watchdog. Name
  tables: `BeginTable("###vars_table", …)` makes
  `//Variables/###vars_table/###var_expr_project_len` resolve.
- **A widget scrolled out of its panel** reports an empty label, though
  `ui_items` still lists it. A `###id` fixes that too: `ItemInfo` resolves a
  path by hash and never consults the label.
- `ui_items`' `label_truncated` says the test engine's 32-byte debug label
  buffer cut the label, not that anything was clipped on screen. What a
  layout test asserts instead is the user-visible contract: a row too long
  for its panel is still addressable by id and readable on hover
  (`tabs/panel_tabs_fit_and_name_on_hover`,
  `assembly/a_long_row_elides_and_names_itself`).
- A combo popup does not take window focus: address it by the popup's own
  window name, not `//$FOCUSED`.
- `ComboClick` wants a ref relative to the window:
  `SetRef("//###ToolWindow")` then `ComboClick("###dwg_orientation/front")`.
  The window-qualified form fails with an assertion about the popup that does
  not read as an addressing problem.
- The tool window is `###ToolWindow` for every tool, so
  `//Hole###ToolWindow` drives whichever session is open.
- A face centre guessed from a primitive's sizes can land on an edge, which
  correctly outranks the face in the pick. Derive click points from
  `GetBoundingBox`.
- A test that types through `ItemInputValue` clears the field with Ctrl+A
  first, so it tests the commit, not how the field opens. If what changed is
  how a field opens, open it and type.
- A test for a background behaviour must prove the thing is really in the
  background (the terminal's tab test checks which tab is visible).

## What the GUI tests cover

"Driven through the GUI" means a test clicks the toolbar button, fills the
tool window and accepts, and then checks the **geometry**, not only that a
feature appeared.

| Feature | Toolbar | GUI test |
|---|---|---|
| Box / Cylinder / Sphere | `###tb_box` … | `primitive/*` (face pick, drag snap, Accept, one undo), `document/create_sphere_and_cylinder`. Most tests that just need a body add a stored box as a fixture (`AddFixtureBox`), not through the tool |
| Sketch | `###tb_new_sketch` | `sketch/*`; plane picks from the tree and the datum row |
| Extrude | `###tb_extrude`, `###tb_sk_extrude` | `sketch/model_toolbar_extrudes_finished`, `handles/extrude_arrow_drag_sets_distance` |
| Revolve | `###tb_revolve`, `###tb_sk_revolve` | `revolve/profile_beside_the_axis_rings` (volume against Pappus) |
| Sweep | `###tb_sweep` | `sweep/profile_along_a_path_makes_a_bar` |
| Loft | `###tb_loft` | `loft/two_profiles_via_the_section_combo` |
| Pipe / 3D curve | `###tb_pipe`, `###tb_curve3d` | `curves/pipe_along_helix_makes_body`, `curves/create_helix_from_menu` |
| Fillet / Chamfer | `###tb_fillet`, `###tb_chamfer` | `fillet/fillet_body_edge_via_click`, `chamfer/edge_via_click_makes_a_chamfer` |
| Hole | `###tb_hole`, `###tb_sk_hole` | `hole/*` |
| Shell | `###tb_shell` | `shell/open_face_pick_hollows_the_box` |
| Draft | `###tb_draft` | `draft/face_pick_names_the_body` |
| Delete Face | `###tb_delete_face` | `direct/delete_preselected_bore`, `direct/add_the_feature_takes_the_floor`, `direct/refused_delete_names_the_face`, `direct/pick_faces_repairs_a_dangling_ref` |
| Offset Face | `###tb_offset_face` | `direct/offset_handle_drags_the_distance` |
| Move Face | `###tb_move_face` | `direct/move_preselected_bore` |
| Replace Face | `###tb_replace_face` | `direct/replace_top_onto_a_datum_row` (the target from the tree) |
| Split / Refine | `###tb_split`, `###tb_refine` | `split/*`, `refine/merges_the_seam_left_by_a_union` |
| Rib | `###tb_rib` | `rib/menu_rib_previews_and_adds_volume` |
| Thread | `###tb_thread` | `thread/face_pick_fills_the_slot` |
| Boolean | `###tb_boolean` | tests in `GuiTestsProject.cpp` and `refine/merges_the_seam_left_by_a_union` |
| Pattern / Mirror | `###tb_pattern`, `###tb_mirror` | `pattern/*` (including `pattern/cut_row_is_the_pattern_source`: a hole patterns as a hole, its source the cut's tree row) |
| Sheet metal | `###tb_base_flange`, `###tb_edge_flange`, `###tb_flat_pattern`, `###tb_hem`, `###tb_jog`, `###tb_corner_relief`, `###tb_normal_cut`, `###tb_unfold`, `###tb_refold` (one flyout, `**/sheet/###more`) | `sheetmetal/edge_pick_bends_a_wall`, `sheetmetal/panel_declares_the_stock`, `sheetmetal/flat_view_on_a_sheet` |
| Datum plane / axis | `###tb_datum_plane`, `###tb_datum_axis` | `datum/plane_offset_then_sketch_on_its_row` |

Every creation tool is also opened with nothing selected by the `defaults/*`
gate. Assembly mates, drawing dimensions, simulation and CAM have their own
click-through suites (`assembly/*`, `drawing/*`, `sim/*`, `cam/*`), which reach
tools through the tool window rather than every toolbar id.

Not driven by a click: **Import** (File > Import, Ctrl+I), because its first
step is the desktop's file dialog — an xdg portal process outside ImGui and
outside the test engine, which also means a stray click on a file-dialog
button during a test opens a real picker on the developer's desktop. The
`import/*` tests call the action behind it (`ui::StartImport`) with a
fixture and click everything after; the readers are covered headlessly. Also the sketch toolbar's reference
image button and its display toggles (`###tb_sk_image`, `###tb_sk_autocon`,
`###tb_sk_glyphs`).

`parity/clicking_and_scripting_agree` builds the same part and drawing sheet by
clicking and over the script surface and compares them.

## The tab-kind coverage matrix

`python3 tools/coverage_matrix.py` prints it (`--out FILE` writes it); it is
not committed, because a copy in the tree goes stale with the next GUI test.
It takes the test list from the binary (`lobole --gui-test-list`) and
credits tests by scanning their bodies. For each interactive tool (measure, section,
mate, gizmo, preview handles, sketch dimensions and drawing, the CAM tool,
simulation loads, the feature tree, the tab bar) it counts the tests that
exercise it on each tab kind it claims to support (part, assembly, drawing),
and flags `GAP` and thin cells. The defect shape it exists for is "tested on a
part tab and nowhere else": the tests that exist all pass while a whole tab
kind is broken.

## Other checks on the UI

- `tests/test_conventions.cpp` holds the UI's source-level rules: no raw
  numeric field outside `ui::ExprInput`; background jobs polled only by the
  loop; one icon table; every tool-session sentinel registered; every scene
  renderer registered.
- `tests/render_pixel_smoke.py` re-renders a fixed scene and diffs it byte
  for byte against a per-GPU baseline; run it after touching the renderers or
  `RendererSync.cpp`.
- `--offscreen` instances have no panels and build no overlays, so anything
  drawn over the viewport by the UI (handles, glyphs, sketch annotations)
  needs a windowed instance (`tools/headless-preview.sh --gui`) or a GUI
  test.

## Known gap

`ResetAppForTest` does not close the floating windows opened by flags it does
not know: `simulationToolOpen`, `bomWindowOpen`, `camGenerateOpen`,
`drawingAddViewOpen` and the assembly editor fields. The BOM test closes its
own window; the others rely on the tests that open them. A stray floating
window eats the viewport clicks of every test after it.
