// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "remote/InspectionContracts.generated.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace remote {

// A refusal with a machine-readable companion: `what()` is the sentence a
// person reads, `diagnostic` is {operation, code, path, param?, expected,
// observed, accepted?} (docs/scripting/rpc.md#structured-errors). `param` is
// the top-level key the path runs through; `accepted` the values an enum, a
// vocabulary or a key list takes. python/lobole/contracts.py builds the
// same object client-side, field for field.
class ContractError : public std::runtime_error {
public:
  nlohmann::json diagnostic;
  ContractError(const std::string& operation, const std::string& code,
                const std::string& path, const nlohmann::json& expected,
                const nlohmann::json& observed, const std::string& message = "")
      : std::runtime_error(message.empty()
                               ? operation + ": " + code + " at " + path +
                                     "; expected " + expected.dump()
                               : message),
        diagnostic{{"operation", operation},
                   {"code", code},
                   {"path", path},
                   {"expected", expected},
                   {"observed", observed}} {
    if (path.rfind("/params/", 0) == 0) {
      const std::string rest = path.substr(8);
      diagnostic["param"] = rest.substr(0, rest.find('/'));
    }
  }
  ContractError(const std::string& message, nlohmann::json diag)
      : std::runtime_error(message), diagnostic(std::move(diag)) {}
};

inline const nlohmann::json& InspectionContracts() {
  static const auto contracts = nlohmann::json::parse(kInspectionContractJson);
  return contracts;
}

// The values a schema accepts by name: an enum's, or every enum of an anyOf.
inline nlohmann::json SchemaAccepted(const nlohmann::json& schema) {
  nlohmann::json out = nlohmann::json::array();
  if (schema.contains("enum"))
    for (const auto& v : schema["enum"])
      out.push_back(v);
  if (schema.contains("anyOf"))
    for (const auto& alt : schema["anyOf"])
      if (alt.contains("enum"))
        for (const auto& v : alt["enum"])
          out.push_back(v);
  return out;
}

// A schema in words, for the refusal's sentence: "an integer >= 1", "one of:
// exact, mesh", "a string or an integer".
inline std::string DescribeSchema(const nlohmann::json& schema) {
  const auto num = [](const nlohmann::json& v) {
    const double d = v.get<double>();
    if (std::floor(d) == d && std::fabs(d) < 1e18)
      return std::to_string(static_cast<long long>(d));
    std::string s = std::to_string(d);
    s.erase(s.find_last_not_of('0') + 1);
    return s;
  };
  if (schema.contains("anyOf")) {
    std::string s;
    for (size_t i = 0; i < schema["anyOf"].size(); i++)
      s += (i ? " or " : "") + DescribeSchema(schema["anyOf"][i]);
    return s;
  }
  if (schema.contains("enum")) {
    std::string s = schema["enum"].size() == 1 ? "\"" : "one of: ";
    size_t shown = 0;
    for (const auto& v : schema["enum"]) {
      if (shown == 12) {
        s += ", +" + std::to_string(schema["enum"].size() - shown) + " more";
        break;
      }
      s += (shown++ ? ", " : "") +
           (v.is_string() ? v.get<std::string>() : v.dump());
    }
    return schema["enum"].size() == 1 ? s + "\"" : s;
  }
  if (!schema.contains("type"))
    return "any value";
  const std::string type = schema["type"];
  std::string s = type == "integer"   ? "an integer"
                  : type == "number"  ? "a number"
                  : type == "string"  ? "a string"
                  : type == "boolean" ? "true or false"
                  : type == "object"  ? "an object"
                  : type == "null"    ? "null"
                  : type == "array"   ? "an array"
                                      : type;
  if (schema.contains("minimum"))
    s += " >= " + num(schema["minimum"]);
  if (schema.contains("exclusiveMinimum"))
    s += " > " + num(schema["exclusiveMinimum"]);
  if (schema.contains("maximum"))
    s += " <= " + num(schema["maximum"]);
  if (schema.contains("exclusiveMaximum"))
    s += " < " + num(schema["exclusiveMaximum"]);
  if (schema.contains("minLength"))
    s += " of at least " + num(schema["minLength"]) + " character(s)";
  if (type == "object" && schema.contains("properties")) {
    std::string keys;
    for (auto it = schema["properties"].begin();
         it != schema["properties"].end(); ++it)
      keys += (keys.empty() ? "" : ", ") + it.key();
    s += " {" + keys + "}";
  }
  if (type == "array") {
    const bool lo = schema.contains("minItems"),
               hi = schema.contains("maxItems");
    if (lo && hi && schema["minItems"] == schema["maxItems"])
      s += " of " + num(schema["minItems"]);
    else if (lo || hi)
      s += " of " + (lo ? num(schema["minItems"]) : std::string("0")) + ".." +
           (hi ? num(schema["maxItems"]) : std::string("any"));
    if (schema.contains("items"))
      s += (lo || hi ? " × " : " of ") + DescribeSchema(schema["items"]);
    if (schema.value("uniqueItems", false))
      s += ", no repeats";
  }
  return s;
}

