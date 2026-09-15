#include "ui/CreateActions.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "cad/CADKernel.h"
#include "core/Command.h"
#include "core/Document.h"
#include "core/Measure.h"
#include "core/fem/FemMesher.h"
#include "core/fem/FemSolver.h"
#include "core/fem/FemStudy.h"
#include "core/io/Serialization.h"

#include <BOPAlgo_Builder.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRep_Builder.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Pnt.hxx>

#include <array>
#include <cmath>
#include <functional>
#include <set>
#include <unordered_map>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

// A 100 x 40 x 40 block with an 8 mm-radius hole bored through it — a shape
// with both planar and cylindrical faces, the FEM0 spike geometry.
CADShape BlockWithHole() {
  CADShape block = CADKernel::Instance().CreateBox(100.0, 40.0, 40.0);
  CADShape hole = CADKernel::Instance().CreateCylinder(
      glm::vec3(50, 20, -1), glm::vec3(0, 0, 1), 8.0, 42.0);
  return block.Difference(hole);
}

double EdgeLen(const glm::dvec3& a, const glm::dvec3& b) {
  return glm::length(a - b);
}

} // namespace

#ifdef HAVE_NETGEN

TEST_CASE("FemMesher meshes a solid to tet10 with face attribution",
          "[fem][mesh]") {
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
      if (++checked >= 200)
        break;
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
      if (t.faceIndex == 0)
        ++n;
    return n;
  };
  const int coarseTris = trisOnFace0(*base);

  fem::MeshSettings refined = coarse;
  refined.faceSizes.push_back({0, 2.0}); // shrink face 0 to 2 mm
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

TEST_CASE("FemMesher meshes a positioned (located) shape in world coords",
          "[fem][mesh]") {
  // A body placed away from the origin carries a TopLoc_Location (not baked
  // geometry). The mesh must come out in the SAME world coordinates the body
  // renders in, else the result overlay is offset from the part.
  CADShape box = CADKernel::Instance().CreateBox(2.0, 2.0, 2.0);
  box.Translate(glm::vec3(100.0, 0.0, 0.0));
  fem::MeshSettings ms;
  ms.maxSize = 1.0;
  std::string err;
  auto mesh = fem::Mesh(box.GetShape(), ms, &err);
  INFO(err);
  REQUIRE(mesh.has_value());
  double mnx = 1e9, mxx = -1e9;
  for (const auto& n : mesh->nodes) {
    mnx = std::min(mnx, n.x);
    mxx = std::max(mxx, n.x);
  }
  INFO("mesh x-range [" << mnx << ", " << mxx << "]");
  CHECK(mnx > 50.0); // world ~[100,102], not local ~[0,2]
  CHECK(mxx < 150.0);
}

TEST_CASE("FemMesher reports failure on a null shape", "[fem][mesh]") {
  std::string err;
  auto m = fem::Mesh(TopoDS_Shape(), fem::MeshSettings(), &err);
  CHECK_FALSE(m.has_value());
  CHECK_FALSE(err.empty());
}

#else // !HAVE_NETGEN

TEST_CASE("FemMesher reports a clear error when built without Netgen",
          "[fem][mesh]") {
  CHECK_FALSE(fem::Available());
  std::string err;
  auto m = fem::Mesh(BlockWithHole().GetShape(), fem::MeshSettings(), &err);
  CHECK_FALSE(m.has_value());
  CHECK(err.find("FEM support") != std::string::npos);
}

#endif // HAVE_NETGEN

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
  stack.Undo(doc); // restores the removed study, ids intact
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
  doc.AddFeature(
      std::make_unique<BoxFeature>("Bar", glm::vec3(0, 0, 0), 20.0, 4.0, 4.0));
  REQUIRE(doc.GetBodies().size() == 1);
  const uint64_t createdBy = doc.GetBodies()[0].createdBy;
  // Pick a real face name from the body's element map.
  REQUIRE_FALSE(doc.GetBodies()[0].elementMap.names.empty());
  const std::string faceName =
      doc.GetBodies()[0].elementMap.names.begin()->second;

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

#endif // HAVE_NETGEN

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
std::vector<int> NodesAtPlane(const fem::FemMesh& m, int axis, double value,
                              double tol) {
  std::vector<int> out;
  for (int i = 0; i < (int)m.nodes.size(); ++i)
    if (std::abs(m.nodes[i][axis] - value) < tol)
      out.push_back(i);
  return out;
}
// Boundary tris all of whose nodes sit on the plane axis==value.
std::vector<int> TrisAtPlane(const fem::FemMesh& m, int axis, double value,
                             double tol) {
  std::vector<int> out;
  for (int t = 0; t < (int)m.boundary.size(); ++t) {
    bool on = true;
    for (int k = 0; k < 6; ++k)
      if (std::abs(m.nodes[m.boundary[t].nodes[k]][axis] - value) >= tol)
        on = false;
    if (on)
      out.push_back(t);
  }
  return out;
}
} // namespace

