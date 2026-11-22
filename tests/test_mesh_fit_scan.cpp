// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// Fitting a primitive to a scan from a picked point
// (docs/modeling/README.md#fitting-a-primitive-to-a-mesh): the robust fit
// against outliers, the region grown from a seed, the tolerance the scan's
// noise sets, and auto's choice -- each checked against the surface the
// fixture was generated on, never against the fit's own numbers.

#include "SameCore.h"
#include "mesh_organic.h"

#include "core/MeshFit.h"
#include "core/mesh/Fit.h"
#include "core/mesh/MeshBvh.h"
#include "core/mesh/Segment.h"
#include "core/mesh/Split.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <limits>
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

// The growth before the region stopped at the edge: every corner within the
// tolerance and facing the seed's side, refitted each time the region
// doubles, grown once more against the last fit and fitted again -- by
// distance alone. What the edge and the cost are measured against, kept
// here as it was so neither can drift.
struct LegacySamples {
  std::vector<glm::dvec3> points, centres, normals;
};

void LegacyGather(const mesh::TriMesh& m, const std::vector<uint32_t>& tris,
                  std::vector<uint8_t>& stamp, LegacySamples& out,
                  size_t most) {
  std::vector<uint32_t> verts;
  for (uint32_t t : tris)
    for (uint32_t v : m.triangles[t])
      if (!stamp[v]) {
        stamp[v] = 1;
        verts.push_back(v);
      }
  for (uint32_t v : verts)
    stamp[v] = 0;
  std::sort(verts.begin(), verts.end());
  out.points.clear();
  out.centres.clear();
  out.normals.clear();
  const size_t n = verts.size(), take = std::min(n, most);
  for (size_t i = 0; i < take; i++)
    out.points.push_back(m.positions[verts[take == n ? i : i * n / take]]);
  std::vector<uint32_t> sorted = tris;
  std::sort(sorted.begin(), sorted.end());
  const size_t nt = sorted.size(), takeT = std::min(nt, most);
  for (size_t i = 0; i < takeT; i++) {
    const uint32_t t = sorted[takeT == nt ? i : i * nt / takeT];
    const glm::dvec3 a = m.AreaNormal(t);
    const double l = glm::length(a);
    if (!(l > 0.0))
      continue;
    out.centres.push_back((m.Corner(t, 0) + m.Corner(t, 1) + m.Corner(t, 2)) /
                          3.0);
    out.normals.push_back(a / l);
  }
}

