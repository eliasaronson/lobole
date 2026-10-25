// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// The periodic disk and git questions, asked off the frame
// (docs/versioning/README.md#the-poll-runs-off-the-frame).
//
// Three questions are asked on a clock rather than by a user: has the project
// changed on disk (F8, every few seconds), what does the history look like
// (while the History panel is drawn), and where do the remotes stand (while
// its Sharing section is open). Each costs a `git` process, and the first a
// walk of the whole project directory: 41 ms on a 4 100-file project on
// Linux, several times that on Windows, where it was a dropped frame every
// three seconds. None of the answers is needed THIS frame.
//
// So the frame builds a request out of copies (the path, the stamp to compare
// against, the HEAD it last saw, the cache it holds), a worker answers it, and
// a later frame applies the answer — unless the question went stale in the
// meantime: another project opened, the project saved or re-read (the disk
// generation moved), or the panel refreshed its cache synchronously after an
// action (the cache's stamp moved). A stale answer is DISCARDED, never applied
// to what is open now.
//
// The synchronous checks stay where they guard a write: a save, a share and
// the `project_changed` RPC each walk the directory themselves.

#include "core/io/GitVault.h"
#include "core/io/ProjectMerge.h"
#include "core/io/ProjectStamp.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace ui {

struct AppState;

// How many versions the History panel lists, and so how many a log refresh
// asks for — the panel's own refresh and the poller's. Long enough that a
// day's work fits; git holds the rest, and "restore something older" is a job
// for the tool that owns the whole history.
constexpr int kHistoryVersionsShown = 60;

struct DiskPollRequest {
  std::string projectPath;

  // F8: has the project changed on disk? Set when the disk is due.
  bool disk = false;
  uint64_t diskGeneration = 0;    // AppState::diskGeneration when asked
  DocumentIO::ProjectStamp stamp; // what the directory looked like
  bool stampUsable = false;       // a stamp of THIS path to compare with
  std::string head;               // HEAD when stamped; empty: no cheap check

  // The History panel's log and "saved, not yet a version".
  bool history = false;
  double historyStamp = -1.0; // AppState::historyStamp when asked
  size_t knownLogSize = 0;    // the cache, to decide whether to pay for
  std::string knownLogHead;   // LogWithChanges

  // The Sharing section's remotes and ahead/behind counts.
  bool remotes = false;
  double remotesStamp = -1.0; // AppState::remotesStamp when asked

  // The conflicts scan (F7): git's unmerged paths plus the duplicate ids,
  // asked when something could have changed the answer (conflictsStale).
  bool conflicts = false;
  uint64_t conflictsGeneration = 0; // AppState::conflictsGeneration when asked
};

struct DiskPollAnswer {
  DiskPollRequest asked;

  bool diskChanged = false;

  bool logChanged = false; // `log` replaces the cache; otherwise kept
  std::vector<GitVault::LogEntry> log;
  bool uncommitted = false;

  std::vector<GitVault::Remote> remoteList;
  GitVault::Divergence divergence;

  ProjectMerge::Report conflictReport;
};

// The worker's half: answers `request` from the filesystem and `git`. Touches
// nothing but its argument, so it runs on any thread.
DiskPollAnswer AnswerDiskPoll(const DiskPollRequest& request);

// One poll in flight at a time, on its own thread. Not a BackgroundJob: a
// poll is not something the user started or can cancel, and on the registry
// it would show a progress bar every few seconds and hold the idle throttle
// awake while it ran (docs/ui/long-operations.md#what-is-not-on-the-registry).
class DiskPoller {
public:
  using Answerer = std::function<DiskPollAnswer(const DiskPollRequest&)>;

  DiskPoller() = default;
  DiskPoller(const DiskPoller&) = delete;
  DiskPoller& operator=(const DiskPoller&) = delete;
  ~DiskPoller(); // joins: quitting mid-poll waits for one git call or walk

  bool Busy() const { return m_thread.joinable(); }
  // A poll is in flight and it includes the conflicts scan.
  bool ScanningConflicts() const { return Busy() && m_conflicts; }

  // Starts answering `request` on a worker; refused (false) while one is in
  // flight. `answer` is AnswerDiskPoll unless a test holds it.
  bool Start(DiskPollRequest request, Answerer answer = AnswerDiskPoll);

  // The answer, once the worker has finished; nothing while it runs. Never
  // blocks.
  std::optional<DiskPollAnswer> Take();

private:
  std::thread m_thread;
  std::atomic<bool> m_done{false};
  bool m_conflicts = false; // the poll in flight scans for conflicts
  DiskPollAnswer m_answer;  // written by the worker before m_done
};

// Applies `answer` to `state` if it still describes what is open; returns
// false (and changes nothing) when it went stale while it was being asked.
// Each part is checked on its own: a save between the start and the answer
// discards the disk part but keeps the remotes.
bool ApplyDiskPollAnswer(AppState& state, const DiskPollAnswer& answer);

// True while `state.conflicts` may not describe the tree: a scan is owed
// (stale, or for another project) or one is in flight. The Conflicts panel
// draws the last answer meanwhile, with its row actions disabled; anything
// that DECIDES on conflicts scans for itself (FinishMerge, the merge RPCs).
bool ConflictScanPending(const AppState& state);

// The frame's half, once a frame from the top of the UI (replacing the
// synchronous poll): applies an answer that has arrived, then starts a poll
// if one is due and none is running. Never waits on the disk or on `git`.
void PumpDiskPoll(AppState& state);

} // namespace ui
