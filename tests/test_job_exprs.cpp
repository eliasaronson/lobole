// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// A CAM job's numbers that are expressions of the project's variables
// (docs/cam/README.md#numbers-that-are-expressions): a tool bound to a
// variable follows it and the job is edited, a value its owner cannot take
// is that owner's error and keeps its last good value, a lone job refuses a
// variable it cannot see, and rename and delete reach a part, an assembly,
// a job, a drawing and a study at once. The probe at the end is the plan's
// "nothing new arrives unbindable" for the job document.

#include "ExprProbe.h"
#include "core/Command.h"
#include "core/Document.h"
#include "core/Occurrence.h"
#include "core/cam/CamOpFields.h"
#include "core/cam/JobDocument.h"
#include "core/drawing/DrawingDocument.h"
#include "core/io/Serialization.h"
#include "ui/AppActionsInternal.h" // SlotDoc, SlotJob
#include "ui/AppState.h"
#include "ui/DocumentSlots.h"
#include "ui/LiteralActions.h"
#include "ui/VariableActions.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include <unistd.h>

using Catch::Matchers::ContainsSubstring;
namespace fs = std::filesystem;

namespace {

// A project with one part and a job cutting it, one setup, one tool.
struct Rig {
  ui::AppState state;
  size_t jobSlot = 0;
  cam::JobDocument& Job() { return *ui::SlotJob(state, jobSlot); }
  cam::Tool& Tool() { return Job().GetSetups().at(0).tools.at(0); }
};

void MakeRig(Rig& r) {
  r.state.document = std::make_unique<Document>();
  r.state.commands = std::make_unique<CommandStack>();
  ui::EnsureParts(r.state);
  const uint64_t part = r.state.parts.at(0).id;
  r.jobSlot = ui::AddJob(r.state, "Job");
  cam::JobDocument& job = r.Job();
  job.AddPart(part, "Part 1");
  cam::Setup& s = job.AddSetup("Top");
  cam::Tool t;
  t.number = 1;
  s.tools.push_back(t);
}

} // namespace

TEST_CASE("A tool bound to a variable follows it, and the job is edited",
          "[variables][cam]") {
  Rig r;
  MakeRig(r);
  REQUIRE(ui::SetProjectVariable(r.state, "cutter", "6", ""));
  r.Tool().diameter.expr = "cutter + 2";
  ui::SyncProjectScope(r.state);
  CHECK(r.Tool().diameter.value == 8.0);
  const uint64_t stamp = r.Job().SaveStamp();
  REQUIRE(ui::SetProjectVariable(r.state, "cutter", "4", ""));
  CHECK(r.Tool().diameter.value == 6.0);
  CHECK(r.Job().SaveStamp() != stamp); // unsaved, its verdict no longer
  CHECK(r.Job().ExprProblems().empty());

  // A diameter the tool cannot take is the tool's error; it keeps 6.
  REQUIRE(ui::SetProjectVariable(r.state, "cutter", "-2", ""));
  CHECK(r.Tool().diameter.value == 6.0);
  REQUIRE(r.Job().ExprProblems().size() == 1);
  CHECK_THAT(r.Job().ExprProblems()[0].owner, ContainsSubstring("tool T1"));
  CHECK_THAT(r.Job().ExprProblems()[0].error, ContainsSubstring("diameter"));
  // And it clears when the value stands again.
  REQUIRE(ui::SetProjectVariable(r.state, "cutter", "3", ""));
  CHECK(r.Tool().diameter.value == 5.0);
  CHECK(r.Job().ExprProblems().empty());
}

TEST_CASE("The literals lint finds a job's number equal to a variable",
          "[variables][cam]") {
  Rig r;
  MakeRig(r);
  REQUIRE(ui::SetProjectVariable(r.state, "bore", "6", ""));
  r.Tool().diameter = 6.0; // the default tool's, a literal
  size_t checked = 0;
  const auto found = ui::LintJob(r.state, r.Job(), &checked);
  CHECK(checked > 3);
  const auto it = std::find_if(found.begin(), found.end(), [](const auto& f) {
    return f.field.path == "cam/0/tools/0/diameter";
  });
  REQUIRE(it != found.end());
  CHECK(it->feature == "tool T1 in setup 'Top'");
  CHECK(it->variables == std::vector<std::string>{"bore"});
  // Bound, it is no literal.
  r.Tool().diameter.expr = "bore";
  for (const auto& f : ui::LintJob(r.state, r.Job()))
    CHECK(f.field.path != "cam/0/tools/0/diameter");
}

