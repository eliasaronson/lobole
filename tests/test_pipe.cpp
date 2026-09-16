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

TEST_CASE("Pipe accepts an arc at engine-scale fractional coordinates",
          "[pipe][sweep]") {
  Document doc;
  auto sk = std::make_unique<SketchFeature>(
      "Elbow", SketchPlane(glm::vec3(0, -135.5f, 0), glm::vec3(0, -1, 0),
                           glm::vec3(1, 0, 0)));
  const double radius = 194.9, start = 1.23, end = 2.75;
  sk->AddEntity(sketch::Arc{{88.0f, 83.5f}, radius, start, end});
  doc.AddFeature(std::move(sk));
  const uint64_t path = doc.GetFeature(doc.FeatureCount() - 1)->GetId();
  doc.AddFeature(std::make_unique<PipeFeature>("Runner", path, 44.0));
  REQUIRE(doc.Regenerate());
  REQUIRE(doc.GetBodies().size() == 1);
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(),
             WithinRel(M_PI * 22.0 * 22.0 * radius * (end - start), 1e-5));
}

TEST_CASE("Pipe follows a line-arc-line path whose joints are irrational",
          "[pipe][sweep]") {
  // A handlebar: straight, bent, straight. The lines meet the arcs at points
  // no float32 holds exactly — here 2.7e-6 out — and the chain walk accepts
  // that (kChainTol is 1e-4) while MakeWire merges at OCC's confusion
  // (1e-7) and refuses the edge. The path then came back "must be an open
  // sketch chain", which it is. Shared joint vertices, as a closed loop has
  // always had.
  Document doc;
  auto sk = std::make_unique<SketchFeature>(
      "Bar",
      SketchPlane(glm::vec3(0), glm::vec3(0, -1, 0), glm::vec3(1, 0, 0)));
  const double r = 55.0, theta = 0.6779; // the bend, in radians
  const double cx = 60.0;                // where the straight run ends
  const float tu = static_cast<float>(cx + r * std::sin(theta));
  const float tv = static_cast<float>(r * (1.0 - std::cos(theta)));
  sk->AddEntity(sketch::Line{{-100.0f, 0.0f}, {static_cast<float>(cx), 0.0f}});
  sk->AddEntity(sketch::Arc{{static_cast<float>(cx), static_cast<float>(r)},
                            r,
                            -M_PI / 2.0,
                            -M_PI / 2.0 + theta});
  // Tangent to the arc where it leaves, as a bent tube's straight is.
  sk->AddEntity(
      sketch::Line{{tu, tv},
                   {tu + static_cast<float>(120.0 * std::cos(theta)),
                    tv + static_cast<float>(120.0 * std::sin(theta))}});
  doc.AddFeature(std::move(sk));
  const uint64_t path = doc.GetFeature(doc.FeatureCount() - 1)->GetId();
  doc.AddFeature(std::make_unique<PipeFeature>("Handlebar", path, 22.2, 1.6));
  REQUIRE(doc.Regenerate());
  REQUIRE(doc.GetFeature(doc.FeatureCount() - 1)->GetError().empty());
  REQUIRE(doc.GetBodies().size() == 1);
  // One body, and long enough to be the whole path rather than one segment.
  glm::vec3 lo, hi;
  doc.GetBodies()[0].shape.GetBoundingBox(lo, hi);
  CHECK(hi.x - lo.x > 240.0f);
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
