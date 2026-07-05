#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/Command.h"
#include "core/Document.h"
#include "core/Feature.h"

using Catch::Matchers::WithinRel;

static std::unique_ptr<Command> MakeAddBox(const std::string& name,
                                           double size = 2.0) {
  return std::make_unique<AddFeatureCommand>(
      std::make_unique<BoxFeature>(name, glm::vec3(0), size, size, size));
}

TEST_CASE("Adding a box feature produces a body", "[document]") {
  Document doc;
  CommandStack stack;

  REQUIRE(stack.Execute(MakeAddBox("Box 1"), doc));

  REQUIRE(doc.FeatureCount() == 1);
  REQUIRE(doc.GetBodies().size() == 1);
  CHECK(doc.GetBodies()[0].name == "Box 1");
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(8.0, 1e-6));
}

TEST_CASE("Undo and redo restore document state", "[document][undo]") {
  Document doc;
  CommandStack stack;

  stack.Execute(MakeAddBox("Box 1"), doc);
  stack.Execute(MakeAddBox("Box 2", 3.0), doc);
  REQUIRE(doc.GetBodies().size() == 2);

  stack.Undo(doc);
  REQUIRE(doc.FeatureCount() == 1);
  REQUIRE(doc.GetBodies().size() == 1);
  CHECK(doc.GetBodies()[0].name == "Box 1");

  stack.Redo(doc);
  REQUIRE(doc.GetBodies().size() == 2);
  CHECK_THAT(doc.GetBodies()[1].shape.Volume(), WithinRel(27.0, 1e-6));
}

TEST_CASE("New command clears the redo stack", "[document][undo]") {
  Document doc;
  CommandStack stack;

  stack.Execute(MakeAddBox("Box 1"), doc);
  stack.Undo(doc);
  REQUIRE(stack.CanRedo());

  stack.Execute(MakeAddBox("Box 2"), doc);
  CHECK_FALSE(stack.CanRedo());
  REQUIRE(doc.GetBodies().size() == 1);
  CHECK(doc.GetBodies()[0].name == "Box 2");
}

TEST_CASE("RemoveFeatureCommand round-trips through undo", "[document][undo]") {
  Document doc;
  CommandStack stack;

  stack.Execute(MakeAddBox("Box 1"), doc);
  stack.Execute(MakeAddBox("Box 2"), doc);

  REQUIRE(stack.Execute(std::make_unique<RemoveFeatureCommand>(0), doc));
  REQUIRE(doc.GetBodies().size() == 1);
  CHECK(doc.GetBodies()[0].name == "Box 2");

  stack.Undo(doc);
  REQUIRE(doc.GetBodies().size() == 2);
  CHECK(doc.GetBodies()[0].name == "Box 1");
}

TEST_CASE("Failed command is not pushed onto the undo stack", "[document]") {
  Document doc;
  CommandStack stack;

  CHECK_FALSE(stack.Execute(std::make_unique<RemoveFeatureCommand>(5), doc));
  CHECK_FALSE(stack.CanUndo());
}

TEST_CASE("Regeneration is deterministic", "[document]") {
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>("A", glm::vec3(0), 1, 1, 1));
  doc.AddFeature(std::make_unique<BoxFeature>("B", glm::vec3(5, 0, 0), 2, 2, 2));

  uint64_t v1 = doc.Version();
  REQUIRE(doc.Regenerate());
  CHECK(doc.Version() == v1 + 1);

  REQUIRE(doc.GetBodies().size() == 2);
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(1.0, 1e-6));
  CHECK_THAT(doc.GetBodies()[1].shape.Volume(), WithinRel(8.0, 1e-6));
}
