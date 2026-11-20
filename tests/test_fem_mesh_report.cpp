// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// The mesh as something looked at, and a study checked before it is run
// (docs/simulation/README.md#the-mesh-report,
// docs/simulation/README.md#checking-a-study). Each case plants the thing
// the report or the check exists to find and asserts it is found: a hole in
// the mesh, a wall one element through, a refinement that changed nothing,
// two solids that touch without sharing nodes, three setup faults at once.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "cad/CADKernel.h"
#include "core/Document.h"
#include "core/Feature.h"
#include "core/Measure.h"
#include "core/fem/FemCheck.h"
#include "core/fem/FemMeshReport.h"
#include "core/fem/FemMesher.h"
#include "core/fem/FemSolver.h"
#include "core/fem/FemStudy.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <set>
#include <string>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

// One straight tet10 on the corners given.
fem::FemMesh OneTet(const glm::dvec3& a, const glm::dvec3& b,
                    const glm::dvec3& c, const glm::dvec3& d) {
  fem::FemMesh m;
  m.nodes = {a,
             b,
             c,
             d,
             0.5 * (a + b),
             0.5 * (b + c),
             0.5 * (c + a),
             0.5 * (a + d),
             0.5 * (b + d),
             0.5 * (c + d)};
  m.tets.push_back({0, 1, 2, 3, 4, 5, 6, 7, 8, 9});
  return m;
}

} // namespace

TEST_CASE("An element's measures: a regular tet reads 1, a curved edge less",
          "[fem][meshreport]") {
  const double s = 1.0 / std::sqrt(2.0);
  fem::FemMesh m =
      OneTet({1, 0, -s}, {-1, 0, -s}, {0, 1, s}, {0, -1, s}); // regular
  const int sign = fem::MeshOrientation(m);
  REQUIRE(sign != 0);
  fem::TetMeasure r = fem::MeasureTet(m, 0, sign);
  CHECK_THAT(r.shape, WithinAbs(1.0, 1e-9));
  CHECK_THAT(r.jacobian, WithinAbs(1.0, 1e-9));
  CHECK_THAT(r.size, WithinAbs(2.0, 1e-9));
  CHECK_THAT(r.Quality(), WithinAbs(1.0, 1e-9));
  // Its volume, exactly: edge 2 -> 2^3 / (6 sqrt 2).
  CHECK_THAT(fem::MeshVolume(m),
             WithinRel(8.0 / (6.0 * std::sqrt(2.0)), 1e-12));

  // Pull one midside node a third of the way to its corner: the corners,
  // and so the shape metric, do not move; the scaled Jacobian does.
  m.nodes[4] = m.nodes[0] + (m.nodes[1] - m.nodes[0]) * (1.0 / 3.0);
  r = fem::MeasureTet(m, 0, sign);
  CHECK_THAT(r.shape, WithinAbs(1.0, 1e-9));
  CHECK(r.jacobian < 0.6);
  CHECK(r.jacobian > 0.0);
  CHECK(r.Quality() == r.jacobian);
  // A fifth folds it (docs/simulation/README.md#curved-elements-that-fold).
  m.nodes[4] = m.nodes[0] + (m.nodes[1] - m.nodes[0]) * 0.2;
  CHECK(fem::MeasureTet(m, 0, sign).jacobian <= 0.0);
}

