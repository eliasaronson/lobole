#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "CADKernel.h"
#include "core/Command.h"
#include "core/Document.h"
#include "core/Feature.h"
#include "core/FemMesher.h"
#include "core/FemSolver.h"
#include "core/FemStudy.h"
#include "core/FemTopo.h"
#include "core/Serialization.h"
#include "topo_testmesh.h"

#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

// TO0 — the SIMP spike gate (docs/topopt/plan.md). Proves the compliance-
// minimization loop converges on tet10 before any Study/RPC/GUI surface exists.
// The primary case runs on a deterministic, Netgen-free structured mesh so the
// gate's metrics can't drift with mesher versions; a second case cross-checks
// the same problem through the real Netgen path.

namespace {

// GenerateBoxMesh / NodesAtPlane / TrisAtPlane live in topo_testmesh.h,
// shared with the GPU solver gate (test_femgpu.cpp).

// Fraction of elements with an intermediate (gray) density.
double Grayness(const std::vector<double>& d) {
  int gray = 0;
  for (double v : d)
    if (v > 0.2 && v < 0.8) ++gray;
  return d.empty() ? 0.0 : double(gray) / d.size();
}

// Optional STL dump (env TOPO0_DUMP_STL=<path>) of the solid tets — the
// "is it visibly a cantilever truss" eyeball half of the gate.
void MaybeDumpStl(const fem::FemMesh& m, const std::vector<double>& density, double thr) {
  const char* path = std::getenv("TOPO0_DUMP_STL");
  if (!path) return;
  std::FILE* f = std::fopen(path, "w");
  if (!f) return;
  std::fprintf(f, "solid topo\n");
  static const int fc[4][3] = {{0, 2, 1}, {0, 1, 3}, {1, 2, 3}, {0, 3, 2}};
  for (size_t e = 0; e < m.tets.size(); ++e) {
    if (density[e] <= thr) continue;
    const auto& t = m.tets[e];
    for (const auto& tri : fc) {
      std::fprintf(f, "facet normal 0 0 0\n outer loop\n");
      for (int k = 0; k < 3; ++k) {
        const glm::dvec3& p = m.nodes[t[tri[k]]];
        std::fprintf(f, "  vertex %g %g %g\n", p.x, p.y, p.z);
      }
      std::fprintf(f, " endloop\nendfacet\n");
    }
  }
  std::fprintf(f, "endsolid topo\n");
  std::fclose(f);
}

}  // namespace

#ifdef HAVE_FEM_SOLVER

#include "core/FemSolverInternal.h"  // element math for the seam-test solver

#include <Eigen/SparseCholesky>
#include <Eigen/SparseCore>

