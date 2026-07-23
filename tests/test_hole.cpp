#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/Document.h"
#include "core/Feature.h"
#include "core/features/HoleFeature.h"
#include "core/features/SketchFeature.h"
#include "core/io/Serialization.h"
#include "TestIO.h"

#include <filesystem>

using Catch::Matchers::WithinRel;

namespace {
// A sketch of points on a plane at height z (normal +Z).
uint64_t PointSketch(Document& doc, double z,
                     const std::vector<glm::vec2>& pts) {
  SketchPlane plane(glm::vec3(0, 0, z), glm::vec3(0, 0, 1),
                    glm::vec3(1, 0, 0));
  auto sk = std::make_unique<SketchFeature>("Holes", plane);
  for (const auto& p : pts)
    sk->AddEntity(sketch::Point{p});
  doc.AddFeature(std::move(sk));
  return doc.GetFeature(doc.FeatureCount() - 1)->GetId();
}
} // namespace

TEST_CASE("Simple hole drills a bore of the expected volume", "[hole]") {
  Document doc;
  // Box 10x10x10 spanning z[0,10].
  doc.AddFeature(std::make_unique<BoxFeature>("B", glm::vec3(0), 10, 10, 10));
  // One hole at the box center on the top face (z=10), drilling down.
  const uint64_t sk = PointSketch(doc, 10.0, {{5, 5}});
  auto ho = std::make_unique<HoleFeature>("H", sk, HoleKind::Simple, 2.0, 5.0);
  doc.AddFeature(std::move(ho));
  REQUIRE(doc.GetBodies().size() == 1);
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(),
             WithinRel(1000.0 - M_PI * 1.0 * 1.0 * 5.0, 1e-3));
}

TEST_CASE("Four holes from one sketch (a bolt pattern)", "[hole]") {
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>("B", glm::vec3(0), 10, 10, 4));
  const uint64_t sk = PointSketch(
      doc, 4.0, {{2, 2}, {8, 2}, {8, 8}, {2, 8}});
  // Through-all: each hole d=1 pierces the 4-thick plate.
  auto ho = std::make_unique<HoleFeature>("H", sk, HoleKind::Simple, 1.0, 1.0);
  ho->through = true;
  doc.AddFeature(std::move(ho));
  REQUIRE(doc.GetBodies().size() == 1);
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(),
             WithinRel(400.0 - 4.0 * M_PI * 0.25 * 4.0, 1e-2));
}

TEST_CASE("Counterbore removes more than a simple hole", "[hole]") {
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>("B", glm::vec3(0), 10, 10, 10));
  const uint64_t sk = PointSketch(doc, 10.0, {{5, 5}});
  auto ho = std::make_unique<HoleFeature>("H", sk, HoleKind::Counterbore, 2.0,
                                          6.0);
  ho->cbDiameter = 4.0;
  ho->cbDepth = 2.0;
  doc.AddFeature(std::move(ho));
  REQUIRE(doc.GetBodies().size() == 1);
  // bore pi*1^2*6 + counterbore pi*2^2*2 (the top 2 of the bore overlaps the
  // cbore, so total removed = pi*1^2*6 + pi*2^2*2 - pi*1^2*2).
  const double removed =
      M_PI * 1 * 1 * 6 + M_PI * 2 * 2 * 2 - M_PI * 1 * 1 * 2;
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(),
             WithinRel(1000.0 - removed, 1e-2));
}

TEST_CASE("Hole errors and round-trips (v21)", "[hole]") {
  Document doc;
  doc.AddFeature(std::make_unique<HoleFeature>("Bad", 999, HoleKind::Simple,
                                               2.0, 5.0));
  CHECK_FALSE(doc.Regenerate());
  CHECK(!doc.GetFeature(0)->GetError().empty());

  Document doc2;
  doc2.AddFeature(std::make_unique<BoxFeature>("B", glm::vec3(0), 8, 8, 8));
  const uint64_t sk = PointSketch(doc2, 8.0, {{4, 4}});
  auto ho = std::make_unique<HoleFeature>("H", sk, HoleKind::Countersink,
                                          2.0, 4.0);
  ho->csDiameter = 4.0;
  ho->csDepth = 1.0;
  doc2.AddFeature(std::move(ho));
  const double v = doc2.GetBodies()[0].shape.Volume();

  const auto path = std::filesystem::temp_directory_path() / "hole_rt.json";
  std::string error;
  REQUIRE(testio::Save(doc2, path.string()));
  auto loaded = testio::Load(path.string());
  REQUIRE(loaded != nullptr);
  auto* rt = dynamic_cast<HoleFeature*>(loaded->GetFeature(2));
  REQUIRE(rt != nullptr);
  CHECK(rt->kind == HoleKind::Countersink);
  CHECK_THAT(loaded->GetBodies()[0].shape.Volume(), WithinRel(v, 1e-9));
  std::filesystem::remove(path);
}

// Fail-loud (dogfooding #3): a hole whose `diameterExpr` references a variable
// that resolves in no scope must stamp a feature error, NOT silently fall back
// to the literal twin and quietly drill the 1 mm default. Phase V's expression
// refresh surfaces the unresolved reference; this pins that the hole path is
// covered so a bad bore can never pass unnoticed again.
TEST_CASE("Hole with an unresolved diameter_expr errors", "[hole]") {
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>("B", glm::vec3(0), 10, 10, 10));
  const uint64_t sk = PointSketch(doc, 10.0, {{5, 5}});
  // No `bore` variable is defined anywhere -> the expr can't resolve.
  auto ho = std::make_unique<HoleFeature>("H", sk, HoleKind::Simple, 1.0, 5.0);
  ho->diameterExpr = "bore";
  doc.AddFeature(std::move(ho));

  CHECK_FALSE(doc.Regenerate());
  const Feature* hole = doc.GetFeature(2);
  REQUIRE(hole->TypeName() == "Hole");
  CHECK_FALSE(hole->GetError().empty());
  CHECK(hole->GetError().find("bore") != std::string::npos);
}
