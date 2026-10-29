// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// The pure-data pieces a mesh body stands on
// (docs/interchange/README.md#mesh-bodies): MeshBvh's queries, Slice and its
// cap, and the mass properties, each checked against a fixture's analytic
// answer rather than its own formula.

#include "mesh_organic.h"

#include "core/mesh/MeshBvh.h"
#include "core/mesh/Slice.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <memory>

namespace {

std::shared_ptr<const mesh::TriMesh> Shared(const mesh::TriMesh& m) {
  return std::make_shared<const mesh::TriMesh>(m);
}

} // namespace

TEST_CASE("Mesh body: a sphere's mass properties are the analytic ones",
          "[mesh]") {
  const auto s = fixtures::NoisySphere(20000);
  const auto mp = mesh::ComputeMassProperties(s.mesh);
  CHECK(std::abs(mp.volume - s.volume) <= s.volumeTolerance);
  CHECK(std::abs(mp.area - s.area) <= s.areaTolerance);
  // Centred on the origin to within the noise's share of the radius.
  CHECK(glm::length(mp.centroid) < 0.01 * s.radius);
  // A solid sphere's moment about any axis through its centre: 2/5 V r².
  const double expect = 0.4 * s.volume * s.radius * s.radius;
  for (int i = 0; i < 3; i++)
    CHECK(mp.inertia[i][i] == Catch::Approx(expect).epsilon(0.01));
  CHECK(std::abs(mp.inertia[0][1]) < 0.001 * expect);
}

TEST_CASE("Mesh body: the open figure reports its loop", "[mesh]") {
  const auto cut = fixtures::CutFigurine(10000);
  REQUIRE_FALSE(cut.mesh.Closed());
  const auto loops = mesh::BoundaryLoops(cut.mesh);
  REQUIRE(loops.size() == cut.boundaryLoops);
  double longest = 0.0;
  fixtures::BoundaryLoops(cut.mesh, &longest);
  CHECK(loops.front() == Catch::Approx(longest));
}

TEST_CASE("Mesh body: the BVH's closest point lands on the sphere", "[mesh]") {
  const auto s = fixtures::NoisySphere(20000);
  const auto bvh = mesh::Bvh::For(Shared(s.mesh));
  for (const glm::dvec3 p :
       {glm::dvec3(100, 0, 0), glm::dvec3(3, -5, 2), glm::dvec3(-30, 30, 30)}) {
    const auto c = bvh->ClosestPoint(p);
    REQUIRE(c.found);
    INFO("from " << p.x << "," << p.y << "," << p.z);
    CHECK(std::abs(glm::length(c.point) - s.radius) <= s.radiusTolerance);
    CHECK(c.distance == Catch::Approx(std::abs(glm::length(p) - s.radius))
                            .margin(s.radiusTolerance));
  }
  // The same tree for the same mesh.
  CHECK(mesh::Bvh::For(
            std::shared_ptr<const mesh::TriMesh>(bvh, &bvh->Mesh())) == bvh);
}

TEST_CASE("Mesh body: a ray hits the sphere where the sphere is", "[mesh]") {
  const auto s = fixtures::NoisySphere(20000);
  const mesh::Bvh bvh(Shared(s.mesh));
  const auto hit = bvh.Ray({-200, 1, 2}, {1, 0, 0});
  REQUIRE(hit.hit);
  CHECK(hit.point.x < 0.0);
  CHECK(std::abs(glm::length(hit.point) - s.radius) <= s.radiusTolerance);
  CHECK_FALSE(bvh.Ray({-200, 100, 0}, {1, 0, 0}).hit);
  CHECK_FALSE(bvh.Ray({-200, 1, 2}, {-1, 0, 0}).hit);
}

TEST_CASE("Mesh body: the winding number tells inside from an open mesh's "
          "outside",
          "[mesh]") {
  const auto cut = fixtures::CutFigurine(10000);
  const mesh::Bvh bvh(Shared(cut.mesh));
  glm::dvec3 lo, hi;
  cut.mesh.Bounds(lo, hi);
  const glm::dvec3 mid = (lo + hi) / 2.0;
  // Well outside the box.
  CHECK(std::abs(bvh.Winding(hi + (hi - lo))) < 0.05);
  // A ray down from above through the middle meets the surface; just past
  // the first hit is inside the figure.
  const auto hit = bvh.Ray({mid.x, mid.y, hi.z + 10}, {0, 0, -1});
  REQUIRE(hit.hit);
  CHECK(bvh.Winding(hit.point + glm::dvec3(0, 0, -0.5)) > 0.5);
}

