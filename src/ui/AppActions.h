#pragma once

#include "ui/AppState.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct Vertex3D;

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

// Translucent default-plane quads (Front/Top/Right); hoveredPlane 0/1/2 is
// brightened, -1 for none. Empty when show is false.
std::vector<Vertex3D> BuildPlaneQuads(bool show, int hoveredPlane);

// Highlighted line geometry for the sketch currently being edited (empty if
// none). Rendered as a per-frame overlay on top of the static sketch lines.
std::vector<Vertex3D> BuildActiveSketchOverlay(AppState& state,
                                               uint64_t activeSketchId);

} // namespace ui
