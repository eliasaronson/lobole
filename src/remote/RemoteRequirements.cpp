// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "remote/RemoteRequirements.h"
#include "cad/CADKernel.h"
#include "common/Logger.h"

#include "core/Angles.h"
#include "core/AssemblySolve.h"
#include "core/DesignSweep.h"
#include "core/Expression.h"
#include "core/InputHash.h"
#include "core/Measure.h"
#include "core/MeasureSeal.h"
#include "core/MeshCollide.h"
#include "core/MotionStudy.h"
#include "core/Occurrence.h"
#include "core/PartMeasures.h"
#include "core/Receipt.h"
#include "core/Requirements.h"
#include "core/fem/FemSolver.h"
#include "core/fem/FemStudy.h"
#include "core/io/Serialization.h"
#include "remote/RemoteAssembly.h"
#include "remote/RemoteCam.h"
#include "remote/RemoteInspection.h"
#include "remote/RemoteModeling.h"
#include "remote/RemoteMotion.h"
#include "remote/RemoteSimulation.h"
#include "ui/AppActionsInternal.h"
#include "ui/AppState.h"
#include "ui/AssemblyActions.h"
#include "ui/BackgroundJobs.h"
#include "ui/CamJobs.h"
#include "ui/DocumentSlots.h"
#include "ui/MotionActions.h"
#include "ui/RequirementsPanel.h"
#include "ui/VariableActions.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <format>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <thread>

#include <Standard_Failure.hxx>
#include <unistd.h> // getpid: a sweep's copies' directory

using json = nlohmann::json;

// Design requirements (docs/requirements/README.md). The model and its saved
// form are core/Requirements; this is where a requirement is MEASURED, and
// it measures through the same handlers the RPCs answer with -- run_study and
// probe_study, check_seat, cam_verify -- so a requirement and the call a
// person would make agree by construction rather than by a second copy.

