// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// Design requirements: the model, its saved form and its states
// (docs/requirements/README.md). Measuring goes through the RPC handlers
// and is tested over the RPC (tests/requirements_smoke.py); this pins what
// needs no app: the vocabulary and its refusals, the five states, the saved
// form in both places a project keeps it, and the old clearance assertions
// loading as requirements.

#include "TestPaths.h"
#include "core/Document.h"
#include "core/InputHash.h"
#include "core/Occurrence.h"
#include "core/Requirements.h"
#include "core/io/Serialization.h"
#include "remote/RemoteRequirements.h"
#include "ui/AppState.h"
#include "ui/DocumentSlots.h"
#include "ui/ProjectActions.h"
#include "ui/VariableActions.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>

using Catch::Matchers::ContainsSubstring;
using nlohmann::json;

namespace {

req::Requirement Mass(double lo, double hi) {
  req::Requirement r;
  r.id = 3;
  r.name = "mass";
  r.source = "the head must stay carryable";
  r.measure = {{"kind", "mass"}, {"document", 1}};
  r.bound = req::BoundFromJson({{"kind", "within"}, {"min", lo}, {"max", hi}});
  return r;
}

std::string Refusal(const json& measure, const json& bound) {
  try {
    req::Validate(measure, req::BoundFromJson(bound));
  } catch (const std::exception& e) {
    return e.what();
  }
  return "";
}

} // namespace

