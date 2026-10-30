// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// A section into a sketch (docs/sketching/README.md#a-section-into-a-sketch):
// the sketch's plane through a scan's mesh body or a B-Rep, drawn as locked
// construction geometry within its tolerance in each fit, refitted when the
// body changes, offset into a profile that extrudes to the part it was for,
// and each refusal.

#include "TestIO.h"
#include "TestPaths.h"
#include "mesh_organic.h"

#include "core/Document.h"
#include "core/Feature.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/SketchFeature.h"
#include "core/io/Serialization.h"
#include "core/mesh/MeshReaders.h"
#include "core/sketch/SketchProfile.h"
#include "core/sketch/SketchSection.h"
#include "core/sketch/SketchSolver.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinRel;

namespace {

constexpr double kPi = 3.14159265358979323846;

// The scan, imported as a mesh body where the file put it.
uint64_t ImportMesh(Document& doc, const mesh::TriMesh& m,
                    const std::string& tag) {
  const auto stl = fixtures::WriteStl(m, tag);
  auto imp = std::make_unique<ImportFeature>("scan", stl.string());
  imp->keepOrigin = true;
  imp->upAxis = interchange::UpAxis::None;
  imp->representation = mesh::Representation::Mesh;
  doc.AddFeature(std::move(imp));
  REQUIRE(doc.GetBodies().size() == 1);
  REQUIRE(doc.GetBodies()[0].IsMesh());
  return doc.GetFeature(doc.FeatureCount() - 1)->GetId();
}

SketchFeature& LastSketch(Document& doc) {
  return *dynamic_cast<SketchFeature*>(doc.GetFeature(doc.FeatureCount() - 1));
}

// Every point the entities draw (ends, arc and spline samples), in UV.
std::vector<glm::dvec2> DrawnPoints(const SketchFeature& sk,
                                    const std::vector<uint32_t>& ids) {
  std::vector<glm::dvec2> pts;
  for (uint32_t id : ids)
    for (const auto& line :
         sketch::TessellateEntityPolylines(sk.entities[sk.IndexOfEntity(id)]))
      pts.insert(pts.end(), line.begin(), line.end());
  return pts;
}

} // namespace

TEST_CASE("A scan's section is drawn within its tolerance in each fit",
          "[sketch][mesh_body]") {
  // A sphere of radius 40 whose surface is moved by up to 0.05 mm, cut at
  // z = 10: a circle of radius sqrt(40² - 10²).
  const auto sphere = fixtures::NoisySphere(20000);
  const double r = std::sqrt(40.0 * 40.0 - 10.0 * 10.0);
  size_t counts[3] = {0, 0, 0};
  for (const sketch::SectionFit fit :
       {sketch::SectionFit::Polyline, sketch::SectionFit::Arcs,
        sketch::SectionFit::Spline}) {
    DYNAMIC_SECTION(sketch::ToString(fit)) {
      Document doc;
      ImportMesh(doc, sphere.mesh, "section-sphere");
      auto sk = std::make_unique<SketchFeature>(
          "Cut", SketchPlane(glm::dvec3(0, 0, 10), {0, 0, 1}, {1, 0, 0}));
      sk->AddSectionGeometry({}, 0.1, fit);
      doc.AddFeature(std::move(sk));
      SketchFeature& s = LastSketch(doc);
      INFO(s.GetError());
      REQUIRE(s.GetError().empty());
      const sketch::ExternalGeom& ext = s.external[0];
      CHECK(ext.usedToleranceMm == 0.1);
      CHECK(ext.deviationMm <= 0.1);
      CHECK(ext.sectionChains == 1);
      CHECK(ext.sectionOpen == 0);
      REQUIRE_FALSE(ext.genIds.empty());
      counts[static_cast<int>(fit)] = ext.genIds.size();
      // Construction, and on the true circle to the tolerance plus the
      // scan's own noise (and the tessellation's sag, under 0.05 here).
      for (uint32_t id : ext.genIds)
        CHECK(sketch::IsConstruction(s.entities[s.IndexOfEntity(id)]));
      for (const glm::dvec2& p : DrawnPoints(s, ext.genIds))
        CHECK(std::abs(glm::length(p) - r) < 0.1 + 0.05 + 0.1);
      // The kinds the fit draws.
      for (uint32_t id : ext.genIds) {
        const sketch::Entity& e = s.entities[s.IndexOfEntity(id)];
        if (fit == sketch::SectionFit::Polyline)
          CHECK(std::holds_alternative<sketch::Line>(e));
        if (fit == sketch::SectionFit::Spline)
          CHECK(std::holds_alternative<sketch::BSpline>(e));
      }
    }
  }
  // Arcs follow a round cut in far fewer pieces than lines do.
  if (counts[0] != 0 && counts[1] != 0)
    CHECK(counts[1] * 4 < counts[0]);
}