inline bool SchemaAccepts(const nlohmann::json& schema,
                          const nlohmann::json& value);

// The supported JSON Schema subset, from one source per family: the
// inspection contracts (api/inspection-contracts.json) and the typed MethodDoc
// rows (remote/RpcContract.h). Domain checks (ownership, freshness, geometry)
// stay in the handler. python/lobole/contracts.py is the same walk in the
// same order, so a client preflight and the server name the same fault.
inline void ValidateInspectionValue(const nlohmann::json& schema,
                                    const nlohmann::json& value,
                                    const std::string& operation,
                                    const std::string& path = "/params") {
  const auto shown = [](const nlohmann::json& v) {
    std::string s = v.dump();
    return s.size() > 80 ? s.substr(0, 77) + "..." : s;
  };
  const auto where = [&](const std::string& at) {
    return at == "/params" ? std::string("its params")
                           : "'" + at.substr(8) + "'";
  };
  const auto fail = [&](const nlohmann::json& expected) {
    // A vocabulary's remedy for the usual mistake rides as `description`.
    std::string hint = expected.value("description", "");
    if (expected.contains("anyOf"))
      for (const auto& alt : expected["anyOf"])
        if (hint.empty())
          hint = alt.value("description", "");
    ContractError e(operation, "invalid_argument", path, expected, value,
                    operation + ": " + where(path) + " must be " +
                        DescribeSchema(expected) + "; got " + shown(value) +
                        (hint.empty() ? "" : ". " + hint));
    if (auto accepted = SchemaAccepted(expected); !accepted.empty())
      e.diagnostic["accepted"] = accepted;
    throw e;
  };
  if (schema.contains("anyOf")) {
    for (const auto& alt : schema["anyOf"])
      if (SchemaAccepts(alt, value))
        return;
    fail(schema);
  }
  if (schema.contains("type")) {
    const std::string type = schema.at("type");
    bool valid = false;
    if (type == "object")
      valid = value.is_object();
    if (type == "array")
      valid = value.is_array();
    if (type == "string")
      valid = value.is_string();
    if (type == "boolean")
      valid = value.is_boolean();
    if (type == "null")
      valid = value.is_null();
    if (type == "number")
      valid = value.is_number() && std::isfinite(value.get<double>());
    // JSON Schema integer includes integral JSON numbers, e.g. 1.0.
    if (type == "integer")
      valid = value.is_number() && std::isfinite(value.get<double>()) &&
              std::floor(value.get<double>()) == value.get<double>();
    if (!valid)
      fail(schema);
  }
  if (schema.contains("enum") &&
      std::find(schema["enum"].begin(), schema["enum"].end(), value) ==
          schema["enum"].end())
    fail(schema);
  if (value.is_number()) {
    const auto number = value.get<double>();
    if (schema.contains("minimum") && number < schema["minimum"].get<double>())
      fail(schema);
    if (schema.contains("maximum") && number > schema["maximum"].get<double>())
      fail(schema);
    if (schema.contains("exclusiveMinimum") &&
        number <= schema["exclusiveMinimum"].get<double>())
      fail(schema);
    if (schema.contains("exclusiveMaximum") &&
        number >= schema["exclusiveMaximum"].get<double>())
      fail(schema);
  }
  if (value.is_string() && schema.contains("minLength") &&
      value.get_ref<const std::string&>().size() <
          schema["minLength"].get<size_t>())
    fail(schema);
  // An object schema without `properties` is an opaque object (a feature
  // spec): its own parser checks inside it.
  if (value.is_object() && schema.contains("properties")) {
    const auto& props = schema["properties"];
    // A key the method does not take first: it is the likelier typo, and
    // "missing" for the key it was meant to be would hide it.
    for (auto it = value.begin(); it != value.end(); ++it) {
      if (!props.contains(it.key())) {
        const std::string at = path + "/" + it.key();
        nlohmann::json keys = nlohmann::json::array();
        std::string list;
        for (auto p = props.begin(); p != props.end(); ++p) {
          keys.push_back(p.key());
          list += (list.empty() ? "" : ", ") + p.key();
        }
        ContractError e(operation, "unknown_argument", at, props, it.value(),
                        operation + ": unknown parameter " + where(at) +
                            "; it takes: " + list);
        e.diagnostic["accepted"] = keys;
        throw e;
      }
    }
    for (const auto& key : schema.value("required", nlohmann::json::array())) {
      const std::string k = key.get<std::string>();
      if (!value.contains(k)) {
        const std::string at = path + "/" + k;
        throw ContractError(
            operation, "missing_argument", at, props[k], nullptr,
            operation + ": missing required parameter " + where(at) + " (" +
                DescribeSchema(props[k]) + ")");
      }
    }
    // A choice (`oneOf` of single required keys): exactly one is given.
    if (schema.contains("oneOf")) {
      std::vector<std::string> keys, given;
      for (const auto& alt : schema["oneOf"])
        for (const auto& k : alt.at("required")) {
          keys.push_back(k.get<std::string>());
          if (value.contains(keys.back()))
            given.push_back(keys.back());
        }
      if (given.size() != 1) {
        const auto join = [](const std::vector<std::string>& v) {
          std::string out;
          for (const std::string& k : v)
            out += (out.empty() ? "" : ", ") + k;
          return out;
        };
        nlohmann::json observed = nullptr;
        for (const std::string& k : given)
          observed[k] = value[k];
        ContractError e(
            operation, given.empty() ? "missing_argument" : "invalid_argument",
            path, schema["oneOf"], observed,
            operation + ": " + where(path) + " takes exactly one of " +
                join(keys) +
                (given.empty() ? "; got none" : "; got " + join(given)));
        e.diagnostic["accepted"] = keys;
        throw e;
      }
    }
    for (auto it = value.begin(); it != value.end(); ++it)
      ValidateInspectionValue(props[it.key()], it.value(), operation,
                              path + "/" + it.key());
  }
  if (value.is_array()) {
    if (schema.contains("minItems") &&
        value.size() < schema["minItems"].get<size_t>())
      fail(schema);
    if (schema.contains("maxItems") &&
        value.size() > schema["maxItems"].get<size_t>())
      fail(schema);
    for (size_t i = 0; i < value.size(); ++i) {
      if (schema.contains("items"))
        ValidateInspectionValue(schema["items"], value[i], operation,
                                path + "/" + std::to_string(i));
      if (schema.value("uniqueItems", false))
        for (size_t j = 0; j < i; ++j)
          if (value[i] == value[j])
            fail(schema);
    }
  }
}

inline bool SchemaAccepts(const nlohmann::json& schema,
                          const nlohmann::json& value) {
  try {
    ValidateInspectionValue(schema, value, "", "/params");
    return true;
  } catch (const ContractError&) {
    return false;
  }
}

inline void ValidateInspectionRequest(const std::string& method,
                                      const nlohmann::json& params) {
  const auto& methods = InspectionContracts().at("methods");
  if (methods.contains(method))
    ValidateInspectionValue(methods[method]["request"], params, method);
}
} // namespace remote
