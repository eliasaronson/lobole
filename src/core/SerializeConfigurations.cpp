#include "core/Serialization.h"
#include "core/SerializationInternal.h"

#include "core/Configuration.h"
#include "core/Document.h"

#include <nlohmann/json.hpp>

using nlohmann::json;

namespace DocumentIO {

// ---- Configurations (v31) -------------------------------------------------
// A configuration persists like a study: tagged JSON, only-non-empty fields.
// Which configuration is active lives in the document envelope (see
// DocumentEnvelope), the rollback-bar pattern.

json ConfigurationToJson(const Configuration& c) {
  json j;
  j["type"] = "Configuration";
  j["id"] = c.id;
  j["name"] = c.name;
  if (!c.parameterOverrides.empty()) {
    j["parameter_overrides"] = json::array();
    for (const auto& [name, expr] : c.parameterOverrides)
      j["parameter_overrides"].push_back({{"name", name}, {"expression", expr}});
  }
  if (!c.suppressedFeatures.empty())
    j["suppressed_features"] = c.suppressedFeatures;
  return j;
}

Configuration ConfigurationFromJson(const json& j) {
  Configuration c;
  c.id = j.at("id").get<uint64_t>();
  c.name = j.value("name", std::string{});
  for (const json& oj : j.value("parameter_overrides", json::array()))
    c.parameterOverrides.emplace_back(oj.value("name", std::string{}),
                                      oj.value("expression", std::string{}));
  c.suppressedFeatures =
      j.value("suppressed_features", std::vector<uint64_t>{});
  return c;
}

}  // namespace DocumentIO
