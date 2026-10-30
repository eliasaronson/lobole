// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// Fitting a primitive to a scan from a picked point
// (docs/modeling/README.md#fitting-a-primitive-to-a-mesh): the robust fit
// against outliers, the region grown from a seed, the tolerance the scan's
// noise sets, and auto's choice -- each checked against the surface the
// fixture was generated on, never against the fit's own numbers.

#include "mesh_organic.h"

#include "core/mesh/Fit.h"
#include "core/mesh/MeshBvh.h"
#include "core/mesh/Segment.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;

namespace {

constexpr double kPi = 3.14159265358979323846;

uint32_t SeedAt(const mesh::TriMesh& m, const glm::dvec3& p) {
  auto shared = std::make_shared<mesh::TriMesh>(m);
  const mesh::Bvh::Closest c = mesh::Bvh::For(shared)->ClosestPoint(p);
  REQUIRE(c.found);
  return c.triangle;
}

// The angle between two lines' directions, degrees, whichever sense.
double LineAngleDeg(const glm::dvec3& a, const glm::dvec3& b) {
  const double c = std::abs(glm::dot(glm::normalize(a), glm::normalize(b)));
  return std::acos(std::min(1.0, c)) * 180.0 / kPi;
}

// The distance of `p` from the line through `o` along unit `d`.
double FromLine(const glm::dvec3& p, const glm::dvec3& o, const glm::dvec3& d) {
  const glm::dvec3 w = p - o;
  return glm::length(w - glm::dot(w, d) * d);
}

const fixtures::Post& Post() {
  static const fixtures::Post post = fixtures::ScannedPost(60000);
  return post;
}

} // namespace

TEST_CASE("The scan's tolerance is set by its noise", "[mesh][fit]") {
  const fixtures::Post& post = Post();
  const double tol = mesh::ScanTolerance(post.organic.mesh);
  std::printf("post (noise %.3g mm): tolerance %.4f mm\n", post.noise, tol);
  // Every vertex within it of the true surface; and not so loose it takes
  // in the lump (1.5 mm) or the next surface.
  CHECK(tol >= post.noise);
  CHECK(tol < 0.25);
  const fixtures::Organic sphere = fixtures::NoisySphere(30000);
  const double st = mesh::ScanTolerance(sphere.mesh);
  std::printf("sphere (noise 0.05 mm): tolerance %.4f mm\n", st);
  CHECK(st >= 0.05);
  CHECK(st < 0.25);
}

TEST_CASE("A cylinder grown from a point on a scan is the generated one",
          "[mesh][fit]") {
  const fixtures::Post& post = Post();
  const mesh::TriMesh& m = post.organic.mesh;
  const double tol = mesh::ScanTolerance(m);
  std::string why;
  const mesh::Region r = mesh::GrowFrom(m, SeedAt(m, post.onCylinder),
                                        mesh::Kind::Cylinder, tol, &why);
  INFO(why);
  REQUIRE(r.fit.ok);
  const double angle = LineAngleDeg(r.fit.axis, post.axis);
  const double offAxis = FromLine(post.base, r.fit.origin, r.fit.axis);
  std::printf("cylinder: radius %.4f (12), axis off %.4f deg, axis line "
              "%.4f mm off, rms %.4f max %.4f, %zu triangles\n",
              r.fit.radius, angle, offAxis, r.fit.rms, r.fit.max,
              r.triangles.size());
  CHECK_THAT(r.fit.radius, WithinAbs(post.radius, 0.02));
  CHECK(angle < 0.1);
  CHECK(offAxis < 0.05);
  CHECK(r.fit.max <= tol * (1.0 + 1e-9));
  // The region is the cylinder's, round the lump: it covers most of the
  // side (2 pi R H) and none of the lump's crest.
  CHECK(r.area > 0.8 * 2.0 * kPi * post.radius * post.height);
  CHECK(r.area < 1.02 * 2.0 * kPi * post.radius * post.height);
  for (uint32_t t : r.triangles)
    for (int c = 0; c < 3; c++)
      REQUIRE(glm::length(m.Corner(t, c) - post.onLump) > post.lumpSigma);
}