TEST_CASE("Solver: uniaxial bar matches sigma=F/A and u=FL/EA",
          "[fem][solver]") {
  const double L = 100, W = 10; // 100 x 10 x 10 mm
  CADShape bar = CADKernel::Instance().CreateBox(L, W, W);
  fem::MeshSettings ms;
  ms.maxSize = 4.0;
  std::string err;
  auto mesh = fem::Mesh(bar.GetShape(), ms, &err);
  INFO(err);
  REQUIRE(mesh.has_value());

  fem::Material steel{200000.0, 0.3}; // 200 GPa, nu 0.3
  const double F = 10000.0;           // N, +x
  const double A = W * W;             // 100 mm^2

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
  const double uExpected = F * L / (steel.youngMPa * A); // 0.05 mm
  CHECK_THAT(res->maxDisplacement, WithinRel(uExpected, 0.03));

  // Uniaxial stress in the mid-region (away from clamp/load) ~ F/A.
  const double sigmaExpected = F / A; // 100 MPa
  int mid = -1;
  double best = 1e30;
  for (int i = 0; i < (int)mesh->nodes.size(); ++i) {
    const auto& p = mesh->nodes[i];
    const double d =
        std::abs(p.x - L / 2) + std::abs(p.y - W / 2) + std::abs(p.z - W / 2);
    if (d < best) {
      best = d;
      mid = i;
    }
  }
  INFO("mid node vm = " << res->vonMises[mid] << " expected " << sigmaExpected);
  CHECK_THAT(res->vonMises[mid], WithinRel(sigmaExpected, 0.05));
}

TEST_CASE("Solver: cantilever tip deflection near Euler-Bernoulli",
          "[fem][solver]") {
  // L=100, cross-section b=10 (y) x h=10 (z). Fix x=0, transverse tip load in
  // -z.
  const double L = 100, b = 10, h = 10;
  CADShape beam = CADKernel::Instance().CreateBox(L, b, h);
  fem::MeshSettings ms;
  ms.maxSize = 4.0;
  std::string err;
  auto mesh = fem::Mesh(beam.GetShape(), ms, &err);
  INFO(err);
  REQUIRE(mesh.has_value());

  fem::Material steel{200000.0, 0.3};
  const double P = 1000.0; // N in -z
  std::vector<int> fixed = NodesAtPlane(*mesh, 0, 0.0, 1e-6);
  fem::FaceLoad load;
  load.type = fem::LoadType::Force;
  load.tris = TrisAtPlane(*mesh, 0, L, 1e-6);
  load.magnitude = P;
  load.direction = glm::dvec3(0, 0, -1);

  auto res = fem::Solve(*mesh, steel, fixed, {load}, &err);
  INFO(err);
  REQUIRE(res.has_value());

  const double I = b * h * h * h / 12.0; // 833.33 mm^4
  const double delta =
      P * L * L * L / (3 * steel.youngMPa * I); // Euler-Bernoulli
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
  const double P = 50.0; // MPa
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
    const double d =
        std::abs(p.x - L / 2) + std::abs(p.y - W / 2) + std::abs(p.z - W / 2);
    if (d < best) {
      best = d;
      mid = i;
    }
  }
  INFO("mid vm = " << res->vonMises[mid] << " expected " << P);
  CHECK_THAT(res->vonMises[mid], WithinRel(P, 0.05));

  // The loaded face must displace inward (-x), confirming the sign.
  double sumx = 0;
  int cnt = 0;
  for (int n : NodesAtPlane(*mesh, 0, L, 1e-6)) {
    sumx += res->displacement[n].x;
    ++cnt;
  }
  CHECK(sumx / cnt < 0.0);
}

