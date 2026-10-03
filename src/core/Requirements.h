#pragma once

// Design requirements stored with the project
// (docs/requirements/README.md).
//
// What a design is FOR used to live outside it: a generator's Python asserts,
// a sentence in a README, a check somebody ran once. A person or an agent
// who edits the model afterwards had nothing that re-checked them, and a
// check run before an edit looked exactly like one run after it. A
// Requirement is that sentence held by the project: what it measures (by
// stable reference: a document id, a study id, an occurrence id, a face's
// element name), a bound, a source in the designer's words, and a revision.
//
// Checking it produces EVIDENCE: the measured value, pass or fail, the input
// hash of what was measured and the revision it was measured at. A
// requirement then reads one of five states, and only one of them is a pass:
//
//   pass / fail  evidence for the current model and the current revision
//   stale        the model changed since: the input hash differs
//   unresolved   a reference no longer exists, or the measure could not be
//                taken (the note says which)
//   unchecked    never checked, or edited since it was
//
// This file is the model and its saved form; measuring is the app's
// (src/remote/RemoteRequirements.cpp), through the RPCs that already compute
// each quantity.

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace req {

// Every measure kind, each wired to the RPC that computes it.
inline constexpr const char* kMeasureKinds[] = {
    "study_probe",  // probe_study: a face's motion, or a formula over several
    "study_result", // study_results: peak displacement / stress, FoS
    "clearance",    // the gap between two occurrences (over a motion study)
    "contact",      // interference: clear | touching | not_clashing
    "mass",         // mass_properties, a part or the assembly
    "volume",       // mass_properties
    "fit_class",    // check_fits: one mate's fit is of a class
    "check_fits",   // check_fits passing
    "check_seat",   // check_seat passing, both rings, every listed bearing
    "bearing_bore", // the smallest bore of the listed bearings, mm
    "check_fasteners", // check_fasteners: no finding but the allowed ones
    "check_literals",  // check_literals clean
    "motion_clean",    // run_motion_study: clean
    "cam_verify",      // cam_verify: ok and complete
};
inline constexpr const char* kBoundKinds[] = {"at_most", "at_least", "within",
                                              "pass"};
inline constexpr const char* kStates[] = {"pass", "fail", "stale", "unresolved",
                                          "unchecked"};

// "a, b, c": the accepted vocabulary a refusal names.
std::string MeasureKindList();
std::string BoundKindList();

// Whether the kind measures a number (bounded at_most / at_least / within)
// or a verdict (bounded `pass`).
[[nodiscard]] bool IsNumericKind(const std::string& kind);
// The keys the measure of `kind` accepts besides `kind`, and which of them
// it requires.
std::vector<std::string> MeasureKeys(const std::string& kind);
std::vector<std::string> RequiredMeasureKeys(const std::string& kind);
// The document kind a measure's `document` must be: "part", "assembly",
// "job", "part|assembly", or "" when the measure takes none.
std::string DocumentKindFor(const std::string& kind);

struct Bound {
  std::string kind = "pass"; // one of kBoundKinds
  // at_most / at_least: `value`; within: `min`..`max`. Each may be an
  // expression over the project's variables (`*Expr`), evaluated when the
  // requirement is checked.
  double value = 0.0, min = 0.0, max = 0.0;
  std::string valueExpr, minExpr, maxExpr;
};

struct Evidence {
  bool taken = false; // false: never checked
  std::optional<double> value;
  std::string unit;
  bool pass = false;
  // The measure could not be taken (a run failed, a face no longer
  // resolved). Never a pass.
  std::string error;
  uint64_t inputHash = 0;
  int revision = 0;
  // The bound as it was evaluated (an expression's value then).
  std::optional<double> limit, limitMin, limitMax;
  // What the measuring RPC said that a reader needs: the findings, the
  // per-case numbers. Small.
  nlohmann::json detail;
  std::string checkedAt; // ISO 8601, UTC
};

struct Requirement {
  uint64_t id = 0;
  std::string name;   // what it is ("lens deflection at 3 g, level")
  std::string source; // why it exists, in a sentence
  int revision = 1;   // bumped by every edit; evidence names the one it saw
  // {kind, document?, ...}: the kind's own keys (MeasureKeys).
  nlohmann::json measure = nlohmann::json::object();
  Bound bound;
  Evidence evidence;

  [[nodiscard]] std::string Kind() const {
    return measure.value("kind", std::string());
  }
  [[nodiscard]] uint64_t Document() const {
    return measure.value("document", uint64_t{0});
  }
};

// Validates `measure` and `bound` against the vocabulary, throwing a refusal
// that names what IS accepted: an unknown measure kind, an unknown key for
// the kind, a missing required key, an unknown bound kind, a numeric bound
// on a verdict measure (or `pass` on a numeric one), `within` with min >
// max. References (documents, studies, occurrences) are the app's to check.
void Validate(const nlohmann::json& measure, const Bound& bound);

// Parses a bound from the RPC/file form {kind, value?, min?, max?}; a number
// or an expression string for each. Throws on an unknown kind.
Bound BoundFromJson(const nlohmann::json& j);
nlohmann::json BoundToJson(const Bound& b);

nlohmann::json EvidenceToJson(const Evidence& e);
Evidence EvidenceFromJson(const nlohmann::json& j);

// The saved form: {id, name, source?, revision, measure, bound, evidence?}.
// FromJson refuses what Validate refuses: a file naming a measure this
// build does not know loads as an error, never as some other measure.
nlohmann::json ToJson(const Requirement& r);
Requirement FromJson(const nlohmann::json& j);
std::vector<Requirement> ListFromJson(const nlohmann::json& j);
nlohmann::json ListToJson(const std::vector<Requirement>& list);

// Whether `value` meets the bound evaluated to (limit / min..max).
[[nodiscard]] bool Holds(const Bound& b, double value, double limit,
                         double limitMin, double limitMax);

// The state from what the app knows now: `unresolvedWhy` non-empty when a
// reference no longer resolves; `currentHash` the input hash the measure
// would read now.
std::string StateOf(const Requirement& r, const std::string& unresolvedWhy,
                    uint64_t currentHash);

// A file-name-safe slug of the name, as the other per-item files use.
std::string Slug(const std::string& name);

} // namespace req
