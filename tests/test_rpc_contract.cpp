// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "remote/InspectionContracts.h"
#include "remote/RemoteHelpers.h"
#include "remote/RemoteServerInternal.h"
#include "remote/RpcContract.h"

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>
#include <vector>

// The typed MethodDoc column (docs/scripting/rpc.md#typed-contracts): what
// it parses to, the JSON Schema it publishes, and what the one validator
// says about a request it refuses.

using nlohmann::json;
namespace contract = remote::contract;

namespace {

const std::vector<std::string> kMates = {"fastened", "revolute", "slider"};
const contract::Vocabulary kMateVocab = {kMates, "a gear is a relation"};

const contract::Vocabulary* Vocab(std::string_view name) {
  return name == "mate_types" ? &kMateVocab : nullptr;
}

json Schema(const char* column) {
  const auto c = contract::ParseColumn(column);
  REQUIRE(c.has_value());
  return contract::RequestSchema(*c, Vocab);
}

// The diagnostic of the refusal `params` earns, or null when accepted.
json Refusal(const json& schema, const json& params) {
  try {
    remote::ValidateInspectionValue(schema, params, "m");
  } catch (const remote::ContractError& e) {
    json d = e.diagnostic;
    d["message"] = e.what();
    return d;
  }
  return nullptr;
}

} // namespace

TEST_CASE("an untyped column is not a contract, an empty one is complete",
          "[remote][contract]") {
  CHECK_FALSE(contract::ParseColumn("a, b").has_value());
  CHECK_FALSE(contract::ParseColumn("*").has_value());
  const auto none = contract::ParseColumn("");
  REQUIRE(none.has_value());
  CHECK(none->params.empty());
  CHECK(Schema("") == json{{"type", "object"},
                           {"properties", json::object()},
                           {"required", json::array()},
                           {"additionalProperties", false}});
  CHECK(contract::ParamNames("a?:int, b:string|null") ==
        std::vector<std::string>{"a", "b"});
  CHECK(contract::ParamNames("a, b") == std::vector<std::string>{"a", "b"});
  CHECK(contract::ParamNames("*").empty());
}

TEST_CASE("the column grammar renders the schema subset",
          "[remote][contract]") {
  const json s = Schema("id:int>=1, offset?:number>0<=10, part?:string|int, "
                        "kind?:'a'|'b', type?:@mate_types, at?:number[3], "
                        "path?:int[]|null, spec:object, anything?:any, "
                        "expect_revision?:revision");
  CHECK(s["required"] == json{"id", "spec"});
  CHECK(s["properties"]["id"] == json{{"type", "integer"}, {"minimum", 1.0}});
  CHECK(s["properties"]["offset"] ==
        json{{"type", "number"}, {"exclusiveMinimum", 0.0}, {"maximum", 10.0}});
  CHECK(s["properties"]["part"] ==
        json{{"anyOf", {{{"type", "string"}}, {{"type", "integer"}}}}});
  CHECK(s["properties"]["kind"] ==
        json{{"type", "string"}, {"enum", {"a", "b"}}});
  CHECK(s["properties"]["type"]["enum"] == json(kMates));
  CHECK(s["properties"]["at"] == json{{"type", "array"},
                                      {"items", {{"type", "number"}}},
                                      {"minItems", 3},
                                      {"maxItems", 3}});
  CHECK(s["properties"]["path"]["anyOf"][1] == json{{"type", "null"}});
  CHECK(s["properties"]["spec"] == json{{"type", "object"}});
  CHECK(s["properties"]["anything"] == json::object());
  CHECK(s["properties"]["expect_revision"] == contract::RevisionSchema());
}

TEST_CASE("a malformed column is refused naming the key",
          "[remote][contract]") {
  const auto refuses = [](const char* column, const std::string& fragment) {
    try {
      const auto c = contract::ParseColumn(column);
      if (c)
        (void)contract::RequestSchema(*c, Vocab);
    } catch (const std::invalid_argument& e) {
      INFO(e.what());
      CHECK(std::string(e.what()).find(fragment) != std::string::npos);
      return;
    }
    FAIL("accepted: " << column);
  };
  refuses("a:int, b", "'b': untyped");
  refuses("a:integer", "unknown type 'integer'");
  refuses("a:string>0", "a range applies to int or number");
  refuses("a:int, a?:int", "listed twice");
  refuses("a:'open", "unterminated literal");
  refuses("a:int[x]", "count");
  refuses("a:@nope", "no vocabulary 'nope'");
  refuses("a:int)", "unexpected");
}

