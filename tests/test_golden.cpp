#include "TestPaths.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestIO.h"
#include "core/Document.h"
#include "core/Feature.h"
#include "core/Units.h"
#include "core/cam/CamJob.h"
#include "core/cam/CamNames.h"
#include "core/cam/CamToolpath.h"
#include "core/features/CurveFeature.h"
#include "core/io/Serialization.h"
#include "core/tube/Rules.h"

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
// document the `side` load default has to repair
// (docs/cam/operations.md#the-capability-table-and-the-field-table).
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
        // Through the table, not a ternary chain ending in "profile" -- that
        // chain would have censused an unhandled type AS a profile and passed,
        // which is a test telling the reader the opposite of the truth.
        got.push_back(cam::OpTypeNameOrThrow(op.type));
    REQUIRE(got.size() == want.size());
    for (size_t i = 0; i < got.size(); i++) {
      INFO("operation " << i);
      CHECK(got[i] == want[i]);
    }
  }
  // v139: each setup's own traverse rate, and its machine's beside it -- the
  // setup's used to reload as 0 while the machine's copy survived, and the
  // two differing in the golden is what shows which one was read.
  if (expect.contains("cam_setup_rapid_feeds")) {
    const auto want =
        expect.at("cam_setup_rapid_feeds").get<std::vector<double>>();
    REQUIRE(setups.size() == want.size());
    for (size_t i = 0; i < setups.size(); i++) {
      INFO("setup " << i);
      CHECK_THAT(setups[i].rapidFeed,
                 Catch::Matchers::WithinAbs(want[i], 1e-9));
    }
  }
  if (expect.contains("cam_machine_rapid_feeds")) {
    const auto want =
        expect.at("cam_machine_rapid_feeds").get<std::vector<double>>();
    REQUIRE(setups.size() == want.size());
    for (size_t i = 0; i < setups.size(); i++) {
      INFO("setup " << i);
      CHECK_THAT(setups[i].machine.rapidFeed,
                 Catch::Matchers::WithinAbs(want[i], 1e-9));
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
// two were one number until they were separated
// (docs/cam/setup.md#fixturing-frame-and-rotary-index), and a golden is how the
// separation stays separated.
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
  if (expect.contains("cam_op_finish_tools")) {
    const auto want = expect.at("cam_op_finish_tools").get<std::vector<int>>();
    std::vector<int> got;
    for (const cam::Setup& setup : setups)
      for (const cam::Operation& op : setup.operations)
        got.push_back(op.finishTool);
    REQUIRE(got.size() == want.size());
    for (size_t i = 0; i < got.size(); i++) {
      INFO("op " << i);
      CHECK(got[i] == want[i]);
    }
  }
  if (expect.contains("cam_op_lead_arcs")) {
    const auto want = expect.at("cam_op_lead_arcs").get<std::vector<bool>>();
    std::vector<bool> got;
    for (const cam::Setup& setup : setups)
      for (const cam::Operation& op : setup.operations)
        got.push_back(op.leadArcs);
    REQUIRE(got.size() == want.size());
    for (size_t i = 0; i < got.size(); i++) {
      INFO("op " << i);
      CHECK(got[i] == want[i]);
    }
  }
  if (expect.contains("cam_op_drilled_bores")) {
    const auto want =
        expect.at("cam_op_drilled_bores").get<std::vector<double>>();
    std::vector<double> got;
    for (const cam::Setup& setup : setups)
      for (const cam::Operation& op : setup.operations)
        got.push_back(op.drilledBores);
    REQUIRE(got.size() == want.size());
    for (size_t i = 0; i < got.size(); i++) {
      INFO("op " << i);
      CHECK(std::abs(got[i] - want[i]) < 1e-9);
    }
  }
  if (expect.contains("cam_op_reliefs")) {
    const auto want =
        expect.at("cam_op_reliefs").get<std::vector<std::string>>();
    std::vector<std::string> got;
    for (const cam::Setup& setup : setups)
      for (const cam::Operation& op : setup.operations)
        got.push_back(op.cornerRelief == cam::CornerRelief::Dogbone ? "dogbone"
                      : op.cornerRelief == cam::CornerRelief::TBone ? "tbone"
                                                                    : "none");
    REQUIRE(got.size() == want.size());
    for (size_t i = 0; i < got.size(); i++) {
      INFO("op " << i);
      CHECK(got[i] == want[i]);
    }
  }
  if (expect.contains("cam_op_pecks")) {
    const auto want = expect.at("cam_op_pecks").get<std::vector<double>>();
    std::vector<double> got;
    for (const cam::Setup& setup : setups)
      for (const cam::Operation& op : setup.operations)
        got.push_back(op.peck);
    REQUIRE(got.size() == want.size());
    for (size_t i = 0; i < got.size(); i++) {
      INFO("op " << i);
      CHECK_THAT(got[i], Catch::Matchers::WithinAbs(want[i], 1e-9));
    }
  }
  if (expect.contains("cam_split_output")) {
    const auto want = expect.at("cam_split_output").get<std::vector<bool>>();
    REQUIRE(setups.size() == want.size());
    for (size_t i = 0; i < setups.size(); i++) {
      INFO("setup " << i);
      CHECK(setups[i].splitOutput == want[i]);
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
  // v135: which configuration of its source each job part cuts.
  if (expect.contains("job_part_configurations")) {
    const auto want =
        expect.at("job_part_configurations").get<std::vector<uint64_t>>();
    std::vector<uint64_t> got;
    for (const cam::JobPart& p : job.GetParts())
      got.push_back(p.configuration);
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
// v91: where the bed sits on the sheet, and which corner X0Y0 is measured
// from. Both decide where the cutting ends up; a setup that loses either
// comes back cutting somewhere else.
void AssertCamReach(const std::vector<cam::Setup>& setups,
                    const nlohmann::json& expect) {
  if (expect.contains("cam_origin_at")) {
    const auto& want = expect.at("cam_origin_at");
    REQUIRE(want.size() == setups.size());
    for (size_t i = 0; i < setups.size(); i++) {
      const std::string at =
          setups[i].originAt == cam::OriginAt::StockCentre ? "stock_centre"
          : setups[i].originAt == cam::OriginAt::ModelMin  ? "model_min"
          : setups[i].originAt == cam::OriginAt::StockMax  ? "stock_max"
                                                           : "stock_min";
      INFO("setup " << i << " origin_at");
      CHECK(at == want[i].get<std::string>());
    }
  }
  if (expect.contains("cam_tool_chiploads")) {
    // v92: a cutter's own chip-load figures. A tool that loses them falls
    // back to a rule off its diameter, silently cutting the citation off
    // every finding it appears in.
    const auto& want = expect.at("cam_tool_chiploads");
    std::vector<std::pair<double, double>> got;
    for (const cam::Setup& s : setups)
      for (const cam::Tool& t : s.tools)
        got.emplace_back(t.chiploadMin, t.chiploadMax);
    REQUIRE(got.size() == want.size());
    for (size_t i = 0; i < got.size(); i++) {
      INFO("tool " << i << " chipload");
      CHECK_THAT(got[i].first,
                 Catch::Matchers::WithinAbs(want[i][0].get<double>(), 1e-12));
      CHECK_THAT(got[i].second,
                 Catch::Matchers::WithinAbs(want[i][1].get<double>(), 1e-12));
    }
  }
  if (expect.contains("cam_op_sample_steps")) {
    // v122: a rotary pass's spacing ALONG the axis. Checked because the
    // surfacing round shipped a bug of exactly this shape -- an adaptive
    // op's `stepover` was written only for some op types, so it reloaded as
    // the default, in a golden that carried the real value and compared only
    // op type NAMES.
    const auto& want = expect.at("cam_op_sample_steps");
    std::vector<double> got;
    for (const cam::Setup& s : setups)
      for (const cam::Operation& o : s.operations)
        got.push_back(o.sampleStep);
    REQUIRE(got.size() == want.size());
    for (size_t i = 0; i < got.size(); i++) {
      INFO("op " << i << " sample_step_mm");
      CHECK_THAT(got[i],
                 Catch::Matchers::WithinAbs(want[i].get<double>(), 1e-12));
    }
  }
  if (expect.contains("cam_op_rotary_patterns")) {
    // v123: parallel passes or one spiral. A spiral that reloads as parallel
    // is a different cut from the same file, and nothing else would say so.
    const auto& want = expect.at("cam_op_rotary_patterns");
    std::vector<std::string> got;
    for (const cam::Setup& s : setups)
      for (const cam::Operation& o : s.operations)
        got.push_back(cam::RotaryPatternName(o.rotaryPattern));
    REQUIRE(got.size() == want.size());
    for (size_t i = 0; i < got.size(); i++) {
      INFO("op " << i << " rotary_pattern");
      CHECK(got[i] == want[i].get<std::string>());
    }
  }
  if (expect.contains("cam_op_leads")) {
    // v124: a helical pass's lead, signed for the hand. A lead that reloads
    // as 0 follows the part's own twist instead -- a different cut, and on a
    // part with no twist, a different PATTERN.
    const auto& want = expect.at("cam_op_leads");
    std::vector<double> got;
    for (const cam::Setup& s : setups)
      for (const cam::Operation& o : s.operations)
        got.push_back(o.leadMm);
    REQUIRE(got.size() == want.size());
    for (size_t i = 0; i < got.size(); i++) {
      INFO("op " << i << " lead_mm");
      CHECK_THAT(got[i],
                 Catch::Matchers::WithinAbs(want[i].get<double>(), 1e-12));
    }
  }
  if (expect.contains("cam_op_index_a")) {
    // v136: the indices one operation is cut at. A list that reloads empty
    // cuts ONE crenel of four, at the setup's own index, and verifies clean.
    const auto& want = expect.at("cam_op_index_a");
    std::vector<std::vector<double>> got;
    for (const cam::Setup& s : setups)
      for (const cam::Operation& o : s.operations)
        got.push_back(o.indexA);
    REQUIRE(got.size() == want.size());
    for (size_t i = 0; i < got.size(); i++) {
      INFO("op " << i << " index_a_deg");
      CHECK(got[i] == want[i].get<std::vector<double>>());
    }
  }
  if (expect.contains("cam_op_engrave_widths") ||
      expect.contains("cam_op_engrave_depths")) {
    // v138: an engraving's groove, stated as its width or its depth. One
    // that reloads as 0 is refused at the next generate ("state width_mm or
    // depth_mm"); one that reloads as the OTHER statement is a different
    // groove for any tool but the one it was typed for.
    std::vector<double> widths, depths;
    for (const cam::Setup& s : setups)
      for (const cam::Operation& o : s.operations) {
        widths.push_back(o.engraveWidth);
        depths.push_back(o.engraveDepth);
      }
    const auto check = [](const char* key, const std::vector<double>& got,
                          const nlohmann::json& want) {
      REQUIRE(got.size() == want.size());
      for (size_t i = 0; i < got.size(); i++) {
        INFO("op " << i << " " << key);
        CHECK_THAT(got[i],
                   Catch::Matchers::WithinAbs(want[i].get<double>(), 1e-12));
      }
    };
    if (expect.contains("cam_op_engrave_widths"))
      check("width_mm", widths, expect.at("cam_op_engrave_widths"));
    if (expect.contains("cam_op_engrave_depths"))
      check("depth_mm", depths, expect.at("cam_op_engrave_depths"));
  }
  if (expect.contains("cam_op_rest_from_tools")) {
    // v125: a rest pass's reference cutter. One that reloads as 0 cuts the
    // whole region again with the small tool -- hours, for a corner.
    const auto want =
        expect.at("cam_op_rest_from_tools").get<std::vector<int>>();
    std::vector<int> got;
    for (const cam::Setup& s : setups)
      for (const cam::Operation& o : s.operations)
        got.push_back(o.restFromTool);
    CHECK(got == want);
  }
  if (expect.contains("cam_op_stepdowns")) {
    // v123: a rotary op reads its stepdown, as the depth between layers;
    // v125: so does a raster, which then roughs.
    const auto& want = expect.at("cam_op_stepdowns");
    std::vector<double> got;
    for (const cam::Setup& s : setups)
      for (const cam::Operation& o : s.operations)
        got.push_back(o.stepdown);
    REQUIRE(got.size() == want.size());
    for (size_t i = 0; i < got.size(); i++) {
      INFO("op " << i << " stepdown");
      CHECK_THAT(got[i],
                 Catch::Matchers::WithinAbs(want[i].get<double>(), 1e-12));
    }
  }
  if (expect.contains("cam_tool_holders")) {
    // v121: the stickout and the collet nut. A tool that loses either stops
    // being checked against holder collisions ENTIRELY and silently -- the
    // check reads "nobody stated a holder", which is indistinguishable from
    // a job that never had one.
    const auto& want = expect.at("cam_tool_holders");
    std::vector<std::pair<double, double>> got;
    for (const cam::Setup& s : setups)
      for (const cam::Tool& t : s.tools)
        got.emplace_back(t.overallLength, t.collarDiameter);
    REQUIRE(got.size() == want.size());
    for (size_t i = 0; i < got.size(); i++) {
      INFO("tool " << i << " holder");
      CHECK_THAT(got[i].first,
                 Catch::Matchers::WithinAbs(want[i][0].get<double>(), 1e-12));
      CHECK_THAT(got[i].second,
                 Catch::Matchers::WithinAbs(want[i][1].get<double>(), 1e-12));
    }
  }
  if (expect.contains("cam_rotary_mounts")) {
    // v132: how the rotary is installed. Lost on a load, a Y-mounted B axis
    // posts as an X-mounted A -- across the part -- or does not post at all.
    const auto& want = expect.at("cam_rotary_mounts");
    REQUIRE(setups.size() == want.size());
    for (size_t i = 0; i < setups.size(); i++) {
      const cam::RotaryMount& r = setups[i].machine.rotary;
      CHECK(std::string(1, r.along) == want[i][0].get<std::string>());
      CHECK(r.chuckEnd == want[i][1].get<int>());
      CHECK(std::string(1, r.word) == want[i][2].get<std::string>());
      CHECK(std::string(r.topSign < 0 ? "-" : "+") + r.topAxis ==
            want[i][3].get<std::string>());
    }
  }
  if (expect.contains("cam_holdings")) {
    // v132: the chuck and the tailstock every rotary path is kept out of.
    const auto& want = expect.at("cam_holdings");
    REQUIRE(setups.size() == want.size());
    for (size_t i = 0; i < setups.size(); i++) {
      const cam::RotaryHolding& h = setups[i].machine.holding;
      CHECK(h.chuckDiameter == want[i][0].get<double>());
      CHECK(h.gripLength == want[i][1].get<double>());
      CHECK(h.hasTailstock == want[i][2].get<bool>());
      CHECK(h.centreLength == want[i][3].get<double>());
      CHECK(h.centreDiameter == want[i][4].get<double>());
      CHECK(h.tailBodyDiameter == want[i][5].get<double>());
      CHECK(h.clearance == want[i][6].get<double>());
    }
  }
  if (expect.contains("cam_setup_holding")) {
    // v132: where a setup's part is held, and by what.
    const auto& want = expect.at("cam_setup_holding");
    REQUIRE(setups.size() == want.size());
    for (size_t i = 0; i < setups.size(); i++) {
      const cam::Setup& s = setups[i];
      const std::string held = s.heldIn == cam::Setup::HeldIn::Rotary ? "rotary"
                               : s.heldIn == cam::Setup::HeldIn::Bed
                                   ? "bed"
                                   : "derived";
      CHECK(held == want[i][0].get<std::string>());
      CHECK(s.gripLength == want[i][1].get<double>());
      CHECK(s.tailstock == want[i][2].get<int>());
      CHECK(s.chuck == want[i][3].get<int>());
    }
  }
  if (expect.contains("cam_tool_shanks")) {
    // v131: how wide the tool is above its flutes. Lost on a load, a 3 mm
    // cutter on a 6 mm shank reads as a plain 3 mm one again, and the op
    // that drives that shank into a wall stops being refused.
    const auto& want = expect.at("cam_tool_shanks");
    std::vector<double> got;
    for (const cam::Setup& s : setups)
      for (const cam::Tool& t : s.tools)
        got.push_back(t.shankDiameter);
    REQUIRE(got.size() == want.size());
    for (size_t i = 0; i < got.size(); i++) {
      INFO("tool " << i << " shank");
      CHECK_THAT(got[i],
                 Catch::Matchers::WithinAbs(want[i].get<double>(), 1e-12));
    }
  }
  if (expect.contains("cam_fixtures")) {
    // v126: what holds the stock (docs/cam/setup.md#workholding-fixtures). A
    // fixture that loses its place or its size stops being where the clamp is,
    // and the verifier then signs off a path through the real one.
    const auto& want = expect.at("cam_fixtures");
    std::vector<const cam::Fixture*> got;
    for (const cam::Setup& s : setups)
      for (const cam::Fixture& f : s.fixtures)
        got.push_back(&f);
    REQUIRE(got.size() == want.size());
    for (size_t i = 0; i < got.size(); i++) {
      INFO("fixture " << i);
      const cam::Fixture& f = *got[i];
      const nlohmann::json& w = want[i];
      CHECK(f.name == w.at("name").get<std::string>());
      CHECK(f.sourceDocId == w.value("part", uint64_t{0}));
      CHECK(f.IsBox() == w.contains("box"));
      for (int k = 0; k < 3; k++) {
        CHECK_THAT(f.pos[k], Catch::Matchers::WithinAbs(
                                 w.at("pos")[k].get<double>(), 1e-12));
        if (w.contains("box"))
          CHECK_THAT(f.box[k], Catch::Matchers::WithinAbs(
                                   w.at("box")[k].get<double>(), 1e-12));
      }
      CHECK_THAT(f.rotZdeg,
                 Catch::Matchers::WithinAbs(w.value("rot_z", 0.0), 1e-12));
      CHECK(f.enabled == w.value("enabled", true));
      // v127: resting on the stock (docs/cam/setup.md#seats). A seat
      // that loses its edge or its distances puts the clamp somewhere else
      // on the board, and the verifier then checks the wrong place.
      CHECK(f.seat.has_value() == w.contains("seat"));
      if (f.seat && w.contains("seat")) {
        const nlohmann::json& ws = w.at("seat");
        CHECK(f.seat->face.featureId == ws.at("feature").get<uint64_t>());
        CHECK(f.seat->face.name == ws.at("name").get<std::string>());
        CHECK(cam::StockEdgeName(f.seat->edge) ==
              ws.at("edge").get<std::string>());
        CHECK_THAT(
            f.seat->alongMm,
            Catch::Matchers::WithinAbs(ws.at("along_mm").get<double>(), 1e-12));
        CHECK_THAT(f.seat->overMm, Catch::Matchers::WithinAbs(
                                       ws.at("over_mm").get<double>(), 1e-12));
        CHECK_THAT(f.seat->spinDeg, Catch::Matchers::WithinAbs(
                                        ws.value("spin_deg", 0.0), 1e-12));
      }
    }
  }
  if (expect.contains("cam_reach_offsets")) {
    const auto& want = expect.at("cam_reach_offsets");
    REQUIRE(want.size() == setups.size());
    for (size_t i = 0; i < setups.size(); i++) {
      INFO("setup " << i << " reach_offset");
      CHECK_THAT(setups[i].reachOffset.x,
                 Catch::Matchers::WithinAbs(want[i][0].get<double>(), 1e-9));
      CHECK_THAT(setups[i].reachOffset.y,
                 Catch::Matchers::WithinAbs(want[i][1].get<double>(), 1e-9));
    }
  }
}

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

// Tolerances (v98). Optional, like the CAM and units assertions. Worth
// gating because the failure is silent in a particular way: a tolerance that
// fails to reload does not break the document or move a surface — the part
// looks exactly right and is simply no longer the size anybody decided on.
// The ELEMENT ref is checked as well as the class, since a tolerance that
// reloads pointing at the wrong face is worse than one that is missing.
void AssertTolerances(const Document& doc, const nlohmann::json& expect) {
  if (!expect.contains("tolerances"))
    return;
  const nlohmann::json& want = expect.at("tolerances");
  REQUIRE(doc.GetTolerances().size() == want.size());
  for (size_t i = 0; i < want.size(); i++) {
    INFO("tolerance " << i);
    const Tolerance& got = doc.GetTolerances()[i];
    CHECK(got.ref.name == want[i].at("element").get<std::string>());
    CHECK(got.ref.featureId == want[i].at("feature_id").get<uint64_t>());
    // The second ref is what makes it a DISTANCE rather than a size, so a
    // golden that loses it reloads as a tolerance on the wrong thing
    // entirely — with limits that still look plausible.
    CHECK(got.ref2.name == want[i].value("b_element", std::string{}));
    CHECK(got.ref2.featureId == want[i].value("b_feature_id", uint64_t{0}));
    CHECK(got.IsDistance() == want[i].contains("b_element"));
    CHECK(got.isoClass == want[i].value("class", std::string{}));
    CHECK_THAT(got.lower,
               Catch::Matchers::WithinAbs(want[i].value("lower", 0.0), 1e-12));
    CHECK_THAT(got.upper,
               Catch::Matchers::WithinAbs(want[i].value("upper", 0.0), 1e-12));
  }
}

// The sheet-metal rules, and — the half a stored field cannot show — the
// numbers they RESOLVE to. A golden that reloads its gauge but derives a
// different K bends the same part to a different flat length, and the
// document looks identical either way.
void AssertSheetMetal(const Document& doc, const nlohmann::json& expect) {
  if (!expect.contains("sheet_metal")) {
    CHECK_FALSE(doc.SheetMetal().has_value());
    return;
  }
  const nlohmann::json& want = expect.at("sheet_metal");
  REQUIRE(doc.SheetMetal().has_value());
  const sheet::SheetMetalRules& got = *doc.SheetMetal();
  CHECK(got.material == want.at("material").get<std::string>());
  CHECK(got.gauge == want.at("gauge").get<std::string>());
  CHECK_THAT(got.thickness, Catch::Matchers::WithinAbs(
                                want.at("thickness").get<double>(), 1e-9));
  const double radius = got.EffectiveBendRadius();
  CHECK_THAT(radius, Catch::Matchers::WithinAbs(
                         want.at("bend_radius").get<double>(), 1e-9));
  CHECK_THAT(
      got.EffectiveKFactor(radius),
      Catch::Matchers::WithinAbs(want.at("k_factor").get<double>(), 1e-9));
  CHECK(sheet::ReliefStyleName(got.reliefStyle) ==
        want.value("relief", std::string("rectangular")));
  if (want.contains("relief_width"))
    CHECK_THAT(got.EffectiveReliefWidth(),
               Catch::Matchers::WithinAbs(want.at("relief_width").get<double>(),
                                          1e-9));
  if (want.contains("relief_depth"))
    CHECK_THAT(got.EffectiveReliefDepth(radius),
               Catch::Matchers::WithinAbs(want.at("relief_depth").get<double>(),
                                          1e-9));
}

void AssertMatchesExpect(const Document& doc, const nlohmann::json& expect) {
  if (expect.contains("generator"))
    CHECK(doc.Generator() == expect.at("generator"));
  if (expect.contains("catalogs"))
    CHECK(doc.GetCatalogs() == expect.at("catalogs"));
  AssertTolerances(doc, expect);
  AssertSheetMetal(doc, expect);
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
  // v117: a bent path's FORMER. The corners survive a round trip as points
  // whatever happens, but `radius` is the one field a bender is set from —
  // lost, the path refuses to build at all, and read as a spline's the same
  // tube comes back with no radius to quote. So the golden states the
  // radius, the bend angles and the exact path length the arcs give.
  if (expect.contains("bent_path")) {
    const CurveFeature* curve = nullptr;
    for (const auto& f : doc.GetFeatures())
      if (const auto* c = dynamic_cast<const CurveFeature*>(f.get()))
        if (c->kind == Curve3DKind::BentPath)
          curve = c;
    REQUIRE(curve != nullptr);
    const nlohmann::json& want = expect.at("bent_path");
    CHECK_THAT(curve->radius, WithinRel(want.at("radius").get<double>(), 1e-9));
    const std::vector<CurveFeature::Bend> bends = curve->Bends();
    REQUIRE(bends.size() == want.at("bends").get<size_t>());
    const auto angles = want.at("angles_deg").get<std::vector<double>>();
    REQUIRE(angles.size() == bends.size());
    for (size_t i = 0; i < bends.size(); i++) {
      INFO("bend " << i);
      CHECK_THAT(bends[i].angleDeg, WithinRel(angles[i], 1e-6));
      CHECK_THAT(bends[i].radius,
                 WithinRel(want.at("radius").get<double>(), 1e-9));
    }
    CHECK_THAT(curve->Length(),
               WithinRel(want.at("length_mm").get<double>(), 1e-9));
  }
  // v118: the stock a part is BENT from. The derived fields are the point:
  // a minimum straight and a tail length are 2 x OD and 1.5 x OD, so a golden
  // that states them in millimetres catches a literal creeping back in, and
  // stating the die set catches a former list quietly dropped on the way
  // through the envelope.
  if (expect.contains("tube")) {
    REQUIRE(doc.Tube().has_value());
    const tube::TubeRules& r = *doc.Tube();
    const nlohmann::json& want = expect.at("tube");
    CHECK(r.material == want.at("material").get<std::string>());
    CHECK(r.formerRadii ==
          want.at("former_radii_mm").get<std::vector<double>>());
    // The straight a clamp needs and the tail a bender grips are multiples
    // of the MEMBER's diameter, so the golden states them against one — and
    // that is the point: a tube part carries no size of its own.
    const double od = want.at("member_od_mm").get<double>();
    CHECK_THAT(r.EffectiveMinStraight(od),
               WithinRel(want.at("min_straight_mm").get<double>(), 1e-9));
    CHECK_THAT(r.EffectiveTailLength(od),
               WithinRel(want.at("tail_length_mm").get<double>(), 1e-9));
    CHECK_THAT(r.EffectiveMaxThinning(),
               WithinRel(want.at("max_thinning").get<double>(), 1e-9));
  }
  // v143: how the part is printed. Drives no geometry, so the volumes above
  // say nothing about it; a pair dropped on the way through the envelope
  // would read back as an unprinted part and every fit report would stop
  // naming the allowance.
  if (expect.contains("print")) {
    REQUIRE(doc.PrintProcess().has_value());
    CHECK(doc.PrintProcess()->process ==
          expect.at("print").at("process").get<std::string>());
    CHECK(doc.PrintProcess()->material ==
          expect.at("print").at("material").get<std::string>());
  }
  // v97: whether an Import keeps the file's own coordinates. A flag that
  // round-trips as `false` is an import that silently moves — the whole
  // reason the flag exists — so the golden states it and the round-trip has
  // to preserve it.
  if (expect.contains("import_keep_origin")) {
    const auto* imp = [&]() -> const ImportFeature* {
      for (const auto& f : doc.GetFeatures())
        if (const auto* i = dynamic_cast<const ImportFeature*>(f.get()))
          return i;
      return nullptr;
    }();
    REQUIRE(imp != nullptr);
    CHECK(imp->keepOrigin == expect.at("import_keep_origin").get<bool>());
  }
  // v103: the unit an STL/BREP import was drawn in. Lost on a round trip,
  // the part reloads 25.4x (or 1000x) off and looks fine.
  if (expect.contains("import_units")) {
    const ImportFeature* imp = nullptr;
    for (const auto& f : doc.GetFeatures())
      if (const auto* i = dynamic_cast<const ImportFeature*>(f.get()))
        imp = i;
    REQUIRE(imp != nullptr);
    CHECK(std::string(units::ToString(imp->lengthUnit)) ==
          expect.at("import_units").get<std::string>());
  }
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
// The JOB corpus (v73, docs/cam/README.md#the-model). CAM setups left the part
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
// a pocket with no `side` key, which is the exact document the `side` load
// default (docs/cam/operations.md#the-capability-table-and-the-field-table)
// exists for.
TEST_CASE("Job golden corpus loads and round-trips", "[golden][cam]") {
  const fs::path root = fs::path(TestSourceDir()) / "tests" / "golden";
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

      // The op FIELDS as loaded, so the round trip below can fail when the
      // writer drops one. `stepover` was written only for three op types and
      // adaptive was not among them, so a 0.25 engagement reloaded at 0.45 --
      // in a golden that carries a 0.25, checked by a gate that only ever
      // compared op type names.
      const auto fields = [](const cam::JobDocument& j) {
        std::vector<double> v;
        for (const cam::Setup& setup : j.GetSetups())
          for (const cam::Operation& op : setup.operations) {
            v.insert(v.end(),
                     {op.topZ, op.bottomZ, op.stepdown, op.stockToLeave,
                      op.stepover, op.edgeSize, op.peck, op.drilledBores,
                      op.finishStock, static_cast<double>(op.finishTool),
                      static_cast<double>(op.tabCount), op.tabWidth,
                      op.tabHeight, static_cast<double>(op.climb),
                      static_cast<double>(op.useOutline),
                      static_cast<double>(op.leadArcs)});
          }
        return v;
      };
      const std::vector<double> before = fields(**job);

      auto check = [&](const cam::JobDocument& j) {
        AssertCamSides(j.GetSetups(), expect);
        AssertCamTopFace(j.GetSetups(), expect);
        AssertCamShapes(j.GetSetups(), expect);
        AssertCamFrames(j.GetSetups(), expect);
        AssertCamStock(j.GetSetups(), bodies, expect);
        AssertCamReach(j.GetSetups(), expect);
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
      const std::vector<double> after = fields(**reloaded);
      REQUIRE(after.size() == before.size());
      for (size_t i = 0; i < before.size(); i++) {
        INFO("golden " << golden.filename().string() << ", op field " << i);
        CHECK_THAT(after[i], Catch::Matchers::WithinAbs(before[i], 1e-9));
      }
      fs::remove(tmp);
    }
  }
}

TEST_CASE("The golden corpus reaches the current kVersion", "[golden]") {
  const fs::path root = fs::path(TestSourceDir()) / "tests" / "golden";
  int newest = 0;
  std::string newestFile;
  // Four corpora, not three. The comment above warns that forgetting one is
  // how an audit reported a present golden as missing — and tests/golden/
  // drawing/ was missing from this very list, so a drawing-only bump could
  // freeze an artifact the gate would never look at (found adding v89).
  for (const fs::path& dir :
       {root, root / "assembly", root / "job", root / "drawing"}) {
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
      fs::path(TestSourceDir()) / "docs" / "versioning" / "format.md";
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
  const fs::path dir = fs::path(TestSourceDir()) / "tests" / "golden";
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
  const fs::path dir = fs::path(TestSourceDir()) / "tests" / "golden";
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
  // every Ctrl+S (docs/versioning/README.md#baked-geometry).
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
