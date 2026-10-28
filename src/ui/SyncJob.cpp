// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/SyncJob.h"

#include "common/Logger.h"
#include "ui/AppState.h"
#include "ui/BackgroundJobs.h"
#include "ui/ConflictsPanel.h"
#include "ui/HistoryPanel.h"
#include "ui/ProjectActions.h"

#include <memory>
#include <thread>
#include <utility>

namespace ui {

namespace {

constexpr const char* kKind = "sync";

// The worker. Touches nothing but its own job and `.git/`.
void Run(SyncJob& job) {
  switch (job.kind) {
  case SyncJob::Kind::Fetch:
    job.ok = GitVault::Fetch(job.dir, job.remote, &job.error);
    break;
  case SyncJob::Kind::Push:
    job.push = GitVault::Push(job.dir, job.remote, job.branch, job.setUpstream,
                              &job.error);
    job.ok = job.push == GitVault::PushResult::Pushed ||
             job.push == GitVault::PushResult::UpToDate;
    break;
  case SyncJob::Kind::Clone:
    job.ok = GitVault::Clone(job.url, job.dir, &job.error);
    break;
  }
}

// What the join used to do: the result, applied on the main thread.
void Apply(AppState& state, const SyncJob& job, bool cancelled);

bool Start(AppState& state, SyncJob sync, const std::string& label) {
  if (BackgroundJobRunning(state, kKind))
    return false; // one at a time
  state.syncMessage.clear();
  state.syncFailed = false;
  auto shared = std::make_shared<SyncJob>(std::move(sync));
  auto job = std::make_unique<BackgroundJob>();
  job->kind = kKind;
  job->label = label;
  // git itself has no interruption points, so a cancel lets the transfer
  // finish -- what it fetched or cloned is kept -- and stops the step that
  // would change the open project after it: a pull's merge, a clone's open
  // (docs/ui/long-operations.md#the-cancel-rule).
  job->cancellable = true;
  BackgroundJob* raw = job.get();
  job->thread = std::thread([shared, raw] {
    Run(*shared);
    // Last: everything Run wrote must be visible to the main thread before
    // it sees this (BackgroundJob::done is seq_cst).
    raw->done.store(true);
  });
  job->finish = [shared, raw](AppState& s) {
    Apply(s, *shared, raw->cancel.load());
    return true;
  };
  StartBackgroundJob(state, std::move(job));
  return true;
}

} // namespace

bool SyncRunning(const AppState& state) {
  return BackgroundJobRunning(state, kKind);
}

bool StartFetch(AppState& state, const std::string& remote, bool mergeAfter) {
  if (state.projectPath.empty())
    return false;
  SyncJob job;
  job.kind = SyncJob::Kind::Fetch;
  job.dir = state.projectPath;
  job.remote = remote;
  job.mergeAfter = mergeAfter;
  return Start(state, std::move(job),
               (mergeAfter ? "Pulling from " : "Fetching ") + remote);
}

bool StartPush(AppState& state, const std::string& remote) {
  if (state.projectPath.empty())
    return false;
  SyncJob job;
  job.kind = SyncJob::Kind::Push;
  job.dir = state.projectPath;
  job.remote = remote;
  job.branch = GitVault::CurrentBranch(state.projectPath);
  // Only the first push of a branch needs to say where it belongs.
  job.setUpstream = GitVault::Upstream(state.projectPath).empty();
  return Start(state, std::move(job), "Sending to " + remote);
}

bool StartClone(AppState& state, const std::string& url,
                const std::string& dir) {
  SyncJob job;
  job.kind = SyncJob::Kind::Clone;
  job.url = url;
  job.dir = dir;
  return Start(state, std::move(job), "Cloning " + url);
}

namespace {
void Apply(AppState& state, const SyncJob& job, bool cancelled) {
  // On the main thread, after the registry joined the worker: every field
  // the worker wrote is ordered before `done`, and the join orders the
  // thread's exit too.
  const SyncJob::Kind kind = job.kind;
  const std::string dir = job.dir;
  const bool mergeAfter = job.mergeAfter;
  const bool ok = job.ok;
  const std::string jobError = job.error;
  const GitVault::PushResult pushResult = job.push;

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
    if (cancelled) {
      state.syncMessage = "Fetched; the merge was cancelled";
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
    if (cancelled) {
      state.syncMessage = "Cloned into " + dir + "; not opened (cancelled)";
      break;
    }
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
} // namespace

} // namespace ui
