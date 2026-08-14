#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestIO.h"
#include "core/Document.h"
#include "core/Feature.h"
#include "core/features/BooleanFeature.h"
#include "core/features/ShellFeature.h"
#include "core/features/SketchFeature.h"
#include "core/io/GeometryCache.h"

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using Catch::Matchers::WithinRel;

namespace {

// A model with enough features that "did it execute?" is unambiguous.
std::unique_ptr<Document> MakePart() {
  auto doc = std::make_unique<Document>();
  doc->AddFeature(
      std::make_unique<BoxFeature>("Plate", glm::vec3(0), 40, 10, 30));
  doc->AddFeature(
      std::make_unique<BoxFeature>("Boss", glm::vec3(5, 10, 5), 8, 6, 8));
  doc->AddFeature(
      std::make_unique<BoxFeature>("Rail", glm::vec3(20, 10, 5), 6, 4, 20));
  return doc;
}

fs::path TempProject(const char* name) {
  const fs::path p = fs::temp_directory_path() / name;
  fs::remove_all(p);
  return p;
}

} // namespace

// Opening a project used to re-execute its whole feature list: 259 s for a
// 148-feature part against 0.4 s to save it (docs/performance/scale-round.md).
// The built bodies are stored beside the recipe, and a load that still matches
// executes nothing.
TEST_CASE("Baked geometry loads instead of rebuilding", "[baked]") {
  const fs::path proj = TempProject("baked-basic.dsgn");
  auto doc = MakePart();
  const double volume = doc->GetBodies()[0].shape.Volume();
  const size_t faces = doc->GetBodies()[0].elementMap.names.size();
  REQUIRE(testio::Save(*doc, proj.string()));
  REQUIRE(fs::exists(proj / "geometry" / "index.json"));

  auto loaded = testio::Load(proj.string());
  REQUIRE(loaded != nullptr);
  CHECK(loaded->LastRegenStats().executed == 0); // the whole point
  REQUIRE(loaded->GetBodies().size() == doc->GetBodies().size());
  CHECK_THAT(loaded->GetBodies()[0].shape.Volume(), WithinRel(volume, 1e-9));
  // The element map rides along: face and edge names are transient in memory,
  // and a dimension or mate that references one must still resolve.
  CHECK(loaded->GetBodies()[0].elementMap.names.size() == faces);
  CHECK(loaded->GetBodies()[0].elementMap.names ==
        doc->GetBodies()[0].elementMap.names);
  CHECK(loaded->GetBodies()[0].name == doc->GetBodies()[0].name);

  // Regenerating again is still free — the blob seeds the prefix cache, so an
  // unrelated document-version bump cannot silently rebuild the model.
  loaded->Regenerate();
  CHECK(loaded->LastRegenStats().executed == 0);

  // The first EDIT rebuilds from the start: only the tip is stored, so there
  // is no intermediate state to resume from. Documented, not accidental.
  dynamic_cast<BoxFeature*>(loaded->GetFeature(0))->width = 44.0;
  loaded->Regenerate();
  CHECK(loaded->LastRegenStats().executed == 3);
  fs::remove_all(proj);
}

// A cosmetic edit changes the chain hash but not the shape. Git would ignore
// the rewrite anyway (identical bytes are the same object), but rsync, backups
// and file watchers should still see only what actually changed — the same
// discipline WriteTextAtomic keeps for the recipe.
TEST_CASE("Renaming a feature does not touch its baked shape", "[baked]") {
  const fs::path proj = TempProject("baked-rename.dsgn");
  auto doc = MakePart();
  REQUIRE(testio::Save(*doc, proj.string()));
  const fs::path brep = proj / "geometry" / "0.brep";
  REQUIRE(fs::exists(brep));
  const auto before = fs::last_write_time(brep);

  doc->GetFeature(0)->SetName("Renamed plate");
  doc->Regenerate();
  REQUIRE(testio::Save(*doc, proj.string()));
  CHECK(fs::last_write_time(brep) == before); // same shape, same file
  // The index DOES change: it carries the new chain hash, which is what makes
  // the blob still match the recipe on the next open.
  auto loaded = testio::Load(proj.string());
  REQUIRE(loaded != nullptr);
  CHECK(loaded->LastRegenStats().executed == 0);
  fs::remove_all(proj);
}

// Warnings are a product of EXECUTION, and a baked open executes nothing —
// without carrying them the model would silently lose its advice on reopen,
// which is the same silence the warnings exist to break.
TEST_CASE("Advice survives a baked open", "[baked]") {
  const fs::path proj = TempProject("baked-warning.dsgn");
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>("A", glm::vec3(0), 2, 2, 2));
  doc.AddFeature(
      std::make_unique<BoxFeature>("B", glm::vec3(10, 0, 0), 2, 2, 2));
  // Subtracting a body that does not touch the target: valid, and warned.
  doc.AddFeature(std::make_unique<BooleanFeature>(
      "Cut", doc.GetFeature(0)->GetId(), doc.GetFeature(1)->GetId(),
      BooleanOp::Subtract));
  doc.Regenerate();
  const std::string warning = doc.GetFeature(2)->GetWarning();
  REQUIRE(!warning.empty());
  REQUIRE(testio::Save(doc, proj.string()));

  auto loaded = testio::Load(proj.string());
  REQUIRE(loaded != nullptr);
  CHECK(loaded->LastRegenStats().executed == 0); // it really was baked
  CHECK(loaded->GetFeature(2)->GetWarning() == warning);
  fs::remove_all(proj);
}

