// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// Topology you can build from
// (docs/topopt/README.md#topology-you-can-build-from): load cases, keep-outs,
// the connectivity filter, a static study of the optimised shape, and the split
// for printing.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestIO.h"
#include "core/Document.h"
#include "core/Feature.h"
#include "core/PrintSplit.h"
#include "core/fem/FemSolver.h"
#include "core/fem/FemStudy.h"
#include "core/fem/FemTopo.h"
#include "core/io/Serialization.h"
#include "topo_testmesh.h"

#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <gp_Pnt.hxx>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

// The tip face's lower-third patch of a structured cantilever, as the gate
// loads it.
fem::FaceLoad TipLoad(const fem::FemMesh& mesh, double Lx, glm::dvec3 dir,
                      double newtons) {
  fem::FaceLoad load;
  load.type = fem::LoadType::Force;
  load.tris = TrisAtPlane(mesh, 0, Lx, 1e-6);
  load.magnitude = newtons;
  load.direction = dir;
  return load;
}

// Signed volume enclosed by a mesh's boundary triangles (corner nodes), by
// the divergence theorem: + for outward winding, - for inward.
double BoundaryVolume(const fem::FemMesh& m) {
  double v = 0.0;
  for (const auto& t : m.boundary) {
    const glm::dvec3 a = m.nodes[t.nodes[0]], b = m.nodes[t.nodes[1]],
                     c = m.nodes[t.nodes[2]];
    v += glm::dot(a, glm::cross(b, c)) / 6.0;
  }
  return v;
}

} // namespace

// ---- Sub-meshes and connectivity (pure geometry) -------------------------

TEST_CASE("ExtractSubMesh closes the cut, wound as the mesh is",
          "[topopt][topobuild]") {
  const fem::FemMesh mesh = GenerateBoxMesh(4, 3, 3, 40, 30, 30);
  std::vector<char> keep(mesh.tets.size(), 0);
  for (size_t e = 0; e < mesh.tets.size(); ++e) {
    glm::dvec3 c(0.0);
    for (int i = 0; i < 4; ++i)
      c += 0.25 * mesh.nodes[mesh.tets[e][i]];
    keep[e] = c.x < 20.0;
  }
  std::vector<int> map;
  const fem::FemMesh sub = fem::ExtractSubMesh(mesh, keep, &map);
  CHECK(sub.tets.size() == mesh.tets.size() / 2);
  CHECK(map.size() == sub.tets.size());
  // A closed surface: 2 x 3 x 3 hexes have 2 (6 + 6 + 9) quads, 84 triangles.
  CHECK(sub.boundary.size() == 84);
  // Wound like the original: the same sign of enclosed volume, half of it.
  const double full = BoundaryVolume(mesh), half = BoundaryVolume(sub);
  CHECK(full * half > 0.0);
  CHECK_THAT(std::abs(half), WithinRel(20.0 * 30.0 * 30.0, 1e-9));
  // The cut face is new (-1); the box faces keep their indices.
  int fresh = 0;
  for (const auto& t : sub.boundary)
    fresh += t.faceIndex == -1;
  CHECK(fresh == 2 * 3 * 3);
  CHECK_THAT(fem::TetSetVolume(mesh, keep), WithinRel(18000.0, 1e-9));
}

