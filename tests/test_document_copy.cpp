#include <catch2/catch_test_macros.hpp>

#include "TestIO.h"
#include "core/Configuration.h"
#include "core/Document.h"
#include "core/Feature.h"
#include "core/Units.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/SketchFeature.h"
#include "core/fem/FemSolver.h"
#include "core/fem/FemStudy.h"
#include "core/io/Serialization.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>

namespace fs = std::filesystem;

// Document's copy constructor is a hand-written member list (it has to be —
// features are owning pointers and the instance id must NOT come along), and a
// hand-written list is a list that can fall behind the class. These tests are
// what says so: the first one saves an original and its copy and diffs the two
// files, so a persistent field the copy forgets shows up here rather than as a
// duplicated tab quietly missing its configurations.

namespace {

static const SketchPlane kXY(PlaneType::XY);

// A document carrying one of everything that persists: two features (one
// referencing the other by id), a parameter driving a dimension, a
// configuration, a study, an embedded asset, non-default units,
// a rollback and an active configuration.
std::unique_ptr<Document> BuildLoadedDocument() {
  auto doc = std::make_unique<Document>();
  doc->SetParameter("height", "12", "pad height");

  auto sk = std::make_unique<SketchFeature>("Sketch", kXY);
  sk->AddRectangle({0, 0}, {40, 25});
  doc->AddFeature(std::move(sk));
  const uint64_t sketchId = doc->GetFeature(0)->GetId();

  auto pad = std::make_unique<ExtrudeFeature>("Pad", sketchId, 12.0,
                                              ExtrudeMode::NewBody);
  pad->distanceExpr = "height"; // an expression-driven field, not a literal
  pad->SetMaterial("steel");
  doc->AddFeature(std::move(pad));

  Configuration cfg;
  cfg.id = doc->AllocateId();
  cfg.name = "Tall";
  cfg.parameterOverrides.push_back({"height", "30"});
  doc->RestoreConfiguration(cfg);

  fem::Study& study = doc->AddStudy("Bend");
  study.meshSize = 4.0;

  doc->SetAsset("underlay.png", {1, 2, 3, 4, 5});
  doc->SetUnits({units::Length::Inch, units::Mass::Gram, units::Angle::Radian});
  doc->SetActiveConfiguration(cfg.id);
  doc->SetRollback(1);
  doc->Regenerate();
  return doc;
}

std::string SaveToString(const Document& doc, const std::string& name) {
  const fs::path path = fs::temp_directory_path() / name;
  REQUIRE(testio::Save(doc, path.string()));
  std::ifstream in(path);
  std::ostringstream text;
  text << in.rdbuf();
  fs::remove(path);
  return text.str();
}

} // namespace

TEST_CASE("A copied document saves identically to its original",
          "[document][copy]") {
  std::unique_ptr<Document> original = BuildLoadedDocument();
  const Document copy(*original);

  // Every persistent field, compared through the one function that is FORCED
  // to know about all of them: a format bump has to teach Save the new field,
  // and this diff then fails until the copy learns it too.
  CHECK(SaveToString(copy, "copy-gate-copy.json") ==
        SaveToString(*original, "copy-gate-original.json"));
}

TEST_CASE("A copy is a new document, not the same one twice",
          "[document][copy]") {
  std::unique_ptr<Document> original = BuildLoadedDocument();
  original->SetRollback(-1); // roll to the tip so there is geometry to compare
  Document copy(*original);

  // Identity is the whole reason this is not `= default`: two documents that
  // share an instance id are one document to everything that remembers "I
  // already handled this one" — the autosave bug in Document::InstanceId()'s
  // comment.
  CHECK(copy.InstanceId() != original->InstanceId());

  // The features are clones, not aliases: same stable ids (they are per
  // document, and references inside the copy must keep resolving), different
  // objects.
  REQUIRE(copy.FeatureCount() == original->FeatureCount());
  for (size_t i = 0; i < copy.FeatureCount(); i++) {
    CHECK(copy.GetFeature(i) != original->GetFeature(i));
    CHECK(copy.GetFeature(i)->GetId() == original->GetFeature(i)->GetId());
    CHECK(copy.GetFeature(i)->TypeName() ==
          original->GetFeature(i)->TypeName());
  }
  // Same recipe, so the same geometry — the copy regenerated itself.
  REQUIRE(copy.GetBodies().size() == original->GetBodies().size());
  REQUIRE(copy.GetBodies().size() == 1);
  CHECK(copy.GetBodies()[0].shape.Volume() ==
        original->GetBodies()[0].shape.Volume());

  // Editing one leaves the other alone — through the parameter table the
  // pad's distance expression reads, and through the active configuration
  // that overrides it.
  copy.SetParameter("height", "40");
  copy.SetActiveConfiguration(0); // off "Tall", back onto the table
  copy.Regenerate();
  CHECK(copy.GetBodies()[0].shape.Volume() !=
        original->GetBodies()[0].shape.Volume());
  CHECK(original->FindParameter("height")->expression == "12");
  CHECK(original->ActiveConfiguration() != 0);
}

TEST_CASE("A copy carries study results, and a stale one stays stale",
          "[document][copy][fem]") {
  std::unique_ptr<Document> original = BuildLoadedDocument();
  const uint64_t studyId = original->GetStudies()[0].id;

  fem::StudyRuntime rt;
  rt.solvedAtVersion = original->Version(); // current as of this geometry
  original->StoreStudyResults(studyId, rt);
  REQUIRE_FALSE(original->StudyResultsStale(studyId));

  // A solve costs minutes and the copy's geometry IS the original's, so the
  // result comes along and reads current — re-stamped, because versions are
  // per document.
  const Document fresh(*original);
  REQUIRE(fresh.StudyResults(studyId) != nullptr);
  CHECK_FALSE(fresh.StudyResultsStale(studyId));

  // But a result that was already stale must not be laundered into a
  // fresh-looking one by being copied.
  original->SetParameter("height", "50");
  original->Regenerate();
  REQUIRE(original->StudyResultsStale(studyId));
  const Document stale(*original);
  REQUIRE(stale.StudyResults(studyId) != nullptr);
  CHECK(stale.StudyResultsStale(studyId));
}
