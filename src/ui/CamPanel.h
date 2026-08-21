#pragma once

#include "render/SketchRenderer.h" // PrimVertex
#include "ui/AppState.h"

#include <vector>

namespace ui {

// The CAM panel (docs/cam/plan.md): setups, tools, operations, and the two
// buttons that matter — generate and post. A dock tab beside Model, because a
// job belongs to the part the way a study does.
void DrawCamPanel(AppState& state);

// Backplot: the generated toolpath drawn in the viewport, rapids and cutting
// moves in different colours. Fed from the SAME toolpath the post consumes,
// never re-derived — a picture that can disagree with the file is worse than
// no picture.
void AppendCamToolpathOverlay(AppState& state, std::vector<PrimVertex>& out);

// Regenerates the cached toolpaths for the active setup. Cheap enough to call
// on any edit (a plate profile is ~200 moves); the cache exists so the
// overlay does not re-slice the model every frame.
void RefreshCamToolpaths(AppState& state);

} // namespace ui
