#include "ui/SyncJob.h"

#include "common/Logger.h"
#include "ui/AppActions.h"
#include "ui/AppState.h"
#include "ui/ConflictsPanel.h"
#include "ui/HistoryPanel.h"

#include <utility>

namespace ui {

namespace {

// The worker. Touches nothing but its own job and `.git/`.
void Run(SyncJob* job) {
  switch (job->kind) {
  case SyncJob::Kind::Fetch:
    job->ok = GitVault::Fetch(job->dir, job->remote, &job->error);
    break;
  case SyncJob::Kind::Push:
    job->push = GitVault::Push(job->dir, job->remote, job->branch,
                               job->setUpstream, &job->error);
    job->ok = job->push == GitVault::PushResult::Pushed ||
              job->push == GitVault::PushResult::UpToDate;
    break;
  case SyncJob::Kind::Clone:
    job->ok = GitVault::Clone(job->url, job->dir, &job->error);
    break;
  }
  // Last, and with release ordering: everything above must be visible to the
  // main thread before it sees this.
  job->done.store(true, std::memory_order_release);
}

bool Start(AppState& state, std::unique_ptr<SyncJob> job) {
  if (state.syncJob)
    return false; // one at a time
  state.syncMessage.clear();
  state.syncFailed = false;
  SyncJob* raw = job.get();
  state.syncJob = std::move(job);
  raw->worker = std::thread(Run, raw);
  return true;
}

} // namespace

bool SyncRunning(const AppState& state) { return state.syncJob != nullptr; }

bool StartFetch(AppState& state, const std::string& remote, bool mergeAfter) {
  if (state.projectPath.empty())
    return false;
  auto job = std::make_unique<SyncJob>();
  job->kind = SyncJob::Kind::Fetch;
  job->dir = state.projectPath;
  job->remote = remote;
  job->mergeAfter = mergeAfter;
  return Start(state, std::move(job));
}

bool StartPush(AppState& state, const std::string& remote) {
  if (state.projectPath.empty())
    return false;
  auto job = std::make_unique<SyncJob>();
  job->kind = SyncJob::Kind::Push;
  job->dir = state.projectPath;
  job->remote = remote;
  job->branch = GitVault::CurrentBranch(state.projectPath);
  // Only the first push of a branch needs to say where it belongs.
  job->setUpstream = GitVault::Upstream(state.projectPath).empty();
  return Start(state, std::move(job));
}

bool StartClone(AppState& state, const std::string& url,
                const std::string& dir) {
  auto job = std::make_unique<SyncJob>();
  job->kind = SyncJob::Kind::Clone;
  job->url = url;
  job->dir = dir;
  return Start(state, std::move(job));
}

void PollSync(AppState& state) {
  if (!state.syncJob || !state.syncJob->done.load(std::memory_order_acquire))
    return;
  // Join before reading anything the worker wrote — the atomic's release/
  // acquire pair orders the fields, and the join makes the thread's exit
  // ordered too. SyncJob itself cannot be copied out (an atomic and a thread
  // are neither copyable nor movable), so take what is needed and drop it.
  state.syncJob->worker.join();
  const SyncJob::Kind kind = state.syncJob->kind;
  const std::string dir = state.syncJob->dir;
  const bool mergeAfter = state.syncJob->mergeAfter;
  const bool ok = state.syncJob->ok;
  const std::string jobError = state.syncJob->error;
  const GitVault::PushResult pushResult = state.syncJob->push;
  state.syncJob.reset();

  state.syncFailed = !ok;
  if (!ok) {
    state.syncMessage = jobError;
    LOG_WARN("Sync failed: {}", jobError);
    return;
  }

  switch (kind) {
  case SyncJob::Kind::Fetch: {
    state.divergence = GitVault::AheadBehind(state.projectPath);
    if (!mergeAfter) {
      state.syncMessage =
          state.divergence.behind > 0
              ? std::to_string(state.divergence.behind) + " to get"
              : "Up to date";
      return;
    }
    // The merge half, on the main thread where it belongs: it moves the
    // working tree under the open document.
    const std::string upstream = GitVault::Upstream(state.projectPath);
    if (upstream.empty()) {
      state.syncMessage = "this branch does not track a remote yet";
      state.syncFailed = true;
      return;
    }
    std::string error;
    if (!MergeBranch(state, upstream, &error)) {
      state.syncMessage = error;
      state.syncFailed = true;
      return;
    }
    InvalidateConflicts(state);
    state.divergence = GitVault::AheadBehind(state.projectPath);
    // A conflicted merge is an outcome, not a failure — the Conflicts panel
    // appears on its own and takes it from there.
    state.syncMessage = GitVault::InMerge(state.projectPath)
                            ? "Merged with conflicts to resolve"
                            : "Up to date";
    break;
  }
  case SyncJob::Kind::Push:
    state.divergence = GitVault::AheadBehind(state.projectPath);
    state.syncMessage = pushResult == GitVault::PushResult::UpToDate
                            ? "Nothing to send"
                            : "Sent";
    break;
  case SyncJob::Kind::Clone: {
    std::string error;
    if (!OpenProject(state, dir, &error)) {
      state.syncMessage = error;
      state.syncFailed = true;
      return;
    }
    MarkProjectOpened(state, dir);
    state.syncMessage = "Cloned";
    break;
  }
  }
  state.remotesStamp = -1.0;
}

} // namespace ui
