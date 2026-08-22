# GUI tests that do not depend on each other — plan

> Status: **design, awaiting sign-off** (2026-08-22). From: *"I thought the
> tests did some setup and cleanup to not be dependent of each other?"* — they
> do, by convention, and the convention broke three times in one day.
>
> The goal is not "more parallelism": the suite already runs four shards and
> takes `-j 8`. It is that **any partition is valid**, so adding a test cannot
> turn an unrelated one red.

## What exists, and why it is not enough

145 of the 155 GUI tests open with `MenuClick("File/New")`. That resets the
**document**. There is no registered setup or teardown hook — nothing the
engine runs between tests — so everything that is not the document is reset
only if the test that changed it remembers to change it back.

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
| **P1** | `ResetForTest` + wrapping every registered `TestFunc` | ~half a session |
| **P2** | Delete the redundant per-test `File/New` | ~1–2 h, mechanical |
| **P3** | `--shuffle [SEED]` in the runner, one shuffled CI job | ~1 h |
| **P4** | `WindowAbsent`/`ItemAbsent` helpers, absence assertions moved over | ~1 h |

**~1 session**, plus however many tests turn out to have been relying on
leaked state — which is the unknown, and which P3 is what surfaces.

## Verification

- `--serial` green. One process is the worst case for leakage and the reason
  today's third bug was findable at all.
- `--shuffle` green across at least five seeds, seeds printed so a failure
  reproduces exactly.
- The known-bad pair green in **both** orders.
- **A deliberate leak test**: a temporary test that opens every tool session,
  turns on the terminal, selects a non-Model dock tab and dirties the
  document, placed immediately before a sensitive test. If the reset works
  the sensitive test does not notice. This proves the mechanism rather than
  assuming it — and it is the check that would have caught all three of
  today's failures.

## Costs and risks

- **Frames.** A layout rebuild plus a few yields, 155 times, is roughly 20 s
  at 60 fps. Acceptable against a 92 s sharded run; if it measures worse,
  rebuild the layout only when the previous test changed it.
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
