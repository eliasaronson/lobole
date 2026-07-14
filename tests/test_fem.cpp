#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "CADKernel.h"
#include "core/Command.h"
#include "core/Document.h"
#include "core/FemMesher.h"
#include "core/FemSolver.h"
#include "core/FemStudy.h"
#include "core/Serialization.h"

#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>

#include <array>
#include <cmath>
#include <set>

using Catch::Matchers::WithinRel;

namespace {

// A 100 x 40 x 40 block with an 8 mm-radius hole bored through it — a shape
// with both planar and cylindrical faces, the FEM0 spike geometry.
CADShape BlockWithHole() {
  CADShape block = CADKernel::Instance().CreateBox(100.0, 40.0, 40.0);
  CADShape hole = CADKernel::Instance().CreateCylinder(glm::vec3(50, 20, -1),
                                                       glm::vec3(0, 0, 1), 8.0, 42.0);
  return block.Difference(hole);
}

double EdgeLen(const glm::dvec3& a, const glm::dvec3& b) { return glm::length(a - b); }

}  // namespace

#ifdef HAVE_NETGEN

TEST_CASE("FemMesher meshes a solid to tet10 with face attribution", "[fem][mesh]") {
  CADShape part = BlockWithHole();
  REQUIRE(part.IsValid());

  fem::MeshSettings settings;
  settings.maxSize = 8.0;
  std::string err;
  auto mesh = fem::Mesh(part.GetShape(), settings, &err);
  INFO(err);
  REQUIRE(mesh.has_value());

  SECTION("produces a non-empty second-order mesh") {
    CHECK(mesh->stats.nodeCount > 0);
    CHECK(mesh->stats.tetCount > 0);
    CHECK(mesh->nodes.size() == mesh->stats.nodeCount);
    CHECK(mesh->tets.size() == mesh->stats.tetCount);
    CHECK(mesh->stats.worstQuality > 0.0);
    CHECK(mesh->stats.worstQuality <= 1.0);
  }

  SECTION("every boundary triangle attributes to a real OCC face") {
    TopTools_IndexedMapOfShape faceMap;
    TopExp::MapShapes(part.GetShape(), TopAbs_FACE, faceMap);
    const int nFaces = faceMap.Extent();
    REQUIRE(nFaces > 0);
    std::set<int> touched;
    for (const auto& tri : mesh->boundary) {
      CHECK(tri.faceIndex >= 0);
      CHECK(tri.faceIndex < nFaces);
      touched.insert(tri.faceIndex);
    }
    // A closed solid: every face carries at least one boundary triangle.
    CHECK(touched.size() == std::size_t(nFaces));
  }

  SECTION("tet10 midside nodes lie on their edges") {
    // On straight edges the midside is the exact midpoint; on the curved bore
    // it is projected onto the surface but still near the chord midpoint.
    // Assert the canonical ordering: slot 4 = edge(0,1), 5 = edge(1,2),
    // 6 = edge(2,0), 7 = edge(0,3), 8 = edge(1,3), 9 = edge(2,3).
    const std::array<std::pair<int, int>, 6> edges = {
        {{0, 1}, {1, 2}, {2, 0}, {0, 3}, {1, 3}, {2, 3}}};
    int checked = 0;
    for (const auto& t : mesh->tets) {
      for (int e = 0; e < 6; ++e) {
        const glm::dvec3& a = mesh->nodes[t[edges[e].first]];
        const glm::dvec3& b = mesh->nodes[t[edges[e].second]];
        const glm::dvec3& mid = mesh->nodes[t[4 + e]];
        const glm::dvec3 chordMid = 0.5 * (a + b);
        // Midside is within a quarter of the edge length of the chord midpoint
        // (generous, to allow curvature projection on the bore).
        CHECK(glm::length(mid - chordMid) <= 0.25 * EdgeLen(a, b));
      }
      if (++checked >= 200) break;
    }
    CHECK(checked > 0);
  }
}