TEST_CASE("A two-solid mesh shares its interface, or says it does not",
          "[fem][meshreport]") {
  // Two tets back to back on the triangle (0,1,2): one mesh, two domains.
  fem::FemMesh m;
  m.nodes = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0, 0, -1}};
  const auto mid = [&](int a, int b) {
    m.nodes.push_back(0.5 * (m.nodes[a] + m.nodes[b]));
    return static_cast<int>(m.nodes.size()) - 1;
  };
  const int m01 = mid(0, 1), m12 = mid(1, 2), m20 = mid(2, 0);
  m.tets.push_back(
      {0, 1, 2, 3, m01, m12, m20, mid(0, 3), mid(1, 3), mid(2, 3)});
  m.tets.push_back(
      {0, 2, 1, 4, m20, m12, m01, mid(0, 4), mid(2, 4), mid(1, 4)});
  m.tetDomain = {0, 1};
  fem::MeshBodyReport r = fem::ReportMesh(m, {}, 1.0);
  CHECK(r.domains == 2);
  REQUIRE(r.interfaces.size() == 1);
  CHECK(r.interfaces[0].sharedFaces == 1);
  CHECK(r.looseDomains.empty());
  CHECK(r.unsharedNodes == 0);

  // The same two solids touching without sharing: the second tet on its own
  // copies of the three interface corners.
  fem::FemMesh apart = m;
  int copy[3];
  for (int k = 0; k < 3; ++k) {
    apart.nodes.push_back(apart.nodes[k]);
    copy[k] = static_cast<int>(apart.nodes.size()) - 1;
  }
  apart.tets[1][0] = copy[0];
  apart.tets[1][1] = copy[2];
  apart.tets[1][2] = copy[1];
  // Both sides' faces are boundary now.
  apart.boundary.push_back({{0, 1, 2, m01, m12, m20}, 0});
  apart.boundary.push_back({{copy[0], copy[2], copy[1], m20, m12, m01}, 1});
  r = fem::ReportMesh(apart, {}, 1.0);
  CHECK(r.interfaces.empty());
  CHECK(r.looseDomains.size() == 2);
  CHECK(r.unsharedNodes == 3);
}

#ifdef HAVE_NETGEN

TEST_CASE("A mesh fills its shape, and one with a hole is refused",
          "[fem][meshreport][mesher]") {
  // A flat-faced box: nothing for deflection to explain away.
  std::string err;
  fem::MeshSettings ms;
  ms.maxSize = 4.0;
  CADShape box = CADKernel::Instance().CreateBox(40, 20, 10);
  auto mesh = fem::Mesh(box.GetShape(), ms, &err);
  INFO(err);
  REQUIRE(mesh.has_value());
  CHECK_THAT(mesh->stats.shapeVolumeMm3, WithinRel(8000.0, 1e-9));
  CHECK_THAT(mesh->stats.volumeMm3, WithinRel(8000.0, 1e-9));
  CHECK(fem::MeshVolumeRefusal(*mesh, mesh->stats.shapeVolumeMm3).empty());

  // Plant a hole: one element gone.
  fem::FemMesh holed = *mesh;
  holed.tets.erase(holed.tets.begin() + holed.tets.size() / 2);
  const std::string why =
      fem::MeshVolumeRefusal(holed, mesh->stats.shapeVolumeMm3);
  INFO(why);
  CHECK_THAT(why, ContainsSubstring("does not fill its shape"));
  CHECK_THAT(why, ContainsSubstring("left part of the body empty"));

  // A curved part: the tube's mesh sits inside the slack its own sag gives,
  // and far inside it (a tet10's boundary is a parabola on the circle).
  CADShape outer = CADKernel::Instance().CreateCylinder(
      glm::vec3(0, 0, 0), glm::vec3(0, 0, 1), 20, 30);
  CADShape bore = CADKernel::Instance().CreateCylinder(
      glm::vec3(0, 0, -1), glm::vec3(0, 0, 1), 10, 32);
  CADShape tube = outer.Difference(bore);
  ms.maxSize = 0.0; // auto: the coarse end
  auto curved = fem::Mesh(tube.GetShape(), ms, &err);
  INFO(err);
  REQUIRE(curved.has_value());
  const double exact = std::numbers::pi * (400.0 - 100.0) * 30.0;
  CHECK_THAT(curved->stats.shapeVolumeMm3, WithinRel(exact, 1e-5));
  const double off = std::abs(curved->stats.volumeMm3 - exact);
  const double slack = fem::MeshVolumeSlack(*curved, exact);
  INFO("off " << off << " mm3 of a slack of " << slack);
  CHECK(off < 0.25 * slack);
  CHECK(fem::MeshVolumeRefusal(*curved, exact).empty());
  // And a region left empty is still seen: a tenth of the elements gone is
  // far past what the curved faces' sag accounts for.
  fem::FemMesh holedTube = *curved;
  holedTube.tets.resize(holedTube.tets.size() * 9 / 10);
  CHECK_THAT(fem::MeshVolumeRefusal(holedTube, exact),
             ContainsSubstring("does not fill its shape"));
}

