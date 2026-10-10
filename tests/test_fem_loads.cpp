// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// Torque and remote loads, and a face's stress
// (docs/simulation/README.md#torque-and-remote-loads,
// docs/simulation/README.md#probing-a-face). Each is checked by its outcome:
// the twist and shear of a shaft against TL/GJ and Tr/J, the reactions a
// remote force leaves at the support, a face's stress against Mc/I.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "cad/CADKernel.h"
#include "cad/CADMaterials.h"
#include "core/Document.h"
#include "core/Feature.h"
#include "core/Measure.h"
#include "core/fem/FemMesher.h"
#include "core/fem/FemSolver.h"
#include "core/fem/FemStudy.h"

#include <BRepAlgoAPI_Fuse.hxx>
#include <TopExp_Explorer.hxx>

#include <array>
#include <cmath>
#include <numbers>
#include <string>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

std::vector<TopoDS_Shape> Faces(const TopoDS_Shape& s) {
  std::vector<TopoDS_Shape> out;
  for (TopExp_Explorer ex(s, TopAbs_FACE); ex.More(); ex.Next())
    out.push_back(ex.Current());
  return out;
}

} // namespace

// A torque needs an axis: a cylinder lends its own, a flat face does not and
// is refused; a remote load needs its point. The same check runs for an RPC,
// a GUI placement and a loaded file (CompleteLoadGeometry).
TEST_CASE("a torque takes a cylinder's axis and refuses a face without one",
          "[fem][loads]") {
  const CADShape shaft = CADKernel::Instance().CreateCylinder(
      glm::dvec3(0, 0, 0), glm::dvec3(0, 0, -1), 5.0, 40.0);
  int cylinders = 0, planes = 0;
  for (const TopoDS_Shape& f : Faces(shaft.GetShape())) {
    fem::Load torque;
    torque.id = 7;
    torque.type = fem::LoadType::Torque;
    torque.magnitude = 1000.0;
    std::string err;
    if (const auto axis = fem::CylindricalFaceAxis(f)) {
      ++cylinders;
      // Modelled along -Z; the axis is signed +Z, and sits on the shaft.
      CHECK_THAT(axis->direction.z, WithinAbs(1.0, 1e-12));
      CHECK_THAT(axis->origin.x, WithinAbs(0.0, 1e-9));
      CHECK_THAT(axis->origin.y, WithinAbs(0.0, 1e-9));
      CHECK_THAT(axis->origin.z, WithinAbs(-20.0, 1e-6)); // the face's middle
      REQUIRE(fem::CompleteLoadGeometry(f, torque, &err));
      CHECK(torque.HasAxis());
    } else {
      ++planes;
      CHECK_FALSE(fem::CompleteLoadGeometry(f, torque, &err));
      CHECK_THAT(err, ContainsSubstring("not cylindrical") &&
                          ContainsSubstring("axis_origin_mm"));
      // Given an axis, the same face takes it.
      torque.axisDirection = glm::dvec3(0, 0, 1);
      CHECK(fem::CompleteLoadGeometry(f, torque, &err));
    }
    fem::Load remote;
    remote.type = fem::LoadType::Remote;
    remote.magnitude = 10.0;
    CHECK_FALSE(fem::CompleteLoadGeometry(f, remote, &err));
    CHECK_THAT(err, ContainsSubstring("point_mm"));
    remote.hasPoint = true;
    CHECK(fem::CompleteLoadGeometry(f, remote, &err));
  }
  CHECK(cylinders == 1);
  CHECK(planes == 2);

  fem::LoadType t = fem::LoadType::Fixed;
  CHECK(fem::LoadTypeFromString("torque", t));
  CHECK(t == fem::LoadType::Torque);
  CHECK(fem::LoadTypeFromString("remote", t));
  CHECK(t == fem::LoadType::Remote);
  CHECK_FALSE(fem::LoadTypeFromString("moment", t));
}

#if defined(HAVE_NETGEN) && defined(HAVE_FEM_SOLVER)

#include "core/fem/FemSolverInternal.h"

