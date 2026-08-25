#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestIO.h"
#include "core/Document.h"
#include "core/Feature.h"
#include "core/Units.h"
#include "core/cam/CamJob.h"
#include "core/io/Serialization.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <vector>

using Catch::Matchers::WithinRel;
namespace fs = std::filesystem;

// The golden corpus (docs/versioning/format.md): one frozen file per format
// era, each derived from the loader migration it exercises. A failure here
// means a BROKEN MIGRATION — fix the loader, do not update the golden file
// (new goldens are only added when kVersion bumps).

namespace {

// The side a clearing operation clears on, per operation, in file order.
// Optional: only the goldens that carry CAM say anything about it.
//
// Worth checking because the corpus could not previously catch a CAM
// regression at all, and one hid here: `side` was written for profiles only,
// so a saved pocket reloaded as "clear OUTSIDE its boundary" — where there is
// nothing — and produced no toolpath. v60-cam-arcs.json is a real pre-fix
// file whose pocket and adaptive ops carry no `side` key, which makes it the
// document the migration has to repair (docs/cam/dogfood-round.md).
void AssertCamSides(const Document& doc, const nlohmann::json& expect) {
  if (!expect.contains("cam_op_sides"))
    return;
  const auto want = expect.at("cam_op_sides").get<std::vector<std::string>>();
  std::vector<std::string> got;
  for (const cam::Setup& setup : doc.GetCamSetups())
    for (const cam::Operation& op : setup.operations)
      got.push_back(op.side == cam::Side::Inside ? "inside"
                    : op.side == cam::Side::On   ? "on"
                                                 : "outside");
  REQUIRE(got.size() == want.size());
  for (size_t i = 0; i < got.size(); i++) {
    INFO("operation " << i);
    CHECK(got[i] == want[i]);
  }
}

// Which face Z zero was picked from. On the SETUP, so a job reopened from its
// own file still knows — the panel used to ask a session field and answered
// "Z zero is not set yet" about an origin it had saved and was using.
void AssertCamTopFace(const Document& doc, const nlohmann::json& expect) {
  if (!expect.contains("cam_top_face"))
    return;
  REQUIRE(!doc.GetCamSetups().empty());
  CHECK(doc.GetCamSetups()[0].topFace.name ==
        expect.at("cam_top_face").get<std::string>());
}

// The document's display units (v64). Optional, like the CAM assertions:
// only the goldens that say anything about units are checked. Worth a golden
// of its own because the failure mode is silent — a file that says "in" and
// loads as mm is wrong in every dimension it shows and looks entirely
// reasonable, which is why the loader REFUSES an unknown string rather than
// substituting one.
void AssertUnits(const Document& doc, const nlohmann::json& expect) {
  if (!expect.contains("units"))
    return;
  const nlohmann::json& want = expect.at("units");
  const units::System& got = doc.Units();
  CHECK(std::string(units::ToString(got.length)) ==
        want.at("length").get<std::string>());
  CHECK(std::string(units::ToString(got.mass)) ==
        want.at("mass").get<std::string>());
  CHECK(std::string(units::ToString(got.angle)) ==
        want.at("angle").get<std::string>());
}

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

// The corpus reaches the CURRENT version — not "every bump has a golden",
// which is what the policy prose says and what the repo has never done.
//
// Measured 2026-08-25: 11 of the 35 documented bumps have no golden, almost
// all of them v2-v16, from before the practice settled. A strict per-bump
// gate would fail on arrival and be deleted, which is how a gate teaches
// people to ignore gates. The two modern gaps (v27 exploded views, v53
// drawing dim kinds) were plain misses, not bumps that could not be frozen.
//
// So gate the thing that actually protects the next bump: the newest golden
// must BE the current version. It passes today with no backfill, and fails
// the moment someone bumps kVersion without freezing an artifact at it —
// which is the drift worth catching.
//
// Both corpora count. The single-document goldens live in tests/golden/ and
// the assembly-schema ones in tests/golden/assembly/, and forgetting the
// second is not hypothetical: the audit that produced this test first
// reported v65's golden MISSING because it looked only in the first.
TEST_CASE("The golden corpus reaches the current kVersion", "[golden]") {
  const fs::path root = fs::path(DESIGNER_SOURCE_DIR) / "tests" / "golden";
  int newest = 0;
  std::string newestFile;
  for (const fs::path& dir : {root, root / "assembly"}) {
    if (!fs::exists(dir))
      continue;
    for (const auto& entry : fs::directory_iterator(dir)) {
      const fs::path& p = entry.path();
      if (p.extension() != ".json" || p.stem().string().ends_with(".expect"))
        continue;
      std::ifstream in(p);
      if (!in)
        continue;
      nlohmann::json j;
      try {
        in >> j;
      } catch (...) {
        continue;
      }
      const int v = j.value("version", 0);
      if (v > newest) {
        newest = v;
        newestFile = p.filename().string();
      }
    }
  }
  INFO("newest golden is v" << newest << " (" << newestFile
                            << "), kVersion is v"
                            << DocumentIO::DocumentFormatVersion()
                            << ". A kVersion bump ships a golden AT that "
                               "version — tests/golden/ for a document, "
                               "tests/golden/assembly/ for assembly schema.");
  CHECK(newest == DocumentIO::DocumentFormatVersion());
}

TEST_CASE("The version history covers every kVersion", "[golden]") {
  // docs/versioning/format.md's migration policy says a bump ships a loader
  // migration, a golden and a round-trip test. The migration and round-trip
  // are enforced by the corpus test below, and the golden AT the current
  // version by the test above (added 2026-08-25 — this comment used to claim
  // all three were covered here, and the golden's existence was not). The
  // version-history table was the other unchecked item, and it quietly
  // stopped at v60 while the code reached v65: five bumps of rot, each of
  // which had shipped its golden correctly. A policy nobody checks is a
  // wish.
  const fs::path doc =
      fs::path(DESIGNER_SOURCE_DIR) / "docs" / "versioning" / "format.md";
  REQUIRE(fs::exists(doc));
  std::ifstream in(doc);
  REQUIRE(in);
  std::string line;
  int highest = 0;
  while (std::getline(in, line)) {
    // Table rows read "| <n> | <commit> | <what changed> |".
    if (line.size() < 3 || line[0] != '|')
      continue;
    const size_t bar = line.find('|', 1);
    if (bar == std::string::npos)
      continue;
    const std::string cell = line.substr(1, bar - 1);
    try {
      const int v = std::stoi(cell);
      highest = std::max(highest, v);
    } catch (...) {
      continue; // the header row and anything else
    }
  }
  INFO("format.md's version history reaches v"
       << highest << ", kVersion is v" << DocumentIO::DocumentFormatVersion());
  CHECK(highest == DocumentIO::DocumentFormatVersion());
}

TEST_CASE("Golden corpus loads, regenerates, and round-trips", "[golden]") {
  const fs::path dir = fs::path(DESIGNER_SOURCE_DIR) / "tests" / "golden";
  REQUIRE(fs::exists(dir));

  std::vector<fs::path> goldens;
  for (const auto& entry : fs::directory_iterator(dir)) {
    const fs::path& p = entry.path();
    if (p.extension() == ".json" && !p.stem().string().ends_with(".expect"))
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
      AssertCamSides(*doc, expect);
      AssertCamTopFace(*doc, expect);
      AssertUnits(*doc, expect);

      // Round-trip through the CURRENT writer: migrations must compose
      const fs::path tmp = fs::temp_directory_path() /
                           ("golden_rt_" + golden.filename().string());
      REQUIRE(testio::Save(*doc, tmp.string()));
      auto reloaded = testio::Load(tmp.string());
      REQUIRE((reloaded != nullptr && error.empty()));
      AssertMatchesExpect(*reloaded, expect);
      AssertCamSides(*reloaded, expect);
      AssertCamTopFace(*reloaded, expect);
      AssertUnits(*reloaded, expect); // survives a save/load round trip
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
      AssertCamSides(*reloaded, expect);
      AssertCamTopFace(*reloaded, expect);
      AssertUnits(*reloaded, expect); // survives a save/load round trip
      fs::remove_all(proj);
    }
  }
}

