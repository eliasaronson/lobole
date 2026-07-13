#pragma once

#include "core/Document.h"
#include "core/Occurrence.h"

#include <nlohmann/json.hpp>

#include <memory>
#include <string>

// Saves/loads the feature list as pretty-printed JSON. This is the on-disk
// project format: human-readable and line-oriented so documents diff and
// merge well under git.
namespace DocumentIO {

// Returns false and fills *error (if given) on failure.
bool Save(const Document& doc, const std::string& path,
          std::string* error = nullptr);

// Returns nullptr and fills *error (if given) on failure. The returned
// document is already regenerated.
std::unique_ptr<Document> Load(const std::string& path,
                               std::string* error = nullptr);

// Single-feature (de)serialization; also used for parameter-edit snapshots
// (ModifyFeatureCommand). FeatureToJson throws on unserializable types;
// FeatureFromJson throws on malformed/unknown input.
nlohmann::json FeatureToJson(const Feature& feature);
std::unique_ptr<Feature> FeatureFromJson(const nlohmann::json& j);

// Assembly (de)serialization (A2). Same on-disk form as a part —
// directory (`.dsgn`, one file per occurrence) or single JSON file, dispatched
// on the path — discriminated by `kind: "assembly"`. Save returns false /
// Load returns nullptr and fills *error on failure.
bool SaveAssembly(const AssemblyDocument& asmDoc, const std::string& path,
                  std::string* error = nullptr);
std::unique_ptr<AssemblyDocument> LoadAssembly(const std::string& path,
                                               std::string* error = nullptr);

// Single-occurrence (de)serialization; used for snapshot commands
// (set_occurrence_transform) the way FeatureToJson serves ModifyFeatureCommand.
nlohmann::json OccurrenceToJson(const Occurrence& occurrence);
Occurrence OccurrenceFromJson(const nlohmann::json& j);

}  // namespace DocumentIO
