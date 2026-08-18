#pragma once

#include "core/Document.h"
#include "core/Occurrence.h"
#include "core/drawing/DrawingDocument.h"

#include <nlohmann/json.hpp>

#include <expected>
#include <memory>
#include <span>
#include <string>

// Saves/loads the feature list as pretty-printed JSON. This is the on-disk
// project format: human-readable and line-oriented so documents diff and
// merge well under git.
namespace DocumentIO {

// On failure returns an unexpected error message.
[[nodiscard]] std::expected<void, std::string> Save(const Document& doc,
                                                    const std::string& path);

// On failure returns an unexpected error message. The returned document is
// already regenerated.
[[nodiscard]] std::expected<std::unique_ptr<Document>, std::string>
Load(const std::string& path);

// Single-feature (de)serialization; also used for parameter-edit snapshots
// (ModifyFeatureCommand). FeatureToJson throws on unserializable types;
// FeatureFromJson throws on malformed/unknown input.
nlohmann::json FeatureToJson(const Feature& feature);
std::unique_ptr<Feature> FeatureFromJson(const nlohmann::json& j);

// Introspection for the describe RPC (read-only metadata; the tables live
// beside the dispatch chains in SerializeFeatures.cpp — extend together).
int DocumentFormatVersion(); // == kVersion without exposing the internal hdr
struct FeatureTypeInfo {
  const char* type; // FeatureFromJson dispatch string, e.g. "Extrude"
  int sinceVersion; // document format version it arrived in (1 = original)
  // One-line JSON dialect for the describe endpoint (V5): field names, axis
  // conventions, defaults. `?` marks optional, `(+_expr)` an expression twin.
  const char* params;
};
std::span<const FeatureTypeInfo> FeatureTypes();
std::span<const char* const> SketchEntityTypes();

// Assembly (de)serialization (A2). Same on-disk form as a part —
// directory (`.dsgn`, one file per occurrence) or single JSON file, dispatched
// on the path — discriminated by `kind: "assembly"`. Save returns false /
// Load returns nullptr and fills *error on failure.
[[nodiscard]] std::expected<void, std::string>
SaveAssembly(const AssemblyDocument& asmDoc, const std::string& path);
[[nodiscard]] std::expected<std::unique_ptr<AssemblyDocument>, std::string>
LoadAssembly(const std::string& path);

// Single-occurrence (de)serialization; used for snapshot commands
// (set_occurrence_transform) the way FeatureToJson serves ModifyFeatureCommand.
nlohmann::json OccurrenceToJson(const Occurrence& occurrence);
Occurrence OccurrenceFromJson(const nlohmann::json& j);

// Single-mate (de)serialization (A3); used to echo mates from the add_mate/
// set_mate RPCs. Throws on malformed input.
nlohmann::json MateToJson(const Mate& mate);
Mate MateFromJson(const nlohmann::json& j);

// Single-exploded-view (de)serialization (v27, A4); same convention as mates.
nlohmann::json ExplodedViewToJson(const ExplodedView& view);
ExplodedView ExplodedViewFromJson(const nlohmann::json& j);

// Single-study (de)serialization (v29, FEM); used both for the studies/ dir
// files and to echo/snapshot studies from the RPCs and Commands. Throws on
// malformed input.
nlohmann::json StudyToJson(const fem::Study& study);
fem::Study StudyFromJson(const nlohmann::json& j);

// Topology-study RESULT (de)serialization (v33). Unlike FEM results (derived,
// never stored), a topology result is minutes of compute over a non-
// reproducible mesh, so it IS persisted: the optimized density field + its
// mesh + the convergence summary. `solvedAtVersion` is NOT written — on load
// the caller stamps the result with the reloaded document's version (a just-
// loaded result reads as current; a subsequent edit marks it stale). Returns
// true only for a topology runtime (topology == true). Throws on malformed
// input.
bool StudyResultToJson(const fem::StudyRuntime& rt, nlohmann::json& out);
fem::StudyRuntime StudyResultFromJson(const nlohmann::json& j);

// Single-configuration (de)serialization (v31); the studies pattern. The
// active-configuration id lives in the document envelope, not here. Throws on
// malformed input.
nlohmann::json ConfigurationToJson(const Configuration& configuration);
Configuration ConfigurationFromJson(const nlohmann::json& j);

// Drawing (de)serialization (v30). Same on-disk form as a part/assembly —
// directory (one file per view/dim) or single JSON file, dispatched on the
// path — discriminated by `kind: "drawing"`. Save returns false / Load
// returns nullptr and fills *error on failure.
[[nodiscard]] std::expected<void, std::string>
SaveDrawing(const DrawingDocument& drawing, const std::string& path);
[[nodiscard]] std::expected<std::unique_ptr<DrawingDocument>, std::string>
LoadDrawing(const std::string& path);

// Single-view / single-dim (de)serialization; used for the dir-format files
// and to echo views/dims from the RPCs. Throw on malformed input.
nlohmann::json DrawingViewToJson(const DrawingView& view);
DrawingView DrawingViewFromJson(const nlohmann::json& j);
nlohmann::json DrawingDimToJson(const DrawingDim& dim);
DrawingDim DrawingDimFromJson(const nlohmann::json& j);

} // namespace DocumentIO
