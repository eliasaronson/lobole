#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/Command.h"
#include "core/Document.h"
#include "core/Serialization.h"
#include "core/SketchFeature.h"

#include <filesystem>
#include <fstream>

using Catch::Matchers::WithinAbs;
namespace fs = std::filesystem;

static std::unique_ptr<BoxFeature> MakeBox(const std::string& name) {
  return std::make_unique<BoxFeature>(name, glm::vec3(0), 2.0, 2.0, 2.0);
}

TEST_CASE("Features get unique, monotonic ids", "[ids]") {
  Document doc;
  doc.AddFeature(MakeBox("A"));
  doc.AddFeature(MakeBox("B"));

  const uint64_t idA = doc.GetFeature(0)->GetId();
  const uint64_t idB = doc.GetFeature(1)->GetId();
  CHECK(idA != 0);
  CHECK(idB > idA);

  size_t index = 99;
  CHECK(doc.FindFeatureById(idB, &index) == doc.GetFeature(1));
  CHECK(index == 1);
  CHECK(doc.FindFeatureById(9999) == nullptr);
}

TEST_CASE("Ids survive remove-undo and modify round-trips", "[ids][undo]") {
  Document doc;
  CommandStack stack;
  stack.Execute(std::make_unique<AddFeatureCommand>(MakeBox("A")), doc);
  stack.Execute(std::make_unique<AddFeatureCommand>(MakeBox("B")), doc);
  const uint64_t idA = doc.GetFeature(0)->GetId();

  // Remove + undo re-inserts the same object: id kept, counter not confused
  stack.Execute(std::make_unique<RemoveFeatureCommand>(0), doc);
  stack.Undo(doc);
  CHECK(doc.GetFeature(0)->GetId() == idA);

  // Modify round-trips the id through JSON
  auto before = DocumentIO::FeatureToJson(*doc.GetFeature(0));
  auto after = before;
  after["width"] = 5.0;
  stack.Execute(std::make_unique<ModifyFeatureCommand>(0, before, after), doc);
  CHECK(doc.GetFeature(0)->GetId() == idA);

  // New features never collide with existing ids
  stack.Execute(std::make_unique<AddFeatureCommand>(MakeBox("C")), doc);
  CHECK(doc.GetFeature(2)->GetId() > doc.GetFeature(1)->GetId());
}

TEST_CASE("Explicit ids from files/RPC advance the counter", "[ids]") {
  Document doc;
  auto boxWithId = MakeBox("X");
  boxWithId->SetId(50);
  doc.AddFeature(std::move(boxWithId));
  doc.AddFeature(MakeBox("Y"));
  CHECK(doc.GetFeature(1)->GetId() == 51);
}

TEST_CASE("v1 documents without ids load and get ids assigned", "[ids][io]") {
  fs::path path = fs::temp_directory_path() / "designer_v1_doc.json";
  {
    std::ofstream out(path);
    out << R"({"format": "designer-document", "version": 1, "features": [
      {"type": "Box", "name": "Old", "position": [0,0,0],
       "width": 2, "height": 2, "depth": 2}]})";
  }
  std::string error;
  auto doc = DocumentIO::Load(path.string(), &error);
  REQUIRE(doc != nullptr);
  CHECK(doc->GetFeature(0)->GetId() != 0);
  fs::remove(path);
}

