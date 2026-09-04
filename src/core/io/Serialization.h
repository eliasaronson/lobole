#pragma once

#include "core/Document.h"
#include "core/Occurrence.h"
#include "core/cam/JobDocument.h"
#include "core/drawing/DrawingDocument.h"

#include <nlohmann/json.hpp>

#include <expected>
#include <filesystem>
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

// Write `text` to `target` via a temp file + rename, so an interrupted write
// can never leave a truncated file. Identical content is skipped entirely, so
// an untouched file keeps its mtime and git/rsync see only what changed.
// (Defined in AtomicWrite.cpp — it used to hide in Serialization.cpp's
// anonymous namespace, where the settings store could not reach it.)
bool WriteTextAtomic(const std::filesystem::path& target,
                     const std::string& text, std::string* error = nullptr);

// Where a NEW project picked in a save dialog should actually be written:
// always the `.dsgn` directory form (F3). A flat single-file project is
// still LOADED (legacy files, the RPC, tests), but saving one from the GUI
// created a project the folder-picking open dialog could never return —
// pickers hand back folders, so a flat `foo.json` was only reachable through
// Open Recent. Empty stays empty (nothing was picked).
std::string ProjectSaveTarget(const std::string& picked);

// Single-feature (de)serialization; also used for parameter-edit snapshots
// (ModifyFeatureCommand). FeatureToJson throws on unserializable types;
// FeatureFromJson throws on malformed/unknown input.
nlohmann::json FeatureToJson(const Feature& feature);
// A body's face/edge names as {faces: {index: name}, edges: {...}} — the
// shape baked geometry (geometry/index.json) and a Derived snapshot share.
nlohmann::json ElementMapToJson(const ElementMap& map);
ElementMap ElementMapFromJson(const nlohmann::json& j);
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

// Single-relation (de)serialization (v83, docs/assemblies/relations-plan.md);
// same convention. An unknown kind or DOF name is refused naming the
// accepted ones — never demoted to a default.
nlohmann::json RelationToJson(const Relation& relation);
Relation RelationFromJson(const nlohmann::json& j);
const char* RelationKindToString(RelationKind kind);
RelationKind RelationKindFromString(const std::string& s);
const char* MateDofToString(MateDof dof);
MateDof MateDofFromString(const std::string& s);

// Single-motion-study (de)serialization (v87, docs/agent-scale-plan.md G2).
// The definition only: driver mate + DOF, range in the DOF's own unit,
// sample count, and the clash check's mode. An unknown DOF or mode is
// refused naming the accepted ones.
nlohmann::json MotionStudyToJson(const MotionStudy& study);
MotionStudy MotionStudyFromJson(const nlohmann::json& j);

// Single-study (de)serialization (v29, FEM); used both for the studies/ dir
// files and to echo/snapshot studies from the RPCs and Commands. Throws on
// malformed input.
nlohmann::json StudyToJson(const fem::Study& study);

// CAM setups (v58) — SerializeCam.cpp. Inline in both save formats.
nlohmann::json CamSetupToJson(const cam::Setup& setup);
cam::Setup CamSetupFromJson(const nlohmann::json& j);
// The machine (docs/cam/job-document.md, round two): its JSON, embedded in a
// setup and written as one file per machine in the global store.
nlohmann::json MachineToJson(const cam::Machine& m);
cam::Machine
MachineFromJson(const nlohmann::json& j); // throws on an unknown post
// The global store: `dir` holds one `<slug>.json` per machine (the app uses
// settings::ConfigDir()/machines). Load returns every readable file sorted by
// name and reports the unreadable ones in `error` without failing the rest;
// Save refuses an unnamed machine, an unknown post, or a name that would
// share a file with a different name; Delete refuses a name it cannot find.
std::vector<cam::Machine> LoadMachines(const std::string& dir,
                                       std::string* error = nullptr);
bool SaveMachine(const std::string& dir, const cam::Machine& m,
                 std::string* error = nullptr);
bool DeleteMachine(const std::string& dir, const std::string& name,
                   std::string* error = nullptr);
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

// Job (de)serialization: the CAM job document (docs/cam/job-document.md),
// discriminated by `kind: "job"`. Unlike a part or a drawing the two on-disk
// forms carry the SAME body — a job is a parts list and a few setups, none of
// which earns a file of its own — so only the filename differs (`index.json`
// inside a directory, or the named file).
[[nodiscard]] std::expected<void, std::string>
SaveJob(const cam::JobDocument& job, const std::string& path);
// The job's body JSON as text: what a save would write, minus the envelope.
// Two jobs with the same fingerprint are the same job -- which is how the
// undo stack knows an edit happened without every edit site saying so (R7).
[[nodiscard]] std::string JobFingerprint(const cam::JobDocument& job);
[[nodiscard]] std::expected<std::unique_ptr<cam::JobDocument>, std::string>
LoadJob(const std::string& path);

// Single-view / single-dim (de)serialization; used for the dir-format files
// and to echo views/dims from the RPCs. Throw on malformed input.
nlohmann::json DrawingViewToJson(const DrawingView& view);
DrawingView DrawingViewFromJson(const nlohmann::json& j);
nlohmann::json DrawingDimToJson(const DrawingDim& dim);
DrawingDim DrawingDimFromJson(const nlohmann::json& j);

} // namespace DocumentIO
