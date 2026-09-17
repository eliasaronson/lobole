#include "core/io/Serialization.h"
#include "core/io/SerializationInternal.h"

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
      j["parameter_overrides"].push_back(
          {{"name", name}, {"expression", expr}});
  }
  if (!c.suppressedFeatures.empty())
    j["suppressed_features"] = c.suppressedFeatures;
  // v110, part only; absent when empty so every earlier configuration
  // re-saves byte-identically.
  if (!c.unsuppressedFeatures.empty())
    j["unsuppressed_features"] = c.unsuppressedFeatures;
  // v101, assembly only; absent when empty, so older files re-save
  // byte-identically.
  if (!c.suppressedMates.empty())
    j["suppressed_mates"] = c.suppressedMates;
  if (!c.suppressedRelations.empty())
    j["suppressed_relations"] = c.suppressedRelations;
  if (!c.mateOverrides.empty()) {
    j["mate_overrides"] = json::array();
    for (const MateOverride& o : c.mateOverrides) {
      json oj = {{"mate", o.mate}};
      if (o.rotation)
        oj["rotation"] = *o.rotation; // radians
      if (o.offset)
        oj["offset"] = *o.offset; // mm
      j["mate_overrides"].push_back(std::move(oj));
    }
  }
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
  c.unsuppressedFeatures =
      j.value("unsuppressed_features", std::vector<uint64_t>{});
  c.suppressedMates = j.value("suppressed_mates", std::vector<uint64_t>{});
  c.suppressedRelations =
      j.value("suppressed_relations", std::vector<uint64_t>{});
  for (const json& oj : j.value("mate_overrides", json::array())) {
    MateOverride o;
    o.mate = oj.at("mate").get<uint64_t>();
    if (oj.contains("rotation"))
      o.rotation = oj.at("rotation").get<float>();
    if (oj.contains("offset"))
      o.offset = oj.at("offset").get<float>();
    c.mateOverrides.push_back(o);
  }
  return c;
}

} // namespace DocumentIO