TEST_CASE("SketchFeature JSON round-trip with all entity types", "[sketch]") {
  SketchPlane plane(glm::vec3(1, 2, 3), glm::vec3(0, 1, 1),
                    glm::vec3(1, 0, 0));
  auto sk = std::make_unique<SketchFeature>("Sketch 1", plane);
  sk->entities.push_back(sketch::Point{{1.0f, 2.0f}});
  sk->entities.push_back(sketch::Line{{0.0f, 0.0f}, {4.0f, 0.0f}});
  sk->entities.push_back(sketch::Circle{{2.0f, 1.0f}, 0.5});
  sk->entities.push_back(sketch::Rectangle{{0.0f, 0.0f}, {4.0f, 2.0f}});
  sk->SetId(7);

  auto j = DocumentIO::FeatureToJson(*sk);
  auto restored = DocumentIO::FeatureFromJson(j);
  auto* rsk = dynamic_cast<SketchFeature*>(restored.get());
  REQUIRE(rsk != nullptr);

  CHECK(rsk->GetId() == 7);
  CHECK(rsk->entities.size() == 4);
  CHECK(DocumentIO::FeatureToJson(*rsk) == j);

  // Plane axes reconstructed orthonormally
  CHECK_THAT(glm::length(rsk->plane.GetNormal()), WithinAbs(1.0f, 1e-4f));
  CHECK_THAT(glm::dot(rsk->plane.GetNormal(), rsk->plane.GetUAxis()),
             WithinAbs(0.0f, 1e-4f));
  glm::vec3 world = rsk->plane.PlaneToWorld({1.0f, 1.0f});
  glm::vec3 expected = sk->plane.PlaneToWorld({1.0f, 1.0f});
  CHECK_THAT(glm::length(world - expected), WithinAbs(0.0f, 1e-4f));

  // A sketch contributes no bodies
  Document doc;
  doc.AddFeature(std::move(restored));
  doc.AddFeature(std::make_unique<BoxFeature>("Box", glm::vec3(0), 1, 1, 1));
  REQUIRE(doc.Regenerate());
  CHECK(doc.GetBodies().size() == 1);
}

TEST_CASE("Sketch constraints round-trip through JSON", "[sketch][serialization]") {
  auto sk = std::make_unique<SketchFeature>("S", SketchPlane(PlaneType::XY));
  sk->entities.push_back(sketch::Line{{0, 0}, {4, 0}});
  sk->entities.push_back(sketch::Circle{{2, 1}, 0.5});
  sk->constraints.push_back(
      {sketch::ConstraintKind::Horizontal, {0, 0}, {0, 0}, 0.0});
  sk->constraints.push_back(
      {sketch::ConstraintKind::Distance, {0, 0}, {0, 1}, 4.0});
  sk->constraints.push_back(
      {sketch::ConstraintKind::Radius, {1, 0}, {0, 0}, 0.5});
  sk->entities.push_back(sketch::Line{{0, 0}, {0, 3}});
  sk->entities.push_back(sketch::Point{{1, 1}});
  sk->constraints.push_back(
      {sketch::ConstraintKind::Angle, {0, 0}, {2, 0}, glm::radians(90.0)});
  sk->constraints.push_back(
      {sketch::ConstraintKind::Tangent, {0, 0}, {1, 0}, 0.0});
  sk->constraints.push_back(
      {sketch::ConstraintKind::PointOnLine, {3, 0}, {0, 0}, 0.0});

  auto j = DocumentIO::FeatureToJson(*sk);
  auto restored = DocumentIO::FeatureFromJson(j);
  auto* rsk = dynamic_cast<SketchFeature*>(restored.get());
  REQUIRE(rsk != nullptr);
  REQUIRE(rsk->constraints.size() == 6);
  CHECK(rsk->constraints[0].kind == sketch::ConstraintKind::Horizontal);
  CHECK(rsk->constraints[1].kind == sketch::ConstraintKind::Distance);
  CHECK(rsk->constraints[1].value == 4.0);
  CHECK(rsk->constraints[2].a.entity == 1u);
  CHECK(rsk->constraints[3].kind == sketch::ConstraintKind::Angle);
  CHECK(rsk->constraints[4].kind == sketch::ConstraintKind::Tangent);
  CHECK(rsk->constraints[5].kind == sketch::ConstraintKind::PointOnLine);
  // Kind strings survive as expected.
  CHECK(j["constraints"][3]["kind"] == "angle");
  CHECK(j["constraints"][4]["kind"] == "tangent");
  CHECK(j["constraints"][5]["kind"] == "point_on_line");
  CHECK(DocumentIO::FeatureToJson(*rsk) == j);
}
