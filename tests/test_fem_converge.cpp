// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// Mesh quality and a peak that has settled
// (docs/simulation/README.md#meshing,
// docs/simulation/README.md#a-peak-that-has-settled,
// docs/simulation/README.md#a-peak-on-a-supports-edge). Each case checks
// the outcome against a converged mesh or a handbook: a hole's refinement
// that once made slivers and a peak 50 % high, the convergence loop on a
// hole and a fillet, a clamp's singular peak left out of it, and the
// "away" value that no longer creeps with the mesh.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/Document.h"
#include "core/Feature.h"
#include "core/Measure.h"
#include "core/features/BooleanFeature.h"
#include "core/features/FilletFeature.h"
#include "core/fem/FemMesher.h"
#include "core/fem/FemSolver.h"
#include "core/fem/FemStudy.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Ax2.hxx>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using Catch::Matchers::WithinRel;

#ifdef HAVE_NETGEN

namespace {

// The face of `body` nearest `p` (mm), by its element name.
ElementRef FaceAt(const Body& body, const glm::dvec3& p) {
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

std::string EdgeAt(const Body& body, const glm::dvec3& p) {
  std::string best;
  double bestD = std::numeric_limits<double>::infinity();
  for (const auto& [idx, name] : body.elementMap.edgeNames) {
    const auto d = Measure::DistanceToElementAt(body, false, idx, p, 1e-3);
    if (d && *d < bestD) {
      bestD = *d;
      best = name;
    }
  }
  REQUIRE_FALSE(best.empty());
  return best;
}

uint64_t LastId(const Document& doc) {
  return doc.GetFeature(doc.FeatureCount() - 1)->GetId();
}

fem::Load Fixed(Document& doc, const ElementRef& face) {
  fem::Load l;
  l.id = doc.AllocateId();
  l.type = fem::LoadType::Fixed;
  l.face = face;
  return l;
}

fem::Load Force(Document& doc, const ElementRef& face, double n,
                const glm::dvec3& dir) {
  fem::Load l;
  l.id = doc.AllocateId();
  l.type = fem::LoadType::Force;
  l.face = face;
  l.magnitude = n;
  l.direction = dir;
  return l;
}

// Steel plate 100 x 40 x 5 with a central Ø10 hole, held at x = 0 and
// pulled 1500 N along +X at x = 100: 10 MPa on the net section. A
// converged mesh gives 24.55 MPa at the hole; Heywood's Kt
// (2 + (1 - d/W)^3 on the net section) 24.22.
constexpr double kHoleConverged = 24.55, kHoleHeywood = 24.22;
struct Part {
  Document doc;
  ElementRef hot; // the face the peak lies on (the bore, a fillet)
  uint64_t study = 0;
};
std::unique_ptr<Part> HolePlate() {
  auto p = std::make_unique<Part>();
  Document& doc = p->doc;
  auto box =
      std::make_unique<BoxFeature>("Plate", glm::dvec3(0), 100.0, 40.0, 5.0);
  box->SetMaterial("steel");
  doc.AddFeature(std::move(box));
  const uint64_t plate = LastId(doc);
  doc.AddFeature(std::make_unique<CylinderFeature>(
      "Hole", glm::dvec3(50, 20, -1), glm::dvec3(0, 0, 1), 5.0, 7.0));
  doc.AddFeature(std::make_unique<BooleanFeature>("Cut", plate, LastId(doc),
                                                  BooleanOp::Subtract));
  REQUIRE(doc.GetBodies().size() == 1);
  const Body& b = doc.GetBodies()[0];
  p->hot = FaceAt(b, {45, 20, 2.5});
  fem::Study& s = doc.AddStudy("Hole");
  s.converge = fem::ConvergeMode::Never;
  s.loads.push_back(Fixed(doc, FaceAt(b, {0, 20, 2.5})));
  s.loads.push_back(Force(doc, FaceAt(b, {100, 20, 2.5}), 1500.0, {1, 0, 0}));
  p->study = s.id;
  return p;
}

// A flat bar stepped from 26 to 20 wide, 5 thick, with r 2 shoulder
// fillets, held at its wide end and pulled 1000 N at its narrow one: 10 MPa
// on the narrow section. A converged mesh gives 20.83 MPa at the fillets;
// Peterson's chart 3.1 20.48 (its fit is good to 3-5 %).
constexpr double kFilletConverged = 20.83, kFilletPeterson = 20.48;
std::unique_ptr<Part> FilletBar() {
  auto p = std::make_unique<Part>();
  Document& doc = p->doc;
  auto wide =
      std::make_unique<BoxFeature>("Wide", glm::dvec3(0), 40.0, 26.0, 5.0);
  wide->SetMaterial("steel");
  doc.AddFeature(std::move(wide));
  const uint64_t a = LastId(doc);
  doc.AddFeature(std::make_unique<BoxFeature>("Narrow", glm::dvec3(40, 3, 0),
                                              40.0, 20.0, 5.0));
  doc.AddFeature(std::make_unique<BooleanFeature>("Join", a, LastId(doc),
                                                  BooleanOp::Union));
  const uint64_t joined = LastId(doc);
  {
    const Body& b = doc.GetBodies()[0];
    doc.AddFeature(std::make_unique<FilletFeature>(
        "Shoulder",
        std::vector<ElementRef>{ElementRef{joined, EdgeAt(b, {40, 3, 2.5})},
                                ElementRef{joined, EdgeAt(b, {40, 23, 2.5})}},
        2.0));
  }
  REQUIRE(doc.GetFeature(doc.FeatureCount() - 1)->GetError().empty());
  REQUIRE(doc.GetBodies().size() == 1);
  const Body& b = doc.GetBodies()[0];
  p->hot = FaceAt(b, {40.586, 2.414, 2.5});
  fem::Study& s = doc.AddStudy("Fillet");
  s.converge = fem::ConvergeMode::Never;
  s.loads.push_back(Fixed(doc, FaceAt(b, {0, 13, 2.5})));
  s.loads.push_back(Force(doc, FaceAt(b, {80, 13, 2.5}), 1000.0, {1, 0, 0}));
  p->study = s.id;
  return p;
}

// Steel cantilever plate 60 x 20 x t, clamped at x = 0, pushed down at its
// tip with 10 (t/2)^2 N: M c / I at the root is 45 MPa whatever t is.
std::unique_ptr<Part> ClampedPlate(double t, const char* material = "steel") {
  auto p = std::make_unique<Part>();
  Document& doc = p->doc;
  auto box =
      std::make_unique<BoxFeature>("Plate", glm::dvec3(0), 60.0, 20.0, t);
  box->SetMaterial(material);
  doc.AddFeature(std::move(box));
  const Body& b = doc.GetBodies()[0];
  fem::Study& s = doc.AddStudy("Cantilever");
  s.converge = fem::ConvergeMode::Never;
  s.loads.push_back(Fixed(doc, FaceAt(b, {0, 10, t / 2})));
  s.loads.push_back(Force(doc, FaceAt(b, {60, 10, t / 2}),
                          10.0 * (t / 2) * (t / 2), {0, 0, -1}));
  p->hot = FaceAt(b, {30, 10, t});
  p->study = s.id;
  return p;
}

fem::Study& StudyOf(Part& p) { return *p.doc.StudyForRun(p.study); }

fem::StudyRuntime Run(Part& p) {
  fem::StudyRuntime rt;
  std::string err;
  const bool ok = fem::RunStudy(p.doc, StudyOf(p), rt, &err);
  INFO(err);
  REQUIRE(ok);
  return rt;
}

fem::StudyRuntime Run(Part& p, const fem::ConvergeOptions& c) {
  fem::StudyRuntime rt;
  std::string err;
  const bool ok = fem::RunStudy(p.doc, StudyOf(p), rt, c, &err);
  INFO(err);
  REQUIRE(ok);
  return rt;
}

// A plate with a hole as an OCC shape, and its bore's face index, for the
// mesher on its own.
TopoDS_Shape HoleShape(int* bore) {
  const TopoDS_Shape box = BRepPrimAPI_MakeBox(100.0, 40.0, 5.0).Shape();
  const TopoDS_Shape cyl =
      BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(50, 20, -1), gp_Dir(0, 0, 1)), 5.0,
                               7.0)
          .Shape();
  const TopoDS_Shape cut = BRepAlgoAPI_Cut(box, cyl).Shape();
  int i = 0;
  *bore = -1;
  for (TopExp_Explorer ex(cut, TopAbs_FACE); ex.More(); ex.Next(), ++i)
    if (BRepAdaptor_Surface(TopoDS::Face(ex.Current())).GetType() ==
        GeomAbs_Cylinder)
      *bore = i;
  REQUIRE(*bore >= 0);
  return cut;
}

