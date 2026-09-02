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

#include "ui/BackgroundJob.h"

#include <memory>
#include <string>
#include <vector>

namespace ui {

struct AppState;

// Join whatever finished, apply it, advance whatever is sliced. Safe to call
// with nothing running (it is, most frames) and safe on a tab with no
// document.
void PollBackgroundJobs(AppState& state);

// --- The registry (docs/ui/long-operations.md, phase 1) --------------------
//
// `state.backgroundJobs`, polled ONLY from PollBackgroundJobs. A job is
// started with its `thread` already running (or about to be: `Start` does
// not launch it, the domain does, so the domain decides what the worker
// captures). When `done` is seen the thread is joined, `finish` runs on the
// main thread, and the entry is dropped once `finish` returns true.

// Join and finish whatever is done. Called by PollBackgroundJobs, and by
// JoinBackgroundJobs when a synchronous caller cannot wait for the loop.
void ReapBackgroundJobs(AppState& state);

// Registers `job` and returns it. The pointer stays valid until the job's
// `finish` has returned true (or `state.backgroundJobs` is cleared).
BackgroundJob& StartBackgroundJob(AppState& state,
                                  std::unique_ptr<BackgroundJob> job);

// Whether a job of `kind` is registered (running or waiting to be reaped).
[[nodiscard]] bool BackgroundJobRunning(const AppState& state,
                                        const std::string& kind);
[[nodiscard]] BackgroundJob* FindBackgroundJob(AppState& state,
                                               const std::string& kind);

// Ask every job of `kind` to stop. Returns how many were asked; a job that
// cannot be interrupted is still asked (it finishes the piece it is on, and
// the domain's `finish` reads `cancel` to decide what NOT to start next).
size_t CancelBackgroundJobs(AppState& state, const std::string& kind);

// Block until every job of `kind` has finished and been applied — the
// synchronous RPC path ("settle first, then answer"), never the frame loop.
void JoinBackgroundJobs(AppState& state, const std::string& kind);

struct BackgroundJobInfo {
  std::string kind, label;
  bool cancellable = false;
  double fraction = -1.0;
  double elapsedSeconds = 0.0;
};
[[nodiscard]] std::vector<BackgroundJobInfo>
ListBackgroundJobs(const AppState& state);

} // namespace ui
