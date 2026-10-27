// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "ui/AppState.h"

namespace ui {

// Version history (F6): the user-facing half of git-backed projects. Every
// clean save is a version; naming one tags it. Drawn on every tab kind — a
// version belongs to the project, not to the part you happen to have open.
void DrawHistoryPanel(AppState& state);

// Save the project and name the current state a version ("" is refused, as
// is a name with no ref-safe characters). Shared by the panel and the
// checkpoint_* RPCs so both mean the same thing.
bool NameCurrentVersion(AppState& state, const std::string& name,
                        std::string* error = nullptr);

// Put the project back to `commit` (a hash, or GitVault::TagRef(name)) and
// reopen it. The restore is committed on top, so history stays linear.
bool RestoreVersion(AppState& state, const std::string& commit,
                    const std::string& label, std::string* error = nullptr);

// True when versions are possible here: a saved `.lob` project directory and
// a usable `git`. The repository itself is created by the first save, so this
// is true one moment before a history exists.
// Check out another line of work and reopen the project — the documents in
// memory belong to the branch being left, and the next save would write them
// straight back over the one being entered.
bool SwitchToBranch(AppState& state, const std::string& branch,
                    std::string* error = nullptr);

// Bring `branch` into the current one. Returns true for a CONFLICTED merge
// as well as a clean one: conflicts are the expected outcome, not a failure,
// and the Conflicts panel takes it from there.
bool MergeBranch(AppState& state, const std::string& branch,
                 std::string* error = nullptr);

bool CanVersionProject(const AppState& state);

// True when there is a history to show — CanVersionProject plus a repository
// on disk. An older project (or one saved while git was missing) is the gap
// between the two.
bool ProjectHasHistory(const AppState& state);

} // namespace ui
