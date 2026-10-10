// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "ui/AppState.h"

#include "core/io/Settings.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ui {

// Copy the preferences that have a live twin in AppState onto it (shadows,
// SSAO, shadow map, font scale). One direction only: `AppState::settings` is
// the single source of truth, and these fields are the renderer's view of it.
void ApplySettings(AppState& state);

// Note that a preference changed: applies it, and arms the debounced write so
// dragging a control is one save rather than sixty.
void MarkSettingsChanged(AppState& state);

// Called every frame; writes the file once the debounce has elapsed.
void FlushSettings(AppState& state, double now);

// Edit ▸ Preferences (Ctrl+,). A floating window, not a dock tab — it is not
// part of the workspace. A category list on the left, one page on the right
// (docs/ui/settings.md#the-window).
void DrawSettingsWindow(AppState& state);

// One page of Preferences. The settings table makes one per distinct
// `Field::section`, in the order the table first names it; the pages it does
// not make (Keyboard, Machines) are registered in SettingsWindow.cpp's
// kHandPages and follow them. The list is the window's only statement of
// order.
struct PreferenceCategory {
  std::string name; // the page title, and its row's "###cat_<name>"
  const char* icon = nullptr;
  // The table rows the page draws; empty for a hand-written page.
  std::vector<const settings::Field*> rows;
  // A library: the user's own things in a list, the selected one's form
  // beside it. Search matches its items by name, and it has no reset.
  bool library = false;
};

// Every page, over `fields` (the real table unless a test passes its own).
std::vector<PreferenceCategory> PreferenceCategories(
    std::span<const settings::Field> fields = settings::Fields());

// Opens Preferences on the named page ("Machines"), to the front, with the
// search cleared. An unknown name opens the window on the page it last showed.
void OpenPreferences(AppState& state, std::string_view category);

} // namespace ui