TEST_CASE("A recipe edited behind the app's back is rebuilt", "[baked]") {
  const fs::path proj = TempProject("baked-stale.dsgn");
  auto doc = MakePart();
  REQUIRE(testio::Save(*doc, proj.string()));

  // Edit a feature file directly — the case that makes a naive cache lie.
  fs::path featureFile;
  for (const auto& e : fs::directory_iterator(proj / "features"))
    if (e.path().filename().string().starts_with("1-"))
      featureFile = e.path();
  REQUIRE(!featureFile.empty());
  nlohmann::json fj;
  {
    std::ifstream in(featureFile);
    fj = nlohmann::json::parse(in);
  }
  fj["width"] = 60.0;
  {
    std::ofstream out(featureFile);
    out << fj.dump(2);
  }

  auto loaded = testio::Load(proj.string());
  REQUIRE(loaded != nullptr);
  CHECK(loaded->LastRegenStats().executed == 3); // hash differs → rebuild
  CHECK_THAT(loaded->GetBodies()[0].shape.Volume(),
             WithinRel(60.0 * 10.0 * 30.0, 1e-6)); // the widened plate
  fs::remove_all(proj);
}

// A blob can only ever make an open faster, never wrong: every way it can be
// broken degrades to the rebuild the app did before it existed.
TEST_CASE("A broken blob is ignored, not fatal", "[baked]") {
  const fs::path proj = TempProject("baked-broken.dsgn");
  auto doc = MakePart();
  const double volume = doc->GetBodies()[0].shape.Volume();
  REQUIRE(testio::Save(*doc, proj.string()));

  SECTION("truncated B-Rep") {
    std::ofstream(proj / "geometry" / "0.brep",
                  std::ios::binary | std::ios::trunc)
        << "not a shape";
  }
  SECTION("unknown blob format") {
    const fs::path index = proj / "geometry" / "index.json";
    nlohmann::json j;
    {
      std::ifstream in(index);
      j = nlohmann::json::parse(in);
    }
    j["geometry_format"] = 99;
    std::ofstream(index, std::ios::trunc) << j.dump(2);
  }
  SECTION("hash from another model") {
    const fs::path index = proj / "geometry" / "index.json";
    nlohmann::json j;
    {
      std::ifstream in(index);
      j = nlohmann::json::parse(in);
    }
    j["chain_hash"] = 12345;
    std::ofstream(index, std::ios::trunc) << j.dump(2);
  }
  SECTION("no blob at all") { fs::remove_all(proj / "geometry"); }

  auto loaded = testio::Load(proj.string());
  REQUIRE(loaded != nullptr);
  CHECK(loaded->LastRegenStats().executed == 3); // rebuilt, no complaint
  CHECK_THAT(loaded->GetBodies()[0].shape.Volume(), WithinRel(volume, 1e-9));
  fs::remove_all(proj);
}

TEST_CASE("A failed model is not baked", "[baked]") {
  const fs::path proj = TempProject("baked-broken-model.dsgn");
  auto doc = MakePart();
  // A fillet with no resolvable body: the document regenerates with an error,
  // and a blob would then promise geometry the recipe does not produce.
  auto bad = std::make_unique<ShellFeature>("Bad", 999,
                                            std::vector<ElementRef>{}, 1.0);
  doc->AddFeature(std::move(bad));
  REQUIRE_FALSE(doc->Regenerate());
  REQUIRE(testio::Save(*doc, proj.string()));
  CHECK_FALSE(fs::exists(proj / "geometry" / "index.json"));

  auto loaded = testio::Load(proj.string());
  REQUIRE(loaded != nullptr);
  CHECK(loaded->LastRegenStats().executed > 0);
  fs::remove_all(proj);
}

TEST_CASE("commit_geometry off removes the blob and persists", "[baked]") {
  const fs::path proj = TempProject("baked-optout.dsgn");
  auto doc = MakePart();
  REQUIRE(testio::Save(*doc, proj.string()));
  REQUIRE(fs::exists(proj / "geometry" / "index.json"));

  doc->SetCommitGeometry(false);
  REQUIRE(testio::Save(*doc, proj.string()));
  CHECK_FALSE(fs::exists(proj / "geometry"));

  auto loaded = testio::Load(proj.string());
  REQUIRE(loaded != nullptr);
  CHECK_FALSE(loaded->CommitGeometry()); // the choice survives a round trip
  CHECK(loaded->LastRegenStats().executed == 3);
  fs::remove_all(proj);
}