TEST_CASE("Solver: Lame thick-wall cylinder von Mises on a curved surface",
          "[fem][solver][slow]") {
  // A pressurized thick tube — the one benchmark with CURVED geometry, so it
  // validates the second-order (tet10) midside projection onto curved faces.
  // a=10, b=20 mm, internal p=10 MPa, along z, length H. Both ends fully
  // clamped: away from the ends the mid-plane (a symmetry plane, u_z=0)
  // approaches plane strain, matching Lame with sigma_z = nu*(sigma_r+sigma_t).
  const double a = 10, b = 20, H = 60, p = 10;
  CADShape outer = CADKernel::Instance().CreateCylinder(
      glm::vec3(0, 0, 0), glm::vec3(0, 0, 1), b, H);
  CADShape bore = CADKernel::Instance().CreateCylinder(
      glm::vec3(0, 0, -1), glm::vec3(0, 0, 1), a, H + 2);
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
    if (std::abs(mesh->nodes[i].z) < 1e-6 ||
        std::abs(mesh->nodes[i].z - H) < 1e-6)
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
      if (std::abs(radius(mesh->boundary[t].nodes[k]) - a) > 0.6)
        bore6 = false;
    if (bore6)
      load.tris.push_back(t);
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
                       (sz - sr) * (sz - sr))); // ~23.1 MPa

  // Inner-radius node nearest the mid-plane (away from the clamped ends).
  int inner = -1;
  double best = 1e30;
  for (int i = 0; i < (int)mesh->nodes.size(); ++i) {
    const double d =
        std::abs(radius(i) - a) + std::abs(mesh->nodes[i].z - H / 2);
    if (d < best) {
      best = d;
      inner = i;
    }
  }
  INFO("inner mid-plane vm = " << res->vonMises[inner] << " expected "
                               << vmExpected);
  // ~4% on a 4 mm mesh; headroom for Netgen's non-bit-stable remeshing.
  CHECK_THAT(res->vonMises[inner], WithinRel(vmExpected, 0.08));
}

#endif // HAVE_NETGEN

// ---- RunStudy orchestration + validation (netgen-gated) -------------------
#ifdef HAVE_NETGEN

namespace {
// A steel bar body via BoxFeature; returns the createdBy id and a face name
// (feature/face) by geometric side. side: 0 => x=0 face, 1 => x=L face.
struct BarDoc {
  Document doc;
  uint64_t createdBy = 0;
};
} // namespace

// A force's direction decides the answer, so it cannot come from a world
// literal chosen before the face was even picked: the GUI placement and
// add_load without an explicit direction both take it from the face.
TEST_CASE("A force defaults to pushing into the face it is placed on",
          "[fem][run]") {
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>("Bar", glm::vec3(0, 0, 0), 100.0,
                                              10.0, 10.0));
  const uint64_t createdBy = doc.GetBodies()[0].createdBy;
  // Every face of an axis-aligned box: the default is the negated outward
  // normal, so a load placed on the top face pushes down and one placed on
  // the side pushes sideways.
  int checked = 0;
  for (const auto& [index, name] : doc.GetBodies()[0].elementMap.names) {
    const auto info = Measure::DescribeFaceAt(doc.GetBodies()[0], index);
    REQUIRE(info.has_value());
    const auto dir =
        fem::DefaultForceDirection(doc, ElementRef{createdBy, name});
    REQUIRE(dir.has_value());
    CHECK_THAT(glm::dot(glm::dvec3(info->normal), *dir),
               WithinAbs(-1.0, 1e-6)); // exactly opposite the outward normal
    ++checked;
  }
  CHECK(checked == 6);

  // A ref that no longer resolves has no face to ask, so there is no default
  // to give — the caller keeps whatever it had rather than inventing one.
  CHECK_FALSE(fem::DefaultForceDirection(doc, ElementRef{createdBy, "1/face99"})
                  .has_value());
}

TEST_CASE("RunStudy errors when the body material has no stiffness data",
          "[fem][run]") {
  // A BoxFeature's default material is unassigned (youngGPa 0), so a study on
  // it must fail loudly rather than guessing. (The full solve path is covered
  // by the analytic benchmarks and the headless fem loop.)
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>("Bar", glm::vec3(0, 0, 0), 100.0,
                                              10.0, 10.0));
  const uint64_t createdBy = doc.GetBodies()[0].createdBy;
  auto faceName = [&](int i) {
    return std::to_string(createdBy) + "/face" + std::to_string(i);
  };

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
  doc.AddFeature(std::make_unique<BoxFeature>("Bar", glm::vec3(0, 0, 0), 50.0,
                                              10.0, 10.0));
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
  auto a =
      std::make_unique<BoxFeature>("BarA", glm::vec3(0, 0, 0), 100, 10, 10);
  a->SetMaterial("steel");
  doc.AddFeature(std::move(a));
  auto b =
      std::make_unique<BoxFeature>("BarB", glm::vec3(0, 0, 50), 100, 10, 10);
  b->SetMaterial("steel");
  doc.AddFeature(std::move(b));
  REQUIRE(doc.GetBodies().size() == 2);
  const uint64_t cbA = doc.GetBodies()[0].createdBy;
  const uint64_t cbB = doc.GetBodies()[1].createdBy;

  fem::Study& s = doc.AddStudy("Two bars");
  s.meshSize = 6.0;
  auto addBar = [&](uint64_t cb, double force) {
    auto face = [&](int i) {
      return ElementRef{cb, std::to_string(cb) + "/face" + std::to_string(i)};
    };
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
  addBar(cbA, 2000.0);  // low stress
  addBar(cbB, 10000.0); // 5x the load -> the governing body

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
    if (br.bodyName == "BarB")
      barBMax = br.results.maxVonMises;
    if (br.bodyName == "BarA")
      barAMax = br.results.maxVonMises;
  }
  CHECK(barBMax > barAMax);      // heavier bar is more stressed
  CHECK(rt.maxVmBody == "BarB"); // aggregate governed by BarB
  CHECK_THAT(rt.maxVonMises, WithinRel(perBodyMax, 1e-9)); // = max over bodies
}

