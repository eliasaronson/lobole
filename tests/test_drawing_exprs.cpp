// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// A drawing's numbers that are expressions of the project's variables
// (docs/drawings/README.md#numbers-that-are-expressions): a view's scale
// bound to a variable follows it and the drawing changes, a scale that
// cannot stand is the view's error and keeps its last good value, a lone
// drawing refuses a variable it cannot see. The probe at the end is the
// plan's "nothing new arrives unbindable" for the drawing document.

#include "ExprProbe.h"
#include "TestIO.h"
#include "TestPaths.h"
#include "core/Command.h"
#include "core/Document.h"
#include "core/drawing/DrawingDocument.h"
#include "ui/AppActionsInternal.h" // SlotDrawing
#include "ui/AppState.h"
#include "ui/DocumentSlots.h"
#include "ui/VariableActions.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include <unistd.h>

using Catch::Matchers::ContainsSubstring;
namespace fs = std::filesystem;

TEST_CASE("A view's scale bound to a variable follows it", "[variables]"
                                                           "[drawing]") {
  ui::AppState state;
  state.document = std::make_unique<Document>();
  state.commands = std::make_unique<CommandStack>();
  ui::EnsureParts(state);
  const uint64_t part = state.parts.at(0).id;
  const size_t slot = ui::AddDrawing(state, "Sheet");
  REQUIRE(ui::SetProjectVariable(state, "s", "0.5", ""));
  DrawingDocument& dwg = *ui::SlotDrawing(state, slot);
  DrawingView v;
  v.name = "Front";
  v.sourceDocId = part;
  v.scale.expr = "s * 2";
  v.positionExpr = {"s * 100", ""};
  const uint64_t id = dwg.AddView(v);
  ui::SyncProjectScope(state);
  const auto view = [&]() -> const DrawingView& {
    return *ui::SlotDrawing(state, slot)->FindView(id);
  };
  CHECK(view().scale.value == 1.0);
  CHECK(view().position.x == 50.0);
  const uint64_t version = ui::SlotDrawing(state, slot)->Version();
  REQUIRE(ui::SetProjectVariable(state, "s", "0.25", ""));
  CHECK(view().scale.value == 0.5);
  CHECK(view().position.x == 25.0);
  CHECK(ui::SlotDrawing(state, slot)->Version() != version);
  // A scale that cannot stand: the view keeps its last good numbers.
  REQUIRE(ui::SetProjectVariable(state, "s", "0", ""));
  CHECK(view().scale.value == 0.5);
  CHECK(view().position.x == 25.0);
  const auto& problems = ui::SlotDrawing(state, slot)->ExprProblems();
  REQUIRE(problems.size() == 1);
  CHECK(problems[0].owner == "view 'Front'");
  CHECK(problems[0].id == id);
  CHECK_THAT(problems[0].error, ContainsSubstring("scale"));
}

TEST_CASE("A lone drawing refuses a variable it cannot see",
          "[variables][drawing]") {
  DrawingDocument dwg;
  DrawingView v;
  v.name = "Front";
  v.scale.expr = "s";
  dwg.AddView(v);
  CHECK(drawing::EvaluateExprs(dwg, SymbolTable{}));
  CHECK(dwg.GetViews()[0].scale.value == 1.0);
  REQUIRE(dwg.ExprProblems().size() == 1);
  CHECK_THAT(dwg.ExprProblems()[0].error, ContainsSubstring("'s'"));
}

// ---- The probe: nothing new arrives unbindable ------------------------------
//
// Every slot DrawingDocument::VisitExprs hands out is given a sentinel text,
// the drawing is written as a single file, and (1) each sentinel must land at
// its slot's path, as `<last key>_expr`, and (2) every number in the file
// must be either a visited slot or on the short list below, each with its
// reason.