TEST_CASE("an unknown measure or bound is refused, naming the accepted ones",
          "[requirements]") {
  CHECK_THAT(Refusal({{"kind", "stress"}, {"document", 1}}, {{"kind", "pass"}}),
             ContainsSubstring("accepted: study_probe, study_result"));
  CHECK_THAT(Refusal({{"kind", "mass"}, {"document", 1}},
                     {{"kind", "under"}, {"value", 1}}),
             ContainsSubstring("accepted: at_most, at_least, within, pass"));
  CHECK_THAT(Refusal({{"kind", "mass"}, {"part", 1}},
                     {{"kind", "at_most"}, {"value", 1}}),
             ContainsSubstring("unknown key 'part' (accepted: document)"));
  CHECK_THAT(Refusal({{"kind", "mass"}}, {{"kind", "at_most"}, {"value", 1}}),
             ContainsSubstring("needs `document`"));
  // A number is bounded, a verdict passes.
  CHECK_THAT(Refusal({{"kind", "mass"}, {"document", 1}}, {{"kind", "pass"}}),
             ContainsSubstring("is a number"));
  CHECK_THAT(Refusal({{"kind", "check_literals"}},
                     {{"kind", "at_most"}, {"value", 0}}),
             ContainsSubstring("is a verdict"));
  CHECK_THAT(Refusal({{"kind", "mass"}, {"document", 1}},
                     {{"kind", "within"}, {"min", 5}, {"max", 1}}),
             ContainsSubstring("min is above max"));
  CHECK_THAT(Refusal({{"kind", "contact"},
                      {"document", 1},
                      {"a", 2},
                      {"b", 2},
                      {"expect", "touching"}},
                     {{"kind", "pass"}}),
             ContainsSubstring("cannot clear itself"));
  CHECK_THAT(Refusal({{"kind", "contact"},
                      {"document", 1},
                      {"a", 2},
                      {"b", 3},
                      {"expect", "near"}},
                     {{"kind", "pass"}}),
             ContainsSubstring("accepted: clear, touching, not_clashing"));
  CHECK_THAT(Refusal({{"kind", "study_probe"}, {"document", 1}, {"study", 2}},
                     {{"kind", "at_most"}, {"value", 1}}),
             ContainsSubstring("exactly one of the two"));
  CHECK(Refusal({{"kind", "study_probe"},
                 {"document", 1},
                 {"study", 2},
                 {"readings", {{"p", {{"face", "Face1"}}}}},
                 {"expression", "p_tx * 2"}},
                {{"kind", "at_most"}, {"value", "lens_max"}})
            .empty());
  // A bearing's bore is a number, over named occurrences.
  CHECK(Refusal({{"kind", "bearing_bore"},
                 {"document", 1},
                 {"occurrences", nlohmann::json::array({4, 5})}},
                {{"kind", "at_least"}, {"value", "shaft_d_min"}})
            .empty());
  CHECK_THAT(Refusal({{"kind", "bearing_bore"},
                      {"document", 1},
                      {"occurrences", nlohmann::json::array({4})}},
                     {{"kind", "pass"}}),
             ContainsSubstring("is a number"));
  CHECK_THAT(Refusal({{"kind", "bearing_bore"},
                      {"document", 1},
                      {"occurrences", nlohmann::json::array()}},
                     {{"kind", "at_least"}, {"value", 15}}),
             ContainsSubstring("non-empty list of bearing occurrence ids"));
  CHECK_THAT(Refusal({{"kind", "bearing_bore"},
                      {"document", 1},
                      {"occurrences", nlohmann::json::array({"6002"})}},
                     {{"kind", "at_least"}, {"value", 15}}),
             ContainsSubstring("occurrence ids"));
  CHECK_THAT(Refusal({{"kind", "bearing_bore"}, {"document", 1}},
                     {{"kind", "at_least"}, {"value", 15}}),
             ContainsSubstring("needs `occurrences`"));
  // A section's area, on ONE stated plane.
  CHECK(Refusal({{"kind", "section_area"},
                 {"document", 1},
                 {"of", "enclosed"},
                 {"base_plane", "xy"},
                 {"offset", 1200},
                 {"bodies", {"South wall", 4}}},
                {{"kind", "within"}, {"min", 1.1e8}, {"max", 1.2e8}})
            .empty());
  CHECK(Refusal({{"kind", "section_area"},
                 {"document", 1},
                 {"of", "material"},
                 {"origin", {0, 0, 5}},
                 {"normal", {0, 0, 1}}},
                {{"kind", "at_most"}, {"value", 10}})
            .empty());
  const json plane{{"kind", "section_area"},
                   {"document", 1},
                   {"of", "enclosed"},
                   {"base_plane", "xy"},
                   {"offset", 5}};
  const json atLeast{{"kind", "at_least"}, {"value", 1}};
  CHECK_THAT(Refusal(plane, {{"kind", "pass"}}),
             ContainsSubstring("is a number"));
  json m = plane;
  m["of"] = "inside";
  CHECK_THAT(Refusal(m, atLeast),
             ContainsSubstring("accepted: material, enclosed"));
  m = plane;
  m["base_plane"] = "top";
  CHECK_THAT(Refusal(m, atLeast), ContainsSubstring("accepted: xy, xz, yz"));
  m = plane;
  m.erase("offset");
  CHECK_THAT(Refusal(m, atLeast), ContainsSubstring("needs `offset`"));
  m = plane;
  m["origin"] = {0, 0, 0};
  CHECK_THAT(Refusal(m, atLeast), ContainsSubstring("exactly one"));
  m = plane;
  m.erase("of");
  CHECK_THAT(Refusal(m, atLeast), ContainsSubstring("needs `of`"));
  m = {{"kind", "section_area"},
       {"document", 1},
       {"of", "material"},
       {"origin", {0, 0, 0}}};
  CHECK_THAT(Refusal(m, atLeast), ContainsSubstring("`normal` is missing"));
  m["normal"] = {0, 0, 0};
  CHECK_THAT(Refusal(m, atLeast), ContainsSubstring("not zero"));
  m = plane;
  m["bodies"] = json::array();
  CHECK_THAT(Refusal(m, atLeast), ContainsSubstring("non-empty list"));
  m["bodies"] = {1.5};
  CHECK_THAT(Refusal(m, atLeast), ContainsSubstring("not 1.5"));
}

