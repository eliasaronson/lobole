// Results keyed by inputs (docs/simulation/README.md#studies-as-jobs): an
// FEM run is answered from a stored result only when its input hash matches,
// so the hash must move on every input that can change an answer -- each
// class on its own here -- and a topology result saved with the project must
// hash the same after a reopen, or it is never reused and a reopened project
// pays for the run again.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "TestIO.h"
#include "core/Document.h"
#include "core/Feature.h"
#include "core/fem/FemSolver.h"
#include "core/fem/FemStudy.h"
#include "core/io/Serialization.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <memory>
#include <string>

namespace {

struct Bar {
  Document doc;
  uint64_t studyId = 0;
  uint64_t forceId = 0;
  ElementRef Face(int i) const {
    const uint64_t cb = doc.GetBodies()[0].createdBy;
    return ElementRef{cb, std::to_string(cb) + "/face" + std::to_string(i)};
  }
};

std::unique_ptr<Bar> MakeBar(fem::AnalysisType type) {
  auto bar = std::make_unique<Bar>();
  auto box =
      std::make_unique<BoxFeature>("Bar", glm::vec3(0, 0, 0), 80.0, 10.0, 10.0);
  box->SetMaterial("steel");
  bar->doc.AddFeature(std::move(box));
  fem::Study& s = bar->doc.AddStudy("Bend");
  s.type = type;
  s.meshSize = 4.0;
  fem::Load fixed;
  fixed.id = bar->doc.AllocateId();
  fixed.type = fem::LoadType::Fixed;
  fixed.face = bar->Face(0);
  s.loads.push_back(fixed);
  fem::Load force;
  force.id = bar->doc.AllocateId();
  force.type = fem::LoadType::Force;
  force.face = bar->Face(1);
  force.magnitude = 500.0;
  force.direction = glm::dvec3(0, 0, -1);
  s.loads.push_back(force);
  bar->studyId = s.id;
  bar->forceId = force.id;
  return bar;
}

fem::Study& StudyOf(Bar& bar) { return *bar.doc.StudyForRun(bar.studyId); }

uint64_t Key(Bar& bar) {
  return fem::StudyInputKey(bar.doc, *bar.doc.FindStudy(bar.studyId));
}

// A topology runtime by hand (no mesher): what a run would store.
fem::StudyRuntime HandMadeTopology(uint64_t inputHash) {
  fem::StudyRuntime rt;
  rt.topology = true;
  rt.iterations = 3;
  rt.converged = true;
  rt.volumeFractionAchieved = 0.4;
  rt.complianceHistory = {3.0, 2.0, 1.5};
  fem::StudyBodyResult b;
  b.bodyIndex = 0;
  b.bodyName = "Bar";
  for (int i = 0; i < 10; ++i)
    b.mesh.nodes.push_back(glm::dvec3(i, 0.5 * i, 0.25 * i));
  b.mesh.tets.push_back({0, 1, 2, 3, 4, 5, 6, 7, 8, 9});
  b.density = {0.7};
  rt.bodies.push_back(std::move(b));
  rt.inputHash = inputHash;
  return rt;
}

} // namespace

