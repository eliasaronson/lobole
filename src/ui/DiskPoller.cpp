// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/DiskPoller.h"

#include "ui/AppState.h"

#include "imgui.h"

namespace ui {

namespace {

// How often the project directory is re-examined. Slow on purpose: the walk
// is cheap per file but a 48-part project is thousands of them, and the
// question "did somebody else write to this" does not need a fast answer —
// it needs a certain one before the next save, which checks for itself.
constexpr double kDiskPollSeconds = 3.0;

// The History panel's log and the Sharing section's remotes, while drawn.
constexpr double kPanelRefreshSeconds = 1.0;

} // namespace

DiskPollAnswer AnswerDiskPoll(const DiskPollRequest& request) {
  DiskPollAnswer answer;
  answer.asked = request;
  const std::string& path = request.projectPath;

  if (request.disk) {
    // HEAD first: every checkout, merge and pull moves it, and reading it is
    // two small files rather than a walk. It does not move for a working-tree
    // edit, which is what the walk is for.
    const std::string head =
        request.head.empty() ? std::string() : GitVault::HeadHash(path);
    if (!head.empty() && head != request.head)
      answer.diskChanged = true;
    else if (request.stampUsable)
      answer.diskChanged =
          !DocumentIO::StampProject(path).Matches(request.stamp);
  }

  if (request.history) {
    // The cheap log first: one process, and it answers "has anything
    // changed" — which is what decides whether to pay for the descriptions.
    std::vector<GitVault::LogEntry> fresh =
        GitVault::Log(path, kHistoryVersionsShown);
    const bool unchanged =
        fresh.size() == request.knownLogSize &&
        (fresh.empty() || fresh[0].hash == request.knownLogHead);
    if (!unchanged) {
      answer.logChanged = true;
      answer.log = GitVault::LogWithChanges(path, kHistoryVersionsShown);
    }
    answer.uncommitted = GitVault::Dirty(path);
  }

  if (request.remotes) {
    answer.remoteList = GitVault::Remotes(path);
    answer.divergence = GitVault::AheadBehind(path);
  }

  // After the disk check, so a change it just saw is in the scan too.
  if (request.conflicts && GitVault::Available())
    answer.conflictReport = ProjectMerge::Scan(path);
  return answer;
}

DiskPoller::~DiskPoller() {
  if (m_thread.joinable())
    m_thread.join();
}

bool DiskPoller::Start(DiskPollRequest request, Answerer answer) {
  if (m_thread.joinable())
    return false;
  m_done.store(false);
  m_conflicts = request.conflicts;
  m_thread = std::thread(
      [this, request = std::move(request), answer = std::move(answer)] {
        m_answer = answer(request);
        m_done.store(true); // last: the answer is visible before the flag
      });
  return true;
}

std::optional<DiskPollAnswer> DiskPoller::Take() {
  if (!m_thread.joinable() || !m_done.load())
    return std::nullopt;
  m_thread.join();
  m_conflicts = false;
  return std::move(m_answer);
}

bool ApplyDiskPollAnswer(AppState& state, const DiskPollAnswer& answer) {
  const DiskPollRequest& asked = answer.asked;
  bool applied = true;
  if (asked.projectPath != state.projectPath)
    return false; // another project is open; nothing here is about it

  // Conflicts first: a scan that ran in this poll ran after its disk check,
  // so a change the disk part reports is already in it.
  bool conflictsApplied = false;
  if (asked.conflicts) {
    // Invalidated again (a save, a merge, a resolution) or scanned
    // synchronously since: a newer answer is owed or already held.
    if (state.conflictsStale || state.conflictsPath != asked.projectPath ||
        asked.conflictsGeneration != state.conflictsGeneration) {
      applied = false;
    } else {
      state.conflicts = answer.conflictReport;
      state.conflictScans++;
      conflictsApplied = true;
    }
  }

  if (asked.disk) {
    // A save, an open or a reload re-stamped the project after this poll
    // started: the answer compares the directory with a stamp that is no
    // longer ours, and "changed" would be our own write.
    if (asked.diskGeneration != state.diskGeneration) {
      applied = false;
    } else {
      state.diskPollsAnswered++;
      // Something else wrote the project, which is the only way a conflict
      // can appear without us doing it — so this is where the conflicts scan
      // earns its keep, rather than on a timer.
      if (answer.diskChanged && !state.diskChanged && !conflictsApplied)
        state.conflictsStale = true;
      if (answer.diskChanged)
        state.diskChanged = true;
    }
  }

  if (asked.history) {
    // The panel refreshed its cache itself after an action (or was told to)
    // while this ran: what it holds is newer than this answer.
    if (asked.historyStamp != state.historyStamp ||
        state.historyPath != asked.projectPath) {
      applied = false;
    } else {
      if (answer.logChanged)
        state.historyLog = answer.log;
      state.historyUncommitted = answer.uncommitted;
    }
  }

  if (asked.remotes) {
    if (asked.remotesStamp != state.remotesStamp) {
      applied = false;
    } else {
      state.remotes = answer.remoteList;
      state.divergence = answer.divergence;
    }
  }
  return applied;
}

bool ConflictScanPending(const AppState& state) {
  return state.conflictsStale || state.conflictsPath != state.projectPath ||
         (state.diskPoller && state.diskPoller->ScanningConflicts());
}

void PumpDiskPoll(AppState& state) {
  if (!state.diskPoller)
    state.diskPoller = std::make_shared<DiskPoller>();
  DiskPoller& poller = *state.diskPoller;
  if (std::optional<DiskPollAnswer> answer = poller.Take())
    ApplyDiskPollAnswer(state, *answer);

  // Focus coming back is the terminal-and-back workflow: ask at once. While a
  // poll is in flight the request waits for it rather than being lost.
  const bool focusReturned = state.windowFocused && !state.diskPollWasFocused;
  state.diskPollWasFocused = state.windowFocused;
  if (focusReturned)
    state.diskPollFocusPending = true;
  if (state.projectPath.empty() && !state.conflictsPath.empty()) {
    state.conflicts = ProjectMerge::Report{}; // nothing open, nothing to say
    state.conflictsPath.clear();
  }
  if (poller.Busy() || state.projectPath.empty())
    return;

  const double now = ImGui::GetTime();
  const int frame = ImGui::GetFrameCount();
  DiskPollRequest request;
  request.projectPath = state.projectPath;

  // Not once the banner is up: the answer is already known.
  if (!state.diskChanged &&
      (state.diskPollFocusPending || state.diskCheckStamp < 0.0 ||
       now - state.diskCheckStamp >= kDiskPollSeconds)) {
    state.diskCheckStamp = now;
    state.diskPollFocusPending = false;
    request.disk = true;
    request.diskGeneration = state.diskGeneration;
    request.stamp = state.diskStamp;
    request.stampUsable =
        state.diskStamp.valid && state.diskStampPath == state.projectPath;
    request.head = state.diskStampHead;
  }

  // The panels' caches, only while they are drawn (they drew last frame),
  // and only on the clock: a cache that was never filled, or that an action
  // invalidated, is refreshed by the panel itself, synchronously, because
  // the person just did something and is looking for the result.
  if (state.historyShownFrame >= frame - 1 && state.historyStamp >= 0.0 &&
      state.historyPath == state.projectPath &&
      now - state.historyStamp > kPanelRefreshSeconds) {
    state.historyStamp = now;
    request.history = true;
    request.historyStamp = now;
    request.knownLogSize = state.historyLog.size();
    if (!state.historyLog.empty())
      request.knownLogHead = state.historyLog[0].hash;
  }
  if (state.remotesShownFrame >= frame - 1 && state.remotesStamp >= 0.0 &&
      now - state.remotesStamp >= kPanelRefreshSeconds) {
    state.remotesStamp = now;
    request.remotes = true;
    request.remotesStamp = now;
  }

  // The conflicts scan, whenever something could have changed its answer —
  // an open, a save, a merge, a resolution, a change from outside. Another
  // project's answer is dropped at once rather than drawn over this one.
  if (state.conflictsStale || state.conflictsPath != state.projectPath) {
    if (state.conflictsPath != state.projectPath)
      state.conflicts = ProjectMerge::Report{};
    state.conflictsStale = false;
    state.conflictsPath = state.projectPath;
    request.conflicts = true;
    request.conflictsGeneration = state.conflictsGeneration;
  }

  if (request.disk || request.history || request.remotes || request.conflicts)
    poller.Start(std::move(request));
}

} // namespace ui