TEST_CASE("A section's outline offset by a clearance extrudes to the part",
          "[sketch][mesh_body]") {
  // The holder: the scan's outline at z = 10 grown by a 2 mm clearance,
  // extruded 5 mm. Its volume is the ring the clearance asked for.
  const auto sphere = fixtures::NoisySphere(20000);
  const double r = std::sqrt(40.0 * 40.0 - 10.0 * 10.0);
  for (const sketch::SectionFit fit :
       {sketch::SectionFit::Arcs, sketch::SectionFit::Spline,
        sketch::SectionFit::Polyline}) {
    DYNAMIC_SECTION(sketch::ToString(fit)) {
      Document doc;
      ImportMesh(doc, sphere.mesh, "section-holder");
      auto sk = std::make_unique<SketchFeature>(
          "Cut", SketchPlane(glm::dvec3(0, 0, 10), {0, 0, 1}, {1, 0, 0}));
      sk->AddSectionGeometry({}, 0.05, fit);
      doc.AddFeature(std::move(sk));
      SketchFeature& s = LastSketch(doc);
      REQUIRE(s.GetError().empty());
      // Construction in, real geometry out: the offset is the profile.
      std::vector<uint32_t> made =
          s.OffsetChain(s.external[0].genIds, 2.0, true);
      REQUIRE_FALSE(made.empty());
      for (uint32_t id : made)
        CHECK_FALSE(sketch::IsConstruction(s.entities[s.IndexOfEntity(id)]));
      const uint64_t sketchId = s.GetId();
      doc.Regenerate();
      doc.AddFeature(std::make_unique<ExtrudeFeature>("Holder", sketchId, 5.0,
                                                      ExtrudeMode::NewBody));
      INFO(doc.GetFeature(doc.FeatureCount() - 1)->GetError());
      REQUIRE(doc.GetBodies().size() == 2);
      const double volume = doc.GetBodies()[1].shape.Volume();
      // Outward or inward, the sign is the sketch's; the radius is 2 mm off
      // the cut either way, to the tolerance and the scan's noise.
      const double reff = std::sqrt(volume / (5.0 * kPi));
      CHECK(std::abs(std::abs(reff - r) - 2.0) < 0.1);
    }
  }
}

TEST_CASE("A section follows the body it cuts", "[sketch]") {
  // A B-Rep box cut through its middle: four lines, refitted when it grows.
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>("Block", glm::dvec3(0, 0, 0),
                                              40.0, 20.0, 10.0));
  const uint64_t boxId = doc.GetFeature(0)->GetId();
  auto sk = std::make_unique<SketchFeature>(
      "Cut", SketchPlane(glm::dvec3(0, 0, 5), {0, 0, 1}, {1, 0, 0}));
  sk->AddSectionGeometry({boxId}, 0.0, sketch::SectionFit::Polyline);
  doc.AddFeature(std::move(sk));
  const auto extentU = [&]() {
    SketchFeature& s = LastSketch(doc);
    REQUIRE(s.GetError().empty());
    double lo = 1e9, hi = -1e9;
    for (const glm::dvec2& p : DrawnPoints(s, s.external[0].genIds)) {
      lo = std::min(lo, p.x);
      hi = std::max(hi, p.x);
    }
    return hi - lo;
  };
  CHECK(LastSketch(doc).external[0].genIds.size() == 4);
  CHECK_THAT(extentU(), WithinRel(40.0, 1e-9));
  // Derived from the box: 1/1000 of its diagonal.
  CHECK_THAT(LastSketch(doc).external[0].usedToleranceMm,
             WithinRel(std::sqrt(40.0 * 40 + 20 * 20 + 10 * 10) * 1e-3, 1e-6));
  CHECK(LastSketch(doc).external[0].deviationMm < 1e-9);
  auto* box = dynamic_cast<BoxFeature*>(doc.GetFeature(0));
  box->width = 60.0;
  doc.Regenerate();
  CHECK_THAT(extentU(), WithinRel(60.0, 1e-9));

  // A plane out of the box is the sketch's error, named.
  LastSketch(doc).plane =
      SketchPlane(glm::dvec3(0, 0, 50), {0, 0, 1}, {1, 0, 0});
  doc.Regenerate();
  CHECK_THAT(LastSketch(doc).GetError(), ContainsSubstring("cuts none"));
}

