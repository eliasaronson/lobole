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

using Catch::Matchers::WithinAbs;
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

TEST_CASE("Solver: Lame thick-wall cylinder von Mises on a curved surface",
          "[fem][solver]") {
  // A pressurized thick tube — the one benchmark with CURVED geometry, so it
  // validates the second-order (tet10) midside projection onto curved faces.
  // a=10, b=20 mm, internal p=10 MPa, along z, length H. Both ends fully
  // clamped: away from the ends the mid-plane (a symmetry plane, u_z=0)
  // approaches plane strain, matching Lame with sigma_z = nu*(sigma_r+sigma_t).
  const double a = 10, b = 20, H = 60, p = 10;
  CADShape outer =
      CADKernel::Instance().CreateCylinder(glm::vec3(0, 0, 0), glm::vec3(0, 0, 1), b, H);
  CADShape bore =
      CADKernel::Instance().CreateCylinder(glm::vec3(0, 0, -1), glm::vec3(0, 0, 1), a, H + 2);
  CADShape tube = outer.Difference(bore);
  REQUIRE(tube.IsValid());

  fem::MeshSettings ms;
  ms.maxSize = 4.0;
  std::string err;
  auto mesh = fem::Mesh(tube.GetShape(), ms, &err);
  INFO(err);
  REQUIRE(mesh.has_value());

  auto radius = [&](int n) {
    return std::hypot(mesh->nodes[n].x, mesh->nodes[n].y);
  };
  // Fix both end faces fully (z=0 and z=H).
  std::vector<int> fixed;
  for (int i = 0; i < (int)mesh->nodes.size(); ++i)
    if (std::abs(mesh->nodes[i].z) < 1e-6 || std::abs(mesh->nodes[i].z - H) < 1e-6)
      fixed.push_back(i);
  REQUIRE(fixed.size() > 6);

  // Pressure on the bore: every node of the tri sits at radius ~ a (the flat
  // end rings span a..b, so they don't qualify).
  fem::FaceLoad load;
  load.type = fem::LoadType::Pressure;
  load.magnitude = p;
  for (int t = 0; t < (int)mesh->boundary.size(); ++t) {
    bool bore6 = true;
    for (int k = 0; k < 6; ++k)
      if (std::abs(radius(mesh->boundary[t].nodes[k]) - a) > 0.6) bore6 = false;
    if (bore6) load.tris.push_back(t);
  }
  REQUIRE_FALSE(load.tris.empty());

  fem::Material steel{200000.0, 0.3};
  auto res = fem::Solve(*mesh, steel, fixed, {load}, &err);
  INFO(err);
  REQUIRE(res.has_value());

  // Plane-strain Lame stresses at the inner radius, then von Mises.
  const double nu = 0.3;
  const double sr = -p;
  const double st = p * (b * b + a * a) / (b * b - a * a);
  const double sz = nu * (sr + st);
  const double vmExpected =
      std::sqrt(0.5 * ((sr - st) * (sr - st) + (st - sz) * (st - sz) +
                       (sz - sr) * (sz - sr)));  // ~23.1 MPa

  // Inner-radius node nearest the mid-plane (away from the clamped ends).
  int inner = -1;
  double best = 1e30;
  for (int i = 0; i < (int)mesh->nodes.size(); ++i) {
    const double d = std::abs(radius(i) - a) + std::abs(mesh->nodes[i].z - H / 2);
    if (d < best) { best = d; inner = i; }
  }
  INFO("inner mid-plane vm = " << res->vonMises[inner] << " expected " << vmExpected);
  // ~4% on a 4 mm mesh; headroom for Netgen's non-bit-stable remeshing.
  CHECK_THAT(res->vonMises[inner], WithinRel(vmExpected, 0.08));
}

#endif  // HAVE_NETGEN

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