TEST_CASE("RemoveIslands takes what holds no support and no load",
          "[topopt][topobuild]") {
  // Three solid blocks along x, separated by void: the first holds the
  // support, the third a load; the middle one holds neither.
  const fem::FemMesh mesh = GenerateBoxMesh(9, 1, 1, 90, 10, 10);
  const size_t n = mesh.tets.size();
  std::vector<double> density(n, 0.0);
  std::vector<char> supported(n, 0), loaded(n, 0);
  for (size_t e = 0; e < n; ++e) {
    glm::dvec3 c(0.0);
    for (int i = 0; i < 4; ++i)
      c += 0.25 * mesh.nodes[mesh.tets[e][i]];
    const int cell = static_cast<int>(c.x / 10.0);
    if (cell == 0 || cell == 4 || cell == 8)
      density[e] = 1.0;
    supported[e] = cell == 0;
    loaded[e] = cell == 8;
  }
  std::vector<int> removed;
  const fem::TopoIslands is =
      fem::RemoveIslands(mesh, density, 0.5, supported, loaded, &removed);
  CHECK(is.components == 1);
  CHECK(is.tets == 6);
  CHECK(removed.size() == 6);
  CHECK_THAT(is.volume, WithinRel(1000.0, 1e-9));
  CHECK(is.loadPathBroken); // the loaded block reaches no support
  int left = 0;
  for (double d : density)
    left += d > 0.5;
  CHECK(left == 12);

  SECTION("joined by a bar: one piece, nothing removed, the path whole") {
    std::fill(density.begin(), density.end(), 1.0);
    const fem::TopoIslands whole =
        fem::RemoveIslands(mesh, density, 0.5, supported, loaded);
    CHECK(whole.components == 0);
    CHECK(whole.tets == 0);
    CHECK_FALSE(whole.loadPathBroken);
  }
}

#ifdef HAVE_FEM_SOLVER

TEST_CASE("SolveCases answers each case as its own solve would",
          "[fem][topobuild]") {
  const double Lx = 60;
  const fem::FemMesh mesh = GenerateBoxMesh(6, 2, 2, Lx, 10, 10);
  const fem::Material steel{210000.0, 0.3};
  const std::vector<int> fixed = NodesAtPlane(mesh, 0, 0.0, 1e-6);
  const fem::FaceLoad down = TipLoad(mesh, Lx, {0, 0, -1}, 100.0);
  const fem::FaceLoad side = TipLoad(mesh, Lx, {0, 1, 0}, 50.0);
  std::string err;
  auto both = fem::SolveCases(mesh, {steel}, fixed, {{down}, {side}}, &err);
  REQUIRE(both);
  REQUIRE(both->size() == 2);
  auto a = fem::Solve(mesh, steel, fixed, {down}, &err);
  auto b = fem::Solve(mesh, steel, fixed, {side}, &err);
  REQUIRE((a && b));
  CHECK_THAT((*both)[0].maxDisplacement, WithinRel(a->maxDisplacement, 1e-9));
  CHECK_THAT((*both)[1].maxDisplacement, WithinRel(b->maxDisplacement, 1e-9));
  CHECK_THAT((*both)[1].maxVonMises, WithinRel(b->maxVonMises, 1e-9));
}

TEST_CASE("Topo: two load cases make a design that carries both",
          "[topopt][topobuild][slow]") {
  // A cantilever loaded down in one case and sideways in the other. With
  // the sideways case weighted to almost nothing the layout is the down
  // case's alone; weighted equally, the design pays a little in the down
  // case for a much stiffer one sideways.
  const double Lx = 96, Ly = 24, Lz = 24;
  const fem::FemMesh mesh = GenerateBoxMesh(16, 4, 4, Lx, Ly, Lz);
  const fem::Material steel{210000.0, 0.3};
  const std::vector<int> fixed = NodesAtPlane(mesh, 0, 0.0, 1e-6);
  fem::TopoParams params;
  params.volumeFraction = 0.3;
  params.filterRadius = 12.0;
  params.maxIterations = 25;
  const auto run = [&](double sideWeight) {
    std::vector<fem::TopoCase> cases(2);
    cases[0] = {"down", 1.0, {TipLoad(mesh, Lx, {0, 0, -1}, 1000.0)}};
    cases[1] = {"side", sideWeight, {TipLoad(mesh, Lx, {0, 1, 0}, 1000.0)}};
    std::string err;
    auto r = fem::TopologyOptimize(mesh, steel, fixed, cases, params, &err, {},
                                   {}, nullptr);
    INFO(err);
    REQUIRE(r);
    return *r;
  };
  const fem::TopoResult alone = run(1e-6);
  const fem::TopoResult together = run(1.0);
  REQUIRE(alone.caseCompliance.size() == 2);
  REQUIRE(together.caseCompliance.size() == 2);
  // The objective each reports is the weighted sum of its cases.
  CHECK_THAT(
      together.complianceHistory.back(),
      WithinRel(together.caseCompliance[0] + together.caseCompliance[1], 1e-9));
  // Sideways, the design that was asked to carry it is far stiffer, at a
  // price downwards...
  CHECK(together.caseCompliance[1] < 0.7 * alone.caseCompliance[1]);
  CHECK(together.caseCompliance[0] > alone.caseCompliance[0]);
  // ...and on the two cases together it is the better design.
  CHECK(together.caseCompliance[0] + together.caseCompliance[1] <
        0.8 * (alone.caseCompliance[0] + alone.caseCompliance[1]));
  CHECK_THAT(together.volumeFractionAchieved, WithinAbs(0.3, 0.02));
}

