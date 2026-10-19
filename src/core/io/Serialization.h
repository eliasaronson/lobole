// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "core/Document.h"
#include "core/Occurrence.h"
#include "core/cam/JobDocument.h"
#include "core/drawing/DrawingDocument.h"

#include <nlohmann/json.hpp>

#include <array>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

// Saves/loads the feature list as pretty-printed JSON. This is the on-disk
// project format: human-readable and line-oriented so documents diff and
// merge well under git.
namespace DocumentIO {

// On failure returns an unexpected error message.
[[nodiscard]] std::expected<void, std::string> Save(const Document& doc,
                                                    const std::string& path);

// The project scope a part's regeneration on load reads (Phase V): the
// project variables' values and expressions. Both must outlive the load; the
// loaded document keeps pointing at them until the caller repoints it.
struct ProjectScope {
  const SymbolTable* symbols = nullptr;
  const std::vector<Parameter>* parameters = nullptr;
};

// On failure returns an unexpected error message. The returned document is
// already regenerated -- against `scope` when given. A part that uses a
// project variable and was loaded without it regenerated with the variable
// unknown: its features failed, its baked geometry did not match, and every
// open rebuilt it from scratch before the project scope put it right.
[[nodiscard]] std::expected<std::unique_ptr<Document>, std::string>
Load(const std::string& path, const ProjectScope& scope = {});

// Write `text` to `target` via a temp file + rename, so an interrupted write
// can never leave a truncated file. Identical content is skipped entirely, so
// an untouched file keeps its mtime and git/rsync see only what changed.
// (Defined in AtomicWrite.cpp — it used to hide in Serialization.cpp's
// anonymous namespace, where the settings store could not reach it.)
bool WriteTextAtomic(const std::filesystem::path& target,
                     const std::string& text, std::string* error = nullptr);

// Whether `target` can be written that way: it names a file (not nothing,
// not a directory) in a directory that exists. Fills `error` with the
// measured condition when it cannot; both atomic writers ask first, so a
// refused write creates nothing.
bool AtomicTargetOk(const std::filesystem::path& target,
                    std::string* error = nullptr);

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
  // The same fields TYPED, in the RPC contract grammar
  // (docs/scripting/rpc.md#typed-feature-specs): what add_feature and
  // modify_feature validate a spec of this type against before it is
  // built, with kFeatureCommonContract's keys beside them. `params` stays
  // the prose an agent reads; this is the half a validator and the SDK's
  // types read.
  const char* contract;
};
std::span<const FeatureTypeInfo> FeatureTypes();

// The keys every feature spec may carry whatever its type: what
// FeatureToJson writes for any feature.
inline constexpr const char* kFeatureCommonContract =
    "name?:string, id?:int>=0, material?:string, appearance?:object, "
    "face_appearance?:object[], suppressed?:bool";

// What get_state stamps AROUND a feature's dialect (index and error state,
// a template instance's template_changed / expanded). A read-modify-write of
// a get_state entry -- a legitimate agent pattern -- sends them back, so
// they are dropped before a spec is checked rather than listed as fields a
// spec takes.
// A primitive's old world-coordinate dialect. The primitive parser refuses
// each BY NAME, pointing at `on` (core/Primitives.cpp) -- it used to work, so
// it is what an agent will send -- so a spec carrying one is left to that
// refusal rather than called an unknown parameter by the type's contract.
inline constexpr std::array<const char*, 3> kPrimitiveRetiredKeys = {
    "position", "center", "direction"};

inline constexpr std::array<const char*, 8> kFeatureEnvelopeKeys = {
    "index",    "error",
    "warning",  "after_earlier_failure",
    "report",   "template_changed",
    "expanded", "face_appearance_unresolved"};

// A sketch entity's JSON dialect, same shape and same job as FeatureTypeInfo.
// It used to be a bare list of NAMES, so `describe` could tell an agent that
// "arc" exists and not one field it takes — which is how a sketch arc's
// RADIAN angles went undocumented long enough to be built wrong.
struct SketchEntityInfo {
  const char* type;   // EntityFromJson dispatch string, e.g. "arc"
  int sinceVersion;   // document format version it arrived in (1 = original)
  const char* params; // field names, units, `?` optional, `(+_expr)` twins
};
std::span<const SketchEntityInfo> SketchEntityTypes();

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

// The vocabulary itself, from the SAME table the parser refuses against, so
// `describe` cannot drift from what add_mate/add_relation accept. An agent
// that cannot see the mate types guesses them; one that cannot see the
// relation kinds does not know coupled DOFs exist at all.
std::vector<std::string> MateTypeNames();
std::vector<std::string> RelationKindNames();

// Single-exploded-view (de)serialization (v27, A4); same convention as mates.
nlohmann::json ExplodedViewToJson(const ExplodedView& view);
ExplodedView ExplodedViewFromJson(const nlohmann::json& j);

// An assembly's sequence (v187, docs/assemblies/README.md#assembly-sequences):
// absent from a file when it has no steps. A step key it does not know, a
// step with no occurrence or a direction that is not three numbers is
// refused, naming what is accepted.
nlohmann::json AssemblySequenceToJson(const AssemblySequence& s);
AssemblySequence AssemblySequenceFromJson(const nlohmann::json& j);

