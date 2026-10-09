// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/Command.h"
#include "core/Document.h"
#include "core/Feature.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/SketchFeature.h"

using Catch::Matchers::WithinRel;

static const SketchPlane kXY(PlaneType::XY);

static std::unique_ptr<BoxFeature> MakeBox(const std::string& name) {
  return std::make_unique<BoxFeature>(name, glm::vec3(0), 2.0, 2.0, 2.0);
}

// A rectangle sketch + the extrude that consumes it, so the extrude depends on
// the sketch coming earlier in history — the reorder-validation fixture.
static void AddSketchThenExtrude(Document& doc) {
  auto sk = std::make_unique<SketchFeature>("Sketch", kXY);
  sk->AddRectangle({0, 0}, {4, 2});
  doc.AddFeature(std::move(sk));
  const uint64_t sid = doc.GetFeature(doc.FeatureCount() - 1)->GetId();
  doc.AddFeature(
      std::make_unique<ExtrudeFeature>("Pad", sid, 3.0, ExtrudeMode::NewBody));
}

TEST_CASE("MoveFeature reorders independent features", "[document][reorder]") {
  Document doc;
  doc.AddFeature(MakeBox("A")); // z 0..2, disjoint from B
  auto b = std::make_unique<BoxFeature>("B", glm::vec3(10, 0, 0), 2, 2, 2);
  doc.AddFeature(std::move(b));
  const double volBefore =
      doc.GetBodies()[0].shape.Volume() + doc.GetBodies()[1].shape.Volume();

  REQUIRE(doc.MoveFeature(1, 0)); // B ahead of A
  CHECK(doc.GetFeature(0)->GetName() == "B");
  CHECK(doc.GetFeature(1)->GetName() == "A");
  // Two independent solids: reordering changes nothing geometric.
  REQUIRE(doc.GetBodies().size() == 2);
  const double volAfter =
      doc.GetBodies()[0].shape.Volume() + doc.GetBodies()[1].shape.Volume();
  CHECK_THAT(volAfter, WithinRel(volBefore, 1e-9));
}

TEST_CASE("MoveFeature rejects out-of-range and no-op moves",
          "[document][reorder]") {
  Document doc;
  doc.AddFeature(MakeBox("A"));
  doc.AddFeature(MakeBox("B"));
  CHECK_FALSE(doc.MoveFeature(0, 0)); // no-op
  CHECK_FALSE(doc.MoveFeature(5, 0)); // from out of range
  CHECK_FALSE(doc.MoveFeature(0, 5)); // to out of range
  CHECK(doc.GetFeature(0)->GetName() == "A");
}

TEST_CASE("MoveFeatureCommand rejects a move that dangles a reference",
          "[document][reorder][undo]") {
  Document doc;
  AddSketchThenExtrude(doc); // [0]=Sketch, [1]=Pad(extrude of Sketch)
  REQUIRE(doc.GetBodies().size() == 1);
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(24.0, 1e-6));

  // Dragging the extrude ahead of its sketch would leave it referencing a
  // sketch that no longer precedes it: refuse, revert, and name the casualty.
  MoveFeatureCommand cmd(1, 0);
  CHECK_FALSE(cmd.Execute(doc));
  CHECK(cmd.BrokenFeature() == "Pad");
  // Document is untouched: original order and geometry both intact.
  CHECK(doc.GetFeature(0)->GetName() == "Sketch");
  CHECK(doc.GetFeature(1)->GetName() == "Pad");
  CHECK(doc.GetFeature(1)->GetError().empty());
  REQUIRE(doc.GetBodies().size() == 1);
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(24.0, 1e-6));

  // Symmetric case: dragging the sketch after the extrude breaks the same way.
  MoveFeatureCommand cmd2(0, 1);
  CHECK_FALSE(cmd2.Execute(doc));
  CHECK(cmd2.BrokenFeature() == "Pad");
  CHECK(doc.GetFeature(0)->GetName() == "Sketch");
}

TEST_CASE("MoveFeatureCommand execute/undo/redo roundtrip",
          "[document][reorder][undo]") {
  Document doc;
  CommandStack stack;
  doc.AddFeature(MakeBox("A"));
  doc.AddFeature(
      std::make_unique<BoxFeature>("B", glm::vec3(10, 0, 0), 2, 2, 2));
  doc.AddFeature(
      std::make_unique<BoxFeature>("C", glm::vec3(20, 0, 0), 2, 2, 2));

  // Valid move (all independent): C to the front → [C, A, B].
  auto cmd = std::make_unique<MoveFeatureCommand>(2, 0);
  REQUIRE(cmd->Execute(doc));
  stack.PushApplied(std::move(cmd)); // effect already applied by Execute
  CHECK(doc.GetFeature(0)->GetName() == "C");
  CHECK(doc.GetFeature(1)->GetName() == "A");
  CHECK(doc.GetFeature(2)->GetName() == "B");

  stack.Undo(doc); // back to [A, B, C]
  CHECK(doc.GetFeature(0)->GetName() == "A");
  CHECK(doc.GetFeature(1)->GetName() == "B");
  CHECK(doc.GetFeature(2)->GetName() == "C");

  stack.Redo(doc); // [C, A, B] again
  CHECK(doc.GetFeature(0)->GetName() == "C");
  CHECK(doc.GetFeature(2)->GetName() == "B");
}