TEST_CASE("a measure key of the wrong type is refused by its own name",
          "[requirements]") {
  // nlohmann's own answer named neither the field nor what goes in it:
  // "type must be number, but is string" for document: "Garden shed".
  CHECK_THAT(Refusal({{"kind", "mass"}, {"document", "Garden shed"}},
                     {{"kind", "at_most"}, {"value", 1}}),
             ContainsSubstring("`measure.document` is the id of the "
                               "part|assembly it measures (list_parts), not "
                               "its name"));
  CHECK_THAT(Refusal({{"kind", "study_result"},
                      {"document", 1},
                      {"study", "Load case"},
                      {"quantity", "max_von_mises"}},
                     {{"kind", "at_most"}, {"value", 1}}),
             ContainsSubstring("`measure.study` is a study id"));
  CHECK_THAT(Refusal({{"kind", "check_seat"},
                      {"document", 1},
                      {"occurrences", {2}},
                      {"allow_unverified", "yes"}},
                     {{"kind", "pass"}}),
             ContainsSubstring("`measure.allow_unverified` is true or false"));
  CHECK_THAT(Refusal({{"kind", "section_area"},
                      {"document", 1},
                      {"of", "enclosed"},
                      {"base_plane", "xy"},
                      {"offset", "2340"}},
                     {{"kind", "at_least"}, {"value", 1}}),
             ContainsSubstring("`measure.offset` is a number"));
  CHECK_THAT(Refusal({{"kind", "check_literals"}, {"documents", {1, "x"}}},
                     {{"kind", "pass"}}),
             ContainsSubstring("`measure.documents` lists part ids"));
}

TEST_CASE("a requirement's state: only current evidence is a pass",
          "[requirements]") {
  req::Requirement r = Mass(100, 200);
  CHECK(req::StateOf(r, "", 42) == "unchecked");

  r.evidence.taken = true;
  r.evidence.revision = r.revision;
  r.evidence.inputHash = 42;
  r.evidence.value = 150;
  r.evidence.pass = req::Holds(r.bound, 150, 0, 100, 200);
  CHECK(req::StateOf(r, "", 42) == "pass");
  // The model moved: the hash of what it would measure is not the one it
  // measured.
  CHECK(req::StateOf(r, "", 43) == "stale");
  // A reference that is gone is unresolved whatever the evidence said.
  CHECK(req::StateOf(r, "document 1 is gone", 42) == "unresolved");
  // An edit leaves evidence of the old revision: unchecked, not pass.
  r.revision = 2;
  CHECK(req::StateOf(r, "", 42) == "unchecked");
  r.revision = 1;
  r.evidence.pass = req::Holds(r.bound, 250, 0, 100, 200);
  CHECK(req::StateOf(r, "", 42) == "fail");
  // A measure that could not be taken is not a fail and never a pass.
  r.evidence.error = "the study failed";
  CHECK(req::StateOf(r, "", 42) == "unresolved");
}

TEST_CASE("bounds hold as stated", "[requirements]") {
  const req::Bound most =
      req::BoundFromJson({{"kind", "at_most"}, {"value", 2}});
  CHECK(req::Holds(most, 2.0, 2.0, 0, 0));
  CHECK_FALSE(req::Holds(most, 2.0001, 2.0, 0, 0));
  const req::Bound least =
      req::BoundFromJson({{"kind", "at_least"}, {"value", "run_gap / 2"}});
  CHECK(least.valueExpr == "run_gap / 2");
  CHECK(req::Holds(least, 0.6, 0.6, 0, 0));
  CHECK_FALSE(req::Holds(least, 0.5, 0.6, 0, 0));
  CHECK_FALSE(req::Holds(most, std::nan(""), 2.0, 0, 0));
}

TEST_CASE("the saved form round-trips, evidence and expressions included",
          "[requirements]") {
  req::Requirement r = Mass(100, 200);
  r.bound = req::BoundFromJson(
      {{"kind", "within"}, {"min", "head_min_g"}, {"max", 200}});
  r.revision = 4;
  r.evidence.taken = true;
  r.evidence.value = 151.5;
  r.evidence.unit = "g";
  r.evidence.pass = true;
  r.evidence.inputHash = 0xfedcba9876543210ull;
  r.evidence.revision = 4;
  r.evidence.limitMin = 120;
  r.evidence.limitMax = 200;
  r.evidence.detail = {{"bodies", 3}};
  const json j = req::ToJson(r);
  CHECK(j.at("evidence").at("input_hash") == "0xfedcba9876543210");
  const req::Requirement back = req::FromJson(j);
  CHECK(req::ToJson(back) == j);
  CHECK(back.bound.minExpr == "head_min_g");
  CHECK(back.evidence.inputHash == r.evidence.inputHash);

  // A file naming a measure this build does not know does not load as
  // something else.
  json bad = j;
  bad["measure"]["kind"] = "massive";
  CHECK_THROWS_WITH(req::FromJson(bad), ContainsSubstring("massive"));
  json dup = json::array({j, j});
  CHECK_THROWS_WITH(req::ListFromJson(dup), ContainsSubstring("share id"));
}

