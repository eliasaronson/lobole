#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestIO.h"
#include "core/Document.h"
#include "core/Feature.h"
#include "core/Units.h"
#include "core/cam/CamJob.h"
#include "core/cam/CamToolpath.h"
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
void AssertCamSides(const std::vector<cam::Setup>& setups,
                    const nlohmann::json& expect) {
  if (!expect.contains("cam_op_sides"))
    return;
  const auto want = expect.at("cam_op_sides").get<std::vector<std::string>>();
  std::vector<std::string> got;
  for (const cam::Setup& setup : setups)
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

// The SHAPE of each tool, and the type of each operation (v66). A V-bit that
// reloads as a flat mill is a chamfer that reloads as a square step, and an
// edge break that reloads as a profile is a full-depth cut round the part —
// both are the silent substitution this project refuses, so both are gated.
void AssertCamShapes(const std::vector<cam::Setup>& setups,
                     const nlohmann::json& expect) {
  if (expect.contains("cam_tool_types")) {
    const auto want =
        expect.at("cam_tool_types").get<std::vector<std::string>>();
    std::vector<std::string> got;
    for (const cam::Setup& setup : setups)
      for (const cam::Tool& t : setup.tools)
        got.push_back(t.type == cam::ToolType::Ball    ? "ball"
                      : t.type == cam::ToolType::VBit  ? "vbit"
                      : t.type == cam::ToolType::Drill ? "drill"
                                                       : "flat");
    REQUIRE(got.size() == want.size());
    for (size_t i = 0; i < got.size(); i++) {
      INFO("tool " << i);
      CHECK(got[i] == want[i]);
    }
  }
  if (expect.contains("cam_op_types")) {
    const auto want = expect.at("cam_op_types").get<std::vector<std::string>>();
    std::vector<std::string> got;
    for (const cam::Setup& setup : setups)
      for (const cam::Operation& op : setup.operations)
        got.push_back(op.type == cam::OpType::EdgeRound  ? "edge_round"
                      : op.type == cam::OpType::Pocket   ? "pocket"
                      : op.type == cam::OpType::Face     ? "face"
                      : op.type == cam::OpType::Drill    ? "drill"
                      : op.type == cam::OpType::Adaptive ? "adaptive"
                                                         : "profile");
    REQUIRE(got.size() == want.size());
    for (size_t i = 0; i < got.size(); i++) {
      INFO("operation " << i);
      CHECK(got[i] == want[i]);
    }
  }
}

// The workpiece and where zero is on it (v68). The stock box is what every
// derived depth, the verifier and the simulator read; the origin is what the
// POST subtracts from every coordinate it writes. A fixed-size sheet that
// reloads as a shrink-wrap, or an origin that reloads at the model origin, is
// a file whose numbers are all offset by however much the box moved — and the
// part comes out looking right in the viewport either way.
void AssertCamStock(const std::vector<cam::Setup>& setups,
                    const std::vector<Body>& bodies,
                    const nlohmann::json& expect) {
  if (expect.contains("cam_stock_boxes")) {
    const auto& want = expect.at("cam_stock_boxes");
    size_t i = 0;
    for (const cam::Setup& setup : setups) {
      REQUIRE(i < want.size());
      glm::dvec3 lo(0.0), hi(0.0);
      INFO("setup " << i);
      REQUIRE(cam::StockBounds(setup, bodies, lo, hi));
      for (int c = 0; c < 3; c++) {
        CHECK_THAT(lo[c], Catch::Matchers::WithinAbs(
                              want[i]["min"][c].get<double>(), 1e-6));
        CHECK_THAT(hi[c], Catch::Matchers::WithinAbs(
                              want[i]["max"][c].get<double>(), 1e-6));
      }
      i++;
    }
    CHECK(i == want.size());
  }
  if (expect.contains("cam_origins")) {
    const auto want =
        expect.at("cam_origins").get<std::vector<std::vector<double>>>();
    size_t i = 0;
    for (const cam::Setup& setup : setups) {
      REQUIRE(i < want.size());
      INFO("setup " << i);
      // Recomputed rather than read: the origin is DERIVED, and the golden
      // asserts the derivation still lands where the file said it did.
      cam::Setup copy = setup;
      REQUIRE(cam::RecomputeOrigin(copy, bodies));
      for (int c = 0; c < 3; c++)
        CHECK_THAT(copy.origin[c], Catch::Matchers::WithinAbs(
                                       want[i][static_cast<size_t>(c)], 1e-6));
      i++;
    }
    CHECK(i == want.size());
  }
}

// How the part is fixtured, and whether the machine has a rotary axis (v67).
// The frame decides the coordinates of every cut, so a setup that reloads
// "as modelled" cuts a laid-flat job in the orientation it was NOT clamped
// in. And the rotary flag is what keeps an A word out of a 3-axis file: the
// two were one number until docs/cam/setup-frame.md separated them, and a
// golden is how the separation stays separated.
void AssertCamFrames(const std::vector<cam::Setup>& setups,
                     const nlohmann::json& expect) {
  if (expect.contains("cam_setup_frames")) {
    const auto want =
        expect.at("cam_setup_frames").get<std::vector<std::vector<double>>>();
    std::vector<std::vector<double>> got;
    for (const cam::Setup& setup : setups)
      got.push_back({setup.frame.x, setup.frame.y, setup.frame.z,
                     setup.frame.w}); // [x, y, z, w], as written
    REQUIRE(got.size() == want.size());
    for (size_t i = 0; i < got.size(); i++) {
      INFO("setup " << i);
      REQUIRE(want[i].size() == 4);
      for (int c = 0; c < 4; c++)
        CHECK_THAT(
            got[i][static_cast<size_t>(c)],
            Catch::Matchers::WithinAbs(want[i][static_cast<size_t>(c)], 1e-12));
    }
  }
  if (expect.contains("cam_setup_rotary")) {
    const auto want = expect.at("cam_setup_rotary").get<std::vector<bool>>();
    std::vector<bool> got;
    for (const cam::Setup& setup : setups)
      got.push_back(setup.machine.hasRotaryAxis);
    REQUIRE(got.size() == want.size());
    for (size_t i = 0; i < got.size(); i++) {
      INFO("setup " << i);
      CHECK(got[i] == want[i]);
    }
  }
  // v77: the machine a setup embeds — its name (empty = the job's own), its
  // post, arc fitting, work area and tool library — and, for a file that
  // predates the block, the three keys that used to be the setup's own,
  // read into an unnamed machine.
  if (expect.contains("cam_machine_names")) {
    const auto want =
        expect.at("cam_machine_names").get<std::vector<std::string>>();
    REQUIRE(setups.size() == want.size());
    for (size_t i = 0; i < setups.size(); i++) {
      INFO("setup " << i);
      CHECK(setups[i].machine.name == want[i]);
    }
  }
  if (expect.contains("cam_posts")) {
    const auto want = expect.at("cam_posts").get<std::vector<std::string>>();
    REQUIRE(setups.size() == want.size());
    for (size_t i = 0; i < setups.size(); i++) {
      INFO("setup " << i);
      CHECK(setups[i].machine.post == want[i]);
    }
  }
  if (expect.contains("cam_fit_arcs")) {
    const auto want = expect.at("cam_fit_arcs").get<std::vector<bool>>();
    REQUIRE(setups.size() == want.size());
    for (size_t i = 0; i < setups.size(); i++) {
      INFO("setup " << i);
      CHECK(setups[i].machine.fitArcs == want[i]);
    }
  }
  if (expect.contains("cam_machine_travel_max")) {
    const auto& want = expect.at("cam_machine_travel_max");
    REQUIRE(setups.size() == want.size());
    for (size_t i = 0; i < setups.size(); i++) {
      INFO("setup " << i);
      if (want[i].is_null()) {
        CHECK(!setups[i].machine.HasWorkArea());
        continue;
      }
      REQUIRE(setups[i].machine.HasWorkArea());
      for (int c = 0; c < 3; c++)
        CHECK_THAT(setups[i].machine.travelMax[c],
                   Catch::Matchers::WithinAbs(want[i][c].get<double>(), 1e-9));
    }
  }
  if (expect.contains("cam_machine_library_sizes")) {
    const auto want =
        expect.at("cam_machine_library_sizes").get<std::vector<size_t>>();
    REQUIRE(setups.size() == want.size());
    for (size_t i = 0; i < setups.size(); i++) {
      INFO("setup " << i);
      CHECK(setups[i].machine.library.size() == want[i]);
    }
  }
}

// What a job HOLDS (v74): the parts it cuts, where each sits in the stock,
// and which part each machining selection is on. The qualifier is the whole
// point of the bump -- a target that reloads without it resolves against
// whatever body answers to the same element name, which is the wrong part,
// cut for real.
void AssertJobParts(const cam::JobDocument& job, const nlohmann::json& expect) {
  if (expect.contains("job_parts")) {
    const auto want = expect.at("job_parts").get<std::vector<std::string>>();
    std::vector<std::string> got;
    for (const cam::JobPart& p : job.GetParts())
      got.push_back(p.name);
    CHECK(got == want);
  }
  if (expect.contains("job_target_parts")) {
    const auto want =
        expect.at("job_target_parts").get<std::vector<uint64_t>>();
    std::vector<uint64_t> got;
    for (const cam::Setup& s : job.GetSetups())
      for (const cam::Operation& op : s.operations)
        for (const cam::Target& t : op.targets)
          got.push_back(t.jobPartId);
    CHECK(got == want);
  }
  // What order it CUTS in (v75), and the order that produces. Both, because
  // the field is only worth having if it reaches the list the post walks.
  if (expect.contains("cam_order")) {
    const std::string want = expect.at("cam_order").get<std::string>();
    REQUIRE(!job.GetSetups().empty());
    const cam::OrderOutputBy o = job.GetSetups()[0].order;
    const std::string got = o == cam::OrderOutputBy::Tool   ? "tool"
                            : o == cam::OrderOutputBy::Part ? "part"
                                                            : "authored";
    CHECK(got == want);
  }
  if (expect.contains("cam_cutting_order")) {
    const auto want =
        expect.at("cam_cutting_order").get<std::vector<uint64_t>>();
    REQUIRE(!job.GetSetups().empty());
    const cam::Setup& s = job.GetSetups()[0];
    std::vector<uint64_t> got;
    for (size_t i : cam::CuttingOrder(s))
      got.push_back(s.operations[i].id);
    CHECK(got == want);
  }
  // Which parts are clamped differently from the setup, and how (v76). A
  // null entry is a placement with no frame of its own -- which must reload
  // as ABSENT, not as an identity that quietly un-clamps the part.
  if (expect.contains("job_placement_frames")) {
    const auto& want = expect.at("job_placement_frames");
    std::vector<const cam::Placement*> got;
    for (const cam::Setup& s : job.GetSetups())
      for (const cam::Placement& p : s.placements)
        got.push_back(&p);
    REQUIRE(got.size() == want.size());
    for (size_t i = 0; i < got.size(); i++) {
      INFO("placement " << i);
      if (want[i].is_null()) {
        CHECK_FALSE(got[i]->frame.has_value());
        continue;
      }
      REQUIRE(got[i]->frame.has_value());
      const glm::dquat& q = *got[i]->frame;
      const double c[4] = {q.x, q.y, q.z, q.w};
      for (int k = 0; k < 4; k++)
        CHECK_THAT(c[k],
                   Catch::Matchers::WithinAbs(
                       want[i][static_cast<size_t>(k)].get<double>(), 1e-12));
    }
  }
  if (expect.contains("job_placements")) {
    const auto& want = expect.at("job_placements");
    std::vector<cam::Placement> got;
    for (const cam::Setup& s : job.GetSetups())
      for (const cam::Placement& p : s.placements)
        got.push_back(p);
    REQUIRE(got.size() == want.size());
    for (size_t i = 0; i < got.size(); i++) {
      INFO("placement " << i);
      CHECK(got[i].jobPartId == want[i].at("part").get<uint64_t>());
      CHECK_THAT(got[i].pos.x, Catch::Matchers::WithinAbs(
                                   want[i].at("pos")[0].get<double>(), 1e-9));
      CHECK_THAT(got[i].pos.y, Catch::Matchers::WithinAbs(
                                   want[i].at("pos")[1].get<double>(), 1e-9));
      CHECK_THAT(got[i].rotZdeg, Catch::Matchers::WithinAbs(
                                     want[i].at("rot_z").get<double>(), 1e-9));
      CHECK(got[i].enabled == want[i].at("enabled").get<bool>());
    }
  }
}

// Which face Z zero was picked from. On the SETUP, so a job reopened from its
// own file still knows — the panel used to ask a session field and answered
// "Z zero is not set yet" about an origin it had saved and was using.
void AssertCamTopFace(const std::vector<cam::Setup>& setups,
                      const nlohmann::json& expect) {
  if (!expect.contains("cam_top_face"))
    return;
  REQUIRE(!setups.empty());
  CHECK(setups[0].topFace.ref.name ==
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
// The JOB corpus (v73, docs/cam/job-document.md). CAM setups left the part
// document, so the assertions that used to ride on a part golden ride here.
//
// A job owns no geometry, and two of the assertions -- the stock box and the
// derived origin -- are claims ABOUT geometry. So each job golden's expect
// file names the part golden whose bodies its setups cut; without that pairing
// the only honest thing left to check would be that the JSON round-trips,
// which is the half that never caught anything.
//
// The setups inside these files are the historical ones, verbatim, including
// the key shapes that predate their own repairs: v73-cam-arcs.json still has
// a pocket with no `side` key, which is the exact document
// docs/cam/dogfood-round.md's migration exists for.
TEST_CASE("Job golden corpus loads and round-trips", "[golden][cam]") {
  const fs::path root = fs::path(DESIGNER_SOURCE_DIR) / "tests" / "golden";
  const fs::path dir = root / "job";
  REQUIRE(fs::exists(dir));

  std::vector<fs::path> goldens;
  for (const auto& entry : fs::directory_iterator(dir)) {
    const fs::path& p = entry.path();
    if (p.extension() == ".json" && !p.stem().string().ends_with(".expect"))
      goldens.push_back(p);
  }
  std::sort(goldens.begin(), goldens.end());
  CHECK(goldens.size() >= 6); // one per CAM era that had a golden

  for (const fs::path& golden : goldens) {
    DYNAMIC_SECTION(golden.filename().string()) {
      fs::path expectPath = golden;
      expectPath.replace_extension(".expect.json");
      REQUIRE(fs::exists(expectPath));
      nlohmann::json expect;
      std::ifstream(expectPath) >> expect;

      auto job = DocumentIO::LoadJob(golden.string());
      REQUIRE(job.has_value());
      REQUIRE((*job) != nullptr);

      // The bodies these setups cut, from the paired part golden.
      std::vector<Body> bodies;
      if (expect.contains("part")) {
        const fs::path partPath = root / expect.at("part").get<std::string>();
        auto part = testio::Load(partPath.string());
        REQUIRE(part != nullptr);
        bodies = part->GetBodies();
      }

      auto check = [&](const cam::JobDocument& j) {
        AssertCamSides(j.GetSetups(), expect);
        AssertCamTopFace(j.GetSetups(), expect);
        AssertCamShapes(j.GetSetups(), expect);
        AssertCamFrames(j.GetSetups(), expect);
        AssertCamStock(j.GetSetups(), bodies, expect);
        AssertJobParts(j, expect);
      };
      check(**job);

      // Round-trip through the CURRENT writer: migrations must compose.
      const fs::path tmp = fs::temp_directory_path() /
                           ("golden_job_rt_" + golden.filename().string());
      REQUIRE(DocumentIO::SaveJob(**job, tmp.string()).has_value());
      auto reloaded = DocumentIO::LoadJob(tmp.string());
      REQUIRE(reloaded.has_value());
      check(**reloaded);
      fs::remove(tmp);
    }
  }
}

TEST_CASE("The golden corpus reaches the current kVersion", "[golden]") {
  const fs::path root = fs::path(DESIGNER_SOURCE_DIR) / "tests" / "golden";
  int newest = 0;
  std::string newestFile;
  for (const fs::path& dir : {root, root / "assembly", root / "job"}) {
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
                               "tests/golden/assembly/ for assembly schema, "
                               "tests/golden/job/ for a CAM job.");
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
      AssertUnits(*doc, expect);

      // Round-trip through the CURRENT writer: migrations must compose
      const fs::path tmp = fs::temp_directory_path() /
                           ("golden_rt_" + golden.filename().string());
      REQUIRE(testio::Save(*doc, tmp.string()));
      auto reloaded = testio::Load(tmp.string());
      REQUIRE((reloaded != nullptr && error.empty()));
      AssertMatchesExpect(*reloaded, expect);
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
