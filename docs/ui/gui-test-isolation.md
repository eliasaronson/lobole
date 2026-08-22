# GUI tests that do not depend on each other — plan

> Status: **BUILT** (2026-08-22/23) — P1–P4. From: *"I thought the
> tests did some setup and cleanup to not be dependent of each other?"* — they
> do, by convention, and the convention broke three times in one day.
>
> The goal is not "more parallelism": the suite already runs four shards and
> takes `-j 8`. It is that **any partition is valid**, so adding a test cannot
> turn an unrelated one red.

## What exists, and why it is not enough

145 of the 155 GUI tests open with `MenuClick("File/New")`, which resets the
**document**.

*Correction, on reading the code rather than assuming:* a `ResetAppForTest()`
already existed and was already wrapped around every registered `TestFunc` —
the enforced-reset mechanism this document proposed as P1 was there. What it
covered was the camera, preferences, selection, the sketch session, measure
and sections. What it did **not** cover was the dock layout, the feature
sessions (preview/editor/mate/dim/in-context) and the whole CAM session — and
the dock layout is what caused two of the three failures below. So the gap
was never the mechanism; it was that nobody extended the list when they added
state.

Three failures on 2026-08-22, each defeating a different assumption:

| what leaked | how it showed | why politeness missed it |
|---|---|---|
| The selected tab in the left dock node (the CAM panel appeared without `NoFocusOnAppearing` and took it) | three unrelated tests: *"unable to hover Model"* | The window state is **ImGui's**, not the app's. No app-level teardown owns it |
| The same tab, left selected by a test that **failed before its own cleanup** | `tree/context_rename` failed two tests later | Cleanup at the END does not run when a test dies. Only a reset at the START is robust |
| `ImGuiTestContext`'s own `IsError()` flag, set by an expected-null `WindowInfo` | every later lookup in that test returned null | Not app state at all. It is the **engine's**, and it is intra-test |

And the ordering is not stable: the 4-way partition is balanced by recorded
times, so it reshuffles whenever anyone adds a test. That is how the third
one surfaced, and it is why a green sharded run is not evidence.

## The design

### P1 — Reset at the start, enforced rather than agreed

A `ResetForTest(ctx)` that runs before every test body. Two properties matter
more than what it contains:

- **At the start, not the end.** A test that fails partway cannot skip it.
- **Enforced.** After registration, iterate `ImGuiTestEngine_GetTestList` and
  wrap each `TestFunc` in a lambda that calls it first. One place, and a new
  test cannot forget because there is no registration path that skips it. A
  helper everyone is *supposed* to call is the convention we already have.

What it resets, in order — session state first, so nothing re-opens as the
document changes:

- tool sessions and armed picks: `camToolOpen`, `sectionToolOpen`, `measure`,
  `slotPick`, `pendingFeatureId`, `editFeatureId`, `sketchEdit`, `mateTool`,
  `dimTool`, `inContext`, `selection`
- CAM/FEM transients: `camGhostOp`, `camActiveSetup`, `camToolpaths`,
  `camVerdict`, `camLeftover`, `showCog`, `perfHudOpen`
- preferences to their defaults — `settings.terminalPanel` above all, since
  turning it on forks a shell
- a new project, then `buildDefaultDockLayout` and focus on Model
- the camera (several tests call `camera->SetUp`)

Deliberately **not** reset: the renderers and the ImGui context. Both are
expensive and both are rebuilt from the document anyway.

### P2 — Remove the setup that is now redundant

The 145 `MenuClick("File/New")` lines become dead weight; deleting them makes
the reset the single source of truth and saves a few frames each. Separate
commit from P1, so a bisect can tell "reset added" from "setup removed".

### P3 — Prove independence instead of assuming it: `--shuffle [SEED]`

The runner gains a flag that randomises order within each shard and prints
the seed. Order dependence then fails **loudly and reproducibly** rather than
waiting for someone to add a test. One shuffled run belongs in CI.

This is the part that actually delivers "any partition is valid". P1 makes it
true; P3 is how we know it stayed true.

### P4 — The intra-test half, which isolation does NOT fix

Worth stating plainly: **perfect isolation would have hidden the third bug.**
The self-poisoning `WindowInfo` was a defect inside one test; it only became
visible because an earlier test had created that window. Run every test in a
fresh process and it sits there green forever.

So: `WindowAbsent(ctx, ref)` and `ItemAbsent(ctx, ref)` helpers that pass
`ImGuiTestOpFlags_NoError`, and every absence assertion goes through them.
Three instances are already fixed (`3dbc0c0`, `6cca9bf`); the helper is what
stops the fourth being written.

## Phases