TEST_CASE("Directory format: editing one feature touches one file",
          "[dirformat]") {
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>("A", glm::vec3(0), 1, 1, 1));
  doc.AddFeature(
      std::make_unique<BoxFeature>("B", glm::vec3(5, 0, 0), 2, 1, 1));
  const fs::path proj = fs::temp_directory_path() / "onefile.dsgn";
  fs::remove_all(proj);
  std::string error;
  REQUIRE(testio::Save(doc, proj.string()));

  // The recipe and the baked geometry are counted apart: the property F3
  // exists for is that an edit rewrites only the files it must, and the
  // geometry blob legitimately changes whenever the geometry does.
  auto mtimes = [&](bool geometry) {
    std::map<std::string, fs::file_time_type> m;
    for (const auto& e : fs::recursive_directory_iterator(proj))
      if (e.is_regular_file() &&
          (e.path().parent_path().filename() == "geometry") == geometry)
        m[e.path().string()] = fs::last_write_time(e.path());
    return m;
  };
  const auto countChanged = [](const auto& before, const auto& after) {
    int changed = 0;
    for (const auto& [path, t] : after)
      if (!before.contains(path) || before.at(path) != t)
        changed++;
    return changed;
  };

  // A save that changes nothing must touch nothing at all — including the
  // megabytes of baked geometry, which would otherwise churn git history on
  // every Ctrl+S (docs/versioning/baked-geometry.md).
  const auto idleRecipe = mtimes(false);
  const auto idleGeometry = mtimes(true);
  REQUIRE(testio::Save(doc, proj.string()));
  CHECK(countChanged(idleRecipe, mtimes(false)) <= 1); // the index is rewritten
  CHECK(countChanged(idleGeometry, mtimes(true)) == 0);

  const auto before = mtimes(false);
  const auto beforeGeometry = mtimes(true);
  dynamic_cast<BoxFeature*>(doc.GetFeature(1))->width = 3.0;
  doc.Regenerate();
  REQUIRE(testio::Save(doc, proj.string()));
  const auto after = mtimes(false);
  REQUIRE(before.size() == after.size());
  // The edited feature's file and the (always-rewritten) index. This is the
  // git-friendliness property that justifies F3.
  CHECK(countChanged(before, after) <= 2);
  // …and the geometry it produced is rebaked, because it really did change.
  CHECK(countChanged(beforeGeometry, mtimes(true)) >= 1);

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