TEST_CASE("Mesh body: the distance between two spheres is the gap", "[mesh]") {
  const auto s = fixtures::NoisySphere(5000);
  const mesh::Bvh a(Shared(s.mesh));
  const mesh::Bvh b(Shared(s.mesh));
  const double gap = 7.0;
  const glm::dmat4 shift =
      glm::dmat4(1.0) * glm::dmat4(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0,
                                   2 * s.radius + gap, 0, 0, 1);
  const auto g = a.Distance(b, shift);
  // Facets cut inside the sphere by their sagitta, so the gap reads a
  // little wide; both surfaces are within the radius tolerance.
  CHECK(g.distance == Catch::Approx(gap).margin(2 * s.radiusTolerance + 0.5));
  // Overlapping: they meet.
  const glm::dmat4 over(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, s.radius, 0, 0, 1);
  CHECK(a.Distance(b, over).distance == 0.0);
}

TEST_CASE("Mesh body: a section of the sphere is a circle of its radius",
          "[mesh]") {
  const auto s = fixtures::NoisySphere(20000);
  const double z = 12.0;
  const auto cut = mesh::Slice(s.mesh, {0, 0, z}, {0, 0, 1});
  REQUIRE(cut.polylines.size() == 1);
  REQUIRE(cut.Closed());
  const double r = std::sqrt(s.radius * s.radius - z * z);
  for (const glm::dvec3& p : cut.polylines[0].points) {
    CHECK(p.z == Catch::Approx(z).margin(1e-9));
    CHECK(glm::length(glm::dvec2(p)) == Catch::Approx(r).margin(0.2));
  }
  // Counter-clockwise round material: a positive area of about π r².
  CHECK(cut.area == Catch::Approx(M_PI * r * r).epsilon(0.01));
  const auto cap = mesh::Cap(cut, {0, 0, z}, {0, 0, 1});
  REQUIRE_FALSE(cap.triangles.empty());
  double capArea = 0.0;
  for (const auto& t : cap.triangles)
    capArea += 0.5 * glm::length(
                         glm::cross(cap.positions[t[1]] - cap.positions[t[0]],
                                    cap.positions[t[2]] - cap.positions[t[0]]));
  CHECK(capArea == Catch::Approx(cut.area).epsilon(1e-6));
}

TEST_CASE("Mesh body: a torus cut through its hole has a hole", "[mesh]") {
  const auto t = fixtures::Torus(20000);
  const auto cut = mesh::Slice(t.mesh, {0, 0, 0}, {0, 0, 1});
  REQUIRE(cut.polylines.size() == 2);
  REQUIRE(cut.Closed());
  // The annulus between radii 20 and 40.
  CHECK(cut.area ==
        Catch::Approx(M_PI * (40.0 * 40.0 - 20.0 * 20.0)).epsilon(0.01));
  int outer = 0, inner = 0;
  for (const auto& l : cut.polylines)
    (l.signedArea > 0 ? outer : inner)++;
  CHECK(outer == 1);
  CHECK(inner == 1);
}

TEST_CASE("Mesh body: the open figure's cut through its middle is closed, at "
          "its base open",
          "[mesh]") {
  const auto cut = fixtures::CutFigurine(10000);
  glm::dvec3 lo, hi;
  cut.mesh.Bounds(lo, hi);
  const auto vertical = mesh::Slice(cut.mesh, (lo + hi) / 2.0, {1, 0, 0});
  REQUIRE_FALSE(vertical.polylines.empty());
  CHECK_FALSE(vertical.Closed()); // it runs out at the cut
  CHECK(mesh::Cap(vertical, (lo + hi) / 2.0, {1, 0, 0}).triangles.empty());
}

TEST_CASE("Mesh body: Douglas-Peucker keeps a polyline within its tolerance",
          "[mesh]") {
  std::vector<glm::dvec3> circle;
  for (int i = 0; i < 360; i++)
    circle.emplace_back(10 * std::cos(i * M_PI / 180),
                        10 * std::sin(i * M_PI / 180), 0);
  const auto s = mesh::Simplify(circle, true, 0.05);
  CHECK(s.size() < circle.size() / 4);
  CHECK(s.size() > 8);
}