namespace remote {

namespace {

constexpr const char* kJobKind = "requirements";

const MethodDoc RequirementMethods[] = {
    {"add_requirement",
     "State a requirement the design must meet and hold it IN the project "
     "(docs/requirements/README.md): {name, measure: {kind, document, ...}, "
     "bound: {kind: at_most|at_least {value} | within {min, max} | pass}, "
     "source?: why it exists, in a sentence}. Measure kinds: study_probe "
     "{document: part, study, face?, at?, quantity?: translation|rotation|"
     "displacement_at|von_mises_max|von_mises_mean, case?, readings?, "
     "expression?, unit?} (a face's motion, or the von Mises stress ON it "
     "in MPa -- a tooth root rather than the global peak -- in a static "
     "study, or a formula over several faces' readings and the project's "
     "variables), "
     "study_result {document, study, quantity: max_displacement|"
     "max_von_mises|factor_of_safety, case?}, clearance {document: "
     "assembly, a, b: occurrence ids, mode?: mesh|exact, motion_study?: the "
     "smallest gap over that study's samples}, contact {document, a, b, "
     "expect: clear|touching|not_clashing}, seal {document: assembly, a, "
     "b: occurrence ids, motion_study?: the worst of its samples} (mm: the "
     "widest ring a and b's contact closes round its axis, by its narrowest "
     "-- 0 when none closes; check_seal), insertion {document: assembly, a "
     "(what is slid on), b (what it goes on): occurrence ids, entry: "
     "positive|negative|either, mate?: the mate id whose frame's Z is the "
     "axis (else the one mate joining them), allowance_mm?, samples?, "
     "rotations?, lead_in_mm?} (pass: a reaches its seat with no more "
     "travel under interference than its own press; check_insertion), "
     "assembly_sequence {document: assembly} (pass: every step of its "
     "sequence goes home against everything placed before it, and no "
     "occurrence is missed; check_assembly_sequence), "
     "mass / volume {document: part "
     "or assembly}, fit_class {document, mate, class: clearance|transition|"
     "interference}, check_fits {document, allow_unverified?}, check_seat "
     "{document, occurrences: [bearing ids], allow_unverified?}, "
     "bearing_bore {document: assembly, occurrences: [bearing ids]} (mm, "
     "the smallest bore of those bearings: bound it at_least the shaft's "
     "own minimum, an expression), "
     "section_area {document: part, of: material|enclosed, base_plane: "
     "xy|xz|yz + offset (mm), or origin + normal, bodies?: [body names or "
     "feature ids]} (mm2: the material a plane cuts, or the area it "
     "ENCLOSES -- the floor inside the walls), "
     "check_fasteners {document, allow?: [{part?, why?, max?}], "
     "allow_skipped?}, "
     "check_literals {document?: one part, or documents?: several, else "
     "every part}, expression {expression: a formula over the variables "
     "(\"od - bore\"), unit?: what it reads in (default mm; the formula is "
     "in base units, mm and degrees), document?: a part, read as its "
     "features see it (local variables over the project's, its active "
     "configuration applied), configuration?: that part's configuration to "
     "read instead (0 = base)} (measured in place wherever the values move: "
     "the call that changes one answers `requirements` for each expression "
     "requirement whose state it changed; an unknown name or a formula that "
     "does not compile is refused naming the scope's variables), "
     "motion_clean "
     "{document, study}, cam_verify {document: job, setup, resolution?, "
     "allow_uncut?: [what prefixes]}. A number is bounded at_most, at_least "
     "or within; a verdict is bounded pass. A bound value may be an "
     "expression over the project's variables. An unknown kind or key is "
     "refused naming the accepted ones; a document, study or occurrence "
     "that does not exist is refused. Units: mm, deg, MPa, g, mm2, mm3 as the "
     "measure reports them; an expression's as its `unit` says.",
     "bound, measure, name, source", NotInBatch,
     "id:int, requirement:{bound:object, id:int, measure:object, "
     "name:string, note?:string, revision:int, source?:string, "
     "state:string, checked_at?:string, detail?:object, "
     "evidence_revision?:int, input_hash?:string, limit?:number, "
     "limit_max?:number, limit_min?:number, unit?:string, "
     "value?:number}"},
    {"set_requirement",
     "Edit a requirement {id, name?, source?, measure?, bound?}. Any edit "
     "bumps its revision and DROPS its evidence: a check of the old "
     "requirement says nothing about the new one, so it reads unchecked "
     "until checked again.",
     "bound, id, measure, name, source", NotInBatch,
     "requirement:{bound:object, id:int, measure:object, name:string, "
     "note?:string, revision:int, source?:string, state:string, "
     "checked_at?:string, detail?:object, evidence_revision?:int, "
     "input_hash?:string, limit?:number, limit_max?:number, "
     "limit_min?:number, unit?:string, value?:number}"},
    {"remove_requirement", "Remove a requirement {id}.", "id", NotInBatch,
     "removed:int"},
    {"list_requirements",
     "Every requirement and where it stands, WITHOUT measuring: state pass | "
     "fail (evidence for the current model and revision) | stale (the model "
     "changed since: the input hash differs) | unresolved (a reference is "
     "gone, or the measure could not be taken; never a pass) | unchecked "
     "(never checked, or edited since). Cheap: hashes, no solves.",
     "", NoFlags,
     "all_pass:bool, requirements:{bound:object, id:int, "
     "measure:object, name:string, note?:string, revision:int, "
     "source?:string, state:string, checked_at?:string, detail?:object, "
     "evidence_revision?:int, input_hash?:string, limit?:number, "
     "unit?:string, value?:number, limit_max?:number, "
     "limit_min?:number}[], summary:{fail:int, pass:int, stale:int, "
     "unchecked:int, unresolved:int}"},
    {"check_requirements",
     "Check the requirements {ids?: some of them, async?, recompute?, "
     "outlive_client?}: measure each through the RPC that computes it and "
     "record evidence (value, pass, the input hash measured, the revision). "
     "A requirement whose evidence is current is answered from it, and a "
     "study whose inputs are unchanged is answered from its stored result "
     "(docs/simulation/README.md#studies-as-jobs), so re-checking an "
     "unchanged design computes nothing; `recompute: true` measures and "
     "re-runs anyway (each study once). What it measures it measures as a "
     "job, off the frame: `async: true` answers at once with the `job` "
     "(polled with study_status {job}, stopped with cancel_study {job}); "
     "without it the call waits for the job and answers with the rows. "
     "Either is cancelled with its client unless outlive_client; nothing "
     "to measure answers at once. A check visits each requirement's tab "
     "and comes back to the one it started on.",
     "async, ids, outlive_client, recompute", NotInBatch,
     "all_pass?:bool, checked?:int, receipt?:{claims:object[], "
     "schema:string, standing:string, summary:string}, "
     "requirements:{bound:object, checked_at?:string, detail?:object, "
     "evidence_revision?:int, id:int, input_hash?:string, "
     "limit?:number, measure:object, name:string, revision:int, "
     "source?:string, state:string, unit?:string, value?:number, "
     "limit_max?:number, limit_min?:number, note?:string}[], "
     "reused?:int, summary:{fail:int, pass:int, stale:int, "
     "unchecked:int, unresolved:int}, checking?:int, job?:int, "
     "kind?:string, started?:bool"},
    {"design_sweep",
     "Regenerate the model over ranges of its variables, on copies, and say "
     "where it stops building or meeting its requirements "
     "(docs/requirements/README.md#design-sweep). {variables?: [{name, "
     "min, max, scope?: project|part|<part id>}] (each end a number or an "
     "expression, in the variable's base units: mm, or degrees for an "
     "angle), from_requirements?: true (each range an expression "
     "requirement states on one bare variable -- a within bound; a "
     "one-sided one needs its other end in variables), samples?: per "
     "variable, ends included, evenly spaced (default 5, 2..33; each "
     "varied alone, the others at their values), corners?: true adds the "
     "box's corners (up to 8 variables) and its centre, refine?: bisect "
     "each failure's bracket (default 0, at most 8 times), documents?: [part "
     "ids] (default: every part whose variables or features read a swept "
     "one, directly or through others), max_points?: (default 64, at most "
     "256), async?, outlive_client?}. A point records, per rebuilt part, "
     "regen ok|broken with the broken features {feature_id, name, error}, "
     "each sketch that does not solve or whose dof moved {feature_id, ok, "
     "dof, conflicting}, volume_mm3 and mass_g; and every expression, mass, "
     "volume and section_area requirement of a rebuilt part, measured as "
     "the check measures it (the others are listed once in not_swept, with "
     "why). first_failure {name: {below?, above?: {last_ok, failed, why}}} "
     "is the failing value nearest the variable's own on each side. "
     "Refused before anything starts: an unknown key, a variable not in "
     "scope (naming those that are) or that nothing reads, one a rebuilt "
     "part's active configuration overrides, min above max, a plan over "
     "max_points (naming the count), a second sweep while one runs. Nothing "
     "in the document changes and the format does not move. A job "
     "(design_sweep): async answers with `job` (study_status {job} follows "
     "it, cancel_study {job} stops it between points keeping what was "
     "measured); otherwise the call waits. It is cancelled with its client "
     "unless outlive_client. The last sweeps are kept in memory: "
     "design_sweep_result.",
     "async, corners, documents, from_requirements, max_points, "
     "outlive_client, refine, samples, variables",
     NotInBatch,
     "base?:object, cancelled?:bool, cap:int, first_failure?:object, "
     "job:int, kind?:string, not_swept?:object[], planned:int, "
     "points?:object[], seconds?:number, started?:bool, "
     "variables:object[]"},
    {"design_sweep_result",
     "A design sweep's answer {job?: the sweep's job, else the last one}: "
     "what design_sweep answers once it has run (cancelled: true with the "
     "points done when it was stopped), or {running: true, fraction, "
     "label} while it runs. Refused naming the sweeps there are.",
     "job", Deferrable,
     "base?:object, cancelled?:bool, cap?:int, first_failure?:object, "
     "fraction?:number, job:int, label?:string, not_swept?:object[], "
     "planned?:int, points?:object[], running?:bool, seconds?:number, "
     "variables?:object[]"},
    // The declared clearances (v97) are clearance requirements now
    // (docs/requirements/README.md#clearance-assertions). These four keep
    // their words for the scripts that use them.
    {"add_assertion",
     "Declare a clearance this assembly must keep: {name, a, b: top-level "
     "occurrence ids, min_mm, mode?: mesh|exact, why?}. It is a REQUIREMENT "
     "(measure clearance, bound at_least min_mm, source why) on the active "
     "assembly; list_requirements shows it with every other one.",
     "a, b, min_mm, mode, name, why", NotInBatch, "id:int, name:string"},
    {"list_assertions",
     "The active assembly's clearance requirements and where each stands, "
     "WITHOUT re-checking: holds | violated | unverifiable | stale | never.",
     "", NoFlags,
     "assertions:{a:int, b:int, id:int, min_mm:number, mode:string, "
     "name:string, note?:string, standing:string, why:string, "
     "last_verdict?:string, measured_mm?:number}[]"},
    {"remove_assertion", "Remove a declared clearance (a requirement) {id}.",
     "id", NotInBatch},
    {"verify_assertions",
     "Check the active assembly's clearance requirements now {id?: just "
     "that one} and record the evidence. Returns a receipt: one whose "
     "occurrence no longer resolves is UNVERIFIABLE, never a pass. In mesh "
     "mode the tessellation tolerance is SUBTRACTED from the measured gap.",
     "id", NotInBatch,
     "assertions:{a:int, b:int, id:int, last_verdict:string, "
     "measured_mm?:number, min_mm:number, mode:string, name:string, "
     "standing:string, why:string, note?:string}[], "
     "receipt:{claims:object[], schema:string, standing:string, "
     "summary:string}"},
};

// ---- Documents --------------------------------------------------------------

const char* KindName(ui::DocKind k) {
  switch (k) {
  case ui::DocKind::Part:
    return "part";
  case ui::DocKind::Assembly:
    return "assembly";
  case ui::DocKind::Drawing:
    return "drawing";
  case ui::DocKind::Job:
    return "job";
  }
  return "part";
}

// The slot a requirement's document is, or why it is not one it can be.
std::optional<size_t> DocSlot(ui::AppState& state, uint64_t docId,
                              const std::string& want, std::string* why) {
  const auto slot = ui::SlotIndexById(state, docId);
  if (!slot) {
    if (why)
      *why = "document " + std::to_string(docId) +
             " is gone (list_parts names the documents there are)";
    return std::nullopt;
  }
  const std::string kind = KindName(state.parts[*slot].kind);
  if (want.find(kind) == std::string::npos) {
    if (why)
      *why = "document " + std::to_string(docId) + " ('" +
             state.parts[*slot].name + "') is a " + kind + "; this measure " +
             "reads a " + want;
    return std::nullopt;
  }
  return slot;
}

std::string DocumentCandidates(const ui::AppState& state) {
  std::vector<std::string> v;
  for (const ui::PartSlot& s : state.parts)
    v.push_back(std::to_string(s.id) + " (" + s.name + ", " + KindName(s.kind) +
                ")");
  return KnownList(v);
}

ElementRef FaceOf(const json& r) {
  if (r.is_string())
    return ElementRef{0, r.get<std::string>()};
  if (!r.is_object() || !r.contains("element"))
    throw std::runtime_error("a face is {feature_id, element}, or just the "
                             "element name (list_elements names them)");
  return ElementRef{r.value("feature_id", uint64_t{0}),
                    r.at("element").get<std::string>()};
}

// ---- Expressions
// -------------------------------------------------------------

// The identifiers the requirement's expressions read: its bound's and a
// study_probe's formula and `at`. Their values are inputs to the evidence,
// so a variable that moves makes it stale.
std::set<std::string> ExpressionNames(const req::Requirement& r) {
  std::set<std::string> names;
  const auto add = [&](const std::string& text) {
    if (text.empty())
      return;
    auto c = ExpressionEngine::Default().Compile(text);
    if (c)
      for (const std::string& d : (*c)->Dependencies())
        names.insert(d);
  };
  add(r.bound.valueExpr);
  add(r.bound.minExpr);
  add(r.bound.maxExpr);
  const json& m = r.measure;
  if (m.contains("expression") && m.at("expression").is_string())
    add(m.at("expression").get<std::string>());
  const auto addAt = [&](const json& at) {
    if (at.is_array())
      for (const json& v : at)
        if (v.is_string())
          add(v.get<std::string>());
  };
  if (m.contains("at"))
    addAt(m.at("at"));
  if (m.contains("readings") && m.at("readings").is_object())
    for (const auto& [name, rd] : m.at("readings").items())
      if (rd.contains("at"))
        addAt(rd.at("at"));
  return names;
}

double Evaluate(const ui::AppState& state, const std::string& text,
                const SymbolTable& extra, const char* what) {
  SymbolTable sym = state.projectSymbols;
  for (const auto& [k, v] : extra)
    sym[k] = v;
  const auto v = ExpressionEngine::Default().EvaluateText(text, sym);
  if (!v)
    throw std::runtime_error(std::string(what) + " '" + text +
                             "': " + v.error().message);
  return *v;
}

std::optional<glm::dvec3> PointOf(const ui::AppState& state, const json& at) {
  if (!at.is_array() || at.size() != 3)
    throw std::runtime_error(
        "`at` is a point [x, y, z] mm (each a number or an expression)");
  glm::dvec3 p(0.0);
  for (int k = 0; k < 3; k++)
    p[k] = at[k].is_number()
               ? at[k].get<double>()
               : Evaluate(state, at[k].get<std::string>(), {}, "at");
  return p;
}

// The scope an `expression` requirement reads
// (docs/requirements/README.md#expression): the project's variables and
// tables; a part's as its features see them (local over project, its active
// configuration applied); or that part in the configuration the measure
// names, whichever is active. `why` non-empty: it cannot be read now.
struct ExprScope {
  const SymbolTable* table = nullptr;
  SymbolTable storage; // a configuration's, when `own`
  bool own = false;
  std::string name; // "the project", "part 'Housing'"
  std::string why;
  json detail = json::object(); // {scope, part?, configuration?}
  const SymbolTable& Table() const { return own ? storage : *table; }
};

std::string ConfigurationList(const Document& doc) {
  std::vector<std::string> v{"0 (base)"};
  for (const Configuration& c : doc.GetConfigurations())
    v.push_back(std::to_string(c.id) + " (" + c.name + ")");
  return KnownList(v);
}

void ScopeOf(ui::AppState& state, const req::Requirement& r, ExprScope& s) {
  const json& m = r.measure;
  if (!m.contains("document")) {
    s.table = &state.projectSymbols;
    s.name = "the project";
    s.detail = {{"scope", "project"}};
    return;
  }
  const auto slot = DocSlot(state, r.Document(), "part", &s.why);
  if (!slot)
    return;
  const Document* doc = ui::SlotDoc(state, *slot);
  if (doc == nullptr) {
    s.why = "document " + std::to_string(r.Document()) + " is not loaded";
    return;
  }
  const std::string part = state.parts[*slot].name;
  s.name = "part '" + part + "'";
  s.detail = {{"scope", "part"}, {"part", part}};
  if (!m.contains("configuration")) {
    s.table = &doc->Symbols();
    if (const Configuration* c = doc->ActiveConfig())
      s.detail["configuration"] = c->name; // active: what its features see
    return;
  }
  const uint64_t cfg = m.at("configuration").get<uint64_t>();
  const Configuration* c = cfg == 0 ? nullptr : doc->FindConfiguration(cfg);
  if (cfg != 0 && c == nullptr) {
    s.why = "configuration " + std::to_string(cfg) + " is gone from '" + part +
            "' (its configurations: " + ConfigurationList(*doc) + ")";
    return;
  }
  s.storage = doc->SymbolsIn(cfg);
  s.own = true;
  const std::string cname = c ? c->name : std::string("base");
  s.name += " in configuration '" + cname + "'";
  s.detail["configuration"] = cname;
}

// Why an expression requirement's names cannot be read in its scope now: a
// name that went anyway (a part deleted, a merge) is a reference gone,
// never a 0.
std::string MissingName(const req::Requirement& r, const ExprScope& s) {
  for (const std::string& text : req::ExpressionTexts(r))
    for (const std::string& name : req::IdentifiersOf(text))
      if (!s.Table().contains(name))
        return "variable '" + name + "' is not in " + s.name;
  return {};
}

// ---- Input hashes
// ------------------------------------------------------------

void HashPartExtras(inputhash::Hasher& h, const Document& doc) {
  h.U64(doc.GetTolerances().size());
  for (const Tolerance& t : doc.GetTolerances()) {
    h.U64(t.ref.featureId);
    h.Str(t.ref.name);
    h.U64(t.ref2.featureId);
    h.Str(t.ref2.name);
    h.Str(t.isoClass);
    h.F64(t.lower);
    h.F64(t.upper);
    if (t.printedPress) // v179; only when set, so others keep their hash
      h.Str("printed_press");
  }
  if (const auto& pp = doc.PrintProcess()) {
    // The rows' content (v169): a profile edited under the same name
    // changes what a printed fit is judged at.
    h.Str(printfit::Fingerprint(*pp));
  } else {
    h.Str("-");
  }
}

uint64_t PartKey(const Document& doc) {
  inputhash::Hasher h;
  h.Str("requirement-part/1");
  h.Bodies(doc.GetBodies());
  HashPartExtras(h, doc);
  return h.Digest();
}

// What check_literals reads: the features as saved, the part's variables
// and the project's, beside the geometry.
void HashLiteralsInputs(inputhash::Hasher& h, const ui::AppState& state,
                        const Document& doc) {
  h.Bodies(doc.GetBodies());
  for (size_t i = 0; i < doc.FeatureCount(); i++)
    h.Json(DocumentIO::FeatureToJson(*doc.GetFeature(i)));
  for (const Parameter& p : doc.GetParameters()) {
    h.Str(p.name);
    h.Str(p.expression);
  }
  for (const Parameter& p : state.projectParameters) {
    h.Str(p.name);
    h.Str(p.expression);
    h.F64(p.value);
  }
}

uint64_t AssemblyKey(ui::AppState& state, const AssemblyDocument& asmDoc) {
  inputhash::Hasher h;
  h.U64(motion::AssemblyInputKey(asmDoc, ui::PartResolverFor(state),
                                 ui::AssemblyResolverFor(state)));
  // What a fit, a seat or a fastener check reads beyond the bodies: the
  // tolerances on the placed parts' faces and how each is made.
  const auto parts = ui::PartResolverFor(state);
  for (const auto& [part, configuration] : assembly::SubtreePartConfigurations(
           asmDoc, ui::AssemblyResolverFor(state))) {
    h.U64(part);
    if (const Document* d = parts(part, configuration))
      HashPartExtras(h, *d);
  }
  return h.Digest();
}

uint64_t JobKey(ui::AppState& state, const cam::JobDocument& job) {
  inputhash::Hasher h;
  h.Str("requirement-job/1");
  const auto parts = ui::PartResolverFor(state);
  for (const cam::JobPart& jp : job.GetParts()) {
    h.U64(jp.id);
    h.U64(jp.sourceDocId);
    h.U64(jp.configuration);
    if (const Document* d = parts(jp.sourceDocId, jp.configuration))
      h.U64(PartKey(*d));
    else
      h.U64(0);
  }
  // Each setup as it reads back: a field the loader fills in (an
  // operation's side) is written once the job has been saved, so the
  // session that built the job and one that reopened it hash the same job.
  for (const cam::Setup& s : job.GetSetups())
    h.Json(DocumentIO::CamSetupToJson(
        DocumentIO::CamSetupFromJson(DocumentIO::CamSetupToJson(s))));
  return h.Digest();
}

// Whether the requirement's references resolve now, and the input hash of
// what it would measure. `why` non-empty: unresolved.
uint64_t CurrentHash(ui::AppState& state, const req::Requirement& r,
                     std::string& why) {
  why.clear();
  const std::string kind = r.Kind();
  const json& m = r.measure;
  inputhash::Hasher h;
  h.Str(kind);
  if (kind == "expression") {
    // The names and values the formula and its bound read, in the scope
    // they read, the configuration it reads them in, and every cell of a
    // table a lookup names: a cell edited makes it stale.
    ExprScope scope;
    ScopeOf(state, r, scope);
    if (!scope.why.empty()) {
      why = scope.why;
      return 0;
    }
    if (const std::string missing = MissingName(r, scope); !missing.empty()) {
      why = missing;
      return 0;
    }
    h.Str(scope.name);
    std::set<std::string> tables;
    for (const std::string& text : req::ExpressionTexts(r)) {
      h.Str(text);
      for (const std::string& name : req::IdentifiersOf(text)) {
        h.Str(name);
        h.F64(scope.Table().at(name));
      }
      for (const std::string& t : req::LookupTablesOf(text))
        tables.insert(t);
    }
    h.U64(m.value("configuration", uint64_t{0}));
    for (const std::string& t : tables) {
      std::map<std::string, double> cells;
      for (const auto& [name, v] : scope.Table()) {
        std::string tn, k, c;
        if (lookuptable::SplitCellSymbol(name, tn, k, c) && tn == t)
          cells[name] = v;
      }
      h.Str(t);
      for (const auto& [name, v] : cells) {
        h.Str(name);
        h.F64(v);
      }
    }
    return h.Digest();
  }
  for (const std::string& name : ExpressionNames(r)) {
    h.Str(name);
    const auto it = state.projectSymbols.find(name);
    h.F64(it == state.projectSymbols.end()
              ? std::numeric_limits<double>::quiet_NaN()
              : it->second);
  }
  if (kind == "check_literals" && m.contains("documents")) {
    for (const json& id : m.at("documents")) {
      const auto slot = DocSlot(state, id.get<uint64_t>(), "part", &why);
      if (!slot)
        return 0;
      if (const Document* d = ui::SlotDoc(state, *slot)) {
        h.U64(state.parts[*slot].id);
        HashLiteralsInputs(h, state, *d);
      }
    }
    return h.Digest();
  }
  if (kind == "check_literals" && !m.contains("document")) {
    h.Str("every part");
    for (size_t i = 0; i < state.parts.size(); i++)
      if (state.parts[i].kind == ui::DocKind::Part)
        if (const Document* d = ui::SlotDoc(state, i)) {
          h.U64(state.parts[i].id);
          HashLiteralsInputs(h, state, *d);
        }
    return h.Digest();
  }
  const auto slot =
      DocSlot(state, r.Document(), req::DocumentKindFor(kind), &why);
  if (!slot)
    return 0;
  const ui::DocKind dk = state.parts[*slot].kind;
  if (dk == ui::DocKind::Part) {
    const Document* doc = ui::SlotDoc(state, *slot);
    if (!doc) {
      why = "document " + std::to_string(r.Document()) + " is not loaded";
      return 0;
    }
    if (kind == "study_probe" || kind == "study_result") {
      const uint64_t sid = m.at("study").get<uint64_t>();
      const fem::Study* s = doc->FindStudy(sid);
      if (!s) {
        why = "study " + std::to_string(sid) + " is gone from '" +
              state.parts[*slot].name + "'";
        return 0;
      }
      std::vector<ElementRef> faces;
      if (m.contains("face"))
        faces.push_back(FaceOf(m.at("face")));
      if (m.contains("readings"))
        for (const auto& [name, rd] : m.at("readings").items())
          faces.push_back(FaceOf(rd.at("face")));
      for (const ElementRef& f : faces) {
        fem::ResolvedFace rf;
        std::string err;
        if (!fem::ResolveFace(*doc, f, "probe", rf, &err)) {
          why = err;
          return 0;
        }
      }
      h.U64(fem::StudyInputKey(*doc, *s));
    } else if (kind == "check_literals") {
      HashLiteralsInputs(h, state, *doc);
    } else if (kind == "section_area" && m.contains("bodies")) {
      // A wall renamed or deleted is a reference gone, not a smaller area.
      try {
        (void)ResolvePartBodies(*doc, m.at("bodies"), "bodies");
      } catch (const std::exception& e) {
        why = e.what();
        return 0;
      }
      h.U64(PartKey(*doc));
    } else {
      h.U64(PartKey(*doc));
    }
    return h.Digest();
  }
  if (dk == ui::DocKind::Assembly) {
    const AssemblyDocument* a = ui::SlotAssembly(state, *slot);
    if (!a) {
      why = "document " + std::to_string(r.Document()) + " is not loaded";
      return 0;
    }
    const auto needOcc = [&](const char* key) {
      const uint64_t id = m.at(key).get<uint64_t>();
      if (a->FindById(id) == nullptr) {
        why = "occurrence " + std::to_string(id) + " (" + key +
              ") is gone from '" + state.parts[*slot].name + "'";
        return false;
      }
      return true;
    };
    const auto needStudy = [&](const char* key) -> const MotionStudy* {
      const uint64_t id = m.at(key).get<uint64_t>();
      const MotionStudy* s = a->FindMotionStudyById(id);
      if (!s)
        why = "motion study " + std::to_string(id) + " is gone from '" +
              state.parts[*slot].name + "'";
      return s;
    };
    if (kind == "clearance" || kind == "contact" || kind == "seal" ||
        kind == "insertion") {
      if (!needOcc("a") || !needOcc("b"))
        return 0;
    }
    if (kind == "check_seat" || kind == "bearing_bore")
      for (const json& o : m.at("occurrences"))
        if (a->FindById(o.get<uint64_t>()) == nullptr) {
          why = "bearing occurrence " + o.dump() + " is gone from '" +
                state.parts[*slot].name + "'";
          return 0;
        }
    if ((kind == "fit_class" || (kind == "insertion" && m.contains("mate"))) &&
        a->FindMateById(m.at("mate").get<uint64_t>()) == nullptr) {
      why = "mate " + m.at("mate").dump() + " is gone from '" +
            state.parts[*slot].name + "'";
      return 0;
    }
    const MotionStudy* study = nullptr;
    if (kind == "motion_clean" || ((kind == "clearance" || kind == "seal") &&
                                   m.contains("motion_study"))) {
      study = needStudy(kind == "motion_clean" ? "study" : "motion_study");
      if (!study)
        return 0;
    }
    if (study) {
      h.U64(motion::InputKey(*a, ui::PartResolverFor(state),
                             ui::AssemblyResolverFor(state), *study));
    } else if (kind == "clearance" || kind == "contact" || kind == "seal" ||
               kind == "insertion") {
      // Only what the gap is between: each side's placement and what it
      // places. A part moved elsewhere in the assembly is not this answer's
      // business, and a key that said it was would cry wolf.
      for (const char* key : {"a", "b"}) {
        const Occurrence* o = a->FindById(m.at(key).get<uint64_t>());
        h.Json(DocumentIO::OccurrenceToJson(*o));
        if (o->refKind == OccurrenceRef::Assembly) {
          if (const AssemblyDocument* sub =
                  ui::AssemblyDocById(state, o->partId))
            h.U64(AssemblyKey(state, *sub));
        } else if (const Document* d = ui::PartResolverFor(state)(
                       o->partId, o->configurationId)) {
          h.U64(PartKey(*d));
        }
      }
    } else if (kind == "assembly_sequence") {
      // Every placed part, and the sequence itself.
      h.U64(AssemblyKey(state, *a));
      h.Json(DocumentIO::AssemblySequenceToJson(a->GetSequence()));
    } else {
      h.U64(AssemblyKey(state, *a));
    }
    return h.Digest();
  }
  if (dk == ui::DocKind::Job) {
    const cam::JobDocument* job = ui::SlotJob(state, *slot);
    if (!job) {
      why = "document " + std::to_string(r.Document()) + " is not loaded";
      return 0;
    }
    const uint64_t setup = m.at("setup").get<uint64_t>();
    if (job->FindSetup(setup) == nullptr) {
      why = "setup " + std::to_string(setup) + " is gone from '" +
            state.parts[*slot].name + "'";
      return 0;
    }
    h.U64(JobKey(state, *job));
    return h.Digest();
  }
  why = "a requirement cannot measure a drawing";
  return 0;
}

// ---- Rows
// --------------------------------------------------------------------

std::string NoteFor(const req::Requirement& r, const std::string& st,
                    const std::string& why) {
  if (st == "unresolved")
    return why.empty() ? r.evidence.error : why;
  if (st == "stale")
    return "the model changed since it was checked: check again";
  if (st == "unchecked")
    return r.evidence.taken ? "edited since it was checked" : "never checked";
  return "";
}

json Row(ui::AppState& state, const req::Requirement& r) {
  std::string why;
  const uint64_t hash = CurrentHash(state, r, why);
  const std::string st = req::StateOf(r, why, hash);
  json j{{"id", r.id},
         {"name", r.name},
         {"revision", r.revision},
         {"state", st},
         {"measure", r.measure},
         {"bound", req::BoundToJson(r.bound)}};
  if (!r.source.empty())
    j["source"] = r.source;
  const std::string note = NoteFor(r, st, why);
  if (!note.empty())
    j["note"] = note;
  const req::Evidence& e = r.evidence;
  if (e.taken) {
    if (e.value)
      j["value"] = *e.value;
    if (!e.unit.empty())
      j["unit"] = e.unit;
    if (e.limit)
      j["limit"] = *e.limit;
    if (e.limitMin)
      j["limit_min"] = *e.limitMin;
    if (e.limitMax)
      j["limit_max"] = *e.limitMax;
    if (!e.detail.is_null() && !e.detail.empty())
      j["detail"] = e.detail;
    if (!e.checkedAt.empty())
      j["checked_at"] = e.checkedAt;
    j["input_hash"] = inputhash::Hex(e.inputHash);
    j["evidence_revision"] = e.revision;
  }
  return j;
}

json Summary(const json& rows) {
  json s{{"pass", 0},
         {"fail", 0},
         {"stale", 0},
         {"unresolved", 0},
         {"unchecked", 0}};
  for (const json& r : rows)
    s[r.at("state").get<std::string>()] =
        s[r.at("state").get<std::string>()].get<int>() + 1; // checked: ours
  return s;
}

json RefreshExpressions(ui::AppState& state); // below, beside CheckOne

json RowsJson(ui::AppState& state, const std::vector<uint64_t>* only) {
  // An expression requirement is measured wherever its values moved: a row
  // read here is never stale for want of a Check.
  RefreshExpressions(state);
  json rows = json::array();
  for (const req::Requirement& r : state.requirements)
    if (!only || std::find(only->begin(), only->end(), r.id) != only->end())
      rows.push_back(Row(state, r));
  json out{{"requirements", rows}, {"summary", Summary(rows)}};
  bool all = !rows.empty();
  for (const json& r : rows)
    all = all && r.at("state") == "pass";
  out["all_pass"] = all;
  return out;
}

req::Requirement* FindRequirement(ui::AppState& state, uint64_t id) {
  for (req::Requirement& r : state.requirements)
    if (r.id == id)
      return &r;
  return nullptr;
}

req::Requirement& NeedRequirement(ui::AppState& state, uint64_t id) {
  if (req::Requirement* r = FindRequirement(state, id))
    return *r;
  std::vector<std::string> have;
  for (const req::Requirement& r : state.requirements)
    have.push_back(std::to_string(r.id) + " (" + r.name + ")");
  throw std::runtime_error("no requirement " + std::to_string(id) +
                           "; requirements: " + KnownList(have));
}

void MarkRequirementsEdited(ui::AppState& state) {
  state.requirementsEdits++;
  state.projectMetaDirty = true;
}

// The references a new or edited requirement names, checked when it is
// stated: a requirement on a document, study or occurrence that does not
// exist is refused rather than saved unresolved.
void RefuseUnresolvable(ui::AppState& state, const req::Requirement& r) {
  const std::string kind = r.Kind();
  std::string why;
  if (kind == "expression") {
    ExprScope scope;
    ScopeOf(state, r, scope);
    if (!scope.why.empty())
      throw std::runtime_error(
          scope.why + (r.measure.contains("configuration")
                           ? std::string()
                           : ". Documents: " + DocumentCandidates(state)));
    // Compiled and resolved now, so a typo is refused by name rather than
    // saved as a requirement that is unresolved from its first breath.
    const auto check = [&](const std::string& text, const char* what) {
      if (text.empty())
        return;
      if (const std::string no =
              req::ScopeRefusal(text, scope.Table(), what, scope.name);
          !no.empty())
        throw std::runtime_error(no);
    };
    check(r.measure.at("expression").get<std::string>(), "measure.expression");
    check(r.bound.valueExpr, "bound.value");
    check(r.bound.minExpr, "bound.min");
    check(r.bound.maxExpr, "bound.max");
    return;
  }
  if (kind == "check_literals" && !r.measure.contains("document")) {
    CurrentHash(state, r, why);
    if (!why.empty())
      throw std::runtime_error(why +
                               ". Documents: " + DocumentCandidates(state));
    return;
  }
  if (!DocSlot(state, r.Document(), req::DocumentKindFor(kind), &why))
    throw std::runtime_error(why + ". Documents: " + DocumentCandidates(state));
  if (r.measure.contains("readings"))
    for (const auto& [name, rd] : r.measure.at("readings").items())
      if (!ExpressionEngine::Default().IsValidParameterName(name))
        throw std::runtime_error("study_probe reading '" + name +
                                 "' is not a name an expression can use");
  CurrentHash(state, r, why);
  if (!why.empty())
    throw std::runtime_error(why);
}

// ---- Measuring
// -----------------------------------------------------------------

// Runs `method` through the domain handlers, as a client's call would run.
json Call(ui::AppState& state, const std::string& method, const json& params) {
  RpcParams p{params};
  Document* doc = state.document.get();
  if (doc)
    if (auto r = HandleModelingRpc(state, *doc, method, p))
      return *r;
  if (auto r = HandleSimulationRpc(state, doc, method, p))
    return *r;
  if (auto r = HandleCamRpc(state, doc, method, p))
    return *r;
  if (auto r = HandleInspectionRpc(state, doc, method, p))
    return *r;
  if (auto r = HandleMotionRpc(state, method, p))
    return *r;
  if (auto r = HandleAssemblyRpc(state, method, p))
    return *r;
  throw std::runtime_error("internal: no handler answers " + method);
}

// Makes the requirement's document the active tab, keeping the camera where
// it is (a first visit would otherwise frame the new tab).
void Activate(ui::AppState& state, uint64_t docId) {
  const auto slot = ui::SlotIndexById(state, docId);
  if (!slot || static_cast<int>(*slot) == state.activePart)
    return;
  const bool framed = state.parts[*slot].everFramed;
  state.parts[*slot].everFramed = true;
  ui::SetActivePart(state, *slot);
  state.parts[*slot].everFramed = framed;
}

void ActivateFirstPart(ui::AppState& state) {
  if (state.document)
    return;
  for (const ui::PartSlot& s : state.parts)
    if (s.kind == ui::DocKind::Part) {
      Activate(state, s.id);
      return;
    }
}

struct Taken;

// A measure's expensive half, for a worker
// (docs/requirements/README.md#checking-is-a-job): it owns everything it reads
// -- deep copies taken on the main thread -- and fills the reading, or throws.
// `cancel` is read between its pieces; `fraction` is how far through it is,
// 0..1.
using Work = std::function<void(Taken&, const std::atomic<bool>& cancel,
                                std::atomic<double>& fraction)>;

struct Taken {
  uint64_t waitJob = 0; // a study job to wait for before measuring again
  bool waitCam = false; // CAM passes being cut
  std::optional<double> value;
  std::string unit;
  bool pass = false;
  std::string error;
  json detail;
  // Set instead of a reading when the measure was asked off the frame
  // (Ctx::async) and the reading is worth a worker.
  Work work;
};

struct Ctx {
  bool async = false;
  bool recompute = false;
};

// run_study, or run_motion_study: a job to wait for, or nothing (the result
// is current now, computed or reused).
uint64_t RunStudy(ui::AppState& state, const char* method, const char* idKey,
                  uint64_t id, const Ctx& ctx) {
  json params{{idKey, id}};
  if (ctx.async)
    params["async"] = true;
  if (ctx.recompute)
    params["recompute"] = true;
  const json r = Call(state, method, params);
  if (r.contains("job") && !r.value("reused", false))
    return r.at("job").get<uint64_t>();
  return 0;
}

double Magnitude(const json& v) {
  double s = 0.0;
  for (const json& c : v)
    s += c.get<double>() * c.get<double>();
  return std::sqrt(s);
}

Taken MeasureStudyProbe(ui::AppState& state, const json& m, const Ctx& ctx) {
  Taken t;
  const uint64_t sid = m.at("study").get<uint64_t>();
  if ((t.waitJob = RunStudy(state, "run_study", "id", sid, ctx)))
    return t;
  const auto probe = [&](const json& face, const json* at) {
    json params{{"id", sid}, {"face", face}};
    if (m.contains("case"))
      params["case"] = m.at("case");
    if (at) {
      const glm::dvec3 p = *PointOf(state, *at);
      params["at"] = json::array({p.x, p.y, p.z});
    }
    return Call(state, "probe_study", params);
  };
  if (m.contains("face")) {
    const json* at = m.contains("at") ? &m.at("at") : nullptr;
    const std::string q = m.value(
        "quantity", std::string(at ? "displacement_at" : "translation"));
    const json r = probe(m.at("face"), at);
    if (q == "translation") {
      t.value = r.at("translation_magnitude_mm").get<double>();
      t.unit = "mm";
    } else if (q == "von_mises_max" || q == "von_mises_mean") {
      // The stress ON the face (a tooth root, a fillet), not the global
      // peak: docs/simulation/README.md#probing-a-face.
      const char* key =
          q == "von_mises_max" ? "von_mises_max_mpa" : "von_mises_mean_mpa";
      if (!r.contains(key))
        throw std::runtime_error("the face carries no stress in the study's "
                                 "result");
      t.value = r.at(key).get<double>();
      t.unit = "MPa";
    } else if (q == "rotation") {
      if (!r.contains("rotation_magnitude_deg"))
        throw std::runtime_error("the face's nodes lie on a line: its "
                                 "rotation is not determined");
      t.value = r.at("rotation_magnitude_deg").get<double>();
      t.unit = "deg";
    } else {
      t.value = Magnitude(r.at("displacement_at_mm"));
      t.unit = "mm";
    }
    t.detail = {{"translation_mm", r.at("translation_mm")}};
    if (r.contains("von_mises_max_mpa")) {
      t.detail["von_mises_max_mpa"] = r.at("von_mises_max_mpa");
      t.detail["von_mises_max_at_mm"] = r.at("von_mises_max_at_mm");
      t.detail["von_mises_mean_mpa"] = r.at("von_mises_mean_mpa");
    }
    if (r.contains("displacement_at_mm"))
      t.detail["displacement_at_mm"] = r.at("displacement_at_mm");
  } else {
    // A formula over several faces' readings: what a mechanism carried by
    // more than one face did (the pan/tilt head's lens, held by two bearing
    // bores, a shoulder and the worm on a ledge).
    SymbolTable sym;
    json readings = json::object();
    for (const auto& [name, rd] : m.at("readings").items()) {
      const json* at = rd.contains("at") ? &rd.at("at") : nullptr;
      const json r = probe(rd.at("face"), at);
      const auto put = [&](const char* suffix, const json& v) {
        for (int k = 0; k < 3; k++)
          sym[name + "_" + suffix + "xyz"[k]] = v.at(k).get<double>();
      };
      put("t", r.at("translation_mm"));
      put("c", r.at("centroid_mm"));
      put("r", r.contains("rotation_rad") ? r.at("rotation_rad")
                                          : json::array({0.0, 0.0, 0.0}));
      if (r.contains("displacement_at_mm"))
        put("d", r.at("displacement_at_mm"));
      if (r.contains("von_mises_max_mpa")) {
        sym[name + "_vmmax"] = r.at("von_mises_max_mpa").get<double>();
        sym[name + "_vmmean"] = r.at("von_mises_mean_mpa").get<double>();
      }
      readings[name] = {{"translation_mm", r.at("translation_mm")}};
      if (r.contains("displacement_at_mm"))
        readings[name]["displacement_at_mm"] = r.at("displacement_at_mm");
    }
    t.value = Evaluate(state, m.at("expression").get<std::string>(), sym,
                       "expression");
    t.unit = m.value("unit", std::string("mm"));
    t.detail = {{"readings", readings}};
  }
  if (m.contains("case"))
    t.detail["case"] = m.at("case");
  return t;
}

Taken MeasureStudyResult(ui::AppState& state, const json& m, const Ctx& ctx) {
  Taken t;
  const uint64_t sid = m.at("study").get<uint64_t>();
  if ((t.waitJob = RunStudy(state, "run_study", "id", sid, ctx)))
    return t;
  const json r = Call(state, "study_results", {{"id", sid}});
  const json* src = &r;
  if (m.contains("case")) {
    src = nullptr;
    const json& want = m.at("case");
    if (r.contains("cases"))
      for (size_t k = 0; k < r.at("cases").size(); k++) {
        const json& c = r.at("cases")[k];
        if ((want.is_string() && c.at("name") == want) ||
            (want.is_number() && want.get<size_t>() == k))
          src = &c;
      }
    if (!src)
      throw std::runtime_error("the study has no load case " + want.dump());
  }
  const std::string q = m.at("quantity").get<std::string>();
  if (q == "max_displacement") {
    t.value = src->at("max_displacement_mm").at("value").get<double>();
    t.unit = "mm";
  } else if (q == "max_von_mises") {
    t.value = src->at("max_von_mises_mpa").at("value").get<double>();
    t.unit = "MPa";
  } else {
    if (!src->contains("factor_of_safety"))
      throw std::runtime_error("no factor of safety: a material without a "
                               "yield strength, or no stress");
    t.value = src->at("factor_of_safety").get<double>();
  }
  return t;
}

// The gap between two placed body sets, the tessellations' combined
// deflection subtracted in mesh mode. nullopt when it cannot be measured.
std::optional<double> Gap(const std::vector<Body>& A, const glm::dmat4& xa,
                          const std::vector<Body>& B, const glm::dmat4& xb,
                          bool mesh) {
  if (mesh) {
    // Two deflections can point opposite ways, so the SUM is what
    // |mesh - exact| is bounded by. Subtracting it is what makes a mesh
    // answer safe to gate on.
    double best = std::numeric_limits<double>::infinity(), tol = 0.0;
    bool known = false;
    for (const Body& ba : A)
      for (const Body& bb : B) {
        const collide::Gap g =
            collide::Nearest(ba.shape, xa, bb.shape, xb, 2.0);
        if (!g.known)
          continue;
        known = true;
        if (g.distance < best) {
          best = g.distance;
          tol = g.tolerance;
        }
      }
    if (!known)
      return std::nullopt;
    return best - tol;
  }
  const std::optional<Measure::SetContact> c =
      Measure::ContactBetween(A, xa, B, xb, true);
  if (!c || !c->distanceKnown)
    return std::nullopt;
  return c->volume > 1e-9 ? -1.0 : c->distance;
}

// How long a clearance measure holds before it measures (tests only:
// SetTestRequirementHold, or LOBOLE_TEST_REQUIREMENT_MS), so a test can
// watch a slow check run beside the frame and cancel it. In the measure
// itself, wherever it runs: a measure put back on the frame stalls it.
std::atomic<int> g_testHoldMs{-1}; // -1: not read from the environment yet

int TestHoldMs() {
  int ms = g_testHoldMs.load();
  if (ms < 0) {
    const char* env = std::getenv("LOBOLE_TEST_REQUIREMENT_MS");
    ms = env ? std::max(0, std::atoi(env)) : 0;
    g_testHoldMs.store(ms);
  }
  return ms;
}

// What the meshes prove of the exact gap: it is at least the tessellations'
// distance less both deflections (each triangle lies within its deflection
// of its surface). -inf when a pair has no tessellation, which proves
// nothing.
double MeshFloor(const std::vector<Body>& A, const glm::dmat4& xa,
                 const std::vector<Body>& B, const glm::dmat4& xb) {
  double floor = std::numeric_limits<double>::infinity();
  for (const Body& ba : A)
    for (const Body& bb : B) {
      if (ba.shape.GetShape().IsNull() || bb.shape.GetShape().IsNull())
        continue; // the exact measure skips it too
      const collide::Gap g = collide::Nearest(ba.shape, xa, bb.shape, xb, 2.0);
      if (!g.known)
        return -std::numeric_limits<double>::infinity();
      floor = std::min(floor, g.distance - g.tolerance);
    }
  return floor;
}

// One clearance's inputs, owned: both sides' bodies, and each pose to
// measure them at (the sides' moves from where they stand now). On a
// worker's behalf the bodies are deep copies, whose collision meshes are
// forgotten with them.
struct ClearanceInputs {
  std::vector<Body> A, B;
  struct Pose {
    glm::dmat4 xa{1.0}, xb{1.0};
    double driver = 0.0;
  };
  std::vector<Pose> poses;
  bool mesh = true;
  bool copies = false;
  bool overStudy = false;
  bool turns = false;   // the driver is a rotation, held in radians
  uint64_t studyId = 0; // for the messages
  int holdMs = 0;       // TestHoldMs
  ClearanceInputs() = default;
  ClearanceInputs(const ClearanceInputs&) = delete;
  ClearanceInputs& operator=(const ClearanceInputs&) = delete;
  ~ClearanceInputs() {
    if (copies)
      for (const std::vector<Body>* side : {&A, &B})
        for (const Body& b : *side)
          collide::Forget(b.shape);
  }
};

// The smallest gap over the poses, into `t`. Exact mode measures the poses
// in order of what their meshes prove (MeshFloor) and stops at the first
// whose floor is no smaller than the best exact gap found: every pose left
// is at least that far apart, so the minimum and where it is are the ones
// measuring every pose would give, from the few poses that can hold it.
void SmallestGap(const ClearanceInputs& in, Taken& t,
                 const std::atomic<bool>& cancel,
                 std::atomic<double>& fraction) {
  const size_t n = in.poses.size();
  double worst = std::numeric_limits<double>::infinity();
  size_t worstAt = 0, measured = 0, exact = 0;
  const auto take = [&](size_t i) {
    const ClearanceInputs::Pose& p = in.poses[i];
    const auto g = Gap(in.A, p.xa, in.B, p.xb, in.mesh);
    if (!g)
      return;
    measured++;
    if (*g < worst) {
      worst = *g;
      worstAt = i;
    }
  };
  const auto stopped = [&] {
    if (cancel.load())
      throw std::runtime_error("cancelled");
  };
  const auto until =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(in.holdMs);
  while (std::chrono::steady_clock::now() < until) {
    stopped();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (in.mesh) {
    for (size_t i = 0; i < n; i++) {
      stopped();
      take(i);
      fraction.store(static_cast<double>(i + 1) / static_cast<double>(n));
    }
  } else {
    std::vector<std::pair<double, size_t>> order;
    order.reserve(n);
    for (size_t i = 0; i < n; i++) {
      stopped();
      const ClearanceInputs::Pose& p = in.poses[i];
      order.emplace_back(MeshFloor(in.A, p.xa, in.B, p.xb), i);
      fraction.store(0.2 * static_cast<double>(i + 1) / static_cast<double>(n));
    }
    std::stable_sort(
        order.begin(), order.end(),
        [](const auto& a, const auto& b) { return a.first < b.first; });
    for (size_t k = 0; k < order.size(); k++) {
      if (order[k].first >= worst)
        break;
      stopped();
      take(order[k].second);
      exact++;
      fraction.store(0.2 + 0.8 * static_cast<double>(k + 1) /
                               static_cast<double>(order.size()));
    }
  }
  if (measured == 0) {
    if (!in.overStudy)
      throw std::runtime_error("the gap could not be measured (no "
                               "tessellation, or the kernel declined it)");
    throw std::runtime_error("no sample of motion study " +
                             std::to_string(in.studyId) + " could be measured");
  }
  t.value = worst;
  t.unit = "mm";
  t.detail = {{"mode", in.mesh ? "mesh" : "exact"}};
  if (!in.overStudy)
    return;
  // Every pose is accounted for: measured, or proved no closer than the
  // smallest by its meshes.
  t.detail["samples"] = in.mesh ? measured : n;
  if (!in.mesh)
    t.detail["exact_samples"] = exact;
  // The driver in the unit the study speaks (motion studies take degrees
  // for a rotation and store radians).
  const double at = in.poses[worstAt].driver;
  if (in.turns)
    t.detail["smallest_at_driver_deg"] = angles::ToDegrees(at);
  else
    t.detail["smallest_at_driver_mm"] = at;
}

// Where a motion study puts occurrences a and b, as the matrices that carry
// each from where it stands now, at every sample, and whether the driver
// turns. A study job to wait for when the study is not current (nothing
// filled then). Throws when a sample did not solve: the answer there is
// unknown, not the best of the rest.
uint64_t StudyPoses(ui::AppState& state, uint64_t a, uint64_t b, uint64_t sid,
                    const Ctx& ctx, std::vector<ClearanceInputs::Pose>& poses,
                    bool& turns) {
  if (const uint64_t job =
          RunStudy(state, "run_motion_study", "study", sid, ctx))
    return job;
  const motion::Result* res = ui::MotionResultFor(state, sid);
  if (!res)
    throw std::runtime_error("motion study " + std::to_string(sid) +
                             " has no result");
  const MotionStudy* study = state.assembly->FindMotionStudyById(sid);
  turns = study && study->driverDof == MateDof::Rotation;
  const Occurrence* oa = state.assembly->FindById(a);
  const Occurrence* ob = state.assembly->FindById(b);
  const glm::dmat4 nowA = ui::OccurrenceMatrix(*oa);
  const glm::dmat4 nowB = ui::OccurrenceMatrix(*ob);
  const auto worldOf = [](const motion::Sample& s,
                          uint64_t id) -> std::optional<glm::dmat4> {
    for (const auto& [occ, w] : s.worlds)
      if (occ == id)
        return w;
    return std::nullopt;
  };
  size_t unsolved = 0;
  for (const motion::Sample& s : res->samples) {
    if (!s.solved) {
      unsolved++;
      continue;
    }
    const auto wa = worldOf(s, a), wb = worldOf(s, b);
    if (!wa || !wb)
      continue;
    poses.push_back(
        {*wa * glm::inverse(nowA), *wb * glm::inverse(nowB), s.value});
  }
  if (unsolved > 0)
    throw std::runtime_error(std::to_string(unsolved) + " of " +
                             std::to_string(res->samples.size()) +
                             " samples of motion study " + std::to_string(sid) +
                             " did not solve, so the answer there is unknown");
  if (poses.empty())
    throw std::runtime_error("no sample of motion study " +
                             std::to_string(sid) + " could be measured");
  return 0;
}

Taken MeasureClearance(ui::AppState& state, const json& m, const Ctx& ctx) {
  Taken t;
  t.unit = "mm";
  const uint64_t a = m.at("a").get<uint64_t>(), b = m.at("b").get<uint64_t>();
  auto in = std::make_shared<ClearanceInputs>();
  in->mesh = m.value("mode", std::string("mesh")) == "mesh";
  // Off the frame, the worker gets its own copies: a boolean and an
  // extrema WRITE p-curves and triangulations into their arguments, and the
  // frame is drawing these bodies
  // (docs/inspect/README.md#query-booleans-are-non-destructive).
  in->copies = ctx.async;
  in->holdMs = TestHoldMs();
  for (const auto& [id, side] : {std::pair{a, &in->A}, std::pair{b, &in->B}})
    for (const Body& body : ui::OccurrenceSubtreeBodies(state, id)) {
      Body nb = body;
      if (in->copies)
        nb.shape = body.shape.DetachedUnlocated().LocatedLike(body.shape);
      side->push_back(std::move(nb));
    }
  if (in->A.empty() || in->B.empty())
    throw std::runtime_error("one side places no visible matter (hidden, or a "
                             "part with no bodies), so there is no gap");
  if (!m.contains("motion_study")) {
    in->poses.push_back({});
  } else {
    // The smallest gap over a motion study's samples: each side moved from
    // where it stands now to where the study put it.
    in->studyId = m.at("motion_study").get<uint64_t>();
    in->overStudy = true;
    if ((t.waitJob =
             StudyPoses(state, a, b, in->studyId, ctx, in->poses, in->turns)))
      return t;
  }
  if (ctx.async) {
    t.work = [in](Taken& out, const std::atomic<bool>& cancel,
                  std::atomic<double>& fraction) {
      SmallestGap(*in, out, cancel, fraction);
    };
    return t;
  }
  std::atomic<bool> never{false};
  std::atomic<double> fraction{0.0};
  SmallestGap(*in, t, never, fraction);
  return t;
}

Taken MeasureContact(ui::AppState& state, const json& m) {
  Taken t;
  const json r =
      Call(state, "interference", {{"a", m.at("a")}, {"b", m.at("b")}});
  const std::string st = r.value("state", std::string());
  const std::string want = m.at("expect").get<std::string>();
  t.pass = want == "not_clashing" ? st != "clash" && !st.empty() : st == want;
  t.detail = {{"state", st}};
  for (const char* k : {"volume_mm3", "distance", "press_fits"})
    if (r.contains(k))
      t.detail[k] = r.at(k);
  return t;
}

// A seal (docs/inspect/README.md#seals): the widest ring the contact of a
// and b closes round its axis, by its narrowest -- 0 when none closes -- at
// the worst sample of a motion study when one is named.
struct SealInputs {
  std::vector<Body> A, B;
  std::vector<ClearanceInputs::Pose> poses;
  bool copies = false, turns = false, overStudy = false;
  SealInputs() = default;
  SealInputs(const SealInputs&) = delete;
  SealInputs& operator=(const SealInputs&) = delete;
};

void SealOver(const SealInputs& in, Taken& t, const std::atomic<bool>& cancel,
              std::atomic<double>& fraction) {
  t.unit = "mm";
  double worst = std::numeric_limits<double>::infinity();
  for (size_t i = 0; i < in.poses.size(); i++) {
    const auto& p = in.poses[i];
    const auto r = Measure::Seal(in.A, p.xa, in.B, p.xb, 720, 1e-3, &cancel);
    if (cancel.load())
      throw std::runtime_error("cancelled");
    // Nothing touching is no seal: a reading of 0, and why.
    const double v =
        r ? (r->rings.empty() ? 0.0 : r->rings[0].leastWidthMm) : 0.0;
    if (v < worst) {
      worst = v;
      t.detail = r ? SealJson(*r)
                   : json{{"sealed", false},
                          {"least_width_mm", 0.0},
                          {"why", r.error()}};
      if (in.overStudy) {
        if (in.turns)
          t.detail["worst_at_driver_deg"] = angles::ToDegrees(p.driver);
        else
          t.detail["worst_at_driver_mm"] = p.driver;
      }
    }
    fraction.store(static_cast<double>(i + 1) /
                   static_cast<double>(in.poses.size()));
  }
  if (in.overStudy)
    t.detail["samples"] = in.poses.size();
  t.value = worst;
}

Taken MeasureSeal(ui::AppState& state, const json& m, const Ctx& ctx) {
  Taken t;
  t.unit = "mm";
  const uint64_t a = m.at("a").get<uint64_t>(), b = m.at("b").get<uint64_t>();
  auto in = std::make_shared<SealInputs>();
  in->copies = ctx.async;
  for (const auto& [id, side] : {std::pair{a, &in->A}, std::pair{b, &in->B}})
    for (const Body& body : ui::OccurrenceSubtreeBodies(state, id)) {
      Body nb = body;
      if (in->copies)
        nb.shape = body.shape.DetachedUnlocated().LocatedLike(body.shape);
      side->push_back(std::move(nb));
    }
  if (in->A.empty() || in->B.empty())
    throw std::runtime_error("one side places no visible matter (hidden, or a "
                             "part with no bodies), so there is no seal");
  if (!m.contains("motion_study")) {
    in->poses.push_back({});
  } else {
    in->overStudy = true;
    if ((t.waitJob =
             StudyPoses(state, a, b, m.at("motion_study").get<uint64_t>(), ctx,
                        in->poses, in->turns)))
      return t;
  }
  if (ctx.async) {
    t.work = [in](Taken& out, const std::atomic<bool>& cancel,
                  std::atomic<double>& fraction) {
      SealOver(*in, out, cancel, fraction);
    };
    return t;
  }
  std::atomic<bool> never{false};
  std::atomic<double> fraction{0.0};
  SealOver(*in, t, never, fraction);
  return t;
}

// An insertion (docs/inspect/README.md#insertion): a slid to its seat on b
// with no more travel under interference than its own press.
struct InsertionInputs {
  std::vector<Body> A, B;
  remote::InsertionCall call;
  InsertionInputs() = default;
  InsertionInputs(const InsertionInputs&) = delete;
  InsertionInputs& operator=(const InsertionInputs&) = delete;
};

void InsertionOver(const InsertionInputs& in, Taken& t,
                   const std::atomic<bool>& cancel,
                   std::atomic<double>& fraction) {
  const auto r = Measure::Insertion(in.A, glm::dmat4(1.0), in.B,
                                    glm::dmat4(1.0), in.call.point, in.call.dir,
                                    in.call.opt, &cancel, &fraction);
  if (cancel.load())
    throw std::runtime_error("cancelled");
  if (!r)
    throw std::runtime_error(r.error());
  t.pass = r->pass;
  t.detail = remote::InsertionJson(*r, in.call.axisFrom);
}

Taken MeasureInsertion(ui::AppState& state, const json& m, const Ctx& ctx) {
  Taken t;
  const uint64_t a = m.at("a").get<uint64_t>(), b = m.at("b").get<uint64_t>();
  auto in = std::make_shared<InsertionInputs>();
  in->call = remote::ReadInsertionCall(state, m, a, b);
  // A side with nothing to slide is refused by name: a frames-only part, a
  // hidden one (docs/inspect/README.md#insertion).
  for (uint64_t id : {a, b})
    if (const std::string why = remote::NoMatterWhy(state, id); !why.empty())
      throw std::runtime_error(why + ", so nothing slides");
  for (const auto& [id, side] : {std::pair{a, &in->A}, std::pair{b, &in->B}})
    for (const Body& body : ui::OccurrenceSubtreeBodies(state, id)) {
      Body nb = body;
      if (ctx.async)
        nb.shape = body.shape.DetachedUnlocated().LocatedLike(body.shape);
      side->push_back(std::move(nb));
    }
  if (in->A.empty() || in->B.empty())
    throw std::runtime_error("one side places no visible matter (hidden, or a "
                             "part with no bodies), so nothing slides");
  if (ctx.async) {
    t.work = [in](Taken& out, const std::atomic<bool>& cancel,
                  std::atomic<double>& fraction) {
      InsertionOver(*in, out, cancel, fraction);
    };
    return t;
  }
  std::atomic<bool> never{false};
  std::atomic<double> fraction{0.0};
  InsertionOver(*in, t, never, fraction);
  return t;
}

// An assembly sequence (docs/assemblies/README.md#assembly-sequences):
// every step goes home against everything placed before it.
Taken MeasureSequence(ui::AppState& state, const Ctx& ctx) {
  Taken t;
  auto check = remote::ReadSequenceCheck(state);
  const auto over = [check](Taken& out, const std::atomic<bool>& cancel,
                            std::atomic<double>& fraction) {
    json r = remote::RunSequenceCheck(*check, cancel, fraction);
    if (cancel.load())
      throw std::runtime_error("cancelled");
    out.pass = r.at("pass").get<bool>();
    out.detail = std::move(r);
  };
  if (ctx.async) {
    t.work = over;
    return t;
  }
  std::atomic<bool> never{false};
  std::atomic<double> fraction{0.0};
  over(t, never, fraction);
  return t;
}

Taken MeasureMass(ui::AppState& state, const json& m, bool mass) {
  Taken t;
  double volume = 0.0;
  std::optional<double> grams;
  const auto slot = ui::SlotIndexById(state, m.at("document").get<uint64_t>());
  if (slot && state.parts[*slot].kind == ui::DocKind::Part) {
    // A part through the function mass_properties answers with, on a const
    // Document, which a design sweep's point reads too.
    const Document* doc = ui::SlotDoc(state, *slot);
    if (doc == nullptr)
      throw std::runtime_error("the part is not loaded");
    const Measure::DocumentMass dm = partmeasure::MassOf(doc->GetBodies());
    volume = dm.volume;
    if (dm.massKnown)
      grams = dm.massG;
  } else {
    json params = json::object();
    if (state.document)
      params["part"] = m.at("document");
    const json r = Call(state, "mass_properties", params);
    const json& total = r.at("total");
    volume = total.at("volume_mm3").get<double>();
    if (total.contains("mass_g") && !total.at("mass_g").is_null())
      grams = total.at("mass_g").get<double>();
  }
  if (mass) {
    if (!grams)
      throw std::runtime_error("no mass: a body has no material density");
    t.value = *grams;
    t.unit = "g";
  } else {
    t.value = volume;
    t.unit = "mm3";
  }
  return t;
}

Taken MeasureCheckFits(ui::AppState& state, const json& m) {
  Taken t;
  const json r = Call(state, "check_fits", json::object());
  const int failed = r.value("failed", 0),
            unverified = r.value("unverified", 0);
  // No fit judged is not a pass: "every fit holds" over none says nothing.
  if (r.value("fits", json::array()).empty())
    throw std::runtime_error("the assembly declares no fit to check (a fit "
                             "is a round mate whose two faces both carry a "
                             "tolerance)");
  t.pass =
      failed == 0 && (unverified == 0 || m.value("allow_unverified", false));
  t.detail = json::object();
  for (const char* k :
       {"status", "passed", "warned", "failed", "reported", "unverified"})
    if (r.contains(k))
      t.detail[k] = r.at(k);
  json bad = json::array();
  for (const json& f : r.value("fits", json::array()))
    if (f.value("status", std::string()) == "fail" ||
        f.value("status", std::string()) == "unverified")
      bad.push_back({{"mate", f.value("mate_name", std::string())},
                     {"status", f.at("status")},
                     {"reason", f.value("reason", std::string())}});
  if (!bad.empty())
    t.detail["findings"] = bad;
  return t;
}

Taken MeasureFitClass(ui::AppState& state, const json& m) {
  Taken t;
  const json r = Call(state, "check_fits", json::object());
  const uint64_t mate = m.at("mate").get<uint64_t>();
  for (const json& f : r.value("fits", json::array()))
    if (f.value("mate", uint64_t{0}) == mate) {
      const std::string kind = f.value("kind", std::string());
      const std::string st = f.value("status", std::string());
      t.pass = kind == m.at("class").get<std::string>() && st != "fail" &&
               st != "unverified";
      t.detail = {{"kind", kind}, {"status", st}};
      if (f.contains("reason"))
        t.detail["reason"] = f.at("reason");
      return t;
    }
  throw std::runtime_error("mate " + std::to_string(mate) +
                           " declares no fit: check_fits judges a round mate "
                           "whose two faces both carry a tolerance");
}

Taken MeasureCheckSeat(ui::AppState& state, const json& m) {
  Taken t;
  t.pass = true;
  const bool allowUnverified = m.value("allow_unverified", false);
  json rows = json::array();
  for (const json& occ : m.at("occurrences")) {
    const json r = Call(state, "check_seat", {{"occurrence", occ}});
    std::set<std::string> rings;
    json bad = json::array();
    for (const json& seat : r.value("seats", json::array())) {
      if (seat.contains("error") || !seat.contains("part"))
        bad.push_back("no seat for the " +
                      seat.value("ring", std::string("?")) + " ring");
      else
        rings.insert(seat.value("ring", std::string()));
    }
    if (rings != std::set<std::string>{"inner", "outer"})
      bad.push_back("both rings were not seated");
    for (const json& c : r.value("checks", json::array())) {
      const std::string st = c.value("status", std::string());
      if (st == "pass" || (st == "unverified" && allowUnverified))
        continue;
      bad.push_back(c.value("characteristic", std::string()) + " " + st +
                    (c.contains("reason")
                         ? ": " + c.at("reason").get<std::string>()
                         : std::string()));
    }
    if (!bad.empty())
      t.pass = false;
    json row{{"occurrence", occ},
             {"designation", r.value("designation", std::string())},
             {"status", r.value("status", std::string())}};
    if (!bad.empty())
      row["findings"] = bad;
    rows.push_back(std::move(row));
  }
  t.detail = {{"bearings", rows}};
  return t;
}

// The smallest bore of the listed bearings: each read off its own catalogue
// row through check_seat, which names the bearing an occurrence places, so a
// bearing swapped for another row is measured as the row it is now. Bounded
// at_least the shaft's own minimum, it says the bearings still take the
// shaft the design asks for: a bearing is a catalogue row and does not
// follow a variable, so a heavier load regenerates the shaft's minimum past
// it without anything else saying so.
Taken MeasureBearingBore(ui::AppState& state, const json& m) {
  Taken t;
  json rows = json::array();
  std::optional<double> smallest;
  for (const json& occ : m.at("occurrences")) {
    const json r = Call(state, "check_seat", {{"occurrence", occ}});
    const double d = r.at("d").get<double>();
    smallest = smallest ? std::min(*smallest, d) : d;
    rows.push_back({{"occurrence", occ},
                    {"designation", r.value("designation", std::string())},
                    {"d", d}});
  }
  t.value = *smallest;
  t.unit = "mm";
  t.detail = {{"bearings", rows}};
  return t;
}

// A section's area through the `section` RPC: the material the plane
// crosses, or the area that material rings (the floor inside a building's
// outer walls, which a listing quotes and a plan scale gets wrong without
// anything saying so).
Taken MeasureSectionArea(ui::AppState& state, const json& m) {
  Taken t;
  const auto slot = ui::SlotIndexById(state, m.at("document").get<uint64_t>());
  const Document* doc = slot ? ui::SlotDoc(state, *slot) : nullptr;
  if (doc == nullptr)
    throw std::runtime_error("the part is not loaded");
  const partmeasure::SectionReading r =
      partmeasure::SectionAreaMeasure(*doc, m);
  t.value = r.value;
  t.unit = "mm2";
  t.detail = r.detail;
  return t;
}

Taken MeasureCheckFasteners(ui::AppState& state, const json& m) {
  Taken t;
  const json r = Call(state, "check_fasteners", json::object());
  const json allow = m.value("allow", json::array());
  json unexpected = json::array(), allowed = json::array();
  std::vector<int64_t> used(allow.size(), 0);
  for (const json& f : r.value("findings", json::array())) {
    const std::string part = f.value("part", std::string());
    const std::string why = f.value("why", std::string());
    bool ok = false;
    for (size_t k = 0; k < allow.size() && !ok; k++) {
      const json& a = allow[k];
      if (a.contains("max") && used[k] >= a.at("max").get<int64_t>())
        continue; // this entry has allowed all it may
      const bool partOk = !a.contains("part") ||
                          part.starts_with(a.at("part").get<std::string>());
      const bool whyOk =
          !a.contains("why") ||
          why.find(a.at("why").get<std::string>()) != std::string::npos;
      if (partOk && whyOk) {
        ok = true;
        used[k]++;
      }
    }
    (ok ? allowed : unexpected).push_back(f);
  }
  const auto count = [](const json& v) -> int64_t {
    return v.is_array()    ? static_cast<int64_t>(v.size())
           : v.is_number() ? v.get<int64_t>()
                           : 0;
  };
  const int64_t examined = count(r.value("examined", json(0)));
  const int64_t skipped = count(r.value("skipped", json(0)));
  t.pass = examined > 0 && unexpected.empty() &&
           (skipped == 0 || m.value("allow_skipped", false));
  t.detail = {{"examined", examined}, {"skipped", skipped}};
  if (!unexpected.empty())
    t.detail["findings"] = unexpected;
  if (!allowed.empty())
    t.detail["allowed"] = allowed;
  if (examined == 0)
    t.detail["note"] = "no fastener was examined";
  return t;
}

Taken MeasureCheckLiterals(ui::AppState& state, const json& m) {
  Taken t;
  std::vector<uint64_t> parts;
  if (m.contains("document"))
    parts.push_back(m.at("document").get<uint64_t>());
  else if (m.contains("documents"))
    parts = m.at("documents").get<std::vector<uint64_t>>();
  else
    for (const ui::PartSlot& s : state.parts)
      if (s.kind == ui::DocKind::Part)
        parts.push_back(s.id);
  json rows = json::array();
  size_t found = 0;
  for (uint64_t id : parts) {
    const json r = Call(state, "check_literals", {{"part", id}});
    const size_t n = r.value("literals", json::array()).size() +
                     r.value("placements", json::array()).size();
    found += n;
    if (n > 0) {
      const auto slot = ui::SlotIndexById(state, id);
      rows.push_back(
          {{"part", slot ? state.parts[*slot].name : ""},
           {"literals", r.value("literals", json::array()).size()},
           {"placements", r.value("placements", json::array()).size()}});
    }
  }
  t.pass = found == 0;
  t.detail = {{"parts", parts.size()}};
  if (!rows.empty())
    t.detail["findings"] = rows;
  return t;
}

Taken MeasureMotionClean(ui::AppState& state, const json& m, const Ctx& ctx) {
  Taken t;
  const uint64_t sid = m.at("study").get<uint64_t>();
  if ((t.waitJob = RunStudy(state, "run_motion_study", "study", sid, ctx)))
    return t;
  const motion::Result* res = ui::MotionResultFor(state, sid);
  if (!res)
    throw std::runtime_error("motion study " + std::to_string(sid) +
                             " has no result");
  if (!res->error.empty())
    throw std::runtime_error(res->error);
  // The findings, in the study's words: which pairs clash and from where,
  // which samples did not solve, where a branch jumped. A failed verdict
  // that names nothing sends the reader to re-run the study to find out.
  const MotionStudy* study = state.assembly->FindMotionStudyById(sid);
  const bool turns = study && study->driverDof == MateDof::Rotation;
  const auto driver = [turns](double v) {
    return turns ? std::format("{:.6g} deg", angles::ToDegrees(v))
                 : std::format("{:.6g} mm", v);
  };
  struct PairSeen {
    size_t samples = 0;
    double first = 0.0;
    bool rigid = false;
  };
  std::map<std::pair<std::string, std::string>, PairSeen> pairs;
  std::vector<double> unsolvedAt;
  json findings = json::array();
  size_t clashes = 0, unsolved = 0;
  for (const motion::Sample& s : res->samples) {
    clashes += s.clashes.size();
    if (!s.solved) {
      unsolved++;
      unsolvedAt.push_back(s.value);
    }
    for (const motion::Clash& c : s.clashes) {
      PairSeen& seen = pairs[std::minmax(c.aName, c.bName)];
      if (seen.samples++ == 0)
        seen.first = s.value;
      seen.rigid = seen.rigid || c.rigid;
    }
    if (s.branchJump)
      findings.push_back("'" + s.branchJump->name + "' jumped branch at " +
                         driver(s.branchJump->atValue));
  }
  for (const auto& [names, seen] : pairs)
    findings.push_back(
        "'" + names.first + "' clashes with '" + names.second + "' " +
        (seen.rigid ? std::string("(rigid to each other, a standing clash)")
                    : "at " + std::to_string(seen.samples) + " of " +
                          std::to_string(res->samples.size()) +
                          " samples, first at " + driver(seen.first)));
  if (!unsolvedAt.empty())
    findings.push_back(std::to_string(unsolvedAt.size()) +
                       " samples did not solve, first at " +
                       driver(unsolvedAt.front()));
  if (res->cancelled)
    findings.push_back("the run was cancelled");
  t.pass = res->clean && !res->cancelled;
  t.detail = {{"samples", res->samples.size()},
              {"clashes", clashes},
              {"unsolved", unsolved},
              {"pairs_checked", res->pairsChecked}};
  if (!t.pass)
    t.detail["findings"] = findings;
  return t;
}

Taken MeasureCamVerify(ui::AppState& state, const json& m, const Ctx& ctx) {
  Taken t;
  const uint64_t setupId = m.at("setup").get<uint64_t>();
  if (ctx.async && state.job) {
    // The passes a verify reads, cut on the workers rather than on the
    // frame: this setup's and every other setup's (cam_verify posts them
    // all). Measured once none is owed.
    bool owed = false;
    for (const cam::Setup& s : state.job->GetSetups())
      if (s.id == setupId || !s.operations.empty())
        owed |= ui::QueueCamPassesFor(state, s);
    if (owed || ui::CamPassesBusy(state)) {
      t.waitCam = true;
      return t;
    }
  }
  json params{{"setup", setupId}};
  if (m.contains("resolution"))
    params["resolution"] = m.at("resolution");
  const json r = Call(state, "cam_verify", params);
  json problems = json::array();
  if (!r.value("ok", false))
    problems.push_back("not safe (a gouge, a crash, or out of travel)");
  if (!r.value("complete", false))
    problems.push_back("not complete (material left on the part)");
  if (r.value("cuts_nothing", false))
    problems.push_back("the program cuts nothing");
  for (const json& f : r.value("failed_ops", json::array()))
    problems.push_back(
        "operation " + f.value("op", json()).dump() +
        " produced no toolpath: " + f.value("error", std::string()));
  for (const json& th : r.value("threads", json::array()))
    if (!(th.value("judged", false) && th.value("in_band", false)))
      problems.push_back("thread " + th.value("designation", std::string()) +
                         " not judged in its band");
  for (const json& f : r.value("fits", json::array()))
    if (!f.value("in_band", false))
      problems.push_back(f.value("class", std::string()) +
                         " wall not cut in its band");
  const json allow = m.value("allow_uncut", json::array());
  for (const json& u : r.value("uncut", json::array())) {
    const std::string what = u.value("what", std::string());
    bool ok = false;
    for (const json& a : allow)
      ok = ok || (a.is_string() && what.starts_with(a.get<std::string>()));
    if (!ok)
      problems.push_back("not cut: " + what);
  }
  t.pass = problems.empty();
  t.detail = {{"ok", r.value("ok", false)},
              {"complete", r.value("complete", false)}};
  for (const char* k : {"max_gouge_mm", "max_leftover_mm", "resolution_mm"})
    if (r.contains(k))
      t.detail[k] = r.at(k);
  if (!problems.empty())
    t.detail["findings"] = problems;
  return t;
}

// An `expression` requirement's reading: the formula in its scope, and
// every identifier it read with its value then.
Taken MeasureExpression(ui::AppState& state, const req::Requirement& r) {
  Taken t;
  ExprScope scope;
  ScopeOf(state, r, scope);
  if (!scope.why.empty())
    throw std::runtime_error(scope.why);
  const req::ExpressionReading reading = req::EvaluateIn(
      r.measure.at("expression").get<std::string>(), scope.Table());
  if (!reading.value)
    throw std::runtime_error("'" +
                             r.measure.at("expression").get<std::string>() +
                             "': " + reading.error);
  t.value = reading.value;
  t.unit = r.measure.value("unit", std::string("mm"));
  t.detail = scope.detail;
  json inputs = json::object();
  for (const auto& [name, v] : reading.inputs)
    inputs[name] = v;
  t.detail["inputs"] = inputs;
  return t;
}

Taken MeasureOn(ui::AppState& state, const req::Requirement& r,
                const Ctx& ctx) {
  const std::string kind = r.Kind();
  const json& m = r.measure;
  if (kind == "expression")
    return MeasureExpression(state, r); // reads values, visits no tab
  if (kind == "check_literals") {
    if (m.contains("document"))
      Activate(state, r.Document());
    else if (m.contains("documents"))
      Activate(state, m.at("documents").at(0).get<uint64_t>());
    else
      ActivateFirstPart(state);
    return MeasureCheckLiterals(state, m);
  }
  Activate(state, r.Document());
  if (kind == "study_probe")
    return MeasureStudyProbe(state, m, ctx);
  if (kind == "study_result")
    return MeasureStudyResult(state, m, ctx);
  if (kind == "clearance")
    return MeasureClearance(state, m, ctx);
  if (kind == "contact")
    return MeasureContact(state, m);
  if (kind == "seal")
    return MeasureSeal(state, m, ctx);
  if (kind == "insertion")
    return MeasureInsertion(state, m, ctx);
  if (kind == "assembly_sequence")
    return MeasureSequence(state, ctx);
  if (kind == "mass" || kind == "volume")
    return MeasureMass(state, m, kind == "mass");
  if (kind == "fit_class")
    return MeasureFitClass(state, m);
  if (kind == "check_fits")
    return MeasureCheckFits(state, m);
  if (kind == "check_seat")
    return MeasureCheckSeat(state, m);
  if (kind == "bearing_bore")
    return MeasureBearingBore(state, m);
  if (kind == "section_area")
    return MeasureSectionArea(state, m);
  if (kind == "check_fasteners")
    return MeasureCheckFasteners(state, m);
  if (kind == "motion_clean")
    return MeasureMotionClean(state, m, ctx);
  if (kind == "cam_verify")
    return MeasureCamVerify(state, m, ctx);
  throw std::runtime_error("unknown measure kind '" + kind + "'");
}

std::string Now() {
  return std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(
                                      std::chrono::system_clock::now()));
}

// A reading judged against the requirement's bound, into `e`. Throws when
// the reading has no value or a bound expression does not evaluate.
void Judge(ui::AppState& state, const req::Requirement& r, const Taken& t,
           req::Evidence& e) {
  e.value = t.value;
  e.unit = t.unit;
  e.detail = t.detail;
  if (!req::IsNumericKind(r.Kind())) {
    e.pass = t.pass;
    return;
  }
  if (!t.value)
    throw std::runtime_error("the measure returned no value");
  const req::Bound& b = r.bound;
  double lim = b.value, lo = b.min, hi = b.max;
  // An expression requirement's bound reads the scope its formula reads;
  // every other bound reads the project's variables.
  ExprScope scope;
  if (r.Kind() == "expression") {
    ScopeOf(state, r, scope);
    if (!scope.why.empty())
      throw std::runtime_error(scope.why);
  }
  const auto side = [&](const std::string& text, const char* what) {
    if (r.Kind() != "expression")
      return Evaluate(state, text, {}, what);
    const req::ExpressionReading v = req::EvaluateIn(text, scope.Table());
    if (!v.value)
      throw std::runtime_error(std::string(what) + " '" + text +
                               "': " + v.error);
    return *v.value;
  };
  if (!b.valueExpr.empty())
    lim = side(b.valueExpr, "bound");
  if (!b.minExpr.empty())
    lo = side(b.minExpr, "bound min");
  if (!b.maxExpr.empty())
    hi = side(b.maxExpr, "bound max");
  if (b.kind == "within") {
    e.limitMin = lo;
    e.limitMax = hi;
  } else {
    e.limit = lim;
  }
  e.pass = req::Holds(b, *t.value, lim, lo, hi);
}

// A measure's worker (Taken::work), and what it measured for: the
// requirement, its revision and the input hash when its copies were taken.
// Owned by the check's run; destroying it stops and joins the worker.
struct Pending {
  uint64_t id = 0, revision = 0, hash = 0;
  Work work;
  Taken taken;
  std::string error;
  std::atomic<bool> cancel{false};
  std::atomic<bool> done{false}; // the worker sets it LAST
  std::atomic<double> fraction{0.0};
  std::thread thread;
  Pending() = default;
  Pending(const Pending&) = delete;
  Pending& operator=(const Pending&) = delete;
  ~Pending() {
    cancel.store(true);
    if (thread.joinable())
      thread.join();
  }
  void Start() {
    thread = std::thread([this] {
      ArmOccSignalHandlers();
      try {
        work(taken, cancel, fraction);
      } catch (const Standard_Failure& e) {
        error = std::string("the measure failed: ") +
                (e.GetMessageString() ? e.GetMessageString()
                                      : "OpenCASCADE failure");
      } catch (const std::exception& e) {
        error = e.what();
      }
      work = nullptr; // the copies go on the worker, not the frame
      done.store(true, std::memory_order_release);
    });
  }
};

// One requirement measured and its evidence written. Returns a job or a CAM
// wait instead when the measure has to wait for one, or the measure's
// worker when it asked for one (Ctx::async only).
struct StepOutcome {
  uint64_t waitJob = 0;
  bool waitCam = false;
  std::unique_ptr<Pending> work;
};

StepOutcome CheckOne(ui::AppState& state, req::Requirement& r, const Ctx& ctx) {
  req::Evidence e;
  e.taken = true;
  e.revision = r.revision;
  e.checkedAt = Now();
  std::string why;
  CurrentHash(state, r, why);
  if (!why.empty()) {
    e.error = why;
  } else {
    try {
      Taken t = MeasureOn(state, r, ctx);
      if (t.waitJob || t.waitCam)
        return {t.waitJob, t.waitCam, nullptr};
      if (t.work) {
        StepOutcome o;
        o.work = std::make_unique<Pending>();
        o.work->id = r.id;
        o.work->revision = r.revision;
        // What the copies are of: an edit while the worker measures moves
        // the hash, and the evidence reads stale rather than describing a
        // model nobody measured.
        o.work->hash = CurrentHash(state, r, why);
        o.work->work = std::move(t.work);
        return o;
      }
      Judge(state, r, t, e);
    } catch (const std::exception& ex) {
      e.error = ex.what();
      e.pass = false;
    }
  }
  // The hash of what was measured, taken AFTER measuring: a run settles
  // nothing the hash reads, and a reference that vanished reads 0.
  e.inputHash = CurrentHash(state, r, why);
  r.evidence = std::move(e);
  MarkRequirementsEdited(state);
  return {};
}

// A worker's reading, landed as its requirement's evidence. Nothing when the
// requirement was removed or edited meanwhile: the reading is about one
// that no longer exists. Returns whether it landed.
bool Land(ui::AppState& state, Pending& p) {
  req::Requirement* r = FindRequirement(state, p.id);
  if (r == nullptr || r->revision != p.revision)
    return false;
  req::Evidence e;
  e.taken = true;
  e.revision = p.revision;
  e.checkedAt = Now();
  if (!p.error.empty()) {
    e.error = p.error;
  } else {
    try {
      Judge(state, *r, p.taken, e);
    } catch (const std::exception& ex) {
      e.error = ex.what();
      e.pass = false;
    }
  }
  e.inputHash = p.hash;
  r->evidence = std::move(e);
  MarkRequirementsEdited(state);
  return true;
}

// The state the evidence alone says: what a row read before the values
// moved.
std::string EvidenceState(const req::Requirement& r) {
  const req::Evidence& e = r.evidence;
  if (!e.taken || e.revision != r.revision)
    return "unchecked";
  if (!e.error.empty())
    return "unresolved";
  return e.pass ? "pass" : "fail";
}

// Every `expression` requirement whose input hash moved, measured in place
// (docs/requirements/README.md#expression): it costs microseconds, so it
// never waits for a Check. Answers the rows whose state that changed, in
// the shape a mutating call answers them: {id, name, state, value?, unit?,
// bound, limit?, note?, finding?}.
json RefreshExpressions(ui::AppState& state) {
  json changed = json::array();
  for (req::Requirement& r : state.requirements) {
    if (r.Kind() != "expression")
      continue;
    std::string why;
    const uint64_t hash = CurrentHash(state, r, why);
    const req::Evidence& e = r.evidence;
    if (e.taken && e.revision == r.revision && e.inputHash == hash &&
        (why.empty() || e.error == why))
      continue;
    const std::string before = EvidenceState(r);
    CheckOne(state, r, Ctx{});
    if (EvidenceState(r) == before)
      continue;
    const json row = Row(state, r);
    json out{{"id", r.id},
             {"name", r.name},
             {"state", row.at("state")},
             {"bound", row.at("bound")}};
    for (const char* k :
         {"value", "unit", "limit", "limit_min", "limit_max", "note"})
      if (row.contains(k))
        out[k] = row.at(k);
    std::string finding;
    for (const std::string& line : req::FindingLines(row))
      finding += (finding.empty() ? "" : "; ") + line;
    if (!finding.empty())
      out["finding"] = finding;
    changed.push_back(std::move(out));
  }
  return changed;
}

bool Current(ui::AppState& state, const req::Requirement& r) {
  std::string why;
  const uint64_t h = CurrentHash(state, r, why);
  const std::string st = req::StateOf(r, why, h);
  return st == "pass" || st == "fail";
}

receipt::Receipt ReceiptFor(const json& rows) {
  receipt::Receipt rec;
  for (const json& row : rows) {
    const std::string st = row.at("state").get<std::string>();
    const receipt::Oracle oracle{
        "designer/requirement/" +
            row.at("measure").value("kind", std::string()),
        "1"};
    const std::string name = row.at("name").get<std::string>();
    if (st == "pass" || st == "fail") {
      receipt::Claim c(name, oracle,
                       st == "pass" ? receipt::Verdict::Pass
                                    : receipt::Verdict::Fail);
      if (row.contains("value"))
        c.value = row.at("value").get<double>();
      c.unit = row.value("unit", std::string());
      rec.Add(std::move(c));
    } else {
      rec.Unverifiable(name, oracle, st + ": " + row.value("note", st));
    }
  }
  return rec;
}

// The log's line for every requirement a check was asked about that does
// not pass: its name, its state and its whole finding, the lines the panel
// shows under the row (req::FindingLines). A failing check read from the log
// says why, not just that.
void LogFindings(ui::AppState& state, const std::vector<uint64_t>& ids) {
  for (uint64_t id : ids) {
    const req::Requirement* r = FindRequirement(state, id);
    if (r == nullptr)
      continue;
    const json row = Row(state, *r);
    const std::vector<std::string> lines = req::FindingLines(row);
    if (lines.empty())
      continue;
    std::string text;
    for (const std::string& l : lines)
      text += (text.empty() ? "" : "; ") + l;
    LOG_WARN("requirement '{}' {}: {}", r->name,
             row.at("state").get<std::string>(), text);
  }
}

bool AnyStudyJobRunning(ui::AppState& state, uint64_t except) {
  for (const ui::BackgroundJobInfo& j : ui::ListBackgroundJobs(state))
    if (j.id != except &&
        (j.kind == "motion" || j.kind == "topo" || j.kind == "study" ||
         j.kind == "mesh" || j.kind == "check"))
      return true;
  return false;
}

// The check as a job (docs/requirements/README.md#checking-is-a-job). Its
// `finish` steps through the requirements on the main thread, one a frame;
// it waits on the study jobs and CAM passes the measures start, and on the
// worker a measure hands its expensive half to (Pending), so the main
// thread only reads the model and writes evidence.
struct CheckRun {
  std::vector<uint64_t> ids;
  std::vector<uint64_t> asked; // ids and the current ones answered as they lie
  size_t next = 0;
  bool recompute = false;
  bool recomputeThis = false; // this requirement's first run, not its retry
  uint64_t waitJob = 0;
  bool waitCam = false;
  std::unique_ptr<Pending> pending; // ids[next]'s worker
  // The studies this run has re-run (recompute): run once, they are as
  // current for the next requirement over them as a re-run would make them.
  std::set<std::pair<uint64_t, uint64_t>> rerun; // {document, study}
  int returnTo = -1;
  bool busy = false;
  size_t checked = 0, reused = 0;
};

void Restore(ui::AppState& state, int slot) {
  if (slot >= 0 && static_cast<size_t>(slot) < state.parts.size() &&
      slot != state.activePart) {
    const bool framed = state.parts[slot].everFramed;
    state.parts[slot].everFramed = true;
    ui::SetActivePart(state, static_cast<size_t>(slot));
    state.parts[slot].everFramed = framed;
  }
}

bool StepCheck(ui::AppState& state, CheckRun& run, ui::BackgroundJob& job) {
  if (run.busy)
    return false; // a handler this step called reaped the registry
  run.busy = true;
  struct Unbusy {
    bool& b;
    ~Unbusy() { b = false; }
  } unbusy{run.busy};
  const double count = static_cast<double>(run.ids.size());
  if (run.pending) {
    // A measure on its worker. Cancel is a request it reads between its
    // pieces; the job ends once it has stopped, so nothing outlives it.
    if (job.cancel.load() && !run.pending->cancel.exchange(true))
      job.label = "Cancelling the requirements check";
    if (!run.pending->done.load(std::memory_order_acquire)) {
      job.fraction.store((static_cast<double>(run.next) +
                          std::clamp(run.pending->fraction.load(), 0.0, 1.0)) /
                         count);
      return false;
    }
    run.pending->thread.join();
    if (!job.cancel.load()) {
      Land(state, *run.pending);
      run.checked++;
      run.next++;
      run.recomputeThis = run.recompute;
    }
    run.pending.reset();
    if (!job.cancel.load())
      return false; // one measured requirement per frame
  }
  if (job.cancel.load()) {
    if (run.waitJob)
      ui::CancelBackgroundJobById(state, run.waitJob,
                                  job.cancelReason.empty() ? "cancel_study"
                                                           : job.cancelReason);
    Restore(state, run.returnTo);
    LogFindings(state, run.asked);
    return true;
  }
  if (run.waitJob) {
    if (ui::FindBackgroundJobById(state, run.waitJob))
      return false;
    const ui::FinishedJob* f = ui::FinishedJobById(state, run.waitJob);
    run.waitJob = 0;
    run.recomputeThis = false;
    if (f && f->outcome != "done" && run.next < run.ids.size()) {
      // The run failed or was cancelled: that is the evidence, not a
      // reason to start it again.
      if (req::Requirement* r = FindRequirement(state, run.ids[run.next])) {
        req::Evidence e;
        e.taken = true;
        e.revision = r->revision;
        e.checkedAt = Now();
        e.error = "its study " + f->outcome +
                  (f->error.empty() ? std::string() : ": " + f->error);
        std::string why;
        e.inputHash = CurrentHash(state, *r, why);
        r->evidence = std::move(e);
        MarkRequirementsEdited(state);
        run.checked++;
      }
      run.next++;
      run.recomputeThis = run.recompute;
      return false;
    }
  }
  if (run.waitCam) {
    if (ui::CamPassesBusy(state))
      return false;
    run.waitCam = false;
  }
  if (AnyStudyJobRunning(state, job.id))
    return false; // one run at a time: wait for whoever is running
  while (run.next < run.ids.size()) {
    req::Requirement* r = FindRequirement(state, run.ids[run.next]);
    if (r == nullptr || (!run.recompute && Current(state, *r))) {
      run.reused += r ? 1 : 0;
      run.next++;
      run.recomputeThis = run.recompute;
      continue;
    }
    job.label = "Checking '" + r->name + "' (" + std::to_string(run.next + 1) +
                " of " + std::to_string(run.ids.size()) + ")";
    job.fraction.store(static_cast<double>(run.next) / count);
    const json& m = r->measure;
    const std::pair<uint64_t, uint64_t> study{
        r->Document(), m.contains("study") ? m.at("study").get<uint64_t>()
                       : m.contains("motion_study")
                           ? m.at("motion_study").get<uint64_t>()
                           : 0};
    const bool recompute =
        run.recomputeThis && (study.second == 0 || !run.rerun.contains(study));
    if (run.recomputeThis && study.second != 0)
      run.rerun.insert(study);
    StepOutcome o = CheckOne(state, *r, Ctx{true, recompute});
    if (o.waitJob || o.waitCam) {
      run.waitJob = o.waitJob;
      run.waitCam = o.waitCam;
      return false;
    }
    if (o.work) {
      run.pending = std::move(o.work);
      run.pending->Start();
      return false;
    }
    run.checked++;
    run.next++;
    run.recomputeThis = run.recompute;
    return false; // one measured requirement per frame
  }
  Restore(state, run.returnTo);
  LogFindings(state, run.asked);
  job.fraction.store(1.0);
  return true;
}

std::shared_ptr<CheckRun> StartCheck(ui::AppState& state,
                                     std::vector<uint64_t> ids,
                                     std::vector<uint64_t> asked,
                                     bool recompute, uint64_t owner,
                                     uint64_t* jobId) {
  auto run = std::make_shared<CheckRun>();
  run->ids = std::move(ids);
  run->asked = std::move(asked);
  run->recompute = recompute;
  run->recomputeThis = recompute;
  run->returnTo = state.activePart;
  auto job = std::make_unique<ui::BackgroundJob>();
  job->kind = kJobKind;
  job->label = "Checking requirements";
  job->cancellable = true;
  job->owner = owner;
  job->started = std::chrono::steady_clock::now();
  job->fraction.store(0.0);
  ui::BackgroundJob* raw = job.get();
  job->finish = [run, raw](ui::AppState& s) {
    return StepCheck(s, *run, *raw);
  };
  // No thread of the registry's: `finish` is the job, and the workers it
  // starts are the run's (CheckRun::pending), joined before it returns true
  // or when the run is dropped.
  job->done.store(true);
  *jobId = ui::StartBackgroundJob(state, std::move(job)).id;
  return run;
}

// A synchronous check_requirements, parked on the check's job
// (ParkRequirementsRequest): answered, as the synchronous form answers,
// once the job is reaped.
class CheckWait : public Parking {
public:
  CheckWait(uint64_t job, std::shared_ptr<CheckRun> run,
            std::vector<uint64_t> ids)
      : m_job(job), m_run(std::move(run)), m_ids(std::move(ids)) {}
  Next Poll(ui::AppState& state, const RpcParams& p, json& answer) override {
    if (ui::FindBackgroundJobById(state, m_job))
      return Next::Wait;
    // Read as the handler reads them, so the server finds no unknown key.
    if (p.contains("ids"))
      (void)p.at("ids");
    (void)p.value("recompute", false);
    (void)p.value("async", false);
    (void)p.value("outlive_client", false);
    const ui::FinishedJob* f = ui::FinishedJobById(state, m_job);
    if (f && f->outcome != "done")
      throw std::runtime_error(
          "the requirements check (job " + std::to_string(m_job) + ") " +
          f->outcome +
          (f->cancelReason.empty() ? std::string()
                                   : " (" + f->cancelReason + ")") +
          (f->error.empty() ? std::string() : ": " + f->error) +
          "; what it measured before is kept (list_requirements)");
    json out = RowsJson(state, &m_ids);
    out["checked"] = m_run->checked;
    out["reused"] = m_ids.size() - std::min(m_ids.size(), m_run->checked);
    AttachReceipt(out, ReceiptFor(out.at("requirements")));
    answer = std::move(out);
    return Next::Answer;
  }

private:
  uint64_t m_job;
  std::shared_ptr<CheckRun> m_run;
  std::vector<uint64_t> m_ids;
};

// ---- Clearance assertions, as requirements
// ------------------------------------

AssemblyDocument& RequireAssembly(ui::AppState& state) {
  if (!state.assembly)
    throw std::runtime_error(
        "declared clearances live on an ASSEMBLY — switch tabs with "
        "set_active_part (see list_parts)");
  return *state.assembly;
}

uint64_t ActiveSlotId(const ui::AppState& state) {
  return state.activePart >= 0 &&
                 static_cast<size_t>(state.activePart) < state.parts.size()
             ? state.parts[state.activePart].id
             : 0;
}

bool IsAssertionOf(const req::Requirement& r, uint64_t asmId) {
  return r.Kind() == "clearance" && r.Document() == asmId &&
         !r.measure.contains("motion_study") && r.bound.kind == "at_least" &&
         r.bound.valueExpr.empty();
}

json AssertionRow(ui::AppState& state, const req::Requirement& r) {
  std::string why;
  const uint64_t hash = CurrentHash(state, r, why);
  const std::string st = req::StateOf(r, why, hash);
  const std::string standing = st == "pass"         ? "holds"
                               : st == "fail"       ? "violated"
                               : st == "unresolved" ? "unverifiable"
                               : st == "stale"      ? "stale"
                                                    : "never";
  json j{{"id", r.id},
         {"name", r.name},
         {"a", r.measure.at("a")},
         {"b", r.measure.at("b")},
         {"min_mm", r.bound.value},
         {"mode", r.measure.value("mode", std::string("mesh"))},
         {"standing", standing}};
  if (!r.source.empty())
    j["why"] = r.source;
  if (r.evidence.taken && r.evidence.revision == r.revision) {
    if (r.evidence.value)
      j["measured_mm"] = *r.evidence.value;
    j["last_verdict"] = !r.evidence.error.empty() ? "unverifiable"
                        : r.evidence.pass         ? "holds"
                                                  : "violated";
  }
  const std::string note = NoteFor(r, st, why);
  if (!note.empty() && standing != "holds" && standing != "violated")
    j["note"] = note;
  return j;
}

// A `document` written as a tab's NAME is refused like any wrong type (an id
// is what a requirement keeps: a name repoints silently on a rename), but
// when the name is an open tab the refusal says which id that is, so the fix
// is one copy rather than a trip to list_parts.
void RefuseDocumentByName(const ui::AppState& state, const json& measure) {
  if (!measure.is_object() || !measure.contains("document") ||
      !measure.at("document").is_string())
    return;
  const std::string name = measure.at("document").get<std::string>();
  for (const auto& slot : state.parts)
    if (slot.name == name)
      throw std::runtime_error(
          "`measure.document` is the id of the document it measures, not "
          "its name: '" +
          name + "' is document " + std::to_string(slot.id) +
          " (a name repoints silently on a rename)");
}

json AddRequirement(ui::AppState& state, req::Requirement r) {
  RefuseDocumentByName(state, r.measure);
  req::Validate(r.measure, r.bound);
  RefuseUnresolvable(state, r);
  r.id = state.nextRequirementId++;
  r.revision = 1;
  r.evidence = {};
  state.requirements.push_back(std::move(r));
  MarkRequirementsEdited(state);
  RefreshExpressions(state); // an expression is measured as it is stated
  return Row(state, state.requirements.back());
}

// ---- The design sweep (docs/requirements/README.md#design-sweep) ----------

constexpr const char* kSweepKind = "design_sweep";

// What design_sweep runs, checked before anything starts: the variables and
// their ranges, the parts it rebuilds (slot indices), the requirements it
// measures at every point, and those it does not, with why.
struct SweepPlan {
  sweep::Spec spec;
  std::vector<size_t> slots;
  std::vector<req::Requirement> measured;
  json notSwept = json::array();
  size_t planned = 0, cap = sweep::kDefaultMaxPoints;
  json variables = json::array(); // as resolved, for the answer
};

// The sweeps kept in memory: their answers by job, newest last.
struct SweepStore {
  std::deque<std::pair<uint64_t, json>> answers;
  uint64_t last = 0;
};
SweepStore& Sweeps() {
  static SweepStore store;
  return store;
}
void KeepSweep(uint64_t job, json answer) {
  SweepStore& s = Sweeps();
  s.answers.emplace_back(job, std::move(answer));
  while (s.answers.size() > 8)
    s.answers.pop_front();
  s.last = job;
}

std::atomic<int> g_testSweepHoldMs{-1};
int TestSweepHoldMs() {
  int ms = g_testSweepHoldMs.load();
  if (ms < 0) {
    const char* env = std::getenv("LOBOLE_TEST_SWEEP_MS");
    ms = env ? std::max(0, std::atoi(env)) : 0;
    g_testSweepHoldMs.store(ms);
  }
  return ms;
}

// The project variables a variable reaches: itself and every one whose
// expression reads it, directly or through others.
std::set<std::string> Reached(const std::vector<Parameter>& params,
                              const std::string& name) {
  std::set<std::string> out{name};
  for (bool grew = true; grew;) {
    grew = false;
    for (const Parameter& p : params) {
      if (out.contains(p.name))
        continue;
      for (const std::string& d : req::IdentifiersOf(p.expression))
        if (out.contains(d)) {
          out.insert(p.name);
          grew = true;
          break;
        }
    }
  }
  return out;
}

// Whether part `doc` reads any of `names` -- its variables or features --
// where they mean the project's (no local one of the name shadows it), or,
// `local`, its own.
bool PartReads(Document& doc, const std::set<std::string>& names, bool local) {
  for (const std::string& n : names) {
    if (!local && doc.FindParameter(n) != nullptr)
      continue;
    if (!doc.FindParameterReferences(n).empty())
      return true;
  }
  return false;
}

double SweepEnd(const json& v, const SymbolTable& scope,
                const std::string& what) {
  double out = 0.0;
  if (v.is_number()) {
    out = v.get<double>();
  } else if (v.is_string()) {
    const auto r =
        ExpressionEngine::Default().EvaluateText(v.get<std::string>(), scope);
    if (!r)
      throw std::runtime_error(what + " '" + v.get<std::string>() +
                               "': " + r.error().message);
    out = *r;
  } else {
    throw std::runtime_error(what +
                             " is a number or an expression in the "
                             "variable's base units (mm, degrees), not " +
                             v.dump());
  }
  if (!std::isfinite(out))
    throw std::runtime_error(what + " is not a finite number");
  return out;
}

// design_sweep's request, read and refused before anything starts
// (docs/scripting/rpc.md#a-refused-call-changes-nothing).
SweepPlan PlanSweep(ui::AppState& state, const RpcParams& p) {
  SweepPlan plan;
  if (ui::BackgroundJob* running = ui::FindBackgroundJob(state, kSweepKind))
    throw std::runtime_error(
        "a design sweep is already running (job " +
        std::to_string(running->id) +
        "): study_status {job} follows it, cancel_study {job} stops it");
  const auto whole = [&](const char* key, int fallback, int lo, int hi) {
    if (!p.contains(key))
      return fallback;
    const json& v = p.at(key);
    if (!v.is_number_integer() || v.get<int64_t>() < lo ||
        v.get<int64_t>() > hi)
      throw std::runtime_error(std::string("`") + key +
                               "` is a whole number from " +
                               std::to_string(lo) + " to " +
                               std::to_string(hi) + ", not " + v.dump());
    return static_cast<int>(v.get<int64_t>());
  };
  const auto flag = [&](const char* key) {
    if (!p.contains(key))
      return false;
    if (!p.at(key).is_boolean())
      throw std::runtime_error(std::string("`") + key +
                               "` is true or false, not " + p.at(key).dump());
    return p.at(key).get<bool>();
  };
  plan.spec.samples =
      whole("samples", sweep::kDefaultSamples, 2, sweep::kMaxSamples);
  plan.spec.refine = whole("refine", 0, 0, sweep::kMaxRefine);
  plan.spec.corners = flag("corners");
  plan.cap = static_cast<size_t>(
      whole("max_points", static_cast<int>(sweep::kDefaultMaxPoints), 1,
            static_cast<int>(sweep::kMaxPointsCeiling)));
  const bool fromRequirements = flag("from_requirements");
  (void)flag("async");
  (void)flag("outlive_client");

  Document* active = state.document.get();
  const auto activeId = [&]() -> uint64_t {
    return state.activePart >= 0 &&
                   static_cast<size_t>(state.activePart) < state.parts.size()
               ? state.parts[state.activePart].id
               : 0;
  };
  // The variables there are, for a refusal: the project's and each part's.
  const auto inScope = [&] {
    std::vector<std::string> v;
    for (const Parameter& q : state.projectParameters)
      v.push_back(q.name);
    for (size_t i = 0; i < state.parts.size(); i++)
      if (const Document* d = ui::SlotDoc(state, i))
        for (const Parameter& q : d->GetParameters())
          v.push_back(q.name + " (part " + std::to_string(state.parts[i].id) +
                      ", '" + state.parts[i].name + "')");
    return KnownList(v, 40);
  };
  // A variable named in `scope` (absent: the project's, else the active
  // part's local one): {part id or 0, the scope its ends read, its value}.
  struct Found {
    uint64_t part = 0;
    const SymbolTable* scope = nullptr;
    double value = 0.0;
  };
  const auto find = [&](const std::string& name, const json* scope) -> Found {
    const Parameter* proj = ui::FindProjectParameter(state, name);
    const auto local = [&](uint64_t partId) -> Found {
      const auto slot = ui::SlotIndexById(state, partId);
      Document* d = slot && state.parts[*slot].kind == ui::DocKind::Part
                        ? ui::SlotDoc(state, *slot)
                        : nullptr;
      if (d == nullptr)
        throw std::runtime_error(
            "variable '" + name + "': part " + std::to_string(partId) +
            " is not a part (documents: " + DocumentCandidates(state) + ")");
      const Parameter* q = d->FindParameter(name);
      if (q == nullptr)
        throw std::runtime_error("part '" + state.parts[*slot].name +
                                 "' has no variable '" + name +
                                 "'; variables in scope: " + inScope());
      return {partId, &d->Symbols(), q->value};
    };
    if (scope == nullptr) {
      if (proj)
        return {0, &state.projectSymbols, proj->value};
      if (active && active->FindParameter(name))
        return local(activeId());
      throw std::runtime_error("no variable '" + name +
                               "'; variables in scope: " + inScope());
    }
    if (scope->is_string() && scope->get<std::string>() == "project") {
      if (!proj)
        throw std::runtime_error("no project variable '" + name +
                                 "'; variables in scope: " + inScope());
      return {0, &state.projectSymbols, proj->value};
    }
    if (scope->is_string() && scope->get<std::string>() == "part") {
      if (!active)
        throw std::runtime_error("variable '" + name +
                                 "': scope \"part\" is the active part's, and "
                                 "the active tab is not a part; give the "
                                 "part's id as `scope`");
      return local(activeId());
    }
    if (scope->is_number_integer() && scope->get<int64_t>() >= 0)
      return local(scope->get<uint64_t>());
    throw std::runtime_error("variable '" + name +
                             "': `scope` is \"project\", \"part\" (the active "
                             "part's) or a part's id, not " +
                             scope->dump());
  };

  struct Entry {
    std::string name;
    uint64_t part = 0;
    double value = 0.0;
    std::optional<double> min, max;
    std::string minFrom, maxFrom; // the requirement that gave it
  };
  std::vector<Entry> entries;
  const auto entryFor = [&](const std::string& name, uint64_t part) -> Entry* {
    for (Entry& e : entries)
      if (e.name == name && e.part == part)
        return &e;
    return nullptr;
  };
  if (p.contains("variables")) {
    const json& vs = p.at("variables");
    if (!vs.is_array())
      throw std::runtime_error("`variables` is a list of {name, min, max, "
                               "scope?}, not " +
                               vs.dump());
    for (const json& v : vs) {
      if (!v.is_object() || !v.contains("name") || !v.at("name").is_string())
        throw std::runtime_error("a swept variable is {name, min, max, "
                                 "scope?}, not " +
                                 v.dump());
      for (const auto& [k, x] : v.items())
        if (k != "name" && k != "min" && k != "max" && k != "scope")
          throw std::runtime_error("variables: unknown key '" + k +
                                   "' (accepted: name, min, max, scope)");
      const std::string name = v.at("name").get<std::string>();
      const Found f =
          find(name, v.contains("scope") ? &v.at("scope") : nullptr);
      if (entryFor(name, f.part) != nullptr)
        throw std::runtime_error("variable '" + name + "' is listed twice");
      Entry e{name, f.part, f.value, std::nullopt, std::nullopt, {}, {}};
      if (v.contains("min"))
        e.min = SweepEnd(v.at("min"), *f.scope, "'" + name + "' min");
      if (v.contains("max"))
        e.max = SweepEnd(v.at("max"), *f.scope, "'" + name + "' max");
      entries.push_back(std::move(e));
    }
  }
  if (fromRequirements) {
    std::vector<std::string> looked;
    size_t taken = 0;
    for (const req::Requirement& r : state.requirements) {
      if (r.Kind() != "expression")
        continue;
      std::string text = r.measure.at("expression").get<std::string>();
      looked.push_back("'" + r.name + "' (" + text + ")");
      const auto first = text.find_first_not_of(" \t");
      const auto last = text.find_last_not_of(" \t");
      text = first == std::string::npos ? ""
                                        : text.substr(first, last - first + 1);
      if (!ExpressionEngine::Default().IsValidParameterName(text) ||
          r.bound.kind == "pass")
        continue;
      ExprScope scope;
      ScopeOf(state, r, scope);
      if (!scope.why.empty())
        continue;
      // The variable as the requirement reads it: its part's local one when
      // the part has one, else the project's.
      uint64_t part = 0;
      if (r.measure.contains("document")) {
        const auto slot = ui::SlotIndexById(state, r.Document());
        const Document* d = slot ? ui::SlotDoc(state, *slot) : nullptr;
        if (d && d->FindParameter(text))
          part = r.Document();
      }
      if (part == 0 && !ui::FindProjectParameter(state, text))
        continue;
      Entry* e = entryFor(text, part);
      if (e == nullptr) {
        const json scopeJ = part == 0 ? json("project") : json(part);
        const Found f = find(text, &scopeJ);
        entries.push_back(
            {text, part, f.value, std::nullopt, std::nullopt, {}, {}});
        e = &entries.back();
      }
      const auto end = [&](const std::string& expr, double literal) {
        if (expr.empty())
          return literal;
        const auto v = req::EvaluateIn(expr, scope.Table());
        if (!v.value)
          throw std::runtime_error("requirement '" + r.name + "': bound '" +
                                   expr + "': " + v.error);
        return *v.value;
      };
      // A range the call states itself wins: it is the one asked for.
      if (r.bound.kind == "within" || r.bound.kind == "at_least")
        if (!e->min) {
          e->min = end(r.bound.kind == "within" ? r.bound.minExpr
                                                : r.bound.valueExpr,
                       r.bound.kind == "within" ? r.bound.min : r.bound.value);
          e->minFrom = r.name;
        }
      if (r.bound.kind == "within" || r.bound.kind == "at_most")
        if (!e->max) {
          e->max = end(r.bound.kind == "within" ? r.bound.maxExpr
                                                : r.bound.valueExpr,
                       r.bound.kind == "within" ? r.bound.max : r.bound.value);
          e->maxFrom = r.name;
        }
      taken++;
    }
    if (taken == 0)
      throw std::runtime_error(
          "from_requirements: no expression requirement bounds one bare "
          "variable (an expression that is just its name, bounded within "
          "{min, max}); looked at: " +
          KnownList(looked));
  }
  if (entries.empty())
    throw std::runtime_error("design_sweep needs `variables` [{name, min, "
                             "max}] or from_requirements: true");
  for (const Entry& e : entries) {
    if (!e.min || !e.max) {
      const std::string from = !e.minFrom.empty()   ? e.minFrom
                               : !e.maxFrom.empty() ? e.maxFrom
                                                    : "";
      const char* missing = !e.min ? "min" : "max";
      if (!from.empty())
        throw std::runtime_error("requirement '" + from + "' bounds '" +
                                 e.name + "' on one side only; give its " +
                                 missing + " in variables [{" + "name: \"" +
                                 e.name + "\", " + missing + ": ...}]");
      throw std::runtime_error("variable '" + e.name +
                               "' needs both `min` "
                               "and `max`");
    }
    if (*e.min > *e.max)
      throw std::runtime_error(
          "variable '" + e.name + "': min " + std::format("{:.6g}", *e.min) +
          " is above max " + std::format("{:.6g}", *e.max));
    plan.spec.variables.push_back({e.name, *e.min, *e.max, e.part, e.value});
  }
  if (plan.spec.corners && plan.spec.variables.size() >
                               static_cast<size_t>(sweep::kMaxCornerVariables))
    throw std::runtime_error("corners: true takes up to " +
                             std::to_string(sweep::kMaxCornerVariables) +
                             " variables (2^n corners), not " +
                             std::to_string(plan.spec.variables.size()));

  // What each variable reaches: the project variables computed from it, or
  // the part-local one alone.
  std::vector<std::set<std::string>> reach;
  for (const sweep::Variable& v : plan.spec.variables)
    reach.push_back(v.part == 0 ? Reached(state.projectParameters, v.name)
                                : std::set<std::string>{v.name});
  // Whether requirement `r` reads variable k where it means it.
  const auto reqReads = [&](const req::Requirement& r, size_t k) {
    const sweep::Variable& v = plan.spec.variables[k];
    const bool partScope =
        r.Kind() == "expression" && r.measure.contains("document");
    if (v.part != 0 && (!partScope || r.Document() != v.part))
      return false;
    if (v.part == 0 && partScope) {
      const auto slot = ui::SlotIndexById(state, r.Document());
      const Document* d = slot ? ui::SlotDoc(state, *slot) : nullptr;
      if (d && d->FindParameter(v.name))
        return false; // its part shadows the name
    }
    for (const std::string& n : reach[k])
      if (req::ReadsName(r, n))
        return true;
    return false;
  };

  // The parts it rebuilds.
  std::set<size_t> slots;
  if (p.contains("documents")) {
    const json& ds = p.at("documents");
    if (!ds.is_array() || ds.empty())
      throw std::runtime_error("`documents` is a non-empty list of part ids "
                               "(list_parts)");
    for (const json& id : ds) {
      std::string why;
      if (!id.is_number_integer() || id.get<int64_t>() < 0)
        throw std::runtime_error("`documents` lists part ids (list_parts), "
                                 "not " +
                                 id.dump());
      const auto slot = DocSlot(state, id.get<uint64_t>(), "part", &why);
      if (!slot)
        throw std::runtime_error("documents: " + why +
                                 ". Documents: " + DocumentCandidates(state));
      slots.insert(*slot);
    }
  } else {
    for (size_t i = 0; i < state.parts.size(); i++) {
      if (state.parts[i].kind != ui::DocKind::Part)
        continue;
      Document* d = ui::SlotDoc(state, i);
      if (d == nullptr)
        continue;
      for (size_t k = 0; k < plan.spec.variables.size(); k++) {
        const sweep::Variable& v = plan.spec.variables[k];
        const bool reads = v.part != 0 ? v.part == state.parts[i].id
                                       : PartReads(*d, reach[k], false);
        bool reqOn = false;
        for (const req::Requirement& r : state.requirements)
          reqOn = reqOn ||
                  (r.Kind() == "expression" && r.measure.contains("document") &&
                   r.Document() == state.parts[i].id && reqReads(r, k));
        if (reads || reqOn)
          slots.insert(i);
      }
    }
  }
  plan.slots.assign(slots.begin(), slots.end());

  // Each variable must reach something, and no rebuilt part may hold it at
  // a configuration's value: the sweep would change nothing there, as a
  // held DOF refuses a drag.
  for (size_t k = 0; k < plan.spec.variables.size(); k++) {
    const sweep::Variable& v = plan.spec.variables[k];
    bool reached = false;
    for (size_t i : plan.slots) {
      Document* d = ui::SlotDoc(state, i);
      if (v.part != 0 ? v.part == state.parts[i].id
                      : PartReads(*d, reach[k], false))
        reached = true;
      if (v.part != 0 && v.part != state.parts[i].id)
        continue;
      if (const Configuration* c = d->ActiveConfig())
        if (c->OverrideFor(v.name) != nullptr)
          throw std::runtime_error(
              "'" + state.parts[i].name + "' is in configuration '" + c->name +
              "', which overrides '" + v.name +
              "': the sweep would change nothing there. Activate its base "
              "(set_active_configuration {id: 0}) or leave it out of "
              "`documents`");
    }
    for (const req::Requirement& r : state.requirements)
      if (r.Kind() == "expression" && reqReads(r, k))
        reached = true;
    if (!reached)
      throw std::runtime_error(
          "no part reads '" + v.name +
          "' (directly or through other variables)" +
          (p.contains("documents") ? std::string(" among `documents`")
                                   : std::string()) +
          ", and no expression requirement does: sweeping it changes "
          "nothing");
  }

  // What it measures at every point, and what it does not.
  std::set<uint64_t> rebuilt;
  for (size_t i : plan.slots)
    rebuilt.insert(state.parts[i].id);
  for (const req::Requirement& r : state.requirements) {
    const std::string kind = r.Kind();
    std::string why;
    if (kind == "expression") {
      if (r.measure.contains("document") && !rebuilt.contains(r.Document()))
        why = "its part is not rebuilt, so none of the swept variables "
              "reaches it";
    } else if (kind == "mass" || kind == "volume" || kind == "section_area") {
      const auto slot = ui::SlotIndexById(state, r.Document());
      if (slot && state.parts[*slot].kind == ui::DocKind::Assembly)
        why = "an assembly reads every part it places; the sweep rebuilds "
              "parts";
      else if (!rebuilt.contains(r.Document()))
        why = "its part is not rebuilt, so none of the swept variables "
              "reaches it";
    } else if (kind == "study_probe" || kind == "study_result" ||
               kind == "motion_clean") {
      why = "a study per point is minutes";
    } else if (kind == "cam_verify") {
      why = "a CAM job's verification per point is minutes";
    } else if (kind == "check_literals") {
      why = "it reads how the features are written, which a sweep does not "
            "change";
    } else {
      why = "an assembly reads every part it places; the sweep rebuilds "
            "parts";
    }
    if (why.empty())
      plan.measured.push_back(r);
    else
      plan.notSwept.push_back(
          {{"id", r.id}, {"name", r.name}, {"kind", kind}, {"why", why}});
  }

  plan.planned = sweep::PlannedCount(plan.spec);
  if (plan.planned > plan.cap)
    throw std::runtime_error(
        "the sweep would run " + std::to_string(plan.planned) +
        " points, over the cap of " + std::to_string(plan.cap) +
        " (max_points, at most " + std::to_string(sweep::kMaxPointsCeiling) +
        "); fewer samples or variables, no corners, less refine, or a "
        "higher max_points");
  for (const sweep::Variable& v : plan.spec.variables) {
    json j{{"name", v.name},
           {"min", v.min},
           {"max", v.max},
           {"value", v.value},
           {"scope", v.part == 0 ? json("project") : json(v.part)}};
    plan.variables.push_back(std::move(j));
  }
  return plan;
}

json PlanJson(const SweepPlan& plan) {
  return json{{"planned", plan.planned},
              {"cap", plan.cap},
              {"variables", plan.variables},
              {"not_swept", plan.notSwept}};
}

// The copies a sweep reads: each rebuilt part in its saved form, with the
// project variables and tables as they are now. Written here, on the main
// thread; read back on the workers.
sweep::Inputs SweepInputs(ui::AppState& state, const SweepPlan& plan,
                          const std::filesystem::path& dir) {
  sweep::Inputs in;
  in.spec = plan.spec;
  in.projectParameters = state.projectParameters;
  in.tables = state.projectTables;
  in.requirements = plan.measured;
  in.holdMs = TestSweepHoldMs();
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  for (size_t i : plan.slots) {
    const Document* d = ui::SlotDoc(state, i);
    sweep::PartCopy copy{
        state.parts[i].id, state.parts[i].name,
        dir / ("part-" + std::to_string(state.parts[i].id) + ".json")};
    if (auto saved = DocumentIO::Save(*d, copy.file.string()); !saved)
      throw std::runtime_error("the copy of '" + copy.name +
                               "' could not be written: " + saved.error());
    in.parts.push_back(std::move(copy));
  }
  return in;
}

std::filesystem::path SweepDir() {
  static std::atomic<uint64_t> counter{0};
  return std::filesystem::temp_directory_path() /
         std::format("lobole-sweep-{}-{}", static_cast<long>(::getpid()),
                     counter.fetch_add(1));
}

json AnswerOf(const SweepPlan& plan, json run, double seconds, uint64_t job) {
  run["planned"] = plan.planned;
  run["cap"] = plan.cap;
  run["variables"] = plan.variables;
  run["not_swept"] = plan.notSwept;
  run["seconds"] = seconds;
  run["job"] = job;
  return run;
}

// A sweep on its job: the walk on a thread of its own, polled from the
// job's `finish` on the main thread, which carries its progress to the
// registry and keeps its answer when it ends.
struct SweepRun {
  SweepPlan plan;
  sweep::Inputs inputs;
  sweep::Progress progress;
  std::filesystem::path dir;
  std::thread thread;
  std::atomic<bool> finished{false};
  json answer;
  std::string error;
  std::chrono::steady_clock::time_point started;
  SweepRun() = default;
  SweepRun(const SweepRun&) = delete;
  SweepRun& operator=(const SweepRun&) = delete;
  ~SweepRun() {
    progress.cancel.store(true);
    if (thread.joinable())
      thread.join();
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  }
};

uint64_t StartSweep(ui::AppState& state, SweepPlan plan, uint64_t owner) {
  auto run = std::make_shared<SweepRun>();
  run->dir = SweepDir();
  run->inputs = SweepInputs(state, plan, run->dir);
  run->plan = std::move(plan);
  run->started = std::chrono::steady_clock::now();
  auto job = std::make_unique<ui::BackgroundJob>();
  job->kind = kSweepKind;
  job->label = "Design sweep: reading the copies";
  job->cancellable = true;
  job->owner = owner;
  job->fraction.store(0.0);
  ui::BackgroundJob* raw = job.get();
  job->finish = [run, raw](ui::AppState&) {
    if (raw->cancel.load())
      run->progress.cancel.store(true);
    {
      std::lock_guard lock(run->progress.mutex);
      if (!run->progress.label.empty())
        raw->label = "Design sweep: " + run->progress.label;
    }
    raw->fraction.store(std::min(
        1.0, static_cast<double>(run->progress.done.load()) /
                 static_cast<double>(std::max<size_t>(1, run->plan.planned))));
    if (!run->finished.load(std::memory_order_acquire))
      return false;
    run->thread.join();
    const double seconds = std::chrono::duration<double>(
                               std::chrono::steady_clock::now() - run->started)
                               .count();
    if (!run->error.empty()) {
      raw->error = run->error;
      KeepSweep(raw->id, json{{"job", raw->id}, {"error", run->error}});
    } else {
      KeepSweep(raw->id,
                AnswerOf(run->plan, std::move(run->answer), seconds, raw->id));
    }
    return true;
  };
  // No thread of the registry's: `finish` polls the run's own each frame,
  // and the run joins it when it is dropped.
  job->done.store(true);
  const uint64_t id = ui::StartBackgroundJob(state, std::move(job)).id;
  run->thread = std::thread([r = run.get()] {
    try {
      r->answer = sweep::Run(r->inputs, r->progress);
    } catch (const std::exception& e) {
      r->error = e.what();
    }
    r->finished.store(true, std::memory_order_release);
  });
  return id;
}

const json* KeptSweep(uint64_t job) {
  for (const auto& [id, answer] : Sweeps().answers)
    if (id == job)
      return &answer;
  return nullptr;
}

// A finished sweep's answer as design_sweep answers it: its refusal thrown
// when it failed.
json SweepAnswer(uint64_t job) {
  const json* a = KeptSweep(job);
  if (a == nullptr)
    throw std::runtime_error("design sweep " + std::to_string(job) +
                             " has no answer kept");
  if (a->contains("error"))
    throw std::runtime_error("the design sweep (job " + std::to_string(job) +
                             ") failed: " + a->at("error").get<std::string>());
  return *a;
}

// A synchronous design_sweep, parked on its job: answered with the sweep
// once the job is reaped.
class SweepWait : public Parking {
public:
  explicit SweepWait(uint64_t job) : m_job(job) {}
  Next Poll(ui::AppState& state, const RpcParams& p, json& answer) override {
    if (ui::FindBackgroundJobById(state, m_job))
      return Next::Wait;
    for (const char* k :
         {"variables", "from_requirements", "samples", "corners", "refine",
          "documents", "max_points", "async", "outlive_client"})
      if (p.contains(k))
        (void)p.at(k);
    answer = SweepAnswer(m_job);
    return Next::Answer;
  }

private:
  uint64_t m_job;
};

json EnginePlan(ui::AppState& state, const json& params) {
  return PlanJson(PlanSweep(state, RpcParams{params}));
}

uint64_t EngineStart(ui::AppState& state, const json& params) {
  return StartSweep(state, PlanSweep(state, RpcParams{params}), 0);
}

json EngineResult(ui::AppState&, uint64_t job) {
  const json* a = KeptSweep(job);
  return a ? *a : json();
}

void EngineRefresh(ui::AppState& state) {
  for (const req::Requirement& r : state.requirements)
    if (r.Kind() == "expression") {
      RefreshExpressions(state);
      return;
    }
}

// The dirty-free sum the panel caches its rows on: every document's save
// stamp and the requirements' own edits.
uint64_t ContentStamp(ui::AppState& state) {
  uint64_t h = state.requirementsEdits * 1000003ull;
  for (size_t i = 0; i < state.parts.size(); i++) {
    uint64_t s = 0;
    if (const Document* d = ui::SlotDoc(state, i))
      s = d->SaveStamp() * 31 + d->InstanceId();
    else if (const AssemblyDocument* a = ui::SlotAssembly(state, i))
      s = a->SaveStamp() * 37 + 1;
    else if (const cam::JobDocument* j = ui::SlotJob(state, i))
      s = j->SaveStamp() * 41 + 2;
    h = h * 1099511628211ull ^ (s + state.parts[i].id);
  }
  // The project's values: a variable no part reads still moves a bound.
  inputhash::Hasher values;
  std::map<std::string, double> sorted(state.projectSymbols.begin(),
                                       state.projectSymbols.end());
  for (const auto& [name, v] : sorted) {
    values.Str(name);
    values.F64(v);
  }
  h ^= values.Digest();
  return h ^ static_cast<uint64_t>(state.activePart + 7);
}

json PanelRows(ui::AppState& state) {
  static uint64_t stamp = ~0ull;
  static json cached;
  const uint64_t now = ContentStamp(state);
  if (now != stamp) {
    stamp = now;
    cached = RowsJson(state, nullptr);
  }
  return cached;
}

uint64_t PanelCheck(ui::AppState& state, bool recompute) {
  return StartRequirementsCheck(state, recompute);
}

[[maybe_unused]] const bool kEngineInstalled = [] {
  ui::SetRequirementsEngine({&PanelRows, &PanelCheck, &EngineRefresh,
                             &EnginePlan, &EngineStart, &EngineResult});
  return true;
}();

} // namespace

std::span<const MethodDoc> RequirementMethodDocs() {
  return RequirementMethods;
}

bool IsRequirementsMethod(const std::string& method) {
  for (const MethodDoc& d : RequirementMethods)
    if (method == d.name)
      return true;
  return false;
}

json RequirementRows(ui::AppState& state) { return RowsJson(state, nullptr); }

void SetTestRequirementHold(int ms) { g_testHoldMs.store(std::max(0, ms)); }

void SetTestSweepHold(int ms) { g_testSweepHoldMs.store(std::max(0, ms)); }

json RefreshExpressionRequirements(ui::AppState& state) {
  for (const req::Requirement& r : state.requirements)
    if (r.Kind() == "expression")
      return RefreshExpressions(state);
  return json::array();
}

uint64_t StartRequirementsCheck(ui::AppState& state, bool recompute) {
  if (ui::FindBackgroundJob(state, kJobKind))
    return ui::FindBackgroundJob(state, kJobKind)->id;
  std::vector<uint64_t> ids, asked;
  for (const req::Requirement& r : state.requirements) {
    asked.push_back(r.id);
    if (recompute || !Current(state, r))
      ids.push_back(r.id);
  }
  if (ids.empty())
    return 0;
  uint64_t job = 0;
  StartCheck(state, std::move(ids), std::move(asked), recompute, 0, &job);
  return job;
}

std::unique_ptr<Parking> ParkRequirementsRequest(ui::AppState& state,
                                                 const std::string& m,
                                                 const json& request) {
  // A synchronous design_sweep: its job, waited on off the frame. One the
  // handler would refuse is left to it, in its own words.
  if (m == "design_sweep" && request.is_object() &&
      request.value("async", json(false)) == json(false)) {
    try {
      ui::ReapBackgroundJobs(state);
      const RpcParams p{request};
      SweepPlan plan = PlanSweep(state, p);
      const bool outlive =
          request.value("outlive_client", json(false)) == json(true);
      const uint64_t job =
          StartSweep(state, std::move(plan), outlive ? 0 : state.rpcConnection);
      return std::make_unique<SweepWait>(job);
    } catch (...) {
      return nullptr;
    }
  }
  // A synchronous check with something to measure: the same job the async
  // form starts, waited on off the frame. Anything the handler would refuse
  // (a requirement that does not exist, a check already running) or answer
  // at once (nothing to measure) is left to it, in its own words.
  if (m != "check_requirements" || !request.is_object() ||
      request.value("async", json(false)) != json(false))
    return nullptr;
  try {
    ui::ReapBackgroundJobs(state);
    if (ui::FindBackgroundJob(state, kJobKind))
      return nullptr;
    std::vector<uint64_t> ids;
    if (request.contains("ids")) {
      for (const json& v : request.at("ids")) {
        if (!v.is_number_unsigned() ||
            FindRequirement(state, v.get<uint64_t>()) == nullptr)
          return nullptr;
        ids.push_back(v.get<uint64_t>());
      }
    } else {
      for (const req::Requirement& r : state.requirements)
        ids.push_back(r.id);
    }
    const bool recompute =
        request.value("recompute", json(false)) == json(true);
    const bool outlive =
        request.value("outlive_client", json(false)) == json(true);
    std::vector<uint64_t> todo;
    for (uint64_t id : ids)
      if (recompute || !Current(state, *FindRequirement(state, id)))
        todo.push_back(id);
    if (todo.empty())
      return nullptr;
    uint64_t job = 0;
    auto run = StartCheck(state, std::move(todo), ids, recompute,
                          outlive ? 0 : state.rpcConnection, &job);
    return std::make_unique<CheckWait>(job, std::move(run), std::move(ids));
  } catch (...) {
    return nullptr; // the handler says what is wrong
  }
}

std::optional<json> HandleRequirementsRpc(ui::AppState& state,
                                          const std::string& m,
                                          const RpcParams& p) {
  if (m == "add_requirement") {
    req::Requirement r;
    r.name = p.at("name").get<std::string>();
    if (r.name.empty())
      throw std::runtime_error("a requirement needs a name: what it is");
    r.source = p.value("source", std::string());
    r.measure = p.at("measure");
    r.bound = req::BoundFromJson(p.at("bound"));
    const json row = AddRequirement(state, std::move(r));
    return json{{"id", row.at("id")}, {"requirement", row}};
  }
  if (m == "set_requirement") {
    req::Requirement& live = NeedRequirement(state, p.at("id").get<uint64_t>());
    req::Requirement r = live;
    if (p.contains("name"))
      r.name = p.at("name").get<std::string>();
    if (p.contains("source"))
      r.source = p.at("source").get<std::string>();
    if (p.contains("measure"))
      r.measure = p.at("measure");
    if (p.contains("bound"))
      r.bound = req::BoundFromJson(p.at("bound"));
    RefuseDocumentByName(state, r.measure);
    req::Validate(r.measure, r.bound);
    RefuseUnresolvable(state, r);
    r.revision = live.revision + 1;
    // The old evidence was about the old requirement.
    r.evidence = {};
    live = std::move(r);
    MarkRequirementsEdited(state);
    RefreshExpressions(state);
    return json{{"requirement", Row(state, live)}};
  }
  if (m == "remove_requirement") {
    const uint64_t id = p.at("id").get<uint64_t>();
    NeedRequirement(state, id);
    std::erase_if(state.requirements,
                  [id](const req::Requirement& r) { return r.id == id; });
    MarkRequirementsEdited(state);
    return json{{"removed", id}};
  }
  if (m == "list_requirements") {
    // rpc-returns-from: src/remote/RemoteRequirements.cpp
    return RowsJson(state, nullptr);
  }
  if (m == "check_requirements") {
    // rpc-returns-from: src/remote/RemoteRequirements.cpp
    ui::ReapBackgroundJobs(state);
    std::vector<uint64_t> ids;
    if (p.contains("ids")) {
      for (const json& v : p.at("ids")) {
        NeedRequirement(state, v.get<uint64_t>());
        ids.push_back(v.get<uint64_t>());
      }
    } else {
      for (const req::Requirement& r : state.requirements)
        ids.push_back(r.id);
    }
    const bool recompute = p.value("recompute", false);
    const bool async = p.value("async", false);
    const bool outlive = p.value("outlive_client", false);
    if (ui::FindBackgroundJob(state, kJobKind))
      throw std::runtime_error(
          "a requirements check is already running (job " +
          std::to_string(ui::FindBackgroundJob(state, kJobKind)->id) +
          "): study_status {job} follows it, cancel_study {job} stops it");
    std::vector<uint64_t> todo;
    for (uint64_t id : ids)
      if (recompute || !Current(state, *FindRequirement(state, id)))
        todo.push_back(id);
    if (async && !todo.empty()) {
      uint64_t job = 0;
      StartCheck(state, todo, ids, recompute, outlive ? 0 : state.rpcConnection,
                 &job);
      json out = RowsJson(state, &ids);
      out["job"] = job;
      out["kind"] = kJobKind;
      out["started"] = true;
      out["checking"] = todo.size();
      out.erase("all_pass"); // not yet: the job is measuring
      return out;
    }
    const int returnTo = state.activePart;
    size_t checked = 0;
    for (uint64_t id : todo) {
      req::Requirement* r = FindRequirement(state, id);
      CheckOne(state, *r, Ctx{false, recompute});
      checked++;
    }
    Restore(state, returnTo);
    LogFindings(state, ids);
    json out = RowsJson(state, &ids);
    out["checked"] = checked;
    out["reused"] = ids.size() - checked;
    AttachReceipt(out, ReceiptFor(out.at("requirements")));
    return out;
  }

  if (m == "design_sweep") {
    // rpc-returns-from: src/remote/RemoteRequirements.cpp
    // rpc-params: async, corners, documents, from_requirements, max_points
    // rpc-params: outlive_client, refine, samples, variables
    ui::ReapBackgroundJobs(state);
    SweepPlan plan = PlanSweep(state, p);
    const bool async = p.value("async", false);
    const bool outlive = p.value("outlive_client", false);
    if (async) {
      json out = PlanJson(plan);
      out["job"] =
          StartSweep(state, std::move(plan), outlive ? 0 : state.rpcConnection);
      out["kind"] = kSweepKind;
      out["started"] = true;
      return out;
    }
    // Not parked (a caller in-process, a test): run it here, on copies all
    // the same.
    const auto started = std::chrono::steady_clock::now();
    const std::filesystem::path dir = SweepDir();
    struct Cleanup {
      std::filesystem::path dir;
      ~Cleanup() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
      }
    } cleanup{dir};
    const sweep::Inputs in = SweepInputs(state, plan, dir);
    sweep::Progress progress;
    json run = sweep::Run(in, progress);
    const double seconds = std::chrono::duration<double>(
                               std::chrono::steady_clock::now() - started)
                               .count();
    json out = AnswerOf(plan, std::move(run), seconds, 0);
    KeepSweep(0, out);
    return out;
  }
  if (m == "design_sweep_result") {
    // rpc-returns-from: src/remote/RemoteRequirements.cpp
    ui::ReapBackgroundJobs(state);
    const uint64_t job =
        p.contains("job") ? p.at("job").get<uint64_t>() : Sweeps().last;
    if (ui::BackgroundJob* j = ui::FindBackgroundJobById(state, job);
        j != nullptr && j->kind == kSweepKind)
      return json{{"job", job},
                  {"running", true},
                  {"fraction", j->fraction.load()},
                  {"label", j->label}};
    if (KeptSweep(job) == nullptr) {
      std::vector<std::string> have;
      for (const auto& [id, a] : Sweeps().answers)
        have.push_back(std::to_string(id));
      throw std::runtime_error((p.contains("job")
                                    ? "no design sweep " + std::to_string(job)
                                    : std::string("no design sweep has run")) +
                               "; the sweeps kept: " + KnownList(have));
    }
    return SweepAnswer(job);
  }