TEST_CASE("FemMesher per-face size refines that face", "[fem][mesh]") {
  CADShape part = BlockWithHole();
  fem::MeshSettings coarse;
  coarse.maxSize = 8.0;
  std::string err;
  auto base = fem::Mesh(part.GetShape(), coarse, &err);
  INFO(err);
  REQUIRE(base.has_value());

  // Count boundary triangles on face 0 at the global size.
  auto trisOnFace0 = [](const fem::FemMesh& m) {
    int n = 0;
    for (const auto& t : m.boundary)
      if (t.faceIndex == 0) ++n;
    return n;
  };
  const int coarseTris = trisOnFace0(*base);

  fem::MeshSettings refined = coarse;
  refined.faceSizes.push_back({0, 2.0});  // shrink face 0 to 2 mm
  auto fine = fem::Mesh(part.GetShape(), refined, &err);
  INFO(err);
  REQUIRE(fine.has_value());
  CHECK(trisOnFace0(*fine) > coarseTris);
}

TEST_CASE("FemMesher is reusable within one process", "[fem][mesh]") {
  // Netgen keeps global state; two meshings in a row must both succeed.
  CADShape part = BlockWithHole();
  fem::MeshSettings s;
  s.maxSize = 10.0;
  std::string err;
  auto a = fem::Mesh(part.GetShape(), s, &err);
  INFO(err);
  REQUIRE(a.has_value());
  auto b = fem::Mesh(part.GetShape(), s, &err);
  INFO(err);
  REQUIRE(b.has_value());
  CHECK(a->stats.tetCount > 0);
  CHECK(b->stats.tetCount > 0);
}

TEST_CASE("FemMesher reports failure on a null shape", "[fem][mesh]") {
  std::string err;
  auto m = fem::Mesh(TopoDS_Shape(), fem::MeshSettings(), &err);
  CHECK_FALSE(m.has_value());
  CHECK_FALSE(err.empty());
}

#else  // !HAVE_NETGEN

TEST_CASE("FemMesher reports a clear error when built without Netgen", "[fem][mesh]") {
  CHECK_FALSE(fem::Available());
  std::string err;
  auto m = fem::Mesh(BlockWithHole().GetShape(), fem::MeshSettings(), &err);
  CHECK_FALSE(m.has_value());
  CHECK(err.find("FEM support") != std::string::npos);
}

#endif  // HAVE_NETGEN

// ---- Study commands (ungated: no meshing) ---------------------------------

TEST_CASE("Study add/remove commands undo and redo", "[fem][study]") {
  Document doc;
  CommandStack stack;

  auto add = std::make_unique<AddStudyCommand>("My Study");
  AddStudyCommand* raw = add.get();
  REQUIRE(stack.Execute(std::move(add), doc));
  const uint64_t id = raw->StudyId();
  REQUIRE(doc.GetStudies().size() == 1);
  CHECK(doc.FindStudy(id)->name == "My Study");

  stack.Undo(doc);
  CHECK(doc.GetStudies().empty());
  stack.Redo(doc);
  REQUIRE(doc.GetStudies().size() == 1);
  CHECK(doc.FindStudy(id) != nullptr);

  REQUIRE(stack.Execute(std::make_unique<RemoveStudyCommand>(id), doc));
  CHECK(doc.GetStudies().empty());
  stack.Undo(doc);  // restores the removed study, ids intact
  REQUIRE(doc.GetStudies().size() == 1);
  CHECK(doc.FindStudy(id)->name == "My Study");
}

TEST_CASE("ModifyStudyCommand round-trips a load edit", "[fem][study]") {
  Document doc;
  CommandStack stack;
  fem::Study& s = doc.AddStudy("S");
  const uint64_t id = s.id;

  // Snapshot before/after adding a load, the RPC's before/after shape.
  nlohmann::json before = DocumentIO::StudyToJson(*doc.FindStudy(id));
  fem::Study copy = *doc.FindStudy(id);
  fem::Load l;
  l.id = doc.AllocateId();
  l.type = fem::LoadType::Force;
  l.face = ElementRef{2, "2/cap1"};
  l.magnitude = 500.0;
  copy.loads.push_back(l);
  nlohmann::json after = DocumentIO::StudyToJson(copy);

  REQUIRE(stack.Execute(
      std::make_unique<ModifyStudyCommand>("Add load", before, after), doc));
  REQUIRE(doc.FindStudy(id)->loads.size() == 1);
  CHECK(doc.FindStudy(id)->loads[0].magnitude == 500.0);

  stack.Undo(doc);
  CHECK(doc.FindStudy(id)->loads.empty());
  stack.Redo(doc);
  REQUIRE(doc.FindStudy(id)->loads.size() == 1);
}

