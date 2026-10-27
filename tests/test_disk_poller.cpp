// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// The disk poll's answer arrives frames after it was asked
// (docs/versioning/README.md#the-poll-runs-off-the-frame). These hold an
// answer on the worker, change what is open while it waits, and check that
// an answer to the old question is discarded rather than applied to the new
// state — a "changed on disk" for a project that was just saved would be our
// own write reported as somebody else's.

#include <catch2/catch_test_macros.hpp>

#include "ui/AppState.h"
#include "ui/DiskPoller.h"

#include <chrono>
#include <future>
#include <optional>
#include <thread>

namespace {

using ui::DiskPollAnswer;
using ui::DiskPollRequest;

// Starts `request` on `poller` with an answerer that says "changed" (and a
// fresh log and remotes) once `gate` opens.
void StartHeld(ui::DiskPoller& poller, const DiskPollRequest& request,
               std::shared_future<void> gate) {
  REQUIRE(poller.Start(request, [gate](const DiskPollRequest& asked) {
    gate.wait();
    DiskPollAnswer answer;
    answer.asked = asked;
    answer.diskChanged = true;
    answer.logChanged = true;
    answer.log.resize(3);
    answer.uncommitted = true;
    answer.remoteList.push_back({"origin", "https://example.invalid/x.git"});
    return answer;
  }));
}

DiskPollAnswer Await(ui::DiskPoller& poller) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (std::chrono::steady_clock::now() < deadline) {
    if (std::optional<DiskPollAnswer> answer = poller.Take())
      return *answer;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  FAIL("the poll never answered");
  return {};
}

void OpenAt(ui::AppState& state, const std::string& path) {
  state.projectPath = path;
  state.diskGeneration++;
  state.diskChanged = false;
  state.conflictsStale = false;
}

DiskPollRequest DiskQuestion(const ui::AppState& state) {
  DiskPollRequest request;
  request.projectPath = state.projectPath;
  request.disk = true;
  request.diskGeneration = state.diskGeneration;
  return request;
}

} // namespace

TEST_CASE("A disk poll answer is applied while it still describes the "
          "project",
          "[disk]") {
  ui::AppState state;
  OpenAt(state, "/projects/a.lob");
  ui::DiskPoller poller;
  std::promise<void> release;
  StartHeld(poller, DiskQuestion(state), release.get_future().share());
  CHECK(poller.Busy());
  CHECK_FALSE(poller.Take().has_value()); // held: nothing yet, and no wait
  release.set_value();
  const DiskPollAnswer answer = Await(poller);
  CHECK_FALSE(poller.Busy());
  CHECK(ui::ApplyDiskPollAnswer(state, answer));
  CHECK(state.diskChanged);
  CHECK(state.conflictsStale); // a write from outside is when to rescan
  CHECK(state.diskPollsAnswered == 1);
}

TEST_CASE("A disk poll answer that arrives after another project opened is "
          "discarded",
          "[disk]") {
  ui::AppState state;
  OpenAt(state, "/projects/a.lob");
  ui::DiskPoller poller;
  std::promise<void> release;
  StartHeld(poller, DiskQuestion(state), release.get_future().share());
  OpenAt(state, "/projects/b.lob"); // while the answer is held
  release.set_value();
  CHECK_FALSE(ui::ApplyDiskPollAnswer(state, Await(poller)));
  CHECK_FALSE(state.diskChanged);
  CHECK_FALSE(state.conflictsStale);
  CHECK(state.diskPollsAnswered == 0);
}

TEST_CASE("A disk poll answer that arrives after the project was saved is "
          "discarded",
          "[disk]") {
  // Same path, new baseline: a save (or a reload) re-stamped the directory
  // after the poll started, so the poll compared against a stamp that is no
  // longer ours — and the change it saw is the save.
  ui::AppState state;
  OpenAt(state, "/projects/a.lob");
  ui::DiskPoller poller;
  std::promise<void> release;
  StartHeld(poller, DiskQuestion(state), release.get_future().share());
  state.diskGeneration++; // StampDiskState, as every save does
  release.set_value();
  CHECK_FALSE(ui::ApplyDiskPollAnswer(state, Await(poller)));
  CHECK_FALSE(state.diskChanged);
}