TEST_CASE("a lone part's file carries the project's requirements",
          "[requirements][serialization]") {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "designer_req_lone";
  fs::create_directories(dir);
  const fs::path path = dir / "part.json";
  Document doc;
  doc.SetRequirementsRaw(req::ListToJson({Mass(1, 2)}));
  REQUIRE(DocumentIO::Save(doc, path.string()));
  auto back = DocumentIO::Load(path.string());
  REQUIRE(back);
  const auto list = req::ListFromJson((*back)->GetRequirementsRaw());
  REQUIRE(list.size() == 1);
  CHECK(list[0].name == "mass");
  CHECK(list[0].source == "the head must stay carryable");

  // Refused on load, naming it, rather than read as another measure.
  std::ifstream in(path);
  json j;
  in >> j;
  in.close();
  j["requirements"][0]["measure"]["kind"] = "massive";
  std::ofstream(path) << j.dump();
  auto refused = DocumentIO::Load(path.string());
  REQUIRE_FALSE(refused);
  CHECK_THAT(refused.error(), ContainsSubstring("massive"));
  fs::remove_all(dir);
}

TEST_CASE("an assembly's saved clearance assertions load as requirements",
          "[requirements][assembly]") {
  ui::AppState state;
  state.document = std::make_unique<Document>();
  state.commands = std::make_unique<CommandStack>();
  ui::EnsureParts(state);
  ui::AddAssembly(state, "Planetary");
  auto golden = DocumentIO::LoadAssembly(
      (std::filesystem::path(TestSourceDir()) /
       "tests/golden/assembly/v100-clearance-assertions.json")
          .string());
  REQUIRE(golden);
  REQUIRE((*golden)->GetAssertions().size() == 2);
  state.assembly = std::move(*golden);
  const uint64_t asmId = state.parts[state.activePart].id;

  ui::AdoptAssertionsAsRequirements(state);
  CHECK(state.assembly->GetAssertions().empty());
  REQUIRE(state.requirements.size() == 2);
  const req::Requirement& r = state.requirements[0];
  CHECK(r.name == "carrier to planet");
  CHECK(r.Kind() == "clearance");
  CHECK(r.Document() == asmId);
  CHECK(r.measure.at("a") == 1);
  CHECK(r.measure.at("b") == 2);
  CHECK(r.measure.at("mode") == "mesh");
  CHECK(r.bound.kind == "at_least");
  CHECK(r.bound.value == 2.0);
  CHECK(r.source == "room for the retaining clip");
  // Its old verdict was keyed on another hash: not carried as evidence.
  CHECK_FALSE(r.evidence.taken);
  CHECK(state.requirements[1].measure.at("mode") == "exact");
  CHECK(state.requirements[1].id != r.id);
  CHECK(state.nextRequirementId > state.requirements[1].id);
  CHECK(state.projectMetaDirty);
}

// ---- The `expression` measure (docs/requirements/README.md#expression) ----

