#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestIO.h"
#include "core/Document.h"
#include "core/Feature.h"
#include "core/features/BooleanFeature.h"
#include "core/io/Serialization.h"

#include <filesystem>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
namespace fs = std::filesystem;

// Two 2x2x2 boxes: A at [0,2]^3, B translated +1 in x -> [1,3]x[0,2]x[0,2].
// Overlap = 1*2*2 = 4. Each box volume 8.
static void AddTwoBoxes(Document& doc, uint64_t& aId, uint64_t& bId) {
  doc.AddFeature(
      std::make_unique<BoxFeature>("A", glm::vec3(0, 0, 0), 2.0, 2.0, 2.0));
  aId = doc.GetFeature(doc.FeatureCount() - 1)->GetId();
  doc.AddFeature(
      std::make_unique<BoxFeature>("B", glm::vec3(1, 0, 0), 2.0, 2.0, 2.0));
  bId = doc.GetFeature(doc.FeatureCount() - 1)->GetId();
}

TEST_CASE("Union combines two bodies into one", "[boolean]") {
  Document doc;
  uint64_t a, b;
  AddTwoBoxes(doc, a, b);
  REQUIRE(doc.GetBodies().size() == 2);
  doc.AddFeature(std::make_unique<BooleanFeature>("U", a, b, BooleanOp::Union));

  REQUIRE(doc.GetBodies().size() == 1); // both consumed
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(),
             WithinRel(8.0 + 8.0 - 4.0, 1e-4)); // 12
}

TEST_CASE("Intersect yields the overlap", "[boolean]") {
  Document doc;
  uint64_t a, b;
  AddTwoBoxes(doc, a, b);
  doc.AddFeature(
      std::make_unique<BooleanFeature>("I", a, b, BooleanOp::Intersect));
  REQUIRE(doc.GetBodies().size() == 1);
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(4.0, 1e-4));
}

TEST_CASE("Subtract removes the tool from the target", "[boolean]") {
  Document doc;
  uint64_t a, b;
  AddTwoBoxes(doc, a, b);
  doc.AddFeature(
      std::make_unique<BooleanFeature>("S", a, b, BooleanOp::Subtract));
  REQUIRE(doc.GetBodies().size() == 1);
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(),
             WithinRel(8.0 - 4.0, 1e-4)); // 4
}

TEST_CASE("The boolean result carries this feature's id", "[boolean]") {
  Document doc;
  uint64_t a, b;
  AddTwoBoxes(doc, a, b);
  doc.AddFeature(std::make_unique<BooleanFeature>("U", a, b, BooleanOp::Union));
  const uint64_t boolId = doc.GetFeature(2)->GetId();
  REQUIRE(doc.GetBodies().size() == 1);
  CHECK(doc.GetBodies()[0].createdBy == boolId);
}

TEST_CASE("A boolean chains onto another boolean's result", "[boolean]") {
  Document doc;
  uint64_t a, b;
  AddTwoBoxes(doc, a, b);
  // First union A|B -> one body owned by the union.
  doc.AddFeature(std::make_unique<BooleanFeature>("U", a, b, BooleanOp::Union));
  const uint64_t unionId = doc.GetFeature(2)->GetId();
  // A third box C at [3,5] (disjoint from the union's [0,3]), then union with
  // it.
  doc.AddFeature(
      std::make_unique<BoxFeature>("C", glm::vec3(3, 0, 0), 2.0, 2.0, 2.0));
  const uint64_t cId = doc.GetFeature(3)->GetId();
  doc.AddFeature(
      std::make_unique<BooleanFeature>("U2", unionId, cId, BooleanOp::Union));

  REQUIRE(doc.GetBodies().size() == 1);
  // (A|B) volume 12, plus a face-adjacent C (vol 8), touching at x=3 -> 20.
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(20.0, 1e-4));
}

TEST_CASE("A boolean with a missing operand fails", "[boolean]") {
  Document doc;
  uint64_t a, b;
  AddTwoBoxes(doc, a, b);
  doc.AddFeature(
      std::make_unique<BooleanFeature>("U", a, 9999, BooleanOp::Union));
  CHECK_FALSE(doc.Regenerate());
}

TEST_CASE("A boolean with identical operands fails", "[boolean]") {
  Document doc;
  uint64_t a, b;
  AddTwoBoxes(doc, a, b);
  doc.AddFeature(std::make_unique<BooleanFeature>("U", a, a, BooleanOp::Union));
  CHECK_FALSE(doc.Regenerate());
}

TEST_CASE("Intersecting disjoint bodies fails (empty result)", "[boolean]") {
  Document doc;
  doc.AddFeature(
      std::make_unique<BoxFeature>("A", glm::vec3(0, 0, 0), 2.0, 2.0, 2.0));
  const uint64_t a = doc.GetFeature(0)->GetId();
  doc.AddFeature(std::make_unique<BoxFeature>("B", glm::vec3(5, 0, 0), 2.0, 2.0,
                                              2.0)); // far away
  const uint64_t b = doc.GetFeature(1)->GetId();
  doc.AddFeature(
      std::make_unique<BooleanFeature>("I", a, b, BooleanOp::Intersect));
  CHECK_FALSE(doc.Regenerate());
}

TEST_CASE("Boolean round-trips through JSON", "[boolean]") {
  const fs::path path =
      fs::temp_directory_path() / "designer_boolean_test.json";
  {
    Document doc;
    uint64_t a, b;
    AddTwoBoxes(doc, a, b);
    doc.AddFeature(
        std::make_unique<BooleanFeature>("Cut", a, b, BooleanOp::Subtract));
    REQUIRE(testio::Save(doc, path.string()));
  }
  const auto loaded = testio::Load(path.string());
  REQUIRE(loaded != nullptr);
  auto* boo = dynamic_cast<BooleanFeature*>(loaded->GetFeature(2));
  REQUIRE(boo != nullptr);
  CHECK(boo->op == BooleanOp::Subtract);
  REQUIRE(loaded->GetBodies().size() == 1);
  CHECK_THAT(loaded->GetBodies()[0].shape.Volume(), WithinRel(4.0, 1e-4));
  fs::remove(path);
}

TEST_CASE("Boolean regeneration is deterministic", "[boolean]") {
  Document doc;
  uint64_t a, b;
  AddTwoBoxes(doc, a, b);
  doc.AddFeature(std::make_unique<BooleanFeature>("U", a, b, BooleanOp::Union));
  const double v1 = doc.GetBodies()[0].shape.Volume();
  doc.Regenerate();
  const double v2 = doc.GetBodies()[0].shape.Volume();
  CHECK_THAT(v1, WithinAbs(v2, 1e-9));
}

TEST_CASE("A boolean with a missing operand reports why", "[boolean][error]") {
  Document doc;
  uint64_t a, b;
  AddTwoBoxes(doc, a, b);
  doc.AddFeature(
      std::make_unique<BooleanFeature>("Cut", a, 9999, BooleanOp::Subtract));
  CHECK_FALSE(doc.Regenerate());
  const std::string err = doc.GetFeature(2)->GetError();
  CHECK(err.find("operand body") != std::string::npos);
}