TEST_CASE("An FEM run's input hash moves with every input",
          "[fem][studyjobs]") {
  auto bar = MakeBar(fem::AnalysisType::Static);
  const uint64_t base = Key(*bar);
  REQUIRE(base != 0);
  CHECK(Key(*bar) == base);

  // The name is a label.
  StudyOf(*bar).name = "renamed";
  CHECK(Key(*bar) == base);

  // Settings.
  StudyOf(*bar).meshSize = 3.0;
  CHECK(Key(*bar) != base);
  StudyOf(*bar).meshSize = 4.0;
  REQUIRE(Key(*bar) == base);
  StudyOf(*bar).type = fem::AnalysisType::Modal;
  CHECK(Key(*bar) != base);
  StudyOf(*bar).type = fem::AnalysisType::Static;
  REQUIRE(Key(*bar) == base);

  // Loads: a magnitude, a direction, a face.
  StudyOf(*bar).loads[1].magnitude = 600.0;
  CHECK(Key(*bar) != base);
  StudyOf(*bar).loads[1].magnitude = 500.0;
  StudyOf(*bar).loads[1].direction = glm::dvec3(0, 1, 0);
  CHECK(Key(*bar) != base);
  StudyOf(*bar).loads[1].direction = glm::dvec3(0, 0, -1);
  StudyOf(*bar).loads[1].face = bar->Face(2);
  CHECK(Key(*bar) != base);
  StudyOf(*bar).loads[1].face = bar->Face(1);
  REQUIRE(Key(*bar) == base);

  // An expression is an input by its VALUE: the same text, another value of
  // the variable it reads, is another run.
  bar->doc.SetParameter("load", "500");
  StudyOf(*bar).loads[1].magnitudeExpr = "load";
  const uint64_t withExpr = Key(*bar);
  bar->doc.SetParameter("load", "700");
  CHECK(Key(*bar) != withExpr);
  bar->doc.SetParameter("load", "500");
  CHECK(Key(*bar) == withExpr);
  StudyOf(*bar).loads[1].magnitudeExpr.clear();
  REQUIRE(Key(*bar) == base);

  // The material.
  bar->doc.GetFeature(0)->SetMaterial("aluminum");
  bar->doc.Regenerate();
  CHECK(Key(*bar) != base);
  bar->doc.GetFeature(0)->SetMaterial("steel");
  bar->doc.Regenerate();
  REQUIRE(Key(*bar) == base);

  // The geometry.
  auto longer = MakeBar(fem::AnalysisType::Static);
  auto* box = dynamic_cast<BoxFeature*>(longer->doc.GetFeature(0));
  REQUIRE(box != nullptr);
  box->width = 81.0;
  longer->doc.Regenerate();
  CHECK(Key(*longer) != base);
}

TEST_CASE("A saved topology result is reused after a reopen, and is stale "
          "when its inputs moved",
          "[topopt][serialize][studyjobs]") {
  namespace fs = std::filesystem;
  auto bar = MakeBar(fem::AnalysisType::Topology);
  const uint64_t hash = Key(*bar);
  REQUIRE(hash != 0);
  bar->doc.StoreStudyResults(bar->studyId, HandMadeTopology(hash));

  const fs::path proj = fs::temp_directory_path() / "study_inputs_reopen.dsgn";
  fs::remove_all(proj);
  REQUIRE(testio::Save(bar->doc, proj.string()));
  auto reloaded = testio::Load(proj.string());
  REQUIRE(reloaded != nullptr);
  const fem::StudyRuntime* got = reloaded->StudyResults(bar->studyId);
  REQUIRE(got != nullptr);
  // The hash came back, and the reopened part hashes to it: the geometry
  // read back from disk is the geometry that was saved, byte for byte.
  CHECK(got->inputHash == hash);
  CHECK(fem::StudyInputKey(*reloaded, *reloaded->FindStudy(bar->studyId)) ==
        hash);
  CHECK_FALSE(reloaded->StudyResultsStale(bar->studyId));

  // A result saved against other inputs reads stale on reopen, rather than
  // current because it was loaded.
  bar->doc.StoreStudyResults(bar->studyId, HandMadeTopology(hash ^ 1));
  REQUIRE(testio::Save(bar->doc, proj.string()));
  auto other = testio::Load(proj.string());
  REQUIRE(other != nullptr);
  REQUIRE(other->StudyResults(bar->studyId) != nullptr);
  CHECK(other->StudyResultsStale(bar->studyId));
  fs::remove_all(proj);
}

TEST_CASE("A topology result's input hash round-trips, and a malformed one is "
          "refused",
          "[topopt][serialize][studyjobs]") {
  nlohmann::json j;
  REQUIRE(DocumentIO::StudyResultToJson(HandMadeTopology(0x0123456789abcdefULL),
                                        j));
  CHECK(j.at("input_hash") == "0x0123456789abcdef");
  CHECK(DocumentIO::StudyResultFromJson(j).inputHash == 0x0123456789abcdefULL);
  j["input_hash"] = "123";
  CHECK_THROWS_WITH(DocumentIO::StudyResultFromJson(j),
                    "topology result: input_hash \"123\" is not 0x and 16 "
                    "hex digits");
}