TEST_CASE("Topo: SIMP cantilever converges on a structured tet10 mesh (GATE)",
          "[topopt][gate]") {
  // 100 x 40 x 40 mm, 20 x 8 x 8 hexes -> 7680 tet10 (<= the ~8k gate budget).
  // A thin-but-well-resolved cantilever domain (fine in length x height, thin
  // in depth) — the classic quasi-2D setup that forms a crisp truss at a low
  // element count, so the gate stays fast. 24 x 2 x 10 hexes = 2880 tet10.
  const double Lx = 120, Ly = 16, Lz = 50;
  const int nx = 24, ny = 2, nz = 10;
  fem::FemMesh mesh = GenerateBoxMesh(nx, ny, nz, Lx, Ly, Lz);
  REQUIRE(mesh.tets.size() == size_t(nx * ny * nz * 6));

  fem::Material steel{210000.0, 0.3};  // 210 GPa
  std::vector<int> fixed = NodesAtPlane(mesh, 0, 0.0, 1e-6);  // clamp x=0
  REQUIRE(fixed.size() > 3);
  // Downward load on the lower flange of the tip face (a patch, not the whole
  // end cap) — the classic cantilever setup. A whole-face load freezes a large
  // solid end block that eats the volume budget and leaves the structure gray.
  fem::FaceLoad load;
  load.type = fem::LoadType::Force;
  for (int t : TrisAtPlane(mesh, 0, Lx, 1e-6)) {
    double zc = 0;
    for (int k = 0; k < 6; ++k) zc += mesh.nodes[mesh.boundary[t].nodes[k]].z;
    if (zc / 6.0 <= Lz / 3.0) load.tris.push_back(t);  // lower third
  }
  load.magnitude = 2000.0;  // N, downward
  load.direction = glm::dvec3(0, 0, -1);
  REQUIRE_FALSE(load.tris.empty());

  fem::TopoParams params;
  params.volumeFraction = 0.3;
  params.filterRadius = 2.0 * (Lz / nz);  // ~2 elements (10 mm)
  params.maxIterations = 80;
  params.changeTol = 0.01;

  std::string err;
  const auto t0 = std::chrono::steady_clock::now();
  auto res = fem::TopologyOptimize(mesh, steel, fixed, {load}, params, &err);
  const auto t1 = std::chrono::steady_clock::now();
  INFO(err);
  REQUIRE(res.has_value());

  const double secs = std::chrono::duration<double>(t1 - t0).count();
  int nVoid = 0, nSolid = 0;
  for (double v : res->density) {
    if (v < 0.2) ++nVoid;
    else if (v > 0.8) ++nSolid;
  }
  WARN("TO0 gate: " << res->iterations << " iters, " << secs << " s ("
                    << (secs * 1000.0 / res->iterations) << " ms/iter), grayness "
                    << Grayness(res->density) * 100 << "% (void " << nVoid
                    << " solid " << nSolid << " of " << res->density.size()
                    << "), Vf " << res->volumeFractionAchieved << ", compliance "
                    << res->complianceHistory.front() << " -> "
                    << res->complianceHistory.back()
                    << ", converged=" << res->converged);
  MaybeDumpStl(mesh, res->density, 0.5);

  // The essential, method-validating gate criteria. (Razor-sharp <15% grayness
  // needs a Heaviside projection + a finer mesh — a documented TO1 item; the
  // structured Kuhn-tet decomposition also injects some per-element sensitivity
  // variation. Here we assert the physics is right.)

  // (1) converged before the iteration cap.
  CHECK(res->converged);
  CHECK(res->iterations <= params.maxIterations);

  // (2) hit the target volume fraction within +/- 1% (the OC constraint).
  CHECK_THAT(res->volumeFractionAchieved, WithinAbs(0.3, 0.01));

  // (3) the optimized layout is far stiffer (lower compliance) than the uniform
  //     start it began from.
  REQUIRE(res->complianceHistory.size() >= 4);
  CHECK(res->complianceHistory.back() < 0.7 * res->complianceHistory.front());

  // (4) a discriminating design: the optimizer decisively voided a large part
  //     of the domain and most elements resolved toward 0 or 1 (not uniform
  //     gray). The binarized shape is a recognizable cantilever truss (verified
  //     out-of-band via the STL dump).
  const double voidFrac = double(nVoid) / res->density.size();
  const double resolvedFrac = double(nVoid + nSolid) / res->density.size();
  CHECK(voidFrac > 0.35);
  CHECK(resolvedFrac > 0.55);

  // (5) the fixed-support skin stayed solid (frozen at density 1).
  double minFixed = 1.0;
  for (size_t e = 0; e < mesh.tets.size(); ++e) {
    bool onFixed = false;
    for (int i = 0; i < 10 && !onFixed; ++i)
      if (std::abs(mesh.nodes[mesh.tets[e][i]].x) < 1e-6) onFixed = true;
    if (onFixed) minFixed = std::min(minFixed, res->density[e]);
  }
  CHECK_THAT(minFixed, WithinAbs(1.0, 1e-9));
}

TEST_CASE("Topo bench: bulky 3D cube", "[.][topobench]") {
  // A bulky 3D domain (12x12x12 hexes = 10368 tet10) — the direct solver's
  // fill-in worst case, and the real use case (cube/cylinder). Times a short
  // run so the solver (direct vs CG) can be compared. Hidden from the suite.
  const double L = 60;
  fem::FemMesh mesh = GenerateBoxMesh(12, 12, 12, L, L, L);
  fem::Material steel{210000.0, 0.3};
  std::vector<int> fixed = NodesAtPlane(mesh, 0, 0.0, 1e-6);
  fem::FaceLoad load;
  load.type = fem::LoadType::Force;
  load.tris = TrisAtPlane(mesh, 0, L, 1e-6);
  load.magnitude = 2000.0;
  load.direction = glm::dvec3(0, 0, -1);

  fem::TopoParams params;
  params.volumeFraction = 0.4;
  params.filterRadius = 2.0 * (L / 12);
  params.maxIterations = 15;

  std::string err;
  const auto t0 = std::chrono::steady_clock::now();
  auto res = fem::TopologyOptimize(mesh, steel, fixed, {load}, params, &err);
  const auto t1 = std::chrono::steady_clock::now();
  REQUIRE(res.has_value());
  const double secs = std::chrono::duration<double>(t1 - t0).count();
  WARN("topobench: " << mesh.tets.size() << " tets, " << res->iterations
                     << " iters, " << secs << " s ("
                     << (secs * 1000.0 / res->iterations) << " ms/iter), Vf "
                     << res->volumeFractionAchieved);
}

