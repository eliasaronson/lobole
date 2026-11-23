// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// How far a body is from a scan
// (docs/inspect/README.md#how-far-a-body-is-from-a-scan): scans generated on
// known surfaces, compared with bodies that differ from them by known
// amounts, through scancompare -- what compare_scan, set_scan_deviation and
// the Compare to scan tool call -- and every number checked against the
// generator's. Every fixture is turned off the axes.

#include "CostCheck.h"
#include "mesh_organic.h"

#include "cad/CADKernel.h"
#include "core/Feature.h"
#include "core/ScanCompare.h"
#include "core/mesh/Deviation.h"
#include "core/mesh/Segment.h"
#include "core/mesh/TriMesh.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <catch2/catch_test_macros.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <cstdio>
#include <ctime>
#include <memory>
#include <random>

namespace {

// A turn off every axis, and a place off the origin.
const glm::dmat3 kTurn = glm::mat3_cast(
    glm::angleAxis(0.61, glm::normalize(glm::dvec3(0.4, -0.7, 0.55))));
const glm::dvec3 kCentre(13.0, -7.0, 21.0);

glm::dvec3 ToWorld(const glm::dvec3& local) { return kCentre + kTurn * local; }
glm::dvec3 ToLocal(const glm::dvec3& world) {
  return glm::transpose(kTurn) * (world - kCentre);
}

// A box [lo, hi] (its own frame) as a scan: each face a grid of about
// `spacing`, welded, every vertex moved along its normal by a deterministic
// noise of at most `noise`, turned and placed by kTurn / kCentre. Closed.
std::shared_ptr<const mesh::TriMesh> ScannedBox(const glm::dvec3& lo,
                                                const glm::dvec3& hi,
                                                double spacing, double noise) {
  std::vector<glm::dvec3> soup;
  for (int axis = 0; axis < 3; axis++)
    for (int side : {-1, 1}) {
      const int u = (axis + 1) % 3, v = (axis + 2) % 3;
      const int nu = std::max(1, int(std::lround((hi[u] - lo[u]) / spacing)));
      const int nv = std::max(1, int(std::lround((hi[v] - lo[v]) / spacing)));
      const auto at = [&](int i, int j) {
        glm::dvec3 p(0.0);
        p[axis] = side > 0 ? hi[axis] : lo[axis];
        p[u] = lo[u] + (hi[u] - lo[u]) * double(i) / nu;
        p[v] = lo[v] + (hi[v] - lo[v]) * double(j) / nv;
        return p;
      };
      for (int i = 0; i < nu; i++)
        for (int j = 0; j < nv; j++) {
          const glm::dvec3 a = at(i, j), b = at(i + 1, j), c = at(i + 1, j + 1),
                           d = at(i, j + 1);
          // u x v is +axis: counter-clockwise from outside on the + side.
          if (side > 0) {
            for (const glm::dvec3& p : {a, b, c, a, c, d})
              soup.push_back(p);
          } else {
            for (const glm::dvec3& p : {a, c, b, a, d, c})
              soup.push_back(p);
          }
        }
    }
  mesh::TriMesh m = mesh::FromSoup(soup, 1e-9);
  std::vector<glm::dvec3> n(m.positions.size(), glm::dvec3(0.0));
  for (uint32_t t = 0; t < m.triangles.size(); t++)
    for (int c = 0; c < 3; c++)
      n[m.triangles[t][c]] += m.AreaNormal(t);
  std::mt19937_64 rng(0xb0c5);
  std::uniform_real_distribution<double> u(-noise, noise);
  for (size_t i = 0; i < m.positions.size(); i++)
    m.positions[i] = ToWorld(m.positions[i] + u(rng) * glm::normalize(n[i]));
  m.quantum = mesh::Float32Quantum(m.positions);
  mesh::BuildAdjacency(m);
  return std::make_shared<const mesh::TriMesh>(std::move(m));
}

// A B-Rep box [lo, hi] (its own frame), turned and placed the same way.
Body BoxBody(const glm::dvec3& lo, const glm::dvec3& hi) {
  const glm::dvec3 s = hi - lo;
  CADShape box(
      BRepPrimAPI_MakeBox(gp_Pnt(lo.x, lo.y, lo.z), s.x, s.y, s.z).Shape());
  glm::dmat4 xf(kTurn);
  xf[3] = glm::dvec4(kCentre, 1.0);
  Body b;
  b.name = "box";
  b.shape = box.Transformed(xf);
  b.createdBy = 2;
  return b;
}

scancompare::Input InputOf(std::shared_ptr<const mesh::TriMesh> scan,
                           const std::vector<const Body*>& bodies,
                           std::optional<double> tol = std::nullopt) {
  scancompare::Input in;
  in.request.scanId = 1;
  in.request.bodyId = 2;
  in.request.toleranceMm = tol;
  in.scanName = "scan";
  in.bodyName = "body";
  in.scan = std::move(scan);
  std::vector<std::pair<const Body*, int>> pairs;
  for (size_t i = 0; i < bodies.size(); i++)
    pairs.emplace_back(bodies[i], int(i));
  scancompare::AddBodies(pairs, tol.value_or(mesh::ScanTolerance(*in.scan)),
                         in);
  return in;
}

Body MeshBody(mesh::TriMesh m) {
  Body b;
  b.name = "mesh";
  b.mesh = std::make_shared<const mesh::TriMesh>(std::move(m));
  b.createdBy = 2;
  return b;
}

double Cpu() {
  timespec ts{};
  clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
  return double(ts.tv_sec) + 1e-9 * double(ts.tv_nsec);
}

} // namespace

