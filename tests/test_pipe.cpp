#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/Document.h"
#include "core/Feature.h"
#include "core/features/CurveFeature.h"
#include "core/features/PipeFeature.h"
#include "core/features/SketchFeature.h"

#include <cmath>
#include <memory>

using Catch::Matchers::WithinRel;

namespace {
uint64_t AddStraightPath(Document& doc, double len) {
  // A vertical line on XZ (normal +Y), from origin up +Z by len.
  auto sk = std::make_unique<SketchFeature>(
      "Path",
      SketchPlane(glm::vec3(0), glm::vec3(0, 1, 0), glm::vec3(1, 0, 0)));
  sk->AddEntity(sketch::Line{{0, 0}, {0, static_cast<float>(len)}});
  doc.AddFeature(std::move(sk));
  return doc.GetFeature(doc.FeatureCount() - 1)->GetId();
}
} // namespace

TEST_CASE("Solid pipe along a straight path is a cylinder", "[pipe]") {
  Document doc;
  const uint64_t path = AddStraightPath(doc, 10.0);
  // Outer diameter 2 (r=1), solid.
  doc.AddFeature(std::make_unique<PipeFeature>("Rod", path, 2.0));
  REQUIRE(doc.Regenerate());
  REQUIRE(doc.GetBodies().size() == 1);
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(),
             WithinRel(M_PI * 1.0 * 1.0 * 10.0, 1e-2));
}

TEST_CASE("Hollow pipe volume is the annulus times length", "[pipe]") {
  Document doc;
  const uint64_t path = AddStraightPath(doc, 10.0);
  // Outer d=2 (r=1), wall 0.25 → inner r=0.75.
  doc.AddFeature(std::make_unique<PipeFeature>("Tube", path, 2.0, 0.25));
  REQUIRE(doc.Regenerate());
  REQUIRE(doc.GetBodies().size() == 1);
  const double annulus = M_PI * (1.0 * 1.0 - 0.75 * 0.75);
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(),
             WithinRel(annulus * 10.0, 2e-2));
}

TEST_CASE("Pipe runs along a 3D helix (curves-3d P2)", "[pipe][curve3d]") {
  Document doc;
  auto cv = std::make_unique<CurveFeature>("Helix", Curve3DKind::Helix);
  cv->radius = 3.0;
  cv->pitch = 1.5;
  cv->turns = 4.0;
  doc.AddFeature(std::move(cv));
  const uint64_t path = doc.GetFeature(doc.FeatureCount() - 1)->GetId();
  doc.AddFeature(std::make_unique<PipeFeature>("Spring", path, 0.6, 0.15));
  REQUIRE(doc.Regenerate());
  REQUIRE(doc.GetBodies().size() == 1);
  CHECK(doc.GetBodies()[0].shape.Volume() > 0.0);
}

TEST_CASE("Pipe rejects a wall thicker than the radius", "[pipe]") {
  Document doc;
  const uint64_t path = AddStraightPath(doc, 5.0);
  doc.AddFeature(std::make_unique<PipeFeature>("Bad", path, 2.0, 1.5));
  CHECK_FALSE(doc.Regenerate());
  CHECK(!doc.GetFeature(doc.FeatureCount() - 1)->GetError().empty());
}