TEST_CASE("A cone, a plane and a sphere grown from a point are the "
          "generated ones",
          "[mesh][fit]") {
  const fixtures::Post& post = Post();
  const mesh::TriMesh& m = post.organic.mesh;
  const double tol = mesh::ScanTolerance(m);
  std::string why;
  SECTION("the cone") {
    const mesh::Region r =
        mesh::GrowFrom(m, SeedAt(m, post.onCone), mesh::Kind::Cone, tol, &why);
    INFO(why);
    REQUIRE(r.fit.ok);
    const double halfDeg = r.fit.halfAngle * 180.0 / kPi;
    const double apexOff = glm::length(r.fit.origin - post.Apex());
    std::printf("cone: half angle %.4f deg (%.4f), apex %.4f mm off, axis "
                "off %.4f deg\n",
                halfDeg, post.HalfAngle() * 180.0 / kPi, apexOff,
                LineAngleDeg(r.fit.axis, post.axis));
    CHECK_THAT(halfDeg, WithinAbs(post.HalfAngle() * 180.0 / kPi, 0.2));
    CHECK(apexOff < 0.3);
    CHECK(LineAngleDeg(r.fit.axis, post.axis) < 0.2);
    // From the apex into the cone: down the post.
    CHECK(glm::dot(r.fit.axis, post.axis) < 0.0);
  }
  SECTION("the top") {
    const mesh::Region r =
        mesh::GrowFrom(m, SeedAt(m, post.onTop), mesh::Kind::Plane, tol, &why);
    REQUIRE(r.fit.ok);
    // Facing out of the part, up the post.
    CHECK(glm::dot(r.fit.axis, post.axis) > 0.0);
    CHECK(LineAngleDeg(r.fit.axis, post.axis) < 0.2);
    const double height = glm::dot(r.fit.origin - post.base, post.axis);
    CHECK_THAT(height, WithinAbs(post.height + post.coneHeight, 0.02));
    double far = 0.0, low = 1e9;
    for (uint32_t t : r.triangles)
      for (int c = 0; c < 3; c++) {
        far = std::max(far, FromLine(m.Corner(t, c), post.base, post.axis));
        low = std::min(low, glm::dot(m.Corner(t, c) - post.base, post.axis));
      }
    std::printf("top: %zu triangles, out to r %.4f, down to z %.4f\n",
                r.triangles.size(), far, low);
    // Exactly the top's own triangles: every one with all three corners on
    // it (within the noise), and nothing of the cone below.
    size_t top = 0;
    for (uint32_t t = 0; t < m.triangles.size(); t++) {
      bool on = true;
      for (int c = 0; c < 3; c++)
        on = on && glm::dot(m.Corner(t, c) - post.base, post.axis) >
                       post.height + post.coneHeight - 2.0 * post.noise;
      top += on ? 1 : 0;
    }
    CHECK(r.triangles.size() == top);
  }
  SECTION("the bottom, facing down") {
    const mesh::Region r = mesh::GrowFrom(m, SeedAt(m, post.onBottom),
                                          mesh::Kind::Plane, tol, &why);
    REQUIRE(r.fit.ok);
    CHECK(glm::dot(r.fit.axis, post.axis) < 0.0);
    CHECK(LineAngleDeg(r.fit.axis, post.axis) < 0.1);
  }
  SECTION("a noisy sphere") {
    const fixtures::Organic s = fixtures::NoisySphere(30000);
    const double st = mesh::ScanTolerance(s.mesh);
    const mesh::Region r = mesh::GrowFrom(
        s.mesh, SeedAt(s.mesh, {0.0, 0.0, 40.0}), mesh::Kind::Sphere, st, &why);
    REQUIRE(r.fit.ok);
    std::printf("sphere: radius %.4f (40), centre %.4f mm off\n", r.fit.radius,
                glm::length(r.fit.origin));
    CHECK_THAT(r.fit.radius, WithinAbs(s.radius, 0.01));
    const mesh::FitResult all = mesh::FitSphere(s.mesh.positions);
    std::printf("sphere: least squares over every vertex, centre %.4f mm "
                "off\n",
                glm::length(all.origin));
    CHECK(glm::length(r.fit.origin) < 0.01);
    CHECK(r.triangles.size() == s.mesh.TriangleCount()); // all of it
  }
}

TEST_CASE("A curved kind on a flat is refused, naming the plane",
          "[mesh][fit]") {
  const fixtures::Post& post = Post();
  const mesh::TriMesh& m = post.organic.mesh;
  std::string why;
  const mesh::Region r =
      mesh::GrowFrom(m, SeedAt(m, post.onBottom), mesh::Kind::Cylinder,
                     mesh::ScanTolerance(m), &why);
  CHECK_FALSE(r.fit.ok);
  CHECK_THAT(why, ContainsSubstring("flat"));
  CHECK_THAT(why, ContainsSubstring("fit a plane"));
}

TEST_CASE("A robust fit is not dragged by a lump that least squares is",
          "[mesh][fit]") {
  // Every vertex of the cylinder's side, the lump's among them.
  const fixtures::Post& post = Post();
  const mesh::TriMesh& m = post.organic.mesh;
  std::vector<glm::dvec3> pts, nrm(m.positions.size(), glm::dvec3(0.0));
  for (uint32_t t = 0; t < m.triangles.size(); t++)
    for (uint32_t v : m.triangles[t])
      nrm[v] += m.AreaNormal(t);
  std::vector<glm::dvec3> normals;
  for (size_t v = 0; v < m.positions.size(); v++) {
    const glm::dvec3 w = m.positions[v] - post.base;
    const double z = glm::dot(w, post.axis);
    if (z > 1.0 && z < post.height - 1.0) {
      pts.push_back(m.positions[v]);
      normals.push_back(glm::normalize(nrm[v]));
    }
  }
  const double tol = mesh::ScanTolerance(m);
  const mesh::FitResult ls = mesh::FitCylinder(pts, post.axis);
  const mesh::FitResult robust =
      mesh::FitRobust(mesh::Kind::Cylinder, pts, normals, tol);
  REQUIRE(ls.ok);
  REQUIRE(robust.ok);
  const double lsOff = FromLine(post.base, ls.origin, ls.axis);
  const double robustOff = FromLine(post.base, robust.origin, robust.axis);
  std::printf("lump: least squares radius %.4f, axis %.4f mm off; robust "
              "%.4f, %.4f mm off, %zu of %zu points kept\n",
              ls.radius, lsOff, robust.radius, robustOff, robust.inliers,
              pts.size());
  CHECK_THAT(robust.radius, WithinAbs(post.radius, 0.02));
  CHECK(robustOff < 0.05);
  // The lump drags least squares several times further.
  CHECK(lsOff > 4.0 * robustOff);
  CHECK(robust.inliers < pts.size());
}