TEST_CASE("A count bound to a variable refuses a fraction",
          "[variables][cam]") {
  Rig r;
  MakeRig(r);
  REQUIRE(ui::SetProjectVariable(r.state, "n", "4", ""));
  cam::Operation op;
  op.type = cam::OpType::Profile;
  op.tabCount.expr = "n";
  r.Job().GetSetups()[0].operations.push_back(op);
  ui::SyncProjectScope(r.state);
  CHECK(r.Job().GetSetups()[0].operations[0].tabCount.value == 4);
  REQUIRE(ui::SetProjectVariable(r.state, "n", "4.5", ""));
  CHECK(r.Job().GetSetups()[0].operations[0].tabCount.value == 4);
  REQUIRE(r.Job().ExprProblems().size() == 1);
  CHECK_THAT(r.Job().ExprProblems()[0].error, ContainsSubstring("tabs/count"));
}

TEST_CASE("A lone job refuses a variable it cannot see", "[variables][cam]") {
  cam::JobDocument job;
  cam::Setup& s = job.AddSetup("Top");
  cam::Tool t;
  t.diameter = 6.0;
  t.diameter.expr = "bore";
  s.tools.push_back(t);
  CHECK(cam::EvaluateJobExprs(job, SymbolTable{}));
  CHECK(job.GetSetups()[0].tools[0].diameter.value == 6.0);
  REQUIRE(job.ExprProblems().size() == 1);
  CHECK_THAT(job.ExprProblems()[0].error, ContainsSubstring("bore"));
}

TEST_CASE("Rename and delete reach a part, an assembly, a job, a drawing and "
          "a study at once",
          "[variables][cam][drawing]") {
  Rig r;
  MakeRig(r);
  REQUIRE(ui::SetProjectVariable(r.state, "gap", "3", ""));
  // A part feature reads it...
  Document& part = *ui::SlotDoc(r.state, 0);
  auto box =
      std::make_unique<BoxFeature>("Plate", glm::dvec3(0.0), 10.0, 10.0, 10.0);
  box->height.expr = "gap * 2";
  part.AddFeature(std::move(box));
  // ...a study of the part...
  fem::Study& st = part.AddStudy("Load case");
  fem::Load l;
  l.magnitude.expr = "gap * 100";
  st.loads.push_back(l);
  // ...an assembly's mate...
  const size_t asmSlot = ui::AddAssembly(r.state, "Box");
  AssemblyDocument& a = *ui::SlotAssembly(r.state, asmSlot);
  Mate m;
  m.name = "Hinge";
  m.type = MateType::Revolute;
  m.offset.expr = "gap";
  const uint64_t hinge = a.AddMate(m);
  // ...the job's tool...
  r.Tool().diameter.expr = "gap * 2";
  // ...and a drawing's view.
  const size_t dwgSlot = ui::AddDrawing(r.state, "Sheet");
  DrawingDocument& dwg = *ui::SlotDrawing(r.state, dwgSlot);
  DrawingView v;
  v.name = "Front";
  v.scale.expr = "gap / 6";
  dwg.AddView(v);

  std::vector<std::string> refs;
  CHECK_FALSE(ui::RemoveProjectVariable(r.state, "gap", &refs));
  const auto has = [&](const std::string& s) {
    return std::find(refs.begin(), refs.end(), s) != refs.end();
  };
  CHECK(has("Part 1: feature 'Plate'"));
  CHECK(has("Part 1: study 'Load case'"));
  CHECK(has("Box: mate 'Hinge'"));
  CHECK(has("Job: tool T1 in setup 'Top'"));
  CHECK(has("Sheet: view 'Front'"));

  REQUIRE(ui::RenameProjectVariable(r.state, "gap", "clearance"));
  CHECK(r.Tool().diameter.expr == "clearance * 2");
  CHECK(ui::SlotDrawing(r.state, dwgSlot)->GetViews()[0].scale.expr ==
        "clearance / 6");
  CHECK(ui::SlotAssembly(r.state, asmSlot)->FindMateById(hinge)->offset.expr ==
        "clearance");
  CHECK(
      ui::SlotDoc(r.state, 0)->GetStudies().at(0).loads.at(0).magnitude.expr ==
      "clearance * 100");
  // Undo puts every one back.
  REQUIRE(ui::UndoProjectVars(r.state));
  CHECK(r.Tool().diameter.expr == "gap * 2");
  CHECK(ui::SlotDrawing(r.state, dwgSlot)->GetViews()[0].scale.expr ==
        "gap / 6");
}

// ---- The probe: nothing new arrives unbindable ------------------------------
//
// Every slot JobDocument::VisitExprs hands out is given a sentinel text, the
// job is written as a file, and (1) each sentinel must land at its slot's
// path, as `<last key>_expr`, and (2) every number in the file must be
// either a visited slot or on the short list below of numbers that are
// deliberately not bindable, each with its reason. A new field then has to
// choose: a slot, or a line here saying why not.

