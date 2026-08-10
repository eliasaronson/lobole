#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestIO.h"
#include "core/Document.h"
#include "core/Feature.h"
#include "core/features/PatternFeature.h"
#include "core/io/Serialization.h"

#include <filesystem>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
namespace fs = std::filesystem;

// Total volume across all bodies.
static double TotalVolume(const Document& doc) {
  double v = 0.0;
  for (const auto& b : doc.GetBodies())
    v += b.shape.Volume();
  return v;
}

static uint64_t AddBox(Document& doc, const char* name, glm::vec3 pos,
                       double s) {
  doc.AddFeature(std::make_unique<BoxFeature>(name, pos, s, s, s));
  return doc.GetFeature(doc.FeatureCount() - 1)->GetId();
}

// --- Linear ------------------------------------------------------------------

TEST_CASE("Linear pattern of disjoint instances yields separate bodies",
          "[pattern]") {
  Document doc;
  uint64_t box = AddBox(doc, "A", {0, 0, 0}, 2.0); // [0,2]^3, vol 8
  auto pat = std::make_unique<PatternFeature>("Arr", PatternKind::Linear, box);
  pat->axis.kind = AxisRef::Kind::X;
  pat->count = 3;
  pat->spacing = 5.0; // 5 apart, 2 wide -> disjoint
  doc.AddFeature(std::move(pat));

  REQUIRE(doc.GetBodies().size() == 3); // seed consumed, 3 instances
  CHECK_THAT(TotalVolume(doc), WithinRel(24.0, 1e-4));
}

TEST_CASE("Linear pattern of touching instances fuses into one body",
          "[pattern]") {
  Document doc;
  uint64_t box = AddBox(doc, "A", {0, 0, 0}, 2.0);
  auto pat = std::make_unique<PatternFeature>("Arr", PatternKind::Linear, box);
  pat->axis.kind = AxisRef::Kind::X;
  pat->count = 3;
  pat->spacing = 1.0; // overlap: [0,2],[1,3],[2,4] -> contiguous [0,4]
  doc.AddFeature(std::move(pat));

  REQUIRE(doc.GetBodies().size() == 1);
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(16.0, 1e-4)); // 4x2x2
}

// --- Circular ----------------------------------------------------------------

TEST_CASE("Circular pattern arrays around an axis", "[pattern]") {
  Document doc;
  uint64_t box = AddBox(doc, "A", {2, -0.5, -0.5}, 1.0); // offset from Z, vol 1
  auto pat =
      std::make_unique<PatternFeature>("Arr", PatternKind::Circular, box);
  pat->axis.kind = AxisRef::Kind::Z; // rotate about global Z through origin
  pat->count = 4;
  pat->angleDeg = 90.0; // 0/90/180/270 -> four disjoint instances
  doc.AddFeature(std::move(pat));

  REQUIRE(doc.GetBodies().size() == 4);
  CHECK_THAT(TotalVolume(doc), WithinRel(4.0, 1e-4));
}

// --- Mirror ------------------------------------------------------------------

TEST_CASE("Mirror across a plane the body is clear of yields two bodies",
          "[pattern]") {
  Document doc;
  uint64_t box = AddBox(doc, "A", {1, 0, 0}, 2.0); // [1,3]^3, clear of x=0
  auto pat = std::make_unique<PatternFeature>("Mir", PatternKind::Mirror, box);
  pat->basePlane = PlaneType::YZ; // the x=0 plane
  doc.AddFeature(std::move(pat));

  REQUIRE(doc.GetBodies().size() == 2); // original + reflection, disjoint
  CHECK_THAT(TotalVolume(doc), WithinRel(16.0, 1e-4));
}