TEST_CASE("A robust fit is the same every time it runs", "[mesh][fit]") {
  const fixtures::Post& post = Post();
  const mesh::TriMesh& m = post.organic.mesh;
  const double tol = mesh::ScanTolerance(m);
  const uint32_t seed = SeedAt(m, post.onCone);
  const mesh::Region a = mesh::GrowFrom(m, seed, mesh::Kind::Cone, tol);
  const mesh::Region b = mesh::GrowFrom(m, seed, mesh::Kind::Cone, tol);
  REQUIRE(a.fit.ok);
  CHECK(a.triangles == b.triangles);
  CHECK(a.fit.origin == b.fit.origin); // to the bit
  CHECK(a.fit.axis == b.fit.axis);
  CHECK(a.fit.halfAngle == b.fit.halfAngle);
}

TEST_CASE("A plane grown on a box is the face picked, not the next one",
          "[mesh][fit]") {
  // A 40 x 20 x 10 cuboid, two triangles a face: every face's plane passes
  // through four of its eight corners, so only passing through the picked
  // triangle tells them apart.
  mesh::TriMesh m;
  for (int i = 0; i < 8; i++)
    m.positions.push_back(
        {(i & 1) ? 20.0 : -20.0, (i & 2) ? 10.0 : -10.0, (i & 4) ? 5.0 : -5.0});
  const int quads[6][4] = {{0, 2, 6, 4}, {1, 3, 7, 5}, {0, 1, 5, 4},
                           {2, 3, 7, 6}, {0, 1, 3, 2}, {4, 5, 7, 6}};
  for (const auto& q : quads)
    for (const auto& tri : {std::array<int, 3>{q[0], q[1], q[2]},
                            std::array<int, 3>{q[0], q[2], q[3]}}) {
      std::array<uint32_t, 3> t{uint32_t(tri[0]), uint32_t(tri[1]),
                                uint32_t(tri[2])};
      const glm::dvec3 a = m.positions[t[0]], b = m.positions[t[1]],
                       c = m.positions[t[2]];
      if (glm::dot(glm::cross(b - a, c - a), a + b + c) < 0.0)
        std::swap(t[1], t[2]);
      m.triangles.push_back(t);
    }
  mesh::BuildAdjacency(m);
  // Its creases are features, not noise: the tolerance is its precision's.
  CHECK(mesh::ScanTolerance(m) <= 4.0 * m.quantum + 1e-12);
  for (uint32_t seed = 0; seed < m.triangles.size(); seed++) {
    std::string why;
    const mesh::Region r =
        mesh::GrowFrom(m, seed, mesh::Kind::Plane, 0.09, &why);
    INFO("seed " << seed << ": " << why);
    REQUIRE(r.fit.ok);
    const glm::dvec3 n = glm::normalize(m.AreaNormal(seed));
    CHECK(glm::dot(r.fit.axis, n) > 1.0 - 1e-9);
    CHECK(r.triangles.size() == 2);
  }
}

// What a fit from a point costs on a scan, by size: hidden, run by name
// (docs/modeling/README.md#fitting-a-primitive-to-a-mesh).
TEST_CASE("What a fit from a point costs", "[.][fit-cost]") {
  using Clock = std::chrono::steady_clock;
  const auto ms = [](Clock::time_point a) {
    return std::chrono::duration<double, std::milli>(Clock::now() - a).count();
  };
  for (const size_t n : {size_t(10000), size_t(300000), size_t(2000000)}) {
    const fixtures::Organic s = fixtures::NoisySphere(n);
    auto t = Clock::now();
    const double tol = mesh::ScanTolerance(s.mesh);
    const double tolMs = ms(t);
    const uint32_t seed = SeedAt(s.mesh, {0.0, 0.0, 40.0});
    t = Clock::now();
    const mesh::Region r =
        mesh::GrowFrom(s.mesh, seed, mesh::Kind::Sphere, tol);
    const double growMs = ms(t);
    REQUIRE(r.fit.ok);
    std::printf("sphere %zu: tolerance %.0f ms, grow+fit %.0f ms (%zu "
                "triangles), radius %.4f\n",
                s.mesh.TriangleCount(), tolMs, growMs, r.triangles.size(),
                r.fit.radius);
  }
}