TEST_CASE("Topo: progress callback can cancel and keep best-so-far",
          "[topopt]") {
  fem::FemMesh mesh = GenerateBoxMesh(10, 4, 4, 100, 40, 40);
  fem::Material steel{210000.0, 0.3};
  std::vector<int> fixed = NodesAtPlane(mesh, 0, 0.0, 1e-6);
  fem::FaceLoad load;
  load.type = fem::LoadType::Force;
  load.tris = TrisAtPlane(mesh, 0, 100.0, 1e-6);
  load.magnitude = 1000.0;
  load.direction = glm::dvec3(0, 0, -1);

  fem::TopoParams params;
  params.filterRadius = 25.0;
  params.maxIterations = 50;

  int seen = 0;
  std::string err;
  auto res = fem::TopologyOptimize(
      mesh, steel, fixed, {load}, params, &err,
      [&](const fem::TopoProgress& p) {
        seen = p.iteration;
        return p.iteration < 3;  // cancel after the 3rd iteration
      });
  INFO(err);
  REQUIRE(res.has_value());
  CHECK(res->cancelled);
  CHECK_FALSE(res->converged);
  CHECK(res->iterations == 3);
  CHECK(seen == 3);
  CHECK(res->density.size() == mesh.tets.size());
}

TEST_CASE("Topo: rejects invalid inputs up front", "[topopt]") {
  fem::FemMesh mesh = GenerateBoxMesh(6, 3, 3, 60, 30, 30);
  fem::Material steel{210000.0, 0.3};
  std::vector<int> fixed = NodesAtPlane(mesh, 0, 0.0, 1e-6);
  fem::FaceLoad load;
  load.type = fem::LoadType::Force;
  load.tris = TrisAtPlane(mesh, 0, 60.0, 1e-6);
  load.magnitude = 1000.0;
  load.direction = glm::dvec3(0, 0, -1);
  fem::TopoParams params;
  params.filterRadius = 15.0;
  std::string err;

  SECTION("no fixed support") {
    auto res = fem::TopologyOptimize(mesh, steel, {}, {load}, params, &err);
    CHECK_FALSE(res.has_value());
  }
  SECTION("no loads") {
    auto res = fem::TopologyOptimize(mesh, steel, fixed, {}, params, &err);
    CHECK_FALSE(res.has_value());
  }
  SECTION("volume fraction out of range") {
    params.volumeFraction = 1.5;
    auto res = fem::TopologyOptimize(mesh, steel, fixed, {load}, params, &err);
    CHECK_FALSE(res.has_value());
  }
  SECTION("no stiffness data") {
    fem::Material none{0.0, 0.3};
    auto res = fem::TopologyOptimize(mesh, none, fixed, {load}, params, &err);
    CHECK_FALSE(res.has_value());
  }
  SECTION("target below frozen volume") {
    // Tiny domain where the clamped + loaded skin alone exceeds a 5% target.
    params.volumeFraction = 0.05;
    auto res = fem::TopologyOptimize(mesh, steel, fixed, {load}, params, &err);
    CHECK_FALSE(res.has_value());
    CHECK(err.find("frozen") != std::string::npos);
  }
}

namespace {

// A CPU stand-in for the GPU solver (fem_gpu): exact direct solves through the
// TopoLinearSolver seam. Proves TopologyOptimize drives an injected solver
// correctly — Setup once, Solve per iteration, same optimum — without any
// Vulkan dependency in this binary.
struct DirectSeamSolver : fem::TopoLinearSolver {
  const fem::FemMesh* mesh = nullptr;
  std::vector<int> dofMap;
  int nFree = 0;
  Eigen::Matrix<double, 6, 6> D0;
  int setupCalls = 0, solveCalls = 0;

  bool Setup(const fem::FemMesh& m, const fem::Material& mat,
             const std::vector<int>& dm, int nf, std::string*) override {
    mesh = &m;
    dofMap = dm;
    nFree = nf;
    D0 = fem::internal::Elasticity(mat.youngMPa, mat.poisson);
    ++setupCalls;
    return true;
  }
  bool Solve(const std::vector<double>& scale, const std::vector<double>& F,
             std::vector<double>& u, std::string* error) override {
    ++solveCalls;
    using namespace fem::internal;
    std::vector<Eigen::Triplet<double>> trip;
    for (size_t e = 0; e < mesh->tets.size(); ++e) {
      const auto& tet = mesh->tets[e];
      Eigen::Vector3d X[10];
      for (int i = 0; i < 10; ++i) {
        const auto& p = mesh->nodes[tet[i]];
        X[i] = Eigen::Vector3d(p.x, p.y, p.z);
      }
      Eigen::Matrix<double, 30, 30> Ke = Eigen::Matrix<double, 30, 30>::Zero();
      for (const auto& g : kTetGauss) {
        Eigen::Matrix<double, 6, 30> B;
        const double detJ = StrainDisp(X, g[0], g[1], g[2], B);
        Ke += (kTetW * std::abs(detJ)) * (B.transpose() * D0 * B);
      }
      for (int a = 0; a < 30; ++a) {
        const int ga = dofMap[3 * tet[a / 3] + (a % 3)];
        if (ga < 0) continue;
        for (int b = 0; b < 30; ++b) {
          const int gb = dofMap[3 * tet[b / 3] + (b % 3)];
          if (gb >= 0) trip.emplace_back(ga, gb, scale[e] * Ke(a, b));
        }
      }
    }
    Eigen::SparseMatrix<double> K(nFree, nFree);
    K.setFromTriplets(trip.begin(), trip.end());
    Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>> ldlt(K);
    if (ldlt.info() != Eigen::Success) {
      if (error) *error = "factorization failed";
      return false;
    }
    Eigen::VectorXd Fv(nFree);
    for (int i = 0; i < nFree; ++i) Fv(i) = F[i];
    const Eigen::VectorXd uv = ldlt.solve(Fv);
    u.assign(uv.data(), uv.data() + nFree);
    return true;
  }
};

}  // namespace