TEST_CASE("Topo: a keep face nothing connects is removed and reported",
          "[topopt][topobuild]") {
  // A short cantilever with a keep patch on the free top face far from both
  // the support and the load: frozen solid, it would print as a floating
  // pad. The run hands it back removed.
  const double Lx = 60, Ly = 20, Lz = 20;
  const fem::FemMesh mesh = GenerateBoxMesh(12, 4, 4, Lx, Ly, Lz);
  const fem::Material steel{210000.0, 0.3};
  const std::vector<int> fixed = NodesAtPlane(mesh, 0, 0.0, 1e-6);
  fem::FaceLoad load = TipLoad(mesh, Lx, {0, 0, -1}, 1000.0);
  load.tris.erase(
      std::remove_if(load.tris.begin(), load.tris.end(),
                     [&](int t) {
                       double z = 0;
                       for (int k = 0; k < 3; ++k)
                         z += mesh.nodes[mesh.boundary[t].nodes[k]].z;
                       return z / 3.0 > Lz / 3.0;
                     }),
      load.tris.end());
  // The keep patch: top-face nodes around x = 40, y = 10 (z = Lz).
  std::vector<int> keep;
  for (int v : NodesAtPlane(mesh, 2, Lz, 1e-6)) {
    const glm::dvec3& p = mesh.nodes[v];
    if (std::abs(p.x - 35.0) <= 2.5 + 1e-9 && std::abs(p.y - 10.0) <= 2.5)
      keep.push_back(v);
  }
  REQUIRE_FALSE(keep.empty());
  fem::TopoParams params;
  params.volumeFraction = 0.4;
  params.filterRadius = 6.0;
  params.maxIterations = 30;
  std::string err;
  auto r = fem::TopologyOptimize(mesh, steel, fixed, {load}, params, &err, {},
                                 keep, nullptr);
  INFO(err);
  REQUIRE(r);
  // Whatever the optimiser made of the patch, nothing it hands back floats:
  // every piece solid at the threshold holds a support or a load.
  std::vector<char> anchorNode(mesh.nodes.size(), 0);
  for (int v : fixed)
    anchorNode[v] = 1;
  for (int t : load.tris)
    for (int v : mesh.boundary[t].nodes)
      anchorNode[v] = 1;
  std::vector<char> solid(mesh.tets.size(), 0);
  for (size_t e = 0; e < mesh.tets.size(); ++e)
    solid[e] = r->density[e] >= r->islands.threshold;
  int count = 0;
  const std::vector<int> comp = fem::TetComponents(mesh, solid, &count);
  std::vector<char> anchored(count, 0);
  for (size_t e = 0; e < mesh.tets.size(); ++e)
    if (comp[e] >= 0)
      for (int v : mesh.tets[e])
        anchored[comp[e]] |= anchorNode[v];
  for (int c = 0; c < count; ++c)
    CHECK(anchored[c]);
  CHECK_FALSE(r->islands.loadPathBroken);
  // The patch's tets are solid or reported gone, never solid and loose.
  if (r->islands.tets > 0)
    CHECK(r->islands.components >= 1);
}

#endif // HAVE_FEM_SOLVER

// ---- Load-case rules ------------------------------------------------------