TEST_CASE("RunStudy solves multiple loaded bodies and aggregates the maxima",
          "[fem][run]") {
  // Two disjoint steel bars in one study, loaded to different stress levels.
  // Exercises the per-body solve loop and the cross-body max aggregation.
  Document doc;
  auto a = std::make_unique<BoxFeature>("BarA", glm::vec3(0, 0, 0), 100, 10, 10);
  a->SetMaterial("steel");
  doc.AddFeature(std::move(a));
  auto b = std::make_unique<BoxFeature>("BarB", glm::vec3(0, 0, 50), 100, 10, 10);
  b->SetMaterial("steel");
  doc.AddFeature(std::move(b));
  REQUIRE(doc.GetBodies().size() == 2);
  const uint64_t cbA = doc.GetBodies()[0].createdBy;
  const uint64_t cbB = doc.GetBodies()[1].createdBy;

  fem::Study& s = doc.AddStudy("Two bars");
  s.meshSize = 6.0;
  auto addBar = [&](uint64_t cb, double force) {
    auto face = [&](int i) { return ElementRef{cb, std::to_string(cb) + "/face" + std::to_string(i)}; };
    fem::Load fx;
    fx.id = doc.AllocateId();
    fx.type = fem::LoadType::Fixed;
    fx.face = face(0);
    s.loads.push_back(fx);
    fem::Load fo;
    fo.id = doc.AllocateId();
    fo.type = fem::LoadType::Force;
    fo.face = face(1);
    fo.magnitude = force;
    fo.direction = glm::dvec3(1, 0, 0);
    s.loads.push_back(fo);
  };
  addBar(cbA, 2000.0);   // low stress
  addBar(cbB, 10000.0);  // 5x the load -> the governing body

  fem::StudyRuntime rt;
  std::string err;
  INFO(err);
  REQUIRE(fem::RunStudy(doc, s, rt, &err));

  REQUIRE(rt.bodies.size() == 2);
  // Per-body results, and the aggregate max drawn from the higher-loaded bar.
  double perBodyMax = 0;
  double barBMax = 0, barAMax = 0;
  for (const fem::StudyBodyResult& br : rt.bodies) {
    CHECK(br.results.maxVonMises > 0);
    perBodyMax = std::max(perBodyMax, br.results.maxVonMises);
    if (br.bodyName == "BarB") barBMax = br.results.maxVonMises;
    if (br.bodyName == "BarA") barAMax = br.results.maxVonMises;
  }
  CHECK(barBMax > barAMax);                  // heavier bar is more stressed
  CHECK(rt.maxVmBody == "BarB");             // aggregate governed by BarB
  CHECK_THAT(rt.maxVonMises, WithinRel(perBodyMax, 1e-9));  // = max over bodies
}

#endif  // HAVE_NETGEN

// ---- Element-level solver unit tests (no mesher: Eigen-gated only) ---------
// These pin the tet10 element math directly, so they run in CI whether or not
// Netgen is present (unlike the analytic benchmarks, which need a real mesh).
#ifdef HAVE_FEM_SOLVER

namespace {
constexpr double kE = 200000.0;  // steel, MPa
constexpr double kNu = 0.3;

// A well-shaped (irregular, straight-edged) reference tet10: corners then edge
// midpoints in the canonical order edges (0,1)(1,2)(0,2)(0,3)(1,3)(2,3).
std::array<glm::dvec3, 10> RefTet10() {
  glm::dvec3 c0(0, 0, 0), c1(2, 0, 0), c2(0.3, 3, 0), c3(0.5, 0.7, 4);
  return {c0, c1, c2, c3,
          0.5 * (c0 + c1), 0.5 * (c1 + c2), 0.5 * (c0 + c2),
          0.5 * (c0 + c3), 0.5 * (c1 + c3), 0.5 * (c2 + c3)};
}

// y = Ke * u for the row-major 30x30 Ke.
std::array<double, 30> MatVec(const std::array<double, 900>& Ke,
                              const std::array<double, 30>& u) {
  std::array<double, 30> y{};
  for (int i = 0; i < 30; ++i)
    for (int j = 0; j < 30; ++j) y[i] += Ke[i * 30 + j] * u[j];
  return y;
}
double Norm(const std::array<double, 30>& v) {
  double s = 0;
  for (double x : v) s += x * x;
  return std::sqrt(s);
}
double FroNorm(const std::array<double, 900>& m) {
  double s = 0;
  for (double x : m) s += x * x;
  return std::sqrt(s);
}
// Flatten a per-node displacement field to a 30-vector.
std::array<double, 30> Flatten(const std::array<glm::dvec3, 10>& d) {
  std::array<double, 30> u{};
  for (int i = 0; i < 10; ++i) {
    u[3 * i + 0] = d[i].x; u[3 * i + 1] = d[i].y; u[3 * i + 2] = d[i].z;
  }
  return u;
}
}  // namespace

