// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// Project tables and lookup() (docs/variables/README.md#tables): the
// expression form, the refusals that name what exists, the table's own
// validation and JSON, a document that carries tables evaluating its
// lookups, the file round trip, and a project's tables reaching every part.
#include "core/Document.h"
#include "core/Expression.h"
#include "core/Feature.h"
#include "core/LookupTable.h"
#include "core/io/Serialization.h"
#include "ui/AppState.h"
#include "ui/DocumentSlots.h"
#include "ui/VariableActions.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;

namespace {

lookuptable::Table Bearings() {
  lookuptable::Table t;
  t.name = "bearings";
  t.columns = {"d", "D", "B"};
  t.rows = {
      {"6000", {10, 26, 8}}, {"6002", {15, 32, 9}}, {"6200", {10, 30, 9}}};
  return t;
}

lookuptable::Table Screws() {
  lookuptable::Table t;
  t.name = "screws";
  t.columns = {"clearance", "head"};
  t.rows = {{"M4", {4.5, 7}}, {"M6", {6.6, 10}}};
  return t;
}

SymbolTable Cells() {
  SymbolTable s;
  lookuptable::AddCellSymbols({Bearings(), Screws()}, s);
  s["size"] = 6002;
  return s;
}

std::expected<double, ExprError> Eval(const std::string& text) {
  return ExpressionEngine::Default().EvaluateText(text, Cells());
}

} // namespace

TEST_CASE("lookup reads a table cell by key and column",
          "[expression][tables]") {
  const auto v = Eval("lookup(\"bearings\", 6002, \"D\")");
  REQUIRE(v.has_value());
  CHECK(*v == 32.0);
  // Single quotes, a string key, a key from an expression, and arithmetic
  // around it.
  CHECK(*Eval("lookup('bearings', '6200', 'd')") == 10.0);
  CHECK(*Eval("lookup(\"bearings\", size, \"B\")") == 9.0);
  CHECK(*Eval("lookup(\"bearings\", 6000 + 2, \"B\")") == 9.0);
  CHECK(*Eval("lookup(\"screws\", \"M6\", \"clearance\") / 2 + 1") ==
        6.6 / 2 + 1);
  // A numeric key has one spelling: 6002.0 is row 6002.
  CHECK(*Eval("lookup(\"bearings\", 6002.0, \"d\")") == 15.0);
  CHECK(lookuptable::NumericKey(6002.0) == "6002");
  CHECK(lookuptable::NumericKey(6.35) == "6.35");
  CHECK(lookuptable::NumericKey(-0.0) == "0");

  // The key's variable is a dependency; the table and column are not.
  const auto c =
      ExpressionEngine::Default().Compile("lookup(\"bearings\", size, \"D\")");
  REQUIRE(c.has_value());
  CHECK((*c)->Dependencies() == std::set<std::string>{"size"});
  // And a rename leaves the quoted names alone.
  CHECK(ExpressionEngine::Default().RenameIdentifier(
            "lookup(\"D\", D, \"D\")", "D", "E") == "lookup(\"D\", E, \"D\")");
  // `lookup` is reserved: no variable may take its name.
  CHECK_FALSE(ExpressionEngine::Default().IsValidParameterName("lookup"));
}

TEST_CASE("lookup refuses a miss naming what exists", "[expression][tables]") {
  const auto table = Eval("lookup(\"bearing\", 6002, \"D\")");
  REQUIRE_FALSE(table.has_value());
  CHECK_THAT(table.error().message,
             ContainsSubstring("unknown table 'bearing'"));
  CHECK_THAT(table.error().message,
             ContainsSubstring("the tables are bearings and screws"));

  const auto key = Eval("lookup(\"bearings\", 6003, \"D\")");
  REQUIRE_FALSE(key.has_value());
  CHECK_THAT(key.error().message,
             ContainsSubstring("has no row '6003' — its keys are 6000, 6002 "
                               "and 6200"));

  const auto column = Eval("lookup(\"bearings\", 6002, \"r\")");
  REQUIRE_FALSE(column.has_value());
  CHECK_THAT(column.error().message,
             ContainsSubstring("has no column 'r' — its columns are B, D and "
                               "d"));

  const auto none =
      ExpressionEngine::Default().EvaluateText("lookup(\"t\", 1, \"c\")", {});
  REQUIRE_FALSE(none.has_value());
  CHECK_THAT(none.error().message, ContainsSubstring("has no tables"));

  // Quoted names belong to a lookup and nowhere else; its table and column
  // must be quoted, since a bare name is a variable.
  for (const char* bad :
       {"\"bearings\"", "1 + \"x\"", "max(\"a\", 2)",
        "lookup(bearings, 6002, \"D\")", "lookup(\"bearings\", 6002, D)",
        "lookup(\"bearings\", 6002)", "lookup(\"bearings, 6002, \"D\")"}) {
    INFO(bad);
    CHECK_FALSE(ExpressionEngine::Default().Compile(bad).has_value());
  }
}