TEST_CASE("Load cases refuse what they cannot mean", "[topopt][topobuild]") {
  fem::Study s;
  s.type = fem::AnalysisType::Topology;
  fem::Load fixed;
  fixed.id = 1;
  fixed.type = fem::LoadType::Fixed;
  fem::Load f;
  f.id = 2;
  f.type = fem::LoadType::Force;
  s.loads = {fixed, f};
  std::string err;
  CHECK(fem::ValidateLoadCases(s, &err)); // no cases, none named

  s.loadCases = {{"down", 1.0, ""}, {"side", 2.0, ""}};
  CHECK_FALSE(fem::ValidateLoadCases(s, &err));
  CHECK(err.find("in no load case") != std::string::npos);
  CHECK(err.find("'down', 'side'") != std::string::npos);

  s.loads[1].loadCase = "down";
  CHECK_FALSE(fem::ValidateLoadCases(s, &err));
  CHECK(err.find("'side' has no force") != std::string::npos);

  fem::Load g = f;
  g.id = 3;
  g.loadCase = "side";
  s.loads.push_back(g);
  CHECK(fem::ValidateLoadCases(s, &err));

  s.loads[0].loadCase = "down"; // a support in a case
  CHECK_FALSE(fem::ValidateLoadCases(s, &err));
  CHECK(err.find("names none") != std::string::npos);
  s.loads[0].loadCase.clear();

  s.loadCases[1].weight = 0.0;
  CHECK_FALSE(fem::ValidateLoadCases(s, &err));
  CHECK(err.find("not positive") != std::string::npos);
  s.loadCases[1].weight = 1.0;

  s.loadCases[1].name = "down";
  CHECK_FALSE(fem::ValidateLoadCases(s, &err));
  CHECK(err.find("named twice") != std::string::npos);

  fem::Study modal;
  modal.type = fem::AnalysisType::Modal;
  modal.loadCases = {{"a", 1.0, ""}};
  CHECK_FALSE(fem::ValidateLoadCases(modal, &err));
}

// ---- Serialization -------------------------------------------------------

TEST_CASE("Load cases, keep-outs and shape_from round-trip through JSON",
          "[topopt][topobuild][serialize]") {
  fem::Study s;
  s.id = 7;
  s.name = "Gen";
  s.type = fem::AnalysisType::Topology;
  s.loadCases = {{"down", 1.0, ""}, {"side", 2.0, "w_side"}};
  s.keepOuts.push_back({"worm block", {}, {}});
  s.keepOuts.push_back({"", {1, 2, 3}, {4, 5, 6}});
  fem::Load l;
  l.id = 8;
  l.type = fem::LoadType::Force;
  l.face = ElementRef{2, "2/g1"};
  l.loadCase = "side";
  s.loads.push_back(l);
  const fem::Study r = DocumentIO::StudyFromJson(DocumentIO::StudyToJson(s));
  REQUIRE(r.loadCases.size() == 2);
  CHECK(r.loadCases[1].name == "side");
  CHECK(r.loadCases[1].weightExpr == "w_side");
  REQUIRE(r.keepOuts.size() == 2);
  CHECK(r.keepOuts[0].body == "worm block");
  CHECK(r.keepOuts[1].boxMax == glm::dvec3(4, 5, 6));
  CHECK(r.loads[0].loadCase == "side");

  fem::Study st;
  st.id = 9;
  st.shapeFrom = {7, 0.4, false};
  const fem::Study rs = DocumentIO::StudyFromJson(DocumentIO::StudyToJson(st));
  CHECK(rs.shapeFrom.study == 7);
  CHECK_THAT(rs.shapeFrom.threshold, WithinAbs(0.4, 1e-12));

  // shape_from on anything but a static study is refused, not dropped.
  nlohmann::json bad = DocumentIO::StudyToJson(s);
  bad["shape_from"] = {{"study", 3}};
  CHECK_THROWS(DocumentIO::StudyFromJson(bad));
  nlohmann::json badKo = DocumentIO::StudyToJson(s);
  badKo["keep_outs"] = nlohmann::json::array({{{"cylinder", 3}}});
  CHECK_THROWS(DocumentIO::StudyFromJson(badKo));
}

