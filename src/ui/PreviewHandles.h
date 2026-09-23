#pragma once

#include "render/SketchRenderer.h" // PrimVertex
#include "ui/AppState.h"

#include <vector>

struct Ray; // FaceSelector.h

namespace ui {

// Direct manipulation on the pending preview feature
// (docs/ui/viewport-handles.md): one accent-colored viewport handle that drags
// the feature's headline scalar — extrude distance (Blind/Symmetric), fillet/
// chamfer radius, revolve angle. The handle mutates the same field the preview
// dialog's widget edits (mutate + Regenerate, no undo entry), so the dialog
// stays live and accept still commits one AddFeatureCommand. No handle when the
// parameter is expression-driven.

// Publishes the pending feature's (or the active section's) handle for the
// shared hit test and drawer (ui/ViewportHandleDraw.h), and appends its
// decorations — the section's plane outline, the arrow's base tick, and
// mid-drag the floating value readout — to `out`, the same on-top channel
// the handles themselves are drawn into.
void BuildPreviewHandleOverlay(AppState& state, std::vector<PrimVertex>& out);

// Is there a viewport handle to hit at all — a pending feature's, or the
// active section's? ViewportInput asks before routing a click here, and the
// answer must not be "is there a pending feature": a section handle belongs
// to no feature, so that gate sent every section drag to the picker instead
// (which selected whatever was under the arrow).
bool HasViewportHandle(AppState& state);

// The gizmo input contract: a click on the handle starts a drag (consumes the
// click), an active drag follows the mouse even off-viewport and ends on
// release. Returns true when the event was consumed.
bool HandlePreviewHandleInput(AppState& state, const Ray& ray, bool clicked,
                              bool down, bool released);

// Escape mid-drag: restore the grab-time value, regenerate, end the drag
// (the preview itself survives — Esc is consumed).
void AbortPreviewHandleDrag(AppState& state);

} // namespace ui