TEST_CASE("The mesh report names a wall one element through and a "
          "refinement that did not take",
          "[fem][meshreport][mesher]") {
  // A 60 x 20 x 1 plate at the auto size (4.2 mm): one element through.
  CADShape plate = CADKernel::Instance().CreateBox(60, 20, 1);
  std::string err;
  fem::MeshSettings ms;
  // Face 0 asked for a size coarser than the study's: nothing to take.
  ms.faceSizes.push_back({0, 50.0});
  auto mesh = fem::Mesh(plate.GetShape(), ms, &err);
  INFO(err);
  REQUIRE(mesh.has_value());
  const double autoSize = fem::AutoMeshSize(plate.GetShape());
  const fem::MeshBodyReport r = fem::ReportMesh(*mesh, ms, autoSize);
  CHECK(r.nodes == mesh->nodes.size());
  CHECK(r.tets == mesh->tets.size());
  CHECK(r.quality.size() == mesh->tets.size());
  CHECK(r.size.size() == mesh->tets.size());
  CHECK(r.sizeMin > 0.0);
  CHECK(r.sizeMin <= r.sizeMean);
  CHECK(r.sizeMean <= r.sizeMax);

  // The worst elements, worst first, the first one the report's worst.
  REQUIRE_FALSE(r.worst.empty());
  CHECK(r.worst.size() <= 12);
  for (size_t k = 1; k < r.worst.size(); ++k)
    CHECK(r.worst[k - 1].quality <= r.worst[k].quality);
  CHECK(r.worstQuality == r.worst.front().quality);
  CHECK(r.worstQuality <= mesh->stats.worstQuality + 1e-9);
  // Each is where it says: inside the plate.
  for (const auto& e : r.worst) {
    CHECK(e.at.z > 0.0);
    CHECK(e.at.z < 1.0);
  }

  // The wall: its two large faces, 1 mm apart, most of the mesh.
  REQUIRE_FALSE(r.thinWalls.empty());
  const auto& wall = r.thinWalls.front();
  CHECK_THAT(wall.thicknessMm, WithinAbs(1.0, 1e-6));
  CHECK(wall.tets > mesh->tets.size() / 2);
  CHECK(wall.at.z > 0.0);
  CHECK(wall.at.z < 1.0);

  // The refinement that asked for nothing.
  REQUIRE(r.refinementMisses.size() == 1);
  CHECK(r.refinementMisses[0].faceIndex == 0);
  CHECK_THAT(r.refinementMisses[0].why,
             ContainsSubstring("no less than the study's mesh size"));

  // A cube at the same relative size has no wall, and a real refinement
  // takes.
  CADShape cube = CADKernel::Instance().CreateBox(20, 20, 20);
  fem::MeshSettings fine;
  fine.faceSizes.push_back({0, 1.0});
  auto cubeMesh = fem::Mesh(cube.GetShape(), fine, &err);
  INFO(err);
  REQUIRE(cubeMesh.has_value());
  const fem::MeshBodyReport c =
      fem::ReportMesh(*cubeMesh, fine, fem::AutoMeshSize(cube.GetShape()));
  CHECK(c.thinWalls.empty());
  CHECK(c.refinementMisses.empty());
  CHECK(c.straightenedNodes == 0);
  CHECK(c.domains == 1);
}

namespace {
ElementRef FaceNear(const Body& body, const glm::dvec3& p) {
  std::string best;
  double bestD = std::numeric_limits<double>::infinity();
  for (const auto& [idx, name] : body.elementMap.names) {
    const auto d = Measure::DistanceToElementAt(body, true, idx, p, 1e-3);
    if (d && *d < bestD) {
      bestD = *d;
      best = name;
    }
  }
  REQUIRE_FALSE(best.empty());
  return ElementRef{body.createdBy, best};
}
} // namespace