namespace {

struct ReqApp {
  ui::AppState state;
  ReqApp() {
    state.document = std::make_unique<Document>();
    state.commands = std::make_unique<CommandStack>();
    ui::EnsureParts(state);
  }
  json Call(const std::string& method, const json& params) {
    remote::RpcParams p{params};
    auto r = remote::HandleRequirementsRpc(state, method, p);
    REQUIRE(r.has_value());
    return *r;
  }
  std::string Refusal(const std::string& method, const json& params) {
    try {
      Call(method, params);
    } catch (const std::exception& e) {
      return e.what();
    }
    return "";
  }
  json Row(uint64_t id) {
    for (const json& r :
         Call("list_requirements", json::object()).at("requirements"))
      if (r.at("id") == id)
        return r;
    FAIL("no requirement " << id);
    return {};
  }
  uint64_t Add(const std::string& name, const json& measure,
               const json& bound) {
    return Call("add_requirement",
                {{"name", name}, {"measure", measure}, {"bound", bound}})
        .at("id")
        .get<uint64_t>();
  }
  uint64_t PartId() const { return state.parts[state.activePart].id; }
};

std::string Finding(const json& row) {
  std::string out;
  for (const std::string& l : req::FindingLines(row))
    out += l + "\n";
  return out;
}

} // namespace

TEST_CASE("an expression requirement over the project's variables passes and "
          "fails as the values move",
          "[requirements]") {
  ReqApp app;
  REQUIRE(ui::SetProjectVariable(app.state, "od", "18", ""));
  REQUIRE(ui::SetProjectVariable(app.state, "bore", "15", ""));
  const uint64_t id =
      app.Add("wall", {{"kind", "expression"}, {"expression", "od - bore"}},
              {{"kind", "at_least"}, {"value", 4}});
  // Measured as it is stated, no Check: microseconds.
  json row = app.Row(id);
  CHECK(row.at("state") == "fail");
  CHECK(row.at("value") == 3.0);
  CHECK(row.at("detail").at("inputs") == json{{"bore", 15.0}, {"od", 18.0}});
  CHECK_THAT(
      Finding(row),
      ContainsSubstring("od - bore = 3 mm (bore 15, od 18), must be >= 4 mm"));
  // The edit that thickens the wall moves it, and says so.
  REQUIRE(ui::SetProjectVariable(app.state, "od", "20", ""));
  const json moved = remote::RefreshExpressionRequirements(app.state);
  REQUIRE(moved.size() == 1);
  CHECK(moved[0].at("id") == id);
  CHECK(moved[0].at("state") == "pass");
  CHECK(moved[0].at("value") == 5.0);
  CHECK(app.Row(id).at("state") == "pass");
  // A bound may be an expression in the same scope.
  const uint64_t rel =
      app.Add("bore under od", {{"kind", "expression"}, {"expression", "bore"}},
              {{"kind", "at_most"}, {"value", "od - 5"}});
  CHECK(app.Row(rel).at("state") == "pass");
  CHECK(app.Row(rel).at("limit") == 15.0);
}

TEST_CASE("a part-scope expression reads the part as its features see it, "
          "configurations included",
          "[requirements][configurations]") {
  ReqApp app;
  REQUIRE(ui::SetProjectVariable(app.state, "depth", "9", ""));
  Document& doc = *app.state.document;
  REQUIRE(doc.SetParameter("lip", "depth / 3"));
  Configuration& deep = doc.AddConfiguration("Deep");
  deep.parameterOverrides.push_back({"depth", "15"});
  const uint64_t deepId = deep.id;
  const uint64_t part = app.PartId();
  const uint64_t inPart = app.Add(
      "lip",
      {{"kind", "expression"}, {"expression", "lip"}, {"document", part}},
      {{"kind", "at_most"}, {"value", 4}});
  const uint64_t inProject =
      app.Add("depth", {{"kind", "expression"}, {"expression", "depth"}},
              {{"kind", "at_most"}, {"value", 12}});
  const uint64_t inDeep = app.Add("lip, deep",
                                  {{"kind", "expression"},
                                   {"expression", "lip"},
                                   {"document", part},
                                   {"configuration", deepId}},
                                  {{"kind", "at_most"}, {"value", 4}});
  CHECK(app.Row(inPart).at("value") == 3.0);
  CHECK(app.Row(inDeep).at("value") == 5.0); // Deep, though base is active
  CHECK(app.Row(inDeep).at("state") == "fail");
  CHECK(app.Row(inDeep).at("detail").at("configuration") == "Deep");
  // Deep active: the part sees its override, the project does not.
  REQUIRE(doc.SetActiveConfiguration(deepId));
  remote::RefreshExpressionRequirements(app.state);
  CHECK(app.Row(inPart).at("value") == 5.0);
  CHECK(app.Row(inPart).at("state") == "fail");
  CHECK(app.Row(inProject).at("value") == 9.0);
  // `configuration: 0` reads the base while Deep is active.
  const uint64_t inBase = app.Add("lip, base",
                                  {{"kind", "expression"},
                                   {"expression", "lip"},
                                   {"document", part},
                                   {"configuration", 0}},
                                  {{"kind", "at_most"}, {"value", 4}});
  CHECK(app.Row(inBase).at("value") == 3.0);
  // A configuration the part lacks is refused naming the ones it has.
  CHECK_THAT(app.Refusal("add_requirement",
                         {{"name", "x"},
                          {"measure",
                           {{"kind", "expression"},
                            {"expression", "lip"},
                            {"document", part},
                            {"configuration", 999}}},
                          {"bound", {{"kind", "at_most"}, {"value", 1}}}}),
             ContainsSubstring("its configurations: 0 (base), " +
                               std::to_string(deepId) + " (Deep)"));
  CHECK_THAT(app.Refusal("add_requirement",
                         {{"name", "x"},
                          {"measure",
                           {{"kind", "expression"},
                            {"expression", "lip"},
                            {"configuration", deepId}}},
                          {"bound", {{"kind", "at_most"}, {"value", 1}}}}),
             ContainsSubstring("needs `document`"));
}