TopoDS_Shape Copy(const TopoDS_Shape& s) {
  return BRepBuilderAPI_Copy(s, true, false).Shape();
}

} // namespace

// Netgen keeps every shape's meshing properties in one static table, shared
// by the process: a refinement is set there, so it must not outlive its
// mesh, leak into the next one, or be raced by a second mesh at once.
TEST_CASE("a refinement leaves nothing in Netgen's property table",
          "[fem][mesher]") {
  int bore = -1;
  const TopoDS_Shape shape = HoleShape(&bore);
  fem::MeshSettings plain;
  plain.maxSize = 6.0;
  fem::MeshSettings refined = plain;
  refined.faceSizes = {{bore, 0.8}};
  std::string err;

  const auto alone = fem::Mesh(Copy(shape), plain, &err);
  REQUIRE(alone);
  const auto refinedAlone = fem::Mesh(Copy(shape), refined, &err);
  REQUIRE(refinedAlone);
  // The refinement reaches the volume: many more tets than without.
  CHECK(refinedAlone->stats.tetCount > 2 * alone->stats.tetCount);

  // The same shape refined, then meshed plain: the plain mesh is the one
  // the shape gives meshed plain twice (Netgen writes triangulation onto a
  // shape it meshes, so a second mesh of one shape is compared with a
  // second mesh, never with a fresh copy's), and the table holds nothing of
  // it after either.
  const TopoDS_Shape same = Copy(shape);
  const auto first = fem::Mesh(same, refined, &err);
  REQUIRE(first);
  CHECK(fem::NetgenPropertyEntries(same) == 0);
  const auto second = fem::Mesh(same, plain, &err);
  REQUIRE(second);
  CHECK(fem::NetgenPropertyEntries(same) == 0);
  CHECK(first->stats.nodeCount == refinedAlone->stats.nodeCount);
  const TopoDS_Shape twice = Copy(shape);
  REQUIRE(fem::Mesh(twice, plain, &err));
  const auto again = fem::Mesh(twice, plain, &err);
  REQUIRE(again);
  CHECK(second->stats.nodeCount == again->stats.nodeCount);
  CHECK(second->stats.tetCount == again->stats.tetCount);

  // Two meshes at once, each refining its own copy, give each the mesh it
  // gives alone (the table is held by one at a time; --tsan names a race).
  std::optional<fem::FemMesh> a, b;
  std::string ea, eb;
  const TopoDS_Shape ca = Copy(shape), cb = Copy(shape);
  std::thread ta([&] { a = fem::Mesh(ca, refined, &ea); });
  std::thread tb([&] { b = fem::Mesh(cb, plain, &eb); });
  ta.join();
  tb.join();
  REQUIRE(a);
  REQUIRE(b);
  CHECK(a->stats.nodeCount == refinedAlone->stats.nodeCount);
  CHECK(a->stats.worstQuality == refinedAlone->stats.worstQuality);
  CHECK(b->stats.nodeCount == alone->stats.nodeCount);
  CHECK(fem::NetgenPropertyEntries(ca) == 0);
  CHECK(fem::NetgenPropertyEntries(cb) == 0);
}

