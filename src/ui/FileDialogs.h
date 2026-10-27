// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "ui/AppState.h"

namespace ui {

// Native (NFD) dialogs when available; import falls back to an ImGui browser
// window otherwise (project open/save require NFD for now).
//
// THE import entry (Ctrl+I): .step/.stp/.stl/.brep in one dialog. A STEP
// placing several instances imports as an assembly, everything else as a
// feature into the active part (ui::StartImport, which runs a STEP read
// in the background behind a progress modal).
void ImportFileDialog(AppState& state);
void OpenReferenceImageDialog(AppState& state); // sketch underlay (v49)
// Folder picker (projects are .lob folders): picking the project — or
// anything inside it — opens it.
void OpenProjectDialog(AppState& state);
// Save As: always asks for a (new) location.
void SaveProjectDialog(AppState& state);
// Plain save (Ctrl+S): re-saves in place when the project has a home; the
// first save falls through to the Save As dialog.
void SaveProjectAction(AppState& state);
// Export the whole document; format chosen by the chosen file's extension
// (.step/.stp, .stl, .brep, .glb/.gltf/.obj/.ply). No-op without NFD.
void ExportDialog(AppState& state);
// What Export does once it has a path, without the dialog — so it can be
// driven where a native dialog cannot open (tests, a container). STEP and the
// visual formats work on a part or an assembly tab; STL/BREP on a part only.
// Refuses an extension it does not write, naming the ones it does.
bool ExportActiveDocument(AppState& state, const std::string& path,
                          std::string* error = nullptr);
// Export the active assembly's bill of materials as CSV (A4). No-op without
// NFD or an active assembly.
void ExportBomDialog(AppState& state);

// Save a motion study's run as an animated GLB (ExportMotionGlb). The refusal
// lands in `state.motionAddRefusal`, shown in the panel. No-op without NFD.
void ExportMotionDialog(AppState& state, uint64_t studyId);

// Film a motion study (StartMotionVideo): a save dialog whose file type picks
// the format — .mp4, .webm, .gif, or .png for numbered frames in a folder
// named after the file. A refusal lands in `state.motionAddRefusal`.
// `state.saveDialogAnswer`, when set, answers instead of a dialog (tests).
void ExportMotionVideoDialog(AppState& state, uint64_t studyId);

// Whether a native file dialog can actually open here (NFD compiled in AND a
// usable backend — on Linux the XDG desktop portal). False in a bare session
// or a container, where Export used to click into silence: callers gate their
// buttons on this and say why instead.
bool FileDialogsAvailable();

// Draws the fallback browser window if it is open (no-op with NFD).
void DrawFileBrowser(AppState& state);

} // namespace ui
