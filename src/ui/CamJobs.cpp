#include "ui/CamJobs.h"
#include "core/cam/CamToolpath.h"

#include "ui/AppState.h"
#include "ui/BackgroundJobs.h"
#include "ui/CamPanel.h"
#include "ui/CamTool.h"
#include "ui/JobActions.h"

#include <memory>
#include <utility>

#ifdef HAVE_CAM

namespace ui {

namespace {

constexpr const char* kKind = "cam";

// What the worker and the main thread share: the request going in, the
// result coming out. Owned by a shared_ptr so the thread lambda and the
// finish continuation both hold it without either owning the other.
struct Pass {
  cam::AdaptiveRequest request;
  cam::AdaptiveResult result;
};

struct SurfacePassJob {
  cam::SurfaceRequest request;
  cam::SurfaceResult result;
};

void StartNextCamPass(AppState& state);

void StartNextAdaptivePass(AppState& state) {
  while (state.camAdaptiveInFlight == 0 && !state.camAdaptiveQueue.empty()) {
    auto pass = std::make_shared<Pass>();
    pass->request = std::move(state.camAdaptiveQueue.front());
    state.camAdaptiveQueue.pop_front();
    // A synchronous path may have filled it meanwhile; nothing to cut.
    if (state.camAdaptiveCache.Contains(pass->request.key)) {
      state.camPassBatchDone++;
      continue;
    }
    const uint64_t key = pass->request.key;
    auto job = std::make_unique<BackgroundJob>();
    job->kind = kKind;
    job->cancellable = false; // libarea's Execute has no interruption points
    job->label = "Clearing '" + pass->request.op.name + "', pass " +
                 std::to_string(state.camPassBatchDone + 1) + " of " +
                 std::to_string(state.camPassBatchTotal);
    job->fraction.store(state.camPassBatchTotal == 0
                            ? -1.0
                            : static_cast<double>(state.camPassBatchDone) /
                                  static_cast<double>(state.camPassBatchTotal));
    BackgroundJob* raw = job.get();
    job->thread = std::thread([pass, raw] {
      pass->result = cam::AdaptiveClear(pass->request);
      raw->done.store(true); // last
    });
    job->finish = [pass, raw, key](AppState& s) -> bool {
      s.camAdaptiveInFlight = 0;
      s.camAdaptiveCache.Put(key, std::move(pass->result));
      s.camPassBatchDone++;
      const bool cancelled = raw->cancel.load();
      if (cancelled) {
        // The result is kept (paid for); nothing else is started, and the
        // toolpaths were already told (CancelCamPasses).
        s.camAdaptiveQueue.clear();
      } else if (s.job != nullptr) {
        // Whichever job tab is active regenerates from ITS document: the
        // landed pass fills in where its key is read, and anything still
        // owed is queued again (deduplicated) — which is also how an edit
        // made during the pass gets its own passes cut next.
        RefreshCamToolpaths(s);
      }
      if (!s.camAdaptiveQueue.empty() || !s.camSurfaceQueue.empty()) {
        StartNextCamPass(s);
      } else {
        s.camPassBatchDone = 0;
        s.camPassBatchTotal = 0;
        if (s.camVerifyWhenSettled && !cancelled) {
          s.camVerifyWhenSettled = false;
          RunCamVerification(s);
        }
      }
      return true;
    };
    state.camAdaptiveInFlight = key;
    StartBackgroundJob(state, std::move(job));
    return;
  }
}

// One surfacing operation on the worker. Minutes, not seconds -- the rotary
// rolling pin's finishing pass alone is 531,432 moves -- and it ran on the
// frame until phase 5.
void StartNextSurfacePass(AppState& state) {
  while (state.camSurfaceInFlight == 0 && !state.camSurfaceQueue.empty()) {
    auto pass = std::make_shared<SurfacePassJob>();
    pass->request = std::move(state.camSurfaceQueue.front());
    state.camSurfaceQueue.pop_front();
    if (state.camSurfaceCache.Contains(pass->request.key)) {
      state.camPassBatchDone++;
      continue;
    }
    const uint64_t key = pass->request.key;
    auto job = std::make_unique<BackgroundJob>();
    job->kind = kKind;
    job->cancellable = false; // the drop cutter has no interruption points
    job->label = "Finishing '" + pass->request.op.name + "', " +
                 std::to_string(state.camPassBatchDone + 1) + " of " +
                 std::to_string(state.camPassBatchTotal);
    job->fraction.store(state.camPassBatchTotal == 0
                            ? -1.0
                            : static_cast<double>(state.camPassBatchDone) /
                                  static_cast<double>(state.camPassBatchTotal));
    BackgroundJob* raw = job.get();
    job->thread = std::thread([pass, raw] {
      pass->result = cam::SurfacePass(pass->request);
      raw->done.store(true); // last
    });
    job->finish = [pass, raw, key](AppState& s) -> bool {
      s.camSurfaceInFlight = 0;
      s.camSurfaceCache.Put(key, std::move(pass->result));
      s.camPassBatchDone++;
      const bool cancelled = raw->cancel.load();
      if (cancelled) {
        s.camSurfaceQueue.clear();
        s.camAdaptiveQueue.clear();
      } else if (s.job != nullptr) {
        RefreshCamToolpaths(s);
      }
      if (!s.camAdaptiveQueue.empty() || !s.camSurfaceQueue.empty()) {
        StartNextCamPass(s);
      } else {
        s.camPassBatchDone = 0;
        s.camPassBatchTotal = 0;
        if (s.camVerifyWhenSettled && !cancelled) {
          s.camVerifyWhenSettled = false;
          RunCamVerification(s);
        }
      }
      return true;
    };
    state.camSurfaceInFlight = key;
    StartBackgroundJob(state, std::move(job));
    return;
  }
}

// Clearing first, then surfacing. Not arbitrary: a finishing pass is planned
// against the part, not against what roughing left, so neither waits on the
// other -- but clearing is seconds and surfacing is minutes, and filling the
// cheap ones first means the backplot grows steadily instead of sitting
// empty through the long one.
void StartNextCamPass(AppState& state) {
  StartNextAdaptivePass(state);
  StartNextSurfacePass(state);
  if (state.camAdaptiveInFlight == 0 && state.camAdaptiveQueue.empty() &&
      state.camSurfaceInFlight == 0 && state.camSurfaceQueue.empty()) {
    state.camPassBatchDone = 0;
    state.camPassBatchTotal = 0;
  }
}

} // namespace

void QueueAdaptivePasses(AppState& state,
                         std::vector<cam::AdaptiveRequest> pending) {
  for (cam::AdaptiveRequest& req : pending) {
    if (state.camAdaptiveCache.Contains(req.key) ||
        req.key == state.camAdaptiveInFlight)
      continue;
    bool queued = false;
    for (const cam::AdaptiveRequest& q : state.camAdaptiveQueue)
      queued |= q.key == req.key;
    if (queued)
      continue;
    state.camAdaptiveQueue.push_back(std::move(req));
    state.camPassBatchTotal++;
  }
  StartNextCamPass(state);
}

void QueueSurfacePasses(AppState& state,
                        std::vector<cam::SurfaceRequest> pending) {
  for (cam::SurfaceRequest& req : pending) {
    if (state.camSurfaceCache.Contains(req.key) ||
        req.key == state.camSurfaceInFlight)
      continue;
    bool queued = false;
    for (const cam::SurfaceRequest& q : state.camSurfaceQueue)
      queued |= q.key == req.key;
    if (queued)
      continue;
    state.camSurfaceQueue.push_back(std::move(req));
    state.camPassBatchTotal++;
  }
  StartNextCamPass(state);
}

bool CamToolpathsPending(const AppState& state) {
  for (const cam::Toolpath& tp : state.camToolpaths)
    if (tp.pending)
      return true;
  return false;
}

std::string CamToolpathsPendingWhy(const AppState& state) {
  size_t pending = 0;
  for (const cam::Toolpath& tp : state.camToolpaths)
    pending += tp.pending ? 1 : 0;
  if (pending == 0)
    return {};
  const CamPassProgress p = CamPassProgressOf(state);
  std::string why = std::to_string(pending) + " toolpath" +
                    (pending == 1 ? " is" : "s are") + " still being cut";
  if (p.total > 0)
    why += " (pass " + std::to_string(p.done + 1) + " of " +
           std::to_string(p.total) + ")";
  why += " -- wait for the passes to land, or cancel the clearing";
  return why;
}

void SettleCamToolpaths(AppState& state) {
  if (state.job == nullptr)
    return;
  // The pass in flight and everything queued behind it, each landing
  // through its own finish (which starts the next): the queue drains on
  // worker threads while this thread waits, so a result computed once is
  // never computed twice.
  JoinBackgroundJobs(state, kKind);
  // Anything still owed — a tab that has never generated, a key evicted —
  // is computed inline.
  const bool defer = state.camDeferPasses;
  state.camDeferPasses = false;
  RefreshCamToolpaths(state);
  state.camDeferPasses = defer;
  if (state.camVerifyWhenSettled) {
    state.camVerifyWhenSettled = false;
    RunCamVerification(state);
  }
}

std::vector<cam::Toolpath> GenerateCamToolpathsNow(AppState& state,
                                                   const cam::Setup& setup) {
  JoinBackgroundJobs(state, kKind);
  cam::GenerateContext ctx;
  ctx.cache = &state.camAdaptiveCache;
  ctx.surfaceCache = &state.camSurfaceCache;
  ctx.bodiesKey = JobRenderVersion(state);
  ctx.defer = false; // the reader's path: compute what is missing, now
  return cam::GenerateAll(setup, JobBodies(state), &ctx);
}

void CancelCamPasses(AppState& state) {
  state.camAdaptiveQueue.clear();
  state.camSurfaceQueue.clear();
  (void)CancelBackgroundJobs(state, kKind);
  for (cam::Toolpath& tp : state.camToolpaths)
    if (tp.pending) {
      tp.pending = false;
      tp.moves.clear();
      tp.reliefs.clear();
      tp.tabs.clear();
      tp.strategy.clear();
      tp.error = "this pass was cancelled before it was cut "
                 "-- press Generate to queue it again";
    }
  state.camVerifyWhenSettled = false;
}

CamPassProgress CamPassProgressOf(const AppState& state) {
  CamPassProgress p;
  p.done = state.camPassBatchDone;
  p.total = state.camPassBatchTotal;
  for (const std::unique_ptr<BackgroundJob>& j : state.backgroundJobs)
    if (j->kind == kKind && !j->reaped) {
      p.running = true;
      p.label = j->label;
    }
  return p;
}

} // namespace ui

#else // !HAVE_CAM

namespace ui {
void QueueAdaptivePasses(AppState&, std::vector<cam::AdaptiveRequest>) {}
bool CamToolpathsPending(const AppState&) { return false; }
std::string CamToolpathsPendingWhy(const AppState&) { return {}; }
void SettleCamToolpaths(AppState&) {}
std::vector<cam::Toolpath> GenerateCamToolpathsNow(AppState&,
                                                   const cam::Setup&) {
  return {};
}
void CancelCamPasses(AppState&) {}
CamPassProgress CamPassProgressOf(const AppState&) { return {}; }
} // namespace ui

#endif
