#pragma once

#include "ui/AppState.h"

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
// part of the workspace.
void DrawSettingsWindow(AppState& state);

} // namespace ui