TEST_CASE("check_study returns every planted problem at once, each with "
          "its fix",
          "[fem][check]") {
  Document doc;
  // No material: the default has no modulus.
  doc.AddFeature(
      std::make_unique<BoxFeature>("Bar", glm::dvec3(0), 100.0, 10.0, 10.0));
  const Body& body = doc.GetBodies()[0];
  fem::Study& s = doc.AddStudy("S");
  // 1. no support: a force and nothing held.
  fem::Load force;
  force.id = doc.AllocateId();
  force.type = fem::LoadType::Force;
  force.face = FaceNear(body, {100, 5, 5});
  force.magnitude = 100.0;
  force.direction = {1, 0, 0};
  s.loads.push_back(force);
  // 2. a torque on a flat face, with no axis of its own.
  fem::Load torque;
  torque.id = doc.AllocateId();
  torque.type = fem::LoadType::Torque;
  torque.face = FaceNear(body, {50, 5, 10});
  torque.magnitude = 500.0;
  s.loads.push_back(torque);
  // 3. an anchor that does not resolve.
  fem::Load gone;
  gone.id = doc.AllocateId();
  gone.type = fem::LoadType::Pressure;
  gone.face = ElementRef{body.createdBy, "no/such/face"};
  gone.magnitude = 1.0;
  s.loads.push_back(gone);

  const std::string before = doc.GetStudies()[0].error;
  const std::vector<fem::StudyProblem> problems = fem::CheckStudy(doc, s);
  std::multiset<std::string> codes;
  for (const fem::StudyProblem& p : problems) {
    INFO(p.code << ": " << p.message);
    codes.insert(p.code);
    // Each says something and offers a call or a note.
    CHECK_FALSE(p.message.empty());
    CHECK((!p.fixMethod.empty() || !p.fixNote.empty()));
  }
  CHECK(codes.count("no_support") == 1);
  CHECK(codes.count("load_geometry") == 1);
  CHECK(codes.count("anchor") == 1);
  CHECK(codes.count("material") == 1); // the fourth, unplanted on purpose
  CHECK(problems.size() == 4);
  for (const fem::StudyProblem& p : problems) {
    if (p.code == "no_support") {
      CHECK(p.fixMethod == "add_load");
      CHECK(p.fixParams.at("type") == "fixed");
      CHECK(p.fixParams.at("study") == s.id);
    }
    if (p.code == "anchor") {
      CHECK(p.loadId == gone.id);
      CHECK(p.fixMethod == "remove_load");
      CHECK(p.fixParams.at("id") == gone.id);
    }
    if (p.code == "load_geometry") {
      CHECK(p.loadId == torque.id);
      CHECK(p.hasFace);
    }
    if (p.code == "material")
      CHECK(p.fixMethod == "set_material");
  }
  // Read-only: the study is as it was.
  CHECK(doc.GetStudies()[0].error == before);
  CHECK(doc.GetStudies()[0].loads.size() == 3);

  // The same rules a run reads: it refuses this study too.
  fem::StudyPlan plan;
  std::string err;
  fem::Study run = s;
  CHECK_FALSE(fem::PlanStudy(doc, run, plan, &err));
}

TEST_CASE("check_study is empty for a study that runs, and finds a region "
          "off its face once there is a mesh",
          "[fem][check][mesher]") {
  Document doc;
  auto box =
      std::make_unique<BoxFeature>("Bar", glm::dvec3(0), 100.0, 10.0, 10.0);
  box->SetMaterial("steel");
  doc.AddFeature(std::move(box));
  const Body& body = doc.GetBodies()[0];
  fem::Study& s = doc.AddStudy("S");
  fem::Load fixed;
  fixed.id = doc.AllocateId();
  fixed.type = fem::LoadType::Fixed;
  fixed.face = FaceNear(body, {0, 5, 5});
  s.loads.push_back(fixed);
  fem::Load force;
  force.id = doc.AllocateId();
  force.type = fem::LoadType::Force;
  force.face = FaceNear(body, {50, 5, 10});
  force.magnitude = 100.0;
  force.direction = {0, 0, -1};
  s.loads.push_back(force);
  CHECK(fem::CheckStudy(doc, s).empty());

  // A sphere region nowhere near the face: nothing wrong until a mesh shows
  // it holds no facet.
  s.loads[1].region.kind = fem::LoadRegion::Kind::Sphere;
  s.loads[1].region.center = {50, 5, 500};
  s.loads[1].region.radiusMm = 1.0;
  CHECK(fem::CheckStudy(doc, s).empty());
  std::vector<fem::StudyBodyMesh> meshes;
  std::string err;
  fem::Study meshed = s;
  REQUIRE(fem::MeshStudy(doc, meshed, meshes, &err));
  std::vector<fem::CheckMesh> view;
  for (const fem::StudyBodyMesh& bm : meshes)
    view.push_back({bm.bodyIndex, &bm.mesh});
  const auto problems = fem::CheckStudy(doc, s, &view);
  REQUIRE(problems.size() == 1);
  CHECK(problems[0].code == "region");
  CHECK(problems[0].loadId == force.id);
}

#endif // HAVE_NETGEN
