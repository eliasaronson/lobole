// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// RemoveFeaturesCommand: a multi-selection's Delete is ONE undo step that
// regenerates once, and Undo puts every feature back where it was.
#include <catch2/catch_test_macros.hpp>

#include "core/Command.h"
#include "core/Document.h"
#include "core/Feature.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/SketchFeature.h"

namespace {

std::unique_ptr<BoxFeature> MakeBox(const std::string& name, float x) {
  return std::make_unique<BoxFeature>(name, glm::vec3(x, 0, 0), 2.0, 2.0, 2.0);
}

std::vector<std::string> Names(Document& doc) {
  std::vector<std::string> out;
  for (size_t i = 0; i < doc.FeatureCount(); i++)
    out.push_back(doc.GetFeature(i)->GetName());
  return out;
}

} // namespace

TEST_CASE("RemoveFeatures takes several in one step and undoes them in place",
          "[document][command]") {
  Document doc;
  CommandStack stack;
  for (const char* n : {"A", "B", "C", "D", "E"})
    doc.AddFeature(MakeBox(n, static_cast<float>(n[0] - 'A') * 10.0f));
  const std::vector<uint64_t> ids = {doc.GetFeature(1)->GetId(),
                                     doc.GetFeature(3)->GetId(),
                                     doc.GetFeature(0)->GetId()};

  // Indices in the order the user picked them, not sorted.
  REQUIRE(stack.Execute(
      std::make_unique<RemoveFeaturesCommand>(std::vector<size_t>{1, 3, 0}),
      doc));
  CHECK(Names(doc) == std::vector<std::string>{"C", "E"});
  CHECK(doc.GetBodies().size() == 2);
  REQUIRE(stack.CanUndo());

  // One Ctrl+Z, and every feature is back at its original index with its
  // original id.
  stack.Undo(doc);
  CHECK(Names(doc) == std::vector<std::string>{"A", "B", "C", "D", "E"});
  CHECK(doc.GetFeature(1)->GetId() == ids[0]);
  CHECK(doc.GetFeature(3)->GetId() == ids[1]);
  CHECK(doc.GetFeature(0)->GetId() == ids[2]);
  CHECK(doc.GetBodies().size() == 5);
  CHECK_FALSE(stack.CanUndo());

  stack.Redo(doc);
  CHECK(Names(doc) == std::vector<std::string>{"C", "E"});
}

TEST_CASE("RemoveFeatures refuses an out-of-range index and removes nothing",
          "[document][command]") {
  Document doc;
  CommandStack stack;
  doc.AddFeature(MakeBox("A", 0.0f));
  doc.AddFeature(MakeBox("B", 10.0f));
  CHECK_FALSE(stack.Execute(
      std::make_unique<RemoveFeaturesCommand>(std::vector<size_t>{0, 7}), doc));
  CHECK(doc.FeatureCount() == 2);
  CHECK_FALSE(stack.CanUndo());
  CHECK_FALSE(stack.Execute(
      std::make_unique<RemoveFeaturesCommand>(std::vector<size_t>{}), doc));
}

TEST_CASE("RemoveFeatures describes one feature by name and several by count",
          "[document][command]") {
  Document doc;
  doc.AddFeature(MakeBox("A", 0.0f));
  doc.AddFeature(MakeBox("B", 10.0f));
  doc.AddFeature(MakeBox("C", 20.0f));
  RemoveFeaturesCommand one({1});
  REQUIRE(one.Execute(doc));
  CHECK(one.Description() == "Remove Box 'B'");
  RemoveFeaturesCommand two({0, 1});
  REQUIRE(two.Execute(doc));
  CHECK(two.Description() == "Remove 2 features");
  CHECK(doc.FeatureCount() == 0);
}

TEST_CASE("RemoveFeatures leaves a dependent broken rather than refusing, "
          "as a single delete does",
          "[document][command]") {
  Document doc;
  auto sk =
      std::make_unique<SketchFeature>("Sketch", SketchPlane(PlaneType::XY));
  sk->AddRectangle({0, 0}, {4, 2});
  doc.AddFeature(std::move(sk));
  const uint64_t sid = doc.GetFeature(0)->GetId();
  doc.AddFeature(
      std::make_unique<ExtrudeFeature>("Pad", sid, 3.0, ExtrudeMode::NewBody));
  doc.AddFeature(MakeBox("Box", 30.0f));
  REQUIRE(doc.GetFeature(1)->GetError().empty());

  CommandStack stack;
  REQUIRE(stack.Execute(
      std::make_unique<RemoveFeaturesCommand>(std::vector<size_t>{0, 2}), doc));
  REQUIRE(doc.FeatureCount() == 1);
  CHECK(doc.GetFeature(0)->GetName() == "Pad");
  CHECK_FALSE(doc.GetFeature(0)->GetError().empty()); // its sketch is gone
  stack.Undo(doc);
  REQUIRE(doc.FeatureCount() == 3);
  CHECK(doc.GetFeature(1)->GetError().empty()); // and back
}