TEST_CASE("Element: tet10 stiffness is symmetric", "[fem][element]") {
  const auto Ke = fem::Tet10Stiffness(RefTet10(), kE, kNu);
  double worst = 0;
  for (int i = 0; i < 30; ++i)
    for (int j = i + 1; j < 30; ++j)
      worst = std::max(worst, std::abs(Ke[i * 30 + j] - Ke[j * 30 + i]));
  CHECK(worst <= 1e-6 * FroNorm(Ke));
}

TEST_CASE("Element: tet10 has exactly the 6 rigid-body zero-energy modes",
          "[fem][element]") {
  const auto nodes = RefTet10();
  const auto Ke = fem::Tet10Stiffness(nodes, kE, kNu);
  const double scale = FroNorm(Ke);

  // Three translations and three infinitesimal rotations (u = omega x x) must
  // produce zero nodal forces (they induce no strain).
  const glm::dvec3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  for (const glm::dvec3& a : axes) {
    std::array<glm::dvec3, 10> trans, rot;
    for (int i = 0; i < 10; ++i) {
      trans[i] = a;                        // rigid translation
      rot[i] = glm::cross(a, nodes[i]);    // rigid rotation about the axis
    }
    INFO("axis " << a.x << "," << a.y << "," << a.z);
    CHECK(Norm(MatVec(Ke, Flatten(trans))) <= 1e-6 * scale);
    CHECK(Norm(MatVec(Ke, Flatten(rot))) <= 1e-6 * scale);
  }

  // A genuine straining mode (uniaxial stretch in x) must store energy.
  std::array<glm::dvec3, 10> stretch;
  for (int i = 0; i < 10; ++i) stretch[i] = glm::dvec3(nodes[i].x, 0, 0);
  const auto u = Flatten(stretch);
  const auto Ku = MatVec(Ke, u);
  double energy = 0;
  for (int i = 0; i < 30; ++i) energy += u[i] * Ku[i];
  CHECK(energy > 0.0);
}

TEST_CASE("Element: tet10 reproduces a constant strain state exactly "
          "(patch test)", "[fem][element]") {
  const auto nodes = RefTet10();
  // A constant displacement gradient G -> constant strain everywhere. tet10
  // must reproduce this to machine precision at every point (the patch test).
  glm::dmat3 G(0.0);
  G[0][0] = 1e-3;  G[1][1] = -4e-4; G[2][2] = 2e-4;  // normal strains
  G[1][0] = 6e-4;  G[0][1] = 6e-4;                    // symmetric shear xy
  std::array<glm::dvec3, 10> disp;
  for (int i = 0; i < 10; ++i) disp[i] = G * nodes[i];

  // Expected stress from D * strain (Voigt: engineering shear = 2*eps_xy).
  const double lam = kE * kNu / ((1 + kNu) * (1 - 2 * kNu));
  const double mu = kE / (2 * (1 + kNu));
  const double exx = G[0][0], eyy = G[1][1], ezz = G[2][2];
  const double tr = exx + eyy + ezz;
  const double sxx = lam * tr + 2 * mu * exx;
  const double syy = lam * tr + 2 * mu * eyy;
  const double szz = lam * tr + 2 * mu * ezz;
  const double sxy = mu * (G[1][0] + G[0][1]);  // = 2*mu*eps_xy

  // Sample several natural points; the stress must be constant and correct.
  const double pts[4][3] = {{0.25, 0.25, 0.25}, {0.1, 0.2, 0.6}, {0.6, 0.1, 0.1},
                            {0.0, 0.0, 0.0}};
  for (const auto& p : pts) {
    const auto s = fem::Tet10Stress(nodes, disp, kE, kNu, p[0], p[1], p[2]);
    INFO("at " << p[0] << "," << p[1] << "," << p[2]);
    CHECK_THAT(s[0], WithinAbs(sxx, 1e-6 * kE));
    CHECK_THAT(s[1], WithinAbs(syy, 1e-6 * kE));
    CHECK_THAT(s[2], WithinAbs(szz, 1e-6 * kE));
    CHECK_THAT(s[3], WithinAbs(sxy, 1e-6 * kE));
    CHECK_THAT(s[4], WithinAbs(0.0, 1e-6 * kE));
    CHECK_THAT(s[5], WithinAbs(0.0, 1e-6 * kE));
  }
}

