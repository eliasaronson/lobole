#pragma once

#include "cad/SketchPlane.h"
#include "ui/AppState.h"

#include <glm/glm.hpp>
#include <optional>
#include <string>
#include <vector>

class SketchFeature;
class DatumPlaneFeature;
struct LineVertex3D;

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

// Starts a new sketch that tracks a datum plane: bakes the datum's current
// plane and records planeRef, so the sketch re-derives its plane whenever the
// datum regenerates. Returns the new sketch's id.
uint64_t BeginSketchOnDatum(AppState& state, const DatumPlaneFeature& datum,
                            const std::string& name);

// The stable element ref for a picked body face ({meshId, faceIndex}), or
// nullopt if that face isn't named (only extrude-NewBody faces are, so far).
std::optional<ElementRef> FaceElementRef(AppState& state, uint32_t meshId,
                                         uint32_t faceIndex);

// Starts a new sketch attached to a body face by an ElementRef: bakes the
// face's current plane and records faceRef, so the sketch (and everything
// built on it) follows edits to the base body. Returns the new sketch's id.
uint64_t BeginSketchOnFace(AppState& state, const ElementRef& ref,
                           const SketchPlane& plane, const std::string& name);

// Re-points an existing face-attached sketch at another face (one undoable
// edit). Backs the "Reattach face…" flow.
void ReattachSketchFace(AppState& state, uint64_t sketchId,
                        const ElementRef& ref);

// Handles viewport interaction while picking a plane or drawing. Called from
// the viewport-input sketch branch. Returns true if it consumed the frame's
// interaction (so normal face-picking is skipped).
bool HandleSketchViewport(AppState& state, const glm::vec2& mousePos,
                          bool overViewport);

// Appends the in-progress rubber-band preview (pending point → cursor) to the
// sketch renderer's overlay. No-op when not drawing.
void BuildSketchOverlay(AppState& state, std::vector<LineVertex3D>& out);

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

// Draws the active tool's staged prompt (+ any transient actionMsg warning) as
// a pill centred at the top of the viewport. No-op outside sketch edit / for
// the Select tool with nothing to say. Call once per frame from the UI pass.
void DrawToolPrompt(AppState& state);

// Mirror activation hook (U8 polish): a pre-selection containing exactly one
// construction line among other entities names its own axis — the mirror
// commits immediately. No-op otherwise (the two-step flow proceeds).
void MirrorToolActivated(AppState& state);

// Projects a plane-UV point to viewport screen coords (pixels), matching the
// picking/render path.
// Projects a world-space point to viewport pixel coordinates (proj*view then
// NDC → screen, offset by the viewport origin).
glm::vec2 WorldToScreen(AppState& state, const glm::vec3& world);

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

// All pick candidates under the cursor, best first: endpoints within a
// slightly larger tolerance than `tolPx` (points outrank entity bodies even
// when the body is closer), then bodies within `tolPx` (nearest first), then
// the datum axes. Same-spot click cycling in the Select tool walks this list.
std::vector<SketchPick> PickSketchCandidates(AppState& state,
                                             const SketchFeature& sketch,
                                             glm::vec2 mousePos, float tolPx);

// Endpoint-first pick: the best candidate — an endpoint hit if the cursor is
// near one, else the nearest line segment / circle circumference within
// `tolPx` as a whole-entity hit (`body = true`).
SketchPick PickSketchEntity(AppState& state, const SketchFeature& sketch,
                            glm::vec2 mousePos, float tolPx);

// Adds a constraint from the current selection, solves, and commits one
// ModifyFeatureCommand. Returns false if the selection isn't valid. A
// non-empty `valueExpr` is stored as the driving expression (`value` must
// already be its evaluation — regeneration re-evaluates it). A non-null
// `labelOffset` places the dimension label there instead of the seeded
// default (the placement-stage path).
bool ApplySketchConstraint(AppState& state, sketch::ConstraintKind kind,
                           double value, const std::string& valueExpr = {},
                           const glm::vec2* labelOffset = nullptr);

// Dimension-on-create: while a draw tool is live, typing a digit opens a
// small value field near the cursor; Enter applies it as a driving dimension
// on the entity just drawn (line length / circle-arc radius / rectangle
// width then height). Esc dismisses without touching the tool's staged
// state. Call once per frame from the UI pass (after DrawDimensions).
void DrawDimensionOnCreate(AppState& state);

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

// Edits a linked pattern's full transform (linear step / circular centre in
// `vec`, circular step angle in `scalar` radians, `count`; linear may also
// carry the v50 second grid direction `vec2`/`count2`) and regenerates it
// id-stably. No-op if unchanged or count < 2. One undo record.
bool EditPatternParams(AppState& state, size_t opIndex, glm::vec2 vec,
                       double scalar, int count, glm::vec2 vec2 = {0.0f, 0.0f},
                       int count2 = 1);

// Removes a linked pattern (its op + generated entities) and regenerates. One
// undo record. No-op if opIndex is out of range.
bool RemovePattern(AppState& state, size_t opIndex);

// Sets a dimension constraint's value (by index in the active sketch),
// re-solves, and commits one undo record. No-op if the index isn't a valid
// constraint. Used by the panel's radius editor and dimension edits.
bool EditDimensionValue(AppState& state, int dimIndex, double value);

// Offsets the selected closed loop by the offset params in SketchEditState and
// commits one undoable edit. Returns false if nothing is selected or the
// selection isn't a single closed loop.
bool ApplyOffset(AppState& state);

} // namespace ui