mesh::Region LegacyGrowFrom(const mesh::TriMesh& m, uint32_t seed,
                            mesh::Kind kind, double tol) {
  constexpr size_t kFitPoints = 65536;
  mesh::Region r;
  r.fit.kind = kind;
  std::vector<uint8_t> stamp(m.positions.size(), 0);
  std::vector<uint8_t> in(m.triangles.size(), 0);
  LegacySamples smp;
  const auto robust = [&](const glm::dvec3* through) {
    return mesh::FitRobust(kind, smp.points, smp.centres, smp.normals, tol,
                           through);
  };
  std::vector<uint32_t> disk{seed}, frontier{seed};
  in[seed] = 1;
  size_t lastFit = 0;
  for (int ring = 0; !frontier.empty(); ring++) {
    std::vector<uint32_t> next;
    for (uint32_t t : frontier)
      for (int e = 0; e < 3; e++) {
        const uint32_t u = m.neighbour[t][e];
        if (u != mesh::kNone && !in[u]) {
          in[u] = 1;
          next.push_back(u);
        }
      }
    disk.insert(disk.end(), next.begin(), next.end());
    frontier = std::move(next);
    if (ring < 1 || disk.size() < 12 || disk.size() < lastFit * 3 / 2)
      continue;
    lastFit = disk.size();
    if (kind == mesh::Kind::Plane)
      break;
    LegacyGather(m, disk, stamp, smp, kFitPoints);
    const mesh::FitResult flat = mesh::FitPlane(smp.points);
    if (flat.ok && flat.max > 3.0 * tol)
      break;
  }
  for (uint32_t t : disk)
    in[t] = 0;
  LegacyGather(m, disk, stamp, smp, kFitPoints);
  const glm::dvec3 seedCentre =
      (m.Corner(seed, 0) + m.Corner(seed, 1) + m.Corner(seed, 2)) / 3.0;
  mesh::FitResult fit = robust(&seedCentre);
  if (!fit.ok)
    return r;
  const double side =
      glm::dot(m.AreaNormal(seed), fit.Normal(seedCentre)) < 0.0 ? -1.0 : 1.0;
  const auto takes = [&](const mesh::FitResult& f, uint32_t t) {
    for (uint32_t v : m.triangles[t])
      if (!(std::abs(f.Distance(m.positions[v])) <= tol))
        return false;
    const glm::dvec3 c =
        (m.Corner(t, 0) + m.Corner(t, 1) + m.Corner(t, 2)) / 3.0;
    return side * glm::dot(m.AreaNormal(t), f.Normal(c)) > 0.0;
  };
  const auto grow = [&](mesh::FitResult& f, bool refit) {
    std::vector<uint32_t> region{seed};
    in[seed] = 1;
    size_t nextFit = std::max<size_t>(2 * disk.size(), 16);
    for (size_t i = 0; i < region.size(); i++) {
      const uint32_t t = region[i];
      for (int e = 0; e < 3; e++) {
        const uint32_t u = m.neighbour[t][e];
        if (u == mesh::kNone || in[u] || !takes(f, u))
          continue;
        in[u] = 1;
        region.push_back(u);
      }
      if (refit && region.size() >= nextFit) {
        nextFit *= 2;
        LegacyGather(m, region, stamp, smp, kFitPoints);
        const mesh::FitResult g = robust(&seedCentre);
        if (g.ok)
          f = g;
      }
    }
    for (uint32_t t : region)
      in[t] = 0;
    return region;
  };
  (void)grow(fit, /*refit=*/true);
  std::vector<uint32_t> region = grow(fit, /*refit=*/false);
  LegacyGather(m, region, stamp, smp, kFitPoints);
  if (const mesh::FitResult g = robust(&seedCentre); g.ok)
    fit = g;
  LegacyGather(m, region, stamp, smp, m.positions.size());
  double sum = 0.0, worst = 0.0;
  for (const glm::dvec3& p : smp.points) {
    const double d = std::abs(fit.Distance(p));
    sum += d * d;
    worst = std::max(worst, d);
  }
  fit.rms =
      smp.points.empty() ? 0.0 : std::sqrt(sum / double(smp.points.size()));
  fit.max = worst;
  if (kind == mesh::Kind::Plane && side < 0.0)
    fit.axis = -fit.axis;
  std::sort(region.begin(), region.end());
  r.fit = fit;
  r.triangles = std::move(region);
  for (uint32_t t : r.triangles)
    r.area += m.Area(t);
  return r;
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
  // Every corner was within the tolerance of the fit it grew under; the
  // clip's least squares moves the surface by a fraction of the noise.
  CHECK(r.fit.max <= tol * 1.01);
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
    // A few of the smallest round the pole lean past the scan's own
    // normal band and stay out: holes, not a neighbour.
    CHECK(r.triangles.size() <= top);
    CHECK(r.triangles.size() >= top * 99 / 100);
    CHECK(low > post.height + post.coneHeight - 2.0 * post.noise);
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

TEST_CASE("A plane fitted near a rounded edge stops at the edge",
          "[mesh][fit]") {
  // A block with 0.5 mm rounds on every edge, scanned with +-0.05 mm of
  // noise, tilted off the axes; its +Z face clicked 2 mm from an edge. A
  // round's first few degrees lie within the tolerance of the face and
  // face its way: growth by distance takes them, and they all stand to one
  // side of it.
  const fixtures::Block b = fixtures::ScannedBlock();
  const mesh::TriMesh& m = b.organic.mesh;
  const double tol = mesh::ScanTolerance(m);
  const uint32_t seed = SeedAt(m, b.nearEdge);
  const glm::dvec3 n = b.FaceNormal(2, 1);
  const glm::dvec3 centre = b.ToWorld({0.0, 0.0, b.half.z});
  const auto err = [&](const mesh::Region& r, double& deg, double& mm) {
    deg = LineAngleDeg(r.fit.axis, n);
    // How far the fitted plane is off the face, at the worst of the
    // face's middle and the click.
    mm = std::max(std::abs(r.fit.Distance(centre)),
                  std::abs(r.fit.Distance(
                      b.nearEdge - glm::dot(b.nearEdge - centre, n) * n)));
  };
  std::string why;
  const mesh::Region now =
      mesh::GrowFrom(m, seed, mesh::Kind::Plane, tol, &why);
  INFO(why);
  REQUIRE(now.fit.ok);
  const mesh::Region old = LegacyGrowFrom(m, seed, mesh::Kind::Plane, tol);
  REQUIRE(old.fit.ok);
  double nowDeg = 0.0, nowMm = 0.0, oldDeg = 0.0, oldMm = 0.0;
  err(now, nowDeg, nowMm);
  err(old, oldDeg, oldMm);
  std::printf("block edge (tolerance %.4f): now %.5f deg, %.5f mm off, %zu "
              "triangles, %zu used; by distance alone %.5f deg, %.5f mm "
              "off, %zu triangles\n",
              tol, nowDeg, nowMm, now.triangles.size(), now.used, oldDeg, oldMm,
              old.triangles.size());
  CHECK(glm::dot(now.fit.axis, n) > 0.0);
  CHECK(nowDeg < 0.01);
  CHECK(nowMm < 0.01);
  // The same mesh, the same click: growth by distance leans into the
  // rounds.
  CHECK(oldMm > 2.0 * nowMm);
  // The used points span the face's flat (30 x 24 less the rounds) and
  // no more than the face: what the band admits of a round's first
  // degrees is inside it.
  std::printf("block edge: extent %.3f x %.3f (flat %.1f x %.1f)\n",
              now.extent.x, now.extent.y, 2.0 * (b.half.x - b.round),
              2.0 * (b.half.y - b.round));
  CHECK(now.extent.x >= 2.0 * (b.half.x - b.round) - 0.1);
  CHECK(now.extent.x <= 2.0 * b.half.x);
  CHECK(now.extent.y >= 2.0 * (b.half.y - b.round) - 0.1);
  CHECK(now.extent.y <= 2.0 * b.half.y);
}

TEST_CASE("A cylinder fit is not dragged by 30% of a neighbouring face",
          "[mesh][fit]") {
  // The post's side, and 30% of the base beside it: the robust fit and the
  // clip keep the side's.
  const fixtures::Post& post = Post();
  const mesh::TriMesh& m = post.organic.mesh;
  std::vector<uint32_t> side, base;
  for (uint32_t t = 0; t < m.triangles.size(); t++) {
    double lo = 1e9, hi = -1e9;
    for (int c = 0; c < 3; c++) {
      const double z = glm::dot(m.Corner(t, c) - post.base, post.axis);
      lo = std::min(lo, z);
      hi = std::max(hi, z);
    }
    if (lo > 0.5 && hi < post.height - 0.5)
      side.push_back(t);
    else if (hi < 2.0 * post.noise)
      base.push_back(t);
  }
  // 30% of the base, its outer part: the triangles beside the side.
  const auto out = [&](uint32_t t) {
    return FromLine((m.Corner(t, 0) + m.Corner(t, 1) + m.Corner(t, 2)) / 3.0,
                    post.base, post.axis);
  };
  std::sort(base.begin(), base.end(),
            [&](uint32_t a, uint32_t b) { return out(a) > out(b); });
  const size_t want = base.size() * 3 / 10;
  std::vector<uint32_t> region = side;
  region.insert(region.end(), base.begin(), base.begin() + long(want));
  std::string why;
  const mesh::Region r =
      mesh::FitTriangles(m, region, SeedAt(m, post.onCylinder),
                         mesh::Kind::Cylinder, mesh::ScanTolerance(m), &why);
  INFO(why);
  REQUIRE(r.fit.ok);
  const double off = FromLine(post.base, r.fit.origin, r.fit.axis);
  std::printf("cylinder with 30%% of the base: radius %.4f (12), axis %.4f "
              "deg and %.4f mm off, %zu of %zu triangles' points used\n",
              r.fit.radius, LineAngleDeg(r.fit.axis, post.axis), off, r.used,
              region.size());
  CHECK_THAT(r.fit.radius, WithinAbs(post.radius, post.noise));
  CHECK(off < post.noise);
  CHECK(LineAngleDeg(r.fit.axis, post.axis) < 0.1);
}

TEST_CASE("A cylinder from part of its arc, and the partial-arc warning",
          "[mesh][fit]") {
  SECTION("140 degrees") {
    const fixtures::Arc a = fixtures::ScannedArc(12.0, 140.0, 30.0, 0.4, 0.05);
    const mesh::TriMesh& m = a.organic.mesh;
    std::string why;
    const mesh::Region r =
        mesh::GrowFrom(m, SeedAt(m, a.onSurface), mesh::Kind::Cylinder,
                       mesh::ScanTolerance(m), &why);
    INFO(why);
    REQUIRE(r.fit.ok);
    const double off = FromLine(a.base, r.fit.origin, r.fit.axis);
    std::printf("140 deg arc: radius %.4f (12), axis %.4f deg %.4f mm off, "
                "arc %.2f deg, length %.3f (30)\n",
                r.fit.radius, LineAngleDeg(r.fit.axis, a.axis), off,
                r.arc * 180.0 / kPi, r.length);
    CHECK_THAT(r.fit.radius, WithinAbs(a.radius, a.noise));
    CHECK(off < a.noise);
    CHECK_THAT(r.arc * 180.0 / kPi, WithinAbs(140.0, 2.0));
    CHECK_THAT(r.length, WithinAbs(30.0, 0.5));
    meshfit::Result res;
    res.region = r;
    CHECK_FALSE(meshfit::ResultJson(res).contains("warnings"));
  }
  SECTION("a few degrees of a large bore") {
    const fixtures::Arc a =
        fixtures::ScannedArc(150.0, 12.0, 30.0, 0.4, 0.02, /*bore=*/true);
    const mesh::TriMesh& m = a.organic.mesh;
    std::string why;
    const mesh::Region r =
        mesh::GrowFrom(m, SeedAt(m, a.onSurface), mesh::Kind::Cylinder,
                       mesh::ScanTolerance(m), &why);
    INFO(why);
    REQUIRE(r.fit.ok);
    std::printf("12 deg of an R150 bore: radius %.3f, arc %.2f deg, diameter "
                "%.3f to %.3f\n",
                r.fit.radius, r.arc * 180.0 / kPi,
                2.0 * (r.fit.radius + r.residualLo),
                2.0 * (r.fit.radius + r.residualHi));
    CHECK_THAT(r.arc * 180.0 / kPi, WithinAbs(12.0, 0.5));
    CHECK_THAT(r.fit.radius, WithinAbs(a.radius, 0.02 * a.radius));
    meshfit::Result res;
    res.region = r;
    const nlohmann::json j = meshfit::ResultJson(res);
    REQUIRE(j.contains("warnings"));
    CHECK_THAT(j.at("warnings")[0].get<std::string>(),
               ContainsSubstring("arc"));
    // A bore: its triangles face the axis.
    const glm::dvec3 c = a.onSurface;
    CHECK(glm::dot(m.AreaNormal(SeedAt(m, c)), r.fit.Normal(c)) < 0.0);
  }
}

TEST_CASE("The used points and the figures they give", "[mesh][fit]") {
  const fixtures::Post& post = Post();
  const mesh::TriMesh& m = post.organic.mesh;
  const double tol = mesh::ScanTolerance(m);
  const uint32_t seed = SeedAt(m, post.onCylinder);
  // Fewer sigma keeps fewer points, and their sigma is smaller than the
  // whole region's rms: which is why the reply carries both.
  size_t lastUsed = std::numeric_limits<size_t>::max();
  for (const int k : {0, 3, 2, 1}) {
    mesh::GrowOptions o;
    o.clipSigma = k;
    const mesh::Region r =
        mesh::GrowFrom(m, seed, mesh::Kind::Cylinder, tol, nullptr, o);
    REQUIRE(r.fit.ok);
    std::printf("clip %d: radius %.4f, used %zu of %zu, sigma %.4f rms %.4f "
                "form %.4f\n",
                k, r.fit.radius, r.used, r.fit.inliers, r.sigma, r.fit.rms,
                r.residualHi - r.residualLo);
    CHECK(r.used <= lastUsed);
    CHECK(r.sigma <= r.fit.rms * (1.0 + 1e-9));
    CHECK_THAT(r.fit.radius, WithinAbs(post.radius, 0.02));
    if (k == 0)
      CHECK(r.used == r.fit.inliers);
    if (k == 1)
      CHECK(r.used < r.fit.inliers * 9 / 10);
    // The used points lie within k robust sigma of the fit (the noise is
    // uniform to +-0.05: a sigma of 1.4826 x 0.025), so the sizes a caliper
    // would read are the radius moved by at most that.
    if (k > 0) {
      CHECK(r.residualHi <= k * 1.4826 * 0.025 * 1.05);
      CHECK(r.residualLo >= -k * 1.4826 * 0.025 * 1.05);
    }
    lastUsed = r.used;
  }
}

// ---- two surfaces, and a plane on a crown
// (docs/modeling/README.md#two-surfaces) --------------------------------

namespace {

// A bore of R4.68 for its first 12.4 mm and R5.18 after, 25 long: a step a
// scan joins with the row of triangles across it.
constexpr double kStepAt = 12.4, kNarrow = 4.68, kWide = 5.18;

const fixtures::Bore& SteppedBore() {
  static const fixtures::Bore b = fixtures::ScannedBore(
      [](double h) { return h < kStepAt ? kNarrow : kWide; }, 25.0, 0.25, 0.05);
  return b;
}

// A 40 x 30 plate with a 0.3 mm step across it 18 mm in.
constexpr double kPlateStep = 0.3, kPlateStepAt = 18.0;

const fixtures::Sheet& SteppedPlate() {
  static const fixtures::Sheet s = fixtures::ScannedSheet(
      [](double x, double) { return x < kPlateStepAt ? 0.0 : kPlateStep; },
      40.0, 30.0, 0.25, 0.05);
  return s;
}

// The region a kind grows over with the split and the crown left out, and
// what the split makes of it.
mesh::TwoSurfaces SplitOf(const mesh::TriMesh& m, uint32_t seed,
                          mesh::Kind kind, double tol) {
  mesh::GrowOptions o;
  o.judge = false;
  std::string why;
  const mesh::Region r = mesh::GrowFrom(m, seed, kind, tol, &why, o);
  INFO(why);
  REQUIRE(r.fit.ok);
  return mesh::SplitTwo(m, r.triangles, r.fit, mesh::ScanTolerance(m));
}

void Print(const char* what, const mesh::TwoSurfaces& t) {
  std::printf("%s: examined %d two %d, shares %.2f/%.2f, apart %.4f vs "
              "scatter %.4f (x%.1f, needs %.0f), two rms %.4f vs smooth "
              "%.4f (x%.2f, needs %.1f)\n",
              what, t.examined, t.two, t.shareA, t.shareB, t.apart, t.scatter,
              t.scatter > 0.0 ? t.apart / t.scatter : 0.0, mesh::kSplitApart,
              t.twoRms, t.smoothRms,
              t.twoRms > 0.0 ? t.smoothRms / t.twoRms : 0.0,
              mesh::kSplitSmooth);
}

} // namespace

TEST_CASE("A stepped bore one fit would average is refused, naming both",
          "[mesh][fit]") {
  const fixtures::Bore& b = SteppedBore();
  const mesh::TriMesh& m = b.organic.mesh;
  const uint32_t seed = SeedAt(m, b.At(6.0, 30.0));
  SECTION("at the scan's own tolerance the click is one of them") {
    std::string why;
    const mesh::Region r = mesh::GrowFrom(m, seed, mesh::Kind::Cylinder,
                                          mesh::ScanTolerance(m), &why);
    INFO(why);
    REQUIRE(r.fit.ok);
    CHECK_THAT(r.fit.radius, WithinAbs(kNarrow, 0.01));
  }
  // A tolerance wide enough to take both: the fit across them is neither.
  const double tol = 0.6;
  SECTION("refused") {
    std::string why;
    mesh::GrowOptions o;
    o.noiseMm = mesh::ScanTolerance(m);
    const mesh::Region r =
        mesh::GrowFrom(m, seed, mesh::Kind::Cylinder, tol, &why, o);
    std::printf("stepped bore: %s\n", why.c_str());
    CHECK_FALSE(r.fit.ok);
    CHECK(r.twoSurfaces);
    CHECK_THAT(why, ContainsSubstring("two cylinders here: R4.68 mm and "
                                      "R5.18 mm, stepping at 12.4 mm along "
                                      "the axis"));
    CHECK_THAT(why, ContainsSubstring("click one, or give radius_mm"));
  }
  SECTION("the numbers, against the generator's") {
    const mesh::TwoSurfaces t = SplitOf(m, seed, mesh::Kind::Cylinder, tol);
    Print("stepped bore", t);
    REQUIRE(t.two);
    CHECK_THAT(t.a.radius, WithinAbs(kNarrow, 0.01));
    CHECK_THAT(t.b.radius, WithinAbs(kWide, 0.01));
    CHECK(LineAngleDeg(t.a.axis, b.axis) < 0.1);
    CHECK(FromLine(b.base, t.a.origin, t.a.axis) < 0.05);
    REQUIRE(t.steps);
    // The step lies between the grid rows either side of 12.4.
    const glm::dvec3 at = t.from + t.stepAt * t.axis;
    CHECK_THAT(glm::dot(at - b.base, b.axis), WithinAbs(kStepAt, 0.2));
    CHECK(glm::dot(t.axis, b.axis) > 0.999);
  }
  SECTION("radius_mm picks one out") {
    mesh::GrowOptions o;
    o.noiseMm = mesh::ScanTolerance(m);
    o.hints.radiusMin = 5.0;
    o.hints.radiusMax = 5.4;
    std::string why;
    const mesh::Region r =
        mesh::GrowFrom(m, seed, mesh::Kind::Cylinder, tol, &why, o);
    INFO(why);
    REQUIRE(r.fit.ok);
    CHECK_THAT(r.fit.radius, WithinAbs(kWide, 0.01));
    CHECK(LineAngleDeg(r.fit.axis, b.axis) < 0.1);
  }
}

TEST_CASE("A 0.3 mm step between two planes is refused, naming both",
          "[mesh][fit]") {
  const fixtures::Sheet& s = SteppedPlate();
  const mesh::TriMesh& m = s.organic.mesh;
  const uint32_t seed = SeedAt(m, s.At(8.0, 15.0));
  SECTION("at the scan's own tolerance the click is one of them") {
    std::string why;
    const mesh::Region r = mesh::GrowFrom(m, seed, mesh::Kind::Plane,
                                          mesh::ScanTolerance(m), &why);
    INFO(why);
    REQUIRE(r.fit.ok);
    CHECK(std::abs(r.fit.Distance(s.At(8.0, 15.0))) < 0.01);
    CHECK(std::abs(r.fit.Distance(s.At(30.0, 15.0)) - kPlateStep) < 0.01);
  }
  const double tol = 0.6;
  SECTION("refused") {
    std::string why;
    mesh::GrowOptions o;
    o.noiseMm = mesh::ScanTolerance(m);
    const mesh::Region r =
        mesh::GrowFrom(m, seed, mesh::Kind::Plane, tol, &why, o);
    std::printf("stepped plate: %s\n", why.c_str());
    CHECK_FALSE(r.fit.ok);
    CHECK_THAT(why, ContainsSubstring("two planes here: 0.30 mm apart and "
                                      "0.0"));
    CHECK_THAT(why, ContainsSubstring("click one, or give within"));
  }
  SECTION("the numbers, against the generator's") {
    const mesh::TwoSurfaces t = SplitOf(m, seed, mesh::Kind::Plane, tol);
    Print("stepped plate", t);
    REQUIRE(t.two);
    CHECK_THAT(t.offset, WithinAbs(kPlateStep, 0.005));
    CHECK(t.angle * 180.0 / kPi < 0.05);
    CHECK(LineAngleDeg(t.a.axis, s.Normal()) < 0.05);
    // The lower through the plate's first part, the upper its second.
    const glm::dvec3 lowPlane = s.ToWorld({0.0, 0.0, 0.0});
    CHECK(std::abs(glm::dot(t.throughA - lowPlane, s.Normal())) < 0.01);
    CHECK(std::abs(glm::dot(t.throughB - lowPlane, s.Normal()) - kPlateStep) <
          0.01);
  }
}

TEST_CASE("A tapered bore and a gently crowned plate are not split",
          "[mesh][fit]") {
  const double tol = 0.6;
  SECTION("a taper") {
    const fixtures::Bore b = fixtures::ScannedBore(
        [](double h) { return 4.9 + 0.4 * h / 25.0; }, 25.0, 0.25, 0.05);
    const mesh::TriMesh& m = b.organic.mesh;
    const uint32_t seed = SeedAt(m, b.At(12.5, 30.0));
    const mesh::TwoSurfaces t = SplitOf(m, seed, mesh::Kind::Cylinder, tol);
    Print("taper", t);
    CHECK(t.examined);
    CHECK_FALSE(t.two);
    // One radius varying evenly along the axis explains it better than
    // two cylinders do.
    CHECK(t.smoothRms < t.twoRms);
    mesh::GrowOptions o;
    o.noiseMm = mesh::ScanTolerance(m);
    std::string why;
    const mesh::Region r =
        mesh::GrowFrom(m, seed, mesh::Kind::Cylinder, tol, &why, o);
    INFO(why);
    CHECK(r.fit.ok);
  }
  SECTION("a crowned plate") {
    // 0.4 mm of crown over 40 mm: a turn of 4.6 deg edge to edge.
    const fixtures::Sheet s = fixtures::ScannedSheet(
        [](double x, double) {
          const double u = (x - 20.0) / 20.0;
          return 0.4 * (1.0 - u * u);
        },
        40.0, 30.0, 0.25, 0.05);
    const mesh::TriMesh& m = s.organic.mesh;
    const uint32_t seed = SeedAt(m, s.At(20.0, 15.0));
    const mesh::TwoSurfaces t = SplitOf(m, seed, mesh::Kind::Plane, tol);
    Print("crowned plate", t);
    CHECK(t.examined);
    CHECK_FALSE(t.two);
    CHECK(t.smoothRms < t.twoRms);
    mesh::GrowOptions o;
    o.noiseMm = mesh::ScanTolerance(m);
    std::string why;
    const mesh::Region r =
        mesh::GrowFrom(m, seed, mesh::Kind::Plane, tol, &why, o);
    INFO(why);
    REQUIRE(r.fit.ok);
    const mesh::PlaneBow bow =
        mesh::BowOf(m, r.triangles, r.fit, r.extent, r.area);
    std::printf("crowned plate: normals turn %.2f deg (4.6), cover %.2f\n",
                bow.spreadRad * 180.0 / kPi, bow.cover);
    CHECK_THAT(bow.spreadRad * 180.0 / kPi, WithinAbs(4.58, 0.3));
  }
}

TEST_CASE("A plane on the crown of a round is refused, naming the cylinder",
          "[mesh][fit]") {
  const fixtures::Post& post = Post();
  const mesh::TriMesh& m = post.organic.mesh;
  std::string why;
  const mesh::Region r =
      mesh::GrowFrom(m, SeedAt(m, post.onCylinder), mesh::Kind::Plane,
                     mesh::ScanTolerance(m), &why);
  std::printf("plane on the post's side: %s\n", why.c_str());
  CHECK_FALSE(r.fit.ok);
  CHECK_THAT(why, ContainsSubstring("strip along the crown of a round"));
  CHECK_THAT(why, ContainsSubstring("a cylinder fits there: R12 mm"));
  CHECK_THAT(why, ContainsSubstring("fit kind cylinder"));
  // The post's top is a plane, and stays one.
  std::string topWhy;
  CHECK(mesh::GrowFrom(m, SeedAt(m, post.onTop), mesh::Kind::Plane,
                       mesh::ScanTolerance(m), &topWhy)
            .fit.ok);
}

// What the region and the clip cost against the growth by distance alone
// that came before them, on the same machine in the same run: CPU time, a
// COST check (docs/ui/testing.md#tests-that-time-things).
TEST_CASE("The region and the clip cost at most twice the old growth",
          "[mesh][fit][slow]") {
  // Both times on one core: a move between a performance and an efficiency
  // core between them read x2.20 where the code costs x1.1.
  const SameCore held;
  for (const size_t n : {size_t(300000), size_t(2000000)}) {
    const fixtures::Organic s = fixtures::NoisySphere(n);
    const double tol = mesh::ScanTolerance(s.mesh);
    const uint32_t seed = SeedAt(s.mesh, {0.0, 0.0, 40.0});
    const auto cpu = [&](auto&& f) {
      const std::clock_t t0 = std::clock();
      f();
      return double(std::clock() - t0) / CLOCKS_PER_SEC;
    };
    mesh::Region now, old;
    const double oldS = cpu(
        [&] { old = LegacyGrowFrom(s.mesh, seed, mesh::Kind::Sphere, tol); });
    const double nowS = cpu(
        [&] { now = mesh::GrowFrom(s.mesh, seed, mesh::Kind::Sphere, tol); });
    REQUIRE(now.fit.ok);
    REQUIRE(old.fit.ok);
    std::printf("sphere %zu: region + clip %.3f s CPU, the old growth %.3f s "
                "(x%.2f); radius %.5f, used %zu\n",
                s.mesh.TriangleCount(), nowS, oldS, nowS / oldS, now.fit.radius,
                now.used);
    CHECK(nowS <= 2.0 * oldS);
    CHECK_THAT(now.fit.radius, WithinAbs(s.radius, 0.01));
  }
  // Where the split runs: a stepped bore of 240k triangles grown at a
  // tolerance that takes both its cylinders, split and refused.
  {
    const fixtures::Bore b = fixtures::ScannedBore(
        [](double h) { return h < 50.0 ? 30.0 : 30.5; }, 100.0, 0.4, 0.05);
    const mesh::TriMesh& m = b.organic.mesh;
    const uint32_t seed = SeedAt(m, b.At(25.0, 40.0));
    const double tol = 0.6;
    mesh::GrowOptions o;
    o.noiseMm = mesh::ScanTolerance(m);
    const auto cpu = [&](auto&& f) {
      const std::clock_t t0 = std::clock();
      f();
      return double(std::clock() - t0) / CLOCKS_PER_SEC;
    };
    mesh::Region now, old;
    std::string why;
    const double oldS =
        cpu([&] { old = LegacyGrowFrom(m, seed, mesh::Kind::Cylinder, tol); });
    const double nowS = cpu([&] {
      now = mesh::GrowFrom(m, seed, mesh::Kind::Cylinder, tol, &why, o);
    });
    REQUIRE(old.fit.ok);
    std::printf("stepped bore %zu: region + split %.3f s CPU, the old "
                "growth %.3f s (x%.2f): %s\n",
                m.TriangleCount(), nowS, oldS, nowS / oldS, why.c_str());
    CHECK(now.twoSurfaces);
    CHECK(nowS <= 2.0 * oldS);
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