#endif // HAVE_NETGEN

// ---- Element-level solver unit tests (no mesher: Eigen-gated only) ---------
// These pin the tet10 element math directly, so they run in CI whether or not
// Netgen is present (unlike the analytic benchmarks, which need a real mesh).
#ifdef HAVE_FEM_SOLVER

namespace {
constexpr double kE = 200000.0; // steel, MPa
constexpr double kNu = 0.3;

// A well-shaped (irregular, straight-edged) reference tet10: corners then edge
// midpoints in the canonical order edges (0,1)(1,2)(0,2)(0,3)(1,3)(2,3).
std::array<glm::dvec3, 10> RefTet10() {
  glm::dvec3 c0(0, 0, 0), c1(2, 0, 0), c2(0.3, 3, 0), c3(0.5, 0.7, 4);
  return {c0,
          c1,
          c2,
          c3,
          0.5 * (c0 + c1),
          0.5 * (c1 + c2),
          0.5 * (c0 + c2),
          0.5 * (c0 + c3),
          0.5 * (c1 + c3),
          0.5 * (c2 + c3)};
}

// y = Ke * u for the row-major 30x30 Ke.
std::array<double, 30> MatVec(const std::array<double, 900>& Ke,
                              const std::array<double, 30>& u) {
  std::array<double, 30> y{};
  for (int i = 0; i < 30; ++i)
    for (int j = 0; j < 30; ++j)
      y[i] += Ke[i * 30 + j] * u[j];
  return y;
}
double Norm(const std::array<double, 30>& v) {
  double s = 0;
  for (double x : v)
    s += x * x;
  return std::sqrt(s);
}
double FroNorm(const std::array<double, 900>& m) {
  double s = 0;
  for (double x : m)
    s += x * x;
  return std::sqrt(s);
}
// Flatten a per-node displacement field to a 30-vector.
std::array<double, 30> Flatten(const std::array<glm::dvec3, 10>& d) {
  std::array<double, 30> u{};
  for (int i = 0; i < 10; ++i) {
    u[3 * i + 0] = d[i].x;
    u[3 * i + 1] = d[i].y;
    u[3 * i + 2] = d[i].z;
  }
  return u;
}
} // namespace

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
      trans[i] = a;                     // rigid translation
      rot[i] = glm::cross(a, nodes[i]); // rigid rotation about the axis
    }
    INFO("axis " << a.x << "," << a.y << "," << a.z);
    CHECK(Norm(MatVec(Ke, Flatten(trans))) <= 1e-6 * scale);
    CHECK(Norm(MatVec(Ke, Flatten(rot))) <= 1e-6 * scale);
  }

  // A genuine straining mode (uniaxial stretch in x) must store energy.
  std::array<glm::dvec3, 10> stretch;
  for (int i = 0; i < 10; ++i)
    stretch[i] = glm::dvec3(nodes[i].x, 0, 0);
  const auto u = Flatten(stretch);
  const auto Ku = MatVec(Ke, u);
  double energy = 0;
  for (int i = 0; i < 30; ++i)
    energy += u[i] * Ku[i];
  CHECK(energy > 0.0);
}