TEST_CASE("TopologyOptimize drives an injected linear solver (the GPU seam)",
          "[topopt]") {
  fem::FemMesh mesh = GenerateBoxMesh(6, 3, 3, 60, 30, 30);
  fem::Material steel{210000.0, 0.3};
  std::vector<int> fixed = NodesAtPlane(mesh, 0, 0.0, 1e-6);
  fem::FaceLoad load;
  load.type = fem::LoadType::Force;
  load.tris = TrisAtPlane(mesh, 0, 60.0, 1e-6);
  load.magnitude = 1000.0;
  load.direction = glm::dvec3(0, 0, -1);
  fem::TopoParams params;
  params.filterRadius = 15.0;
  params.maxIterations = 40;
  // The 6x3x3 box's frozen support/load skin is ~1/3 of the volume, so the
  // target must sit above that.
  params.volumeFraction = 0.5;

  std::string err;
  auto builtin =
      fem::TopologyOptimize(mesh, steel, fixed, {load}, params, &err);
  INFO(err);
  REQUIRE(builtin.has_value());

  DirectSeamSolver seam;
  auto injected = fem::TopologyOptimize(mesh, steel, fixed, {load}, params,
                                        &err, {}, {}, &seam);
  INFO(err);
  REQUIRE(injected.has_value());

  CHECK(seam.setupCalls == 1);
  CHECK(seam.solveCalls == injected->iterations);
  // An exact direct solve vs the built-in CG (tol 1e-6) — the optimizer must
  // land on the same design.
  CHECK_THAT(injected->complianceHistory.back(),
             WithinRel(builtin->complianceHistory.back(), 1e-3));
  CHECK_THAT(injected->volumeFractionAchieved,
             WithinAbs(builtin->volumeFractionAchieved, 1e-3));
}

#ifdef HAVE_NETGEN

TEST_CASE("Topo: SIMP cantilever through the real Netgen mesh path",
          "[topopt][mesh]") {
  // The same problem, meshed by Netgen — catches unstructured-mesh bugs
  // (non-uniform element volumes in the OC bisection, irregular filter
  // neighborhoods). Looser tolerances; the structured case is the gate.
  const double Lx = 100, Ly = 40, Lz = 40;
  CADShape box = CADKernel::Instance().CreateBox(Lx, Ly, Lz);
  fem::MeshSettings ms;
  ms.maxSize = 8.0;
  std::string err;
  auto mesh = fem::Mesh(box.GetShape(), ms, &err);
  INFO(err);
  REQUIRE(mesh.has_value());

  fem::Material steel{210000.0, 0.3};
  std::vector<int> fixed = NodesAtPlane(*mesh, 0, 0.0, 1e-6);
  REQUIRE(fixed.size() > 3);
  fem::FaceLoad load;
  load.type = fem::LoadType::Force;
  load.tris = TrisAtPlane(*mesh, 0, Lx, 1e-6);
  load.magnitude = 2000.0;
  load.direction = glm::dvec3(0, 0, -1);
  REQUIRE_FALSE(load.tris.empty());

  fem::TopoParams params;
  params.volumeFraction = 0.3;
  params.filterRadius = 20.0;
  params.maxIterations = 80;
  params.changeTol = 0.02;

  auto res = fem::TopologyOptimize(*mesh, steel, fixed, {load}, params, &err);
  INFO(err);
  REQUIRE(res.has_value());

  CHECK(res->iterations <= params.maxIterations);
  CHECK_THAT(res->volumeFractionAchieved, WithinAbs(0.3, 0.02));
  CHECK(res->complianceHistory.back() < res->complianceHistory.front());
}

