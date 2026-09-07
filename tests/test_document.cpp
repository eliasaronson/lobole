#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestIO.h"
#include "common/BuildInfo.h"
#include "core/Command.h"
#include "core/Document.h"
#include "core/Feature.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/SketchFeature.h"
#include "core/io/Serialization.h"

#include <filesystem>

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
  doc.AddFeature(
      std::make_unique<BoxFeature>("B", glm::vec3(5, 0, 0), 2, 2, 2));

  uint64_t v1 = doc.Version();
  REQUIRE(doc.Regenerate());
  CHECK(doc.Version() == v1 + 1);

  REQUIRE(doc.GetBodies().size() == 2);
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(1.0, 1e-6));
  CHECK_THAT(doc.GetBodies()[1].shape.Volume(), WithinRel(8.0, 1e-6));
}

TEST_CASE("Regeneration continues past a failed feature", "[document][regen]") {
  Document doc;
  doc.AddFeature(
      std::make_unique<BoxFeature>("A", glm::vec3(0), 1.0, 1.0, 1.0));
  // Extrude referencing a nonexistent sketch: fails, contributes nothing.
  doc.AddFeature(std::make_unique<ExtrudeFeature>("Broken", 9999, 1.0,
                                                  ExtrudeMode::NewBody));
  doc.AddFeature(
      std::make_unique<BoxFeature>("B", glm::vec3(5, 0, 0), 1.0, 1.0, 1.0));
  CHECK_FALSE(doc.Regenerate());        // something errored...
  REQUIRE(doc.GetBodies().size() == 2); // ...but B still regenerated
  CHECK(!doc.GetFeature(1)->GetError().empty());
  CHECK(doc.GetFeature(0)->GetError().empty());
  CHECK(doc.GetFeature(2)->GetError().empty());
}

TEST_CASE("Suppressed features are skipped and round-trip",
          "[document][regen]") {
  Document doc;
  doc.AddFeature(
      std::make_unique<BoxFeature>("A", glm::vec3(0), 1.0, 1.0, 1.0));
  doc.AddFeature(
      std::make_unique<BoxFeature>("B", glm::vec3(5, 0, 0), 2.0, 1.0, 1.0));
  REQUIRE(doc.GetBodies().size() == 2);

  doc.GetFeature(0)->SetSuppressed(true);
  REQUIRE(doc.Regenerate()); // suppressed is not an error
  REQUIRE(doc.GetBodies().size() == 1);
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(2.0, 1e-9));

  // Referencing a suppressed feature's output fails like a deleted one.
  Document doc2;
  auto sk = std::make_unique<SketchFeature>("S", SketchPlane(PlaneType::XY));
  sk->AddRectangle({0, 0}, {4, 2});
  sk->SetSuppressed(true);
  doc2.AddFeature(std::move(sk));
  const uint64_t skId = doc2.GetFeature(0)->GetId();
  doc2.AddFeature(
      std::make_unique<ExtrudeFeature>("Pad", skId, 3.0, ExtrudeMode::NewBody));
  CHECK_FALSE(doc2.Regenerate());
  CHECK(doc2.GetBodies().empty());
  CHECK(!doc2.GetFeature(1)->GetError().empty());

  // suppressed survives save/load (v19).
  const auto path = std::filesystem::temp_directory_path() / "suppress_rt.json";
  std::string error;
  REQUIRE(testio::Save(doc2, path.string()));
  auto loaded = testio::Load(path.string());
  REQUIRE(loaded != nullptr);
  CHECK(loaded->GetFeature(0)->IsSuppressed());
  CHECK_FALSE(loaded->GetFeature(1)->IsSuppressed());
  std::filesystem::remove(path);
}

