#include "ui/BackgroundJobs.h"

#include "ui/AppState.h"
#include "ui/RendererSync.h"

#include <algorithm>
#include <chrono>

namespace ui {

void PollBackgroundJobs(AppState& state) {
  // Order is not load-bearing — no job's completion feeds another's — but it
  // is kept stable so a log read across frames is comparable.

  // Advance an in-flight offline render by one sample batch. Not a worker
  // thread at all: it slices on the main thread with its own one-shot submits
  // (docs/ui/long-operations.md#what-is-not-on-the-registry says why that is
  // the one candidate for a real background render thread later).
  PumpRenderJob(state);

  // (A git fetch/push/clone is a "sync" entry in the registry below — A3
  // phase 2 — and was the one polled from a collapsible TreeNode.)

  // Everything else — the FEM solves and meshes, a git sync, an adaptive
  // pass — is a registry entry whose `finish` knows which document it
  // belongs to. (The FEM polls used to take "the active document", so a
  // solve started on a part tab was not reaped while an assembly tab was
  // active; phase 3 gave the job its part slot id instead.)
  ReapBackgroundJobs(state);
}

// --- The registry ------------------------------------------------------------

void ReapBackgroundJobs(AppState& state) {
  // By index, and only over the entries that existed when this started: a
  // `finish` is allowed to start the NEXT job (that is how a queue of
  // adaptive passes advances), and that lands at the back of the vector.
  const size_t had = state.backgroundJobs.size();
  for (size_t i = 0; i < had && i < state.backgroundJobs.size(); i++) {
    BackgroundJob& job = *state.backgroundJobs[i];
    if (!job.done.load())
      continue;
    if (job.thread.joinable())
      job.thread.join();
    if (job.finish && !job.finish(state))
      continue; // asked to be called again next frame
    job.finish = nullptr;
    job.reaped = true;
    // How it ended, kept for a status asked by id after the end.
    FinishedJob f;
    f.id = job.id;
    f.kind = job.kind;
    f.label = job.label;
    f.studyId = job.studyId;
    f.error = job.error;
    f.cancelReason = job.cancelReason;
    f.outcome = !job.error.empty()  ? "failed"
                : job.cancel.load() ? "cancelled"
                                    : "done";
    f.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                              job.started)
                    .count();
    state.finishedJobs.push_back(std::move(f));
    while (state.finishedJobs.size() > 64)
      state.finishedJobs.pop_front();
  }
  std::erase_if(
      state.backgroundJobs,
      [](const std::unique_ptr<BackgroundJob>& j) { return j->reaped; });
}

BackgroundJob& StartBackgroundJob(AppState& state,
                                  std::unique_ptr<BackgroundJob> job) {
  job->started = std::chrono::steady_clock::now();
  job->id = state.nextJobId++;
  state.backgroundJobs.push_back(std::move(job));
  return *state.backgroundJobs.back();
}

bool BackgroundJobRunning(const AppState& state, const std::string& kind) {
  for (const std::unique_ptr<BackgroundJob>& j : state.backgroundJobs)
    if (j->kind == kind && !j->reaped)
      return true;
  return false;
}

BackgroundJob* FindBackgroundJob(AppState& state, const std::string& kind) {
  for (std::unique_ptr<BackgroundJob>& j : state.backgroundJobs)
    if (j->kind == kind && !j->reaped)
      return j.get();
  return nullptr;
}

size_t CancelBackgroundJobs(AppState& state, const std::string& kind) {
  size_t asked = 0;
  for (std::unique_ptr<BackgroundJob>& j : state.backgroundJobs)
    if (j->kind == kind && !j->reaped) {
      j->cancel.store(true);
      asked++;
    }
  return asked;
}

void JoinBackgroundJobs(AppState& state, const std::string& kind) {
  // A `finish` may start a successor of the same kind, so loop until none is
  // left rather than joining the ones present now.
  for (;;) {
    BackgroundJob* job = FindBackgroundJob(state, kind);
    if (job == nullptr)
      return;
    if (job->thread.joinable())
      job->thread.join();
    else if (!job->done.load())
      return; // no thread and not done: a sliced job that only the loop
              // advances, and waiting here would wait forever
    job->done.store(true);
    ReapBackgroundJobs(state);
  }
}

std::vector<BackgroundJobInfo> ListBackgroundJobs(const AppState& state) {
  std::vector<BackgroundJobInfo> out;
  const auto now = std::chrono::steady_clock::now();
  for (const std::unique_ptr<BackgroundJob>& j : state.backgroundJobs) {
    if (j->reaped)
      continue;
    BackgroundJobInfo info;
    info.id = j->id;
    info.owner = j->owner;
    info.studyId = j->studyId;
    info.kind = j->kind;
    info.label = j->label;
    info.cancellable = j->cancellable;
    info.fraction = j->fraction.load();
    info.elapsedSeconds =
        std::chrono::duration<double>(now - j->started).count();
    out.push_back(std::move(info));
  }
  return out;
}

BackgroundJob* FindBackgroundJobById(AppState& state, uint64_t id) {
  for (std::unique_ptr<BackgroundJob>& j : state.backgroundJobs)
    if (j->id == id && !j->reaped)
      return j.get();
  return nullptr;
}

const FinishedJob* FinishedJobById(const AppState& state, uint64_t id) {
  for (const FinishedJob& f : state.finishedJobs)
    if (f.id == id)
      return &f;
  return nullptr;
}

bool CancelBackgroundJobById(AppState& state, uint64_t id,
                             const std::string& reason) {
  BackgroundJob* job = FindBackgroundJobById(state, id);
  if (job == nullptr)
    return false;
  if (!job->cancel.exchange(true))
    job->cancelReason = reason;
  return true;
}

size_t CancelJobsOwnedBy(AppState& state, uint64_t owner,
                         const std::string& reason) {
  if (owner == 0)
    return 0;
  size_t asked = 0;
  for (std::unique_ptr<BackgroundJob>& j : state.backgroundJobs)
    if (j->owner == owner && !j->reaped) {
      if (!j->cancel.exchange(true))
        j->cancelReason = reason;
      asked++;
    }
  return asked;
}

} // namespace ui
