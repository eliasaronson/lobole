#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/Document.h"
#include "core/Feature.h"
#include "core/features/CurveFeature.h"
#include "core/features/SketchFeature.h"
#include "core/features/SweepFeature.h"

#include <cmath>
#include <vector>

using Catch::Matchers::WithinRel;

namespace {
uint64_t AddSketch(Document& doc, const SketchPlane& plane, const char* name,
                   const std::vector<sketch::Entity>& ents) {
  auto sk = std::make_unique<SketchFeature>(name, plane);
  for (const auto& e : ents)
    sk->AddEntity(e);
  doc.AddFeature(std::move(sk));
  return doc.GetFeature(doc.FeatureCount() - 1)->GetId();
}
} // namespace

TEST_CASE("Sweep a circle along a straight vertical path is a cylinder",
          "[sweep]") {
  Document doc;
  // Profile: circle r=1 on XY (normal +Z) at origin.
  const uint64_t prof = AddSketch(
      doc, SketchPlane(glm::vec3(0), glm::vec3(0, 0, 1), glm::vec3(1, 0, 0)),
      "Prof", {sketch::Circle{{0, 0}, 1.0}});
  // Path: a line on the XZ plane (normal +Y) going up +Z by 10 from origin.
  const uint64_t path = AddSketch(
      doc, SketchPlane(glm::vec3(0), glm::vec3(0, 1, 0), glm::vec3(1, 0, 0)),
      "Path", {sketch::Line{{0, 0}, {0, 10}}});

  doc.AddFeature(std::make_unique<SweepFeature>("Pipe", prof, path));
  REQUIRE(doc.GetBodies().size() == 1);
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(),
             WithinRel(M_PI * 1.0 * 1.0 * 10.0, 1e-2));
}

TEST_CASE("Sweep a circle along a 3D fit-spline path (curves-3d P1)",
          "[sweep][curve3d]") {
  Document doc;
  // Profile: small circle r=0.5 on XY at the spline's start point.
  const uint64_t prof = AddSketch(
      doc, SketchPlane(glm::vec3(0), glm::vec3(0, 0, 1), glm::vec3(1, 0, 0)),
      "Prof", {sketch::Circle{{0, 0}, 0.5}});
  auto cv = std::make_unique<CurveFeature>("Path", Curve3DKind::FitSpline);
  cv->points = {{0, 0, 0}, {2, 1, 3}, {5, 0, 6}};
  doc.AddFeature(std::move(cv));
  const uint64_t path = doc.GetFeature(doc.FeatureCount() - 1)->GetId();

  doc.AddFeature(std::make_unique<SweepFeature>("Tube", prof, path));
  REQUIRE(doc.Regenerate());
  REQUIRE(doc.GetBodies().size() == 1);
  // Tube ~ pi*0.5^2 * (spline length). Spline runs at least the chord ~8.5,
  // so volume is comfortably positive.
  CHECK(doc.GetBodies()[0].shape.Volume() > 5.0);
}

TEST_CASE("Sweep a circle along a helix is a coil (curves-3d P1)",
          "[sweep][curve3d]") {
  Document doc;
  const uint64_t prof = AddSketch(
      doc,
      SketchPlane(glm::vec3(2, 0, 0), glm::vec3(0, 1, 0), glm::vec3(1, 0, 0)),
      "Prof", {sketch::Circle{{0, 0}, 0.2}});
  auto cv = std::make_unique<CurveFeature>("Helix", Curve3DKind::Helix);
  cv->radius = 2.0;
  cv->pitch = 1.0;
  cv->turns = 3.0;
  doc.AddFeature(std::move(cv));
  const uint64_t path = doc.GetFeature(doc.FeatureCount() - 1)->GetId();

  doc.AddFeature(std::make_unique<SweepFeature>("Coil", prof, path));
  REQUIRE(doc.Regenerate());
  REQUIRE(doc.GetBodies().size() == 1);
  // Coil volume ~ pi*0.2^2 * helixLength. helixLength = 3*sqrt((2pi*2)^2+1).
  const double len = 3.0 * std::sqrt(std::pow(2.0 * M_PI * 2.0, 2) + 1.0);
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(),
             WithinRel(M_PI * 0.04 * len, 0.1));
}

