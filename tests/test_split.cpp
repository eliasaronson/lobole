#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestIO.h"
#include "core/Document.h"
#include "core/Feature.h"
#include "core/features/SplitFeature.h"
#include "core/io/Serialization.h"

#include <filesystem>

using Catch::Matchers::WithinRel;
namespace fs = std::filesystem;

// A 2x2x2 box occupying [0,2]^3 (BoxFeature position is the min corner), vol 8.
static uint64_t AddBox(Document& doc) {
  doc.AddFeature(
      std::make_unique<BoxFeature>("B", glm::vec3(0, 0, 0), 2.0, 2.0, 2.0));
  return doc.GetFeature(doc.FeatureCount() - 1)->GetId();
}

// Splits the box with the XY base plane raised to z = `offset`.
static SplitFeature* AddPlaneSplit(Document& doc, uint64_t bodyId,
                                   double offset) {
  auto sp = std::make_unique<SplitFeature>("Split", bodyId);
  sp->knife = SplitKnifeKind::Plane;
  sp->basePlane = PlaneType::XY;
  sp->offset = offset;
  auto* raw = sp.get();
  doc.AddFeature(std::move(sp));
  return raw;
}

TEST_CASE("A plane splits a box into two pieces", "[split]") {
  Document doc;
  const uint64_t b = AddBox(doc);
  REQUIRE(doc.GetBodies().size() == 1);
  AddPlaneSplit(doc, b, 1.0); // z = 1 halves [0,2]

  REQUIRE(doc.GetBodies().size() == 2); // source consumed, two pieces
  double total = 0.0;
  for (const auto& body : doc.GetBodies()) {
    CHECK_THAT(body.shape.Volume(), WithinRel(4.0, 1e-4)); // each half
    total += body.shape.Volume();
  }
  CHECK_THAT(total, WithinRel(8.0, 1e-4)); // volume conserved
}

TEST_CASE("An off-center plane splits proportionally", "[split]") {
  Document doc;
  const uint64_t b = AddBox(doc);
  AddPlaneSplit(doc, b, 0.5); // z = 0.5 -> pieces of 2 and 6

  REQUIRE(doc.GetBodies().size() == 2);
  double total = 0.0;
  for (const auto& body : doc.GetBodies())
    total += body.shape.Volume();
  CHECK_THAT(total, WithinRel(8.0, 1e-4));
}

TEST_CASE("Both split pieces carry this feature's id", "[split]") {
  Document doc;
  const uint64_t b = AddBox(doc);
  auto* sp = AddPlaneSplit(doc, b, 1.0);
  REQUIRE(doc.GetBodies().size() == 2);
  for (const auto& body : doc.GetBodies())
    CHECK(body.createdBy == sp->GetId());
}

TEST_CASE("Each split piece names its own faces", "[split]") {
  Document doc;
  const uint64_t b = AddBox(doc);
  AddPlaneSplit(doc, b, 1.0);
  REQUIRE(doc.GetBodies().size() == 2);
  // The "<id>/bN" prefix makes each piece's faces individually resolvable,
  // so a box half's 6 faces are all named.
  for (const auto& body : doc.GetBodies())
    CHECK(body.elementMap.names.size() == 6);
}

TEST_CASE("A plane that misses the body fails to split", "[split]") {
  Document doc;
  const uint64_t b = AddBox(doc);
  AddPlaneSplit(doc, b, 5.0); // z = 5 is outside [0,2]
  CHECK_FALSE(doc.Regenerate());
}

TEST_CASE("A split with a missing body fails", "[split]") {
  Document doc;
  AddBox(doc);
  AddPlaneSplit(doc, 9999, 1.0);
  CHECK_FALSE(doc.Regenerate());
}

TEST_CASE("Split regeneration is deterministic", "[split]") {
  Document doc;
  const uint64_t b = AddBox(doc);
  AddPlaneSplit(doc, b, 1.0);
  REQUIRE(doc.GetBodies().size() == 2);
  doc.Regenerate();
  REQUIRE(doc.GetBodies().size() == 2);
  for (const auto& body : doc.GetBodies())
    CHECK_THAT(body.shape.Volume(), WithinRel(4.0, 1e-4));
}

TEST_CASE("A plane-knife split round-trips through JSON", "[split]") {
  const fs::path path = fs::temp_directory_path() / "designer_split_test.json";
  {
    Document doc;
    const uint64_t b = AddBox(doc);
    AddPlaneSplit(doc, b, 1.0);
    REQUIRE(testio::Save(doc, path.string()));
  }
  const auto loaded = testio::Load(path.string());
  REQUIRE(loaded != nullptr);
  auto* sp = dynamic_cast<SplitFeature*>(loaded->GetFeature(1));
  REQUIRE(sp != nullptr);
  CHECK(sp->knife == SplitKnifeKind::Plane);
  CHECK(sp->basePlane == PlaneType::XY);
  CHECK_THAT(sp->offset, WithinRel(1.0, 1e-9));
  REQUIRE(loaded->GetBodies().size() == 2);
}

TEST_CASE("A face-knife split round-trips through JSON", "[split]") {
  const fs::path path =
      fs::temp_directory_path() / "designer_split_face_test.json";
  uint64_t bId = 0;
  {
    Document doc;
    bId = AddBox(doc);
    auto sp = std::make_unique<SplitFeature>("Split", bId);
    sp->knife = SplitKnifeKind::Face;
    sp->face = ElementRef{bId, "someface"};
    doc.AddFeature(std::move(sp));
    REQUIRE(testio::Save(doc, path.string()));
  }
  const auto loaded = testio::Load(path.string());
  REQUIRE(loaded != nullptr);
  auto* sp = dynamic_cast<SplitFeature*>(loaded->GetFeature(1));
  REQUIRE(sp != nullptr);
  CHECK(sp->knife == SplitKnifeKind::Face);
  CHECK(sp->face.featureId == bId); // ref preserved
  CHECK(sp->face.name == "someface");
  fs::remove(path);
}