// A refinement on the hole grades into its edges and the volume behind it.
// Through the legacy per-face array it did not: 0.3 mm left a 0.014 sliver
// and a peak 50 % high, 0.2 mm did not mesh at all.
TEST_CASE("a hole's refinement meshes clean and reads the converged peak",
          "[fem][mesher][slow]") {
  auto p = HolePlate();
  for (const double h : {0.3, 0.2, 0.15}) {
    INFO("refinement " << h << " mm");
    StudyOf(*p).refinements = {{p->hot, h, "", 0}};
    std::vector<fem::StudyBodyMesh> meshes;
    std::string err;
    REQUIRE(fem::MeshStudy(p->doc, StudyOf(*p), meshes, &err));
    REQUIRE(meshes.size() == 1);
    CHECK(meshes[0].mesh.stats.worstQuality >= 0.1);
    if (h < 0.2)
      continue; // its peak, 24.57, was measured once: 200 k nodes, 3 min
    const fem::StudyRuntime rt = Run(*p);
    INFO("peak " << rt.maxVonMises);
    CHECK_THAT(rt.maxVonMises, WithinRel(kHoleConverged, 0.01));
  }
}

// The loop refines the face the peak is on, which is where the answer was:
// shrinking the GLOBAL size moved the hole's peak under 2 % a step while it
// stayed 5 % low, down to 353 k nodes.
TEST_CASE("a hole's and a fillet's peak converge where they are",
          "[fem][converge][slow]") {
  fem::ConvergeOptions always;
  always.mode = fem::ConvergeMode::Always;
  const auto t0 = std::chrono::steady_clock::now();
  {
    auto p = HolePlate();
    const fem::StudyRuntime rt = Run(*p, always);
    const fem::Convergence& c = rt.convergence;
    INFO("peak " << rt.maxVonMises << " after " << c.steps.size() - 1
                 << " refinements, last change " << c.lastChangePct << " %");
    WARN("hole: " << rt.maxVonMises << " MPa in " << c.steps.size() - 1
                  << " refinements, " << c.steps.back().nodes << " nodes");
    CHECK(c.checked);
    CHECK(c.converged);
    CHECK(c.steps.size() - 1 <= 3);
    CHECK(c.lastChangePct < 2.0);
    CHECK_THAT(rt.maxVonMises, WithinRel(kHoleConverged, 0.01));
    CHECK_THAT(rt.maxVonMises, WithinRel(kHoleHeywood, 0.03));
    // What was refined is the bore, and the study still has no refinement.
    REQUIRE(c.steps.size() >= 2);
    REQUIRE_FALSE(c.steps[1].refined.empty());
    CHECK(StudyOf(*p).refinements.empty());
  }
  {
    auto p = FilletBar();
    const fem::StudyRuntime rt = Run(*p, always);
    const fem::Convergence& c = rt.convergence;
    INFO("peak " << rt.maxVonMises << " after " << c.steps.size() - 1
                 << " refinements, last change " << c.lastChangePct << " %");
    WARN("fillet: " << rt.maxVonMises << " MPa in " << c.steps.size() - 1
                    << " refinements, " << c.steps.back().nodes << " nodes");
    CHECK(c.converged);
    CHECK(c.steps.size() - 1 <= 3);
    CHECK_THAT(rt.maxVonMises, WithinRel(kFilletConverged, 0.01));
    CHECK_THAT(rt.maxVonMises, WithinRel(kFilletPeterson, 0.05));
  }
  const double s =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
          .count();
  WARN("both converged in " << s << " s"); // over 10 s: [slow]
}