TEST_CASE("Guided sweep uses a second curve to orient the section (P3)",
          "[sweep][curve3d]") {
  Document doc;
  // Profile: circle r=0.4 on XY at origin.
  const uint64_t prof = AddSketch(
      doc, SketchPlane(glm::vec3(0), glm::vec3(0, 0, 1), glm::vec3(1, 0, 0)),
      "Prof", {sketch::Circle{{0, 0}, 0.4}});
  // Spine: straight up +Z by 8 (line on XZ plane).
  const uint64_t path = AddSketch(
      doc, SketchPlane(glm::vec3(0), glm::vec3(0, 1, 0), glm::vec3(1, 0, 0)),
      "Path", {sketch::Line{{0, 0}, {0, 8}}});
  // Guide: a 3D curve running alongside the spine but curving out in +X.
  auto g = std::make_unique<CurveFeature>("Guide", Curve3DKind::FitSpline);
  g->points = {{1, 0, 0}, {2, 0, 4}, {1, 0, 8}};
  doc.AddFeature(std::move(g));
  const uint64_t guide = doc.GetFeature(doc.FeatureCount() - 1)->GetId();

  auto sw = std::make_unique<SweepFeature>("Guided", prof, path);
  sw->guideId = guide;
  doc.AddFeature(std::move(sw));
  REQUIRE(doc.Regenerate());
  REQUIRE(doc.GetBodies().size() == 1);
  CHECK(doc.GetBodies()[0].shape.Volume() > 0.0);
}

TEST_CASE("Sweep errors on a missing guide", "[sweep]") {
  Document doc;
  const uint64_t prof = AddSketch(
      doc, SketchPlane(glm::vec3(0), glm::vec3(0, 0, 1), glm::vec3(1, 0, 0)),
      "Prof", {sketch::Circle{{0, 0}, 0.4}});
  const uint64_t path = AddSketch(
      doc, SketchPlane(glm::vec3(0), glm::vec3(0, 1, 0), glm::vec3(1, 0, 0)),
      "Path", {sketch::Line{{0, 0}, {0, 8}}});
  auto sw = std::make_unique<SweepFeature>("Bad", prof, path);
  sw->guideId = 99999; // nonexistent
  doc.AddFeature(std::move(sw));
  CHECK_FALSE(doc.Regenerate());
  CHECK(!doc.GetFeature(doc.FeatureCount() - 1)->GetError().empty());
}

TEST_CASE("Sweep errors on a missing sketch", "[sweep]") {
  Document doc;
  doc.AddFeature(std::make_unique<SweepFeature>("Bad", 111, 222));
  CHECK_FALSE(doc.Regenerate());
  CHECK(!doc.GetFeature(0)->GetError().empty());
}

TEST_CASE("Sweep follows a bent path (all segments, rounded corner)",
          "[sweep]") {
  Document doc;
  const uint64_t prof = AddSketch(
      doc, SketchPlane(glm::vec3(0), glm::vec3(0, 0, 1), glm::vec3(1, 0, 0)),
      "Prof", {sketch::Circle{{0, 0}, 0.5}});
  // L-path on XZ: up 6 then over 5 (total spine 11).
  const uint64_t path = AddSketch(
      doc, SketchPlane(glm::vec3(0), glm::vec3(0, 1, 0), glm::vec3(1, 0, 0)),
      "Path", {sketch::Line{{0, 0}, {0, 6}}, sketch::Line{{0, 6}, {5, 6}}});
  doc.AddFeature(std::make_unique<SweepFeature>("Elbow", prof, path));
  REQUIRE(doc.GetBodies().size() == 1);
  // Both segments swept (a MakePipe would stop at the corner near pi*0.25*6);
  // full spine ~11 gives ~pi*0.25*11 minus a little at the rounded corner.
  const double v = doc.GetBodies()[0].shape.Volume();
  CHECK(v > 7.5); // well past the single-segment 4.7
  CHECK(v < 9.0);
}

TEST_CASE("Sweep on a degenerate path errors instead of aborting", "[sweep]") {
  // A path sketch whose only segment is zero-length made OCC throw
  // StdFail_NotDone from BRepBuilderAPI_MakeEdge::Edge(), which uncaught
  // aborted the whole process (surfaced by an Onshape import). It must fail
  // the feature cleanly now.
  Document doc;
  const uint64_t prof = AddSketch(
      doc, SketchPlane(glm::vec3(0), glm::vec3(0, 0, 1), glm::vec3(1, 0, 0)),
      "Prof", {sketch::Circle{{0, 0}, 0.5}});
  const uint64_t path = AddSketch(
      doc, SketchPlane(glm::vec3(0), glm::vec3(0, 1, 0), glm::vec3(1, 0, 0)),
      "Path", {sketch::Line{{2, 3}, {2, 3}}}); // coincident endpoints
  doc.AddFeature(std::make_unique<SweepFeature>("Bad", prof, path));
  CHECK_FALSE(doc.Regenerate());
  CHECK(!doc.GetFeature(2)->GetError().empty());
}
