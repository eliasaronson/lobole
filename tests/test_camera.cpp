#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "Camera.h"

#include <cmath>

using Catch::Matchers::WithinAbs;
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

TEST_CASE("Camera basis stays orthonormal under arbitrary tumbling", "[camera]") {
  // The quaternion orientation can never drift: forward/up/right stay a unit
  // orthonormal frame no matter how the view is orbited and rolled.
  Camera cam;
  for (int i = 0; i < 200; i++) {
    cam.Orbit(53.0, -91.0);
    cam.Roll(17.0);
    const glm::vec3 f = cam.GetForward(), u = cam.GetUp(), r = cam.GetRight();
    CHECK_THAT(glm::length(f), WithinAbs(1.0f, 1e-3f));
    CHECK_THAT(glm::length(u), WithinAbs(1.0f, 1e-3f));
    CHECK_THAT(glm::length(r), WithinAbs(1.0f, 1e-3f));
    CHECK_THAT(glm::dot(f, u), WithinAbs(0.0f, 1e-3f));
    CHECK_THAT(glm::dot(f, r), WithinAbs(0.0f, 1e-3f));
    CHECK_THAT(glm::dot(u, r), WithinAbs(0.0f, 1e-3f));
  }
}

TEST_CASE("Resetting the view after tumbling is never degenerate", "[camera]") {
  // The exact bug the refactor removes: a stale up vector left the reset view
  // collapsed (right = forward x up == 0). Orientation is the only state now,
  // so a look-at reset always yields a valid frame.
  Camera cam;
  for (int i = 0; i < 50; i++)
    cam.Orbit(0.0, 130.0); // tumble repeatedly through the poles
  cam.SetPosition(glm::vec3(5, 5, 5));
  cam.SetTarget(glm::vec3(0, 0, 0));
  cam.SetUp(glm::vec3(0, 1, 0));

  const glm::vec3 pos = cam.GetPosition();
  CHECK_THAT(pos.x, WithinAbs(5.0f, 1e-3f));
  CHECK_THAT(pos.y, WithinAbs(5.0f, 1e-3f));
  CHECK_THAT(pos.z, WithinAbs(5.0f, 1e-3f));
  CHECK_THAT(glm::length(cam.GetRight()), WithinAbs(1.0f, 1e-3f)); // not collapsed
  CHECK(std::isfinite(cam.GetViewMatrix()[0][0]));
}

TEST_CASE("SetTarget keeps the position; SetPosition keeps the target",
          "[camera]") {
  Camera cam;
  const glm::vec3 p = cam.GetPosition();
  cam.SetTarget(glm::vec3(2, 1, 0));
  CHECK_THAT(glm::length(cam.GetPosition() - p), WithinAbs(0.0f, 1e-3f));
  CHECK_THAT(glm::length(cam.GetTarget() - glm::vec3(2, 1, 0)),
             WithinAbs(0.0f, 1e-3f));

  const glm::vec3 t = cam.GetTarget();
  cam.SetPosition(glm::vec3(-3, 4, 2));
  CHECK_THAT(glm::length(cam.GetTarget() - t), WithinAbs(0.0f, 1e-3f));
  CHECK_THAT(glm::length(cam.GetPosition() - glm::vec3(-3, 4, 2)),
             WithinAbs(0.0f, 1e-3f));
}

TEST_CASE("AlignToFace on a horizontal face resolves to a valid top-down view",
          "[camera]") {
  // faceNormal == world-up is the collinear case the old code special-cased by
  // hand; the guarded look-at now handles it, so the animated result is a clean
  // straight-down view rather than a NaN.
  Camera cam;
  for (int i = 0; i < 10; i++)
    cam.Orbit(40.0, 40.0); // arbitrary starting orientation
  cam.AlignToFace(glm::vec3(0, 0, 0), glm::vec3(0, 1, 0), 5.0f);
  cam.Update(2.0f); // run the 1s animation to completion in one step

  const glm::vec3 f = cam.GetForward();
  CHECK_THAT(glm::dot(f, glm::vec3(0, -1, 0)), WithinAbs(1.0f, 1e-2f)); // looks down
  CHECK_THAT(glm::length(cam.GetRight()), WithinAbs(1.0f, 1e-3f));      // valid frame
  CHECK_THAT(cam.GetPosition().y, WithinRel(5.0f, 1e-2f)); // faceCenter + n*dist
  CHECK(std::isfinite(cam.GetPosition().x));
}