TEST_CASE("A topology result's cases, keep-outs and islands round-trip",
          "[topopt][topobuild][serialize]") {
  fem::StudyRuntime rt;
  rt.topology = true;
  rt.caseNames = {"down", "side"};
  rt.caseWeights = {1.0, 2.0};
  rt.caseCompliance = {3.5, 7.25};
  rt.keepOuts.push_back({"box", 12, 340.5});
  rt.islands.components = 2;
  rt.islands.tets = 9;
  rt.islands.volume = 88.0;
  rt.islands.keepFaces = {1};
  rt.islands.threshold = 0.42;
  fem::StudyBodyResult b;
  for (int i = 0; i < 10; ++i)
    b.mesh.nodes.push_back(glm::dvec3(i, 0, 0));
  b.mesh.tets.push_back({0, 1, 2, 3, 4, 5, 6, 7, 8, 9});
  b.density = {0.7};
  rt.bodies.push_back(b);
  nlohmann::json j;
  REQUIRE(DocumentIO::StudyResultToJson(rt, j));
  const fem::StudyRuntime r = DocumentIO::StudyResultFromJson(j);
  CHECK(r.caseNames == rt.caseNames);
  CHECK(r.caseWeights == rt.caseWeights);
  CHECK(r.caseCompliance == rt.caseCompliance);
  REQUIRE(r.keepOuts.size() == 1);
  CHECK(r.keepOuts[0].tets == 12);
  CHECK(r.islands.tets == 9);
  CHECK(r.islands.keepFaces == std::vector<int>{1});
  CHECK_THAT(r.islands.threshold, WithinAbs(0.42, 1e-12));
  // The digest a shape study keys on is computed on load.
  CHECK(r.densityDigest == fem::DensityDigest(b.mesh, b.density));
}

// ---- The split for printing ---------------------------------------------

TEST_CASE("A split's pieces fit the bed or the split is refused",
          "[topobuild][printsplit]") {
  const TopoDS_Shape bar =
      BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), gp_Pnt(120, 30, 30)).Shape();
  std::string err;

  SECTION("given cuts make the cells, and the volume is all there") {
    auto plan =
        printsplit::PlanCuts(bar, {50, 50, 50}, {{0, 40.0}, {0, 80.0}}, &err);
    REQUIRE(plan);
    REQUIRE(plan->pieces.size() == 3);
    double v = 0.0;
    for (const auto& p : plan->pieces) {
      v += p.volume;
      CHECK(p.fits);
      CHECK_THAT(p.Size().x, WithinAbs(40.0, 1e-6));
    }
    CHECK_THAT(v, WithinRel(120.0 * 30 * 30, 1e-9));
  }
  SECTION("automatic: the fewest equal slabs along the longest axis") {
    auto plan = printsplit::PlanAutomatic(bar, {50, 50, 50}, &err);
    REQUIRE(plan);
    CHECK(plan->automatic);
    CHECK(plan->pieces.size() == 3);
    auto whole = printsplit::PlanAutomatic(bar, {130, 40, 40}, &err);
    REQUIRE(whole);
    CHECK(whole->fitsWhole);
  }
  SECTION("a bed no slab along the length can fit is refused") {
    CHECK_FALSE(printsplit::PlanAutomatic(bar, {20, 20, 20}, &err));
    CHECK(err.find("does not fit") != std::string::npos);
  }
  SECTION("a cut that misses the body is refused") {
    CHECK_FALSE(printsplit::PlanCuts(bar, {50, 50, 50}, {{1, 45.0}}, &err));
    CHECK(err.find("misses the body") != std::string::npos);
  }
  SECTION("a piece may lie on its side") {
    std::array<int, 3> o{};
    CHECK(printsplit::FitsBed({100, 10, 10}, {20, 120, 20}, &o));
    CHECK(o[1] == 0); // its x along the bed's y
    CHECK_FALSE(printsplit::FitsBed({100, 30, 10}, {20, 120, 20}));
  }
  SECTION("a cell's separate solids are pieces of their own") {
    // A U: two posts on a base, cut above the base.
    TopoDS_Shape u =
        BRepAlgoAPI_Fuse(
            BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), gp_Pnt(60, 10, 10)).Shape(),
            BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), gp_Pnt(10, 10, 60)).Shape())
            .Shape();
    u = BRepAlgoAPI_Fuse(
            u,
            BRepPrimAPI_MakeBox(gp_Pnt(50, 0, 0), gp_Pnt(60, 10, 60)).Shape())
            .Shape();
    auto plan = printsplit::PlanCuts(u, {100, 100, 100}, {{2, 10.0}}, &err);
    REQUIRE(plan);
    // The base, and each post on its own: a box takes each alone.
    REQUIRE(plan->pieces.size() == 3);
    double v = 0.0;
    for (const auto& p : plan->pieces) {
      CHECK(p.solids == 1);
      v += p.volume;
    }
    CHECK_THAT(v, WithinRel(60.0 * 100 + 2 * 50.0 * 100, 1e-9));
    CHECK_THAT(plan->pieces[1].Size().z, WithinAbs(50.0, 1e-6));
  }
}

