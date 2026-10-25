// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "ui/AppState.h"

namespace ui {

// Merge conflicts, in the words the tabs use (F7).
//
// The panel appears only when there is something to resolve — a permanent
// tab that says "no conflicts" every day of the year is noise, and this is
// the one panel whose absence is the good news. It shows up for a merge
// started in the app, and for a project opened while a merge was left
// half-finished somewhere else (a terminal, a sync folder).
void DrawConflictsPanel(AppState& state);

// True when the project has anything to resolve — unmerged paths, or two
// files claiming one id. Cheap: reads the cached scan, which may trail the
// tree by a few frames (ConflictScanPending, ui/DiskPoller.h) — so it is for
// showing, never for deciding; a decision scans (ProjectMerge::Scan).
bool ProjectHasConflicts(AppState& state);

// Rescan, off the frame: the disk poller starts one on the next frame and the
// answer lands a few frames later. Call after anything that resolves or
// creates a conflict.
void InvalidateConflicts(AppState& state);

// Conclude a merge: verify the resolved project LOADS AND REGENERATES, then
// commit it and reopen. Refuses (and fills state.conflictsBreakage) when the
// project does not work, because "git exited 0" was never the bar.
bool FinishMerge(AppState& state, std::string* error = nullptr);

// Throw the merge away and reopen the project as it was.
bool AbandonMerge(AppState& state, std::string* error = nullptr);

} // namespace ui