namespace {

const exprprobe::Reasons kNotBindable = {
    {"version", "the format's"},
    {"parts/*/id", "an id"},
    {"parts/*/source", "an id"},
    {"parts/*/configuration", "an id"},
    {"cam/*/id", "an id"},
    {"cam/*/machine/**",
     "the machine the setup was set up on: a copy of a profile from the "
     "machine library, which is outside every project and reads no "
     "variables; a job picks a machine, it does not dimension one (its "
     "rotary holding -- the chuck, the tailstock -- is the machine's too)"},
    {"cam/*/origin/*",
     "derived from the stock, the fixture and the picked face on every "
     "load: an output, its rule (`origin_at`) is what is saved"},
    {"cam/*/frame/*", "a quaternion: how the part is clamped, from a pick"},
    {"cam/*/top_face/**", "ids and names"},
    {"cam/*/tools/*/number",
     "the T-word: which tool, the name operations call it by"},
    {"cam/*/placements/*/id", "an id"},
    {"cam/*/placements/*/part", "an id"},
    {"cam/*/placements/*/frame/*",
     "a quaternion: a part laid face up, from a pick"},
    {"cam/*/fixtures/*/id", "an id"},
    {"cam/*/fixtures/*/part", "an id"},
    {"cam/*/fixtures/*/seat/face/**", "ids and names"},
    {"cam/*/fixtures/*/pos/*",
     "a seated fixture's pose, solved from its seat on every read (its seat's "
     "numbers are slots; an unseated one's pose is)"},
    {"cam/*/fixtures/*/rot_z", "the same"},
    {"cam/*/operations/*/id", "an id"},
    {"cam/*/operations/*/tool", "a tool's T-word: which tool cuts"},
    {"cam/*/operations/*/finish_tool", "the same, for the finishing pass"},
    {"cam/*/operations/*/rest_from_tool", "the same, for rest machining"},
    {"cam/*/operations/*/targets/**", "ids and names"},
};

// One of everything a job holds: each tool type, each operation type, a
// fixed stock, a placement, a box fixture and a seated part fixture.
cam::JobDocument Everything() {
  cam::JobDocument job;
  const uint64_t part = job.AddPart(7, "Plate");
  job.GetParts()[0].configuration = 3;
  cam::Setup& s = job.AddSetup("Top");
  s.stock.mode = cam::StockMode::Fixed;
  s.stock.size = {100, 80, 20};
  s.rotationA = 15.0;
  s.gripLength = 12.0;
  s.rapidFeed = 3000.0;
  s.reachOffset = {5, 0};
  s.frame = glm::dquat(0.7071067811865476, 0.7071067811865476, 0, 0);
  s.topFace.jobPartId = part;
  s.topFace.ref = {4, "Face3"};
  s.origin = {1, 2, 3};
  int number = 1;
  for (const cam::ToolType type :
       {cam::ToolType::Flat, cam::ToolType::Ball, cam::ToolType::VBit,
        cam::ToolType::Drill, cam::ToolType::ThreadMill}) {
    cam::Tool t;
    t.number = number++;
    t.type = type;
    t.chiploadMin = 0.01;
    t.chiploadMax = 0.05;
    t.overallLength = 50.0;
    t.collarDiameter = 20.0;
    t.shankDiameter = 6.0;
    s.tools.push_back(t);
  }
  cam::Placement p;
  p.id = job.AllocateId();
  p.jobPartId = part;
  p.pos = {10, 20};
  p.rotZdeg = 90.0;
  p.frame = glm::dquat(1, 0, 0, 0);
  s.placements.push_back(p);
  cam::Fixture box;
  box.id = job.AllocateId();
  box.box = {10, 20, 30};
  box.name = "Clamp";
  box.pos = {1, 2, 3};
  box.rotZdeg = 5.0;
  s.fixtures.push_back(box);
  cam::Fixture seated;
  seated.id = job.AllocateId();
  seated.sourceDocId = 9;
  seated.pos = {4, 5, 6};
  seated.rotZdeg = 7.0;
  cam::FixtureSeat seat;
  seat.face = {3, "Face1"};
  seat.alongMm = 10.0;
  seat.overMm = 2.0;
  seat.spinDeg = 15.0;
  seated.seat = seat;
  s.fixtures.push_back(seated);
  for (int k = 0; k < static_cast<int>(cam::OpType::Count); k++) {
    cam::Operation op;
    op.id = job.AllocateId();
    op.type = static_cast<cam::OpType>(k);
    op.name = "op " + std::to_string(k);
    // Only what the type has: the panel and the RPC refuse the rest, so
    // no file carries them.
    const auto has = [&](cam::OpField f) { return cam::OpTypeHas(op.type, f); };
    if (has(cam::OpField::Finish))
      op.finishTool = 2;
    if (has(cam::OpField::RestFromTool))
      op.restFromTool = 3;
    if (has(cam::OpField::Tabs))
      op.tabCount = 4;
    if (has(cam::OpField::Indexed))
      op.indexA = {0.0, 90.0};
    op.targets.push_back({part, {4, "Face2"}});
    // Every table number away from its default, so each is written.
    for (const cam::OpFieldRow& row : cam::OpFieldRows())
      if (cam::OpTypeHasRow(op.type, row) &&
          std::holds_alternative<ExprValue cam::Operation::*>(row.member))
        op.*std::get<ExprValue cam::Operation::*>(row.member) = 0.25;
    if (has(cam::OpField::ThreadPasses))
      op.threadPasses = 2;
    s.operations.push_back(op);
  }
  return job;
}

nlohmann::json Written(const cam::JobDocument& job, const std::string& tag) {
  const fs::path path =
      fs::temp_directory_path() /
      ("lobole_job_probe_" + tag + "_" + std::to_string(::getpid()) + ".json");
  REQUIRE(DocumentIO::SaveJob(job, path.string()).has_value());
  std::ifstream in(path);
  nlohmann::json j = nlohmann::json::parse(in);
  fs::remove(path);
  return j;
}

} // namespace