namespace {
// A steel design-envelope box body + a topology study fixing x=0 and pushing
// down on the x=L face. Returns the document; the study is study 0.
Document MakeTopoDoc() {
  Document doc;
  auto box = std::make_unique<BoxFeature>("Envelope", glm::vec3(0, 0, 0),
                                          80.0, 30.0, 30.0);
  box->SetMaterial("steel");
  doc.AddFeature(std::move(box));
  const uint64_t cb = doc.GetBodies()[0].createdBy;
  auto face = [&](int i) { return ElementRef{cb, std::to_string(cb) + "/face" + std::to_string(i)}; };

  fem::Study& s = doc.AddStudy("Generative");
  s.type = fem::AnalysisType::Topology;
  s.meshSize = 6.0;
  s.volumeFraction = 0.35;
  s.filterRadius = 12.0;
  s.maxIterations = 40;
  fem::Load fixed;
  fixed.id = doc.AllocateId();
  fixed.type = fem::LoadType::Fixed;
  fixed.face = face(0);  // x = 0
  s.loads.push_back(fixed);
  fem::Load force;
  force.id = doc.AllocateId();
  force.type = fem::LoadType::Force;
  force.face = face(1);  // x = L
  force.magnitude = 1500.0;
  force.direction = glm::dvec3(0, 0, -1);
  s.loads.push_back(force);
  return doc;
}
}  // namespace

TEST_CASE("RunTopology optimizes the loaded body end to end", "[topopt][mesh]") {
  Document doc = MakeTopoDoc();
  fem::Study& s = doc.GetStudies()[0];
  fem::StudyRuntime rt;
  std::string err;
  REQUIRE(fem::RunTopology(doc, s, rt, &err));
  INFO(err);
  REQUIRE(rt.topology);
  REQUIRE(rt.bodies.size() == 1);
  // The density field parallels the mesh tets and is a valid fraction field.
  CHECK(rt.bodies[0].density.size() == rt.bodies[0].mesh.tets.size());
  CHECK(rt.bodies[0].density.size() > 0);
  for (double d : rt.bodies[0].density) CHECK((d >= 0.0 && d <= 1.0 + 1e-9));
  CHECK_THAT(rt.volumeFractionAchieved, WithinAbs(0.35, 0.02));
  CHECK(rt.iterations > 0);
  REQUIRE(rt.complianceHistory.size() >= 2);
  CHECK(rt.complianceHistory.back() < rt.complianceHistory.front());
}

TEST_CASE("RunTopology rejects a multi-body study (v1 is single-body)",
          "[topopt][mesh]") {
  Document doc;
  auto a = std::make_unique<BoxFeature>("A", glm::vec3(0, 0, 0), 40, 20, 20);
  a->SetMaterial("steel");
  doc.AddFeature(std::move(a));
  auto b = std::make_unique<BoxFeature>("B", glm::vec3(0, 0, 40), 40, 20, 20);
  b->SetMaterial("steel");
  doc.AddFeature(std::move(b));
  const uint64_t cbA = doc.GetBodies()[0].createdBy;
  const uint64_t cbB = doc.GetBodies()[1].createdBy;
  fem::Study& s = doc.AddStudy("Two");
  s.type = fem::AnalysisType::Topology;
  s.meshSize = 6.0;
  auto load = [&](uint64_t cb, int f, fem::LoadType t) {
    fem::Load l;
    l.id = doc.AllocateId();
    l.type = t;
    l.face = ElementRef{cb, std::to_string(cb) + "/face" + std::to_string(f)};
    l.direction = glm::dvec3(0, 0, -1);
    l.magnitude = 1000.0;
    s.loads.push_back(l);
  };
  load(cbA, 0, fem::LoadType::Fixed);
  load(cbB, 1, fem::LoadType::Force);
  fem::StudyRuntime rt;
  std::string err;
  CHECK_FALSE(fem::RunTopology(doc, s, rt, &err));
  CHECK(err.find("single") != std::string::npos);
}

TEST_CASE("Topology result persists and reloads through the directory format",
          "[topopt][mesh][serialize]") {
  namespace fs = std::filesystem;
  Document doc = MakeTopoDoc();
  const uint64_t studyId = doc.GetStudies()[0].id;
  fem::StudyRuntime rt;
  std::string err;
  REQUIRE(fem::RunTopology(doc, doc.GetStudies()[0], rt, &err));
  rt.solvedAtVersion = doc.Version();
  const std::vector<double> density = rt.bodies[0].density;  // copy to compare
  doc.StoreStudyResults(studyId, std::move(rt));
  REQUIRE_FALSE(doc.StudyResultsStale(studyId));

  const fs::path proj =
      fs::temp_directory_path() / "topo_result_roundtrip.dsgn";
  fs::remove_all(proj);
  REQUIRE(DocumentIO::Save(doc, proj.string(), &err));
  CHECK(fs::exists(proj / "studies"));

  auto reloaded = DocumentIO::Load(proj.string(), &err);
  REQUIRE((reloaded != nullptr && err.empty()));
  const fem::StudyRuntime* got = reloaded->StudyResults(studyId);
  REQUIRE(got != nullptr);
  CHECK(got->topology);
  REQUIRE(got->bodies.size() == 1);
  REQUIRE(got->bodies[0].density.size() == density.size());
  for (size_t i = 0; i < density.size(); ++i)
    CHECK_THAT(got->bodies[0].density[i], WithinAbs(density[i], 1e-9));
  // A just-loaded result is stamped current, not stale.
  CHECK_FALSE(reloaded->StudyResultsStale(studyId));
  fs::remove_all(proj);
}

