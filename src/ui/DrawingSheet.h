#pragma once

#include "core/drawing/DrawingRender.h"
#include "ui/AppState.h"

#include "imgui.h"

#include <glm/glm.hpp>

// The sheet viewport's space, shared by what draws on it and what is picked
// on it: the dimension tool (DrawingPanel.cpp) and the annotation and
// detail sessions (DrawingAnnotate.cpp). Defined in DrawingPanel.cpp.
namespace ui {

// Sheet mm (origin bottom-left, +Y up) to screen pixels (+Y down), and back.
ImVec2 SheetToScreen(const AppState& state, glm::dvec2 mm);
glm::dvec2 ScreenToSheet(const AppState& state, ImVec2 px);

double PointSegDist(glm::dvec2 p, glm::dvec2 a, glm::dvec2 b);

// The projected curve nearest `cursor` (sheet mm) across all views, within
// `maxDist`, that carries a dimensionable ref; `viewIdOut` gets its view.
const drawing::SheetCurve* PickCurve(AppState& state, glm::dvec2 cursor,
                                     double maxDist, uint64_t& viewIdOut);
ProjRef RefOfCurve(const drawing::SheetCurve& c, uint64_t viewId);

void SheetArrow(ImDrawList* dl, AppState& state, glm::dvec2 tip, glm::dvec2 dir,
                ImU32 col);

// Sheet furniture (a balloon, a frame, a hatch) on screen, as the exporters
// draw it.
void DrawSheetGraphics(ImDrawList* dl, AppState& state,
                       const drawing::SheetGraphics& g, ImU32 col);

} // namespace ui