TEST_CASE("A section of an open scan draws its open chains",
          "[sketch][mesh_body]") {
  // The figure with its base cut away, as a table-top scan arrives, cut by
  // an upright plane: the cut runs out at the open bottom.
  const auto fig = fixtures::CutFigurine(8000);
  Document doc;
  ImportMesh(doc, fig.mesh, "section-open");
  const glm::dvec3 c = 0.5 * (fig.lo + fig.hi);
  auto sk = std::make_unique<SketchFeature>(
      "Upright", SketchPlane(glm::dvec3(c.x, c.y, 0), {0, 1, 0}, {1, 0, 0}));
  sk->AddSectionGeometry({}, 0.0, sketch::SectionFit::Arcs);
  doc.AddFeature(std::move(sk));
  SketchFeature& s = LastSketch(doc);
  INFO(s.GetError());
  REQUIRE(s.GetError().empty());
  CHECK(s.external[0].sectionOpen >= 1);
  CHECK(s.external[0].deviationMm <= s.external[0].usedToleranceMm);
  CHECK_FALSE(s.external[0].genIds.empty());
}

TEST_CASE("A section is saved and read back, its fit refused when unknown",
          "[sketch][serialization]") {
  SketchFeature sk("Cut", SketchPlane(PlaneType::XY));
  sk.AddSectionGeometry({7, 9}, 0.25, sketch::SectionFit::Spline);
  const nlohmann::json j = DocumentIO::FeatureToJson(sk);
  const nlohmann::json& e = j.at("external").at(0);
  CHECK(e.at("kind") == "section");
  CHECK(e.at("fit") == "spline");
  CHECK(e.at("tolerance_mm") == 0.25);
  auto back = DocumentIO::FeatureFromJson(j);
  const auto* s = dynamic_cast<const SketchFeature*>(back.get());
  REQUIRE(s != nullptr);
  REQUIRE(s->external.size() == 1);
  CHECK(s->external[0].kind == sketch::ExternalKind::Section);
  CHECK(s->external[0].fit == sketch::SectionFit::Spline);
  CHECK(s->external[0].toleranceMm == 0.25);
  CHECK(s->external[0].sectionBodies == std::vector<uint64_t>{7, 9});

  nlohmann::json bad = j;
  bad["external"][0]["fit"] = "smooth";
  CHECK_THROWS_WITH(DocumentIO::FeatureFromJson(bad),
                    ContainsSubstring("polyline, arcs, spline"));
  bad = j;
  bad["external"][0]["tolerance_mm"] = -1.0;
  CHECK_THROWS_WITH(DocumentIO::FeatureFromJson(bad),
                    ContainsSubstring("tolerance_mm"));
  bad = j;
  bad["external"][0]["element"] = "f1";
  CHECK_THROWS(DocumentIO::FeatureFromJson(bad));
  CHECK_FALSE(sketch::SectionFitFromString("Arcs"));
}

TEST_CASE("A section names a body that is not before it", "[sketch]") {
  Document doc;
  auto sk = std::make_unique<SketchFeature>("Cut", SketchPlane(PlaneType::XY));
  doc.AddFeature(std::move(sk));
  CHECK(LastSketch(doc).GetError().empty());
  LastSketch(doc).AddSectionGeometry({}, 0.0, sketch::SectionFit::Arcs);
  doc.Regenerate();
  CHECK_THAT(LastSketch(doc).GetError(), ContainsSubstring("no body"));
  LastSketch(doc).external.clear();
  LastSketch(doc).AddSectionGeometry({99}, 0.0, sketch::SectionFit::Arcs);
  doc.Regenerate();
  CHECK_THAT(LastSketch(doc).GetError(),
             ContainsSubstring("is not before this sketch"));
}

TEST_CASE("The fits hold a stated tolerance on a dense cut", "[sketch]") {
  // An ellipse sampled at 0.01 mm steps: no arc or line is it, so each fit
  // has to work for its tolerance.
  sketch::CutChain c;
  c.closed = true;
  for (int k = 0; k < 6000; k++) {
    const double t = 2.0 * kPi * k / 6000.0;
    c.points.push_back({30.0 * std::cos(t), 12.0 * std::sin(t)});
  }
  for (const double tol : {0.2, 0.02, 0.002}) {
    for (const sketch::SectionFit fit :
         {sketch::SectionFit::Polyline, sketch::SectionFit::Arcs,
          sketch::SectionFit::Spline}) {
      INFO(sketch::ToString(fit) << " at " << tol);
      const sketch::FittedSection f = sketch::FitSection({c}, fit, tol);
      CHECK(f.deviation <= tol);
      REQUIRE_FALSE(f.entities.empty());
      // Closed, so one loop that extrudes to the ellipse's area, to the
      // tolerance round its perimeter (about 134 mm).
      const auto loops = sketch::BuildLoops(f.entities);
      REQUIRE(loops.size() == 1);
      const CADShape prism =
          sketch::BuildPrism(SketchPlane(PlaneType::XY), loops, 1.0, false);
      REQUIRE(prism.IsValid());
      CHECK(std::abs(prism.Volume() - kPi * 30.0 * 12.0) <= 134.0 * tol);
    }
  }
}