TEST_CASE("A panel refresh after an action discards the poll's older log "
          "but not its disk answer",
          "[disk]") {
  ui::AppState state;
  OpenAt(state, "/projects/a.lob");
  state.historyPath = state.projectPath;
  state.historyStamp = 10.0;
  state.historyLog.resize(1);
  state.remotesStamp = 10.0;

  DiskPollRequest request = DiskQuestion(state);
  request.history = true;
  request.historyStamp = state.historyStamp;
  request.remotes = true;
  request.remotesStamp = state.remotesStamp;

  ui::DiskPoller poller;
  std::promise<void> release;
  StartHeld(poller, request, release.get_future().share());
  // Naming a version refreshes the panel's log synchronously; the held
  // answer was read before that commit and would put the old list back.
  state.historyStamp = 11.0;
  state.historyLog.resize(2);
  release.set_value();
  CHECK_FALSE(ui::ApplyDiskPollAnswer(state, Await(poller)));
  CHECK(state.historyLog.size() == 2);
  CHECK_FALSE(state.historyUncommitted);
  // The parts nobody touched still land.
  CHECK(state.diskChanged);
  CHECK(state.remotes.size() == 1);
}

TEST_CASE("Only one disk poll runs at a time", "[disk]") {
  ui::AppState state;
  OpenAt(state, "/projects/a.lob");
  ui::DiskPoller poller;
  std::promise<void> release;
  StartHeld(poller, DiskQuestion(state), release.get_future().share());
  CHECK_FALSE(poller.Start(DiskQuestion(state)));
  release.set_value();
  Await(poller);
  CHECK_FALSE(poller.Busy());
}

namespace {

DiskPollRequest ConflictQuestion(ui::AppState& state) {
  // What PumpDiskPoll does when a scan is owed.
  state.conflictsStale = false;
  state.conflictsPath = state.projectPath;
  DiskPollRequest request;
  request.projectPath = state.projectPath;
  request.conflicts = true;
  request.conflictsGeneration = state.conflictsGeneration;
  return request;
}

void StartHeldScan(ui::DiskPoller& poller, const DiskPollRequest& request,
                   std::shared_future<void> gate) {
  REQUIRE(poller.Start(request, [gate](const DiskPollRequest& asked) {
    gate.wait();
    DiskPollAnswer answer;
    answer.asked = asked;
    answer.conflictReport.conflicts.resize(1); // "one to resolve"
    return answer;
  }));
}

} // namespace

TEST_CASE("A conflicts scan off the frame lands while nothing moved",
          "[disk]") {
  ui::AppState state;
  OpenAt(state, "/projects/a.lob");
  ui::DiskPoller poller;
  std::promise<void> release;
  StartHeldScan(poller, ConflictQuestion(state), release.get_future().share());
  CHECK(poller.ScanningConflicts());
  release.set_value();
  CHECK(ui::ApplyDiskPollAnswer(state, Await(poller)));
  CHECK(state.conflicts.conflicts.size() == 1);
  CHECK(state.conflictScans == 1);
  CHECK_FALSE(poller.ScanningConflicts());
}

TEST_CASE("A conflicts scan is discarded when a resolution or a "
          "synchronous scan came after it",
          "[disk]") {
  ui::AppState state;
  OpenAt(state, "/projects/a.lob");

  SECTION("invalidated while it ran (a resolution, a save)") {
    ui::DiskPoller poller;
    std::promise<void> release;
    StartHeldScan(poller, ConflictQuestion(state),
                  release.get_future().share());
    state.conflictsStale = true; // InvalidateConflicts
    release.set_value();
    CHECK_FALSE(ui::ApplyDiskPollAnswer(state, Await(poller)));
    CHECK(state.conflicts.Empty());
    CHECK(state.conflictsStale); // still owed: the next frame asks again
  }
  SECTION("FinishMerge scanned for itself while it ran") {
    ui::DiskPoller poller;
    std::promise<void> release;
    StartHeldScan(poller, ConflictQuestion(state),
                  release.get_future().share());
    state.conflictsGeneration++; // ScanNow, which found nothing left
    release.set_value();
    CHECK_FALSE(ui::ApplyDiskPollAnswer(state, Await(poller)));
    CHECK(state.conflicts.Empty()); // the newer answer stands
  }
  SECTION("another project opened while it ran") {
    ui::DiskPoller poller;
    std::promise<void> release;
    StartHeldScan(poller, ConflictQuestion(state),
                  release.get_future().share());
    OpenAt(state, "/projects/b.lob");
    release.set_value();
    CHECK_FALSE(ui::ApplyDiskPollAnswer(state, Await(poller)));
    CHECK(state.conflicts.Empty());
  }
}
