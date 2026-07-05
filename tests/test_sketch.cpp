#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "SketchPlane.h"

#include <glm/glm.hpp>

using Catch::Matchers::WithinAbs;

static void CheckRoundtrip(const SketchPlane& plane, const glm::vec2& uv) {
    glm::vec3 world = plane.PlaneToWorld(uv);
    glm::vec2 back = plane.WorldToPlane(world);
    CHECK_THAT(back.x, WithinAbs(uv.x, 1e-4f));
    CHECK_THAT(back.y, WithinAbs(uv.y, 1e-4f));
}

TEST_CASE("Default planes roundtrip plane<->world coordinates", "[sketch][plane]") {
    SECTION("XY plane") {
        SketchPlane plane(PlaneType::XY);
        CheckRoundtrip(plane, {0.0f, 0.0f});
        CheckRoundtrip(plane, {1.5f, -2.5f});
    }
    SECTION("XZ plane") {
        SketchPlane plane(PlaneType::XZ);
        CheckRoundtrip(plane, {3.0f, 4.0f});
    }
    SECTION("YZ plane") {
        SketchPlane plane(PlaneType::YZ);
        CheckRoundtrip(plane, {-1.0f, 2.0f});
    }
}

TEST_CASE("Points on the plane stay on the plane", "[sketch][plane]") {
    SketchPlane plane(PlaneType::XY, glm::vec3(0.0f, 0.0f, 5.0f));
    glm::vec3 world = plane.PlaneToWorld({2.0f, 3.0f});
    float dist = glm::dot(world - plane.GetOrigin(), plane.GetNormal());
    CHECK_THAT(dist, WithinAbs(0.0f, 1e-4f));
}

TEST_CASE("Custom plane axes are orthonormal", "[sketch][plane]") {
    SketchPlane plane(glm::vec3(1, 2, 3), glm::vec3(0, 1, 1), glm::vec3(1, 0, 0));
    CHECK_THAT(glm::length(plane.GetNormal()), WithinAbs(1.0f, 1e-4f));
    CHECK_THAT(glm::length(plane.GetUAxis()), WithinAbs(1.0f, 1e-4f));
    CHECK_THAT(glm::length(plane.GetVAxis()), WithinAbs(1.0f, 1e-4f));
    CHECK_THAT(glm::dot(plane.GetNormal(), plane.GetUAxis()), WithinAbs(0.0f, 1e-4f));
    CHECK_THAT(glm::dot(plane.GetNormal(), plane.GetVAxis()), WithinAbs(0.0f, 1e-4f));
    CHECK_THAT(glm::dot(plane.GetUAxis(), plane.GetVAxis()), WithinAbs(0.0f, 1e-4f));
}