TEST_CASE("The vNEXT golden's section is drawn on load", "[sketch][golden]") {
  // tests/golden/vNEXT-sketch-section.json: a box cut through its middle,
  // the section saved with the four sides it drew, drawn again by the load's
  // regeneration onto the same ids.
  auto doc = testio::Load(std::string(TestSourceDir()) +
                          "/tests/golden/vNEXT-sketch-section.json");
  REQUIRE(doc != nullptr);
  auto& s = *dynamic_cast<SketchFeature*>(doc->GetFeature(1));
  INFO(s.GetError());
  REQUIRE(s.GetError().empty());
  REQUIRE(s.external.size() == 1);
  CHECK(s.external[0].fit == sketch::SectionFit::Arcs);
  CHECK(s.external[0].sectionBodies == std::vector<uint64_t>{1});
  CHECK(s.external[0].genIds ==
        std::vector<uint32_t>{517431015, 2469267207, 118616326, 4231634379});
  for (uint32_t id : s.external[0].genIds)
    CHECK(
        std::holds_alternative<sketch::Line>(s.entities[s.IndexOfEntity(id)]));
}

// What a section into a sketch costs on a scan, by size: hidden, run by name
// (docs/sketching/README.md#what-a-section-costs).
TEST_CASE("What a section into a sketch costs", "[.][section-cost]") {
  using Clock = std::chrono::steady_clock;
  const auto ms = [](Clock::time_point a) {
    return std::chrono::duration<double, std::milli>(Clock::now() - a).count();
  };
  for (const size_t n : {size_t(10000), size_t(300000), size_t(2000000)}) {
    const auto fig = fixtures::Figurine(n);
    Body body;
    body.name = "figure";
    body.mesh = std::make_shared<mesh::TriMesh>(fig.mesh);
    const double midZ = 0.5 * (fig.lo.z + fig.hi.z);
    const SketchPlane across(glm::dvec3(0, 0, midZ), {0, 0, 1}, {1, 0, 0});
    const SketchPlane upright(0.5 * (fig.lo + fig.hi), {0, 1, 0}, {1, 0, 0});
    for (const auto& [name, plane] :
         {std::pair<const char*, SketchPlane>{"across", across},
          std::pair<const char*, SketchPlane>{"upright", upright}}) {
      auto t = Clock::now();
      const sketch::SectionCut cut = sketch::CutBodies({&body}, plane, 0.01);
      const double cutMs = ms(t);
      size_t points = 0;
      for (const auto& c : cut.chains)
        points += c.points.size();
      const double tol = sketch::DefaultSectionTolerance(cut.diagonal);
      for (const sketch::SectionFit fit :
           {sketch::SectionFit::Polyline, sketch::SectionFit::Arcs,
            sketch::SectionFit::Spline}) {
        t = Clock::now();
        const auto f = sketch::FitSection(cut.chains, fit, tol);
        const double fitMs = ms(t);
        // Into a sketch and solved, as a regeneration does it.
        SketchFeature sk("S", plane);
        t = Clock::now();
        std::vector<uint32_t> ids;
        for (const auto& e : f.entities)
          ids.push_back(sk.AddEntity(sketch::Entity(e)));
        sketch::ExternalGeom held;
        held.kind = sketch::ExternalKind::Section;
        held.genIds = ids;
        sk.external.push_back(held);
        sketch::Solver solver;
        solver.Load(sk);
        solver.Solve();
        const double sketchMs = ms(t);
        std::printf("section %zu %s: cut %.1f ms (%zu chains, %zu points); "
                    "%s at %.3f mm: %.1f ms, %zu entities, deviation %.4f; "
                    "into a sketch and solved %.1f ms\n",
                    fig.mesh.triangles.size(), name, cutMs, cut.chains.size(),
                    points, sketch::ToString(fit), tol, fitMs,
                    f.entities.size(), f.deviation, sketchMs);
        CHECK(f.deviation <= tol);
      }
    }
  }
}