TEST_CASE("compare_scan: a face raised and a face sunk are the two hotspots",
          "[scan_compare][mesh_body]") {
  // The scan is of the box as made; the model's top stands 0.20 mm proud of
  // it and its bottom 0.15 mm short. The scan's top lies inside the model
  // (negative) and its bottom outside (positive).
  constexpr double kNoise = 0.02;
  const glm::dvec3 lo(-15.0, -12.0, -5.0), hi(15.0, 12.0, 5.0);
  const auto scan = ScannedBox(lo, hi, 0.5, kNoise);
  const Body body =
      BoxBody(lo + glm::dvec3(0, 0, 0.15), hi + glm::dvec3(0, 0, 0.20));
  const scancompare::Input in = InputOf(scan, {&body});
  const scancompare::Outcome got = scancompare::Run(in, nullptr, nullptr);
  const mesh::Deviation& d = got.deviation;
  std::printf("raised/sunk: %s\n", got.reply.dump().c_str());
  const double tol = d.toleranceMm;
  CHECK(tol < 0.075); // so 0.15 is twice it: a feature
  CHECK(d.chordMm <= tol);
  REQUIRE(d.hotspots.size() == 2);
  const mesh::DeviationHotspot& top = d.hotspots[0];
  const mesh::DeviationHotspot& bottom = d.hotspots[1];
  const double slack = kNoise + d.chordMm + 1e-6;
  CHECK(std::abs(top.depthMm - -0.20) <= slack);
  CHECK(std::abs(bottom.depthMm - 0.15) <= slack);
  CHECK(top.feature);
  CHECK(bottom.feature);
  // On those faces: the top's peak on the top plane, the bottom's on the
  // bottom, each named as the body's face there.
  CHECK(std::abs(ToLocal(top.atMm).z - hi.z) < 2.0 * kNoise);
  CHECK(std::abs(ToLocal(bottom.atMm).z - lo.z) < 2.0 * kNoise);
  REQUIRE(top.face != mesh::kNone);
  REQUIRE(bottom.face != mesh::kNone);
  CHECK(top.face != bottom.face);
  // Each covers its face, 30 x 24 = 720 mm2, to within a strip round it.
  CHECK(std::abs(top.areaMm2 - 720.0) < 60.0);
  CHECK(std::abs(bottom.areaMm2 - 720.0) < 60.0);
  // Every other face inside the tolerance: away from the top and bottom
  // planes, every vertex.
  size_t others = 0, outside = 0;
  for (size_t i = 0; i < scan->positions.size(); i++) {
    const glm::dvec3 l = ToLocal(scan->positions[i]);
    if (l.z > hi.z - 0.5 || l.z < lo.z + 0.5)
      continue;
    others++;
    if (!(std::abs(d.distanceMm[i]) <= tol))
      outside++;
  }
  CHECK(others > 1000);
  CHECK(outside == 0);
  // The faces a vertex was measured to: the top vertices to the top face.
  for (size_t i = 0; i < scan->positions.size(); i++) {
    const glm::dvec3 l = ToLocal(scan->positions[i]);
    if (std::abs(l.x) < 10.0 && std::abs(l.y) < 8.0 && l.z > hi.z - 0.1)
      CHECK(d.faceOf[i] == top.face);
  }
  CHECK(got.reply.at("hotspots").size() == 2);
  CHECK(got.reply.at("hotspots")[0].at("face").get<int>() ==
        in.faces[top.face].face);
  CHECK(got.receipt.Roll() == receipt::Standing::Failed);
}