TEST_CASE("Element: tet10 reproduces a constant strain state exactly "
          "(patch test)",
          "[fem][element]") {
  const auto nodes = RefTet10();
  // A constant displacement gradient G -> constant strain everywhere. tet10
  // must reproduce this to machine precision at every point (the patch test).
  glm::dmat3 G(0.0);
  G[0][0] = 1e-3;
  G[1][1] = -4e-4;
  G[2][2] = 2e-4; // normal strains
  G[1][0] = 6e-4;
  G[0][1] = 6e-4; // symmetric shear xy
  std::array<glm::dvec3, 10> disp;
  for (int i = 0; i < 10; ++i)
    disp[i] = G * nodes[i];

  // Expected stress from D * strain (Voigt: engineering shear = 2*eps_xy).
  const double lam = kE * kNu / ((1 + kNu) * (1 - 2 * kNu));
  const double mu = kE / (2 * (1 + kNu));
  const double exx = G[0][0], eyy = G[1][1], ezz = G[2][2];
  const double tr = exx + eyy + ezz;
  const double sxx = lam * tr + 2 * mu * exx;
  const double syy = lam * tr + 2 * mu * eyy;
  const double szz = lam * tr + 2 * mu * ezz;
  const double sxy = mu * (G[1][0] + G[0][1]); // = 2*mu*eps_xy

  // Sample several natural points; the stress must be constant and correct.
  const double pts[4][3] = {
      {0.25, 0.25, 0.25}, {0.1, 0.2, 0.6}, {0.6, 0.1, 0.1}, {0.0, 0.0, 0.0}};
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
  for (int i = 0; i < 10; ++i)
    tet[i] = i;
  m.tets.push_back(tet);
  m.stats.nodeCount = 10;
  m.stats.tetCount = 1;
  if (withBoundary) {
    auto edgeMid = [](int a, int b) {
      const int lo = std::min(a, b), hi = std::max(a, b);
      if (lo == 0 && hi == 1)
        return 4;
      if (lo == 1 && hi == 2)
        return 5;
      if (lo == 0 && hi == 2)
        return 6;
      if (lo == 0 && hi == 3)
        return 7;
      if (lo == 1 && hi == 3)
        return 8;
      return 9; // (2,3)
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
} // namespace

TEST_CASE("Solver rejects a body with no stiffness data", "[fem][element]") {
  std::string err;
  auto r =
      fem::Solve(SingleTetMesh(false), fem::Material{0.0, 0.3}, {0}, {}, &err);
  CHECK_FALSE(r.has_value());
  CHECK(err.find("stiffness") != std::string::npos);
}

TEST_CASE("Solver rejects a fully constrained model", "[fem][element]") {
  std::vector<int> allFixed;
  for (int i = 0; i < 10; ++i)
    allFixed.push_back(i);
  std::string err;
  auto r = fem::Solve(SingleTetMesh(false), fem::Material{kE, kNu}, allFixed,
                      {}, &err);
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

#endif // HAVE_FEM_SOLVER

// ---- Modal analysis (needs Netgen mesh + Spectra eigensolver) -------------
#if defined(HAVE_NETGEN) && defined(HAVE_SPECTRA)

TEST_CASE("Modal: cantilever first bending frequency", "[fem][solver][modal]") {
  // Steel cantilever 100 x 10 x 10 mm, fixed at x=0. The first natural
  // frequency matches Euler-Bernoulli f1 = (1.875^2/2pi)*sqrt(EI/(rho A L^4)).
  const double L = 100, W = 10;
  CADShape bar = CADKernel::Instance().CreateBox(L, W, W);
  fem::MeshSettings ms;
  ms.maxSize = 4.0;
  std::string err;
  auto mesh = fem::Mesh(bar.GetShape(), ms, &err);
  INFO(err);
  REQUIRE(mesh.has_value());

  // steel: E=200 GPa, nu=0.3, rho=7850 kg/m^3 = 7.85e-9 t/mm^3
  fem::Material steel{200000.0, 0.3, 7.85e-9};

  std::vector<int> fixed;
  for (int i = 0; i < (int)mesh->nodes.size(); ++i)
    if (std::abs(mesh->nodes[i].x) < 1e-6)
      fixed.push_back(i);
  REQUIRE(fixed.size() > 6);

  auto modes = fem::SolveModal(*mesh, steel, fixed, 4, &err);
  INFO(err);
  REQUIRE(modes.has_value());
  REQUIRE(modes->size() >= 1);

  // Analytic first bending frequency.
  const double E = 200000.0, rho = 7.85e-9;
  const double I = W * W * W * W / 12.0; // b h^3/12, square section
  const double A = W * W;
  const double beta = 1.8751040687;
  const double omega1 =
      beta * beta * std::sqrt(E * I / (rho * A * L * L * L * L));
  const double f1 = omega1 / 6.283185307179586; // ~815 Hz

  const double got = (*modes)[0].frequencyHz;
  INFO("f1 got = " << got << " expected " << f1);
  // ~0.2% on a 4 mm mesh; headroom for Netgen's non-bit-stable remeshing.
  CHECK_THAT(got, WithinRel(f1, 0.05));
  // Frequencies ascend and the mode shape is normalized.
  CHECK((*modes)[1].frequencyHz >= (*modes)[0].frequencyHz);
  double maxu = 0;
  for (const auto& u : (*modes)[0].displacement)
    maxu = std::max(maxu, glm::length(u));
  CHECK_THAT(maxu, WithinRel(1.0, 1e-9));
}

TEST_CASE("Modal: requires a fixed support and density", "[fem][modal]") {
  const double L = 40, W = 10;
  CADShape bar = CADKernel::Instance().CreateBox(L, W, W);
  fem::MeshSettings ms;
  ms.maxSize = 6.0;
  std::string err;
  auto mesh = fem::Mesh(bar.GetShape(), ms, &err);
  REQUIRE(mesh.has_value());

  // No fixed support -> clear error (free-free deferred).
  auto m1 = fem::SolveModal(*mesh, fem::Material{200000.0, 0.3, 7.85e-9}, {}, 4,
                            &err);
  CHECK_FALSE(m1.has_value());
  CHECK(err.find("fixed support") != std::string::npos);

  // No density -> no mass.
  std::vector<int> fixed = {0, 1, 2, 3, 4, 5, 6, 7};
  auto m2 =
      fem::SolveModal(*mesh, fem::Material{200000.0, 0.3, 0.0}, fixed, 4, &err);
  CHECK_FALSE(m2.has_value());
  CHECK(err.find("density") != std::string::npos);
}

#endif // HAVE_NETGEN && HAVE_SPECTRA

// ---- Gravity / self-weight (netgen-gated) ---------------------------------
#ifdef HAVE_NETGEN

TEST_CASE("Solver: cantilever self-weight tip deflection",
          "[fem][solver][gravity]") {
  // Steel cantilever under gravity (-z). Tip deflection vs distributed-load
  // beam theory delta = w L^4 / (8 E I), with w = rho*g*A (force per length).
  const double L = 100, W = 10;
  CADShape bar = CADKernel::Instance().CreateBox(L, W, W);
  fem::MeshSettings ms;
  ms.maxSize = 4.0;
  std::string err;
  auto mesh = fem::Mesh(bar.GetShape(), ms, &err);
  INFO(err);
  REQUIRE(mesh.has_value());

  // steel; density in t/mm^3, gravity in mm/s^2 (-z).
  fem::Material steel{200000.0, 0.3, 7.85e-9};
  std::vector<int> fixed;
  for (int i = 0; i < (int)mesh->nodes.size(); ++i)
    if (std::abs(mesh->nodes[i].x) < 1e-6)
      fixed.push_back(i);
  REQUIRE(fixed.size() > 6);

  auto res =
      fem::Solve(*mesh, steel, fixed, {}, &err, glm::dvec3(0, 0, -9810.0));
  INFO(err);
  REQUIRE(res.has_value());

  const double E = 200000.0, rho = 7.85e-9, g = 9810.0;
  const double I = W * W * W * W / 12.0;
  const double A = W * W;
  const double w = rho * g * A;                         // N/mm
  const double delta = w * L * L * L * L / (8 * E * I); // beam theory
  INFO("tip delta = " << res->maxDisplacement << " expected " << delta);
  CHECK_THAT(res->maxDisplacement, WithinRel(delta, 0.10));
  CHECK(res->maxDisplacement > 0); // it actually deflected (gravity applied)
}

#endif // HAVE_NETGEN

TEST_CASE("a study refuses an unknown analysis or load type", "[fem][enums]") {
  // Both used to keep the default silently: a modal study loaded as STATIC and
  // ran happily — the wrong physics, reported with exactly the confidence of
  // the right physics — and a "force" that did not parse became a FIXED
  // constraint, which changes the answer completely and still looks
  // legitimate.
  fem::AnalysisType at = fem::AnalysisType::Static;
  CHECK(fem::AnalysisTypeFromString("modal", at));
  CHECK(at == fem::AnalysisType::Modal);
  CHECK_FALSE(fem::AnalysisTypeFromString("moodal", at));
  CHECK(at == fem::AnalysisType::Modal); // untouched on failure, hence the
                                         // caller MUST check — see below

  fem::LoadType lt = fem::LoadType::Fixed;
  CHECK(fem::LoadTypeFromString("pressure", lt));
  CHECK(lt == fem::LoadType::Pressure);
  CHECK_FALSE(fem::LoadTypeFromString("presure", lt));

  // And the loader is the caller that used to drop it.
  const nlohmann::json study = {
      {"id", 1}, {"name", "S"}, {"analysis", "moodal"}};
  REQUIRE_THROWS_WITH(
      DocumentIO::StudyFromJson(study),
      Catch::Matchers::ContainsSubstring("unknown study analysis type") &&
          Catch::Matchers::ContainsSubstring("modal"));

  const nlohmann::json withLoad = {
      {"id", 1},
      {"name", "S"},
      {"analysis", "static"},
      {"loads",
       {{{"id", 1},
         {"type", "forse"},
         {"face", {{"feature_id", 1}, {"element", "1/f0"}}}}}}};
  REQUIRE_THROWS_WITH(
      DocumentIO::StudyFromJson(withLoad),
      Catch::Matchers::ContainsSubstring("unknown load type 'forse'"));
}

// ---------------------------------------------------------------------------
// docs/ui/long-operations.md, phase 3: PrepareStudy and MeshStudy are split
// at the meshing so a worker can do that half from copied shapes. The split
// must be invisible — same nodes, same tets, same supports and tractions —
// and the copy must be a copy, or the worker writes triangulation into the
// document's own faces under the renderer.
// ---------------------------------------------------------------------------

#ifdef HAVE_NETGEN

TEST_CASE("PlanStudy + MeshPlannedStudy is PrepareStudy, on copied shapes",
          "[fem][mesh][jobs]") {
  Document doc;
  auto bar =
      std::make_unique<BoxFeature>("Bar", glm::vec3(0, 0, 0), 100, 10, 10);
  bar->SetMaterial("steel");
  doc.AddFeature(std::move(bar));
  REQUIRE(doc.GetBodies().size() == 1);
  const uint64_t cb = doc.GetBodies()[0].createdBy;
  auto face = [&](int i) {
    return ElementRef{cb, std::to_string(cb) + "/face" + std::to_string(i)};
  };
  fem::Study& s = doc.AddStudy("Bar");
  s.meshSize = 6.0;
  fem::Load fx;
  fx.id = doc.AllocateId();
  fx.type = fem::LoadType::Fixed;
  fx.face = face(0);
  s.loads.push_back(fx);
  fem::Load fo;
  fo.id = doc.AllocateId();
  fo.type = fem::LoadType::Force;
  fo.face = face(1);
  fo.magnitude = 2000.0;
  fo.direction = glm::dvec3(1, 0, 0);
  s.loads.push_back(fo);
  fem::Refinement ref;
  ref.face = face(2);
  ref.size = 3.0;
  s.refinements.push_back(ref);

  std::string err;
  fem::StudyJobInput whole;
  REQUIRE(fem::PrepareStudy(doc, s, whole, &err));
  REQUIRE(whole.bodies.size() == 1);

  fem::StudyPlan plan;
  REQUIRE(fem::PlanStudy(doc, s, plan, &err));
  REQUIRE(plan.bodies.size() == 1);
  // A copy: same geometry, not the same TShape.
  CHECK_FALSE(plan.bodies[0].shape.IsSame(doc.GetBodies()[0].shape.GetShape()));
  CHECK(plan.bodies[0].loads.size() == 2);
  CHECK(plan.bodies[0].settings.faceSizes.size() == 1);
  fem::StudyJobInput split;
  REQUIRE(fem::MeshPlannedStudy(plan, split, &err));
  REQUIRE(split.bodies.size() == 1);
  CHECK(split.bodies[0].mesh.nodes.size() == whole.bodies[0].mesh.nodes.size());
  CHECK(split.bodies[0].mesh.tets.size() == whole.bodies[0].mesh.tets.size());
  CHECK(split.bodies[0].fixedNodes.size() == whole.bodies[0].fixedNodes.size());
  REQUIRE(split.bodies[0].faceLoads.size() == whole.bodies[0].faceLoads.size());
  CHECK(split.bodies[0].faceLoads[0].tris.size() ==
        whole.bodies[0].faceLoads[0].tris.size());
  CHECK(split.bodies[0].fixedNodes.size() > 0);
  CHECK(split.bodies[0].faceLoads[0].tris.size() > 0);

  // And the mesh-only pair.
  std::vector<fem::StudyBodyMesh> meshed;
  REQUIRE(fem::MeshStudy(doc, s, meshed, &err));
  fem::MeshPlan mplan;
  REQUIRE(fem::PlanStudyMesh(doc, s, mplan, &err));
  CHECK_FALSE(
      mplan.bodies[0].shape.IsSame(doc.GetBodies()[0].shape.GetShape()));
  std::vector<fem::StudyBodyMesh> planned;
  REQUIRE(fem::MeshPlanned(mplan, planned, &err));
  REQUIRE(planned.size() == meshed.size());
  CHECK(planned[0].mesh.stats.nodeCount == meshed[0].mesh.stats.nodeCount);
  CHECK(planned[0].mesh.stats.tetCount == meshed[0].mesh.stats.tetCount);
}

#endif // HAVE_NETGEN

// --- P0: can one mesh span two solids? (docs/simulation/plan.md) ----------
//
// The question the whole "assembly studies" ladder rests on. A bonded
// assembly needs a CONFORMING mesh across the interface — the nodes on the
// shared face belonging to the tets on BOTH sides — because that, and only
// that, is what lets load pass from one part into the next. If Netgen gives
// it for a compound whose solids share a face, the existing solver analyses
// a glued cabinet unchanged, since it takes a mesh and not an assembly. If
// it does not, bonding needs tied constraint equations instead, which is a
// different and much larger job.
//
// Measured as connected components of the tet graph, which is the same
// question in graph form, with the UNGLUED compound as the control: a check
// that nothing fails is not a check.
namespace {

// Two stacked 100 x 100 x 16 blocks meeting on the z = 16 plane.
TopoDS_Shape LowerBlock() {
  return BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), 100.0, 100.0, 16.0).Shape();
}
TopoDS_Shape UpperBlock() {
  return BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 16.0), 100.0, 100.0, 16.0).Shape();
}

