// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "render/Camera.h"

#include <cmath>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

TEST_CASE("Orbit preserves the distance to the target", "[camera]") {
  Camera cam;
  const glm::vec3 defaultPosition = Camera::DefaultIsometricPosition();
  const float d0 = glm::length(cam.GetPosition() - cam.GetTarget());
  CHECK_THAT(d0, WithinAbs(glm::length(defaultPosition), 1e-4f));
  cam.Orbit(37.0, -21.0);
  CHECK_THAT(glm::length(cam.GetPosition() - cam.GetTarget()),
             WithinRel(d0, 1e-4f));
}

TEST_CASE("Orbit does not gimbal-lock through vertical", "[camera]") {
  Camera cam;
  const glm::vec3 target = cam.GetTarget();
  const float d0 = glm::length(cam.GetPosition() - target);

  // Sweep vertically well past straight-up and straight-down, many times. The
  // old spherical orbit clamped near the poles and stopped moving; the
  // trackball keeps tumbling and stays finite with the distance preserved.
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

TEST_CASE("Camera basis stays orthonormal under arbitrary tumbling",
          "[camera]") {
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
  const glm::vec3 defaultPosition = Camera::DefaultIsometricPosition();
  cam.SetPosition(defaultPosition);
  cam.SetTarget(glm::vec3(0, 0, 0));
  cam.SetUp(glm::vec3(0, 1, 0));

  const glm::vec3 pos = cam.GetPosition();
  CHECK_THAT(pos.x, WithinAbs(defaultPosition.x, 1e-3f));
  CHECK_THAT(pos.y, WithinAbs(defaultPosition.y, 1e-3f));
  CHECK_THAT(pos.z, WithinAbs(defaultPosition.z, 1e-3f));
  CHECK_THAT(glm::length(cam.GetRight()),
             WithinAbs(1.0f, 1e-3f)); // not collapsed
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
  CHECK_THAT(glm::dot(f, glm::vec3(0, -1, 0)),
             WithinAbs(1.0f, 1e-2f)); // looks down
  CHECK_THAT(glm::length(cam.GetRight()),
             WithinAbs(1.0f, 1e-3f)); // valid frame
  CHECK_THAT(cam.GetPosition().y,
             WithinRel(5.0f, 1e-2f)); // faceCenter + n*dist
  CHECK(std::isfinite(cam.GetPosition().x));
}

TEST_CASE("SnapToView orients instantly with no animation", "[camera]") {
  Camera cam;
  for (int i = 0; i < 10; i++)
    cam.Orbit(40.0, 40.0); // arbitrary starting orientation
  const glm::vec3 pivot = cam.GetTarget();
  const float d0 = cam.GetDistance();

  cam.SnapToView(glm::vec3(0, 0, 1), glm::vec3(0, 1, 0)); // front
  CHECK(!cam.IsAnimating()); // effective immediately, no Update needed
  CHECK_THAT(glm::dot(cam.GetForward(), glm::vec3(0, 0, -1)),
             WithinAbs(1.0f, 1e-4f));
  CHECK_THAT(glm::dot(cam.GetUp(), glm::vec3(0, 1, 0)), WithinAbs(1.0f, 1e-4f));
  // Pivot and zoom are untouched: only the orientation snaps.
  CHECK_THAT(glm::length(cam.GetTarget() - pivot), WithinAbs(0.0f, 1e-5f));
  CHECK_THAT(cam.GetDistance(), WithinRel(d0, 1e-4f));

  // Straight-down view: the explicit up resolves the collinear-up case.
  cam.SnapToView(glm::vec3(0, 1, 0), glm::vec3(0, 0, -1)); // top
  CHECK_THAT(glm::dot(cam.GetForward(), glm::vec3(0, -1, 0)),
             WithinAbs(1.0f, 1e-4f));
  CHECK_THAT(glm::dot(cam.GetUp(), glm::vec3(0, 0, -1)),
             WithinAbs(1.0f, 1e-4f));
  CHECK_THAT(glm::length(cam.GetRight()), WithinAbs(1.0f, 1e-3f));
}

TEST_CASE("SnapToView cancels an in-flight animation", "[camera]") {
  Camera cam;
  cam.AlignToFace(glm::vec3(0, 0, 0), glm::vec3(0, 1, 0), 5.0f); // 1s animation
  CHECK(cam.IsAnimating());
  cam.SnapToView(glm::vec3(0, 0, 1), glm::vec3(0, 1, 0));
  CHECK(!cam.IsAnimating());
  cam.Update(0.5f); // a later frame must not resume the animation
  CHECK_THAT(glm::dot(cam.GetForward(), glm::vec3(0, 0, -1)),
             WithinAbs(1.0f, 1e-4f));
}

namespace {
glm::vec2 Pixel(const Camera& cam, const glm::vec3& p) {
  const glm::vec4 clip = cam.GetViewProjectionMatrix() * glm::vec4(p, 1.0f);
  return glm::vec2(clip) / clip.w;
}
} // namespace

TEST_CASE("ZoomAbout keeps the point on its pixel at any depth", "[camera]") {
  Camera cam;
  cam.SetAspectRatio(1.6f);
  // Off-centre, and nearer the camera than the pivot: a zoom that only held
  // points at the pivot's depth would move it.
  const glm::vec3 p = cam.GetTarget() + cam.GetRight() * 3.0f +
                      cam.GetUp() * -2.0f - cam.GetForward() * 5.0f;
  const glm::vec2 before = Pixel(cam, p);
  const float d0 = cam.GetDistance();
  for (const double delta : {-2.0, -2.0, 2.0, 5.0}) {
    cam.ZoomAbout(delta, p);
    CHECK_THAT(Pixel(cam, p).x, WithinAbs(before.x, 1e-4f));
    CHECK_THAT(Pixel(cam, p).y, WithinAbs(before.y, 1e-4f));
  }
  CHECK(cam.GetDistance() != d0); // it did zoom
}

TEST_CASE("ZoomAbout at the distance limit leaves the view alone", "[camera]") {
  Camera cam;
  cam.SetDistance(0.0f); // pinned at the minimum
  const glm::vec3 pivot = cam.GetTarget();
  const float d = cam.GetDistance();
  cam.ZoomAbout(-5.0, pivot + cam.GetRight() * 10.0f);
  CHECK_THAT(cam.GetDistance(), WithinAbs(d, 1e-6f));
  CHECK(glm::length(cam.GetTarget() - pivot) < 1e-5f);
}

TEST_CASE("ZoomAbout the pivot is a plain Zoom", "[camera]") {
  Camera a, b;
  a.ZoomAbout(-3.0, a.GetTarget());
  b.Zoom(-3.0);
  CHECK(glm::length(a.GetPosition() - b.GetPosition()) < 1e-4f);
  CHECK(glm::length(a.GetTarget() - b.GetTarget()) < 1e-5f);
}
