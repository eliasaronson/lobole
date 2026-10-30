// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// A method's typed request contract, declared in its MethodDoc `params`
// column (docs/scripting/rpc.md#typed-contracts). An untyped column is a bare
// key list and stays what it was; a column whose keys carry `:type` is a
// contract, which the dispatcher validates before the handler runs and
// `describe` publishes as a JSON Schema.
//
//   "feature_id?:int>=1, feature:object, type?:@mate_types, scope?:'a'|'b'"
//
// Pure logic, in designer_core, so the grammar is unit-tested
// (tests/test_rpc_contract.cpp). tools/method_doc_scan.py reads the same
// grammar for the generated SDK stubs; rpc_contracts_smoke holds the two
// readings equal.
namespace remote::contract {

enum class Kind {
  Int,
  Number,
  Bool,
  String,
  Object,
  Null,
  Any,
  Revision, // the {session, part_id, instance, version} a snapshot carries
  Literal,  // 'text'
  Vocab,    // @name: one of describe's published vocabularies
};

struct Alt {
  Kind kind = Kind::Any;
  bool array = false;
  int length = -1;  // T[N]; -1 for T[]
  std::string text; // the literal, or the vocabulary's name
  std::optional<double> min, max;
  bool minExclusive = false, maxExclusive = false;
};

struct Param {
  std::string name;
  bool required = true;
  // `key^group`: the key belongs to a choice, of which exactly one key is
  // given (a cam placed by `face` OR `frame`); empty for an ordinary key.
  std::string oneOf;
  std::vector<Alt> alts;
  std::string type; // as written, after the ':'
};

struct Contract {
  std::vector<Param> params;
  const Param* Find(std::string_view name) const;
};

// The contract of a typed column; an empty one for "" (a method that takes
// nothing is fully typed already); nullopt for an untyped column (no key
// carries a ':') or "*". Throws
// std::invalid_argument naming the key and the problem for a malformed one,
// including a column that types some keys and not others.
std::optional<Contract> ParseColumn(std::string_view column);

// The plain key names of any column, typed or not ("*" gives none).
std::vector<std::string> ParamNames(std::string_view column);

// A vocabulary's values, and the remedy a refusal adds after them when the
// usual mistake has one ("a gear is a RELATION: see add_relation"). Rendered
// as the schema's `description`, which the refusal's sentence ends with.
struct Vocabulary {
  std::vector<std::string> values;
  std::string hint;
};

// A vocabulary by name, or nullptr when there is no such vocabulary. The
// dispatcher's resolver reads the same tables `describe` publishes.
using VocabResolver = std::function<const Vocabulary*(std::string_view)>;

// The contract as the JSON Schema subset ValidateInspectionValue enforces:
// {type: object, properties, required, additionalProperties: false}. Throws
// std::invalid_argument for a vocabulary the resolver does not know.
nlohmann::json RequestSchema(const Contract& contract,
                             const VocabResolver& vocab);

// The schema of `expect_revision` (Kind::Revision).
nlohmann::json RevisionSchema();

} // namespace remote::contract
