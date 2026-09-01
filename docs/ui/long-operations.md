# Long operations off the frame — plan (A3)

> Status: **planned, not built.** Written 2026-09-01 after reading all five
> existing implementations, which is what changed the plan: the review
> (capability-review-2026-08-31.md, A3) said there were four cases with three
> hand-rolled answers. There are **five**, and reading them turned up two live
> bugs that have nothing to do with tidiness.

## What exists

Five long operations, five shapes. The rule they all follow is already
articulated, in `ImportJob.h`:

> "the worker touches ONLY pure data, and every Document mutation stays on the
> main thread."

That rule is right and is not in question here. What is unshared is everything
around it.

| job | progress | cancel | polled by | completion |
|---|---|---|---|---|
| `TopoJob` | 4 atomics (iteration, compliance, maxChange, volumeFraction) | yes, keeps best | `StudyPanel`, `RemoteSimulation`, `RemoteServer` | `optional<TopoResult>` after join |
| `StudyJob` | none | **no** — the linear solver has no interruption points | same three | `StudyRuntime` after join |
| `ImportJob` | phase enum + fraction + partsDone/Total | yes | the **frame loop** (main.cpp, twice) | plan + model, then a *sliced* main-thread materialize |
| `SyncJob` | none | no | **`HistoryPanel`, inside a `TreeNode`** | ok/error after join |
| `RenderJob` | plain bools, main-thread sliced | yes | inline in main.cpp | path/error |

Four different poll drivers, three progress vocabularies, two of five
cancellable, and one that is not a thread at all.

## The two bugs this turned up

Both are the same defect, and neither needs the mechanism below to fix.

**A collapsed section stops a push from completing.** `PollSync` is called at
`HistoryPanel.cpp:686`, inside `if (ImGui::TreeNode("Sharing"))`. Collapse that
section — or close the History panel — during a fetch, push or clone, and
nothing ever joins the worker or reports its result. The operation finishes and
the app never finds out.

**Leaving Simulation mode strands a solve.** `PollTopologyRun` and
`PollStudyRun` are called from `DrawTopoResults`, which `UI.cpp:801` draws only
while `state.document && state.simulation.IsActive()`. Switch to an assembly
tab or leave Simulation mode mid-solve and the result is not stored until you
come back. An RPC client still reaps it (`RemoteSimulation` polls too), which
is why this has survived: it is invisible to every scripted test and only bites
a person clicking.

**Fix these first, and separately.** Moving a poll out of a `TreeNode` is one
line, and holding a one-line bug fix hostage to a refactor is how both get
delayed. They also make the case for the rest of this document better than any
argument about duplication does.

## The mechanism

One registry, owned by `AppState`, and **exactly two poll sites**: the frame
loop and the RPC pump. Where a job is polled stops being a property of which
panel is open.

```cpp
struct Job {
  std::string kind;   // "import" | "topo" | "study" | "sync" | "cam" | …
  std::string label;  // "Clearing pocket 2 of 5" — shown as-is
  std::atomic<double> fraction{-1.0};  // <0 = indeterminate, no bar
  std::atomic<bool> cancel{false};     // main sets; a job may ignore it
  std::atomic<bool> done{false};       // worker sets last
  std::string error;                   // read after done + join
  std::thread thread;
  // Runs on the MAIN thread after the join. Returns false to be called again
  // next frame — which is how ImportJob's sliced materialize fits without a
  // second mechanism.
  std::function<bool(AppState&)> finish;
  ~Job() { cancel.store(true); if (thread.joinable()) thread.join(); }
};
```

Three things this deliberately does **not** try to unify:

- **The progress vocabulary.** `fraction` plus a `label` the domain writes is
  the common denominator; `TopoJob`'s compliance and `ImportJob`'s
  partsDone/Total stay where they are, because a shared struct with every
  field any job might want is how the abstraction rots.
- **Cancellability.** `StudyJob` cannot be interrupted and saying so is
  honest. `cancel` is a request; a job that ignores it reports that it does.