TEST_CASE("Rollback bar regenerates only the prefix and round-trips",
          "[document][rollback]") {
  Document doc;
  doc.AddFeature(
      std::make_unique<BoxFeature>("A", glm::vec3(0), 1.0, 1.0, 1.0));
  doc.AddFeature(
      std::make_unique<BoxFeature>("B", glm::vec3(5, 0, 0), 2.0, 1.0, 1.0));
  doc.AddFeature(
      std::make_unique<BoxFeature>("C", glm::vec3(10, 0, 0), 3.0, 1.0, 1.0));
  REQUIRE(doc.GetBodies().size() == 3);

  doc.SetRollback(1); // only A
  REQUIRE(doc.GetBodies().size() == 1);
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(1.0, 1e-9));
  CHECK(doc.GetFeature(2)->GetError().empty()); // no stale errors past the bar

  // Persisted with the document.
  const auto path = std::filesystem::temp_directory_path() / "rollback.json";
  std::string error;
  REQUIRE(testio::Save(doc, path.string()));
  auto loaded = testio::Load(path.string());
  REQUIRE(loaded != nullptr);
  CHECK(loaded->Rollback() == 1);
  CHECK(loaded->GetBodies().size() == 1);
  std::filesystem::remove(path);

  // Rolling to the end restores everything; out-of-range means end.
  doc.SetRollback(-1);
  CHECK(doc.GetBodies().size() == 3);
  doc.SetRollback(99);
  CHECK(doc.Rollback() == -1);
  CHECK(doc.GetBodies().size() == 3);
}

TEST_CASE("Incremental regeneration re-runs only from the first change",
          "[document][regen][cache]") {
  Document doc;
  doc.AddFeature(
      std::make_unique<BoxFeature>("A", glm::vec3(0), 1.0, 1.0, 1.0));
  doc.AddFeature(
      std::make_unique<BoxFeature>("B", glm::vec3(5, 0, 0), 2.0, 1.0, 1.0));
  doc.AddFeature(
      std::make_unique<BoxFeature>("C", glm::vec3(10, 0, 0), 3.0, 1.0, 1.0));

  // Nothing changed: fully cached.
  REQUIRE(doc.Regenerate());
  CHECK(doc.LastRegenStats().executed == 0);
  CHECK(doc.LastRegenStats().cached == 3);
  REQUIRE(doc.GetBodies().size() == 3);
  CHECK_THAT(doc.GetBodies()[2].shape.Volume(), WithinRel(3.0, 1e-9));

  // Edit the LAST feature: only it re-runs.
  dynamic_cast<BoxFeature*>(doc.GetFeature(2))->width = 4.0;
  REQUIRE(doc.Regenerate());
  CHECK(doc.LastRegenStats().executed == 1);
  CHECK(doc.LastRegenStats().cached == 2);
  CHECK_THAT(doc.GetBodies()[2].shape.Volume(), WithinRel(4.0, 1e-9));

  // Edit the FIRST: everything after re-runs.
  dynamic_cast<BoxFeature*>(doc.GetFeature(0))->width = 2.0;
  REQUIRE(doc.Regenerate());
  CHECK(doc.LastRegenStats().executed == 3);
  CHECK(doc.LastRegenStats().cached == 0);
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(2.0, 1e-9));
  CHECK_THAT(doc.GetBodies()[2].shape.Volume(), WithinRel(4.0, 1e-9));

  // Rollback reuses the cache both ways.
  doc.SetRollback(1);
  CHECK(doc.LastRegenStats().executed == 0);
  CHECK(doc.GetBodies().size() == 1);
  doc.SetRollback(-1);
  CHECK(doc.LastRegenStats().executed == 0);
  CHECK(doc.GetBodies().size() == 3);
}

TEST_CASE("A material change rebuilds nothing", "[document][regen][cache]") {
  // A recolour is not a shape. Before the material was stripped from the
  // chain hash, setting one on the FIRST feature re-ran every feature after
  // it: 42.7 s on a manifold and 17.5 s on a cylinder head, per part, for a
  // change its own RPC handler describes as "PBR/albedo but not geometry".
  Document doc;
  doc.AddFeature(
      std::make_unique<BoxFeature>("A", glm::vec3(0), 1.0, 1.0, 1.0));
  doc.AddFeature(
      std::make_unique<BoxFeature>("B", glm::vec3(5, 0, 0), 2.0, 1.0, 1.0));
  doc.AddFeature(
      std::make_unique<BoxFeature>("C", glm::vec3(10, 0, 0), 3.0, 1.0, 1.0));
  REQUIRE(doc.Regenerate());

  // The FIRST feature: the worst case, since everything is downstream of it.
  doc.GetFeature(0)->SetMaterial("steel");
  REQUIRE(doc.Regenerate());
  CHECK(doc.LastRegenStats().executed == 0);
  CHECK(doc.LastRegenStats().cached == 3);
  // ...and it took effect anyway, which is the half a strip alone would lose:
  // the material reaches physics (BOM, FEM) through Body::material, so the
  // stamp pass has to run on a cache hit as well as on a rebuild.
  REQUIRE(doc.GetBodies().size() == 3);
  CHECK(doc.GetBodies()[0].material == "steel");
  CHECK(doc.GetBodies()[1].material.empty());

  // Clearing it is a cache hit too, and also lands.
  doc.GetFeature(0)->SetMaterial("");
  REQUIRE(doc.Regenerate());
  CHECK(doc.LastRegenStats().executed == 0);
  CHECK(doc.GetBodies()[0].material.empty());

  // Geometry is untouched throughout.
  CHECK_THAT(doc.GetBodies()[2].shape.Volume(), WithinRel(3.0, 1e-9));
}

