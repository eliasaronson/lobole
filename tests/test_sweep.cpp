#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/Document.h"
#include "core/Feature.h"
#include "core/features/SketchFeature.h"
#include "core/features/SweepFeature.h"

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
      "Path",
      {sketch::Line{{0, 0}, {0, 6}}, sketch::Line{{0, 6}, {5, 6}}});
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
