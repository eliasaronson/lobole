// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// Recognition on files another program wrote: tests/fixtures/mesh/*.stl are
// OpenSCAD's ASCII exports of the .scad beside each, whose header lists what
// is in it (docs/interchange/README.md#recognising-a-faceted-mesh). OpenSCAD
// prints six significant digits, so these also hold the reader to an ASCII
// file's own precision.

#include "TestPaths.h"

#include "core/mesh/MeshReaders.h"
#include "core/mesh/Segment.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <filesystem>

namespace {

mesh::TriMesh Read(const char* name) {
  const auto path =
      std::filesystem::path(TestSourceDir()) / "tests/fixtures/mesh" / name;
  mesh::TriMesh m;
  std::string error;
  INFO(error);
  REQUIRE(mesh::ReadStl(path.string(), m, error));
  return m;
}

double AxisAngle(const glm::dvec3& a, const glm::dvec3& b) {
  return std::acos(
      std::min(1.0, std::abs(glm::dot(glm::normalize(a), glm::normalize(b)))));
}

// How many regions of `kind` have this radius about this axis line.
int Count(const mesh::Segmentation& s, mesh::Kind kind, double radius,
          const glm::dvec3& point, const glm::dvec3& axis, double tol) {
  int n = 0;
  for (const mesh::Region& r : s.regions) {
    if (r.fit.kind != kind || AxisAngle(r.fit.axis, axis) > 1e-3 ||
        std::abs(r.fit.radius - radius) > tol)
      continue;
    const glm::dvec3 w = point - r.fit.origin;
    const glm::dvec3 a = glm::normalize(r.fit.axis);
    if (glm::length(w - glm::dot(w, a) * a) <= tol)
      n++;
  }
  return n;
}

} // namespace

TEST_CASE("OpenSCAD's motor plate: holes, counterbores and a nut trap",
          "[mesh_recognition]") {
  const mesh::TriMesh m = Read("nema17_mount.stl");
  REQUIRE(m.Closed());
  CHECK_THAT(m.quantum, Catch::Matchers::WithinRel(1e-4, 1e-9)); // 30.0000
  const mesh::Segmentation s = mesh::SegmentAll(m, {});
  const double tol = 1e-3;
  const glm::dvec3 z(0, 0, 1);
  CHECK(Count(s, mesh::Kind::Cylinder, 11.0, {0, 0, 0}, z, tol) == 1);
  for (double x : {-15.5, 15.5})
    for (double y : {-15.5, 15.5}) {
      CHECK(Count(s, mesh::Kind::Cylinder, 1.7, {x, y, 0}, z, tol) == 1);
      CHECK(Count(s, mesh::Kind::Cylinder, 3.0, {x, y, 0}, z, tol) == 1);
    }
  CHECK(s.Count(mesh::Kind::Cylinder) == 9);
  REQUIRE(s.polygonal.size() == 1);
  CHECK(s.polygonal[0].sides == 6);
  CHECK_THAT(s.polygonal[0].circumradius, Catch::Matchers::WithinAbs(3.2, tol));
  CHECK(s.facetsLeft == 0);
}

TEST_CASE("OpenSCAD's pipe clamp: a saddle, and a default-resolution hole",
          "[mesh_recognition]") {
  const mesh::TriMesh m = Read("pipe_clamp.stl");
  REQUIRE(m.Closed());
  const double tol = 1e-3;
  const glm::dvec3 x(1, 0, 0), z(0, 0, 1);
  const mesh::Segmentation s = mesh::SegmentAll(m, {});
  CHECK(Count(s, mesh::Kind::Cylinder, 12.5, {0, 0, 0}, x, tol) == 1);
  CHECK(Count(s, mesh::Kind::Cylinder, 20.0, {0, 0, 0}, x, tol) == 1);
  for (double y : {-27.0, 27.0})
    CHECK(Count(s, mesh::Kind::Cylinder, 2.25, {0, y, 0}, z, tol) == 1);
  // The hole OpenSCAD made five-sided is listed, not rounded.
  CHECK(Count(s, mesh::Kind::Cylinder, 1.5, {6, 27, 0}, z, tol) == 0);
  REQUIRE(s.polygonal.size() == 1);
  CHECK(s.polygonal[0].sides == 5);

  SECTION("min_sides 5 asks for it as a hole") {
    mesh::SegmentOptions o;
    o.minSides = 5;
    const mesh::Segmentation s5 = mesh::SegmentAll(m, o);
    CHECK(Count(s5, mesh::Kind::Cylinder, 1.5, {6, 27, 0}, z, tol) == 1);
    CHECK(s5.polygonal.empty());
  }
}

TEST_CASE("OpenSCAD's knob: a cone, a cylinder, a sphere and a bore",
          "[mesh_recognition]") {
  const mesh::TriMesh m = Read("knob.stl");
  REQUIRE(m.Closed());
  const double tol = 1e-3;
  const glm::dvec3 z(0, 0, 1);
  const mesh::Segmentation s = mesh::SegmentAll(m, {});
  CHECK(Count(s, mesh::Kind::Cylinder, 12.0, {0, 0, 0}, z, tol) == 1);
  CHECK(Count(s, mesh::Kind::Cylinder, 3.0, {0, 0, 0}, z, tol) == 1);
  int cones = 0, spheres = 0;
  for (const mesh::Region& r : s.regions) {
    if (r.fit.kind == mesh::Kind::Cone &&
        glm::length(r.fit.origin - glm::dvec3(0, 0, 30)) < 1e-2 &&
        std::abs(r.fit.halfAngle - std::atan(0.5)) < 1e-4)
      cones++;
    if (r.fit.kind == mesh::Kind::Sphere &&
        glm::length(r.fit.origin - glm::dvec3(0, 0, 14)) < tol &&
        std::abs(r.fit.radius - 12.0) < tol)
      spheres++;
  }
  CHECK(cones == 1);
  CHECK(spheres == 1);
}