// A run that checks nothing says so, and auto checks only while a solve
// costs under its limit.
TEST_CASE("converge: never is not checked, and auto stops for time",
          "[fem][converge]") {
  auto p = HolePlate();
  fem::ConvergeOptions never;
  never.mode = fem::ConvergeMode::Never;
  const fem::StudyRuntime once = Run(*p, never);
  CHECK_FALSE(once.convergence.checked);
  CHECK(once.convergence.why == "converge: never");

  fem::ConvergeOptions hurried;
  hurried.mode = fem::ConvergeMode::Auto;
  hurried.autoSeconds = 0.0; // every solve takes longer than this
  const fem::StudyRuntime stopped = Run(*p, hurried);
  CHECK(stopped.convergence.checked);
  CHECK_FALSE(stopped.convergence.converged);
  CHECK(stopped.convergence.stopped == "time");
  CHECK(stopped.convergence.stoppedSeconds > 0.0);
  CHECK(stopped.convergence.steps.size() == 1);
  // It is the first solve's answer.
  CHECK(stopped.maxVonMises == once.maxVonMises);
}

// A clamp's edge is a singularity: its peak grows without limit, so the
// loop reads the stress away from the edge, and that settles.
TEST_CASE("a clamped cantilever settles away from its clamp's edge",
          "[fem][converge][pin]") {
  auto p = ClampedPlate(2.0);
  fem::ConvergeOptions always;
  always.mode = fem::ConvergeMode::Always;
  const fem::StudyRuntime rt = Run(*p, always);
  const fem::Convergence& c = rt.convergence;
  REQUIRE(c.checked);
  // Once refined, the clamp's peak is on its edge, and never the one read.
  REQUIRE(c.steps.size() >= 2);
  for (size_t k = 1; k < c.steps.size(); ++k) {
    REQUIRE(c.steps[k].peaks.size() == 1);
    CHECK(c.steps[k].peaks[0].away);
  }
  INFO("away " << c.steps.back().peaks[0].mpa << " after " << c.steps.size() - 1
               << " refinements, last change " << c.lastChangePct << " %");
  CHECK(c.converged);
  const fem::FemResults& r = rt.bodies[0].results;
  CHECK(r.supportEdgePeak.flagged);
  // Read a clamp's reach (a quarter of the 20 mm side it holds) from the
  // root, the bending stress is M c / I there: 45 (1 - 5 / 60) MPa.
  CHECK_THAT(r.supportEdgePeak.awayMPa, WithinRel(45.0 * 55.0 / 60.0, 0.03));
}