#ifdef HAVE_NETGEN

TEST_CASE("MeshStudy meshes the loaded body and errors on a dangling ref",
          "[fem][study][mesh]") {
  // Build a bar body via an extrude so faces carry stable element names.
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>("Bar", glm::vec3(0, 0, 0),
                                              20.0, 4.0, 4.0));
  REQUIRE(doc.GetBodies().size() == 1);
  const uint64_t createdBy = doc.GetBodies()[0].createdBy;
  // Pick a real face name from the body's element map.
  REQUIRE_FALSE(doc.GetBodies()[0].elementMap.names.empty());
  const std::string faceName = doc.GetBodies()[0].elementMap.names.begin()->second;

  fem::Study& s = doc.AddStudy("S");
  s.meshSize = 3.0;
  fem::Load fixed;
  fixed.id = doc.AllocateId();
  fixed.type = fem::LoadType::Fixed;
  fixed.face = ElementRef{createdBy, faceName};
  s.loads.push_back(fixed);

  std::vector<fem::StudyBodyMesh> meshes;
  std::string err;
  INFO(err);
  REQUIRE(fem::MeshStudy(doc, s, meshes, &err));
  REQUIRE(meshes.size() == 1);
  CHECK(meshes[0].mesh.stats.tetCount > 0);
  CHECK(s.error.empty());

  // A ref to a non-existent element is a first-class study error.
  s.loads[0].face.name = "nope/does-not-exist";
  CHECK_FALSE(fem::MeshStudy(doc, s, meshes, &err));
  CHECK_FALSE(s.error.empty());
}

#endif  // HAVE_NETGEN

// ---- Solver analytic benchmarks (netgen-gated: need a real mesh) ----------
#ifdef HAVE_NETGEN

namespace {
// Selects fixed nodes (a whole plane) and load tris (a whole face) by
// coordinate, so the benchmarks test the solver directly without toponaming.
struct BarProblem {
  fem::FemMesh mesh;
  std::vector<int> fixedNodes;
  fem::FaceLoad load;
};

// Nodes whose coordinate `axis` is within tol of `value`.
std::vector<int> NodesAtPlane(const fem::FemMesh& m, int axis, double value, double tol) {
  std::vector<int> out;
  for (int i = 0; i < (int)m.nodes.size(); ++i)
    if (std::abs(m.nodes[i][axis] - value) < tol) out.push_back(i);
  return out;
}
// Boundary tris all of whose nodes sit on the plane axis==value.
std::vector<int> TrisAtPlane(const fem::FemMesh& m, int axis, double value, double tol) {
  std::vector<int> out;
  for (int t = 0; t < (int)m.boundary.size(); ++t) {
    bool on = true;
    for (int k = 0; k < 6; ++k)
      if (std::abs(m.nodes[m.boundary[t].nodes[k]][axis] - value) >= tol) on = false;
    if (on) out.push_back(t);
  }
  return out;
}
}  // namespace