TEST_CASE("Every number a job writes is a slot or says why it is not",
          "[variables][cam][conventions]") {
  cam::JobDocument job = Everything();
  // (1) A sentinel through every slot lands where its path says.
  int n = 0;
  std::vector<std::pair<std::string, std::string>> sentinels;
  job.VisitExprs([&](const std::string&, const ExprSlot& slot) {
    const std::string text = "probe_" + std::to_string(n++);
    slot.Expr() = text;
    sentinels.push_back({slot.Path(), text});
  });
  REQUIRE(n > 60);
  const nlohmann::json j = Written(job, "w");
  std::set<std::string> visited;
  for (const auto& [path, text] : sentinels) {
    INFO(path);
    const nlohmann::json* number = exprprobe::At(j, path);
    CHECK((number != nullptr && number->is_number()));
    const nlohmann::json* expr = exprprobe::At(j, exprprobe::ExprPathOf(path));
    REQUIRE(expr != nullptr);
    CHECK(*expr == text);
    visited.insert(path);
  }
  // (2) Every number is a slot or listed.
  std::vector<std::string> numbers;
  exprprobe::Numbers(j, "", numbers);
  for (const std::string& path : numbers) {
    INFO(path);
    CHECK((visited.contains(path) ||
           exprprobe::Listed(kNotBindable, exprprobe::Generic(path))));
  }
  // And it reads back with every text where it was.
  const fs::path file =
      fs::temp_directory_path() /
      ("lobole_job_probe_rt_" + std::to_string(::getpid()) + ".json");
  REQUIRE(DocumentIO::SaveJob(job, file.string()).has_value());
  auto loaded = DocumentIO::LoadJob(file.string());
  fs::remove(file);
  REQUIRE(loaded.has_value());
  std::vector<std::pair<std::string, std::string>> back;
  (*loaded)->VisitExprs([&](const std::string&, const ExprSlot& slot) {
    back.push_back({slot.Path(), slot.Expr()});
  });
  CHECK(back == sentinels);
}

TEST_CASE("A job file's expression that is not text is refused, one that does "
          "not parse opens broken",
          "[variables][cam]") {
  cam::JobDocument job = Everything();
  job.GetSetups()[0].tools[0].diameter.expr = "cutter";
  nlohmann::json j = Written(job, "bad");
  const fs::path file =
      fs::temp_directory_path() /
      ("lobole_job_bad_" + std::to_string(::getpid()) + ".json");
  const auto load = [&](const nlohmann::json& body) {
    std::ofstream(file) << body.dump();
    auto r = DocumentIO::LoadJob(file.string());
    fs::remove(file);
    return r;
  };
  nlohmann::json notText = j;
  notText["cam"][0]["tools"][0]["diameter_expr"] = 5;
  CHECK_FALSE(load(notText).has_value());
  nlohmann::json unparsed = j;
  unparsed["cam"][0]["tools"][0]["diameter_expr"] = "(";
  auto opened = load(unparsed);
  REQUIRE(opened.has_value());
  CHECK(cam::EvaluateJobExprs(**opened, SymbolTable{}));
  CHECK_FALSE((*opened)->ExprProblems().empty());
}