TEST_CASE("compare_scan: a 1 mm wall reads its own side, not the far one",
          "[scan_compare][mesh_body]") {
  // A plate 1 mm thick, and its scan 0.6 mm into it: the scan's top lies
  // 0.6 under the model's top and 0.4 over its bottom. The nearest surface
  // is the far side; the right reading is -0.6 off the top.
  constexpr double kNoise = 0.01;
  const glm::dvec3 lo(-20.0, -15.0, -0.5), hi(20.0, 15.0, 0.5);
  const auto scan = ScannedBox(lo - glm::dvec3(0, 0, 0.6),
                               hi - glm::dvec3(0, 0, 0.6), 0.5, kNoise);
  const Body body = BoxBody(lo, hi);
  const scancompare::Outcome got =
      scancompare::Run(InputOf(scan, {&body}), nullptr, nullptr);
  const mesh::Deviation& d = got.deviation;
  size_t topCount = 0, bottomCount = 0;
  for (size_t i = 0; i < scan->positions.size(); i++) {
    const glm::dvec3 l = ToLocal(scan->positions[i]);
    if (std::abs(l.x) > 19.0 || std::abs(l.y) > 14.0)
      continue;
    if (l.z > -0.2) {
      topCount++;
      CHECK(std::abs(d.distanceMm[i] - -0.6) <= kNoise + d.chordMm + 1e-6);
    } else {
      bottomCount++;
      CHECK(std::abs(d.distanceMm[i] - 0.6) <= kNoise + d.chordMm + 1e-6);
    }
  }
  CHECK(topCount > 1000);
  CHECK(bottomCount > 1000);
}

TEST_CASE("compare_scan: a scan of half the part leaves half the body unseen",
          "[scan_compare][mesh_body]") {
  const fixtures::Solid part =
      fixtures::ScannedSolid(fixtures::SolidShape::Bracket, 0.8, 0.05, true);
  // The share of the body's area on the side the scan did not see: its own
  // frame's -x.
  double unseen = 0.0, all = 0.0;
  for (uint32_t t = 0; t < part.body.triangles.size(); t++) {
    const glm::dvec3 c = (part.body.Corner(t, 0) + part.body.Corner(t, 1) +
                          part.body.Corner(t, 2)) /
                         3.0;
    const double a = part.body.Area(t);
    all += a;
    if ((glm::transpose(part.turn) * (c - part.centre)).x < 0.0)
      unseen += a;
  }
  const Body body = MeshBody(part.body);
  const scancompare::Outcome got = scancompare::Run(
      InputOf(std::make_shared<const mesh::TriMesh>(part.scan.mesh), {&body}),
      nullptr, nullptr);
  std::printf("half: no_scan %.4f against %.4f unseen; within %.4f\n",
              got.deviation.noScan, unseen / all, got.deviation.within);
  CHECK(std::abs(got.deviation.noScan - unseen / all) < 0.05);
  CHECK(got.deviation.within > 0.97); // what it saw, it saw right
  CHECK(got.receipt.Roll() == receipt::Standing::Unverified);
}