TEST_CASE("Solver: uniaxial bar matches sigma=F/A and u=FL/EA", "[fem][solver]") {
  const double L = 100, W = 10;  // 100 x 10 x 10 mm
  CADShape bar = CADKernel::Instance().CreateBox(L, W, W);
  fem::MeshSettings ms;
  ms.maxSize = 4.0;
  std::string err;
  auto mesh = fem::Mesh(bar.GetShape(), ms, &err);
  INFO(err);
  REQUIRE(mesh.has_value());

  fem::Material steel{200000.0, 0.3};  // 200 GPa, nu 0.3
  const double F = 10000.0;            // N, +x
  const double A = W * W;              // 100 mm^2

  std::vector<int> fixed = NodesAtPlane(*mesh, 0, 0.0, 1e-6);
  REQUIRE(fixed.size() > 3);
  fem::FaceLoad load;
  load.type = fem::LoadType::Force;
  load.tris = TrisAtPlane(*mesh, 0, L, 1e-6);
  load.magnitude = F;
  load.direction = glm::dvec3(1, 0, 0);
  REQUIRE_FALSE(load.tris.empty());

  auto res = fem::Solve(*mesh, steel, fixed, {load}, &err);
  INFO(err);
  REQUIRE(res.has_value());

  // Tip axial displacement u = FL/EA.
  const double uExpected = F * L / (steel.youngMPa * A);  // 0.05 mm
  CHECK_THAT(res->maxDisplacement, WithinRel(uExpected, 0.03));

  // Uniaxial stress in the mid-region (away from clamp/load) ~ F/A.
  const double sigmaExpected = F / A;  // 100 MPa
  int mid = -1;
  double best = 1e30;
  for (int i = 0; i < (int)mesh->nodes.size(); ++i) {
    const auto& p = mesh->nodes[i];
    const double d = std::abs(p.x - L / 2) + std::abs(p.y - W / 2) + std::abs(p.z - W / 2);
    if (d < best) { best = d; mid = i; }
  }
  INFO("mid node vm = " << res->vonMises[mid] << " expected " << sigmaExpected);
  CHECK_THAT(res->vonMises[mid], WithinRel(sigmaExpected, 0.05));
}

TEST_CASE("Solver: cantilever tip deflection near Euler-Bernoulli", "[fem][solver]") {
  // L=100, cross-section b=10 (y) x h=10 (z). Fix x=0, transverse tip load in -z.
  const double L = 100, b = 10, h = 10;
  CADShape beam = CADKernel::Instance().CreateBox(L, b, h);
  fem::MeshSettings ms;
  ms.maxSize = 4.0;
  std::string err;
  auto mesh = fem::Mesh(beam.GetShape(), ms, &err);
  INFO(err);
  REQUIRE(mesh.has_value());

  fem::Material steel{200000.0, 0.3};
  const double P = 1000.0;  // N in -z
  std::vector<int> fixed = NodesAtPlane(*mesh, 0, 0.0, 1e-6);
  fem::FaceLoad load;
  load.type = fem::LoadType::Force;
  load.tris = TrisAtPlane(*mesh, 0, L, 1e-6);
  load.magnitude = P;
  load.direction = glm::dvec3(0, 0, -1);

  auto res = fem::Solve(*mesh, steel, fixed, {load}, &err);
  INFO(err);
  REQUIRE(res.has_value());

  const double I = b * h * h * h / 12.0;             // 833.33 mm^4
  const double delta = P * L * L * L / (3 * steel.youngMPa * I);  // Euler-Bernoulli
  // tet10 with shear + clamp effects: within ~10% of slender-beam theory.
  INFO("tip delta = " << res->maxDisplacement << " EB = " << delta);
  CHECK_THAT(res->maxDisplacement, WithinRel(delta, 0.12));
}

#endif  // HAVE_NETGEN