namespace {

// Boundary triangles all of whose nodes satisfy `on`.
template <typename Fn>
std::vector<int> TrisWhere(const fem::FemMesh& m, Fn&& on) {
  std::vector<int> out;
  for (int t = 0; t < static_cast<int>(m.boundary.size()); ++t) {
    bool all = true;
    for (int n : m.boundary[t].nodes)
      all = all && on(m.nodes[n]);
    if (all)
      out.push_back(t);
  }
  return out;
}

std::vector<int> NodesWhere(const fem::FemMesh& m,
                            bool (*on)(const glm::dvec3&, double), double v) {
  std::vector<int> out;
  for (int i = 0; i < static_cast<int>(m.nodes.size()); ++i)
    if (on(m.nodes[i], v))
      out.push_back(i);
  return out;
}

// The consistent nodal loads of `loads` with every DOF free, per node.
std::vector<glm::dvec3> NodalLoads(const fem::FemMesh& mesh,
                                   const std::vector<fem::FaceLoad>& loads) {
  const fem::internal::DofMap map =
      fem::internal::BuildDofMap(static_cast<int>(mesh.nodes.size()), {});
  Eigen::VectorXd F = Eigen::VectorXd::Zero(map.nFree);
  std::string err;
  REQUIRE(fem::internal::AssembleFaceLoads(mesh, loads, map.map, F, &err));
  std::vector<glm::dvec3> out(mesh.nodes.size());
  for (size_t i = 0; i < out.size(); ++i)
    out[i] = glm::dvec3(F(3 * i), F(3 * i + 1), F(3 * i + 2));
  return out;
}

constexpr double kR = 5.0, kL = 40.0;
const fem::Material kSteel{200000.0, 0.3};

fem::FemMesh ShaftMesh() {
  const CADShape shaft = CADKernel::Instance().CreateCylinder(
      glm::dvec3(0, 0, 0), glm::dvec3(0, 0, 1), kR, kL);
  fem::MeshSettings ms;
  ms.maxSize = 2.0;
  std::string err;
  auto mesh = fem::Mesh(shaft.GetShape(), ms, &err);
  INFO(err);
  REQUIRE(mesh.has_value());
  return *mesh;
}

} // namespace

// The traction (T/J) a x (x - o) sums to exactly T about the axis -- the
// same Gauss points integrate J and the load -- and, on a face symmetric
// about it, to no net force.
TEST_CASE("a torque's nodal loads are T about the axis and no net force",
          "[fem][loads]") {
  const fem::FemMesh mesh = ShaftMesh();
  const double T = 2500.0;
  fem::FaceLoad load;
  load.type = fem::LoadType::Torque;
  load.magnitude = T;
  load.origin = glm::dvec3(0, 0, kL);
  load.axis = glm::dvec3(0, 0, 3); // need not be unit
  load.tris = TrisWhere(
      mesh, [](const glm::dvec3& p) { return std::abs(p.z - kL) < 1e-6; });
  REQUIRE_FALSE(load.tris.empty());
  const auto f = NodalLoads(mesh, {load});
  glm::dvec3 net(0.0), moment(0.0);
  for (size_t i = 0; i < f.size(); ++i) {
    net += f[i];
    moment += glm::cross(mesh.nodes[i] - load.origin, f[i]);
  }
  CHECK_THAT(moment.z, WithinRel(T, 1e-9));
  CHECK(std::abs(moment.x) < 1e-9 * T);
  CHECK(std::abs(moment.y) < 1e-9 * T);
  // The mesh of a disc is not exactly symmetric, so the net force is
  // T/R times how far its centroid strays from the axis: small.
  CHECK(glm::length(net) < 1e-3 * T / kR);
}