TEST_CASE("Expression edits invalidate the cache through parameters",
          "[document][regen][cache]") {
  Document doc;
  doc.SetParameter("w", "2", "");
  auto sk = std::make_unique<SketchFeature>("S", SketchPlane(PlaneType::XY));
  sk->AddRectangle({0, 0}, {4, 2});
  doc.AddFeature(std::move(sk));
  const uint64_t skId = doc.GetFeature(0)->GetId();
  auto ext =
      std::make_unique<ExtrudeFeature>("Pad", skId, 1.0, ExtrudeMode::NewBody);
  ext->distanceExpr = "w";
  doc.AddFeature(std::move(ext));
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(16.0, 1e-6));

  // Changing the parameter changes the evaluated JSON -> suffix re-runs
  // (SetParameter regenerates internally; read the stats of THAT run).
  doc.SetParameter("w", "3", "");
  CHECK(doc.LastRegenStats().executed == 1); // sketch cached, extrude re-ran
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(24.0, 1e-6));

  // A cached failure stays an error without re-executing.
  doc.GetFeature(1)->SetSuppressed(true);
  doc.Regenerate();
  doc.GetFeature(1)->SetSuppressed(false);
  doc.Regenerate();
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(24.0, 1e-6));
}

TEST_CASE("Downstream failures are tagged after-earlier-failure",
          "[document][regen]") {
  Document doc;
  auto sk = std::make_unique<SketchFeature>("S", SketchPlane(PlaneType::XY));
  sk->AddRectangle({0, 0}, {2, 2});
  doc.AddFeature(std::move(sk));
  const uint64_t sid = doc.GetFeature(0)->GetId();
  // Root failure: an extrude referencing a nonexistent sketch (makes no
  // body). Collateral: a cut with nothing to cut into.
  doc.AddFeature(std::make_unique<ExtrudeFeature>("Root", 9999, 1.0,
                                                  ExtrudeMode::NewBody));
  doc.AddFeature(
      std::make_unique<ExtrudeFeature>("Cut", sid, 1.0, ExtrudeMode::Cut));

  CHECK_FALSE(doc.Regenerate());
  CHECK_FALSE(doc.GetFeature(1)->GetError().empty());
  CHECK_FALSE(doc.GetFeature(1)->ErrorAfterEarlierFailure()); // the root
  CHECK_FALSE(doc.GetFeature(2)->GetError().empty());
  CHECK(doc.GetFeature(2)->ErrorAfterEarlierFailure()); // collateral

  // Fixing the root (point it at the real sketch) clears both flags.
  auto* root = dynamic_cast<ExtrudeFeature*>(doc.GetFeature(1));
  REQUIRE(root != nullptr);
  root->sketchId = sid;
  CHECK(doc.Regenerate());
  CHECK(doc.GetFeature(1)->GetError().empty());
  CHECK(doc.GetFeature(2)->GetError().empty());
  CHECK_FALSE(doc.GetFeature(2)->ErrorAfterEarlierFailure());
}

// Help > About and any bug report quote the same block; if it stops naming
// the build, a report stops being reproducible. (The version and commit are
// injected by CMake, so this checks the shape, not the values.)
TEST_CASE("Build info names the version and the kernel", "[buildinfo]") {
  const std::string summary = BuildInfo::Summary("Test GPU", "1.3.0");
  INFO(summary);
  CHECK(summary.find(BuildInfo::Version()) != std::string::npos);
  CHECK(summary.find(BuildInfo::KernelVersion()) != std::string::npos);
  CHECK(summary.find("Test GPU") != std::string::npos);
  CHECK(summary.find("Vulkan 1.3.0") != std::string::npos);
  // Headless has no renderer, and must not claim one.
  CHECK(BuildInfo::Summary().find("GPU:") == std::string::npos);
  // OpenCASCADE is what decides what a model does; never "none" in a build
  // that has it.
  CHECK(BuildInfo::KernelVersion() != "none");
}

