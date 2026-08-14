#pragma once

#include "ui/AppState.h"

namespace ui {

// Native (NFD) dialogs when available; import falls back to an ImGui browser
// window otherwise (project open/save require NFD for now).
//
// THE import entry (Ctrl+I): .step/.stp/.stl/.brep in one dialog. A STEP
// placing several instances imports as an assembly, everything else as a
// feature into the active part (ImportCADFile).
void ImportFileDialog(AppState& state);
void OpenReferenceImageDialog(AppState& state); // sketch underlay (v49)
// Folder picker (projects are .dsgn folders): picking the project — or
// anything inside it — opens it.
void OpenProjectDialog(AppState& state);
// Save As: always asks for a (new) location.
void SaveProjectDialog(AppState& state);
// Plain save (Ctrl+S): re-saves in place when the project has a home; the
// first save falls through to the Save As dialog.
void SaveProjectAction(AppState& state);
// Export the whole document; format chosen by the chosen file's extension
// (.step/.stp, .stl, .brep). No-op without NFD.
void ExportDialog(AppState& state);
// Export the active assembly's bill of materials as CSV (A4). No-op without
// NFD or an active assembly.
void ExportBomDialog(AppState& state);

// Whether a native file dialog can actually open here (NFD compiled in AND a
// usable backend — on Linux the XDG desktop portal). False in a bare session
// or a container, where Export used to click into silence: callers gate their
// buttons on this and say why instead.
bool FileDialogsAvailable();

// Draws the fallback browser window if it is open (no-op with NFD).
void DrawFileBrowser(AppState& state);

} // namespace ui
