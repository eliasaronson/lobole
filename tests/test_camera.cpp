#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "Camera.h"

#include <cmath>

using Catch::Matchers::WithinRel;

TEST_CASE("Orbit preserves the distance to the target", "[camera]") {
  Camera cam; // default: position (5,5,5), target origin
  const float d0 = glm::length(cam.GetPosition() - cam.GetTarget());
  cam.Orbit(37.0, -21.0);
  CHECK_THAT(glm::length(cam.GetPosition() - cam.GetTarget()),
             WithinRel(d0, 1e-4f));
}

TEST_CASE("Orbit does not gimbal-lock through vertical", "[camera]") {
  Camera cam;
  const glm::vec3 target = cam.GetTarget();
  const float d0 = glm::length(cam.GetPosition() - target);

  // Sweep vertically well past straight-up and straight-down, many times. The
  // old spherical orbit clamped near the poles and stopped moving; the trackball
  // keeps tumbling and stays finite with the distance preserved.
  glm::vec3 prev = cam.GetPosition();
  bool everStuck = false;
  for (int i = 0; i < 300; i++) {
    cam.Orbit(0.0, 120.0); // a big vertical drag each step
    const glm::vec3 p = cam.GetPosition();
    REQUIRE(std::isfinite(p.x));
    REQUIRE(std::isfinite(p.y));
    REQUIRE(std::isfinite(p.z));
    CHECK_THAT(glm::length(p - target), WithinRel(d0, 1e-3f));
    if (glm::length(p - prev) < 1e-4f)
      everStuck = true; // two consecutive frames identical → stuck at a pole
    prev = p;
  }
  CHECK_FALSE(everStuck);
}