TEST_CASE("a refusal names the key, what it must be and what it got",
          "[remote][contract]") {
  const json s = Schema("id:int>=1, kind?:'exact'|'mesh', type?:@mate_types, "
                        "part?:string|int, at?:number[3]");
  CHECK(Refusal(s, {{"id", 3}, {"kind", "mesh"}, {"part", "P"}}).is_null());
  CHECK(Refusal(s, {{"id", 3.0}}).is_null()); // an integral number is an int

  const json missing = Refusal(s, json::object());
  CHECK(missing["code"] == "missing_argument");
  CHECK(missing["param"] == "id");
  CHECK(missing["message"] ==
        "m: missing required parameter 'id' (an integer >= 1)");

  const json type = Refusal(s, {{"id", "3"}});
  CHECK(type["code"] == "invalid_argument");
  CHECK(type["path"] == "/params/id");
  CHECK(type["observed"] == "3");
  CHECK(type["message"] == "m: 'id' must be an integer >= 1; got \"3\"");

  const json range = Refusal(s, {{"id", 0}});
  CHECK(range["code"] == "invalid_argument");
  CHECK(range["param"] == "id");

  const json fraction = Refusal(s, {{"id", 1.5}});
  CHECK(fraction["param"] == "id");

  const json enumMiss = Refusal(s, {{"id", 1}, {"kind", "fast"}});
  CHECK(enumMiss["accepted"] == json{"exact", "mesh"});
  CHECK(enumMiss["message"] ==
        "m: 'kind' must be one of: exact, mesh; got \"fast\"");

  const json vocab = Refusal(s, {{"id", 1}, {"type", "revolut"}});
  CHECK(vocab["accepted"] == json(kMates));
  // The vocabulary's remedy ends the sentence.
  CHECK(vocab["message"] == "m: 'type' must be one of: fastened, revolute, "
                            "slider; got \"revolut\". a gear is a relation");

  const json either = Refusal(s, {{"id", 1}, {"part", true}});
  CHECK(either["message"] == "m: 'part' must be a string or an integer; got "
                             "true");

  const json shape = Refusal(s, {{"id", 1}, {"at", {1, 2}}});
  CHECK(shape["param"] == "at");
  CHECK(shape["message"] ==
        "m: 'at' must be an array of 3 × a number; got [1,2]");

  const json unknown = Refusal(s, {{"id", 1}, {"typo", 1}});
  CHECK(unknown["code"] == "unknown_argument");
  CHECK(unknown["accepted"] == json{"at", "id", "kind", "part", "type"});
}

// Portable schemas (docs/scripting/mcp.md#portable-schemas): some model
// vendors refuse a whole tool list over one `"type": [..]`, so every
// alternative is an `anyOf`. Every request and result schema describe
// publishes is walked; a planted type array must fail the walk.
namespace {

void TypeArrays(const json& schema, const std::string& where,
                std::vector<std::string>& out) {
  if (schema.is_object()) {
    if (const auto t = schema.find("type"); t != schema.end() && t->is_array())
      out.push_back(where +
                    (schema.contains("enum") ? " (beside an enum)" : ""));
    for (auto it = schema.begin(); it != schema.end(); ++it)
      TypeArrays(it.value(), where + "/" + it.key(), out);
  } else if (schema.is_array()) {
    for (size_t i = 0; i < schema.size(); i++)
      TypeArrays(schema[i], where + "[" + std::to_string(i) + "]", out);
  }
}

} // namespace

TEST_CASE("every published schema writes alternatives as anyOf",
          "[remote][contract]") {
  std::vector<std::string> bad;
  size_t schemas = 0;
  for (const auto& table : AllMethodTables())
    for (const remote::MethodDoc& d : table)
      if (const json* s = RequestSchemaOf(d.name)) {
        schemas++;
        TypeArrays(*s, std::string(d.name) + ".request", bad);
      }
  const auto& inspection = remote::InspectionContracts().at("methods");
  for (auto it = inspection.begin(); it != inspection.end(); ++it) {
    schemas++;
    TypeArrays(it.value().at("result"), it.key() + ".result", bad);
  }
  CHECK(schemas > 50);
  INFO("type arrays: " << json(bad).dump());
  CHECK(bad.empty());

  // The column grammar's own alternatives are anyOf...
  std::vector<std::string> own;
  TypeArrays(Schema("path?:string|null"), "column", own);
  CHECK(own.empty());
  // ...and the walk sees a planted array, an enum beside it too.
  std::vector<std::string> planted;
  TypeArrays(json::parse(R"({"properties": {"a": {"type": ["string", "null"]},
                         "b": {"type": ["string", "null"], "enum": ["x"]}}})"),
             "planted", planted);
  CHECK(planted ==
        std::vector<std::string>{"planted/properties/a",
                                 "planted/properties/b (beside an enum)"});
}
