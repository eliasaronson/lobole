#pragma once

#include "render/SketchRenderer.h"
#include "ui/AppState.h"

// The simulation panel, its result field legend, and the
// viewport interaction for placing and dragging loads.
//
// Split out of ui/AppActions.h, which had grown to ~180 declarations spanning
// every domain in the app and is included by 40 translation units — so every
// panel and RPC module paid for all of it. See
// docs/architecture-review-2026-08-24.md.
namespace ui {

// The Studies panel drawn in the Model dock while in simulation mode. Defined
// in StudyPanel.cpp.
void DrawStudyPanel(AppState& state);

// The result-field colorbar legend overlaid on the viewport while a field is
// displayed. Defined in StudyPanel.cpp.
void DrawFieldLegend(AppState& state);

// Assigns the armed boundary condition (state.simulation.tool) to the face
// under `ray`, as an undoable study edit. No-op if no tool is armed or the
// pick misses a named face. Defined in StudyPanel.cpp.
// Applies the armed BC/keep tool to a picked face (U1c: the selection
// pipeline resolves the pick; this consumes it).
void SimulationApplyToFace(AppState& state, const ElementRef& face);

// Boundary-condition glyphs while simulation mode is active: arrows on
// force/pressure faces (force along its direction, pressure into the face),
// anchor hatches on fixed faces, diamonds on keep faces.
void AppendSimulationBCGlyphs(AppState& state, std::vector<PrimVertex>& out);

// Dragging a force arrow to change its magnitude. Its own grab payload
// (SimulationState::loadDrag), like the gizmo's and the section arrow's —
// the shared layer answers WHICH handle, not what happens to it
// (docs/ui/viewport-handles.md). Returns true when it consumed the event.
bool HandleLoadDragInput(AppState& state, const Ray& ray, bool clicked,
                         bool down, bool released);

} // namespace ui