// ---- Keep-outs and shape studies through a document (Netgen) -------------

#if defined(HAVE_NETGEN) && defined(HAVE_FEM_SOLVER)

namespace {

Document MakeBar() {
  Document doc;
  auto box = std::make_unique<BoxFeature>("Envelope", glm::vec3(0, 0, 0), 80.0,
                                          30.0, 30.0);
  box->SetMaterial("steel");
  doc.AddFeature(std::move(box));
  const uint64_t cb = doc.GetBodies()[0].createdBy;
  const auto face = [&](int i) {
    return ElementRef{cb, std::to_string(cb) + "/face" + std::to_string(i)};
  };
  fem::Study& s = doc.AddStudy("Generative");
  s.type = fem::AnalysisType::Topology;
  s.meshSize = 6.0;
  s.volumeFraction = 0.35;
  s.filterRadius = 12.0;
  s.maxIterations = 20;
  s.loadCases = {{"down", 1.0, ""}, {"side", 1.0, ""}};
  fem::Load fixed;
  fixed.id = doc.AllocateId();
  fixed.type = fem::LoadType::Fixed;
  fixed.face = face(0); // x = 0
  s.loads.push_back(fixed);
  fem::Load down;
  down.id = doc.AllocateId();
  down.type = fem::LoadType::Force;
  down.face = face(1); // x = L
  down.magnitude = 1500.0;
  down.direction = glm::dvec3(0, 0, -1);
  down.loadCase = "down";
  s.loads.push_back(down);
  fem::Load side = down;
  side.id = doc.AllocateId();
  side.direction = glm::dvec3(0, 1, 0);
  side.loadCase = "side";
  s.loads.push_back(side);
  return doc;
}

} // namespace

TEST_CASE("A keep-out leaves its tets out of the design domain",
          "[topopt][topobuild][mesh]") {
  Document doc = MakeBar();
  fem::Study& s = *doc.StudyForRun(doc.GetStudies()[0].id);
  fem::TopoInput plain;
  std::string err;
  REQUIRE(fem::PrepareTopology(doc, s, plain, &err));
  REQUIRE(plain.cases.size() == 2);

  s.keepOuts.push_back({"", {30, -1, 15}, {50, 31, 31}});
  fem::TopoInput cut;
  REQUIRE(fem::PrepareTopology(doc, s, cut, &err));
  REQUIRE(cut.keepOuts.size() == 1);
  CHECK(cut.keepOuts[0].tets > 0);
  CHECK(cut.mesh.tets.size() == plain.mesh.tets.size() - cut.keepOuts[0].tets);
  for (const auto& t : cut.mesh.tets) {
    glm::dvec3 c(0.0);
    for (int i = 0; i < 4; ++i)
      c += 0.25 * cut.mesh.nodes[t[i]];
    CHECK_FALSE((c.x > 30 && c.x < 50 && c.z > 15));
  }

  SECTION("one over the support is refused, naming it") {
    s.keepOuts.back() = {"", {-1, -1, -1}, {4, 31, 31}};
    fem::TopoInput bad;
    CHECK_FALSE(fem::PrepareTopology(doc, s, bad, &err));
    CHECK(err.find("support") != std::string::npos);
  }
  SECTION("a body the part does not have is refused, naming those it has") {
    s.keepOuts.back() = {"worm block", {}, {}};
    fem::TopoInput bad;
    CHECK_FALSE(fem::PrepareTopology(doc, s, bad, &err));
    CHECK(err.find("'Envelope'") != std::string::npos);
  }
}

