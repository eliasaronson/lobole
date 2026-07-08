#pragma once

#include "SketchPlane.h"
#include "ui/AppState.h"

#include <glm/glm.hpp>
#include <optional>
#include <string>
#include <vector>

class SketchFeature;
struct Vertex3D;

namespace ui {

// The three default sketch planes, indexed 0 Front / 1 Top / 2 Right,
// matching BuildPlaneQuads and PlaneType XY/XZ/YZ.
SketchPlane DefaultPlane(int index);

// Resolves the active sketch's current index from its stable id; exits edit
// mode (returns nullptr) if it no longer exists (e.g. undone). Call before
// issuing any index-based command against the sketch.
SketchFeature* ResolveActiveSketch(AppState& state, size_t* outIndex = nullptr);

// Starts a new sketch on the given plane: pushes an AddFeatureCommand, enters
// edit mode on it, and aligns the camera. Returns the new sketch's id.
uint64_t BeginSketchOnPlane(AppState& state, const SketchPlane& plane,
                            const std::string& name);

// Handles viewport interaction while picking a plane or drawing. Called from
// the viewport-input sketch branch. Returns true if it consumed the frame's
// interaction (so normal face-picking is skipped).
bool HandleSketchViewport(AppState& state, const glm::vec2& mousePos,
                          bool overViewport);

// Appends the in-progress rubber-band preview (pending point → cursor) to the
// sketch renderer's overlay. No-op when not drawing.
void BuildSketchOverlay(AppState& state, std::vector<Vertex3D>& out);

// Draws the auto-constraint snap indicator + ghost glyphs (H, V, coincident, …)
// at the snapped cursor, via ImGui's foreground draw list. No-op when not
// snapping. Call once per frame from the UI pass.
void DrawSnapGlyphs(AppState& state);

// Draws the rubber-band selection rectangle while a box-select drag is active
// (blue solid = window / left→right; green dashed = crossing / right→left).
// No-op otherwise. Call once per frame from the UI pass.
void DrawSelectionBox(AppState& state);

// Draws dimension annotations (extension/dimension lines, arrows, value text)
// for the active sketch's driving dimensions, and handles double-click-to-edit
// their value. Call once per frame from the UI pass.
void DrawDimensions(AppState& state);

// Draws small badges (H, V, =, ⟂, ∥, ●, ⌗) on the geometry for the active
// sketch's non-dimensional constraints. Gated on showConstraintGlyphs. Call
// once per frame from the UI pass.
void DrawConstraintGlyphs(AppState& state);

// The staged prompt for the active tool: what the current click will do and,
// for multi-step tools, which step of how many. `step`/`stepCount` are 0 for
// single-action tools; `text` is empty for the Select tool. Pure function of
// the edit state (no ImGui) so the step logic is testable.
struct ToolPrompt {
  std::string text;
  int step = 0;
  int stepCount = 0;
};
ToolPrompt SketchToolPrompt(const SketchEditState& edit);

// Draws the active tool's staged prompt (+ any transient actionMsg warning) as a
// pill centred at the top of the viewport. No-op outside sketch edit / for the
// Select tool with nothing to say. Call once per frame from the UI pass.
void DrawToolPrompt(AppState& state);

// Projects a plane-UV point to viewport screen coords (pixels), matching the
// picking/render path.
glm::vec2 SketchUVToScreen(AppState& state, const SketchPlane& plane,
                           glm::vec2 uv);

// Screen-space pick of a sketch element within tolerance. `body` is true when
// the hit is on an entity's interior (line segment / circle rim) rather than an
// endpoint — in that case `ref` names the whole entity (`{id, 0}`).
struct SketchPick {
  bool hit = false;
  bool body = false;
  sketch::PointRef ref;
};
SketchPick PickSketchPoint(AppState& state, const SketchFeature& sketch,
                           glm::vec2 mousePos, float tolPx);

// Endpoint-first pick: returns an endpoint hit if the cursor is within `tolPx`
// of one, else the nearest line segment / circle circumference within `tolPx`
// as a whole-entity hit (`body = true`).
SketchPick PickSketchEntity(AppState& state, const SketchFeature& sketch,
                            glm::vec2 mousePos, float tolPx);

// Adds a constraint from the current selection, solves, and commits one
// ModifyFeatureCommand. Returns false if the selection isn't valid.
bool ApplySketchConstraint(AppState& state, sketch::ConstraintKind kind,
                           double value);

// Toggles the construction flag on the selected entities (one undoable edit).
// Returns false if nothing is selected.
bool ToggleConstruction(AppState& state);

// Re-solves the active sketch (without mutating it) and stores the DOF +
// conflicting-constraint indices in SketchEditState, for status feedback and
// coloring. Call once per frame while editing.
void UpdateSketchStatus(AppState& state);

// Removes the constraint at `index` from the active sketch, re-solves, and
// commits one undoable edit.
bool DeleteConstraint(AppState& state, size_t index);

// Patterns the current selection using the pattern params in SketchEditState
// (linear or circular, linked = SketchEditState::linkGenerative). Commits one
// undoable edit. Returns false if nothing is selected.
bool ApplyPattern(AppState& state);

// Sets the count of the linked pattern (derived op) at `opIndex` on the active
// sketch, regenerates, and commits one undoable edit.
bool SetPatternCount(AppState& state, size_t opIndex, int count);

// Offsets the selected closed loop by the offset params in SketchEditState and
// commits one undoable edit. Returns false if nothing is selected or the
// selection isn't a single closed loop.
bool ApplyOffset(AppState& state);

} // namespace ui
