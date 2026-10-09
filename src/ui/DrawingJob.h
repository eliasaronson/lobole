// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// Drawing projection off the frame
// (docs/drawings/README.md#projection-off-the-frame).
//
// A `drawing` job projects the views of one drawing tab, one view per job:
// each job's worker projects its view on the worker's own copies of the
// source shapes, and its `finish` lands that view on the sheet and starts the
// job for the next, so the views appear one at a time and the frame never
// waits for hidden-line removal. The job takes the session's projection and
// cut caches for its life and gives them back when the last view has landed
// or it was cancelled.
//
// What a view is -- gathering its bodies, assembling it onto the sheet -- is
// DrawingActions.cpp's; this is only how the expensive half gets off the
// frame.

#include "ui/AppState.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ui {

// One view's work order: gathered on the main thread, run by whoever holds
// the caches (the worker, or UpdateDrawingRendersNow inline), landed on the
// main thread.
struct DrawingViewWork {
  uint64_t viewId = 0;
  std::string name;
  drawing::ProjectionInputs inputs; // the worker's own copies
  DrawingView view;                 // what it projects (main thread only)
  // The projection saved with the drawing, installed instead of projecting
  // when its input hash is this one's.
  std::shared_ptr<const drawing::ViewProjection> candidate;
  uint64_t sourceVersion = 0;
  std::string gatherError; // set: nothing to project, this lands as is
  // Out:
  std::shared_ptr<const drawing::ViewProjection> out;
  std::string error;
  bool reused = false; // the saved projection answered: no HLR ran
  double seconds = 0.0;
};

// Hashes the inputs; installs the candidate when the hashes agree, else cuts
// and projects. Any thread that holds `cache` and `cuts` (either may be
// null).
void RunDrawingViewWork(
    DrawingViewWork& work, drawing::ProjectionCache* cache,
    drawing::CutCache* cuts, const std::function<bool()>& cancelled,
    const std::function<void(const drawing::ProjectionProgress&)>& progress);

// Starts projecting `work`, views of the drawing in slot `slotId`, on
// `drawing` jobs. `budgetSeconds` > 0 bounds the whole run (update_drawing's
// `budget_s`). Takes the session's caches; there is one drawing run at a
// time, and the caller has checked none is running.
void StartDrawingJob(AppState& state, uint64_t slotId,
                     std::vector<DrawingViewWork> work, double budgetSeconds);

// The run in flight, for the panel's bar and the RPC.
struct DrawingJobStatus {
  uint64_t slotId = 0;
  std::string view;  // the view projecting now
  int viewIndex = 0; // 0-based, of viewCount
  int viewCount = 0;
  int pairsDone = 0, pairsOwed = 0;
  double fraction = 0.0; // views landed plus this one's share
  bool cancelAsked = false;
};
[[nodiscard]] std::optional<DrawingJobStatus>
RunningDrawingJob(const AppState& state);

// Asks the run in flight to stop: the views that landed stay, the rest keep
// their last geometry (or placeholder) and say "projection cancelled".
void CancelDrawingJob(AppState& state);

} // namespace ui
