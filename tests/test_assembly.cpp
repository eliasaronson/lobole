#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/Occurrence.h"
#include "core/Serialization.h"

#include <glm/gtc/quaternion.hpp>

#include <filesystem>
#include <fstream>

using Catch::Matchers::WithinAbs;
namespace fs = std::filesystem;

namespace {

// A small assembly: three occurrences, two of them of the same part (id 2),
// with distinct transforms, visibility, and a fixed anchor.
AssemblyDocument SampleAssembly() {
  AssemblyDocument asmDoc;

  Occurrence base;
  base.name = "base";
  base.partId = 1;
  base.fixed = true; // grounded anchor
  asmDoc.AddOccurrence(base);

  Occurrence arm;
  arm.name = "arm";
  arm.partId = 2;
  arm.position = glm::vec3(10.0f, 0.0f, 0.0f);
  arm.orientation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 0, 1));
  asmDoc.AddOccurrence(arm);

  Occurrence arm2;
  arm2.name = "arm (2)";
  arm2.partId = 2; // same part → the instancing win
  arm2.position = glm::vec3(-5.0f, 3.0f, 1.0f);
  arm2.visible = false;
  arm2.partVersion = "abc123"; // git pin round-trips even before F5 uses it
  asmDoc.AddOccurrence(arm2);

  return asmDoc;
}

void ExpectSameAssembly(const AssemblyDocument& a, const AssemblyDocument& b) {
  REQUIRE(a.OccurrenceCount() == b.OccurrenceCount());
  const auto& oa = a.GetOccurrences();
  const auto& ob = b.GetOccurrences();
  for (size_t i = 0; i < oa.size(); i++) {
    CHECK(oa[i].id == ob[i].id); // order + ids preserved
    CHECK(oa[i].name == ob[i].name);
    CHECK(oa[i].partId == ob[i].partId);
    CHECK(oa[i].partVersion == ob[i].partVersion);
    CHECK(oa[i].visible == ob[i].visible);
    CHECK(oa[i].fixed == ob[i].fixed);
    for (int k = 0; k < 3; k++)
      CHECK_THAT(oa[i].position[k], WithinAbs(ob[i].position[k], 1e-6f));
    for (int k = 0; k < 4; k++)
      CHECK_THAT(oa[i].orientation[k], WithinAbs(ob[i].orientation[k], 1e-6f));
  }
}

} // namespace

TEST_CASE("AssemblyDocument assigns stable ids and finds/removes", "[assembly]") {
  AssemblyDocument asmDoc = SampleAssembly();
  REQUIRE(asmDoc.OccurrenceCount() == 3);
  // Ids grow from 1, unique.
  CHECK(asmDoc.GetOccurrences()[0].id == 1);
  CHECK(asmDoc.GetOccurrences()[1].id == 2);
  CHECK(asmDoc.GetOccurrences()[2].id == 3);

  REQUIRE(asmDoc.FindById(2) != nullptr);
  CHECK(asmDoc.FindById(2)->name == "arm");
  CHECK(asmDoc.FindById(999) == nullptr);

  REQUIRE(asmDoc.RemoveOccurrence(2));
  CHECK(asmDoc.OccurrenceCount() == 2);
  CHECK(asmDoc.FindById(2) == nullptr);
  CHECK_FALSE(asmDoc.RemoveOccurrence(2)); // already gone

  // A new add never reuses id 2 (F1 stable-id contract).
  Occurrence extra;
  extra.partId = 3;
  const uint64_t newId = asmDoc.AddOccurrence(extra);
  CHECK(newId == 4);
}