// A shaft clamped at one end and twisted at the other turns by TL/GJ, and
// its surface carries the shear Tr/J -- von Mises sqrt(3) times it.
TEST_CASE("a torque on a shaft's end twists it by TL/GJ with shear Tr/J",
          "[fem][loads][solver]") {
  const fem::FemMesh mesh = ShaftMesh();
  const double T = 2000.0; // N*mm
  fem::FaceLoad load;
  load.type = fem::LoadType::Torque;
  load.magnitude = T;
  load.origin = glm::dvec3(0, 0, kL);
  load.axis = glm::dvec3(0, 0, 1);
  load.tris = TrisWhere(
      mesh, [](const glm::dvec3& p) { return std::abs(p.z - kL) < 1e-6; });
  const auto fixed = NodesWhere(
      mesh, [](const glm::dvec3& p, double) { return std::abs(p.z) < 1e-6; },
      0.0);
  std::string err;
  const auto res = fem::Solve(mesh, kSteel, fixed, {load}, &err);
  INFO(err);
  REQUIRE(res.has_value());

  const double G = kSteel.youngMPa / (2.0 * (1.0 + kSteel.poisson));
  const double J = std::numbers::pi * std::pow(kR, 4) / 2.0;
  const int endFace = mesh.boundary[load.tris.front()].faceIndex;
  const auto fm = fem::FitFaceMotion(mesh, res->displacement, endFace);
  REQUIRE(fm.has_value());
  const double twist = T * kL / (G * J);
  INFO("end twist " << fm->rotation.z << " rad, TL/GJ " << twist);
  CHECK_THAT(fm->rotation.z, WithinRel(twist, 0.03));
  CHECK(std::abs(fm->rotation.x) < 0.01 * twist);
  CHECK(std::abs(fm->rotation.y) < 0.01 * twist);

  // The surface away from both ends, averaged over its nodes.
  double sum = 0.0;
  int n = 0;
  for (size_t i = 0; i < mesh.nodes.size(); ++i) {
    const glm::dvec3& p = mesh.nodes[i];
    if (std::abs(std::hypot(p.x, p.y) - kR) < 1e-3 &&
        std::abs(p.z - kL / 2) < 8.0) {
      sum += res->vonMises[i];
      ++n;
    }
  }
  REQUIRE(n > 10);
  const double vm = std::sqrt(3.0) * T * kR / J;
  INFO("surface von Mises " << sum / n << " (" << n << " nodes), sqrt(3) Tr/J "
                            << vm);
  CHECK_THAT(sum / n, WithinRel(vm, 0.05));
}

// A force at a point off the face: its nodal loads are F and (p - c) x F,
// and the support answers with exactly the opposite, which the reactions
// (K u at the clamped nodes) must show.
TEST_CASE("a remote load is its force and moment, and the support answers "
          "them",
          "[fem][loads][solver]") {
  const double L = 100, b = 10, h = 10;
  const CADShape beam = CADKernel::Instance().CreateBox(L, b, h);
  fem::MeshSettings ms;
  ms.maxSize = 4.0;
  std::string err;
  auto mesh = fem::Mesh(beam.GetShape(), ms, &err);
  INFO(err);
  REQUIRE(mesh.has_value());

  fem::FaceLoad load;
  load.type = fem::LoadType::Remote;
  load.magnitude = 300.0;
  load.direction = glm::dvec3(0, 1, -2); // need not be unit
  const glm::dvec3 p(L + 40.0, 25.0, 5.0);
  load.origin = p;
  load.tris = TrisWhere(
      *mesh, [&](const glm::dvec3& q) { return std::abs(q.x - L) < 1e-6; });
  REQUIRE_FALSE(load.tris.empty());
  const glm::dvec3 F = load.magnitude * glm::normalize(load.direction);
  const glm::dvec3 c(L, b / 2, h / 2);
  const glm::dvec3 M = glm::cross(p - c, F);

  const auto f = NodalLoads(*mesh, {load});
  glm::dvec3 net(0.0), moment(0.0);
  for (size_t i = 0; i < f.size(); ++i) {
    net += f[i];
    moment += glm::cross(mesh->nodes[i] - c, f[i]);
  }
  CHECK(glm::length(net - F) < 1e-9 * glm::length(F));
  CHECK(glm::length(moment - M) < 1e-9 * glm::length(M));

  const auto fixed = NodesWhere(
      *mesh, [](const glm::dvec3& q, double) { return std::abs(q.x) < 1e-6; },
      0.0);
  const auto res = fem::Solve(*mesh, kSteel, fixed, {load}, &err);
  INFO(err);
  REQUIRE(res.has_value());
  // Reactions: the internal force K u at the clamped nodes, element by
  // element.
  std::vector<char> isFixed(mesh->nodes.size(), 0);
  for (int i : fixed)
    isFixed[i] = 1;
  glm::dvec3 R(0.0), RM(0.0);
  for (const auto& tet : mesh->tets) {
    std::array<glm::dvec3, 10> X;
    for (int k = 0; k < 10; ++k)
      X[k] = mesh->nodes[tet[k]];
    const auto K = fem::Tet10Stiffness(X, kSteel.youngMPa, kSteel.poisson);
    for (int a = 0; a < 10; ++a) {
      if (!isFixed[tet[a]])
        continue;
      glm::dvec3 fa(0.0);
      for (int i = 0; i < 3; ++i)
        for (int bn = 0; bn < 10; ++bn)
          for (int j = 0; j < 3; ++j)
            fa[i] += K[(3 * a + i) * 30 + 3 * bn + j] *
                     res->displacement[tet[bn]][j];
      R += fa;
      RM += glm::cross(mesh->nodes[tet[a]] - c, fa);
    }
  }
  INFO("reaction " << R.x << " " << R.y << " " << R.z << ", moment " << RM.x
                   << " " << RM.y << " " << RM.z);
  // K u = f: what the support pushes back is -(F, M) about the same point.
  CHECK(glm::length(R + F) < 1e-6 * glm::length(F));
  CHECK(glm::length(RM + M) < 1e-6 * glm::length(M));
}

