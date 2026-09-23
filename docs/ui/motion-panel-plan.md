# The Motions panel: what Preview and Validate give back — plan

## The problem

The assembly Motions panel (`src/ui/OccurrencePanel.cpp`) has three buttons
under a Pose slider, and two of them do less than they say:

- **Preview motion** solves every sample of the study without collision
  checks — and then shows only *"Kinematics only: collision checks not run."*
  The verdict's second branch returns on `!checkedClashes` before it looks at
  whether the samples SOLVED, so a mechanism that locks or jumps branch at
  90 degrees previews exactly like one that works. The poses the run computed
  are read by nothing in the GUI: the slider re-solves live, there is no
  playback, and the one consumer (`export_motion`, the animated GLB) is
  reachable only from a script.
- **Both buttons edit the study.** Preview writes `check_clash = false` and
  Validate writes `check_clash = true, clash_mode = exact` into the stored
  definition, each an undo step. Press Preview on a study a script validates
  and the saved document now says "not checked" — the script's next run
  reports `not_checked` with nothing to say why; press Validate on a mesh
  study and it is an exact one from then on.

## The shape of the fix

1. **A run can override, a button never edits.** `StartMotionRun` takes an
   optional `RunOverride {checkClash, clashMode}` applied to the RUN's copy
   of the study (in `Prepare`, before the meshes are prebuilt). The result's
   staleness revision stays computed from the stored study, so a preview of
   an unchanged study is fresh, not stale. Preview runs `{false}`, Validate
   runs `{true, exact}`; the stored study is untouched and no undo step is
   pushed. The `run_motion_study` RPC is unchanged: it runs the study as
   defined.
2. **One verdict, in core, that says whether it moved.** The branch ladder
   moves out of the panel into `motion::Verdict(result, study, stale)`
   (`MotionStudy.{h,cpp}`) returning a severity and a line, and it asks
   about SOLVING before collisions:
   stale → cancelled → *"Does not solve at sample 19 (90 deg): residual
   0.40 mm, 1 errored mate"* → *"Moves through all 73 samples; collisions
   not checked"* (a preview) → unknown pairs → clean → not clean (which now
   names the clashing pair). Unit-tested without a GUI.
3. **Play the poses the run made.** A Play/Pause button animates the
   viewport through the stored samples — display-only, like an exploded view:
   no solve, no document edit. `AppState` gains the playback state (study,
   position, playing); `DisplayMatrix` returns the interpolated sample pose
   (position lerp, orientation slerp) for a top-level occurrence while it is
   set, the full resync uses it, and each frame pushes it through
   `PushOccurrenceLive` the way the explode slider does. `TickMotionPlayback`
   runs beside `TickCamPlayback` in both main loops and loops over the range;
   the frame pacer counts it as playing. Stopping clears the pose and forces
   one resync, so picking is right again (it lags while playing, as it does
   while exploding). Play is refused — disabled, with the reason — with no
   result, a stale one, or an unsolved sample, the same rule the GLB export
   has. Anything that invalidates the result stops playback.
4. **Export the animation from the panel.** *Export animation…* opens a save
   dialog and calls the existing `ExportMotionGlb`; its refusal is shown in
   the panel, not only logged. Disabled without a fresh solved result or
   without native dialogs, like the BOM's Export CSV.

When nothing is playing, every transform is computed exactly as today: the
byte-exact pixel gate must not move.

## Files

`src/ui/MotionActions.{h,cpp}` (override, playback tick, pose lookup,
export dialog), `src/ui/OccurrencePanel.cpp` (buttons, verdict, transport),
`src/ui/AssemblyActions.cpp` (`DisplayMatrix`), `src/ui/RendererSync.cpp`
(the full-sync transform), `src/ui/AppState.h` (playback state),
`src/ui/FileDialogs.{h,cpp}` (the dialog), `src/app/main.cpp` (tick +
pacer), `src/core/MotionStudy.{h,cpp}` (Verdict).

## Verification

- Unit: `motion::Verdict` over a preview that solves, a preview that does
  not, a cancelled run, a clean and a clashing validation, a stale result.
- GUI (`assembly/a_motion_study_scrubs_and_runs`): Preview and Validate leave
  `check_clash`/`clash_mode` as they were and push no undo step; Play moves an
  occurrence's DISPLAY transform while its document transform stays put;
  Pause/stop restores it; the export button exists (the dialog itself is not
  clicked — a portal dialog opens on the real desktop).
- `render_pixel_smoke` and the full headless and GUI suites.
- Docs: docs/scripting/iteration-workflow.md and docs/ui/u15-assembly-panel.md
  ("motion has no play").