namespace {

const exprprobe::Reasons kNotBindable = {
    {"version", "the format's"},
    {"views/*/id", "an id"},
    {"views/*/source", "an id"},
    {"views/*/base_view", "an id"},
    {"views/*/hidden/*", "an id"},
    {"views/*/configuration", "an id"},
    {"views/*/exploded_view", "an id"},
    {"views/*/detail/view", "an id"},
    {"dims/*/id", "an id"},
    {"dims/*/view", "an id"},
    {"dims/*/refs/**", "a dimension's references: ids and an end (0, 1, 2)"},
    {"dims/*/label_offset/*",
     "where a dimension's label sits, placed by a drag or by its own layout "
     "(AutoDimOffset): a hand's choice on the sheet, not a dimension"},
    {"notes/*/id", "an id"},
    {"notes/*/view", "an id"},
    {"notes/*/dim", "an id"},
    {"notes/*/leader/**", "a leader's reference: ids and an end"},
    {"notes/*/pos/*",
     "where a note stands, placed by a drag or laid out (layout_balloons, "
     "AutoNotePosition), which rewrite it"},
};

DrawingDocument Everything() {
  DrawingDocument d;
  DrawingView base;
  base.name = "Front";
  base.sourceDocId = 3;
  base.scale = 0.5;
  base.position = {100, 80};
  base.rotationDeg = 90.0;
  base.configuration = 4;
  base.hidden = {7};
  base.section.on = true;
  base.section.point = {1, 2, 3};
  base.section.label = "A";
  const uint64_t front = d.AddView(base);
  DrawingView child;
  child.name = "Top";
  child.sourceDocId = 3;
  child.baseViewId = front;
  child.explodedView = 9;
  d.AddView(child);
  DrawingView detail;
  detail.name = "Detail B";
  detail.sourceDocId = 3;
  detail.detail.on = true;
  detail.detail.parentViewId = front;
  detail.detail.center = {4, 5, 6};
  detail.detail.radius = 7.0;
  detail.detail.label = "B";
  d.AddView(detail);
  DrawingDim dim;
  dim.viewId = front;
  ProjRef r;
  r.viewId = front;
  r.edge = {2, "Edge1"};
  r.end = 1;
  dim.refs.push_back(r);
  dim.labelOffset = {3, 4};
  const uint64_t dimId = d.AddDim(dim);
  DrawingNote text;
  text.text = "Break all edges";
  text.pos = {10, 20};
  text.viewId = front;
  text.leader = r;
  d.AddNote(text);
  DrawingNote datum;
  datum.style = NoteStyle::Datum;
  datum.text = "A";
  datum.pos = {30, 40};
  d.AddNote(datum);
  DrawingNote fcf;
  fcf.style = NoteStyle::Fcf;
  fcf.fcf.symbol = GeoSymbol::Position;
  fcf.fcf.tolerance = 0.1;
  fcf.fcf.datums = {"A"};
  fcf.attachDim = dimId;
  d.AddNote(fcf);
  return d;
}

nlohmann::json Written(const DrawingDocument& d) {
  const fs::path path =
      fs::temp_directory_path() /
      ("lobole_dwg_probe_" + std::to_string(::getpid()) + ".json");
  REQUIRE(testio::SaveDrawing(d, path.string()));
  std::ifstream in(path);
  nlohmann::json j = nlohmann::json::parse(in);
  fs::remove(path);
  return j;
}

} // namespace

TEST_CASE("Every number a drawing writes is a slot or says why it is not",
          "[variables][drawing][conventions]") {
  DrawingDocument d = Everything();
  int n = 0;
  std::vector<std::pair<std::string, std::string>> sentinels;
  d.VisitExprs([&](const std::string&, const ExprSlot& slot) {
    const std::string text = "probe_" + std::to_string(n++);
    slot.Expr() = text;
    sentinels.push_back({slot.Path(), text});
  });
  REQUIRE(n > 15);
  const nlohmann::json j = Written(d);
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
  std::vector<std::string> numbers;
  exprprobe::Numbers(j, "", numbers);
  for (const std::string& path : numbers) {
    INFO(path);
    CHECK((visited.contains(path) ||
           exprprobe::Listed(kNotBindable, exprprobe::Generic(path))));
  }
  const fs::path file =
      fs::temp_directory_path() /
      ("lobole_dwg_probe_rt_" + std::to_string(::getpid()) + ".json");
  REQUIRE(testio::SaveDrawing(d, file.string()));
  auto loaded = testio::LoadDrawing(file.string());
  fs::remove(file);
  REQUIRE(loaded != nullptr);
  std::vector<std::pair<std::string, std::string>> back;
  loaded->VisitExprs([&](const std::string&, const ExprSlot& slot) {
    back.push_back({slot.Path(), slot.Expr()});
  });
  CHECK(back == sentinels);
}

TEST_CASE("The drawing expressions golden loads with its texts (vNEXT)",
          "[variables][drawing][golden]") {
  const fs::path golden = fs::path(TestSourceDir()) / "tests" / "golden" /
                          "drawing" / "vNEXT-drawing-expressions.json";
  REQUIRE(fs::exists(golden));
  auto dwg = testio::LoadDrawing(golden.string());
  REQUIRE(dwg != nullptr);
  std::vector<std::vector<std::string>> got;
  dwg->VisitExprs([&](const std::string& owner, const ExprSlot& slot) {
    if (slot.Bound())
      got.push_back({owner, slot.Path(), slot.Expr()});
  });
  const std::vector<std::vector<std::string>> want = {
      {"view 'Front'", "views/0/scale", "sheet_scale"},
      {"view 'Front'", "views/0/position/0", "margin * 2"},
      {"view 'Front'", "views/0/rotation_deg", "quarter"},
      {"view 'Front'", "views/0/section/point/2", "thickness / 2"},
      {"view 'Detail B'", "views/1/scale", "sheet_scale * 4"},
      {"view 'Detail B'", "views/1/detail/center/0", "margin / 6"},
      {"view 'Detail B'", "views/1/detail/radius_mm", "margin * 2 / 15"},
      {"note 4", "notes/1/fcf/tolerance_mm", "tol"},
  };
  CHECK(got == want);
  // Evaluated over the project's variables, each lands where its text says.
  const SymbolTable vars = {{"sheet_scale", 0.25},
                            {"margin", 75.0},
                            {"quarter", 90.0},
                            {"thickness", 12.0},
                            {"tol", 0.05}};
  (void)drawing::EvaluateExprs(*dwg, vars);
  CHECK(dwg->ExprProblems().empty());
  CHECK(dwg->GetViews()[0].scale.value == 0.25);
  CHECK(dwg->GetViews()[0].position.x == 150.0);
  CHECK(dwg->GetViews()[1].detail.radius.value == 10.0);
  CHECK(dwg->GetNotes()[1].fcf.tolerance.value == 0.05);
}