TEST_CASE("Assembly round-trips through the single-file form", "[assembly]") {
  AssemblyDocument original = SampleAssembly();
  const fs::path path =
      fs::temp_directory_path() / "designer_asm_roundtrip.json";
  std::string error;
  REQUIRE(DocumentIO::SaveAssembly(original, path.string(), &error));

  // Sanity: the file declares the assembly discriminator + shared version.
  {
    std::ifstream in(path);
    nlohmann::json j = nlohmann::json::parse(in);
    CHECK(j.at("format") == "designer-document");
    CHECK(j.at("kind") == "assembly");
    CHECK(j.at("occurrences").size() == 3);
  }

  auto reloaded = DocumentIO::LoadAssembly(path.string(), &error);
  REQUIRE(reloaded != nullptr);
  ExpectSameAssembly(original, *reloaded);
  fs::remove(path);
}

TEST_CASE("Assembly round-trips through the directory form", "[assembly]") {
  AssemblyDocument original = SampleAssembly();
  const fs::path dir = fs::temp_directory_path() / "designer_asm_roundtrip.dsgn";
  fs::remove_all(dir);
  std::string error;
  REQUIRE(DocumentIO::SaveAssembly(original, dir.string(), &error));

  // One file per occurrence, named by stable id; index lists the order.
  CHECK(fs::exists(dir / "index.json"));
  CHECK(fs::exists(dir / "occurrences" / "1-base.json"));
  CHECK(fs::exists(dir / "occurrences" / "2-arm.json"));

  auto reloaded = DocumentIO::LoadAssembly(dir.string(), &error);
  REQUIRE(reloaded != nullptr);
  ExpectSameAssembly(original, *reloaded);
  fs::remove_all(dir);
}

// A FROZEN golden (docs/versioning/format.md): hardcoded keys/values, never
// regenerated. Loading it pins the exact v23 on-disk schema, so a silent key
// rename (which a save→load round-trip would not catch) fails here.
TEST_CASE("Frozen v23 assembly golden loads at the exact schema", "[assembly][golden]") {
  // Assembly goldens live in a subdirectory so the part golden harness
  // (which scans tests/golden/ non-recursively for {features,bodies,volumes})
  // does not try to load them as parts.
  const fs::path golden = fs::path(DESIGNER_SOURCE_DIR) / "tests" / "golden" /
                          "assembly" / "v23-assembly.json";
  REQUIRE(fs::exists(golden));
  std::string error;
  auto asmDoc = DocumentIO::LoadAssembly(golden.string(), &error);
  REQUIRE(asmDoc != nullptr);
  REQUIRE(asmDoc->OccurrenceCount() == 3);

  const Occurrence* base = asmDoc->FindById(1);
  REQUIRE(base != nullptr);
  CHECK(base->partId == 1);
  CHECK(base->fixed);
  CHECK_FALSE(base->partVersion.has_value());

  const Occurrence* arm = asmDoc->FindById(2);
  REQUIRE(arm != nullptr);
  CHECK(arm->partId == 2);
  CHECK_THAT(arm->position.x, WithinAbs(10.0f, 1e-6f));
  CHECK_THAT(arm->orientation.z, WithinAbs(0.70710677f, 1e-6f)); // 90° about Z

  const Occurrence* arm2 = asmDoc->FindById(3);
  REQUIRE(arm2 != nullptr);
  CHECK(arm2->partId == 2); // same part as id 2 → the instancing win
  CHECK_FALSE(arm2->visible);
  REQUIRE(arm2->partVersion.has_value());
  CHECK(*arm2->partVersion == "abc123");
}

TEST_CASE("A part loader rejects an assembly and vice versa", "[assembly]") {
  AssemblyDocument asmDoc = SampleAssembly();
  const fs::path path = fs::temp_directory_path() / "designer_asm_kind.json";
  std::string error;
  REQUIRE(DocumentIO::SaveAssembly(asmDoc, path.string(), &error));

  // Loading an assembly as a part must fail cleanly, not throw or misparse.
  error.clear();
  auto asPart = DocumentIO::Load(path.string(), &error);
  CHECK(asPart == nullptr);
  CHECK_FALSE(error.empty());
  fs::remove(path);
}