TEST_CASE("a table cell edited makes a lookup's expression move",
          "[requirements][variables]") {
  ReqApp app;
  lookuptable::Table t;
  t.name = "bearings";
  t.columns = {"d", "D"};
  t.rows.push_back({"6002", {15, 32}});
  REQUIRE(ui::SetProjectTable(app.state, t).empty());
  REQUIRE(ui::SetProjectVariable(app.state, "od", "40", ""));
  const uint64_t id =
      app.Add("wall",
              {{"kind", "expression"},
               {"expression", "od - lookup(\"bearings\", 6002, \"D\")"}},
              {{"kind", "at_least"}, {"value", 4}});
  CHECK(app.Row(id).at("value") == 8.0);
  t.rows[0].values[1] = 37;
  REQUIRE(ui::SetProjectTable(app.state, t).empty());
  const json moved = remote::RefreshExpressionRequirements(app.state);
  REQUIRE(moved.size() == 1);
  CHECK(moved[0].at("state") == "fail");
  CHECK(app.Row(id).at("value") == 3.0);
  // A table it reads cannot be deleted from under it.
  CHECK_THAT(ui::RemoveProjectTable(app.state, "bearings"),
             ContainsSubstring("requirement 'wall'"));
  CHECK_THAT(app.Refusal("add_requirement",
                         {{"name", "x"},
                          {"measure",
                           {{"kind", "expression"},
                            {"expression", "lookup(\"screws\", 3, \"d\")"}}},
                          {"bound", {{"kind", "at_most"}, {"value", 1}}}}),
             ContainsSubstring("looks up table 'screws'"));
}

