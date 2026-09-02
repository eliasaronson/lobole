#pragma once

#include "render/MeshRenderer.h"   // LineVertex3D
#include "render/SketchRenderer.h" // PrimVertex
#include "ui/AppState.h"
#include "ui/FeatureEditor.h" // PickSlot

#include <vector>

// The CAM verb session and what it draws (docs/cam/gui.md, G2-G4).
//
// The panel (CamPanel.cpp) is the LIST — setups, tools, operations, the
// numbers. This is the way IN: three verbs, each one click on the model, with
// every depth derived from what was clicked. A first-time user should be able
// to cut a part without typing a number, and should be able to SEE what the
// job will remove before the spindle turns.
//
// Deliberately a tool session and not a mode: CAM does not take the viewport
// over (the backplot coexists with the model, and a picked face means "that
// face"), and modes have a known exit-bug class in this codebase — U11 spent a
// round on simulation mode not exiting when it should.
namespace ui {

// The floating verb window. Drawn by DrawToolWindow when camToolOpen; what
// it shows is state.camVerb's.
void DrawCamToolWindow(AppState& state);

// A toolbar verb (Commands.cpp `job.*`): opens the window for `verb` and arms
// its pick, or -- for the two verbs that need no pick -- makes the operation
// right away. Pressing the verb that is already armed puts it away, which is
// the way out that is not the Escape key.
void StartCamVerb(AppState& state, AppState::CamVerb verb);

// The tool the next verb derives for: the one chosen this session, else the
// LAST TOOL USED (the setup's newest operation's), else the setup's first.
// A job is cut with one tool far more often than not, and the tool you just
// used is the one you have in the spindle.
[[nodiscard]] int VerbTool(const AppState& state, const cam::Setup& setup);

// The plan view of the active setup, framed on its stock
// (`job.look_down_spindle`).
void LookDownTheSpindle(AppState& state);

// Arms one of the CAM pick slots (kCamToolSlot + the slot id).
void ArmCamPick(AppState& state, PickSlot slot, const char* prompt);

// Every CAM pick lands here; the armed slot says what the face MEANS.
void CamFacePicked(AppState& state, const SelectionItem& item);

// G3, the visualisation ladder. `prims` takes lines and dots (the ghost
// outline, the hover candidate, the leftover marks); `quads` takes the
// translucent surfaces (the stock box, the walls of the removed volume).
void AppendCamVisualisation(AppState& state, std::vector<PrimVertex>& prims,
                            std::vector<LineVertex3D>& quads);

// Depth as a label on the part, so a derived number is visible without
// reading a field. 2D, so it runs with the other ImGui overlays.
void DrawCamLabels(AppState& state);

// G4: post the cached toolpaths, simulate the file, and store the verdict in
// the words the panel prints. Called after every Generate — verification
// stops being a thing you remember to run.
void RunCamVerification(AppState& state);

} // namespace ui