- **The result type.** It lives in the domain's own struct and is consumed by
  that domain's `finish`. The registry never sees it.

### What it buys immediately

An RPC surface that works for every job rather than one: `jobs` (list what is
running, with kind, label, fraction) and `cancel_job {kind}`. Today
`study_status` is bespoke, and `mesh_study` has no async path at all —
[known-bugs.md](../known-bugs.md) records a modal study outliving the client's
60 s socket timeout, which "LOOKS like a hang followed by a mystery". With the
registry, `mesh_study` gets polling for free rather than growing a second
bespoke status verb.

## Order

Deliberately not a big-bang rewrite of five working things — the refactoring
policy is "split what the next feature extends", and four of these five work.

0. **The two bugs above**, now, independently of everything else.
1. **The registry, the two poll sites, and `jobs`/`cancel_job`** — with the
   CAM adaptive pocket as the only adopter. Nothing existing changes, and the
   new job proves the shape before anything is migrated onto it.
2. **`SyncJob`** — the worst-affected and the simplest (no progress, no
   cancel). If the shape cannot carry it, that is cheap to discover here.
3. **`TopoJob` + `StudyJob`** together: they share a panel and a poll site, and
   splitting them would leave the panel polling two ways.
4. **`ImportJob`** last — the sliced materialize and the modal make it the
   hardest, and by then the registry has been proven three times.

`RenderJob` is not a thread; whether it belongs here at all is a question for
when the rest is done, not now.

## Phase 1 in detail — the CAM adaptive pocket

The first adopter, and the reason A3 is worth doing now rather than later:
generating an adaptive pocket takes **~7 s on the UI thread**
([known-bugs.md](../known-bugs.md)), on the operation a person adds and then
adjusts. It is the one big cost in CAM that is not incremental.

It is also already diagnosed, which removes the usual first day of work.
`tools/profile.sh --test 'cam: clearing the outline passes over holes'`
attributes **25% of self time to `Adaptive2d::CalcCutArea`** and about 30% more
to Clipper's scanbeam underneath it — vendored library, inside
`Adaptive2d::Execute`, with no interruption points of its own.

That last part shapes the work:

- **Cancel is coarse.** libarea cannot be interrupted mid-`Execute`. The job
  can refuse to *start* the next operation, and can drop a finished result the
  user has already cancelled, but a single adaptive op runs to completion.
  Report that rather than offering a Cancel that does nothing.
- **The backplot shows the PREVIOUS toolpath while a new one is generating.**
  Blanking it would be worse than the freeze: an empty backplot reads as "this
  operation produces nothing", which is a real state the panel already has to
  express.
- **The operation row owns the progress**, not a modal. A modal is right for
  import (the document is being built and nothing else is safe) and wrong here
  (the rest of the job is still editable).
- **The cheap half first, and possibly instead.** Caching by operation
  parameters + document version would make a re-open or an unrelated edit never
  re-run libarea at all. If that removes most of the observed cost, the worker
  becomes a smaller change against a smaller problem — measure before building
  both.

## Out of scope

**Regeneration itself.** `Document::Regenerate` is synchronous on the calling
thread and stays that way here. Making it async is not a job-registry question
— it is a question about the document model's single-threaded mutation
assumption, which checkpoints, undo, the golden corpus and every RPC handler
rest on. That is A2's neighbourhood, not this one.

## Gates

- The two bugs get a test each: a sync job completes with the History panel
  closed, and a study job is reaped after leaving Simulation mode. Both are
  headless-testable through the actions layer (`tests/test_actions.cpp`).
- `jobs` and `cancel_job` join `rpc_params_gate` like every other RPC.
- The CAM adaptive job asserts what the freeze cost: generating an adaptive
  operation does not block a frame. The GUI suite can measure that directly —
  it already asserts frame behaviour in `shell/idle_throttle_rests_and_input_wakes`.
