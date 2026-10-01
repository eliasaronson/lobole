#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "cad/SketchPlane.h"

#include <glm/glm.hpp>

using Catch::Matchers::WithinAbs;

static void CheckRoundtrip(const SketchPlane& plane, const glm::dvec2& uv) {
  glm::dvec3 world = plane.PlaneToWorld(uv);
  glm::dvec2 back = plane.WorldToPlane(world);
  CHECK_THAT(back.x, WithinAbs(uv.x, 1e-4));
  CHECK_THAT(back.y, WithinAbs(uv.y, 1e-4));
}

TEST_CASE("Default planes roundtrip plane<->world coordinates",
          "[sketch][plane]") {
  SECTION("XY plane") {
    SketchPlane plane(PlaneType::XY);
    CheckRoundtrip(plane, {0.0, 0.0});
    CheckRoundtrip(plane, {1.5, -2.5});
  }
  SECTION("XZ plane") {
    SketchPlane plane(PlaneType::XZ);
    CheckRoundtrip(plane, {3.0, 4.0});
  }
  SECTION("YZ plane") {
    SketchPlane plane(PlaneType::YZ);
    CheckRoundtrip(plane, {-1.0, 2.0});
  }
}

TEST_CASE("Points on the plane stay on the plane", "[sketch][plane]") {
  SketchPlane plane(PlaneType::XY, glm::dvec3(0.0, 0.0, 5.0));
  glm::dvec3 world = plane.PlaneToWorld({2.0, 3.0});
  double dist = glm::dot(world - plane.GetOrigin(), plane.GetNormal());
  CHECK_THAT(dist, WithinAbs(0.0, 1e-4));
}

TEST_CASE("Custom plane axes are orthonormal", "[sketch][plane]") {
  SketchPlane plane(glm::dvec3(1, 2, 3), glm::dvec3(0, 1, 1),
                    glm::dvec3(1, 0, 0));
  CHECK_THAT(glm::length(plane.GetNormal()), WithinAbs(1.0, 1e-4));
  CHECK_THAT(glm::length(plane.GetUAxis()), WithinAbs(1.0, 1e-4));
  CHECK_THAT(glm::length(plane.GetVAxis()), WithinAbs(1.0, 1e-4));
  CHECK_THAT(glm::dot(plane.GetNormal(), plane.GetUAxis()),
             WithinAbs(0.0, 1e-4));
  CHECK_THAT(glm::dot(plane.GetNormal(), plane.GetVAxis()),
             WithinAbs(0.0, 1e-4));
  CHECK_THAT(glm::dot(plane.GetUAxis(), plane.GetVAxis()),
             WithinAbs(0.0, 1e-4));
}
