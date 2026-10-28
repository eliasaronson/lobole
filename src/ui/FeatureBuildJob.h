// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// Heavy features build off the frame
// (docs/ui/long-operations.md#heavy-features): a part whose regeneration
// stopped at a heavy feature (Document::PendingBuild) gets a `feature` job,
// which runs the feature's Execute on deep copies of its inputs and hands the
// result back to the part's regen cache. One job per part. The poll that starts
// them is part of PollBackgroundJobs, so a build lands whatever panel or tab is
// showing.

#include <cstdint>
#include <string>

class Document;
class CommandStack;

namespace ui {

struct AppState;

inline constexpr const char* kFeatureBuildKind = "feature";

// Every part the app holds builds heavy features off the frame
// (Document::SetBuildsOffFrame). Cheap; the poll calls it, and so does the
// RPC pump before each request, so a part made and edited in one tick
// builds off the frame too.
void OptPartsIntoOffFrameBuilds(AppState& state);

// Starts, supersedes and cancels the `feature` jobs (called once per loop
// iteration from PollBackgroundJobs, after the reap). A cancel, however it
// was asked for (the tool window, cancel_background_job, a test), is applied
// here: the edit that made the build is rolled back when it is still the
// last on its part's undo stack, else the feature is left not built
// (cancelled).
void PollFeatureBuilds(AppState& state);

// The part slot id a `feature` job builds for `doc` (0 for the lone part of
// a document with no tabs), and the part for a slot id; null once closed.
uint64_t FeatureBuildSlotOf(AppState& state, const Document* doc);
Document* FeatureBuildDoc(AppState& state, uint64_t slotId);
CommandStack* FeatureBuildCommands(AppState& state, uint64_t slotId);

// Whether a `feature` job is running for the part (any slot when 0 is not
// a slot: pass the id FeatureBuildSlotOf gave).
bool FeatureBuildRunning(AppState& state, uint64_t slotId);

// Cancels the part's running build (the tool window's and the tree's
// Cancel); the outcome is applied by the next poll. False when none runs.
bool CancelFeatureBuild(AppState& state, uint64_t slotId);

// Waits, pumping the registry, until no part has a feature building (the
// synchronous form, for a `batch` and the tests). False on the deadline.
bool SettleFeatureBuilds(AppState& state, double seconds);

// One line for a status bar or a tooltip: "Building 'Delete face' (3 s, 40%)",
// or empty when the active part builds nothing.
std::string FeatureBuildStatus(AppState& state);

} // namespace ui
