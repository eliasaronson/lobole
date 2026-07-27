#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestIO.h"
#include "core/Document.h"
#include "core/Feature.h"
#include "core/features/CurveFeature.h"
#include "core/features/LoftFeature.h"
#include "core/features/SketchFeature.h"
#include "core/io/Serialization.h"

#include <cmath>
#include <filesystem>
#include <memory>
#include <vector>

using Catch::Matchers::WithinRel;

namespace {
// A square sketch of half-size `h` centered on (0,0) of a plane at z=`z`.
uint64_t SquareAt(Document& doc, double z, double h, const char* name) {
  SketchPlane plane(glm::vec3(0, 0, z), glm::vec3(0, 0, 1), glm::vec3(1, 0, 0));
  auto sk = std::make_unique<SketchFeature>(name, plane);
  sk->AddRectangle({-h, -h}, {h, h});
  doc.AddFeature(std::move(sk));
  return doc.GetFeature(doc.FeatureCount() - 1)->GetId();
}
} // namespace

TEST_CASE("Loft between two squares makes a frustum", "[loft]") {
  Document doc;
  // Bottom square 4x4 (half 2) at z=0, top square 2x2 (half 1) at z=6.
  const uint64_t a = SquareAt(doc, 0.0, 2.0, "Bottom");
  const uint64_t b = SquareAt(doc, 6.0, 1.0, "Top");
  doc.AddFeature(
      std::make_unique<LoftFeature>("Frustum", std::vector<uint64_t>{a, b}));
  REQUIRE(doc.GetBodies().size() == 1);
  // Prismatoid: V = h/6 (A_bottom + 4 A_mid + A_top). A_bottom=16, A_top=4,
  // mid square half=1.5 -> side 3 -> A_mid=9. V = 6/6 (16 + 36 + 4) = 56.
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(56.0, 1e-3));
}

TEST_CASE("Loft needs at least two sections", "[loft]") {
  Document doc;
  const uint64_t a = SquareAt(doc, 0.0, 2.0, "Only");
  doc.AddFeature(
      std::make_unique<LoftFeature>("Bad", std::vector<uint64_t>{a}));
  CHECK_FALSE(doc.Regenerate());
  CHECK(!doc.GetFeature(doc.FeatureCount() - 1)->GetError().empty());
}

namespace {
// A circle of radius `r` centered on (0,0) of a plane at z=`z`.
uint64_t CircleAt(Document& doc, double z, double r, const char* name) {
  SketchPlane plane(glm::vec3(0, 0, z), glm::vec3(0, 0, 1), glm::vec3(1, 0, 0));
  auto sk = std::make_unique<SketchFeature>(name, plane);
  sk->AddEntity(sketch::Circle{{0, 0}, r});
  doc.AddFeature(std::move(sk));
  return doc.GetFeature(doc.FeatureCount() - 1)->GetId();
}
} // namespace

TEST_CASE("Guided loft skins sections along an explicit spine (P3)",
          "[loft][curve3d]") {
  Document doc;
  // Two equal circles r=1 at z=0 and z=10; spine straight up +Z by 10.
  const uint64_t a = CircleAt(doc, 0.0, 1.0, "Bot");
  const uint64_t b = CircleAt(doc, 10.0, 1.0, "Top");
  auto path = std::make_unique<CurveFeature>("Spine", Curve3DKind::FitSpline);
  path->points = {{0, 0, 0}, {0, 0, 5}, {0, 0, 10}};
  doc.AddFeature(std::move(path));
  const uint64_t spine = doc.GetFeature(doc.FeatureCount() - 1)->GetId();

  auto lo = std::make_unique<LoftFeature>("Along", std::vector<uint64_t>{a, b});
  lo->pathId = spine;
  doc.AddFeature(std::move(lo));
  REQUIRE(doc.Regenerate());
  REQUIRE(doc.GetBodies().size() == 1);
  // Equal circles skinned along a straight spine ~ a cylinder.
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(),
             WithinRel(M_PI * 1.0 * 1.0 * 10.0, 0.1));
}

