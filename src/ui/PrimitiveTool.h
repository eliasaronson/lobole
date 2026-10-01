#pragma once

#include "core/Primitives.h"
#include "ui/AppState.h"

#include <cstdint>
#include <string>

// The toolbar's Box / Cylinder / Sphere (docs/ui/tool-sessions.md#primitives).
//
// A primitive is what a person would draw — a dimensioned sketch on a face
// or a plane, extruded or revolved (core/Primitives.h,
// docs/modeling/README.md#primitives) — so the tool starts where that
// starts: with the thing it stands on. Clicking the tool arms the plane pick
// (SketchPurpose::Primitive); a planar face, a base plane or a datum plane
// starts the preview, which is the expanded sketch AND its solid, pending
// together (AppState::pendingCompanionId) and committed as one undo step.
// The tool window edits the spec, and dragging the ghost moves its offset in
// the reference's frame, snapping to the face's centre and edges.
namespace ui {

// The toolbar action. A planar face or a datum plane already selected is
// the reference (select-first); otherwise the plane pick arms, and a
// preselected face that cannot carry one (curved) says why.
void StartPrimitiveTool(AppState& state, primitive::Kind kind);

// Starts the preview on `on` with the tool's armed kind and derived
// defaults. Returns "" or the refusal, leaving the pick armed.
std::string BeginPrimitiveOn(AppState& state, const primitive::On& on);

// The same from a viewport face hit: refuses a curved face and one with no
// stable name (there is no world-baked fallback for a primitive).
std::string BeginPrimitiveOnFace(AppState& state, uint32_t meshId,
                                 uint32_t faceIndex);

// The preview the session describes is the pending one.
bool PrimitiveSessionActive(const AppState& state);

// The tool window's body while the session is live: the primitive's fields,
// which rewrite the preview (primitive::Refill) as they change.
void DrawPrimitiveParamWidgets(AppState& state);

// Dragging the ghost: a press on it grabs, the drag moves offset (u, v) on
// its plane, release ends. Returns true when the event was consumed.
bool HandlePrimitiveDragInput(AppState& state, bool overViewport);
// Escape mid-drag: the offset goes back to where it was at grab.
void AbortPrimitiveDrag(AppState& state);

// What the drag snapped to, drawn at the primitive's centre.
void DrawPrimitiveSnapCue(AppState& state);

// One axis of the snap, in the plane's frame. `centre` is where the
// primitive's centre would go, `half` its half-size along the axis; with an
// outline (`hasExtent`) its sides snap flush with [lo, hi] and its centre
// onto either edge line, and always onto the reference's own centre (0).
// Ctrl frees the snap, so `capture` = 0 returns `centre` untouched.
struct AxisSnap {
  double value = 0.0;
  const char* what = ""; // "" = free, else "centre" / "flush" / "on edge"
};
AxisSnap SnapPrimitiveAxis(double centre, double half, bool hasExtent,
                           double lo, double hi, double capture);

} // namespace ui