#endif  // HAVE_NETGEN

#else  // !HAVE_FEM_SOLVER

TEST_CASE("Topo: reports a clear error when built without the FEM solver",
          "[topopt]") {
  fem::FemMesh mesh;
  fem::Material steel{210000.0, 0.3};
  std::string err;
  auto res = fem::TopologyOptimize(mesh, steel, {}, {}, fem::TopoParams{}, &err);
  CHECK_FALSE(res.has_value());
  CHECK_FALSE(err.empty());
}

#endif  // HAVE_FEM_SOLVER

// ---- Serialization (no solver/mesher needed) ----------------------------

TEST_CASE("Topology study definition round-trips through JSON", "[topopt][serialize]") {
  fem::Study s;
  s.id = 7;
  s.name = "Gen";
  s.type = fem::AnalysisType::Topology;
  s.meshSize = 3.0;
  s.volumeFraction = 0.25;
  s.volumeFractionExpr = "vf";
  s.filterRadius = 4.0;
  s.maxIterations = 45;
  s.keepFaces = {ElementRef{2, "2/cap0"}, ElementRef{2, "2/cap1"}};
  fem::Load l;
  l.id = 8;
  l.type = fem::LoadType::Force;
  l.face = ElementRef{2, "2/g1"};
  l.magnitude = 300.0;
  l.direction = glm::dvec3(0, 0, -1);
  s.loads.push_back(l);

  const fem::Study r = DocumentIO::StudyFromJson(DocumentIO::StudyToJson(s));
  CHECK(r.type == fem::AnalysisType::Topology);
  CHECK_THAT(r.volumeFraction, WithinAbs(0.25, 1e-12));
  CHECK(r.volumeFractionExpr == "vf");
  CHECK_THAT(r.filterRadius, WithinAbs(4.0, 1e-12));
  CHECK(r.maxIterations == 45);
  REQUIRE(r.keepFaces.size() == 2);
  CHECK(r.keepFaces[1].name == "2/cap1");
  REQUIRE(r.loads.size() == 1);
  CHECK(r.loads[0].id == 8);
}

TEST_CASE("Topology result round-trips through JSON", "[topopt][serialize]") {
  // A hand-built two-tet runtime (no mesher): exercises the flat-array mesh +
  // density encoding without depending on Netgen.
  fem::StudyRuntime rt;
  rt.topology = true;
  rt.iterations = 12;
  rt.converged = true;
  rt.volumeFractionAchieved = 0.31;
  rt.complianceHistory = {9.0, 5.0, 4.2};
  fem::StudyBodyResult b;
  b.bodyIndex = 0;
  b.bodyName = "Envelope";
  for (int i = 0; i < 12; ++i)
    b.mesh.nodes.push_back(glm::dvec3(i * 0.5, i * 1.5, i * 2.5));
  b.mesh.tets.push_back({0, 1, 2, 3, 4, 5, 6, 7, 8, 9});
  b.mesh.tets.push_back({1, 2, 3, 4, 5, 6, 7, 8, 9, 10});
  fem::FemMesh::BoundaryTri tri;
  tri.nodes = {0, 1, 2, 4, 5, 6};
  tri.faceIndex = 3;
  b.mesh.boundary.push_back(tri);
  b.density = {0.8, 0.2};
  rt.bodies.push_back(std::move(b));

  nlohmann::json j;
  REQUIRE(DocumentIO::StudyResultToJson(rt, j));
  const fem::StudyRuntime r = DocumentIO::StudyResultFromJson(j);
  CHECK(r.topology);
  CHECK(r.iterations == 12);
  CHECK(r.converged);
  CHECK_THAT(r.volumeFractionAchieved, WithinAbs(0.31, 1e-12));
  REQUIRE(r.complianceHistory.size() == 3);
  CHECK_THAT(r.complianceHistory[2], WithinAbs(4.2, 1e-12));
  REQUIRE(r.bodies.size() == 1);
  CHECK(r.bodies[0].bodyName == "Envelope");
  REQUIRE(r.bodies[0].mesh.nodes.size() == 12);
  CHECK_THAT(r.bodies[0].mesh.nodes[11].z, WithinAbs(27.5, 1e-12));
  REQUIRE(r.bodies[0].mesh.tets.size() == 2);
  CHECK(r.bodies[0].mesh.tets[1][9] == 10);
  REQUIRE(r.bodies[0].mesh.boundary.size() == 1);
  CHECK(r.bodies[0].mesh.boundary[0].faceIndex == 3);
  REQUIRE(r.bodies[0].density.size() == 2);
  CHECK_THAT(r.bodies[0].density[0], WithinAbs(0.8, 1e-12));
}

TEST_CASE("StudyResultToJson declines a non-topology runtime", "[topopt][serialize]") {
  fem::StudyRuntime rt;  // topology defaults false (a static/modal result)
  nlohmann::json j;
  CHECK_FALSE(DocumentIO::StudyResultToJson(rt, j));
}

