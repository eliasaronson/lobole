// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "core/io/GitVault.h"

#include <string>

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
// The thread and the completion live in the registry (ui/BackgroundJobs.h,
// kind "sync") since A3 phase 2; this struct is the worker's whole world —
// plain data, copied in, read back on the main thread after the join. One at
// a time.
struct SyncJob {
  enum class Kind { Fetch, Push, Clone };
  Kind kind = Kind::Fetch;
  std::string dir, remote, branch, url;
  bool setUpstream = false;
  bool mergeAfter = false;
  bool ok = false;
  std::string error;
  GitVault::PushResult push = GitVault::PushResult::Failed;
};

bool StartFetch(AppState& state, const std::string& remote, bool mergeAfter);
bool StartPush(AppState& state, const std::string& remote);
bool StartClone(AppState& state, const std::string& url,
                const std::string& dir);

// Called once a frame: joins a finished job and applies its result on the
// main thread (the merge half of a pull, opening a fresh clone, re-stamping).

// A line for the panel: what is happening, or what went wrong.
bool SyncRunning(const AppState& state);

} // namespace ui
