// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/DrawingJob.h"

#include "cad/CADKernel.h" // ArmOccSignalHandlers
#include "common/Logger.h"
#include "ui/BackgroundJobs.h"
#include "ui/DrawingActions.h"

#include <atomic>
#include <chrono>
#include <thread>

// The `drawing` job (docs/drawings/README.md#projection-off-the-frame). What
// a run's jobs share is a DrawingRun: the work orders, which view is next,
// the caches taken for the run's life, and the progress the panel reads.
// One view per job, the next started from the last one's `finish`, so a
// view lands the frame after its worker is done rather than with the rest.

namespace ui {

constexpr const char* kDrawingKind = "drawing";

struct DrawingRun {
  uint64_t slotId = 0;
  std::vector<DrawingViewWork> work;
  size_t next = 0; // the view projecting now (main thread)
  double budgetSeconds = 0.0;
  std::chrono::steady_clock::time_point started;
  // Taken from AppState for the run's life: only the worker touches them
  // while a view projects, and only `finish` hands them back.
  drawing::ProjectionCache cache;
  drawing::CutCache cuts;
  // The view in flight's progress, written by its worker.
  std::atomic<int> pairsDone{0}, pairsOwed{0};
  std::atomic<bool> budgetOut{false};
  BackgroundJob* job = nullptr; // the current job (main thread)
};

void RunDrawingViewWork(
    DrawingViewWork& w, drawing::ProjectionCache* cache,
    drawing::CutCache* cuts, const std::function<bool()>& cancelled,
    const std::function<void(const drawing::ProjectionProgress&)>& progress) {
  const auto t0 = std::chrono::steady_clock::now();
  w.out = nullptr;
  w.error.clear();
  w.reused = false;
  if (!w.gatherError.empty()) {
    w.error = w.gatherError;
    return;
  }
  // The hash first: it is what a saved projection is checked against, and
  // what the one about to be projected is saved under.
  uint64_t hash = 0;
  try {
    hash = drawing::ProjectionInputHash(w.inputs);
  } catch (const std::exception& e) {
    LOG_WARN("Drawing view '{}': its inputs could not be hashed ({}); it "
             "projects, and is not saved",
             w.name, e.what());
  } catch (...) {
    LOG_WARN("Drawing view '{}': its inputs could not be hashed; it projects, "
             "and is not saved",
             w.name);
  }
  if (w.candidate && hash != 0 && w.candidate->inputHash == hash) {
    w.out = w.candidate; // saved with the drawing, and still the answer
    w.reused = true;
  } else {
    drawing::ProjectRun run;
    run.cache = cache;
    run.cuts = cuts;
    run.cancelled = cancelled;
    run.progress = progress;
    auto projected = drawing::ProjectInputs(w.inputs, run, &w.error);
    if (projected) {
      projected->inputHash = hash;
      w.out = std::make_shared<const drawing::ViewProjection>(
          std::move(*projected));
    }
  }
  w.seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
          .count();
}

namespace {

void StartNextView(AppState& state, const std::shared_ptr<DrawingRun>& run);

// The last view landed, or the run was stopped: the views left land as
// stopped, the caches go back, and whatever waited on the run goes.
void EndRun(AppState& state, const std::shared_ptr<DrawingRun>& run,
            const std::string& why) {
  for (size_t k = run->next; k < run->work.size(); ++k) {
    run->work[k].out = nullptr;
    LandDrawingViewWork(state, run->slotId, run->work[k], why);
  }
  state.drawingProjectionCache = std::move(run->cache);
  state.drawingCutCache = std::move(run->cuts);
  run->job = nullptr;
  if (state.drawingRun == run)
    state.drawingRun = nullptr;
  if (why == "cancelled")
    ++state.drawingJobCancellations;
  LOG_INFO("Drawing projection {} after {:.1f} s ({} of {} views landed)",
           why.empty() ? std::string("done") : why,
           std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                         run->started)
               .count(),
           run->next, run->work.size());
  DrawingRunEnded(state, run->slotId, why == "cancelled");
}

void StartNextView(AppState& state, const std::shared_ptr<DrawingRun>& run) {
  const size_t idx = run->next;
  DrawingViewWork& w = run->work[idx];
  auto job = std::make_unique<BackgroundJob>();
  job->kind = kDrawingKind;
  job->label =
      "Projecting '" + (w.name.empty() ? std::string("view") : w.name) + "'" +
      (run->work.size() > 1 ? " (view " + std::to_string(idx + 1) + " of " +
                                  std::to_string(run->work.size()) + ")"
                            : std::string());
  job->cancellable = true;
  const double n = static_cast<double>(run->work.size());
  job->fraction.store(static_cast<double>(idx) / n);
  BackgroundJob* raw = job.get();
  run->job = raw;
  run->pairsDone.store(0);
  run->pairsOwed.store(0);
  job->thread = std::thread([run, raw, idx, n] {
    ArmOccSignalHandlers();
    const auto cancelled = [run, raw] {
      if (raw->cancel.load())
        return true;
      if (run->budgetSeconds > 0.0 &&
          std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                        run->started)
                  .count() > run->budgetSeconds) {
        run->budgetOut.store(true);
        return true;
      }
      return false;
    };
    const auto progress = [run, raw, idx,
                           n](const drawing::ProjectionProgress& p) {
      run->pairsDone.store(p.pairsDone);
      run->pairsOwed.store(p.pairsOwed);
      const double share = p.stepsOwed > 0
                               ? static_cast<double>(p.stepsDone) / p.stepsOwed
                               : 0.0;
      raw->fraction.store((static_cast<double>(idx) + share) / n);
    };
    // Every shape this touches is the worker's own (the work order's
    // copies) and so are the caches: the frame meshes and picks the
    // documents' shapes meanwhile (docs/ui/long-operations.md).
    if (cancelled())
      run->work[idx].error = "cancelled";
    else
      RunDrawingViewWork(run->work[idx], &run->cache, &run->cuts, cancelled,
                         progress);
    raw->fraction.store(static_cast<double>(idx + 1) / n);
    raw->done.store(true); // last
  });
  job->finish = [run, raw, idx](AppState& s) -> bool {
    DrawingViewWork& done = run->work[idx];
    const bool stopped = raw->cancel.load() || run->budgetOut.load();
    std::string why;
    if (!done.out && (done.error == "cancelled" || stopped))
      why = run->budgetOut.load() ? "budget" : "cancelled";
    LandDrawingViewWork(s, run->slotId, done, why);
    run->next = idx + 1;
    if (!stopped && run->next < run->work.size()) {
      StartNextView(s, run);
      return true;
    }
    EndRun(s, run,
           stopped ? (run->budgetOut.load() ? "budget" : "cancelled") : "");
    return true;
  };
  StartBackgroundJob(state, std::move(job));
}

} // namespace