TEST_CASE("A table validates and round-trips through JSON", "[tables]") {
  CHECK(lookuptable::Validate(Bearings()).empty());
  lookuptable::Table t = Bearings();
  t.rows.push_back({"6002", {1, 2, 3}});
  CHECK_THAT(lookuptable::Validate(t), ContainsSubstring("two rows keyed"));
  t = Bearings();
  t.rows[0].values.pop_back();
  CHECK_THAT(lookuptable::Validate(t), ContainsSubstring("2 values for 3"));
  t = Bearings();
  t.columns[1] = "d";
  CHECK_THAT(lookuptable::Validate(t), ContainsSubstring("twice"));
  t = Bearings();
  t.name = "6 bearings";
  CHECK_THAT(lookuptable::Validate(t), ContainsSubstring("is not"));

  const nlohmann::json j = lookuptable::ToJson(Bearings());
  // A numeric key is written as the number.
  CHECK(j.at("rows")[1].at("key") == 6002.0);
  std::string why;
  const auto back = lookuptable::FromJson(j, &why);
  INFO(why);
  REQUIRE(back.has_value());
  CHECK(*back == Bearings());
  const auto screws =
      lookuptable::FromJson(lookuptable::ToJson(Screws()), &why);
  REQUIRE(screws.has_value());
  CHECK(screws->rows[0].key == "M4");

  CHECK_THROWS_WITH(
      lookuptable::ListFromJson(nlohmann::json::array(
          {lookuptable::ToJson(Bearings()), lookuptable::ToJson(Bearings())})),
      ContainsSubstring("two tables are named 'bearings'"));
  nlohmann::json bad = lookuptable::ToJson(Bearings());
  bad["rows"][0]["values"][0] = "ten";
  CHECK_FALSE(lookuptable::FromJson(bad, &why).has_value());
  CHECK_THAT(why, ContainsSubstring("not a number"));
}

TEST_CASE("A document's lookups drive its geometry and survive a save",
          "[tables][serialization]") {
  Document doc;
  doc.SetTablesRaw({Bearings()});
  doc.SetParameter("size", "6002", "");
  doc.SetParameter("od", "lookup(\"bearings\", size, \"D\")", "");
  REQUIRE(doc.FindParameter("od") != nullptr);
  CHECK(doc.FindParameter("od")->error.empty());
  CHECK(doc.FindParameter("od")->value == 32.0);
  auto box = std::make_unique<BoxFeature>("Box", glm::dvec3(0), 1.0, 1.0, 1.0);
  box->widthExpr = "lookup(\"bearings\", 6200, \"D\")";
  doc.AddFeature(std::move(box));
  REQUIRE(doc.Regenerate());
  CHECK_THAT(doc.GetBodies().front().shape.Volume(), WithinAbs(30.0, 1e-6));

  const auto path =
      std::filesystem::temp_directory_path() / "designer_lookup_table.json";
  REQUIRE(DocumentIO::Save(doc, path.string()).has_value());
  auto loaded = DocumentIO::Load(path.string());
  std::filesystem::remove(path);
  REQUIRE(loaded.has_value());
  REQUIRE((*loaded)->GetTables().size() == 1);
  CHECK((*loaded)->GetTables()[0] == Bearings());
  CHECK((*loaded)->FindParameter("od")->value == 32.0);
  CHECK_THAT((*loaded)->GetBodies().front().shape.Volume(),
             WithinAbs(30.0, 1e-6));
}

TEST_CASE("A project table reaches every part and undoes with the variables",
          "[tables][actions]") {
  ui::AppState state;
  state.document = std::make_unique<Document>();
  state.commands = std::make_unique<CommandStack>();
  ui::EnsureParts(state);
  REQUIRE(ui::SetProjectTable(state, Bearings()).empty());
  REQUIRE(ui::SetProjectVariable(state, "od",
                                 "lookup(\"bearings\", 6002, \"D\")", ""));
  CHECK(ui::FindProjectParameter(state, "od")->value == 32.0);

  // An edit to the table re-evaluates what reads it.
  lookuptable::Table t = Bearings();
  t.rows[1].values[1] = 33.0;
  REQUIRE(ui::SetProjectTable(state, t).empty());
  CHECK(ui::FindProjectParameter(state, "od")->value == 33.0);
  // ... and undoes on the project's undo, which restores the table.
  ui::GlobalUndo(state);
  CHECK(state.projectTables.at(0) == Bearings());
  CHECK(ui::FindProjectParameter(state, "od")->value == 32.0);

  // A malformed table is refused and changes nothing.
  lookuptable::Table bad = Bearings();
  bad.rows[0].values.clear();
  CHECK_THAT(ui::SetProjectTable(state, bad), ContainsSubstring("0 values"));
  CHECK(state.projectTables.at(0) == Bearings());

  // Deleting a table something reads is refused, naming the reader.
  CHECK_THAT(ui::RemoveProjectTable(state, "bearings"),
             ContainsSubstring("project variable 'od'"));
  CHECK_THAT(ui::RemoveProjectTable(state, "nuts"),
             ContainsSubstring("the tables are bearings"));
  REQUIRE(ui::RemoveProjectVariable(state, "od", nullptr));
  CHECK(ui::RemoveProjectTable(state, "bearings").empty());
  CHECK(state.projectTables.empty());
}
