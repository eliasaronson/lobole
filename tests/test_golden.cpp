#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/Document.h"
#include "core/Serialization.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
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
      auto doc = DocumentIO::Load(golden.string(), &error);
      REQUIRE((doc != nullptr && error.empty()));
      AssertMatchesExpect(*doc, expect);

      // Round-trip through the CURRENT writer: migrations must compose
      const fs::path tmp = fs::temp_directory_path() /
                           ("golden_rt_" + golden.filename().string());
      REQUIRE(DocumentIO::Save(*doc, tmp.string(), &error));
      auto reloaded = DocumentIO::Load(tmp.string(), &error);
      REQUIRE((reloaded != nullptr && error.empty()));
      AssertMatchesExpect(*reloaded, expect);
      fs::remove(tmp);
    }
  }
}