| | scope | size |
|---|---|---|
| **P1** | Extend `ResetAppForTest` (the wrapper already existed) to the dock layout, the feature sessions and CAM | **done** — `ResetAppForTest`, plus `isolation/a_messy_test_leaks_nothing` |
| **P2** | Delete the redundant per-test `File/New` | **done** — 126 removed, **21 kept** |
| **P3** | `--shuffle [SEED]` in the runner + `--gui-test-shuffle` in the binary | **done** — found two real dependencies on its first run |
| **P4** | Absence assertions pass `NoError` | **done** ahead of this plan (`3dbc0c0`, `6cca9bf`); the named helpers are still worth adding so the next one cannot be written bare |

**~1 session**, plus however many tests turn out to have been relying on
leaked state — which is the unknown, and which P3 is what surfaces.

## Verification

- `--serial` green. One process is the worst case for leakage and the reason
  today's third bug was findable at all.
- `--shuffle` green across at least five seeds, seeds printed so a failure
  reproduces exactly.
- The known-bad pair green in **both** orders.
- **A deliberate leak test** — built as `isolation/a_messy_test_leaks_nothing`,
  and *verified by weakening the reset*: commenting out the single line that
  rebuilds the dock layout turns the following test red, so the guard is real
  rather than decorative. It opens every tool session,
  turns on the terminal, selects a non-Model dock tab and dirties the
  document, and ends WITHOUT cleaning up, immediately before a test that
  reads the model tree. It would have caught all three of today's failures.

## Costs and risks

- **Frames.** Decided 2026-08-22: **reset unconditionally**, do not track
  whether the layout was touched. Two reasons.

  The estimate was wrong first: 20 s assumed 60 fps, but the engine runs
  `ImGuiTestRunSpeed_Fast`, so the real saving from skipping the rebuild is
  ~4 s on a 92 s run.

  And the tracking would be brittle in exactly the failure mode this document
  exists to fix. The leak that started it — the CAM panel taking the shared
  dock tab — was **not an action any test performed**; it was a side effect of
  a window appearing for the first time. A dirty flag set by our own code
  records what we did, not what ImGui did on our behalf, so it would have read
  "clean" in the one case that mattered. Observing instead of inferring (a
  hash of the dock state) means walking the node tree anyway, is more code
  than the reset it saves, and a hash missing a field fails silently in the
  same direction. 4 s is not worth buying that.
- **Tests that relied on leaked state** will fail. That is the point, but it
  is unbudgeted work; expect a handful.
- **Preferences are written to disk.** The runner already sets a per-shard
  `XDG_CONFIG_HOME`; the reset must assert it is set rather than trust it, or
  a stray local run rewrites the user's real settings.

## Deferred, with reasons

- **A process per test.** The strongest isolation and the wrong trade: 155
  Vulkan initialisations, and it would have *hidden* the `WindowInfo` bug
  rather than caught it. Isolation is not the same as correctness.
- **Resetting the ImGui context wholesale** (`imgui.ini`, all window state).
  Tempting, but the dock layout is part of what several tests assert about,
  and rebuilding it is what P1 already does at a fraction of the cost.

## What building it turned up

**The layout reset exposed a real product bug, and vindicated a fix I had
reverted.** Rebuilding the default layout while the terminal preference is
off deletes the node the terminal was docked in, and ImGui then re-homes the
window into the **central** node — where it covers the entire 3D viewport.
Measured at the failure: the terminal's rect came back exactly equal to the
viewport rect, so a click meant for the model landed in the terminal.

I had guessed at this mechanism earlier while chasing a different bug, found
it fixed nothing, and reverted it on the grounds that an unproven change
should not ship. That was right then and the fix is right now — the
difference is a measurement instead of a hypothesis. A user hits it by
turning the terminal off and using View > Reset Layout.

One test relied on leaked state and had to be understood rather than
adjusted: `terminal/hover_decides_who_gets_the_keys` moves the pointer to the
centre of the viewport and expects the app to take the keyboard back. It was
passing only because the layout was never rebuilt underneath it.

## What P2 and P3 turned up

**P3 paid for itself on the first run.** Two order dependencies that the
time-balanced partition had been hiding:

- `document/parametric_box_undo_redo` and `create_sphere_and_cylinder`
  measured a **body** count against a **feature** count baseline. Those are
  only the same number in a document containing nothing but solids, and the
  shuffle put a datum test — planes and sketches, features with no body — in
  front of them.
- Seed 11 **segfaulted a whole shard, losing 49 tests**: an assembly test left
  the app on an assembly tab, where `document` is null, and the next test
  dereferenced `Doc()`. That is what made the reset create a fresh project
  rather than merely tidy around one, which is also what makes P2 safe.

**P2 needed to be less mechanical than it looked.** A blanket removal of all
147 `MenuClick("File/New")` calls passed the compiler and broke
`folder_empty_create`, which uses `File > New` **mid-test** on purpose — to
prove that reopening a saved project restores its folder. Removing only the
ones that are a test's opening setup leaves 126 gone and **21 kept**. The
regex that distinguishes them keys on the call sitting directly after the
lambda opening and its `SetRef`.

Seeds are printed by every shuffled run and derived from one base, so
`--shuffle <SEED>` replays a failing run exactly — which is the whole
difference between a randomised suite and a flaky one.
