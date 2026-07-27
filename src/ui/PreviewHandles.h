#pragma once

#include "render/SketchRenderer.h" // LineVertex3D
#include "ui/AppState.h"

#include <vector>

struct Ray; // FaceSelector.h

namespace ui {

// Direct manipulation on the pending preview feature (docs/ui/
// direct-manipulation.md): one accent-colored viewport handle that drags the
// feature's headline scalar — extrude distance (Blind/Symmetric), fillet/
// chamfer radius, revolve angle. The handle mutates the same field the
// preview dialog's widget edits (mutate + Regenerate, no undo entry), so the
// dialog stays live and accept still commits one AddFeatureCommand. No handle
// when the parameter is expression-driven.

// Appends the handle geometry (and, mid-drag, the floating value readout) for
// the pending feature. Same overlay channel as the assembly gizmo.
void BuildPreviewHandleOverlay(AppState& state,
                               std::vector<LineVertex3D>& lines);

// The gizmo input contract: a click on the handle starts a drag (consumes the
// click), an active drag follows the mouse even off-viewport and ends on
// release. Returns true when the event was consumed.
bool HandlePreviewHandleInput(AppState& state, const Ray& ray, bool clicked,
                              bool down, bool released);

// Escape mid-drag: restore the grab-time value, regenerate, end the drag
// (the preview itself survives — Esc is consumed).
void AbortPreviewHandleDrag(AppState& state);

} // namespace ui