TEST_CASE("compare_scan: a scan against the body it was sampled from is "
          "within",
          "[scan_compare][mesh_body]") {
  for (fixtures::SolidShape shape :
       {fixtures::SolidShape::Bracket, fixtures::SolidShape::Plate}) {
    const fixtures::Solid part = fixtures::ScannedSolid(shape, 0.8, 0.05);
    const Body body = MeshBody(part.body);
    const scancompare::Outcome got = scancompare::Run(
        InputOf(std::make_shared<const mesh::TriMesh>(part.scan.mesh), {&body}),
        nullptr, nullptr);
    const mesh::Deviation& d = got.deviation;
    std::printf("own body: within %.4f no_scan %.4f mean %.4f p95 %.4f "
                "tol %.4f, %zu hotspots\n",
                d.within, d.noScan, d.meanMm, d.p95Mm, d.toleranceMm,
                d.hotspots.size());
    CHECK(d.within > 0.99);
    CHECK(d.noScan < 0.01);
    CHECK(std::abs(d.meanMm) < 0.01);
    for (const mesh::DeviationHotspot& h : d.hotspots)
      CHECK_FALSE(h.feature);
    CHECK(got.receipt.Roll() == receipt::Standing::Verified);
  }
}

TEST_CASE("compare_scan: the tolerance is asked, else the noise, never under "
          "the chord",
          "[scan_compare][mesh_body]") {
  const glm::dvec3 lo(-15.0, -12.0, -5.0), hi(15.0, 12.0, 5.0);
  const auto scan = ScannedBox(lo, hi, 0.5, 0.02);
  const Body body = BoxBody(lo, hi);
  const mesh::Deviation asked =
      scancompare::Run(InputOf(scan, {&body}, 0.3), nullptr, nullptr).deviation;
  CHECK(asked.toleranceMm == 0.3);
  CHECK(asked.toleranceFrom == "asked");
  const mesh::Deviation noise =
      scancompare::Run(InputOf(scan, {&body}), nullptr, nullptr).deviation;
  CHECK(noise.toleranceFrom == "scan_noise");
  CHECK(noise.toleranceMm == noise.noiseMm);
  // Asked finer than the body's triangles can say: the chord.
  scancompare::Input fine = InputOf(scan, {&body}, 1e-7);
  fine.chordMm = 0.01;
  const mesh::Deviation floored =
      scancompare::Run(fine, nullptr, nullptr).deviation;
  CHECK(floored.toleranceFrom == "chord");
  CHECK(floored.toleranceMm == 0.01);
}

TEST_CASE("compare_scan stops when it is cancelled", "[scan_compare]") {
  const glm::dvec3 lo(-15.0, -12.0, -5.0), hi(15.0, 12.0, 5.0);
  const auto scan = ScannedBox(lo, hi, 0.5, 0.02);
  const Body body = BoxBody(lo, hi);
  std::atomic<bool> cancel{true};
  const scancompare::Outcome got =
      scancompare::Run(InputOf(scan, {&body}), &cancel, nullptr);
  CHECK(got.deviation.cancelled);
  CHECK(got.reply.is_null());
}

TEST_CASE("compare_scan of a 500k-triangle scan costs what it did",
          "[scan_compare][slow]") {
  const fixtures::Solid part =
      fixtures::ScannedSolid(fixtures::SolidShape::Bracket, 0.33, 0.03, false,
                             /*bodySpacing=*/0.4);
  std::printf("cost: %zu scan triangles, %zu body triangles\n",
              part.scan.mesh.triangles.size(), part.body.triangles.size());
  REQUIRE(part.scan.mesh.triangles.size() > 450000);
  const Body body = MeshBody(part.body);
  const scancompare::Input in =
      InputOf(std::make_shared<const mesh::TriMesh>(part.scan.mesh), {&body});
  const double t0 = Cpu();
  const scancompare::Outcome got = scancompare::Run(in, nullptr, nullptr);
  const double cpu = Cpu() - t0;
  std::printf("cost: compare %.2f CPU s; within %.4f\n", cpu,
              got.deviation.within);
  CHECK(got.deviation.within > 0.99);
  // Measured at 1.6 CPU s (2026-11, 505k scan triangles against a 341k
  // triangle body, threads on both kinds of core); the budget leaves room
  // for every thread on the slow ones, which cost 1.8 times as much.
  CHECK_COST(cpu < 4.0);
}