TEST_CASE("an expression's names are held to its scope",
          "[requirements][variables]") {
  ReqApp app;
  REQUIRE(ui::SetProjectVariable(app.state, "od", "18", ""));
  REQUIRE(ui::SetProjectVariable(app.state, "bore", "15", ""));
  const json bound{{"kind", "at_least"}, {"value", 4}};
  // Refused when stated: an unknown name names the variables there are, a
  // formula that does not compile says where.
  CHECK_THAT(
      app.Refusal(
          "add_requirement",
          {{"name", "x"},
           {"measure", {{"kind", "expression"}, {"expression", "od - bor"}}},
           {"bound", bound}}),
      ContainsSubstring("'bor', which is not a variable of the project "
                        "(its variables: bore, od)"));
  CHECK_THAT(app.Refusal("add_requirement",
                         {{"name", "x"},
                          {"measure",
                           {{"kind", "expression"}, {"expression", "od - "}}},
                          {"bound", bound}}),
             ContainsSubstring("does not read"));
  CHECK_THAT(
      app.Refusal("add_requirement",
                  {{"name", "x"},
                   {"measure", {{"kind", "expression"}, {"expression", "od"}}},
                   {"bound", {{"kind", "at_least"}, {"value", "odd"}}}}),
      ContainsSubstring("bound.value 'odd' reads 'odd'"));

  const uint64_t id = app.Add(
      "wall", {{"kind", "expression"}, {"expression", "od - bore"}}, bound);
  // A rename rewrites it; a delete is refused naming it.
  REQUIRE(ui::RenameProjectVariable(app.state, "bore", "seat_d"));
  CHECK(app.Row(id).at("measure").at("expression") == "od - seat_d");
  CHECK(app.Row(id).at("state") == "fail");
  REQUIRE(ui::UndoProjectVars(app.state));
  CHECK(app.Row(id).at("measure").at("expression") == "od - bore");
  std::vector<std::string> refs;
  CHECK_FALSE(ui::RemoveProjectVariable(app.state, "bore", &refs));
  REQUIRE(refs.size() == 1);
  CHECK(refs[0] == "requirement 'wall'");
  // A name that goes anyway (a merge) reads unresolved, naming it: never 0.
  std::erase_if(app.state.projectParameters,
                [](const Parameter& p) { return p.name == "bore"; });
  ui::SyncProjectScope(app.state);
  json row = app.Row(id);
  CHECK(row.at("state") == "unresolved");
  CHECK(row.at("note") == "variable 'bore' is not in the project");
  // An evaluation error is unresolved with the evaluator's message.
  REQUIRE(ui::SetProjectVariable(app.state, "bore", "0", ""));
  const uint64_t ratio = app.Add(
      "ratio", {{"kind", "expression"}, {"expression", "od / bore"}}, bound);
  row = app.Row(ratio);
  CHECK(row.at("state") == "unresolved");
  CHECK_THAT(row.at("note").get<std::string>(),
             ContainsSubstring("division by zero"));
  CHECK_FALSE(row.contains("value"));
}

TEST_CASE("a part-local variable a requirement reads is renamed with it and "
          "kept from deletion",
          "[requirements][variables]") {
  ReqApp app;
  Document& doc = *app.state.document;
  REQUIRE(doc.SetParameter("lip", "3"));
  const uint64_t id = app.Add("lip",
                              {{"kind", "expression"},
                               {"expression", "lip * 2"},
                               {"document", app.PartId()}},
                              {{"kind", "at_most"}, {"value", 8}});
  REQUIRE(ui::RenamePartVariable(app.state, doc, "lip", "rim"));
  CHECK(app.Row(id).at("measure").at("expression") == "rim * 2");
  std::vector<std::string> refs;
  CHECK_FALSE(ui::RemovePartVariable(app.state, doc, "rim", &refs));
  CHECK(refs == std::vector<std::string>{"requirement 'lip'"});
  // The part removed: the requirement reads unresolved, saying why.
  ui::AddPart(app.state, "Other");
  REQUIRE(ui::DeletePart(app.state, 0));
  CHECK(app.Row(id).at("state") == "unresolved");
  CHECK_THAT(app.Row(id).at("note").get<std::string>(),
             ContainsSubstring("is gone"));
}

TEST_CASE("the expression golden round-trips with its evidence",
          "[requirements][serialization]") {
  auto golden =
      DocumentIO::Load((std::filesystem::path(TestSourceDir()) /
                        "tests/golden/v189-expression-requirement.json")
                           .string());
  REQUIRE(golden);
  const auto list = req::ListFromJson((*golden)->GetRequirementsRaw());
  REQUIRE(list.size() == 2);
  CHECK(list[0].Kind() == "expression");
  CHECK(list[0].evidence.detail.at("inputs").at("od") == 18.0);
  CHECK(list[1].measure.at("configuration") == 7);
  CHECK(list[1].bound.maxExpr == "od + 12");
  CHECK(list[1].evidence.limitMax == 30.0);
  for (const req::Requirement& r : list)
    CHECK(req::ToJson(req::FromJson(req::ToJson(r))) == req::ToJson(r));
}
