#pragma once

// Advancing every long-running operation, from ONE place per loop.
//
// The app runs five of these (docs/ui/long-operations.md): a STEP import, a
// topology optimization, a static/modal solve, a git fetch/push/clone, and the
// offline path tracer. Each has its own worker and its own `Poll…` that joins
// it and applies the result on the main thread — which is right, and is not
// what this file changes.
//
// What it changes is WHO calls them. They were reached from wherever seemed
// natural at the time, and two of those places are conditional:
//
//   - `PollSync` was called inside `if (ImGui::TreeNode("Sharing"))` in the
//     History panel. Collapsing that section — or closing the panel — during a
//     push meant nothing ever joined the worker or reported the result. The
//     operation finished and the app never found out.
//   - `PollTopologyRun`/`PollStudyRun` were called from `DrawTopoResults`,
//     which only runs while Simulation mode is active. Leaving that mode
//     mid-solve stranded the result until you came back. An RPC client still
//     reaped it, which is why this survived: invisible to every scripted test,
//     and only a person clicking ever saw it.
//
// So whether a background operation can finish is not a property of which
// panel is open. `PollBackgroundJobs` is called once per iteration by each of
// the app's three loops — the windowed frame loop, the `--offscreen` frame
// loop, and the `--headless` pump — and nowhere else.
//
// The RPC layer keeps its own poll calls: a handler must apply a finished
// result BEFORE it answers a question about it, and a client that asks
// `study_status` between two ticks of the headless pump should not get a
// stale answer merely because it asked early.

namespace ui {

struct AppState;

// Join whatever finished, apply it, advance whatever is sliced. Safe to call
// with nothing running (it is, most frames) and safe on a tab with no
// document.
void PollBackgroundJobs(AppState& state);

} // namespace ui