// "Away" is a distance derived from the support, not one element: one
// element shrinks as the mesh refines and the value crept toward the
// singularity with it (40.7 -> 44.7 MPa from auto to 0.4 mm).
TEST_CASE("a clamp's away value holds still as the mesh refines",
          "[fem][converge][slow]") {
  auto p = ClampedPlate(2.0);
  std::vector<double> away;
  for (const double h : {0.0, 1.5, 0.75}) {
    StudyOf(*p).meshSize = h;
    const fem::StudyRuntime rt = Run(*p);
    const fem::FemResults& r = rt.bodies[0].results;
    // A peak off the clamp's reach is its own away value.
    away.push_back(r.supportEdgePeak.flagged ? r.supportEdgePeak.awayMPa
                                             : r.maxVonMises);
  }
  const auto [lo, hi] = std::minmax_element(away.begin(), away.end());
  WARN("away " << away[0] << ", " << away[1] << ", " << away[2]);
  CHECK((*hi - *lo) / *lo < 0.03);
}

// The reach is derived from what the support holds: a pin's bore, a
// clamp's face.
TEST_CASE("a support's reach is a quarter of what it holds",
          "[fem][converge]") {
  auto p = ClampedPlate(2.0);
  fem::StudyPlan plan;
  std::string err;
  REQUIRE(fem::PlanStudy(p->doc, StudyOf(*p), plan, &err));
  fem::StudyJobInput in;
  REQUIRE(fem::MeshPlannedStudy(plan, in, &err));
  REQUIRE(in.bodies.size() == 1);
  REQUIRE(in.bodies[0].supports.size() == 1);
  // The 20 x 2 end face: a quarter of 20.
  CHECK_THAT(in.bodies[0].supports[0].awayMm, WithinRel(5.0, 0.01));
  // A bore of radius 4 on its axis: a quarter of 4.
  const fem::FemMesh& mesh = in.bodies[0].mesh;
  std::vector<int> all(mesh.boundary.size());
  for (size_t i = 0; i < all.size(); ++i)
    all[i] = static_cast<int>(i);
  // Every boundary facet, read as though round the plate's long axis,
  // measures their mean distance from it, so check against that directly.
  double r = 0.0;
  int n = 0;
  for (const auto& t : mesh.boundary)
    for (int k = 0; k < 3; ++k) {
      const glm::dvec3 q = mesh.nodes[t.nodes[k]] - glm::dvec3(0, 10, 1);
      r += std::hypot(q.y, q.z);
      ++n;
    }
  CHECK_THAT(fem::SupportAwayMm(mesh, all, fem::LoadType::Pin,
                                glm::dvec3(0, 10, 1), glm::dvec3(1, 0, 0)),
             WithinRel(0.25 * r / n, 1e-9));
}

// The stress across a printed part's layers is read away from the edge the
// same way: its peak at a clamp is flagged, and its away value is what the
// part carries.
TEST_CASE("the stress across layers is read away from a clamp's edge",
          "[fem][converge]") {
  auto p = ClampedPlate(2.0, "pla");
  const fem::StudyRuntime rt = Run(*p);
  const fem::StudyBodyResult& b = rt.bodies[0];
  REQUIRE(b.supportEdgeZone.size() == b.mesh.nodes.size());
  // Layers stacked along the plate's length: its bending stress pulls them
  // apart, hardest at the clamp.
  const std::vector<double> f =
      fem::AcrossLayersField(b.results, glm::dvec3(1, 0, 0));
  const fem::FieldPeak pk = fem::PeakOf(b.mesh, f);
  const fem::FemResults::SupportEdgePeak e = fem::PeakAwayFromEdges(
      b.mesh, f, pk.node, b.supportEdgeZone, b.edgeOwners);
  CHECK(e.flagged);
  CHECK(e.awayMPa < pk.value);
  CHECK_THAT(e.awayMPa, WithinRel(45.0 * 55.0 / 60.0, 0.05));
  // Kept as the project keeps it, the zones read the same.
  fem::StudyRuntime kept = rt;
  fem::KeepWhatIsRead(kept);
  const fem::StudyBodyResult& k = kept.bodies[0];
  const std::vector<double> fk =
      fem::AcrossLayersField(k.results, glm::dvec3(1, 0, 0));
  const fem::FieldPeak pkk = fem::PeakOf(k.mesh, fk);
  const fem::FemResults::SupportEdgePeak ek = fem::PeakAwayFromEdges(
      k.mesh, fk, pkk.node, k.supportEdgeZone, k.edgeOwners);
  CHECK(ek.flagged);
  CHECK_THAT(ek.awayMPa, WithinRel(e.awayMPa, 1e-5));
}

#endif // HAVE_NETGEN
