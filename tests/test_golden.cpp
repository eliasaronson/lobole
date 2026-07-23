#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/Document.h"
#include "core/Feature.h"
#include "core/io/Serialization.h"
#include "TestIO.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <map>
#include <fstream>
#include <vector>

using Catch::Matchers::WithinRel;
namespace fs = std::filesystem;

// The golden corpus (docs/versioning/format.md): one frozen file per format
// era, each derived from the loader migration it exercises. A failure here
// means a BROKEN MIGRATION — fix the loader, do not update the golden file
// (new goldens are only added when kVersion bumps).

namespace {

void AssertMatchesExpect(const Document& doc, const nlohmann::json& expect) {
  CHECK(doc.FeatureCount() == expect.at("features").get<size_t>());
  REQUIRE(doc.GetBodies().size() == expect.at("bodies").get<size_t>());
  std::vector<double> volumes;
  for (const auto& body : doc.GetBodies())
    volumes.push_back(body.shape.Volume());
  std::sort(volumes.begin(), volumes.end());
  auto expected = expect.at("volumes").get<std::vector<double>>();
  std::sort(expected.begin(), expected.end());
  REQUIRE(volumes.size() == expected.size());
  for (size_t i = 0; i < volumes.size(); i++)
    CHECK_THAT(volumes[i], WithinRel(expected[i], 1e-3));
}

} // namespace

TEST_CASE("Golden corpus loads, regenerates, and round-trips", "[golden]") {
  const fs::path dir = fs::path(DESIGNER_SOURCE_DIR) / "tests" / "golden";
  REQUIRE(fs::exists(dir));

  std::vector<fs::path> goldens;
  for (const auto& entry : fs::directory_iterator(dir)) {
    const fs::path& p = entry.path();
    if (p.extension() == ".json" &&
        !p.stem().string().ends_with(".expect"))
      goldens.push_back(p);
  }
  std::sort(goldens.begin(), goldens.end());
  CHECK(goldens.size() >= 8); // one per documented era — see format.md

  for (const fs::path& golden : goldens) {
    DYNAMIC_SECTION(golden.filename().string()) {
      fs::path expectPath = golden;
      expectPath.replace_extension(".expect.json");
      REQUIRE(fs::exists(expectPath));
      nlohmann::json expect;
      std::ifstream(expectPath) >> expect;

      // Load + regenerate (AddFeature regenerates during Load)
      std::string error;
      auto doc = testio::Load(golden.string());
      REQUIRE((doc != nullptr && error.empty()));
      AssertMatchesExpect(*doc, expect);

      // Round-trip through the CURRENT writer: migrations must compose
      const fs::path tmp = fs::temp_directory_path() /
                           ("golden_rt_" + golden.filename().string());
      REQUIRE(testio::Save(*doc, tmp.string()));
      auto reloaded = testio::Load(tmp.string());
      REQUIRE((reloaded != nullptr && error.empty()));
      AssertMatchesExpect(*reloaded, expect);
      fs::remove(tmp);
    }
  }
}

TEST_CASE("Golden corpus round-trips through the directory format (F3)",
          "[golden][dirformat]") {
  const fs::path dir = fs::path(DESIGNER_SOURCE_DIR) / "tests" / "golden";
  std::vector<fs::path> goldens;
  for (const auto& entry : fs::directory_iterator(dir)) {
    const fs::path& p = entry.path();
    if (p.extension() == ".json" && !p.stem().string().ends_with(".expect"))
      goldens.push_back(p);
  }
  std::sort(goldens.begin(), goldens.end());

  for (const fs::path& golden : goldens) {
    DYNAMIC_SECTION(golden.filename().string()) {
      fs::path expectPath = golden;
      expectPath.replace_extension(".expect.json");
      nlohmann::json expect;
      std::ifstream(expectPath) >> expect;

      std::string error;
      auto doc = testio::Load(golden.string());
      REQUIRE(doc != nullptr);

      const fs::path proj = fs::temp_directory_path() /
                            ("golden_dir_" + golden.stem().string() + ".dsgn");
      fs::remove_all(proj);
      REQUIRE(testio::Save(*doc, proj.string()));
      CHECK(fs::exists(proj / "index.json"));
      auto reloaded = testio::Load(proj.string());
      REQUIRE((reloaded != nullptr && error.empty()));
      AssertMatchesExpect(*reloaded, expect);
      fs::remove_all(proj);
    }
  }
}

TEST_CASE("Directory format: editing one feature touches one file",
          "[dirformat]") {
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>("A", glm::vec3(0), 1, 1, 1));
  doc.AddFeature(std::make_unique<BoxFeature>("B", glm::vec3(5, 0, 0), 2, 1,
                                              1));
  const fs::path proj = fs::temp_directory_path() / "onefile.dsgn";
  fs::remove_all(proj);
  std::string error;
  REQUIRE(testio::Save(doc, proj.string()));

  auto mtimes = [&] {
    std::map<std::string, fs::file_time_type> m;
    for (const auto& e : fs::recursive_directory_iterator(proj))
      if (e.is_regular_file())
        m[e.path().string()] = fs::last_write_time(e.path());
    return m;
  };
  const auto before = mtimes();
  dynamic_cast<BoxFeature*>(doc.GetFeature(1))->width = 3.0;
  doc.Regenerate();
  REQUIRE(testio::Save(doc, proj.string()));
  const auto after = mtimes();
  REQUIRE(before.size() == after.size());
  int changed = 0;
  for (const auto& [path, t] : after)
    if (before.at(path) != t)
      changed++;
  // The edited feature's file and the (always-rewritten) index. This is the
  // git-friendliness property that justifies F3.
  CHECK(changed <= 2);

  // Deleting a feature removes its file.
  doc.RemoveFeature(0);
  REQUIRE(testio::Save(doc, proj.string()));
  int featureFiles = 0;
  for (const auto& e : fs::directory_iterator(proj / "features"))
    featureFiles++;
  CHECK(featureFiles == 1);
  fs::remove_all(proj);
}

TEST_CASE("Opening the index.json inside a .dsgn opens the project",
          "[dirformat]") {
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>("A", glm::vec3(0), 2, 2, 2));
  const fs::path proj = fs::temp_directory_path() / "viaindex.dsgn";
  fs::remove_all(proj);
  std::string error;
  REQUIRE(testio::Save(doc, proj.string()));
  auto loaded = testio::Load((proj / "index.json").string());
  REQUIRE(loaded != nullptr);
  CHECK(loaded->FeatureCount() == 1);
  fs::remove_all(proj);
}
