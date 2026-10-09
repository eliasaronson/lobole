// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// The studio light rig (core/LightRig.h,
// docs/rendering/README.md#the-light-rig).
//
// Through the real Camera: the rig is derived from the view matrix every
// renderer is handed, so these drive Camera::SnapToView and read
// GetViewMatrix rather than building a basis by hand.
#include <catch2/catch_test_macros.hpp>

#include "core/LightRig.h"
#include "core/io/Settings.h"
#include "render/Camera.h"

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <cmath>
#include <string>
#include <vector>

namespace {

bool Near(const glm::vec3& a, const glm::vec3& b, float tol = 1e-4f) {
  return glm::length(a - b) < tol;
}

bool Finite(const glm::vec3& v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// Every way a camera can stand: the named views (straight down and straight
// up included, with the table's roll), and a sweep of azimuths, elevations
// up to the poles, and rolls.
struct Pose {
  std::string name;
  glm::vec3 toCamera;
  glm::vec3 up;
};

std::vector<Pose> Poses() {
  std::vector<Pose> poses;
  for (const conv::NamedView& v : conv::kNamedViews)
    poses.push_back({v.name, glm::vec3(v.toCamera), glm::vec3(v.up)});
  // Exactly down the up axis with world-ish ups that are NOT the table's:
  // the frame must still be defined (no horizontal to swing about).
  poses.push_back({"down, +X up", conv::kWorldUpF, conv::kWorldRightF});
  poses.push_back({"down, -X up", conv::kWorldUpF, -conv::kWorldRightF});
  poses.push_back({"up, +X up", -conv::kWorldUpF, conv::kWorldRightF});
  for (int az = 0; az < 360; az += 30)
    for (int el = -90; el <= 90; el += 15) {
      const float a = glm::radians(float(az)), e = glm::radians(float(el));
      const glm::vec3 dir = std::cos(e) * (std::cos(a) * conv::kWorldRightF +
                                           std::sin(a) * conv::kWorldFrontF) +
                            std::sin(e) * conv::kWorldUpF;
      // At the poles world up is the view axis; screen up is then any
      // horizontal, as a camera arriving there by orbit would have it.
      const glm::vec3 up = std::abs(el) == 90
                               ? std::cos(a) * conv::kWorldFrontF -
                                     std::sin(a) * conv::kWorldRightF
                               : conv::kWorldUpF;
      poses.push_back(
          {"az " + std::to_string(az) + " el " + std::to_string(el), dir, up});
    }
  return poses;
}

Camera At(const Pose& p) {
  Camera cam;
  cam.SnapToView(glm::normalize(p.toCamera), p.up);
  return cam;
}

} // namespace

TEST_CASE("the key stays to the left of and above the view from any camera",
          "[light_rig]") {
  for (const Pose& p : Poses()) {
    INFO(p.name);
    const Camera cam = At(p);
    const glm::vec3 right = cam.GetRight(), up = cam.GetUp();
    const glm::vec3 back = -cam.GetForward();
    const conv::LightRig rig = conv::StudioRig(cam.GetViewMatrix(), false);
    REQUIRE(Finite(rig.key));
    REQUIRE(Finite(rig.fill));
    REQUIRE(Finite(rig.rim));
    CHECK(std::abs(glm::length(rig.key) - 1.0f) < 1e-4f);
    // Key: left, up, and on the camera's side, so what you see is lit.
    CHECK(glm::dot(rig.key, right) < -0.3f);
    CHECK(glm::dot(rig.key, up) > 0.6f);
    CHECK(glm::dot(rig.key, back) > 0.4f);
    // Fill on the other side, also toward the viewer; rim behind and high.
    CHECK(glm::dot(rig.fill, right) > 0.5f);
    CHECK(glm::dot(rig.fill, back) > 0.4f);
    CHECK(glm::dot(rig.rim, back) < -0.4f);
    CHECK(glm::dot(rig.rim, up) > 0.6f);
    // The shadow map's roll is never parallel to the key.
    CHECK(std::abs(glm::dot(rig.up, rig.key)) < 0.9f);
    // The environment turns with the camera: whatever faces the viewer
    // samples the studio where it does at the home view.
    CHECK(Near(rig.envRotation * back, conv::HomeViewBasis().back));
    CHECK(Near(rig.envRotation * up, conv::HomeViewBasis().up));
  }
}

TEST_CASE("the rig in camera coordinates is the same from every viewpoint",
          "[light_rig]") {
  // What "camera-relative" promises: the back of a part, seen from behind,
  // is lit exactly as the front was from the front.
  auto inCamera = [](const Camera& cam, const glm::vec3& w) {
    return glm::vec3(glm::dot(w, cam.GetRight()), glm::dot(w, cam.GetUp()),
                     glm::dot(w, -cam.GetForward()));
  };
  const Camera front = At({"front", conv::kWorldFrontF, conv::kWorldUpF});
  const conv::LightRig f = conv::StudioRig(front.GetViewMatrix(), false);
  for (const Pose& p : Poses()) {
    INFO(p.name);
    const Camera cam = At(p);
    const conv::LightRig r = conv::StudioRig(cam.GetViewMatrix(), false);
    CHECK(Near(inCamera(cam, r.key), inCamera(front, f.key)));
    CHECK(Near(inCamera(cam, r.fill), inCamera(front, f.fill)));
    CHECK(Near(inCamera(cam, r.rim), inCamera(front, f.rim)));
  }
}

TEST_CASE("locked to the model, the rig is where it stands at the home view",
          "[light_rig]") {
  // The home view's frame is the default camera's.
  const Camera home;
  const conv::ViewBasis h = conv::HomeViewBasis();
  CHECK(Near(h.right, home.GetRight()));
  CHECK(Near(h.up, home.GetUp()));
  CHECK(Near(h.back, -home.GetForward()));

  // Turning the lock on at the home view changes nothing on screen.
  const conv::LightRig free = conv::StudioRig(home.GetViewMatrix(), false);
  const conv::LightRig locked = conv::ModelLightRig();
  CHECK(Near(free.key, locked.key));
  CHECK(Near(free.fill, locked.fill));
  CHECK(Near(free.rim, locked.rim));
  for (int c = 0; c < 3; ++c)
    CHECK(Near(free.envRotation[c], glm::mat3(1.0f)[c]));

  // ...and from anywhere else it no longer follows the camera.
  for (const Pose& p : Poses()) {
    INFO(p.name);
    const conv::LightRig r = conv::StudioRig(At(p).GetViewMatrix(), true);
    CHECK(r.key == locked.key);
    CHECK(r.fill == locked.fill);
    CHECK(r.rim == locked.rim);
    CHECK(r.envRotation == glm::mat3(1.0f));
  }
  // Held against the world, the locked key is high, as the old fixed rig's
  // was: shadows fall down onto what a part stands on.
  CHECK(glm::dot(locked.key, conv::kWorldUpF) > 0.5f);
}

TEST_CASE("lock_lights_to_model round-trips and switches the rig",
          "[light_rig][settings]") {
  settings::Settings s;
  CHECK_FALSE(s.lockLightsToModel); // camera-relative by default
  std::string error;
  REQUIRE(settings::ApplyJson(s, {{"lock_lights_to_model", true}}, &error));
  CHECK(s.lockLightsToModel);
  CHECK(settings::ToJson(s).at("lock_lights_to_model").get<bool>());
  settings::Settings back;
  REQUIRE(settings::ApplyJson(back, settings::ToJson(s), &error));
  CHECK(back == s);
  // Refused, not coerced.
  CHECK_FALSE(
      settings::ApplyJson(back, {{"lock_lights_to_model", "yes"}}, &error));
  CHECK(error.find("lock_lights_to_model") != std::string::npos);

  // What the renderers do with it: from behind, the unlocked key lights the
  // face toward the camera; locked, it lights the face the home view saw.
  const Camera behind = At({"back", -conv::kWorldFrontF, conv::kWorldUpF});
  const glm::vec3 towardViewer = -behind.GetForward();
  const conv::LightRig unlocked = conv::StudioRig(
      behind.GetViewMatrix(), settings::Settings{}.lockLightsToModel);
  const conv::LightRig locked =
      conv::StudioRig(behind.GetViewMatrix(), s.lockLightsToModel);
  CHECK(glm::dot(unlocked.key, towardViewer) > 0.4f);
  CHECK(glm::dot(locked.key, towardViewer) < 0.0f);
}