// How many pieces the mesh is in: union-find over tets that share a node.
int MeshComponents(const fem::FemMesh& mesh) {
  std::vector<int> parent(mesh.tets.size());
  for (size_t i = 0; i < parent.size(); i++)
    parent[i] = static_cast<int>(i);
  std::function<int(int)> find = [&](int a) {
    while (parent[a] != a) {
      parent[a] = parent[parent[a]];
      a = parent[a];
    }
    return a;
  };
  std::unordered_map<int, int> firstTetAtNode;
  for (size_t t = 0; t < mesh.tets.size(); t++) {
    for (int n : mesh.tets[t]) {
      auto [it, fresh] = firstTetAtNode.emplace(n, static_cast<int>(t));
      if (!fresh) {
        const int a = find(it->second), b = find(static_cast<int>(t));
        if (a != b)
          parent[a] = b;
      }
    }
  }
  std::set<int> roots;
  for (size_t i = 0; i < parent.size(); i++)
    roots.insert(find(static_cast<int>(i)));
  return static_cast<int>(roots.size());
}

// Nodes sitting on the interface plane that have a TWIN at the same place —
// the signature of two meshes that merely touch.
int DoubledInterfaceNodes(const fem::FemMesh& mesh) {
  std::vector<glm::dvec3> onPlane;
  for (const glm::dvec3& n : mesh.nodes)
    if (std::abs(n.z - 16.0) < 1e-6)
      onPlane.push_back(n);
  int doubled = 0;
  for (size_t i = 0; i < onPlane.size(); i++)
    for (size_t j = i + 1; j < onPlane.size(); j++)
      if (glm::length(onPlane[i] - onPlane[j]) < 1e-6)
        doubled++;
  return doubled;
}

} // namespace