// A face's stress is read where it is. The beam is two boxes fused, so its
// top is two faces split at mid-span: the outer one's peak is the bending
// stress M c / I at x = L/2, away from the clamp's corners and the load;
// the free end, which the global peak is not on, reads less than it.
TEST_CASE("a face's probed stress is Mc/I where the face begins",
          "[fem][loads][probe]") {
  const double L = 100, b = 10, h = 10;
  const TopoDS_Shape near =
      CADKernel::Instance().CreateBox(L / 2, b, h).GetShape();
  const TopoDS_Shape far =
      CADKernel::Instance()
          .CreateBox(glm::dvec3(L / 2, 0, 0), glm::dvec3(L, b, h))
          .GetShape();
  BRepAlgoAPI_Fuse fuse(near, far);
  REQUIRE(fuse.IsDone());
  fem::MeshSettings ms;
  ms.maxSize = 3.0;
  std::string err;
  auto mesh = fem::Mesh(fuse.Shape(), ms, &err);
  INFO(err);
  REQUIRE(mesh.has_value());
  const double P = 1000.0;
  fem::FaceLoad load;
  load.type = fem::LoadType::Force;
  load.magnitude = P;
  load.direction = glm::dvec3(0, 0, -1);
  load.tris = TrisWhere(
      *mesh, [&](const glm::dvec3& q) { return std::abs(q.x - L) < 1e-6; });
  const auto topFar = TrisWhere(*mesh, [&](const glm::dvec3& q) {
    return std::abs(q.z - h) < 1e-6 && q.x > L / 2 - 1e-6;
  });
  REQUIRE_FALSE(topFar.empty());
  const auto fixed = NodesWhere(
      *mesh, [](const glm::dvec3& q, double) { return std::abs(q.x) < 1e-6; },
      0.0);
  const auto res = fem::Solve(*mesh, kSteel, fixed, {load}, &err);
  INFO(err);
  REQUIRE(res.has_value());

  const int face = mesh->boundary[topFar.front()].faceIndex;
  for (int t : topFar) // one face, the split held
    REQUIRE(mesh->boundary[t].faceIndex == face);
  const int tipFace = mesh->boundary[load.tris.front()].faceIndex;
  const auto top = fem::FaceStressOf(*mesh, res->vonMises, face);
  const auto tip = fem::FaceStressOf(*mesh, res->vonMises, tipFace);
  REQUIRE(top.has_value());
  REQUIRE(tip.has_value());
  const double I = b * h * h * h / 12.0;
  const double sigma = P * (L / 2) * (h / 2) / I; // 300 MPa at x = L/2
  INFO("top max " << top->maxMPa << " at x " << top->maxAt.x << ", mean "
                  << top->meanMPa << ", tip max " << tip->maxMPa << ", global "
                  << res->maxVonMises << ", Mc/I " << sigma);
  CHECK_THAT(top->maxMPa, WithinRel(sigma, 0.12));
  CHECK_THAT(top->maxAt.x, WithinAbs(L / 2, 1e-6));
  // The mean of a stress falling linearly to ~0 at the tip: about half.
  CHECK_THAT(top->meanMPa, WithinRel(sigma / 2, 0.15));
  CHECK_THAT(top->area, WithinRel(L / 2 * b, 1e-9));
  CHECK(top->maxMPa < res->maxVonMises);
  CHECK(tip->maxMPa < res->maxVonMises);
  CHECK(tip->meanMPa < res->maxVonMises);
  CHECK_FALSE(fem::FaceStressOf(*mesh, res->vonMises, 999).has_value());
}