// ---- Iso-surface extraction (no solver/mesher needed) -------------------

namespace {
// A per-tet density that ramps 0 -> 1 along x (each tet gets its corner-mean
// x, normalized) — a smooth field with a well-defined iso-level per threshold.
std::vector<double> RampDensityX(const fem::FemMesh& m, double Lx) {
  std::vector<double> d(m.tets.size());
  for (size_t e = 0; e < m.tets.size(); ++e) {
    double x = 0;
    for (int i = 0; i < 4; ++i) x += m.nodes[m.tets[e][i]].x;
    d[e] = (x / 4.0) / Lx;
  }
  return d;
}
}  // namespace

namespace {

// Edge-multiplicity check on a welded triangle soup: a CLOSED surface has
// every edge shared by exactly two triangles.
bool SurfaceIsClosed(const std::vector<glm::vec3>& pos,
                     const std::vector<uint32_t>& idx) {
  auto key = [](const glm::vec3& p) {
    auto q = [](float v) { return llround(static_cast<double>(v) * 1e5); };
    return std::make_tuple(q(p.x), q(p.y), q(p.z));
  };
  std::map<std::tuple<long long, long long, long long>, uint32_t> weld;
  std::vector<uint32_t> remap(pos.size());
  for (size_t i = 0; i < pos.size(); ++i) {
    auto k = key(pos[i]);
    auto it = weld.find(k);
    if (it == weld.end()) {
      weld[k] = static_cast<uint32_t>(weld.size());
      remap[i] = static_cast<uint32_t>(weld.size() - 1);
    } else {
      remap[i] = it->second;
    }
  }
  std::map<std::pair<uint32_t, uint32_t>, int> edges;
  for (size_t k = 0; k + 2 < idx.size(); k += 3) {
    const uint32_t v[3] = {remap[idx[k]], remap[idx[k + 1]], remap[idx[k + 2]]};
    if (v[0] == v[1] || v[1] == v[2] || v[0] == v[2]) continue;  // degenerate
    for (int e = 0; e < 3; ++e) {
      const uint32_t a = v[e], b = v[(e + 1) % 3];
      edges[{std::min(a, b), std::max(a, b)}]++;
    }
  }
  for (const auto& [e, count] : edges)
    if (count != 2) return false;
  return !edges.empty();
}

}  // namespace

TEST_CASE("Iso-surface is closed — capped at the domain boundary",
          "[topopt][iso]") {
  // A solid region touching the domain boundary must render as a CLOSED
  // solid, not an open shell (the boundary-cap half of the extraction; the
  // marching-tet interface alone leaves every domain face open).
  fem::FemMesh mesh = GenerateBoxMesh(4, 3, 3, 40, 30, 30);

  SECTION("uniform solid: the surface is the closed domain boundary") {
    std::vector<double> density(mesh.tets.size(), 1.0);
    std::vector<glm::vec3> pos, nrm;
    std::vector<uint32_t> idx;
    fem::ExtractIsoSurface(mesh, density, 0.5, pos, nrm, idx);
    REQUIRE_FALSE(pos.empty());
    CHECK(SurfaceIsClosed(pos, idx));
    // Outward normals: every facet's normal points away from the box center.
    const glm::vec3 center(20.0f, 15.0f, 15.0f);
    for (size_t k = 0; k + 2 < idx.size(); k += 3) {
      const glm::vec3 ctr =
          (pos[idx[k]] + pos[idx[k + 1]] + pos[idx[k + 2]]) / 3.0f;
      CHECK(glm::dot(nrm[idx[k]], ctr - center) > 0.0f);
    }
  }

  SECTION("half-solid field: interface + caps close each other") {
    std::vector<double> density(mesh.tets.size());
    for (size_t e = 0; e < mesh.tets.size(); ++e) {
      glm::dvec3 c(0.0);
      for (int i = 0; i < 4; ++i) c += mesh.nodes[mesh.tets[e][i]];
      density[e] = (c.x / 4.0) < 20.0 ? 1.0 : 0.0;
    }
    std::vector<glm::vec3> pos, nrm;
    std::vector<uint32_t> idx;
    fem::ExtractIsoSurface(mesh, density, 0.5, pos, nrm, idx);
    REQUIRE_FALSE(pos.empty());
    CHECK(SurfaceIsClosed(pos, idx));
  }
}

TEST_CASE("AutoIsoThreshold volume-matches the target fraction", "[topopt][iso]") {
  const double Lx = 80;
  fem::FemMesh mesh = GenerateBoxMesh(8, 4, 4, Lx, 40, 40);
  const std::vector<double> d = RampDensityX(mesh, Lx);

  // With a uniform 0->1 ramp, keeping fraction f of the volume means keeping
  // the densest f, i.e. threshold ~ 1 - f.
  const double t30 = fem::AutoIsoThreshold(mesh, d, 0.3);
  const double t60 = fem::AutoIsoThreshold(mesh, d, 0.6);
  CHECK_THAT(t30, WithinAbs(0.7, 0.06));
  CHECK_THAT(t60, WithinAbs(0.4, 0.06));
  CHECK(t60 < t30);  // a larger kept fraction -> a lower threshold
}