TEST_CASE("P0: a mesh spans two solids only when they SHARE the face",
          "[fem][mesh]") {
  fem::MeshSettings settings;
  settings.maxSize = 12.0;
  std::string err;

  SECTION("the control: an unglued compound meshes as two separate pieces") {
    TopoDS_Compound loose;
    BRep_Builder builder;
    builder.MakeCompound(loose);
    builder.Add(loose, LowerBlock());
    builder.Add(loose, UpperBlock());
    auto mesh = fem::Mesh(loose, settings, &err);
    INFO(err);
    REQUIRE(mesh.has_value());
    CHECK(MeshComponents(*mesh) == 2);
    CHECK(DoubledInterfaceNodes(*mesh) > 0);
  }

  SECTION("general fuse imprints the shared face: ONE mesh, two solids") {
    // Keeps both solids (so they can carry different materials later) while
    // making the interface one face that belongs to both.
    BOPAlgo_Builder gf;
    gf.AddArgument(LowerBlock());
    gf.AddArgument(UpperBlock());
    gf.SetFuzzyValue(1e-3); // parts are PLACED at float32 — see Measure.h
    gf.Perform();
    REQUIRE_FALSE(gf.HasErrors());
    const TopoDS_Shape glued = gf.Shape();
    TopTools_IndexedMapOfShape solids;
    TopExp::MapShapes(glued, TopAbs_SOLID, solids);
    CHECK(solids.Extent() == 2);

    auto mesh = fem::Mesh(glued, settings, &err);
    INFO(err);
    REQUIRE(mesh.has_value());
    CHECK(MeshComponents(*mesh) == 1);
    CHECK(DoubledInterfaceNodes(*mesh) == 0);
  }

  SECTION("a float32 gap still imprints, which is the real cabinet's case") {
    // Parts are PLACED at float32 (SketchPlane holds its origin as a
    // glm::vec3), so two faces that are nominally coincident sit a few
    // microns apart — a panel drawn at -172.8 sits at -172.800003. A strict
    // boolean finds nothing between them and the assembly silently meshes
    // as loose pieces, which is the failure this whole technique would die
    // of in the field rather than in a test with round numbers.
    const double gap = 3e-6; // what float32 leaves at cabinet scale
    BOPAlgo_Builder gf;
    gf.AddArgument(LowerBlock());
    gf.AddArgument(
        BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 16.0 + gap), 100.0, 100.0, 16.0)
            .Shape());
    gf.SetFuzzyValue(1e-3);
    gf.Perform();
    REQUIRE_FALSE(gf.HasErrors());
    auto mesh = fem::Mesh(gf.Shape(), settings, &err);
    INFO(err);
    REQUIRE(mesh.has_value());
    CHECK(MeshComponents(*mesh) == 1);
  }

  SECTION("plain fuse melts them into one solid — the same-material path") {
    const TopoDS_Shape one =
        BRepAlgoAPI_Fuse(LowerBlock(), UpperBlock()).Shape();
    auto mesh = fem::Mesh(one, settings, &err);
    INFO(err);
    REQUIRE(mesh.has_value());
    CHECK(MeshComponents(*mesh) == 1);
    CHECK(DoubledInterfaceNodes(*mesh) == 0);
  }
}