// The default axis through the whole pipeline: a torque on a shaft's
// CYLINDRICAL face, with no axis given, turns about the shaft. Spread
// evenly along the length, it twists the free end by TL/2GJ.
TEST_CASE("a torque on a shaft's cylindrical face turns it about its own axis",
          "[fem][loads][run]") {
  Document doc;
  auto cyl = std::make_unique<CylinderFeature>("Shaft", glm::dvec3(0, 0, 0),
                                               glm::dvec3(0, 0, 1), kR, kL);
  cyl->SetMaterial("steel");
  doc.AddFeature(std::move(cyl));
  REQUIRE(doc.GetBodies().size() == 1);
  const Body& body = doc.GetBodies()[0];
  ElementRef side, bottom, top;
  for (const auto& [index, name] : body.elementMap.names) {
    const auto info = Measure::DescribeFaceAt(body, index);
    REQUIRE(info.has_value());
    const ElementRef ref{body.createdBy, name};
    if (info->kind == "cylinder")
      side = ref;
    else if (info->centroid.z < kL / 2)
      bottom = ref;
    else
      top = ref;
  }
  REQUIRE_FALSE(side.name.empty());
  REQUIRE_FALSE(bottom.name.empty());
  REQUIRE_FALSE(top.name.empty());
  REQUIRE(fem::CylindricalFaceAxis(doc, side).has_value());
  CHECK_FALSE(fem::CylindricalFaceAxis(doc, top).has_value());

  fem::Study& s = doc.AddStudy("Twist");
  s.meshSize = 2.0;
  fem::Load fx;
  fx.id = doc.AllocateId();
  fx.type = fem::LoadType::Fixed;
  fx.face = bottom;
  s.loads.push_back(fx);
  fem::Load tq;
  tq.id = doc.AllocateId();
  tq.type = fem::LoadType::Torque;
  tq.face = side;
  tq.magnitude = 2000.0;
  s.loads.push_back(tq);
  fem::StudyRuntime rt;
  std::string err;
  REQUIRE(fem::RunStudy(doc, s, rt, &err));
  INFO(err);
  REQUIRE(rt.bodies.size() == 1);

  fem::ResolvedFace rf;
  REQUIRE(fem::ResolveFace(doc, top, "top", rf, &err));
  const auto fm = fem::FitFaceMotion(
      rt.bodies[0].mesh, rt.bodies[0].results.displacement, rf.faceIndex);
  REQUIRE(fm.has_value());
  const auto& mp = CADMaterials::MaterialLibrary::GetMaterial(
      CADMaterials::MaterialLibrary::MaterialTypeFromString("steel"));
  const double E = mp.youngGPa * 1000.0;
  const double G = E / (2.0 * (1.0 + mp.poisson));
  const double J = std::numbers::pi * std::pow(kR, 4) / 2.0;
  const double twist = tq.magnitude * kL / (2.0 * G * J);
  INFO("end twist " << fm->rotation.z << ", TL/2GJ " << twist);
  CHECK_THAT(fm->rotation.z, WithinRel(twist, 0.05));

  // The same load on the flat end, with no axis, is refused by the run.
  s.loads[1].face = top;
  CHECK_FALSE(fem::RunStudy(doc, s, rt, &err));
  CHECK_THAT(err, ContainsSubstring("not cylindrical"));
}

#endif // HAVE_NETGEN && HAVE_FEM_SOLVER
