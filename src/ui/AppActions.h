#pragma once

#include "ui/AppState.h"

#include <memory>
#include <string>

namespace ui {

// Swaps in a new document (File > New / Open) with a fresh undo stack and
// forces the renderers to resync.
void ReplaceDocument(AppState& state, std::unique_ptr<Document> doc);

// Frames the camera on the document's combined bounding box.
void FitCameraToDocument(AppState& state);

// Mirrors document bodies into the renderers when the document version
// changed (edit, undo, redo). Call once per frame before drawing.
void SyncDocumentRenderers(AppState& state);

// Imports a STEP file as an undoable ImportFeature and fits the camera.
void LoadSTEPFile(AppState& state, const std::string& filename);

} // namespace ui
