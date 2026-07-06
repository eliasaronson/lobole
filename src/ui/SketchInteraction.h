#pragma once

#include "SketchPlane.h"
#include "ui/AppState.h"

#include <glm/glm.hpp>
#include <optional>
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

// Projects a plane-UV point to viewport screen coords (pixels), matching the
// picking/render path.
glm::vec2 SketchUVToScreen(AppState& state, const SketchPlane& plane,
                           glm::vec2 uv);

// Screen-space pick of the nearest sketch endpoint within tolerance.
struct SketchPick {
  bool hit = false;
  sketch::PointRef ref;
};
SketchPick PickSketchPoint(AppState& state, const SketchFeature& sketch,
                           glm::vec2 mousePos, float tolPx);

// Adds a constraint from the current selection, solves, and commits one
// ModifyFeatureCommand. Returns false if the selection isn't valid.
bool ApplySketchConstraint(AppState& state, sketch::ConstraintKind kind,
                           double value);

} // namespace ui
