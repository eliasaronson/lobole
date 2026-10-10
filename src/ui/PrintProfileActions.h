// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "ui/AppState.h"

#include <string>

// The print profile store, as the app sees it
// (docs/tolerances/README.md#print-profiles). `state.printProfiles` mirrors
// `~/.config/lobole/print_profiles/*.json`: loaded at startup, reloaded after
// every save and delete, so the Print process section's "is this part on one
// of my profiles" question is answered from memory. The machine store
// (ui/MachineActions.h) is the pattern.
namespace ui {

// settings::ConfigDir()/print_profiles.
[[nodiscard]] std::string PrintProfileDir();

// Re-read the store into state.printProfiles. Unreadable files are logged,
// not fatal: one broken profile must not hide the others.
void ReloadPrintProfiles(AppState& state);

// Write one profile to the store and reload. False + `error` on a refusal
// (printfit::ValidateProfile, or a name that would share a file).
bool SavePrintProfile(AppState& state, const printfit::Profile& profile,
                      std::string* error = nullptr);
bool DeletePrintProfile(AppState& state, const std::string& name,
                        std::string* error = nullptr);

// Rename a stored profile: refused when another has the name (or would share
// its file); the old file goes only once the new one is written.
bool RenamePrintProfile(AppState& state, const std::string& from,
                        const std::string& to, std::string* error = nullptr);

// A name no stored profile has: `base`, else `base 2`, `base 3`...
[[nodiscard]] std::string FreePrintProfileName(const AppState& state,
                                               const std::string& base);

} // namespace ui