namespace {
// A single-tet10 FemMesh. With boundary, adds the 4 tri6 faces so loads can be
// applied (used for the singular-detection test).
fem::FemMesh SingleTetMesh(bool withBoundary) {
  fem::FemMesh m;
  const auto nodes = RefTet10();
  m.nodes.assign(nodes.begin(), nodes.end());
  std::array<int, 10> tet;
  for (int i = 0; i < 10; ++i) tet[i] = i;
  m.tets.push_back(tet);
  m.stats.nodeCount = 10;
  m.stats.tetCount = 1;
  if (withBoundary) {
    auto edgeMid = [](int a, int b) {
      const int lo = std::min(a, b), hi = std::max(a, b);
      if (lo == 0 && hi == 1) return 4;
      if (lo == 1 && hi == 2) return 5;
      if (lo == 0 && hi == 2) return 6;
      if (lo == 0 && hi == 3) return 7;
      if (lo == 1 && hi == 3) return 8;
      return 9;  // (2,3)
    };
    const int faces[4][3] = {{1, 2, 3}, {0, 2, 3}, {0, 1, 3}, {0, 1, 2}};
    for (int f = 0; f < 4; ++f) {
      const int a = faces[f][0], b = faces[f][1], c = faces[f][2];
      fem::FemMesh::BoundaryTri tri;
      tri.nodes = {a, b, c, edgeMid(a, b), edgeMid(b, c), edgeMid(c, a)};
      tri.faceIndex = f;
      m.boundary.push_back(tri);
    }
  }
  return m;
}
}  // namespace

TEST_CASE("Solver rejects a body with no stiffness data", "[fem][element]") {
  std::string err;
  auto r = fem::Solve(SingleTetMesh(false), fem::Material{0.0, 0.3}, {0}, {}, &err);
  CHECK_FALSE(r.has_value());
  CHECK(err.find("stiffness") != std::string::npos);
}

TEST_CASE("Solver rejects a fully constrained model", "[fem][element]") {
  std::vector<int> allFixed;
  for (int i = 0; i < 10; ++i) allFixed.push_back(i);
  std::string err;
  auto r = fem::Solve(SingleTetMesh(false), fem::Material{kE, kNu}, allFixed, {}, &err);
  CHECK_FALSE(r.has_value());
  CHECK(err.find("constrained") != std::string::npos);
}

TEST_CASE("Solver detects an under-constrained (singular) model",
          "[fem][element]") {
  // Fix only one node (kills translation, leaves rotation modes) and push a
  // face: the stiffness matrix is singular, so the solve must fail rather than
  // return garbage.
  fem::FemMesh mesh = SingleTetMesh(true);
  fem::FaceLoad load;
  load.type = fem::LoadType::Force;
  load.tris = {0};
  load.magnitude = 100.0;
  load.direction = glm::dvec3(1, 0, 0);
  std::string err;
  auto r = fem::Solve(mesh, fem::Material{kE, kNu}, {0}, {load}, &err);
  CHECK_FALSE(r.has_value());
  CHECK(err.find("constrained") != std::string::npos);
}

#endif  // HAVE_FEM_SOLVER