TEST_CASE("Mirror of a body touching the plane merges into one body",
          "[pattern]") {
  Document doc;
  uint64_t box = AddBox(doc, "A", {0, 0, 0}, 2.0); // face on x=0
  auto pat = std::make_unique<PatternFeature>("Mir", PatternKind::Mirror, box);
  pat->basePlane = PlaneType::YZ;
  doc.AddFeature(std::move(pat));

  REQUIRE(doc.GetBodies().size() == 1); // [-2,2]x[0,2]x[0,2]
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(16.0, 1e-4));
}

// --- Expressions -------------------------------------------------------------

TEST_CASE("Expression-driven count and spacing follow parameter edits",
          "[pattern]") {
  Document doc;
  doc.SetParameter("n", "3");
  doc.SetParameter("gap", "5");
  uint64_t box = AddBox(doc, "A", {0, 0, 0}, 2.0);
  auto pat = std::make_unique<PatternFeature>("Arr", PatternKind::Linear, box);
  pat->axis.kind = AxisRef::Kind::X;
  pat->countExpr = "n";
  pat->spacingExpr = "gap";
  doc.AddFeature(std::move(pat));
  REQUIRE(doc.GetBodies().size() == 3);

  doc.SetParameter("n", "5");
  CHECK(doc.GetBodies().size() == 5); // count follows the variable
  CHECK_THAT(TotalVolume(doc), WithinRel(40.0, 1e-4));
}

// --- Failure + serialization -------------------------------------------------

TEST_CASE("A pattern with a missing source fails", "[pattern]") {
  Document doc;
  AddBox(doc, "A", {0, 0, 0}, 2.0);
  auto pat = std::make_unique<PatternFeature>("Arr", PatternKind::Linear, 9999);
  pat->axis.kind = AxisRef::Kind::X;
  doc.AddFeature(std::move(pat));
  CHECK_FALSE(doc.Regenerate());
}

TEST_CASE("Pattern round-trips through JSON", "[pattern]") {
  const fs::path path =
      fs::temp_directory_path() / "designer_pattern_test.json";
  {
    Document doc;
    doc.SetParameter("n", "4");
    uint64_t box = AddBox(doc, "A", {0, 0, 0}, 2.0);
    auto pat =
        std::make_unique<PatternFeature>("Arr", PatternKind::Linear, box);
    pat->axis.kind = AxisRef::Kind::X;
    pat->countExpr = "n";
    pat->spacing = 5.0;
    doc.AddFeature(std::move(pat));
    REQUIRE(testio::Save(doc, path.string()));
  }
  const auto loaded = testio::Load(path.string());
  REQUIRE(loaded != nullptr);
  auto* pat = dynamic_cast<PatternFeature*>(loaded->GetFeature(1));
  REQUIRE(pat != nullptr);
  CHECK(pat->kind == PatternKind::Linear);
  CHECK(pat->countExpr == "n");
  CHECK(pat->axis.kind == AxisRef::Kind::X);
  CHECK(loaded->GetBodies().size() == 4);
  fs::remove(path);
}

TEST_CASE("Pattern regeneration is deterministic", "[pattern]") {
  Document doc;
  uint64_t box = AddBox(doc, "A", {0, 0, 0}, 2.0);
  auto pat = std::make_unique<PatternFeature>("Arr", PatternKind::Linear, box);
  pat->axis.kind = AxisRef::Kind::X;
  pat->count = 3;
  pat->spacing = 5.0;
  doc.AddFeature(std::move(pat));
  const double v1 = TotalVolume(doc);
  doc.Regenerate();
  CHECK_THAT(TotalVolume(doc), WithinAbs(v1, 1e-9));
}

TEST_CASE("A pattern with a missing source reports why", "[pattern][error]") {
  Document doc;
  auto pat = std::make_unique<PatternFeature>("Pat", PatternKind::Linear, 9999);
  pat->count = 3;
  doc.AddFeature(std::move(pat));
  CHECK_FALSE(doc.Regenerate());
  const std::string err = doc.GetFeature(0)->GetError();
  CHECK(err.find("source body") != std::string::npos);
}