TEST_CASE("FirstConsumerIndex finds what a sketch feeds",
          "[document][rollback]") {
  Document doc;
  auto sk = std::make_unique<SketchFeature>("S", SketchPlane(PlaneType::XY));
  sk->AddRectangle({0, 0}, {4, 2});
  doc.AddFeature(std::move(sk));
  const uint64_t skId = doc.GetFeature(0)->GetId();
  doc.AddFeature(std::make_unique<BoxFeature>("Unrelated", glm::vec3(20, 0, 0),
                                              1.0, 1.0, 1.0));
  doc.AddFeature(
      std::make_unique<ExtrudeFeature>("Pad", skId, 3.0, ExtrudeMode::NewBody));
  REQUIRE(doc.GetBodies().size() == 2);

  // The extrude, not the box that merely sits between them in the list.
  CHECK(doc.FirstConsumerIndex(skId) == 2);
  // A feature nothing consumes, and an id that is not a feature at all.
  CHECK(doc.FirstConsumerIndex(doc.GetFeature(2)->GetId()) == -1);
  CHECK(doc.FirstConsumerIndex(doc.GetFeature(1)->GetId()) == -1);
  CHECK(doc.FirstConsumerIndex(0) == -1);
  CHECK(doc.FirstConsumerIndex(99999) == -1);

  // EARLIEST consumer: a second extrude on the same sketch does not move it.
  doc.AddFeature(std::make_unique<ExtrudeFeature>("Pad2", skId, 1.0,
                                                  ExtrudeMode::NewBody));
  CHECK(doc.FirstConsumerIndex(skId) == 2);
}

TEST_CASE("The view rollback hides a consumer without touching the document",
          "[document][rollback]") {
  Document doc;
  auto sk = std::make_unique<SketchFeature>("S", SketchPlane(PlaneType::XY));
  sk->AddRectangle({0, 0}, {4, 2});
  doc.AddFeature(std::move(sk));
  const uint64_t skId = doc.GetFeature(0)->GetId();
  doc.AddFeature(
      std::make_unique<ExtrudeFeature>("Pad", skId, 3.0, ExtrudeMode::NewBody));
  REQUIRE(doc.GetBodies().size() == 1);

  // What "edit this sketch" does: stop before the solid built from it.
  doc.SetViewRollback(doc.FirstConsumerIndex(skId));
  CHECK(doc.GetBodies().empty());
  CHECK(doc.ViewRollback() == 1);
  CHECK(doc.Rollback() == -1); // the user's own bar is untouched

  // It is a VIEW, not an edit: the saved document still has its solid, and
  // loading one never resurrects a view rollback.
  const auto path =
      std::filesystem::temp_directory_path() / "view_rollback.json";
  REQUIRE(testio::Save(doc, path.string()));
  auto loaded = testio::Load(path.string());
  REQUIRE(loaded != nullptr);
  CHECK(loaded->ViewRollback() == -1);
  CHECK(loaded->Rollback() == -1);
  CHECK(loaded->GetBodies().size() == 1);
  std::filesystem::remove(path);

  doc.SetViewRollback(-1);
  CHECK(doc.GetBodies().size() == 1);
}

TEST_CASE("The two rollback markers take the earlier of the two",
          "[document][rollback]") {
  Document doc;
  doc.AddFeature(
      std::make_unique<BoxFeature>("A", glm::vec3(0), 1.0, 1.0, 1.0));
  doc.AddFeature(
      std::make_unique<BoxFeature>("B", glm::vec3(5, 0, 0), 2.0, 1.0, 1.0));
  doc.AddFeature(
      std::make_unique<BoxFeature>("C", glm::vec3(10, 0, 0), 3.0, 1.0, 1.0));

  doc.SetRollback(2);     // the user parked the bar after B
  doc.SetViewRollback(1); // a sketch edit wants to stop after A
  CHECK(doc.EffectiveRollback() == 1);
  CHECK(doc.GetBodies().size() == 1);

  // Clearing the view marker falls back to the bar, not to the end.
  doc.SetViewRollback(-1);
  CHECK(doc.EffectiveRollback() == 2);
  CHECK(doc.GetBodies().size() == 2);

  // ...and a view marker LATER than the bar changes nothing.
  doc.SetViewRollback(3);
  CHECK(doc.EffectiveRollback() == 2);
  CHECK(doc.GetBodies().size() == 2);
}
