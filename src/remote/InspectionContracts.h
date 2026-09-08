#pragma once

#include "remote/InspectionContracts.generated.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace remote {

// Keep the readable legacy error string, with a machine-readable companion.
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
                   {"observed", observed}} {}
};

inline const nlohmann::json& InspectionContracts() {
  static const auto contracts = nlohmann::json::parse(kInspectionContractJson);
  return contracts;
}

// Supported JSON Schema subset is intentionally small and generated from one
// source. Domain checks (ownership, freshness, geometry) stay in the handler.
inline void ValidateInspectionValue(const nlohmann::json& schema,
                                    const nlohmann::json& value,
                                    const std::string& operation,
                                    const std::string& path = "/params") {
  const auto fail = [&](const nlohmann::json& expected) {
    throw ContractError(operation, "invalid_argument", path, expected, value);
  };
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
  if (type == "number")
    valid = value.is_number() && std::isfinite(value.get<double>());
  // JSON Schema integer includes integral JSON numbers, e.g. 1.0.
  if (type == "integer")
    valid = value.is_number() && std::isfinite(value.get<double>()) &&
            std::floor(value.get<double>()) == value.get<double>();
  if (!valid)
    fail(schema);
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
  }
  if (value.is_string() && schema.contains("minLength") &&
      value.get_ref<const std::string&>().size() <
          schema["minLength"].get<size_t>())
    fail(schema);
  if (value.is_object()) {
    for (const auto& key : schema.value("required", nlohmann::json::array())) {
      if (!value.contains(key.get<std::string>()))
        throw ContractError(
            operation, "missing_argument", path + "/" + key.get<std::string>(),
            schema["properties"][key.get<std::string>()], nullptr);
    }
    for (auto it = value.begin(); it != value.end(); ++it) {
      if (!schema["properties"].contains(it.key()))
        throw ContractError(operation, "unknown_argument",
                            path + "/" + it.key(), schema["properties"],
                            it.value());
      ValidateInspectionValue(schema["properties"][it.key()], it.value(),
                              operation, path + "/" + it.key());
    }
  }
  if (value.is_array()) {
    if (schema.contains("minItems") &&
        value.size() < schema["minItems"].get<size_t>())
      fail(schema);
    if (schema.contains("maxItems") &&
        value.size() > schema["maxItems"].get<size_t>())
      fail(schema);
    for (size_t i = 0; i < value.size(); ++i) {
      ValidateInspectionValue(schema["items"], value[i], operation,
                              path + "/" + std::to_string(i));
      if (schema.value("uniqueItems", false))
        for (size_t j = 0; j < i; ++j)
          if (value[i] == value[j])
            fail(schema);
    }
  }
}

inline void ValidateInspectionRequest(const std::string& method,
                                      const nlohmann::json& params) {
  const auto& methods = InspectionContracts().at("methods");
  if (methods.contains(method))
    ValidateInspectionValue(methods[method]["request"], params, method);
}
} // namespace remote
