#pragma once

#include "ui/RayMath.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <span>

struct Ray; // cad/FaceSelector.h

namespace ui {

// One draggable arrow or arc in the viewport (docs/ui/viewport-handles.md).
//
// Three owners emit these — the pending feature's direct-manipulation handle,
// the active section's offset arrow, and the assembly gizmo — and they share
// exactly two things: which handle is under the cursor, and how it is drawn.
// They deliberately do NOT share the drag payload: one drags a scalar feature
// parameter, one drags view state, one drags an occurrence transform or a
// mate DOF. Merging those gives a union with flags deciding which half is
// live, which is what the section handle's `HandleDrag::sectionId` graft
// already looked like at a third of the size.
enum class HandleKind { Linear, Rotate };

// Who owns a handle. Encoded in the id so ids are unique across owners
// without any of them knowing about the others.
enum class HandleOwner : uint8_t {
  None = 0,
  Feature = 1, // the pending preview feature's headline scalar
  Section = 2, // the active section's offset
  Gizmo = 3,   // an occurrence / mate DOF / explode offset axis
};

// A handle id: the owner plus whatever that owner needs to tell its own
// handles apart (an axis index, a section id).
constexpr uint64_t HandleId(HandleOwner owner, uint64_t sub = 0) {
  return (static_cast<uint64_t>(owner) << 56) | sub;
}
constexpr HandleOwner OwnerOf(uint64_t id) {
  return static_cast<HandleOwner>(id >> 56);
}
// The gizmo's sub-id: which axis, and arrow vs arc.
constexpr uint64_t GizmoSub(int axis, bool rotate) {
  return static_cast<uint64_t>(axis) | (rotate ? 0x10u : 0u);
}
constexpr int GizmoAxisOf(uint64_t sub) { return static_cast<int>(sub & 0xF); }
constexpr bool GizmoRotateOf(uint64_t sub) { return (sub & 0x10u) != 0; }

constexpr uint64_t SubOf(uint64_t id) { return id & ((uint64_t(1) << 56) - 1); }

struct ViewportHandle {
  uint64_t id = 0;
  HandleKind kind = HandleKind::Linear;
  glm::vec3 origin{0.0f};          // arrow base, or arc centre
  glm::vec3 dir{0.0f, 0.0f, 1.0f}; // arrow direction, or arc plane normal
  float length = 1.0f;             // arrow length, or arc radius
  glm::vec3 color{1.0f};
};

// The nearest handle under `ray` within `grab` world units, or -1. Identical
// to the two hit tests it replaces: a linear handle is measured at its
// closest ray-axis point (clamped to the shaft, so the ray does not "hit" an
// arrow a metre behind its tip), an arc at its radius in its own plane.
inline int HitTestHandles(std::span<const ViewportHandle> handles,
                          const Ray& ray, float grab);

// Brighter tint for the handle under the cursor / being dragged, so feedback
// is per-handle rather than one accent for all — the gizmo's X/Y/Z must stay
// distinguishable while hovered.
inline glm::vec3 HandleTint(const ViewportHandle& h, uint64_t hovered,
                            uint64_t active) {
  if (h.id != 0 && h.id == active)
    return glm::min(h.color * 1.9f + glm::vec3(0.25f), glm::vec3(1.0f));
  if (h.id != 0 && h.id == hovered)
    return glm::min(h.color * 1.45f + glm::vec3(0.12f), glm::vec3(1.0f));
  return h.color;
}

} // namespace ui

// The implementation needs Ray's definition; keep the header usable from
// translation units that only want the types above.
#include "cad/FaceSelector.h"

namespace ui {

inline int HitTestHandles(std::span<const ViewportHandle> handles,
                          const Ray& ray, float grab) {
  int best = -1;
  float bestDist = grab;
  for (size_t i = 0; i < handles.size(); i++) {
    const ViewportHandle& h = handles[i];
    if (h.kind == HandleKind::Linear) {
      const float t =
          std::clamp(RayAxisParam(ray.origin, ray.direction, h.origin, h.dir),
                     0.15f * h.length, h.length);
      const float d =
          PointRayDist(h.origin + h.dir * t, ray.origin, ray.direction);
      if (d < bestDist) {
        bestDist = d;
        best = static_cast<int>(i);
      }
    } else {
      glm::vec3 hit;
      if (RayPlane(ray.origin, ray.direction, h.origin, h.dir, hit)) {
        const float d = std::abs(glm::length(hit - h.origin) - h.length);
        if (d < bestDist) {
          bestDist = d;
          best = static_cast<int>(i);
        }
      }
    }
  }
  return best;
}

} // namespace ui