// Single-relation (de)serialization (v83, docs/assemblies/relations.md);
// same convention. An unknown kind or DOF name is refused naming the
// accepted ones — never demoted to a default.
nlohmann::json RelationToJson(const Relation& relation);
Relation RelationFromJson(const nlohmann::json& j);
const char* RelationKindToString(RelationKind kind);
RelationKind RelationKindFromString(const std::string& s);
const char* MateDofToString(MateDof dof);
MateDof MateDofFromString(const std::string& s);
// A freewheel's `drives` (v104): +1 "positive", -1 "negative"; anything else
// is refused naming both.
const char* FreewheelDrivesToString(int drives);
int FreewheelDrivesFromString(const std::string& s);
// An intermittent relation's `motion` (v174) and its `slots`, as the slot
// count it stores: 0 for "uniform" and "table" (v184), N >= 3 for
// "geneva"; an unknown motion, a Geneva under 3 slots, and slots on any
// other motion are refused.
int IntermittentSlotsFor(const std::string& motion, int slots);
// A `table` motion's points (v184), [[driver fraction, step fraction],
// ...]: refused, naming why, when it is not an array of pairs of numbers
// or not a law (IntermittentTableError).
std::vector<std::pair<double, double>>
IntermittentTableFromJson(const nlohmann::json& j);

// Single-motion-study (de)serialization (v87,
// docs/assemblies/README.md#motion-studies). The definition only: driver mate +
// DOF, range in the DOF's own unit, sample count, and the clash check's mode.
// An unknown DOF or mode is refused naming the accepted ones.
nlohmann::json ClearanceAssertionToJson(const ClearanceAssertion& a);
ClearanceAssertion ClearanceAssertionFromJson(const nlohmann::json& j);
nlohmann::json MotionStudyToJson(const MotionStudy& study);
MotionStudy MotionStudyFromJson(const nlohmann::json& j);

// Single-study (de)serialization (v29, FEM); used both for the studies/ dir
// files and to echo/snapshot studies from the RPCs and Commands. Throws on
// malformed input.
nlohmann::json StudyToJson(const fem::Study& study);
// A load's region (v183): {normal, from_mm, to_mm} or {center_mm,
// radius_mm}; throws naming `who` and what is accepted.
fem::LoadRegion LoadRegionFromJson(const nlohmann::json& j,
                                   const std::string& who);

// CAM setups (v58) — SerializeCam.cpp. Inline in both save formats.
nlohmann::json CamSetupToJson(const cam::Setup& setup);
cam::Setup CamSetupFromJson(const nlohmann::json& j);
// The machine (docs/cam/setup.md#machine-profiles): its JSON, embedded in a
// setup and written as one file per machine in the global store.
nlohmann::json MachineToJson(const cam::Machine& m);
// How a machine's rotary is installed, and what holds a part in it (v132):
// read and REFUSED whole -- a partial or unknown statement throws, naming
// what is accepted (docs/cam/rotary.md#how-the-rotary-is-installed).
cam::RotaryMount RotaryMountFromJson(const nlohmann::json& j);
cam::RotaryHolding RotaryHoldingFromJson(const nlohmann::json& j);
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
// Print profiles (docs/tolerances/README.md#print-profiles): one
// `<slug>.json` per profile under `dir` (the app uses
// settings::ConfigDir()/print_profiles), the same contract as the machine
// store. Load returns every readable file sorted by name and reports the
// unreadable ones in `error` without failing the rest; Save refuses what
// printfit::ValidateProfile refuses and a name that would share a file with a
// different name; Delete refuses a name it cannot find.
nlohmann::json PrintProfileToJson(const printfit::Profile& p);
// Throws on a malformed profile, naming what is wrong (an unknown kind, a
// missing field, anything ValidateProfile refuses).
printfit::Profile PrintProfileFromJson(const nlohmann::json& j);
// A row on the wire: {kind, from_mm, to_mm (null or absent = and up),
// allowance_mm, source?}.
nlohmann::json PrintRowToJson(const printfit::Row& r);
printfit::Row PrintRowFromJson(const nlohmann::json& j);
std::vector<printfit::Profile> LoadPrintProfiles(const std::string& dir,
                                                 std::string* error = nullptr);
bool SavePrintProfile(const std::string& dir, const printfit::Profile& p,
                      std::string* error = nullptr);
bool DeletePrintProfile(const std::string& dir, const std::string& name,
                        std::string* error = nullptr);
fem::Study StudyFromJson(const nlohmann::json& j);

// A study's RESULT (de)serialization: topology (v33), static and modal
// (v153). Saved with the project beside its study, with the input hash that
// produced it, so a reopened project's run of the same inputs is answered
// from it (docs/simulation/README.md#studies-as-jobs): what fem::
// KeepWhatIsRead keeps, never the solver's scratch. The numeric arrays ride
// in the json as a binary value (kResultData, SerializationInternal.h),
// which the directory save writes as <base>.result.bin beside the json
// (v154) and the load puts back; a v153 json with its arrays inline reads.
// `solvedAtVersion` is NOT written -- on load the caller stamps it (current
// when the inputs still hash the same, stale otherwise). Returns false for a
// runtime that holds nothing solved. Throws on malformed input.
bool StudyResultToJson(const fem::StudyRuntime& rt, nlohmann::json& out);
fem::StudyRuntime StudyResultFromJson(const nlohmann::json& j);
// A motion study's result (v153), the same way: every sample's poses (in the
// binary value since v154), mate values, clashes and unknown contacts, the
// press fits it relieved (v159), the
// pair counts, and its input hash.
nlohmann::json MotionResultToJson(const motion::Result& result);
motion::Result MotionResultFromJson(const nlohmann::json& j);

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

// Job (de)serialization: the CAM job document (docs/cam/README.md#the-model),
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