TEST_CASE("Guided loft with a guide curve builds a valid solid (P3)",
          "[loft][curve3d]") {
  Document doc;
  const uint64_t a = CircleAt(doc, 0.0, 1.0, "Bot");
  const uint64_t b = CircleAt(doc, 10.0, 1.0, "Top");
  auto path = std::make_unique<CurveFeature>("Spine", Curve3DKind::FitSpline);
  path->points = {{0, 0, 0}, {0, 0, 10}};
  doc.AddFeature(std::move(path));
  const uint64_t spine = doc.GetFeature(doc.FeatureCount() - 1)->GetId();
  // Guide runs along the section border (radius 1 in +X) and flares outward.
  auto g = std::make_unique<CurveFeature>("Guide", Curve3DKind::FitSpline);
  g->points = {{1, 0, 0}, {1.5, 0, 5}, {2, 0, 10}};
  doc.AddFeature(std::move(g));
  const uint64_t guide = doc.GetFeature(doc.FeatureCount() - 1)->GetId();

  auto lo =
      std::make_unique<LoftFeature>("Guided", std::vector<uint64_t>{a, b});
  lo->pathId = spine;
  lo->guideId = guide;
  doc.AddFeature(std::move(lo));
  REQUIRE(doc.Regenerate());
  REQUIRE(doc.GetFeature(doc.FeatureCount() - 1)->GetError().empty());
  REQUIRE(doc.GetBodies().size() == 1);
  CHECK(doc.GetBodies()[0].shape.Volume() > 0.0);
}

TEST_CASE("Guided loft round-trips path and guide ids", "[loft]") {
  auto lo = std::make_unique<LoftFeature>("L", std::vector<uint64_t>{7, 8, 9});
  lo->pathId = 42;
  lo->guideId = 43;
  auto back = DocumentIO::FeatureFromJson(DocumentIO::FeatureToJson(*lo));
  auto* r = dynamic_cast<LoftFeature*>(back.get());
  REQUIRE(r);
  CHECK(r->sketchIds == std::vector<uint64_t>{7, 8, 9});
  CHECK(r->pathId == 42);
  CHECK(r->guideId == 43);
}

// Regression for the "hourglass" bug (dogfooding #2): a smoothed loft fits ONE
// BSpline surface through all sections, so a 4-vertex square paired with a
// 1-seam circle gets a ~45deg-misaligned correspondence whose twist propagates
// down the whole skin — pinching the r5 -> r5 span below a straight cylinder.
// LoftFeature now rules mismatched-topology sections into independent per-pair
// patches, so the transition is monotonic and the cylinder span stays exact.
TEST_CASE("Loft square to two circles does not pinch", "[loft]") {
  Document doc;
  const uint64_t a = SquareAt(doc, 0.0, 10.0, "Sq"); // 20x20
  const uint64_t b = CircleAt(doc, 6.0, 5.0, "C1");  // r5
  const uint64_t c = CircleAt(doc, 30.0, 5.0, "C2"); // r5
  doc.AddFeature(
      std::make_unique<LoftFeature>("L", std::vector<uint64_t>{a, b, c}));
  REQUIRE(doc.GetBodies().size() == 1);
  const double vol = doc.GetBodies()[0].shape.Volume();
  // The z6..z30 span alone is an r5 cylinder (pi*25*24 ~= 1885); the square->
  // circle base (z0..z6) only ADDS material, so a clean loft must exceed it.
  const double cyl = M_PI * 25.0 * 24.0;
  CHECK(vol >= cyl);   // no hourglass: total >= the upper cylinder segment
  CHECK(vol > 2500.0); // hand floor well clear of the pinched value (~1849)
}

// The matched-section path must stay exact: two coaxial identical circles are a
// pure cylinder (no ruling fallback, no twist).
TEST_CASE("Loft between matched circles is an exact cylinder", "[loft]") {
  Document doc;
  const uint64_t a = CircleAt(doc, 6.0, 5.0, "C1");
  const uint64_t b = CircleAt(doc, 30.0, 5.0, "C2");
  doc.AddFeature(
      std::make_unique<LoftFeature>("L", std::vector<uint64_t>{a, b}));
  REQUIRE(doc.GetBodies().size() == 1);
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(),
             WithinRel(M_PI * 25.0 * 24.0, 1e-3));
}

TEST_CASE("Loft round-trips through serialization", "[loft]") {
  Document doc;
  const uint64_t a = SquareAt(doc, 0.0, 2.0, "B");
  const uint64_t b = SquareAt(doc, 5.0, 2.0, "T");
  doc.AddFeature(
      std::make_unique<LoftFeature>("L", std::vector<uint64_t>{a, b}));
  const double v = doc.GetBodies()[0].shape.Volume(); // straight box: 4*4*5=80
  CHECK_THAT(v, WithinRel(80.0, 1e-3));

  const auto path = std::filesystem::temp_directory_path() / "loft_rt.json";
  std::string error;
  REQUIRE(testio::Save(doc, path.string()));
  auto loaded = testio::Load(path.string());
  REQUIRE(loaded != nullptr);
  auto* rt = dynamic_cast<LoftFeature*>(loaded->GetFeature(2));
  REQUIRE(rt != nullptr);
  CHECK(rt->sketchIds.size() == 2);
  CHECK_THAT(loaded->GetBodies()[0].shape.Volume(), WithinRel(v, 1e-9));
  std::filesystem::remove(path);
}