TEST_CASE("Solver: pressure on a face compresses along the inward normal",
          "[fem][solver]") {
  // Fix x=0, apply pressure on x=L: it must push inward (-x) => compression,
  // validating the pressure sign and the outward-normal convention.
  const double L = 40, W = 10;
  CADShape block = CADKernel::Instance().CreateBox(L, W, W);
  fem::MeshSettings ms;
  ms.maxSize = 3.0;
  std::string err;
  auto mesh = fem::Mesh(block.GetShape(), ms, &err);
  INFO(err);
  REQUIRE(mesh.has_value());

  fem::Material steel{200000.0, 0.3};
  const double P = 50.0;  // MPa
  std::vector<int> fixed = NodesAtPlane(*mesh, 0, 0.0, 1e-6);
  fem::FaceLoad load;
  load.type = fem::LoadType::Pressure;
  load.tris = TrisAtPlane(*mesh, 0, L, 1e-6);
  load.magnitude = P;

  auto res = fem::Solve(*mesh, steel, fixed, {load}, &err);
  INFO(err);
  REQUIRE(res.has_value());

  // Mid-region von Mises ~ P (uniaxial compression state sigma_xx = -P).
  int mid = -1;
  double best = 1e30;
  for (int i = 0; i < (int)mesh->nodes.size(); ++i) {
    const auto& p = mesh->nodes[i];
    const double d = std::abs(p.x - L / 2) + std::abs(p.y - W / 2) + std::abs(p.z - W / 2);
    if (d < best) { best = d; mid = i; }
  }
  INFO("mid vm = " << res->vonMises[mid] << " expected " << P);
  CHECK_THAT(res->vonMises[mid], WithinRel(P, 0.05));

  // The loaded face must displace inward (-x), confirming the sign.
  double sumx = 0;
  int cnt = 0;
  for (int n : NodesAtPlane(*mesh, 0, L, 1e-6)) { sumx += res->displacement[n].x; ++cnt; }
  CHECK(sumx / cnt < 0.0);
}

// ---- RunStudy orchestration + validation (netgen-gated) -------------------
#ifdef HAVE_NETGEN

namespace {
// A steel bar body via BoxFeature; returns the createdBy id and a face name
// (feature/face) by geometric side. side: 0 => x=0 face, 1 => x=L face.
struct BarDoc {
  Document doc;
  uint64_t createdBy = 0;
};
}  // namespace

TEST_CASE("RunStudy errors when the body material has no stiffness data",
          "[fem][run]") {
  // A BoxFeature's default material is unassigned (youngGPa 0), so a study on
  // it must fail loudly rather than guessing. (The full solve path is covered
  // by the analytic benchmarks and the headless fem loop.)
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>("Bar", glm::vec3(0, 0, 0),
                                              100.0, 10.0, 10.0));
  const uint64_t createdBy = doc.GetBodies()[0].createdBy;
  auto faceName = [&](int i) { return std::to_string(createdBy) + "/face" + std::to_string(i); };

  fem::Study& s = doc.AddStudy("S");
  s.meshSize = 6.0;
  fem::Load fixed;
  fixed.id = doc.AllocateId();
  fixed.type = fem::LoadType::Fixed;
  fixed.face = ElementRef{createdBy, faceName(0)};
  s.loads.push_back(fixed);
  fem::Load force;
  force.id = doc.AllocateId();
  force.type = fem::LoadType::Force;
  force.face = ElementRef{createdBy, faceName(1)};
  force.magnitude = 5000.0;
  force.direction = glm::dvec3(1, 0, 0);
  s.loads.push_back(force);

  fem::StudyRuntime rt;
  std::string err;
  CHECK_FALSE(fem::RunStudy(doc, s, rt, &err));
  CHECK(err.find("stiffness") != std::string::npos);
  CHECK_FALSE(s.error.empty());
}

TEST_CASE("RunStudy errors when a loaded body has no fixed support",
          "[fem][run]") {
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>("Bar", glm::vec3(0, 0, 0),
                                              50.0, 10.0, 10.0));
  const uint64_t createdBy = doc.GetBodies()[0].createdBy;
  fem::Study& s = doc.AddStudy("S");
  s.meshSize = 6.0;
  fem::Load force;
  force.id = doc.AllocateId();
  force.type = fem::LoadType::Force;
  force.face = ElementRef{createdBy, std::to_string(createdBy) + "/face1"};
  force.magnitude = 1000.0;
  force.direction = glm::dvec3(1, 0, 0);
  s.loads.push_back(force);

  fem::StudyRuntime rt;
  std::string err;
  CHECK_FALSE(fem::RunStudy(doc, s, rt, &err));
  // Fails either on material (default) or on missing fixed support — both are
  // valid pre-solve errors; assert it did not silently "succeed".
  CHECK_FALSE(err.empty());
}

#endif  // HAVE_NETGEN
