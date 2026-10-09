// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "ui/AppState.h"

#include <string>

// The machine store, as the app sees it
// (docs/cam/setup.md#machine-profiles). `state.machines` mirrors
// `~/.config/lobole/machines/*.json`: loaded at startup, reloaded after every
// save and delete, so the panel's "is this setup on one of my machines"
// question is answered from memory.
namespace ui {

// settings::ConfigDir()/machines.
[[nodiscard]] std::string MachineDir();

// Re-read the store into state.machines. Unreadable files are logged, not
// fatal: one broken profile must not hide the others.
void ReloadMachines(AppState& state);

// Write one machine to the store and reload. False + `error` on a refusal
// (no name, unknown post, a name that would share a file with another).
bool SaveMachine(AppState& state, const cam::Machine& machine,
                 std::string* error = nullptr);
bool DeleteMachine(AppState& state, const std::string& name,
                   std::string* error = nullptr);

// Put `machine` on `setup`: the embedded copy, and the per-job values it
// seeds (rapidFeed; the tool table, from the machine's library, when nothing
// has been cut yet). The one place a setup's machine is assigned, so a
// second seeded field cannot be forgotten at one of two doors.
void ApplyMachine(cam::Setup& setup, const cam::Machine& machine);

} // namespace ui