  // The declared clearances (v97), as clearance requirements.
  if (m == "add_assertion") {
    RequireAssembly(state);
    req::Requirement r;
    r.name = p.at("name").get<std::string>();
    r.source = p.value("why", std::string());
    r.measure = {{"kind", "clearance"},
                 {"document", ActiveSlotId(state)},
                 {"a", p.at("a").get<uint64_t>()},
                 {"b", p.at("b").get<uint64_t>()},
                 {"mode", p.value("mode", std::string("mesh"))}};
    r.bound.kind = "at_least";
    r.bound.value = p.at("min_mm").get<double>();
    if (!(r.bound.value >= 0.0))
      throw std::runtime_error("min_mm must be zero or positive");
    if (r.name.empty())
      throw std::runtime_error(
          "an assertion needs a `name` — it is what the clearance IS (\"prop "
          "tip to arm\"), and it is what a failure will be reported as");
    const json row = AddRequirement(state, std::move(r));
    return json{{"id", row.at("id")}, {"name", row.at("name")}};
  }
  if (m == "list_assertions") {
    RequireAssembly(state);
    const uint64_t me = ActiveSlotId(state);
    json rows = json::array();
    for (const req::Requirement& r : state.requirements)
      if (IsAssertionOf(r, me))
        rows.push_back(AssertionRow(state, r));
    return json{{"assertions", rows}};
  }
  if (m == "remove_assertion") {
    RequireAssembly(state);
    const uint64_t id = p.at("id").get<uint64_t>();
    const req::Requirement* r = FindRequirement(state, id);
    if (!r || !IsAssertionOf(*r, ActiveSlotId(state)))
      throw std::runtime_error("no declared clearance " + std::to_string(id) +
                               " on this assembly (list_assertions)");
    std::erase_if(state.requirements,
                  [id](const req::Requirement& x) { return x.id == id; });
    MarkRequirementsEdited(state);
    return json{{"removed", id}};
  }
  if (m == "verify_assertions") {
    RequireAssembly(state);
    const uint64_t me = ActiveSlotId(state);
    const bool one = p.contains("id");
    const uint64_t only = p.value("id", uint64_t{0});
    json rows = json::array();
    receipt::Receipt rec;
    bool found = false;
    for (req::Requirement& r : state.requirements) {
      if (!IsAssertionOf(r, me) || (one && r.id != only))
        continue;
      found = true;
      // Always measured now: "verify" is the act of looking again.
      CheckOne(state, r, Ctx{false, true});
      const json row = AssertionRow(state, r);
      const receipt::Oracle oracle{
          row.at("mode") == "mesh" ? "designer/mesh-gap" : "designer/exact-gap",
          "1"};
      if (row.at("standing") == "holds" || row.at("standing") == "violated") {
        receipt::Claim c(r.name, oracle,
                         row.at("standing") == "holds"
                             ? receipt::Verdict::Pass
                             : receipt::Verdict::Fail);
        c.value = row.value("measured_mm", 0.0);
        c.unit = "mm";
        c.note = row.at("mode") == "mesh"
                     ? "off the tessellations, with the combined deflection "
                       "already subtracted"
                     : "exact, on the B-Rep";
        rec.Add(std::move(c));
      } else {
        rec.Unverifiable(r.name, oracle, row.value("note", std::string()));
      }
      rows.push_back(row);
    }
    if (one && !found)
      throw std::runtime_error("no declared clearance " + std::to_string(only) +
                               " on this assembly (list_assertions)");
    json out{{"assertions", rows}};
    AttachReceipt(out, rec);
    return out;
  }
  return std::nullopt;
}

} // namespace remote
