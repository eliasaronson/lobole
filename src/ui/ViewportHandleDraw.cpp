#include "ui/ViewportHandleDraw.h"

#include "ui/RayMath.h"       // PerpTo
#include "ui/SketchOverlay.h" // AppendPrimLine
#include "ui/ViewportHandle.h"

#include <glm/gtc/constants.hpp>

#include <cmath>

namespace ui {

uint64_t ActiveHandleId(const AppState& state) {
  if (state.gizmoDrag.active)
    return HandleId(HandleOwner::Gizmo,
                    GizmoSub(state.gizmoDrag.axis, state.gizmoDrag.rotate));
  if (state.handleDrag.active)
    return state.handleDrag.sectionId
               ? HandleId(HandleOwner::Section, state.handleDrag.sectionId)
               : HandleId(HandleOwner::Feature);
  if (state.simulation.loadDrag.active)
    return HandleId(HandleOwner::Load, state.simulation.loadDrag.loadId);
  return 0;
}

void AppendHandleArrow(const glm::vec3& base, const glm::vec3& dir,
                       float length, const glm::vec4& color,
                       std::vector<PrimVertex>& out) {
  // Head proportions are the gizmo's: 0.18 of the length, flared 45 degrees.
  const float head = 0.18f * length;
  const glm::vec3 tip = base + dir * length, side = PerpTo(dir);
  AppendPrimLine(base, tip, kHandleHalfPx, color, out);
  AppendPrimLine(tip, tip - dir * head + side * head, kHandleHalfPx, color,
                 out);
  AppendPrimLine(tip, tip - dir * head - side * head, kHandleHalfPx, color,
                 out);
}

void AppendHandleRing(const glm::vec3& centre, const glm::vec3& normal,
                      float radius, const glm::vec4& color,
                      std::vector<PrimVertex>& out) {
  const glm::vec3 b1 = PerpTo(normal), b2 = glm::cross(normal, b1);
  // 48 chords: 7.5 degrees each, so at a 4 px stroke the outside corner of a
  // joint is under a quarter pixel and the anti-aliasing hides it.
  constexpr int kSegments = 48;
  glm::vec3 prev = centre + b1 * radius;
  for (int s = 1; s <= kSegments; s++) {
    const float a = static_cast<float>(s) / kSegments * glm::two_pi<float>();
    const glm::vec3 p = centre + (std::cos(a) * b1 + std::sin(a) * b2) * radius;
    AppendPrimLine(prev, p, kHandleHalfPx, color, out);
    prev = p;
  }
}

void DrawViewportHandles(const AppState& state, std::vector<PrimVertex>& out) {
  const uint64_t active = ActiveHandleId(state);
  for (const ViewportHandle& h : state.viewportHandles) {
    const glm::vec4 color(HandleTint(h, state.hoveredHandle, active), 1.0f);
    if (h.kind == HandleKind::Linear)
      AppendHandleArrow(h.origin, h.dir, h.length, color, out);
    else
      AppendHandleRing(h.origin, h.dir, h.length, color, out);
  }
}

} // namespace ui
