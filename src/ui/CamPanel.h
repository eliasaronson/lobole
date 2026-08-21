#pragma once

#include "core/cam/CamPost.h"      // PostOptions
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

// The post options a setup implies. One place, so the panel and the tool
// session cannot disagree about whether this machine gets arcs (C7).
[[nodiscard]] inline cam::PostOptions PostOpts(const cam::Setup& setup) {
  cam::PostOptions opts;
  opts.fitArcs = setup.fitArcs;
  return opts;
}

// The setup the panel, the tool session and the overlays all act on (the one
// selected in the combo, else the first). Null when there is none.
cam::Setup* ActiveCamSetup(AppState& state);

} // namespace ui
