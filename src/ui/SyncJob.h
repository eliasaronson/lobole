#pragma once

#include "core/io/GitVault.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace ui {

struct AppState;

// A network git operation running off the UI thread (F9 C2).
//
// Fetch, push and clone are the only slow, blocking things GitVault does, and
// they touch `.git/` only — never the working tree, never the Document — so
// the cross-thread surface is this struct and nothing else.
//
// Merge is deliberately NOT here. It is local and fast, and it moves the
// working tree under an open document, so it belongs on the main thread where
// it stays ordered with the UI. "Pull" is therefore fetch (worker) followed
// by merge (main thread) — which is also why a conflicted pull can raise the
// Conflicts panel with no thread involved at all.
//
// Same shape as TopoJob, deliberately: a worker plus atomics the main thread
// polls, and a single join. One at a time.
struct SyncJob {
  enum class Kind { Fetch, Push, Clone };

  Kind kind = Kind::Fetch;
  // Copied in before the worker starts; never touched by the main thread
  // while it runs.
  std::string dir, remote, branch, url;
  bool setUpstream = false;
  // Fetch only: merge the upstream in once the refs are down. This is what
  // makes "Pull" one button while keeping the merge on the main thread.
  bool mergeAfter = false;

  std::atomic<bool> done{false}; // worker sets last

  // Read by the main thread only after `done` (and the join).
  bool ok = false;
  std::string error;
  GitVault::PushResult push = GitVault::PushResult::Failed;

  std::thread worker;

  // A joinable thread destructing calls std::terminate, so quitting the app
  // mid-fetch would take it down rather than close it. The same guard
  // TopoJob has — and unlike the optimizer there is nothing to cancel: a
  // fetch or push runs to its own timeout and touches only `.git/`, so
  // waiting for it is both safe and short.
  ~SyncJob() {
    if (worker.joinable())
      worker.join();
  }
  SyncJob() = default;
  SyncJob(const SyncJob&) = delete;
  SyncJob& operator=(const SyncJob&) = delete;
};

// Start one. Refused (returns false) while another is running — one at a
// time, like the topology runner.
bool StartFetch(AppState& state, const std::string& remote, bool mergeAfter);
bool StartPush(AppState& state, const std::string& remote);
bool StartClone(AppState& state, const std::string& url,
                const std::string& dir);

// Called once a frame: joins a finished job and applies its result on the
// main thread (the merge half of a pull, opening a fresh clone, re-stamping).
void PollSync(AppState& state);

// A line for the panel: what is happening, or what went wrong.
bool SyncRunning(const AppState& state);

} // namespace ui
