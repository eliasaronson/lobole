#pragma once

#include "core/cam/CamPost.h"      // PostOptions
#include "render/SketchRenderer.h" // PrimVertex
#include "ui/AppState.h"

#include <memory>
#include <string>
#include <vector>

namespace ui {

// The CAM panel (docs/cam/README.md): setups, tools, operations, and the two
// buttons that matter — generate and post. A dock tab beside Model, because a
// job belongs to the part the way a study does.
void DrawCamPanel(AppState& state);

// Backplot: the generated toolpath drawn in the viewport, rapids and cutting
// moves in different colours. Fed from the SAME toolpath the post consumes,
// never re-derived — a picture that can disagree with the file is worse than
// no picture.
void AppendCamToolpathOverlay(AppState& state, std::vector<PrimVertex>& out);

// The same backplot split for the renderer's cached channel
// (docs/cam/gui.md#what-the-viewport-draws). `CamBackplot` is the cached vertex
// list, SHARED rather than copied, with the key that changes when it does --
// null when nothing is drawn this way (the path hidden, no toolpath, or a
// simulation playing its own). `AppendCamToolpathExtras` is everything else
// the overlay draws: the tool glyphs, or the played path.
// `AppendCamToolpathOverlay` is the two joined, for a test that reads the
// whole picture.
struct CamBackplotRef {
  std::shared_ptr<const std::vector<PrimVertex>> verts;
  uint64_t key = 0;
};
[[nodiscard]] CamBackplotRef CamBackplot(AppState& state);
void AppendCamToolpathExtras(AppState& state, std::vector<PrimVertex>& out);

// A fingerprint of the toolpaths, O(toolpaths) not O(moves): what the
// backplot is keyed on, and what anything else derived from every move can be
// cached on too.
[[nodiscard]] uint64_t CamToolpathsKey(const std::vector<cam::Toolpath>& paths);

// Regenerates the cached toolpaths for the active setup — ALL of them. Use it
// when something setup-wide changed (the stock, a tool, the frame); for an
// edit to one operation use RefreshCamToolpath below, because "all of them"
// is not cheap: one adaptive pocket on a real part measured 7.1 s, and every
// edit used to pay it again (docs/cam/README.md#background-work).
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

// ActiveCamSetup moved to ui/JobActions.h -- it reads the job document, not
// the CAM panel, and a job tab exists in a build with no toolpath generator.

// Removes operations by id from whichever setup holds them. The toolpath
// cache, the ghost, the verb session and the selection let go of them, and
// the verdict is re-run against what is left. ONE verb: the docked panel and
// the tool window each had their own, with different side effects, and the
// Delete key (ListSelection.h) needed a third. Returns how many went.
int RemoveCamOperations(AppState& state, const std::vector<uint64_t>& ids);

// The row every operation list draws for one operation, in the part feature
// list's style (docs/ui/panels.md#cam-job-tab-the-cam-panel): the type's glyph,
// the name as a full-width Selectable, the tool as a dim tail, dimmed when
// disabled, a red mark when it cut nothing. Click selects (Ctrl/Shift extend
// over `order`, the ids as listed), double-click edits, drag reorders,
// right-click offers Edit, Enable/Disable, show what it removes, and Delete for
// the selection. The tooltip is the op's summary and what its toolpath came to.
// Inside the caller's PushID(op.id). The caller applies what it asks for,
// because it is walking the list those would change.
struct CamOpRowResult {
  bool clicked = false;         // any click on the row, or its menu opening
  bool deleteSelection = false; // the menu's Delete
  bool edit = false;            // double-click, or the menu's Edit
  bool toggleEnabled = false;   // the menu's Enable / Disable
  uint64_t droppedHere = 0;     // an op dragged onto this row, by id
};
CamOpRowResult CamOperationRow(AppState& state, const cam::Setup& setup,
                               const cam::Operation& op,
                               const std::vector<uint64_t>& order);

// What an operation IS, in a phrase, from its own fields and its type: a
// rotary op says its pattern, stepover and lead, a pocket its depth and
// passes. Not "deep in 1 pass, climb" on an op where neither word means
// anything.
[[nodiscard]] std::string CamOpSummary(const cam::Setup& setup,
                                       const cam::Operation& op);
// What its toolpath came to: moves, cutting length, and how low it went --
// as a RADIUS for a rotary op, whose Z is the distance from the axis.
[[nodiscard]] std::string CamOpPathLine(const cam::Operation& op,
                                        const cam::Toolpath& tp);

// An operation's settings -- what the panel's "Advanced" tree held -- drawn by
// the CAM tool window when an op is being edited. True when a field changed;
// the caller then calls ApplyCamOpEdit.
bool DrawCamOpSettings(AppState& state, cam::Setup& setup, cam::Operation& op);
// After an edit to one operation: re-cut only it, mark the project dirty, and
// drop a verdict about the previous toolpath.
void ApplyCamOpEdit(AppState& state, uint64_t opId);

// One setup tool's fields (name, diameter, flute, feed, end shape, tip angle,
// holder), drawn by the CAM tool window's tool editor. True when one changed;
// the caller then calls ApplyCamToolEdit, which re-cuts every operation,
// since any of them may name the tool.
bool DrawCamToolFields(AppState& state, cam::Tool& tool);
void ApplyCamToolEdit(AppState& state);

// The operations of `setup` that cut with tool `number`, by name.
[[nodiscard]] std::vector<std::string> CamOpsUsingTool(const cam::Setup& setup,
                                                       int number);
// Removes tool `number` from the active setup, as one undo step. REFUSED,
// false with the reason in `why`, while an operation cuts with it: deleting
// those too would lose work, and leaving them naming a missing number is a
// silent substitution.
bool RemoveCamTool(AppState& state, int number, std::string* why = nullptr);

// The tool window's editors for the two things the panel's tree lists that
// have settings but are not an operation, a tool or a fixture
// (docs/ui/panels.md#cam-job-tab-the-cam-panel): the setup being edited
// (`camEditSetup`) and a job part's placement (`camEditPart`). False when
// there is nothing to edit, so the caller draws the next thing.
bool DrawCamSetupEditor(AppState& state);
bool DrawCamPartEditor(AppState& state);

} // namespace ui
