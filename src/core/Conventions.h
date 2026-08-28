#pragma once

#include <glm/glm.hpp>

#include <string_view>

// The world frame, stated ONCE.
//
// Four subsystems (simulation gravity, drawing views, CAM's spindle, the
// cutting simulation) have independently assumed an up axis that disagreed
// with the app's, because the app's choice was implicit in scattered
// vec3(0,1,0) literals and hand-written view tables. Everything that MEANS
// "up" — a named view, a plane name, a gravity default, a light rig — derives
// from the constants here, and tests/test_world_up.cpp fails if any consumer
// drifts. docs/up-axis.md is the full story.
//
// A literal (0,1,0) elsewhere in the tree is a claim that the value is NOT
// world up — a local basis pick, a UV axis, a face normal. If it means "up",
// it belongs routed through here.
namespace conv {

// The world basis. kWorldFront is the direction the FRONT of a part faces —
// equivalently, from the part toward the front-view camera. The three form a
// right-handed frame: kWorldRight x kWorldUp == kWorldFront (checked by the
// gate), so flipping kWorldUp requires re-deriving kWorldFront with it.
//
// Z-up, front toward -Y: the frame every convention we interoperate with
// uses — G-code's spindle, drawing practice, FreeCAD/SolidWorks/Onshape/
// Blender. The app was Y-up until 2026-08-28 (the renderer's habit, adopted
// as a model convention); docs/up-axis.md is the history and the flip.
inline constexpr glm::dvec3 kWorldUp{0.0, 0.0, 1.0};
inline constexpr glm::dvec3 kWorldFront{0.0, -1.0, 0.0};
inline constexpr glm::dvec3 kWorldRight{1.0, 0.0, 0.0};

// Float convenience for the render/UI layer.
inline const glm::vec3 kWorldUpF{kWorldUp};
inline const glm::vec3 kWorldFrontF{kWorldFront};
inline const glm::vec3 kWorldRightF{kWorldRight};

// Self-weight acceleration (m/s^2): straight down, by definition of down.
inline const glm::dvec3 kGravity = -9.81 * kWorldUp;

// One named-view table for every consumer: the view.* commands, the `view`
// RPC / screenshot(view=), the view cube's labels and clicks, and the
// drawings' OrientationAxes. Before this table existed there were four
// copies, and one of them (drawings) shipped wrong for months while the
// other three passed review.
struct NamedView {
  const char* name;    // RPC / command / serialization spelling ("front")
  const char* label;   // human spelling, as on the view cube ("Front")
  glm::dvec3 toCamera; // unit vector from the pivot toward the camera
  glm::dvec3 up;       // world direction that maps to screen up
};

// top/bottom roll: the plan-view fold. Looking down, the part's front edge
// sits at the BOTTOM of the screen/sheet — what puts a drawing's top view
// directly above its front view.
inline const NamedView kNamedViews[] = {
    {"front", "Front", kWorldFront, kWorldUp},
    {"back", "Back", -kWorldFront, kWorldUp},
    {"right", "Right", kWorldRight, kWorldUp},
    {"left", "Left", -kWorldRight, kWorldUp},
    {"top", "Top", kWorldUp, -kWorldFront},
    {"bottom", "Bottom", -kWorldUp, kWorldFront},
    {"iso", "Iso", glm::normalize(kWorldRight + kWorldUp + kWorldFront),
     kWorldUp},
};

inline const NamedView* FindView(std::string_view name) {
  for (const NamedView& v : kNamedViews)
    if (name == v.name)
      return &v;
  return nullptr;
}

// The named view a direction (pivot -> camera) corresponds to, or nullptr if
// it is not one of the six axis-aligned views (iso never matches: a cube face
// is never diagonal).
inline const NamedView* ViewForDirection(const glm::dvec3& toCamera) {
  for (const NamedView& v : kNamedViews)
    if (glm::dot(glm::normalize(toCamera), v.toCamera) > 0.999)
      return &v;
  return nullptr;
}

// The studio light rig, shared by the raster renderer, the shadow map and the
// ray tracer (three copies drifted before). Directions are TOWARD the light.
// Key from the upper front-right, fill from the lower back-left, rim from
// behind-right for edge definition.
inline const glm::vec3 kKeyLightDir =
    glm::normalize(glm::vec3(1.0f) + 0.5f * kWorldUpF);
inline const glm::vec3 kFillLightDir =
    glm::normalize(glm::vec3(-0.5f) - 0.3f * kWorldUpF);
inline const glm::vec3 kRimLightDir = glm::normalize(
    0.8f * kWorldRightF + 0.2f * kWorldUpF - 0.9f * kWorldFrontF);

} // namespace conv
