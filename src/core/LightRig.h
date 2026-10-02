#pragma once

#include "core/Conventions.h"

#include <glm/glm.hpp>

#include <cmath>

// The studio light rig: where the key, fill and rim stand, and which way the
// image-based environment faces. docs/rendering/README.md#the-light-rig.
//
// ONE derivation, shared by every consumer — the mesh and stock renderers,
// the shadow map (which renders depth from `key`), every screenshot and
// capture path (they re-record those renderers) and the ray-traced still
// (MakeStillDesc). A still of the viewport is therefore lit the way the
// viewport is. The colours and intensities are conv::kKeyLight & co.; only
// the placement lives here.
//
// By default the rig is CAMERA-RELATIVE, as in every mainstream CAD viewport
// (a headlight rig): orbit to the back of a part and the side you are looking
// at is lit exactly as the front was. The world-fixed rig had the key in the
// upper front-right, so from behind only the dim fill reached what you saw.
// "Lock lights to model" (Settings::lockLightsToModel) freezes the rig where
// the camera-relative one stands at the HOME view, so turning the lock on at
// the home view changes nothing on screen — it only stops the rig following
// the camera from then on.
namespace conv {

// An orthonormal camera frame in world coordinates. `back` points from the
// view target toward the camera (the opposite of the view direction).
struct ViewBasis {
  glm::vec3 right{1.0f, 0.0f, 0.0f};
  glm::vec3 up{0.0f, 1.0f, 0.0f};
  glm::vec3 back{0.0f, 0.0f, 1.0f};
};

struct LightRig {
  // Unit vectors TOWARD each light, in world space.
  glm::vec3 key{0.0f};
  glm::vec3 fill{0.0f};
  glm::vec3 rim{0.0f};
  // A world direction never parallel to `key` (45 degrees from it): the
  // shadow map's roll. A fixed axis would degenerate for the key directions
  // a camera-relative rig can now produce.
  glm::vec3 up{0.0f};
  // World direction -> the direction the environment maps are sampled in.
  // Camera-relative, the studio turns with the camera, so a face is lit by
  // the IBL according to how it faces the VIEWER; identity when locked.
  glm::mat3 envRotation{1.0f};
};

// The camera frame out of a world->view matrix (a rigid look-at: its
// rotation rows are the camera's axes).
inline ViewBasis BasisFromView(const glm::mat4& view) {
  return {glm::normalize(glm::vec3(view[0][0], view[1][0], view[2][0])),
          glm::normalize(glm::vec3(view[0][1], view[1][1], view[2][1])),
          glm::normalize(glm::vec3(view[0][2], view[1][2], view[2][2]))};
}

// The home (reset) view's frame: Camera::DefaultIsometricPosition looking at
// the origin, rolled so world up is screen up. That function lives in render/
// and core cannot include it; tests/test_light_rig.cpp holds the two equal.
inline ViewBasis HomeViewBasis() {
  const glm::vec3 back = glm::normalize(glm::vec3(1.0f) - 0.25f * kWorldUpF);
  const glm::vec3 right = glm::normalize(glm::cross(kWorldUpF, back));
  return {right, glm::cross(back, right), back};
}

// The rig placed in a camera frame. Key 40 degrees to the camera's LEFT and
// 45 up: off the view axis, so it models the form instead of flattening it
// as a headlamp does, and throws its shadow down and to the right, where the
// eye reads a form lit from the upper left as convex. Fill opposite it (60
// degrees right) and barely lifted (15), opening the shadow side without a
// second shadow. Rim behind and high (55 up) for the bright edge that
// separates a body from what is behind it. "Up" is SCREEN up, so the rig is
// defined from every viewpoint, straight down the up axis included.
inline LightRig CameraLightRig(const ViewBasis& b) {
  auto place = [&](float leftDeg, float liftDeg) {
    const float s = glm::radians(leftDeg), l = glm::radians(liftDeg);
    const glm::vec3 swung = std::cos(s) * b.back - std::sin(s) * b.right;
    return glm::normalize(std::cos(l) * swung + std::sin(l) * b.up);
  };
  LightRig r;
  r.key = place(40.0f, 45.0f);
  r.fill = place(-60.0f, 15.0f);
  r.rim = place(180.0f, 55.0f);
  r.up = b.up;
  // world -> camera coordinates -> the same coordinates in the home frame:
  // at the home view this is the identity, so locking there changes nothing.
  const ViewBasis h = HomeViewBasis();
  const glm::mat3 home(h.right, h.up, h.back);
  const glm::mat3 cam(b.right, b.up, b.back);
  r.envRotation = home * glm::transpose(cam);
  return r;
}

// World-fixed: the camera-relative rig as it stands at the home view.
inline LightRig ModelLightRig() {
  LightRig r = CameraLightRig(HomeViewBasis());
  r.envRotation = glm::mat3(1.0f); // exactly, not to rounding
  return r;
}

// What every renderer asks for.
inline LightRig StudioRig(const glm::mat4& view, bool lockedToModel) {
  return lockedToModel ? ModelLightRig() : CameraLightRig(BasisFromView(view));
}

} // namespace conv
