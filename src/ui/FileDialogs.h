#pragma once

#include "ui/AppState.h"

namespace ui {

// Native (NFD) dialogs when available; STEP import falls back to an ImGui
// browser window otherwise (project open/save require NFD for now).
void OpenStepFileDialog(AppState& state);
void OpenProjectDialog(AppState& state);
void SaveProjectDialog(AppState& state);
// Export the whole document; format chosen by the chosen file's extension
// (.step/.stp, .stl, .brep). No-op without NFD.
void ExportDialog(AppState& state);
// Export the active assembly's bill of materials as CSV (A4). No-op without
// NFD or an active assembly.
void ExportBomDialog(AppState& state);

// Draws the fallback browser window if it is open (no-op with NFD).
void DrawFileBrowser(AppState& state);

} // namespace ui
