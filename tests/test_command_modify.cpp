#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/Command.h"
#include "core/Document.h"
#include "core/Feature.h"
#include "core/io/Serialization.h"

using Catch::Matchers::WithinRel;

static std::unique_ptr<BoxFeature> MakeBox(const std::string& name,
                                           double w = 2.0, double h = 2.0,
                                           double d = 2.0) {
  return std::make_unique<BoxFeature>(name, glm::vec3(0), w, h, d);
}

TEST_CASE("ReplaceFeature swaps parameters and regenerates", "[document]") {
  Document doc;
  doc.AddFeature(MakeBox("Box 1"));
  const uint64_t versionBefore = doc.Version();

  REQUIRE(doc.ReplaceFeature(0, MakeBox("Box 1", 5.0, 2.0, 2.0)));

  CHECK(doc.Version() == versionBefore + 1);
  REQUIRE(doc.GetBodies().size() == 1);
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(20.0, 1e-6));
}

TEST_CASE("ReplaceFeature rejects bad input", "[document]") {
  Document doc;
  doc.AddFeature(MakeBox("Box 1"));
  const uint64_t version = doc.Version();

  CHECK_FALSE(doc.ReplaceFeature(1, MakeBox("out of range")));
  CHECK_FALSE(doc.ReplaceFeature(0, nullptr));
  CHECK(doc.Version() == version);
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(8.0, 1e-6));
}

TEST_CASE("ModifyFeatureCommand execute/undo/redo roundtrip",
          "[document][undo]") {
  Document doc;
  CommandStack stack;
  doc.AddFeature(MakeBox("Box 1"));

  auto before = DocumentIO::FeatureToJson(*doc.GetFeature(0));
  auto modified = MakeBox("Box 1", 5.0, 2.0, 2.0);
  auto after = DocumentIO::FeatureToJson(*modified);

  REQUIRE(stack.Execute(
      std::make_unique<ModifyFeatureCommand>(0, before, after), doc));
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(20.0, 1e-6));

  stack.Undo(doc);
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(8.0, 1e-6));

  stack.Redo(doc);
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(20.0, 1e-6));
}

TEST_CASE("ModifyFeatureCommand execute is idempotent (live-edit pattern)",
          "[document][undo]") {
  Document doc;
  doc.AddFeature(MakeBox("Box 1"));
  auto before = DocumentIO::FeatureToJson(*doc.GetFeature(0));

  // Simulate a live drag: the UI mutates the feature in place first...
  auto* box = dynamic_cast<BoxFeature*>(doc.GetFeature(0));
  REQUIRE(box != nullptr);
  box->width = 5.0;
  doc.Regenerate();
  auto after = DocumentIO::FeatureToJson(*doc.GetFeature(0));

  // ...then the command executes on release over already-current state.
  ModifyFeatureCommand cmd(0, before, after);
  REQUIRE(cmd.Execute(doc));
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(20.0, 1e-6));
  REQUIRE(cmd.Execute(doc)); // twice == once
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(20.0, 1e-6));

  cmd.Undo(doc);
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(8.0, 1e-6));
}

TEST_CASE("FeatureToJson/FromJson roundtrip for every feature type",
          "[serialization]") {
  std::vector<std::unique_ptr<Feature>> features;
  features.push_back(
      std::make_unique<BoxFeature>("b", glm::vec3(1, 2, 3), 2, 3, 4));
  features.push_back(
      std::make_unique<SphereFeature>("s", glm::vec3(-1, 0, 1), 1.5));
  features.push_back(std::make_unique<CylinderFeature>(
      "c", glm::vec3(0, 1, 0), glm::vec3(0, 0, 1), 0.5, 2.0));
  features.push_back(std::make_unique<ImportFeature>("i", "part.step"));

  for (const auto& feature : features) {
    auto j = DocumentIO::FeatureToJson(*feature);
    auto restored = DocumentIO::FeatureFromJson(j);
    REQUIRE(restored != nullptr);
    CHECK(restored->TypeName() == feature->TypeName());
    CHECK(restored->GetName() == feature->GetName());
    CHECK(DocumentIO::FeatureToJson(*restored) == j);
  }
}

// A replacement changes what a feature IS, never which one it is. The
// modify_feature RPC sends parameters and no "id", and ReplaceFeature used to
// take the incoming id verbatim — so the feature landed with id 0, saved
// itself as "0-box.json", listed 0 in index.json, and dangled every reference
// that named its old id. Stable, never-reused ids are the contract the
// directory format and the merge machinery are built on (F1/F3/F7).
TEST_CASE("ReplaceFeature keeps the feature's stable id", "[document]") {
  Document doc;
  doc.AddFeature(MakeBox("Box 1"));
  const uint64_t id = doc.GetFeature(0)->GetId();
  REQUIRE(id != 0);

  // A replacement built from JSON that carries no identity.
  REQUIRE(doc.ReplaceFeature(0, MakeBox("Box 1", 9.0)));
  CHECK(doc.GetFeature(0)->GetId() == id);
  CHECK(doc.FindFeatureById(id) == doc.GetFeature(0));

  // One that DOES carry an id keeps it — undo re-inserts a feature by
  // identity and must not be second-guessed.
  auto explicitId = MakeBox("Box 1", 4.0);
  explicitId->SetId(77);
  REQUIRE(doc.ReplaceFeature(0, std::move(explicitId)));
  CHECK(doc.GetFeature(0)->GetId() == 77);
}