void StartDrawingJob(AppState& state, uint64_t slotId,
                     std::vector<DrawingViewWork> work, double budgetSeconds) {
  if (work.empty())
    return;
  auto run = std::make_shared<DrawingRun>();
  run->slotId = slotId;
  run->work = std::move(work);
  run->budgetSeconds = budgetSeconds;
  run->started = std::chrono::steady_clock::now();
  run->cache = std::move(state.drawingProjectionCache);
  state.drawingProjectionCache = drawing::ProjectionCache();
  run->cuts = std::move(state.drawingCutCache);
  state.drawingCutCache = drawing::CutCache();
  state.drawingRun = run;
  StartNextView(state, run);
}

std::optional<DrawingJobStatus> RunningDrawingJob(const AppState& state) {
  const std::shared_ptr<DrawingRun>& run = state.drawingRun;
  if (!run || run->next >= run->work.size())
    return std::nullopt;
  DrawingJobStatus st;
  st.slotId = run->slotId;
  st.view = run->work[run->next].name;
  st.viewIndex = static_cast<int>(run->next);
  st.viewCount = static_cast<int>(run->work.size());
  st.pairsDone = run->pairsDone.load();
  st.pairsOwed = run->pairsOwed.load();
  st.fraction = run->job ? run->job->fraction.load() : 0.0;
  st.cancelAsked = run->job && run->job->cancel.load();
  return st;
}

void CancelDrawingJob(AppState& state) {
  CancelBackgroundJobs(state, kDrawingKind);
}

} // namespace ui
