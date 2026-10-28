// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// mesh::SegmentAll on generated CAD parts, where every surface is known
// (docs/interchange/README.md#recognising-a-faceted-mesh).

#include "mesh_fixtures.h"

#include "core/mesh/Segment.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

namespace {

constexpr double kPi = 3.14159265358979323846;

double AxisAngle(const glm::dvec3& a, const glm::dvec3& b) {
  return std::acos(
      std::min(1.0, std::abs(glm::dot(glm::normalize(a), glm::normalize(b)))));
}

double LineDistance(const glm::dvec3& p, const glm::dvec3& a,
                    const glm::dvec3& q) {
  const glm::dvec3 w = q - p;
  const glm::dvec3 n = glm::normalize(a);
  return glm::length(w - glm::dot(w, n) * n);
}

// The region of `kind` matching `want`: same radius, axis and line.
const mesh::Region* Match(const mesh::Segmentation& s, mesh::Kind kind,
                          const fixtures::Round& want, double tol) {
  for (const mesh::Region& r : s.regions) {
    if (r.fit.kind != kind)
      continue;
    if (AxisAngle(r.fit.axis, want.axis) > 1e-5)
      continue;
    if (kind == mesh::Kind::Cone) {
      if (glm::length(r.fit.origin - want.point) > tol ||
          std::abs(r.fit.halfAngle - want.halfAngle) > 1e-5)
        continue;
    } else if (kind == mesh::Kind::Sphere) {
      if (glm::length(r.fit.origin - want.point) > tol ||
          std::abs(r.fit.radius - want.radius) > tol)
        continue;
    } else if (LineDistance(r.fit.origin, r.fit.axis, want.point) > tol ||
               std::abs(r.fit.radius - want.radius) > tol) {
      continue;
    }
    return &r;
  }
  return nullptr;
}

void CheckRounds(const mesh::Segmentation& s, const fixtures::Part& part,
                 double tol) {
  for (const auto& c : part.cylinders) {
    INFO("cylinder r=" << c.radius << " at " << c.point.x << "," << c.point.y
                       << "," << c.point.z);
    CHECK(Match(s, mesh::Kind::Cylinder, c, tol) != nullptr);
  }
  for (const auto& c : part.cones) {
    INFO("cone at " << c.point.x << "," << c.point.y << "," << c.point.z);
    CHECK(Match(s, mesh::Kind::Cone, c, tol) != nullptr);
  }
  for (const auto& c : part.spheres) {
    INFO("sphere r=" << c.radius);
    CHECK(Match(s, mesh::Kind::Sphere, c, tol) != nullptr);
  }
}

} // namespace

TEST_CASE("Segment finds every hole of the plate at 24 and 128 sides",
          "[mesh_recognition]") {
  const int sides = GENERATE(24, 128);
  INFO(sides << " sides");
  const fixtures::Part plate = fixtures::Plate();
  // The chord that makes the smallest hole (r 2.5) exactly that many-sided;
  // the larger ones come out with up to twice as many.
  const double chord = 2.5 * (1.0 - std::cos(kPi / sides)) * 1.0001;
  const mesh::TriMesh m = fixtures::MeshOf(plate, chord, 2 * kPi / sides);
  const mesh::Segmentation s = mesh::SegmentAll(m, {});
  CHECK(s.Count(mesh::Kind::Cylinder) == 24);
  CHECK(s.Count(mesh::Kind::Plane) == 6);
  CHECK(s.regions.size() == 30);
  CHECK(s.polygonal.empty());
  CHECK(s.facetsLeft == 0);
  CheckRounds(s, plate, 1e-4);
  for (const mesh::Region& r : s.regions)
    if (r.fit.kind == mesh::Kind::Cylinder) {
      CHECK(r.sides >= sides);
      CHECK(r.sides <= sides * 2);
      CHECK(r.fit.max <= s.tolerance);
    }
  // Every triangle in exactly one region, regions in file order.
  for (size_t t = 0; t < m.triangles.size(); t++)
    REQUIRE(s.regionOf[t] < s.regions.size());
  for (size_t i = 1; i < s.regions.size(); i++)
    CHECK(s.regions[i - 1].firstTriangle < s.regions[i].firstTriangle);
}

TEST_CASE("Segment keeps a nut trap flat and lists it", "[mesh_recognition]") {
  const fixtures::Part bracket = fixtures::Bracket();
  const mesh::TriMesh m = fixtures::MeshOf(bracket, 0.01, 0.2);
  const mesh::Segmentation s = mesh::SegmentAll(m, {});
  CheckRounds(s, bracket, 1e-4);
  CHECK(s.Count(mesh::Kind::Cylinder) == bracket.cylinders.size());
  CHECK(s.Count(mesh::Kind::Cone) == bracket.cones.size());
  REQUIRE(s.polygonal.size() == 3);
  for (const fixtures::Flat& want : bracket.polygons) {
    bool found = false;
    for (const mesh::Polygon& p : s.polygonal)
      if (p.sides == want.sides &&
          std::abs(p.centre.x - want.centreXY.x) < 1e-4 &&
          std::abs(p.centre.y - want.centreXY.y) < 1e-4 &&
          std::abs(p.circumradius - want.circumradius) < 1e-3)
        found = true;
    INFO(want.sides << " sides");
    CHECK(found);
  }
  CHECK(s.facetsLeft == 0);

  SECTION("min_sides 8 takes the octagon and the decagon as round") {
    mesh::SegmentOptions o;
    o.minSides = 8;
    const mesh::Segmentation s8 = mesh::SegmentAll(m, o);
    CHECK(s8.Count(mesh::Kind::Cylinder) == bracket.cylinders.size() + 2);
    REQUIRE(s8.polygonal.size() == 1);
    CHECK(s8.polygonal[0].sides == 6);
  }
}

TEST_CASE("Segment keeps a fillet off the planes it is tangent to",
          "[mesh_recognition]") {
  const fixtures::Part angle = fixtures::FilletedAngle();
  const mesh::TriMesh m = fixtures::MeshOf(angle, 0.01, 0.2);
  const mesh::Segmentation s = mesh::SegmentAll(m, {});
  CheckRounds(s, angle, 1e-4);
  CHECK(s.Count(mesh::Kind::Cylinder) == angle.cylinders.size());
  // The inside fillet is a quarter turn of radius 6 along 40: its facets
  // are chords, a little short of the arc.
  for (const mesh::Region& r : s.regions)
    if (r.fit.kind == mesh::Kind::Cylinder &&
        std::abs(r.fit.radius - 6.0) < 1e-4) {
      CHECK_THAT(r.area, Catch::Matchers::WithinRel(6.0 * kPi / 2 * 40, 5e-3));
    }
  CHECK(s.facetsLeft == 0);
}

TEST_CASE("Segment finds an oblique hole and a dome", "[mesh_recognition]") {
  const fixtures::Part part = fixtures::ObliqueAndDome();
  const mesh::TriMesh m = fixtures::MeshOf(part, 0.01, 0.2);
  const mesh::Segmentation s = mesh::SegmentAll(m, {});
  CheckRounds(s, part, 1e-4);
  CHECK(s.Count(mesh::Kind::Cylinder) == 1);
  CHECK(s.Count(mesh::Kind::Sphere) == 1);
}
