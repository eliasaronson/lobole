// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "ui/AppState.h"

#include "imgui.h"

#include <cstdint>
#include <vector>

// The drawing tab's annotation sessions (docs/drawings/guide.md#in-the-gui):
// a feature control frame, a datum feature, a detail view, and laying out
// balloons. Each session is the one floating tool window plus direct
// manipulation on the sheet; what the RPC refuses, the window says.
namespace ui {

// Arms the frame or the datum session (ends any other drawing tool).
void ArmDrawingNoteTool(AppState& state, AppState::DrawingNoteTool::Kind kind);
// A frame stacked with dimension `dimId`, chosen from its row: the session
// opens already aimed at it (select-first).
void BeginFcfOnDimension(AppState& state, uint64_t dimId);
// Arms the detail session; `parentViewId` non-zero when it was chosen from
// that view's row.
void ArmDetailTool(AppState& state, uint64_t parentViewId = 0);
// Every drawing session off: the dimension pick, the annotations, the detail.
void EndDrawingTools(AppState& state);

// The views of the sheet's first assembly, which "Lay out balloons" goes
// round from the toolbar; empty when the sheet draws no assembly.
std::vector<uint64_t> BalloonViews(AppState& state);
// Lays a balloon on every row of the parts list round `viewIds`, and says
// what came of it in state.drawingMessage. False when it was refused.
bool LayOutBalloons(AppState& state, const std::vector<uint64_t>& viewIds);

// The sessions on the sheet: hover, the pick, the drag, the preview. Called
// from DrawSheet after the dimensions are drawn.
void HandleDrawingAnnotateTools(ImDrawList* dl, AppState& state);

// Their tool windows (PreviewFeature.cpp's DrawToolWindow picks one).
void DrawDrawingNoteToolWindow(AppState& state);
void DrawDetailToolWindow(AppState& state);

} // namespace ui