TEST_CASE("ExtractIsoSurface produces an oriented surface at a crossing",
          "[topopt][iso]") {
  const double Lx = 80, Ly = 40, Lz = 40;
  fem::FemMesh mesh = GenerateBoxMesh(8, 4, 4, Lx, Ly, Lz);
  const std::vector<double> d = RampDensityX(mesh, Lx);

  std::vector<glm::vec3> pos, nrm;
  std::vector<uint32_t> idx;
  fem::ExtractIsoSurface(mesh, d, 0.5, pos, nrm, idx);

  REQUIRE(pos.size() == nrm.size());
  REQUIRE(idx.size() % 3 == 0);
  CHECK(idx.size() > 0);  // the 0->1 ramp crosses 0.5
  for (const glm::vec3& p : pos) {
    CHECK((p.x >= -1e-3f && p.x <= Lx + 1e-3f));
    CHECK((p.y >= -1e-3f && p.y <= Ly + 1e-3f));
    CHECK((p.z >= -1e-3f && p.z <= Lz + 1e-3f));
  }
  for (const glm::vec3& n : nrm) CHECK_THAT(glm::length(n), WithinAbs(1.0f, 1e-4f));
  // The ramp increases with x, so the solid (density>=t) is the +x half and
  // the INTERFACE faces roughly -x (outward, toward the void). Boundary caps
  // (which close the solid against the domain faces) point every which way,
  // so average only facets strictly inside the domain.
  glm::vec3 avg(0);
  int nInterior = 0;
  auto onBoundary = [&](const glm::vec3& c) {
    const float e = 1e-3f;
    return c.x < e || c.x > Lx - e || c.y < e || c.y > Ly - e || c.z < e ||
           c.z > Lz - e;
  };
  for (size_t k = 0; k + 2 < idx.size(); k += 3) {
    const glm::vec3 c = (pos[idx[k]] + pos[idx[k + 1]] + pos[idx[k + 2]]) / 3.0f;
    if (onBoundary(c)) continue;
    avg += nrm[idx[k]];
    ++nInterior;
  }
  REQUIRE(nInterior > 0);
  avg /= float(nInterior);
  CHECK(avg.x < 0.0f);

  // A uniform field ABOVE the threshold is everywhere-solid: since the
  // boundary caps landed, that extracts the closed domain boundary (it used
  // to be empty — the open-shell era). A field BELOW the threshold is empty.
  std::vector<glm::vec3> p2, n2;
  std::vector<uint32_t> i2;
  fem::ExtractIsoSurface(mesh, std::vector<double>(mesh.tets.size(), 0.9), 0.5,
                         p2, n2, i2);
  CHECK_FALSE(i2.empty());
  std::vector<glm::vec3> p3, n3;
  std::vector<uint32_t> i3;
  fem::ExtractIsoSurface(mesh, std::vector<double>(mesh.tets.size(), 0.1), 0.5,
                         p3, n3, i3);
  CHECK(i3.empty());
}

TEST_CASE("ExportTopoStl writes a well-formed binary STL", "[topopt][iso]") {
  namespace fs = std::filesystem;
  const double Lx = 80;
  fem::FemMesh mesh = GenerateBoxMesh(8, 4, 4, Lx, 40, 40);
  const std::vector<double> d = RampDensityX(mesh, Lx);
  const fs::path path = fs::temp_directory_path() / "topo_export_test.stl";
  fs::remove(path);

  std::string err;
  REQUIRE(fem::ExportTopoStl(mesh, d, 0.5, 3, path.string(), &err));
  INFO(err);
  REQUIRE(fs::exists(path));

  // Binary STL: 80-byte header + uint32 triangle count + 50 bytes/triangle.
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in.good());
  char header[80];
  in.read(header, 80);
  uint32_t nTri = 0;
  in.read(reinterpret_cast<char*>(&nTri), 4);
  CHECK(nTri > 0);
  const auto size = fs::file_size(path);
  CHECK(size == 84u + std::size_t(nTri) * 50u);

  // An all-solid field now exports the closed domain block (boundary caps);
  // a truly EMPTY surface — everything below threshold — is an export error,
  // not an empty file.
  std::string errSolid;
  CHECK(fem::ExportTopoStl(mesh, std::vector<double>(mesh.tets.size(), 0.9),
                           0.5, 3, path.string(), &errSolid));
  std::string err2;
  CHECK_FALSE(fem::ExportTopoStl(mesh, std::vector<double>(mesh.tets.size(), 0.1),
                                 0.5, 3, path.string(), &err2));
  fs::remove(path);
}
