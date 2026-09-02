#pragma once

#include "render/SketchRenderer.h" // PrimVertex
#include "ui/AppState.h"

#include <cstdint>
#include <vector>

namespace ui {

// Drawing for viewport handles (docs/ui/viewport-handles.md): ONE drawer for
// every owner. An owner publishes a ViewportHandle into
// AppState::viewportHandles and draws nothing itself; this pass then draws
// each published handle from the record the hit test uses, so what
// highlights under the cursor is what is on screen by construction rather
// than by three drawers agreeing. Before this, the gizmo, the preview
// handle and the FEM load arrow each drew their own arrow — two of them 1 px
// lines, one thick, with heads of different angles.

// Half-width of a handle's stroke, in pixels. The thick screen-space
// primitive pipeline takes a half-width; 2.0 draws a 4 px anti-aliased line.
inline constexpr float kHandleHalfPx = 2.0f;

// The handle being dragged right now — whichever owner is mid-drag — or 0.
uint64_t ActiveHandleId(const AppState& state);

// The shared shapes, for an owner's decorations that should match its handle
// (a section's plane outline, the preview arrow's base tick). Colour carries
// alpha so a decoration can be drawn fainter than the handle it belongs to.
// An arrow: shaft from `base` along unit `dir` for `length`, with an open
// two-stroke head. A ring: `segments` chords around `centre` in the plane
// normal to unit `normal`, starting at PerpTo(normal) — the same
// parametrisation RotateAngle measures against.
void AppendHandleArrow(const glm::vec3& base, const glm::vec3& dir,
                       float length, const glm::vec4& color,
                       std::vector<PrimVertex>& out);
void AppendHandleRing(const glm::vec3& centre, const glm::vec3& normal,
                      float radius, const glm::vec4& color,
                      std::vector<PrimVertex>& out);

// Draws every handle in state.viewportHandles, tinted by hover/drag, into
// `out`. Called ONCE per frame after every owner has published, into the
// on-top (depth test off) primitive channel: a handle must stay visible and
// grabbable through the geometry it moves.
void DrawViewportHandles(const AppState& state, std::vector<PrimVertex>& out);

} // namespace ui