TEST_CASE("A static study of the optimised shape against the solid",
          "[topopt][topobuild][mesh][slow]") {
  Document doc = MakeBar();
  const uint64_t topoId = doc.GetStudies()[0].id;
  {
    fem::StudyRuntime rt;
    std::string err;
    REQUIRE(fem::RunTopology(doc, *doc.StudyForRun(topoId), rt, &err));
    REQUIRE(rt.caseCompliance.size() == 2);
    rt.solvedAtVersion = doc.Version();
    doc.StoreStudyResults(topoId, std::move(rt));
  }
  const fem::Study topo = *doc.FindStudy(topoId);
  const auto shapeStudy = [&](bool whole) {
    fem::Study& s = doc.AddStudy(whole ? "solid" : "shape");
    s.loads = topo.loads;
    s.loadCases = topo.loadCases;
    s.shapeFrom = {topoId, 0.0, whole};
    return s.id;
  };
  const uint64_t shapeId = shapeStudy(false);
  const uint64_t solidId = shapeStudy(true);
  fem::StudyRuntime shape, solid;
  std::string err;
  REQUIRE(fem::RunStudy(doc, *doc.StudyForRun(shapeId), shape, &err));
  INFO(err);
  REQUIRE(fem::RunStudy(doc, *doc.StudyForRun(solidId), solid, &err));
  REQUIRE(shape.caseNames.size() == 2);
  REQUIRE(shape.bodies.size() == 1);
  REQUIRE(shape.bodies[0].cases.size() == 2);
  // The shape is what the result kept: fewer tets, less volume, softer in
  // both cases than the solid domain, and the solid is the whole bar.
  CHECK(shape.bodies[0].mesh.tets.size() < solid.bodies[0].mesh.tets.size());
  CHECK_THAT(solid.shapeVolume, WithinRel(80.0 * 30 * 30, 1e-6));
  CHECK(shape.shapeVolume < 0.6 * solid.shapeVolume);
  for (int k = 0; k < 2; ++k)
    CHECK(shape.bodies[0].Case(k).maxDisplacement >
          solid.bodies[0].Case(k).maxDisplacement);
  // Each case moves the tip its own way.
  CHECK(shape.bodies[0].Case(0).maxDisplacement > 0.0);
  const fem::FemResults& side = shape.bodies[0].Case(1);
  double dy = 0.0, dz = 0.0;
  for (const glm::dvec3& d : side.displacement) {
    dy = std::max(dy, std::abs(d.y));
    dz = std::max(dz, std::abs(d.z));
  }
  CHECK(dy > dz);

  // Fresh while the topology result is the one it was cut from; stale once
  // that result is replaced.
  shape.solvedAtVersion = doc.Version();
  doc.StoreStudyResults(shapeId, shape);
  CHECK_FALSE(doc.StudyResultsStale(shapeId));
  const uint64_t keyBefore = fem::StudyInputKey(doc, *doc.FindStudy(shapeId));
  fem::StudyRuntime other = *doc.StudyResults(topoId);
  other.bodies[0].density[0] = 0.5 * (other.bodies[0].density[0] + 0.01);
  other.densityDigest =
      fem::DensityDigest(other.bodies[0].mesh, other.bodies[0].density);
  doc.StoreStudyResults(topoId, std::move(other));
  CHECK(doc.StudyResultsStale(shapeId));
  // ...and its input hash moved with it: a run now is not answered from
  // the old one.
  CHECK(fem::StudyInputKey(doc, *doc.FindStudy(shapeId)) != keyBefore);
}

#endif // HAVE_NETGEN && HAVE_FEM_SOLVER
