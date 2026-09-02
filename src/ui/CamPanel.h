#pragma once

#include "core/cam/CamPost.h"      // PostOptions
#include "render/SketchRenderer.h" // PrimVertex
#include "ui/AppState.h"

#include <string>
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

// Regenerates the cached toolpaths for the active setup — ALL of them. Use it
// when something setup-wide changed (the stock, a tool, the frame); for an
// edit to one operation use RefreshCamToolpath below, because "all of them"
// is not cheap: one adaptive pocket on a real part measured 7.1 s, and every
// edit used to pay it again (docs/known-bugs.md).
void RefreshCamToolpaths(AppState& state);

// Regenerates ONE operation's toolpath in place (appending if it is new), and
// drops one by id. The vector is per-operation and in operations order, so
// the result is the same as a full pass — `test_cam.cpp` pins that.
void RefreshCamToolpath(AppState& state, uint64_t opId);
void DropCamToolpath(AppState& state, uint64_t opId);

// The post options a setup implies. One place, so the panel and the tool
// session cannot disagree about whether this machine gets arcs (C7).
[[nodiscard]] inline cam::PostOptions PostOpts(const cam::Setup& setup) {
  cam::PostOptions opts;
  opts.fitArcs = setup.machine.fitArcs;
  return opts;
}

// The setup the panel, the tool session and the overlays all act on (the one
// selected in the combo, else the first). Null when there is none.
cam::Setup* ActiveCamSetup(AppState& state);

// Removes operations by id from whichever setup holds them. The toolpath
// cache, the ghost, the verb session and the selection let go of them, and
// the verdict is re-run against what is left. ONE verb: the docked panel and
// the tool window each had their own, with different side effects, and the
// Delete key (ListSelection.h) needed a third. Returns how many went.
int RemoveCamOperations(AppState& state, const std::vector<uint64_t>& ids);

// The row every operation list draws for one operation: `sentence` is what
// it is, drawn as a Selectable so a click selects it, Ctrl/Shift extend
// (`order` is the ids as listed, which a Shift range spans) and right-click
// offers Delete for the selection. Inside the caller's PushID(op.id). Returns
// true when the menu asked for Delete — the caller defers it, because it is
// walking the list the delete would shrink.
bool CamOperationRow(AppState& state, uint64_t opId,
                     const std::string& sentence,
                     const std::vector<uint64_t>& order);

} // namespace ui
