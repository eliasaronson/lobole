#include "remote/RemoteSimulation.h"

#include "cad/CADMaterials.h"
#include "core/Command.h"
#include "core/Document.h"
#include "core/InputHash.h"
#include "core/Measure.h"
#include "core/Occurrence.h"
#include "core/Units.h"
#include "core/fem/FemSolver.h"
#include "core/fem/FemStudy.h"
#include "core/fem/FemTopo.h"
#include "core/io/Serialization.h"
#include "remote/RemoteHelpers.h"
#include "remote/RemoteProject.h" // RequireFreshTabName
#include "ui/AppState.h"
#include "ui/AssemblyActions.h" // PartDocById, SnapshotAssembly, OccurrenceMatrix
#include "ui/BackgroundJobs.h"
#include "ui/DocumentSlots.h" // AddPart, SetActivePart
#include "ui/ImportActions.h"
#include "ui/SimulationActions.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using nlohmann::json;

namespace remote {

namespace {

// Which study a verb that ACTS ON one is addressing.
//
// `id` is what every other object in this API answers to — a mate, a
// section, a relation, a configuration, a CAM setup — and the study family
// was the one holdout, naming its own subject `study` in all 23 of its
// methods. Both work now, exactly as `remove_occurrence` takes `id` or
// `occurrence`. A verb that acts on something ELSE and merely names the
// study for context (add_load, add_refinement, set_result_display) still
// takes `study` only: there, `id` would mean the load or the refinement.
//
// Takes the VALUES, not the params object, so both key names stay visible
// at the call site — `rpc_params_gate` reads the handler for the keys it
// touches, and a helper that reached into `p` itself once made add_mate
// look as though it had quietly stopped accepting either.
uint64_t StudySubject(bool hasId, uint64_t id, bool hasStudy, uint64_t study) {
  if (hasId && hasStudy && id != study)
    throw std::runtime_error(
        "id and study name different studies — pass one of them");
  if (hasId)
    return id;
  if (hasStudy)
    return study;
  throw std::runtime_error(
      "which study? pass `id` (or `study`, which means the same here)");
}

// --- A study job by its id (docs/simulation/README.md#studies-as-jobs) ----

bool IsStudyJobKind(const std::string& kind) {
  return kind == "motion" || kind == "topo" || kind == "study" ||
         kind == "mesh" || kind == "check";
}

// The study jobs there are to ask about: running, then recently ended.
std::string KnownStudyJobs(const ui::AppState& state) {
  std::vector<std::string> have;
  for (const ui::BackgroundJobInfo& j : ui::ListBackgroundJobs(state))
    if (IsStudyJobKind(j.kind))
      have.push_back(std::to_string(j.id) + " (" + j.kind + ", running)");
  for (const ui::FinishedJob& f : state.finishedJobs)
    if (IsStudyJobKind(f.kind))
      have.push_back(std::to_string(f.id) + " (" + f.kind + ", " + f.outcome +
                     ")");
  return KnownList(have);
}

[[noreturn]] void RefuseNotStudyJob(uint64_t id, const std::string& kind) {
  throw std::runtime_error("job " + std::to_string(id) + " is a '" + kind +
                           "' job, not a study -- background_jobs lists it and "
                           "cancel_background_job {kind} stops it");
}

// study_status {job} and cancel_study {job}: a running job reports how far
// it is (or is asked to stop); one that has ended reports how it ended. An
// id that names no study job is refused, naming the ones there are.
json StudyJobJson(ui::AppState& state, uint64_t id, bool cancel) {
  if (ui::BackgroundJob* j = ui::FindBackgroundJobById(state, id)) {
    if (!IsStudyJobKind(j->kind))
      RefuseNotStudyJob(id, j->kind);
    if (cancel) {
      if (!j->cancellable)
        throw std::runtime_error("job " + std::to_string(id) +
                                 " cannot be cancelled");
      ui::CancelBackgroundJobById(state, id, "cancel_study");
      return json{{"job", id},
                  {"kind", j->kind},
                  {"study", j->studyId},
                  {"cancelled", true}};
    }
    const double elapsed = std::chrono::duration<double>(
                               std::chrono::steady_clock::now() - j->started)
                               .count();
    json out{{"job", id},
             {"kind", j->kind},
             {"study", j->studyId},
             {"label", j->label},
             {"running", true},
             {"elapsed_seconds", elapsed},
             {"cancellable", j->cancellable},
             {"cancel_requested", j->cancel.load()},
             {"outlives_client", j->owner == 0}};
    const double fraction = j->fraction.load();
    out["fraction"] = fraction >= 0.0 ? json(fraction) : json(nullptr);
    return out;
  }
  if (const ui::FinishedJob* f = ui::FinishedJobById(state, id)) {
    if (!IsStudyJobKind(f->kind))
      RefuseNotStudyJob(id, f->kind);
    if (cancel)
      throw std::runtime_error("job " + std::to_string(id) +
                               " has already ended (" + f->outcome +
                               "); there is nothing to cancel");
    json out{{"job", id},
             {"kind", f->kind},
             {"study", f->studyId},
             {"label", f->label},
             {"running", false},
             {"outcome", f->outcome},
             {"seconds", f->seconds}};
    if (!f->error.empty())
      out["error"] = f->error;
    if (!f->cancelReason.empty())
      out["cancel_reason"] = f->cancelReason;
    return out;
  }
  throw std::runtime_error("no study job " + std::to_string(id) +
                           "; study jobs: " + KnownStudyJobs(state));
}

json KeepOutJson(const fem::KeepOut& k) {
  if (!k.body.empty())
    return json{{"body", k.body}};
  return json{{"box_min", {k.boxMin.x, k.boxMin.y, k.boxMin.z}},
              {"box_max", {k.boxMax.x, k.boxMax.y, k.boxMax.z}}};
}

json ShapeFromJson(const fem::ShapeSource& s) {
  json j{{"study", s.study}};
  if (s.whole)
    j["whole"] = true;
  else
    j["threshold"] = s.threshold;
  return j;
}

// A load case named by `v` (its name, or its 0-based index) among `names`;
// refused naming the cases there are.
int CaseIndex(const std::vector<std::string>& names, const json& v) {
  std::string known;
  for (const std::string& n : names)
    known += (known.empty() ? "'" : ", '") + n + "'";
  if (names.empty())
    throw std::runtime_error("`case`: this study has no load cases");
  if (v.is_number_integer()) {
    const int k = v.get<int>();
    if (k < 0 || k >= static_cast<int>(names.size()))
      throw std::runtime_error("`case` " + std::to_string(k) +
                               " is out of range; the cases are " + known);
    return k;
  }
  if (v.is_string())
    for (size_t k = 0; k < names.size(); ++k)
      if (names[k] == v.get<std::string>())
        return static_cast<int>(k);
  throw std::runtime_error("no load case " + v.dump() + "; the cases are " +
                           known);
}

// Where the studies live: the active PART, or the active ASSEMBLY
// (docs/simulation/README.md#assembly-studies). One handler, two homes — the
// study record is the same, and the verbs are the same; what differs is how
// an edit becomes an undo step (a command on a part, a full-state snapshot
// on an assembly) and where a result's staleness is measured.
struct Host {
  ui::AppState& state;
  Document* part = nullptr;
  AssemblyDocument* asmDoc = nullptr;

  bool IsAssembly() const { return asmDoc != nullptr; }
  const std::vector<fem::Study>& Studies() const {
    return part ? part->GetStudies() : asmDoc->GetStudies();
  }
  // A RUN's access (Document::StudyForRun): meshing and solving write the
  // study's transient fields. An edit is Modify, below, which a part records
  // as a command and so as a saved change.
  fem::Study* Find(uint64_t id) {
    return part ? part->StudyForRun(id) : asmDoc->FindStudy(id);
  }
  fem::Study& Require(uint64_t id) {
    fem::Study* s = Find(id);
    if (!s)
      throw std::runtime_error("no such study");
    return *s;
  }
  const fem::StudyRuntime* Results(uint64_t id) const {
    return part ? part->StudyResults(id) : asmDoc->StudyResults(id);
  }
  bool Stale(uint64_t id) {
    return part ? part->StudyResultsStale(id)
                : ui::AssemblyStudyStale(state, *asmDoc, id);
  }
  uint64_t AllocateId() {
    return part ? part->AllocateId() : asmDoc->AllocateId();
  }
  const SymbolTable& Symbols() const {
    return part ? part->Symbols() : state.projectSymbols;
  }
  const fem::Study& Add(const std::string& name, fem::AnalysisType type) {
    if (part) {
      auto cmd = std::make_unique<AddStudyCommand>(name, type);
      AddStudyCommand* raw = cmd.get();
      state.commands->Execute(std::move(cmd), *part);
      return *part->FindStudy(raw->StudyId());
    }
    ui::SnapshotAssembly(state);
    fem::Study& s = asmDoc->AddStudy(name);
    s.type = type;
    return s;
  }
  void Remove(uint64_t id) {
    Require(id);
    if (part) {
      state.commands->Execute(std::make_unique<RemoveStudyCommand>(id), *part);
      return;
    }
    ui::SnapshotAssembly(state);
    asmDoc->RemoveStudy(id);
  }
  // One undoable mutation of one study.
  void Modify(uint64_t id, const std::string& desc,
              const std::function<void(fem::Study&)>& mutate) {
    const fem::Study& cur = Require(id);
    if (part) {
      json before = DocumentIO::StudyToJson(cur);
      fem::Study copy = cur;
      mutate(copy);
      json after = DocumentIO::StudyToJson(copy);
      state.commands->Execute(
          std::make_unique<ModifyStudyCommand>(desc, before, after), *part);
      return;
    }
    fem::Study copy = cur;
    mutate(copy); // may throw, BEFORE the snapshot: nothing to undo then
    ui::SnapshotAssembly(state);
    *asmDoc->FindStudy(id) = std::move(copy);
    asmDoc->BumpVersion(); // an edit invalidates results, as on a part
  }
  // The material of a solved body, for the factor of safety: a part's body
  // by index, an assembly's meshed occurrence's placed body by index.
  std::string BodyMaterial(const fem::Study& s, int bodyIndex) {
    if (part) {
      const auto& bodies = part->GetBodies();
      return bodyIndex >= 0 && bodyIndex < (int)bodies.size()
                 ? bodies[bodyIndex].material
                 : std::string{};
    }
    for (const fem::Participant& pt : s.participants)
      if (pt.role == fem::ParticipantRole::Meshed) {
        const auto bodies = ui::OccurrenceSubtreeBodies(state, pt.occurrenceId);
        return bodyIndex >= 0 && bodyIndex < (int)bodies.size()
                   ? bodies[bodyIndex].material
                   : std::string{};
      }
    return {};
  }
  // An occurrence of the assembly, by id or by name.
  const Occurrence& OccurrenceOf(const json& v, const char* what) {
    if (!asmDoc)
      throw std::runtime_error(std::string(what) +
                               " names an occurrence, which only an "
                               "assembly's study has");
    if (v.is_number_unsigned() || v.is_number_integer()) {
      const Occurrence* occ = asmDoc->FindById(v.get<uint64_t>());
      if (!occ)
        throw std::runtime_error(std::string(what) +
                                 ": no occurrence with id " +
                                 std::to_string(v.get<uint64_t>()) +
                                 " (list_occurrences names them)");
      return *occ;
    }
    if (v.is_string()) {
      for (const Occurrence& occ : asmDoc->GetOccurrences())
        if (occ.name == v.get<std::string>())
          return occ;
      throw std::runtime_error(std::string(what) + ": no occurrence named '" +
                               v.get<std::string>() +
                               "' (list_occurrences names them)");
    }
    throw std::runtime_error(std::string(what) +
                             " is an occurrence id or name");
  }
  std::string OccurrenceName(uint64_t id) const {
    const Occurrence* occ = asmDoc ? asmDoc->FindById(id) : nullptr;
    return occ ? occ->name : std::to_string(id);
  }
};

} // namespace

// FEM study RPCs, carved out of RemoteServer::HandleSemantic. All act on the
// active PART or, since S1, the active ASSEMBLY (see Host above); a drawing
// or job tab is refused by the handler, naming where studies live. Returns the
// result json when `m` is one of ours, std::nullopt otherwise; throws
// std::runtime_error on error (the caller's try/catch turns it into an error
// response). describe: one row per `m ==` branch below — extend together.
static constexpr MethodDoc SimulationMethods[] = {
    {"enter_simulation",
     "Enter simulation mode on the active part or assembly (S1: an assembly's "
     "studies live on it, drawn in its Simulation tool window).",
     "study"},
    {"exit_simulation", "Leave simulation mode.", ""},
    {"add_study",
     "Add a study {type: static|modal|topology, name?}. `kind` is accepted "
     "for the same thing — the enum, the file and `list_studies` all say "
     "`type`, and so do add_mate/add_load/add_cam_op/add_relation.",
     "kind, name, type"},
    {"remove_study",
     "Remove a study {id}. `study` names the same thing here and in every "
     "verb that ACTS ON a study (set_study, mesh_study, run_study, "
     "study_results, study_status, cancel_study): `id` is what a mate, a "
     "section, a relation, a configuration and a CAM setup all answer to, "
     "and this family was the holdout. A verb that only names a study for "
     "CONTEXT (add_load, add_refinement, set_result_display) still takes "
     "`study`, because there `id` would mean the load or the refinement.",
     "id, study"},
    {"list_studies", "List studies with status.", "", remote::NoFlags,
     "studies[]{id,name,type,mesh_size,loads,refinements,has_results,stale,"
     "participants?,volume_fraction?,filter_radius?,max_iterations?,keep_faces?"
     ",keep_outs?,load_cases?,shape_from?}"},
    {"set_study",
     "Edit study settings (mesh size, materials, solver options). mesh_size "
     "and filter_radius are MILLIMETRES; `gravity` is metres per second "
     "squared (m/s^2) as a [x, y, z] vector, so Earth down is [0, 0, -9.81] "
     "with Z up. volume_fraction is a fraction of 1 and the iteration counts "
     "are counts. Takes `id` (or `study`, the same thing here) — see "
     "remove_study.\n"
     "On an ASSEMBLY, `participants` is the study: a list of {occurrence, "
     "role: meshed|bonded|rigid|ignored, mesh_size?}. Exactly one occurrence "
     "is `meshed`; `bonded` is meshed AND glued to it where they touch, with "
     "its own material; `rigid` is not meshed and holds the meshed part "
     "where they touch — but only once CLAMPED, which is a load "
     "(add_load {type: fixed, contact_with}), not a consequence of the role. "
     "A clamp is resolved against the whole group, so a rigid support that "
     "touches only a bonded member holds it just the same; one that touches "
     "nothing meshed is refused by name.\n"
     "`load_cases` is the whole list of a static or topology study's load "
     "cases, [{name, weight?}] (weight a plain factor, default 1, or an "
     "expression): each force or pressure then names its case (add_load "
     "`case`) and the supports hold in all of them. A topology study "
     "minimises the weighted sum of its cases' compliances; a static study "
     "solves every case on one factorisation and reports each "
     "(docs/topopt/README.md#load-cases). `shape_from` makes a STATIC study "
     "solve a topology study's result instead of meshing the body: {study, "
     "threshold? (0..1, 0 = the volume-matched level)} or {study, whole: "
     "true} (every tet of its design domain, the solid reference); null "
     "clears it "
     "(docs/topopt/README.md#a-static-study-of-the-optimised-shape).",
     "filter_radius, gravity, id, load_cases, max_iterations, mesh_size, "
     "name, num_modes, participants, shape_from, study, type, "
     "volume_fraction"},
    {"add_load",
     "Add a load/fixture to a study {study, type: fixed|force|pressure, face, "
     "magnitude, direction?}. `magnitude` is NEWTONS for a force -- the total "
     "on that face, spread over its area -- and MPa for a pressure, which "
     "pushes along the inward normal. `fixed` ignores it. Every result scales "
     "linearly with this number and a wrong unit is invisible in the answer, "
     "so it is stated here rather than left to be inferred from the solver. "
     "An omitted force `direction` pushes into the picked face (the same "
     "default a GUI placement takes). A `face` is {feature_id, element}, or "
     "just the element name — the body that has it is then found for you. "
     "On a study with load cases a force or pressure names its `case`; a "
     "support names none (it holds in every case).",
     "case, contact_with, direction, face, magnitude, occurrence, study, "
     "type"},
    {"remove_load",
     "Remove a load from a study {study, id}. `load` is accepted for the "
     "same thing. It is the load's ID, as `add_load` and `list_studies` "
     "report it — this row said \"by index\", which is the same number only "
     "until you remove one.",
     "id, load, study"},
    {"add_refinement",
     "Add a local mesh refinement on an element: `size` is the target element "
     "edge length there, in MILLIMETRES.",
     "face, occurrence, size, study"},
    {"remove_refinement", "Remove a refinement.", "face, study"},
    {"add_keep_face", "Topology optimization: mark a face to keep.",
     "face, study"},
    {"add_keep_out",
     "Topology optimization: a region the result must leave empty "
     "{study, body} -- a body of this part, by name (a neighbour arrives as "
     "a Derived reference body) -- or {study, box_min, box_max}, a box in "
     "part coordinates, both corners [x, y, z] in MILLIMETRES. The tets "
     "whose centroids fall inside are removed before the run: void, and not "
     "design space (the volume fraction is of what remains). A body the "
     "part does not have is refused naming those it has; a keep-out that "
     "would take a support's, a load's or a keep face's tet is refused at "
     "the run (docs/topopt/README.md#keep-outs). Returns its index.",
     "body, box_max, box_min, study"},
    {"remove_keep_out",
     "Remove a keep-out {study, index} (0-based, as list_studies lists "
     "them).",
     "index, study"},
    {"check_topology",
     "A static check of a topology result before re-modelling from it "
     "{study, threshold?}: makes (or refreshes) two static studies of the "
     "result -- '<name>: shape', its tets at or above `threshold` (0..1, "
     "default the volume-matched level) connected to the supports, and "
     "'<name>: solid', every tet of the design domain -- with the topology "
     "study's supports, loads and load cases, and solves both as a JOB "
     "(kind 'check'; one factorisation each serves every case): it answers "
     "{started, job, kind, shape_study, solid_study}, study_status {job} "
     "follows it, cancel_study {job} stops it, and it is cancelled with "
     "its connection unless `outlive_client: true`. When both studies "
     "hold results for their inputs (a check already run, or a reopened "
     "project's) nothing starts and it answers per case the shape's and "
     "the solid's max_displacement_mm, max_von_mises_mpa and "
     "factor_of_safety, their displacement ratio, and each one's "
     "volume_mm3 and mass_g, `reused: true`: call it again when the job "
     "is done. `recompute: true` solves anyway. The two studies stay, for "
     "probe_study {case} and set_result_display {case}. Solid elements at "
     "a threshold, not SIMP-penalised stiffness "
     "(docs/topopt/README.md#a-static-study-of-the-optimised-shape).",
     "outlive_client, recompute, study, threshold", remote::NoFlags,
     "{started, job, kind, shape_study, solid_study, outlives_client} | "
     "{reused, topology, threshold, shape_study, solid_study, dropped_tets, "
     "islands, cases[]{name, shape{study, max_displacement_mm, "
     "max_displacement_at, max_von_mises_mpa, max_von_mises_at, "
     "factor_of_safety?, volume_mm3, mass_g?}, solid{...}, "
     "displacement_ratio}}"},
    {"remove_keep_face", "Unmark a kept face.", "face, study"},
    {"mesh_study",
     "Mesh the study (Netgen tets); returns element counts/quality. "
     "Synchronous by default — a fine mesh takes minutes, so either give the "
     "client a long timeout (the server finishes even if the client gives "
     "up) or pass `async: true`: the mesh then runs on a background job and "
     "study_status answers {running, meshing: true} until it lands, after "
     "which its `mesh` carries the same bodies/nodes/elements/worst_quality. "
     "Async answers `job` (its id) and, like run_study's, the job is "
     "cancelled when the connection that started it closes unless "
     "`outlive_client: true`. "
     "Takes `id` (or `study`, the same thing here) — see remove_study.",
     "async, id, outlive_client, study"},
    {"run_study",
     "Solve the study (blocking, or `async` and poll study_status). Async "
     "meshes AND solves off the frame; the result is stored into the part "
     "the study belongs to even if another tab is active when it lands. A "
     "topology study always runs async. An async run is a JOB: the reply's "
     "`job` is its id, which study_status {job} and cancel_study {job} take, "
     "and it is cancelled when the connection that started it closes "
     "unless `outlive_client: true`. A run whose INPUTS hash the same as a "
     "stored result (the study with its expressions evaluated, the bodies' "
     "exact geometry, names and materials; on an assembly the taking-part "
     "occurrences and their placed bodies) is answered from it with "
     "`reused: true` and nothing computed -- a reopened project's result "
     "of any kind included; `input_hash` names the inputs, `recompute: true` "
     "runs anyway. Takes "
     "`id` (or `study`, the same thing here) — see remove_study.",
     "async, id, outlive_client, recompute, study"},
    {"cancel_study",
     "Cancel a running study: {job} (the id run_study, run_motion_study or "
     "mesh_study answered -- any study job, motion included) or `id` (or "
     "`study`, the same thing here; a part or assembly study's running "
     "job). A topology run keeps the best shape so far; a static or modal "
     "run stops at its next boundary (after meshing; the factorization has "
     "no interruption points and ends first) and drops its answer. A job "
     "id that names no study job, or one that has ended, is refused.",
     "id, job, study"},
    {"study_status",
     "Status/progress of a study: {running, job, meshing?, iteration…} "
     "while a job runs, else the cached result's state (with `input_hash`) "
     "plus `mesh` from the last mesh_study {async}. Takes `id` (or `study`, "
     "the same thing here) — see remove_study — or `job`, a study job's id "
     "from any tab: {job, kind, study, running, fraction, elapsed_seconds, "
     "cancellable, outlives_client} while it runs, {running: false, outcome "
     "(done|cancelled|failed), error?, cancel_reason?, seconds} once it has "
     "ended; an id that names no study job is refused, naming those there "
     "are.",
     "id, job, study"},
    {"study_results",
     "Result summary: {type, elements, nodes, stale} plus a `bodies` "
     "array, one entry PER BODY, because a part study meshes and solves each "
     "body separately (a part's bodies share no bonded contact); an assembly "
     "study bonds its participants "
     "(docs/simulation/README.md#assembly-studies). A body carries "
     "max_displacement_mm: {value, at}, max_von_mises_mpa: {value, at} and "
     "factor_of_safety for a static study, `frequencies_hz` for a modal one, "
     "and the optimization summary for a topology one; the static maxima are "
     "also aggregated at the top level. Read a modal result as "
     "`bodies[i].frequencies_hz` — this row used to name the per-body keys "
     "as though they were top-level, which is a KeyError rather than a wrong "
     "answer, but only after the solve has been paid for. Refused with "
     "\"study has not been run\" when there is "
     "nothing to report: a study never run. A result is saved with the "
     "project beside its study and reads current after a reopen while its "
     "inputs hash as they did, stale otherwise (`has_results`, `stale` in "
     "list_studies). Takes `id` (or "
     "`study`, the same thing here) — see remove_study.",
     "id, study"},
    {"set_result_display",
     "Choose the result field the VIEWPORT draws, and how far it exaggerates "
     "the deformation {study, field: von_mises|displacement|shape|density|"
     "none, deform_scale?, mode? (0-based, which mode shape for a modal "
     "study), threshold?}. It colours the raster viewport, which is what "
     "`screenshot` captures. It does NOT reach `render_still`: the path "
     "tracer renders the MODEL, so a still taken after this shows the "
     "undeformed part in its own material and looks like the call silently "
     "did nothing. A static study with load cases shows `case` (its name or "
     "0-based index).",
     "case, deform_scale, field, mode, study, threshold"},
    {"probe_study",
     "How one FACE moved under a solved static PART study, as a rigid body "
     "{id (or study), face, at?}: the least-squares small-displacement fit "
     "of the face's mesh nodes, u(p) = translation + rotation x (p - "
     "centroid), each node weighted by the area it stands for. Returns "
     "{nodes, area_mm2, centroid_mm, translation_mm (the centroid's "
     "displacement, MILLIMETRES), rotation_rad (a small-angle rotation "
     "vector, RADIANS), rotation_magnitude_deg, rms_residual_mm (what the "
     "rigid fit leaves: the face's own distortion)}. `at` is a point "
     "[x, y, z] in MILLIMETRES (part coordinates): the fitted motion carried "
     "there comes back as displacement_at_mm -- where whatever the face "
     "holds rigidly goes, a bearing seat's shaft, the lens on that shaft. "
     "The maxima study_results reports are somewhere on the part; this is "
     "what one seat did. Refused on a stale study (re-run it: the face "
     "named now may not be the face that was meshed), an assembly study, "
     "or a modal or topology one (docs/simulation/README.md#probing-a-face). "
     "A study with load cases takes `case` (its name or 0-based index; "
     "default the first).",
     "at, case, face, id, study"},
    {"set_topo_ghost", "Toggle the topology-optimization ghost overlay.",
     "on, study"},
    {"export_topo_stl", "Export the optimized topology as STL.",
     "path, smooth, study, threshold"},
    {"topo_to_body",
     "A topology result as a body to model against {study, path, name?, "
     "threshold?, smooth?}: the smoothed iso-surface (export_topo_stl's) is "
     "written to `path` and imported, in the part's own coordinates, as a "
     "new part `name` (default '<study> shape') with the design body's "
     "material -- a sewn solid of the surface's triangles that booleans, "
     "measures, sections and derived copies take. The import re-reads "
     "`path` when it regenerates, so keep the file with the project. "
     "Returns the part, its volume_mm3, the triangle count and whether the "
     "surface was watertight (docs/topopt/README.md#a-body-from-the-result).",
     "name, path, smooth, study, threshold", remote::NotInBatch,
     "{part, part_id, path, triangles, watertight, threshold, volume_mm3, "
     "valid}"},
};
std::span<const MethodDoc> SimulationMethodDocs() { return SimulationMethods; }

namespace {

// check_topology (docs/topopt/README.md#a-static-study-of-the-optimised-shape):
// the topology result's shape and its solid design domain, each a static
// study with the topology study's supports, loads and cases, run now and
// compared case by case.
json CheckTopology(ui::AppState& state, Host& host, Document* doc,
                   const RpcParams& p) {
  ui::ReapBackgroundJobs(state);
  if (host.IsAssembly() || doc == nullptr)
    throw std::runtime_error("check_topology reads a part's topology study");
  const uint64_t id = p.at("study").get<uint64_t>();
  const fem::Study topo = host.Require(id); // a copy: studies are added below
  if (topo.type != fem::AnalysisType::Topology)
    throw std::runtime_error("check_topology: study " + std::to_string(id) +
                             " is not a topology study");
  const fem::StudyRuntime* trt = host.Results(id);
  if (trt == nullptr)
    throw std::runtime_error("check_topology: '" + topo.name +
                             "' has not been run");
  if (host.Stale(id))
    throw std::runtime_error(
        "check_topology: '" + topo.name +
        "' is stale -- the part changed after it ran; run it again first");
  const double threshold = p.value("threshold", 0.0);
  if (threshold < 0.0 || threshold >= 1.0)
    throw std::runtime_error(
        "threshold must be in [0, 1) (0 = the volume-matched level)");

  // The two studies, found by what they are (a static study of this result,
  // whole or not) so a second check refreshes them rather than adding more,
  // solved as a job unless both hold results for their inputs.
  uint64_t shapeId = 0, solidId = 0;
  std::string err;
  ui::JobStart start;
  start.recompute = p.value("recompute", false);
  start.owner = p.value("outlive_client", false) ? 0 : state.rpcConnection;
  if (!ui::StartTopologyCheck(state, *doc, id, threshold, &shapeId, &solidId,
                              &err, &start))
    throw std::runtime_error("check_topology: " + err);
  if (!start.reused)
    return json{{"started", true},
                {"running", true},
                {"job", start.jobId},
                {"kind", "check"},
                {"shape_study", shapeId},
                {"solid_study", solidId},
                {"outlives_client", start.owner == 0}};
  const fem::StudyRuntime* shape = host.Results(shapeId);
  const fem::StudyRuntime* solid = host.Results(solidId);
  const fem::Study* shapeStudy = host.Find(shapeId);

  // Mass from the material's density and the solved solid's volume.
  const std::string material =
      trt->bodies.empty()
          ? std::string{}
          : host.BodyMaterial(*shapeStudy, trt->bodies.front().bodyIndex);
  const auto& mp = CADMaterials::MaterialLibrary::GetMaterial(
      CADMaterials::MaterialLibrary::MaterialTypeFromString(material));
  const double yield = mp.yieldMPa;
  const auto side = [&](const fem::StudyRuntime& rt, int k, uint64_t sid) {
    const fem::FemResults& r = rt.bodies.front().Case(k);
    json j{
        {"study", sid},
        {"max_displacement_mm", r.maxDisplacement},
        {"max_displacement_at", {r.maxDispAt.x, r.maxDispAt.y, r.maxDispAt.z}},
        {"max_von_mises_mpa", r.maxVonMises},
        {"max_von_mises_at", {r.maxVmAt.x, r.maxVmAt.y, r.maxVmAt.z}},
        {"volume_mm3", rt.shapeVolume}};
    if (yield > 0 && r.maxVonMises > 0)
      j["factor_of_safety"] = yield / r.maxVonMises;
    if (mp.density > 0)
      j["mass_g"] = rt.shapeVolume * 1e-9 * mp.density * 1000.0;
    return j;
  };
  json cases = json::array();
  const size_t n = std::max<size_t>(1, shape->caseNames.size());
  for (size_t k = 0; k < n; ++k) {
    json a = side(*shape, static_cast<int>(k), shapeId);
    json b = side(*solid, static_cast<int>(k), solidId);
    const double da = a["max_displacement_mm"].get<double>();
    const double db = b["max_displacement_mm"].get<double>();
    json c{{"name",
            shape->caseNames.empty() ? std::string{} : shape->caseNames[k]},
           {"shape", std::move(a)},
           {"solid", std::move(b)},
           {"displacement_ratio", db > 0 ? da / db : 0.0}};
    cases.push_back(std::move(c));
  }
  return json{{"reused", true},
              {"topology", id},
              {"threshold", shape->shapeThreshold},
              {"threshold_lowered_from", shape->shapeThresholdLoweredFrom},
              {"shape_study", shapeId},
              {"solid_study", solidId},
              {"dropped_tets", shape->shapeIslandTets},
              {"islands",
               {{"components", trt->islands.components},
                {"tets", trt->islands.tets},
                {"volume_mm3", trt->islands.volume},
                {"keep_faces", trt->islands.keepFaces},
                {"load_path_broken", trt->islands.loadPathBroken}}},
              {"cases", std::move(cases)}};
}

} // namespace

std::optional<json> HandleSimulationRpc(ui::AppState& state, Document* doc,
                                        const std::string& m,
                                        const RpcParams& p) {
  // Only ours past this point; and only where studies live.
  {
    bool ours = false;
    for (const MethodDoc& d : SimulationMethods)
      ours = ours || m == d.name;
    if (!ours)
      return std::nullopt;
  }
  // A study JOB is asked about by its id from any tab: it may be running on
  // one that is not active (a motion study's assembly, a part behind it).
  if ((m == "study_status" || m == "cancel_study") && p.contains("job")) {
    if (p.contains("id") || p.contains("study"))
      throw std::runtime_error(
          "job and id/study both name what to ask about; pass one -- job is "
          "the id run_study or run_motion_study answered, id the study's");
    ui::ReapBackgroundJobs(state);
    return StudyJobJson(state, p.at("job").get<uint64_t>(),
                        m == "cancel_study");
  }
  if (!doc && !state.assembly) {
    const char* kind = state.drawing ? "a drawing"
                       : state.job   ? "a job"
                                     : "not a part";
    throw std::runtime_error("the active document is " + std::string(kind) +
                             "; studies live on a part or an assembly — switch "
                             "to one of those tabs");
  }
  Host host{state, doc, doc ? nullptr : state.assembly.get()};
  const auto topoOnly = [&]() {
    if (host.IsAssembly())
      throw std::runtime_error("topology optimisation is a part study; an "
                               "assembly study is static or modal");
  };
  if (m == "add_study") {
    // Optional `kind` sets the analysis type at creation (the doc row always
    // promised it; it used to require a second set_study call). Folding it
    // into the create keeps this one undo step.
    // `type` is what the enum (fem::AnalysisType), the serialization
    // (AnalysisTypeToString) and `list_studies` all call this, and what the
    // other add_ verbs take (add_mate, add_load, add_cam_op, add_relation).
    // This one asked for `kind` and then reported `type` back, so the word
    // you wrote was not the word you read. Both are accepted.
    fem::AnalysisType type = fem::AnalysisType::Static;
    const bool hasKind = p.contains("kind") || p.contains("type");
    const std::string want = p.contains("type")
                                 ? p.at("type").get<std::string>()
                                 : p.value("kind", std::string{});
    if (hasKind && !fem::AnalysisTypeFromString(want, type))
      throw std::runtime_error("study type must be static, modal, or topology");
    if (hasKind && type == fem::AnalysisType::Topology)
      topoOnly();
    const fem::Study* s = &host.Add(p.value("name", std::string{}), type);
    return json{{"id", s->id},
                {"name", s->name},
                {"type", fem::AnalysisTypeToString(s->type)}};
  }
  if (m == "remove_study") {
    const uint64_t id =
        StudySubject(p.contains("id"), p.value("id", uint64_t{0}),
                     p.contains("study"), p.value("study", uint64_t{0}));
    host.Remove(id);
    return json::object();
  }
  if (m == "list_studies") {
    json studies = json::array();
    for (const fem::Study& s : host.Studies()) {
      // Report evaluated sizes/magnitudes (against current parameters) without
      // mutating the stored study.
      fem::Study eval = s;
      fem::EvaluateStudyExprs(eval, host.Symbols(), nullptr);
      json loads = json::array();
      for (const fem::Load& l : eval.loads) {
        json lj = {{"id", l.id}, {"type", fem::LoadTypeToString(l.type)}};
        // An assembly study's anchor says whose face it is, or which
        // occurrence's contact it clamps (S1).
        if (l.contactWith != 0) {
          lj["contact_with"] = l.contactWith;
          lj["contact_with_name"] = host.OccurrenceName(l.contactWith);
        } else {
          lj["face"] = {{"feature_id", l.face.featureId},
                        {"element", l.face.name}};
        }
        if (l.occurrenceId != 0) {
          lj["occurrence"] = l.occurrenceId;
          lj["occurrence_name"] = host.OccurrenceName(l.occurrenceId);
        }
        if (l.type != fem::LoadType::Fixed)
          lj["magnitude"] = l.magnitude;
        if (l.type == fem::LoadType::Force)
          lj["direction"] = {l.direction.x, l.direction.y, l.direction.z};
        if (!l.loadCase.empty())
          lj["case"] = l.loadCase;
        loads.push_back(std::move(lj));
      }
      json sj = {{"id", s.id},
                 {"name", s.name},
                 {"type", fem::AnalysisTypeToString(s.type)},
                 {"mesh_size", eval.meshSize},
                 {"loads", loads},
                 {"refinements", eval.refinements.size()},
                 {"has_results", host.Results(s.id) != nullptr},
                 {"stale", host.Stale(s.id)}};
      if (!eval.participants.empty()) {
        json parts = json::array();
        for (const fem::Participant& pt : eval.participants) {
          json pj = {{"occurrence", pt.occurrenceId},
                     {"name", host.OccurrenceName(pt.occurrenceId)},
                     {"role", fem::ParticipantRoleToString(pt.role)}};
          if (pt.meshSize > 0.0)
            pj["mesh_size"] = pt.meshSize;
          parts.push_back(std::move(pj));
        }
        sj["participants"] = parts;
      }
      if (s.type == fem::AnalysisType::Topology) {
        sj["volume_fraction"] = eval.volumeFraction;
        sj["filter_radius"] = eval.filterRadius;
        sj["max_iterations"] = eval.maxIterations;
        sj["keep_faces"] = eval.keepFaces.size();
        json kos = json::array();
        for (const fem::KeepOut& k : eval.keepOuts)
          kos.push_back(KeepOutJson(k));
        sj["keep_outs"] = std::move(kos);
      }
      if (!eval.loadCases.empty()) {
        json cs = json::array();
        for (const fem::LoadCase& c : eval.loadCases)
          cs.push_back({{"name", c.name}, {"weight", c.weight}});
        sj["load_cases"] = std::move(cs);
      }
      if (eval.shapeFrom.study != 0)
        sj["shape_from"] = ShapeFromJson(eval.shapeFrom);
      studies.push_back(std::move(sj));
    }
    return json{{"studies", studies}};
  }

  // Face-ref decode shared by the study mutation RPCs.
  //
  // An element name already CARRIES its feature id -- "30/face0", "1/b1/f2"
  // -- so demanding `feature_id` beside it made the caller repeat what they
  // had just been handed by list_elements, and getting it wrong read as
  // nlohmann's "key 'feature_id' not found". Take the name alone (as a bare
  // string, or as {element}) and read the id off its prefix; an explicit
  // feature_id still wins.
  // A face is {feature_id, element} — or just the element name, as a bare
  // string or {element}, in which case the body that HAS it is found. The
  // name's own prefix is not the answer: it is the feature that created the
  // element, which after a boolean is not the body's created_by.
  auto studyFaceOf = [](const json& r) {
    if (r.is_string())
      return ElementRef{0, r.get<std::string>()};
    if (!r.is_object() || !r.contains("element"))
      throw std::runtime_error(
          "a face is {feature_id, element}, or just the element name "
          "(list_elements names them) and the body that has it is found");
    return ElementRef{r.contains("feature_id")
                          ? r.at("feature_id").get<uint64_t>()
                          : uint64_t{0},
                      r.at("element").get<std::string>()};
  };
  // Runs one study mutation as a ModifyStudyCommand: snapshot before, apply
  // `mutate` to a copy, snapshot after, execute.
  auto modifyStudy = [&](uint64_t id, const std::string& desc,
                         const std::function<void(fem::Study&)>& mutate) {
    host.Modify(id, desc, mutate);
  };

  if (m == "set_study") {
    const uint64_t id =
        StudySubject(p.contains("id"), p.value("id", uint64_t{0}),
                     p.contains("study"), p.value("study", uint64_t{0}));
    modifyStudy(id, "Edit study", [&](fem::Study& s) {
      if (p.contains("name"))
        s.name = p.at("name").get<std::string>();
      if (p.contains("mesh_size")) {
        const json& v = p.at("mesh_size");
        if (v.is_string()) {
          s.meshSizeExpr = v.get<std::string>(); // evaluated at mesh time
        } else {
          s.meshSize = v.get<double>();
          s.meshSizeExpr.clear();
        }
      }
      if (p.contains("type") &&
          !fem::AnalysisTypeFromString(p.at("type").get<std::string>(), s.type))
        throw std::runtime_error(
            "analysis type must be static, modal, or topology");
      if (s.type == fem::AnalysisType::Topology && host.IsAssembly())
        topoOnly();
      // Assembly studies (S1): who takes part, and how. The whole list at
      // once — a role table is one statement, not a sequence of edits.
      if (p.contains("participants")) {
        const json& list = p.at("participants");
        if (!list.is_array())
          throw std::runtime_error(
              "participants is a list of {occurrence, role: "
              "meshed|bonded|rigid|ignored, mesh_size?}");
        s.participants.clear();
        for (const json& pj : list) {
          if (!pj.is_object() || !pj.contains("occurrence"))
            throw std::runtime_error(
                "each participant is {occurrence, role, mesh_size?}");
          fem::Participant pt;
          pt.occurrenceId =
              host.OccurrenceOf(pj.at("occurrence"), "participant").id;
          const std::string role = pj.value("role", std::string{"rigid"});
          if (!fem::ParticipantRoleFromString(role, pt.role))
            throw std::runtime_error(
                "participant role '" + role +
                "' is not one of meshed, bonded, rigid, ignored");
          if (pj.contains("mesh_size")) {
            const json& v = pj.at("mesh_size");
            if (v.is_string())
              pt.meshSizeExpr = v.get<std::string>();
            else
              pt.meshSize = v.get<double>();
          }
          for (const fem::Participant& have : s.participants)
            if (have.occurrenceId == pt.occurrenceId)
              throw std::runtime_error("participant " +
                                       host.OccurrenceName(pt.occurrenceId) +
                                       " is listed twice");
          s.participants.push_back(std::move(pt));
        }
      }
      if (p.contains("num_modes"))
        s.numModes = p.at("num_modes").get<int>();
      if (p.contains("gravity")) {
        const auto& g = p.at("gravity");
        s.gravity = glm::dvec3(g.at(0).get<double>(), g.at(1).get<double>(),
                               g.at(2).get<double>());
      }
      // Topology parameters (expression-or-literal, like mesh size).
      if (p.contains("volume_fraction")) {
        const json& v = p.at("volume_fraction");
        if (v.is_string())
          s.volumeFractionExpr = v.get<std::string>();
        else {
          s.volumeFraction = v.get<double>();
          s.volumeFractionExpr.clear();
        }
      }
      if (p.contains("filter_radius")) {
        const json& v = p.at("filter_radius");
        if (v.is_string())
          s.filterRadiusExpr = v.get<std::string>();
        else {
          s.filterRadius = v.get<double>();
          s.filterRadiusExpr.clear();
        }
      }
      if (p.contains("max_iterations"))
        s.maxIterations = p.at("max_iterations").get<int>();
      if (p.contains("load_cases")) {
        const json& list = p.at("load_cases");
        if (!list.is_array())
          throw std::runtime_error(
              "load_cases is a list of {name, weight?} -- the whole list");
        std::vector<fem::LoadCase> cases;
        for (const json& cj : list) {
          if (!cj.is_object() || !cj.contains("name") ||
              !cj.at("name").is_string())
            throw std::runtime_error("each load case is {name, weight?}");
          fem::LoadCase c;
          c.name = cj.at("name").get<std::string>();
          if (c.name.empty())
            throw std::runtime_error("a load case needs a name");
          for (const fem::LoadCase& have : cases)
            if (have.name == c.name)
              throw std::runtime_error("load case '" + c.name +
                                       "' is listed twice");
          if (cj.contains("weight")) {
            const json& w = cj.at("weight");
            if (w.is_string())
              c.weightExpr = w.get<std::string>();
            else
              c.weight = w.get<double>();
          }
          if (c.weightExpr.empty() && !(c.weight > 0.0))
            throw std::runtime_error("load case '" + c.name +
                                     "': weight must be positive");
          cases.push_back(std::move(c));
        }
        if (!cases.empty() && host.IsAssembly())
          throw std::runtime_error(
              "load cases are a part study's (static or topology)");
        if (!cases.empty() && s.type == fem::AnalysisType::Modal)
          throw std::runtime_error("a modal study has no loads to put in "
                                   "load cases");
        // A load may not be left naming a case that is gone.
        for (const fem::Load& l : s.loads) {
          if (l.loadCase.empty())
            continue;
          bool found = false;
          for (const fem::LoadCase& c : cases)
            found = found || c.name == l.loadCase;
          if (!found)
            throw std::runtime_error(
                "load " + std::to_string(l.id) + " is in load case '" +
                l.loadCase +
                "', which the new list drops: remove or re-add that load "
                "first");
        }
        s.loadCases = std::move(cases);
      }
      if (p.contains("shape_from")) {
        const json& v = p.at("shape_from");
        if (v.is_null() || (v.is_number() && v.get<double>() == 0)) {
          s.shapeFrom = fem::ShapeSource{};
        } else {
          if (!v.is_object() || !v.contains("study"))
            throw std::runtime_error(
                "shape_from is {study, threshold?} or {study, whole: true}, "
                "or null to mesh the body again");
          if (host.IsAssembly())
            throw std::runtime_error(
                "shape_from is a part study's: a topology result is a part's");
          if (s.type != fem::AnalysisType::Static)
            throw std::runtime_error(
                "only a static study takes its shape from a topology result");
          fem::ShapeSource src;
          src.study = v.at("study").get<uint64_t>();
          const fem::Study* topo = doc->FindStudy(src.study);
          if (topo == nullptr || topo->type != fem::AnalysisType::Topology)
            throw std::runtime_error("shape_from: study " +
                                     std::to_string(src.study) +
                                     " is not a topology study of this part");
          src.whole = v.value("whole", false);
          src.threshold = v.value("threshold", 0.0);
          if (src.threshold < 0.0 || src.threshold >= 1.0)
            throw std::runtime_error(
                "shape_from threshold must be in [0, 1) (0 = the "
                "volume-matched level)");
          if (src.whole && src.threshold != 0.0)
            throw std::runtime_error(
                "shape_from: whole takes every tet, so it takes no threshold");
          s.shapeFrom = src;
        }
      }
    });
    return json::object();
  }
  if (m == "add_keep_face" || m == "remove_keep_face") {
    // Topology keep-faces: frozen solid so bolt bosses / bearing seats survive
    // the optimization. params: {study, face:{feature_id, element}}
    const uint64_t id = p.at("study").get<uint64_t>();
    const ElementRef face = studyFaceOf(p.at("face"));
    if (m == "add_keep_face") {
      modifyStudy(id, "Add keep face",
                  [&](fem::Study& s) { s.keepFaces.push_back(face); });
    } else {
      modifyStudy(id, "Remove keep face", [&](fem::Study& s) {
        s.keepFaces.erase(std::remove_if(s.keepFaces.begin(), s.keepFaces.end(),
                                         [&](const ElementRef& f) {
                                           return f.featureId ==
                                                      face.featureId &&
                                                  f.name == face.name;
                                         }),
                          s.keepFaces.end());
      });
    }
    return json::object();
  }
  // A topology study's keep-outs (docs/topopt/README.md#keep-outs).
  const auto keepOutStudy = [&](uint64_t id) {
    topoOnly();
    if (host.Require(id).type != fem::AnalysisType::Topology)
      throw std::runtime_error("keep-outs belong to a topology study");
  };
  if (m == "remove_keep_out") {
    const uint64_t id = p.at("study").get<uint64_t>();
    keepOutStudy(id);
    const int index = p.at("index").get<int>();
    modifyStudy(id, "Remove keep-out", [&](fem::Study& s) {
      if (index < 0 || index >= static_cast<int>(s.keepOuts.size()))
        throw std::runtime_error("keep-out index " + std::to_string(index) +
                                 " is out of range; the study has " +
                                 std::to_string(s.keepOuts.size()));
      s.keepOuts.erase(s.keepOuts.begin() + index);
    });
    return json::object();
  }
  if (m == "add_keep_out") {
    const uint64_t id = p.at("study").get<uint64_t>();
    keepOutStudy(id);
    fem::KeepOut k;
    if (p.contains("body")) {
      if (p.contains("box_min") || p.contains("box_max"))
        throw std::runtime_error(
            "a keep-out is a body OR a box (box_min, box_max), not both");
      k.body = p.at("body").get<std::string>();
      std::string names;
      bool found = false;
      for (const Body& b : doc->GetBodies()) {
        names += (names.empty() ? "'" : ", '") + b.name + "'";
        found = found || b.name == k.body;
      }
      if (!found)
        throw std::runtime_error(
            "add_keep_out: the part has no body '" + k.body + "' (it has " +
            names +
            "); a neighbour's geometry arrives as a Derived reference body");
    } else if (p.contains("box_min") && p.contains("box_max")) {
      const auto vec = [&](const char* key) {
        const json& v = p.at(key);
        if (!v.is_array() || v.size() != 3)
          throw std::runtime_error(std::string(key) +
                                   " is a point [x, y, z] in millimetres");
        return glm::dvec3(v.at(0).get<double>(), v.at(1).get<double>(),
                          v.at(2).get<double>());
      };
      k.boxMin = vec("box_min");
      k.boxMax = vec("box_max");
      if (!(k.boxMax.x > k.boxMin.x && k.boxMax.y > k.boxMin.y &&
            k.boxMax.z > k.boxMin.z))
        throw std::runtime_error(
            "add_keep_out: box_max must exceed box_min on every axis");
    } else {
      throw std::runtime_error(
          "add_keep_out takes {study, body} or {study, box_min, box_max}");
    }
    size_t index = 0;
    modifyStudy(id, "Add keep-out", [&](fem::Study& s) {
      index = s.keepOuts.size();
      s.keepOuts.push_back(k);
    });
    return json{{"index", index}};
  }
  if (m == "check_topology") {
    return CheckTopology(state, host, doc, p);
  }
  if (m == "add_refinement") {
    const uint64_t id = p.at("study").get<uint64_t>();
    fem::Refinement r;
    r.face = studyFaceOf(p.at("face"));
    if (p.contains("occurrence"))
      r.occurrenceId = host.OccurrenceOf(p.at("occurrence"), "refinement").id;
    else if (host.IsAssembly())
      throw std::runtime_error(
          "a refinement on an assembly study says whose face: pass "
          "occurrence (id or name)");
    const json& v = p.at("size");
    if (v.is_string())
      r.sizeExpr = v.get<std::string>();
    else
      r.size = v.get<double>();
    modifyStudy(id, "Add refinement",
                [&](fem::Study& s) { s.refinements.push_back(r); });
    return json::object();
  }
  if (m == "remove_refinement") {
    const uint64_t id = p.at("study").get<uint64_t>();
    const ElementRef face = studyFaceOf(p.at("face"));
    modifyStudy(id, "Remove refinement", [&](fem::Study& s) {
      s.refinements.erase(
          std::remove_if(s.refinements.begin(), s.refinements.end(),
                         [&](const fem::Refinement& r) {
                           return r.face.featureId == face.featureId &&
                                  r.face.name == face.name;
                         }),
          s.refinements.end());
    });
    return json::object();
  }
  if (m == "add_load") {
    const uint64_t id = p.at("study").get<uint64_t>();
    fem::Load l;
    l.id = host.AllocateId();
    if (!fem::LoadTypeFromString(p.at("type").get<std::string>(), l.type))
      throw std::runtime_error("load type must be fixed, force, or pressure");
    // An assembly study's anchor (S1): a face on a named occurrence, or —
    // for a fixed support — the CONTACT with a rigid occurrence, which has
    // no face to name because the patch exists in neither part.
    const Document* faceDoc = doc;
    if (p.contains("contact_with")) {
      if (l.type != fem::LoadType::Fixed)
        throw std::runtime_error(
            "only a fixed support anchors on a contact; a force or pressure "
            "names a face (occurrence + face)");
      l.contactWith =
          host.OccurrenceOf(p.at("contact_with"), "contact_with").id;
      const fem::Study& study = host.Require(id);
      const fem::Participant* pt = study.FindParticipant(l.contactWith);
      if (!pt || pt->role != fem::ParticipantRole::Rigid)
        throw std::runtime_error(
            "contact_with " + host.OccurrenceName(l.contactWith) +
            ": not a rigid participant of this study (set_study "
            "participants: [{occurrence, role: rigid}, ...] first)");
    } else {
      l.face = studyFaceOf(p.at("face"));
      if (p.contains("occurrence")) {
        const Occurrence& occ =
            host.OccurrenceOf(p.at("occurrence"), "occurrence");
        l.occurrenceId = occ.id;
        faceDoc = ui::OccurrencePartDoc(state, occ);
        if (!faceDoc)
          throw std::runtime_error("occurrence " + occ.name +
                                   " refers to no part in this project");
        fem::ResolvedFace rf;
        std::string why;
        if (!fem::ResolveFace(*faceDoc, l.face, "load", rf, &why))
          throw std::runtime_error(why + " (on " + occ.name + ")");
      } else if (host.IsAssembly()) {
        throw std::runtime_error(
            "a load on an assembly study says whose face: pass occurrence "
            "(id or name), or contact_with for a fixed support at a contact");
      }
    }
    if (p.contains("magnitude")) {
      const json& v = p.at("magnitude");
      if (v.is_string())
        l.magnitudeExpr = v.get<std::string>();
      else
        l.magnitude = v.get<double>();
    }
    if (p.contains("direction")) {
      const auto& d = p.at("direction");
      l.direction = glm::dvec3(d.at(0).get<double>(), d.at(1).get<double>(),
                               d.at(2).get<double>());
    } else if (l.type == fem::LoadType::Force) {
      // No direction given: push into the face, exactly as a GUI placement
      // does. The alternative was the struct's literal, which is a world
      // direction that knows nothing about the face it was applied to.
      if (faceDoc)
        if (const auto into = fem::DefaultForceDirection(*faceDoc, l.face)) {
          l.direction = *into;
          // A part's inward normal, turned into the world the placed part
          // is solved in.
          if (l.occurrenceId != 0)
            if (const Occurrence* occ = host.asmDoc->FindById(l.occurrenceId))
              l.direction = glm::mat3_cast(occ->orientation) * l.direction;
        }
    }
    if (p.contains("case")) {
      if (l.type == fem::LoadType::Fixed)
        throw std::runtime_error(
            "a support holds in every load case, so it names none; `case` "
            "is for a force or a pressure");
      l.loadCase = p.at("case").get<std::string>();
      const fem::Study& st = host.Require(id);
      if (st.FindLoadCase(l.loadCase) < 0) {
        std::vector<std::string> names;
        for (const fem::LoadCase& c : st.loadCases)
          names.push_back(c.name);
        CaseIndex(names, p.at("case")); // refuses, naming the cases
      }
    }
    modifyStudy(id, "Add load", [&](fem::Study& s) { s.loads.push_back(l); });
    return json{{"id", l.id}};
  }
  if (m == "remove_load") {
    // The load is what this acts on, so it is `id`; the study is context and
    // stays `study`. `load` remains accepted. The row said "by index" and
    // the code has always compared against `l.id` — which happen to differ
    // the moment you remove one.
    const uint64_t id = p.at("study").get<uint64_t>();
    const uint64_t loadId = p.contains("id") ? p.at("id").get<uint64_t>()
                                             : p.at("load").get<uint64_t>();
    modifyStudy(id, "Remove load", [&](fem::Study& s) {
      s.loads.erase(
          std::remove_if(s.loads.begin(), s.loads.end(),
                         [&](const fem::Load& l) { return l.id == loadId; }),
          s.loads.end());
    });
    return json::object();
  }
  if (m == "mesh_study") {
    // Mesh (no solve) every body the study touches; a headless milestone
    // before the solver exists. params: {study, async?}
    const uint64_t id =
        StudySubject(p.contains("id"), p.value("id", uint64_t{0}),
                     p.contains("study"), p.value("study", uint64_t{0}));
    fem::Study* s = &host.Require(id);
    if (p.value("async", false)) {
      // Off the frame (docs/ui/long-operations.md#fem-meshing-and-solving): the
      // plan is made here, the mesh on a worker; study_status says `meshing:
      // true` until it lands and then carries the same `bodies` this returns.
      if (host.IsAssembly())
        throw std::runtime_error(
            "an assembly study meshes synchronously (run_study async: true "
            "meshes and solves off the frame)");
      std::string err;
      ui::JobStart start;
      start.owner = p.value("outlive_client", false) ? 0 : state.rpcConnection;
      if (!ui::StartMeshRun(state, *doc, id, &err, &start))
        throw std::runtime_error(err);
      return json{{"started", true},
                  {"running", true},
                  {"job", start.jobId},
                  {"kind", "mesh"},
                  {"outlives_client", start.owner == 0}};
    }
    std::vector<fem::StudyBodyMesh> meshes;
    std::string err;
    const bool meshed =
        host.IsAssembly()
            ? ui::MeshAssemblyStudy(state, *host.asmDoc, id, meshes, &err)
            : fem::MeshStudy(*doc, *s, meshes, &err);
    if (!meshed)
      throw std::runtime_error(err);
    json bodies = json::array();
    for (const fem::StudyBodyMesh& bm : meshes) {
      bodies.push_back({{"body", bm.bodyName},
                        {"nodes", bm.mesh.stats.nodeCount},
                        {"elements", bm.mesh.stats.tetCount},
                        {"worst_quality", bm.mesh.stats.worstQuality}});
    }
    return json{{"bodies", bodies}};
  }
  if (m == "run_study" || m == "study_results" || m == "study_status" ||
      m == "cancel_study") {
    // run_study: mesh + solve now (explicit act, never automatic) and cache the
    // results — a topology study runs ASYNC (a worker thread) and returns
    // immediately, the others are synchronous. study_status/cancel_study drive
    // the async run. study_results: read the cached results (never solves).
    // params: {study}
    // Catch a finished async run first, so its result is stored before we
    // read (the registry stores it into the part it ran on, whichever tab
    // is active).
    ui::ReapBackgroundJobs(state);
    const uint64_t id =
        StudySubject(p.contains("id"), p.value("id", uint64_t{0}),
                     p.contains("study"), p.value("study", uint64_t{0}));
    fem::Study* s = &host.Require(id);

    // The job running this study, of whichever FEM kind: one at a time each.
    const auto studyJob = [&]() -> ui::BackgroundJob* {
      for (const char* kind : {"topo", "study", "mesh"})
        if (ui::BackgroundJob* j = ui::FindBackgroundJob(state, kind))
          if (j->studyId == id)
            return j;
      return nullptr;
    };
    if (m == "cancel_study") {
      // A static or modal job stops at its next boundary and drops its
      // answer; a topology job keeps the best shape so far.
      if (ui::BackgroundJob* j = studyJob()) {
        const uint64_t jobId = j->id;
        ui::CancelBackgroundJobById(state, jobId, "cancel_study");
        return json{{"cancelled", true}, {"job", jobId}};
      }
      return json{{"cancelled", false}};
    }
    if (m == "study_status") {
      const ui::BackgroundJob* job = studyJob();
      const uint64_t jobId = job ? job->id : 0;
      const ui::MeshRunStatus mrs = ui::GetMeshRunStatus(state, id);
      if (mrs.running)
        return json{{"running", true},
                    {"job", jobId},
                    {"meshing", true},
                    {"elapsed_seconds", mrs.elapsedSeconds}};
      const ui::StudyRunStatus srs = ui::GetStudyRunStatus(state, id);
      if (srs.running)
        return json{{"running", true},
                    {"job", jobId},
                    {"solver", srs.solver},
                    {"elapsed_seconds", srs.elapsedSeconds}};
      const ui::TopoRunStatus st = ui::GetTopologyStatus(state, id);
      if (st.running)
        return json{{"running", true},
                    {"job", jobId},
                    {"iteration", st.iteration},
                    {"max_iterations", st.maxIterations},
                    {"compliance", st.compliance},
                    {"max_change", st.maxChange},
                    {"volume_fraction", st.volumeFraction},
                    {"solver", st.solver},
                    {"elapsed_seconds", st.elapsedSeconds}};
      // Not running — report the cached result's state (if any), and what
      // the last mesh_study {async} found.
      const fem::StudyRuntime* rt = host.Results(id);
      json out{{"running", false},
               {"has_results", rt != nullptr},
               {"converged", rt && rt->converged},
               {"cancelled", rt && rt->cancelled},
               {"iterations", rt ? rt->iterations : 0},
               {"solver", rt ? rt->solver : ""},
               {"solve_seconds", rt ? rt->solveSeconds : 0.0},
               {"stale", host.Stale(id)}};
      if (rt && rt->inputHash != 0)
        out["input_hash"] = inputhash::Hex(rt->inputHash);
      if (!s->error.empty())
        out["error"] = s->error;
      if (const ui::MeshReport* mr =
              doc ? ui::MeshReportFor(state, *doc, id) : nullptr) {
        json bodies = json::array();
        for (const ui::MeshReport::Body& b : mr->bodies)
          bodies.push_back({{"body", b.name},
                            {"nodes", b.nodes},
                            {"elements", b.elements},
                            {"worst_quality", b.worstQuality}});
        json mesh{{"bodies", bodies}, {"seconds", mr->seconds}};
        if (!mr->error.empty())
          mesh["error"] = mr->error;
        out["mesh"] = mesh;
      }
      return out;
    }

    // A run answered from a stored result for the same inputs
    // (docs/simulation/README.md#studies-as-jobs) says so in its reply.
    bool reused = false;
    if (m == "run_study") {
      ui::JobStart start;
      start.recompute = p.value("recompute", false);
      start.owner = p.value("outlive_client", false) ? 0 : state.rpcConnection;
      const auto started = [&](const char* kind) {
        return json{{"started", true},
                    {"running", true},
                    {"job", start.jobId},
                    {"kind", kind},
                    {"outlives_client", start.owner == 0}};
      };
      if (s->type == fem::AnalysisType::Topology) {
        std::string err;
        topoOnly();
        if (!ui::StartTopologyRun(state, *doc, id, &err, &start))
          throw std::runtime_error(err);
        if (!start.reused)
          return started("topo");
      } else if (p.value("async", false)) {
        // Static/modal: async on request (a big mesh can outlive a remote
        // client's socket timeout when solved synchronously).
        std::string err;
        const bool ok = host.IsAssembly()
                            ? ui::StartAssemblyStudyRun(state, *host.asmDoc, id,
                                                        &err, &start)
                            : ui::StartStudyRun(state, *doc, id, &err, &start);
        if (!ok)
          throw std::runtime_error(err);
        if (!start.reused)
          return started("study");
      } else if (!ui::ReuseStudyResult(
                     state, doc, host.IsAssembly() ? host.asmDoc : nullptr, id,
                     &start)) {
        std::string err;
        const bool ran =
            host.IsAssembly()
                ? ui::RunAssemblyStudyAndCache(state, *host.asmDoc, id, &err)
                : ui::RunStudyAndCache(*doc, id, &err);
        if (!ran)
          throw std::runtime_error(err);
        if (const fem::StudyRuntime* done = host.Results(id))
          ui::RememberStudyResult(state, *done);
      }
      reused = start.reused;
    }

    const fem::StudyRuntime* rt = host.Results(id);
    if (!rt)
      throw std::runtime_error("study has not been run");
    // What every answer below carries: the inputs it is for, and on a run
    // whether it was computed or reused.
    const auto stamp = [&](json j) {
      if (rt->inputHash != 0)
        j["input_hash"] = inputhash::Hex(rt->inputHash);
      if (m == "run_study") {
        j["reused"] = reused;
        if (reused)
          j["started"] = false;
      }
      return j;
    };

    // Topology studies report the optimization summary (the density field is
    // read via set_result_display / export, not echoed here).
    if (s->type == fem::AnalysisType::Topology) {
      std::size_t nodes = 0, elements = 0;
      for (const fem::StudyBodyResult& b : rt->bodies) {
        nodes += b.mesh.stats.nodeCount;
        elements += b.mesh.stats.tetCount;
      }
      json out{{"type", "topology"},
               {"nodes", nodes},
               {"elements", elements},
               {"iterations", rt->iterations},
               {"converged", rt->converged},
               {"cancelled", rt->cancelled},
               {"volume_fraction", rt->volumeFractionAchieved},
               {"compliance", rt->complianceHistory.empty()
                                  ? 0.0
                                  : rt->complianceHistory.back()},
               {"solver", rt->solver},
               {"solve_seconds", rt->solveSeconds},
               {"stale", host.Stale(id)}};
      // Each case's own compliance; `compliance` is their weighted sum.
      if (!rt->caseNames.empty()) {
        json cases = json::array();
        for (size_t k = 0; k < rt->caseNames.size(); ++k)
          cases.push_back(
              {{"name", rt->caseNames[k]},
               {"weight",
                k < rt->caseWeights.size() ? rt->caseWeights[k] : 1.0},
               {"compliance",
                k < rt->caseCompliance.size() ? rt->caseCompliance[k] : 0.0}});
        out["cases"] = std::move(cases);
      }
      json kos = json::array();
      for (const fem::KeepOutReport& k : rt->keepOuts)
        kos.push_back(
            {{"what", k.what}, {"tets", k.tets}, {"volume_mm3", k.volume}});
      out["keep_outs"] = std::move(kos);
      // What the connectivity filter removed, and the keep faces it held:
      // a keep face nothing connects is named, not silently printed as a
      // floating ring.
      out["islands"] = {{"threshold", rt->islands.threshold},
                        {"components", rt->islands.components},
                        {"tets", rt->islands.tets},
                        {"volume_mm3", rt->islands.volume},
                        {"keep_faces", rt->islands.keepFaces},
                        {"load_path_broken", rt->islands.loadPathBroken}};
      return stamp(out);
    }

    // An assembly study's answer carries its role table (S1): the reader
    // must know which bound each neighbour holds — absent is a lower bound
    // on stiffness, rigid an upper one.
    json roles = json::array();
    for (const fem::Participant& pt : s->participants)
      roles.push_back({{"occurrence", pt.occurrenceId},
                       {"name", host.OccurrenceName(pt.occurrenceId)},
                       {"role", fem::ParticipantRoleToString(pt.role)}});

    // Modal studies report per-body natural frequencies instead of stress.
    if (s->type == fem::AnalysisType::Modal) {
      std::size_t nodes = 0, elements = 0;
      json bodies = json::array();
      for (const fem::StudyBodyResult& b : rt->bodies) {
        nodes += b.mesh.stats.nodeCount;
        elements += b.mesh.stats.tetCount;
        json freqs = json::array();
        for (const auto& mode : b.results.modes)
          freqs.push_back(mode.frequencyHz);
        bodies.push_back({{"body", b.bodyName}, {"frequencies_hz", freqs}});
      }
      json out{{"type", "modal"},
               {"nodes", nodes},
               {"elements", elements},
               {"bodies", bodies},
               {"stale", host.Stale(id)}};
      if (!roles.empty())
        out["participants"] = roles;
      return stamp(out);
    }

    // Aggregate mesh size and a factor of safety (min over bodies with yield
    // data) from the governing von Mises.
    std::size_t nodes = 0, elements = 0;
    double minFoS = 0.0;
    bool haveFoS = false;
    json perBody = json::array();
    for (const fem::StudyBodyResult& b : rt->bodies) {
      nodes += b.mesh.stats.nodeCount;
      elements += b.mesh.stats.tetCount;
      json bj = {{"body", b.bodyName},
                 {"max_von_mises_mpa", b.results.maxVonMises},
                 {"max_displacement_mm", b.results.maxDisplacement}};
      // Factor of safety against yield, when the body's material defines one.
      if (b.results.maxVonMises > 0) {
        const auto mt = CADMaterials::MaterialLibrary::MaterialTypeFromString(
            host.BodyMaterial(*s, b.bodyIndex));
        const double yield =
            CADMaterials::MaterialLibrary::GetMaterial(mt).yieldMPa;
        if (yield > 0) {
          const double fos = yield / b.results.maxVonMises;
          bj["factor_of_safety"] = fos;
          if (!haveFoS || fos < minFoS)
            minFoS = fos;
          haveFoS = true;
        }
      }
      perBody.push_back(std::move(bj));
    }

    json result = {
        {"nodes", nodes},
        {"elements", elements},
        {"max_von_mises_mpa",
         {{"value", rt->maxVonMises},
          {"at", {rt->maxVmAt.x, rt->maxVmAt.y, rt->maxVmAt.z}},
          {"body", rt->maxVmBody}}},
        {"max_displacement_mm",
         {{"value", rt->maxDisplacement},
          {"at", {rt->maxDispAt.x, rt->maxDispAt.y, rt->maxDispAt.z}},
          {"body", rt->maxDispBody}}},
        {"bodies", perBody},
        {"stale", host.Stale(id)}};
    if (haveFoS)
      result["factor_of_safety"] = minFoS;
    if (!roles.empty())
      result["participants"] = roles;
    // Load cases: each one's maxima (the top level is the worst of them).
    if (!rt->caseNames.empty()) {
      json cases = json::array();
      for (size_t k = 0; k < rt->caseNames.size(); ++k) {
        double vm = 0.0, disp = 0.0;
        glm::dvec3 vmAt(0.0), dispAt(0.0);
        double fos = 0.0;
        bool caseFoS = false;
        for (const fem::StudyBodyResult& b : rt->bodies) {
          const fem::FemResults& r = b.Case(static_cast<int>(k));
          if (r.maxVonMises > vm) {
            vm = r.maxVonMises;
            vmAt = r.maxVmAt;
          }
          if (r.maxDisplacement > disp) {
            disp = r.maxDisplacement;
            dispAt = r.maxDispAt;
          }
          const auto mt = CADMaterials::MaterialLibrary::MaterialTypeFromString(
              host.BodyMaterial(*s, b.bodyIndex));
          const double yield =
              CADMaterials::MaterialLibrary::GetMaterial(mt).yieldMPa;
          if (yield > 0 && r.maxVonMises > 0) {
            const double f = yield / r.maxVonMises;
            if (!caseFoS || f < fos)
              fos = f;
            caseFoS = true;
          }
        }
        json cj = {{"name", rt->caseNames[k]},
                   {"max_displacement_mm",
                    {{"value", disp}, {"at", {dispAt.x, dispAt.y, dispAt.z}}}},
                   {"max_von_mises_mpa",
                    {{"value", vm}, {"at", {vmAt.x, vmAt.y, vmAt.z}}}}};
        if (caseFoS)
          cj["factor_of_safety"] = fos;
        cases.push_back(std::move(cj));
      }
      result["cases"] = std::move(cases);
    }
    if (s->shapeFrom.study != 0)
      result["shape"] = {
          {"from", s->shapeFrom.study},
          {"whole", s->shapeFrom.whole},
          {"threshold", rt->shapeThreshold},
          {"volume_mm3", rt->shapeVolume},
          {"dropped_tets", rt->shapeIslandTets},
          {"threshold_lowered_from", rt->shapeThresholdLoweredFrom}};
    {
      // An FEM factor of safety is a PREDICTION (core/Receipt.h). It is a
      // linear-elastic solver's estimate on a tetrahedral approximation of
      // the part, against a library yield number — every one of which is a
      // modelling choice. The figure is quoted downstream as though it were
      // measured, so the basis travels with it and the receipt can never roll
      // up better than Provisional.
      receipt::Receipt r;
      const receipt::Oracle solver{"designer/fem-static",
                                   rt->solver.empty() ? "cpu" : rt->solver};
      const bool stale = host.Stale(id);
      if (stale) {
        // The numbers are real; they are just answers about a part that no
        // longer exists. Today that is a `stale: true` the caller has to know
        // to read, beside a factor of safety they will quote.
        r.Unverifiable("the results describe the current geometry", solver,
                       "the part changed after this study was solved — these "
                       "numbers are the previous geometry's. Re-run the study "
                       "before believing them");
      } else if (rt->topology && !rt->converged) {
        // `converged` is a TOPOLOGY field — FemSolver.h says so ("empty/zero
        // for static & modal runtimes"), and it is written only by
        // MakeTopoRuntime. Reading it on a static study made every solved
        // beam unverifiable, which is a false alarm and costs exactly what a
        // false pass costs. A static solve reports no convergence signal at
        // all, so the receipt does not claim one.
        r.Unverifiable("the optimization converged", solver,
                       "the run stopped at the iteration cap without reaching "
                       "the change tolerance");
      } else if (!haveFoS) {
        r.Unverifiable("factor of safety against yield", solver,
                       "no body in this study has a material with a yield "
                       "strength, so there is nothing to divide by — a stress "
                       "with no yield is not a margin");
      } else {
        receipt::Claim c("factor of safety against yield >= 1", solver,
                         minFoS >= 1.0 ? receipt::Verdict::Pass
                                       : receipt::Verdict::Fail);
        c.basis = receipt::Basis::Predicted;
        c.value = minFoS;
        c.unit = "x";
        c.note = "linear-elastic static solve on " + std::to_string(elements) +
                 " elements, against the library yield strength. A prediction,"
                 " not a measurement";
        r.Add(std::move(c));
      }
      AttachReceipt(result, r);
    }
    return stamp(result);
  }
  if (m == "probe_study") {
    // How one face moved, as a rigid body (fem::FitFaceMotion): what a
    // bearing in a seat, or anything else carried by a face, does under the
    // load. The maxima study_results reports are somewhere on the part; the
    // question a mechanism asks is what its own seats did.
    ui::ReapBackgroundJobs(state);
    const uint64_t id =
        StudySubject(p.contains("id"), p.value("id", uint64_t{0}),
                     p.contains("study"), p.value("study", uint64_t{0}));
    const fem::Study& s = host.Require(id);
    if (host.IsAssembly())
      throw std::runtime_error(
          "probe_study reads a PART study: an assembly study meshes the "
          "imprinted fuse of its participants, whose faces are not the "
          "parts' faces -- study the part, loaded as the assembly loads it");
    if (s.type != fem::AnalysisType::Static)
      throw std::runtime_error(
          std::string("probe_study reads a static study's displacements; "
                      "this study is ") +
          fem::AnalysisTypeToString(s.type));
    const fem::StudyRuntime* rt = host.Results(id);
    if (!rt)
      throw std::runtime_error("study has not been run");
    if (host.Stale(id))
      throw std::runtime_error(
          "the study is stale: the part changed after it was solved, so a "
          "face named now may not be the face that was meshed -- run_study "
          "again first");
    if (!p.contains("face"))
      throw std::runtime_error("probe_study needs `face` (the element, as "
                               "add_load takes it)");
    const ElementRef face = studyFaceOf(p.at("face"));
    fem::ResolvedFace rf;
    std::string why;
    if (!fem::ResolveFace(*doc, face, "probe", rf, &why))
      throw std::runtime_error(why);
    const fem::StudyBodyResult* body = nullptr;
    for (const fem::StudyBodyResult& b : rt->bodies)
      if (b.bodyIndex == rf.bodyIndex)
        body = &b;
    if (!body)
      throw std::runtime_error(
          "face '" + face.name +
          "' is on a body this study did not solve: a part study meshes only "
          "the bodies its loads are on");
    const int caseIdx =
        p.contains("case") ? CaseIndex(rt->caseNames, p.at("case")) : 0;
    const auto fm = fem::FitFaceMotion(
        body->mesh, body->Case(caseIdx).displacement, rf.faceIndex);
    if (!fm)
      throw std::runtime_error("face '" + face.name +
                               "' has no triangles in the study's mesh");
    const auto vec = [](const glm::dvec3& v) {
      return json::array({v.x, v.y, v.z});
    };
    json out{
        {"face", face.name},
        {"body", body->bodyName},
        {"case", rt->caseNames.empty() ? json() : json(rt->caseNames[caseIdx])},
        {"nodes", fm->nodes},
        {"area_mm2", fm->area},
        {"centroid_mm", vec(fm->centroid)},
        {"translation_mm", vec(fm->translation)},
        {"translation_magnitude_mm", glm::length(fm->translation)},
        {"rotation_determined", fm->rotationDetermined},
        {"rms_residual_mm", fm->rmsResidual}};
    if (fm->rotationDetermined) {
      out["rotation_rad"] = vec(fm->rotation);
      out["rotation_magnitude_deg"] = glm::degrees(glm::length(fm->rotation));
    }
    if (p.contains("at")) {
      const json& a = p.at("at");
      if (!a.is_array() || a.size() != 3)
        throw std::runtime_error("`at` is a point [x, y, z] in millimetres");
      const glm::dvec3 at(a.at(0).get<double>(), a.at(1).get<double>(),
                          a.at(2).get<double>());
      if (!fm->rotationDetermined)
        throw std::runtime_error(
            "face '" + face.name +
            "': its nodes lie on a line, so a rotation about that line moves "
            "none of them and the motion cannot be carried to `at`");
      out["at_mm"] = vec(at);
      out["displacement_at_mm"] = vec(fm->At(at));
    }
    return out;
  }
  if (m == "set_topo_ghost") {
    // Toggle the modeling-mode shape-guide ghost for a topology study: a
    // translucent render of its optimized shape, to re-model against.
    // params: {study, on?: true}
    const uint64_t id = p.at("study").get<uint64_t>();
    host.Require(id);
    const bool on = p.value("on", true);
    state.topoGhostStudyId = on ? id : 0;
    state.syncedDocumentVersion = UINT64_MAX; // force a renderer resync
    return json{{"study", id}, {"on", on}};
  }
  if (m == "export_topo_stl") {
    // Export a topology result's optimized shape (iso-surface, Taubin-smoothed)
    // as a binary STL. params: {study, path, threshold?: 0..1 (0=auto),
    // smooth?: iterations (default 3)}
    ui::ReapBackgroundJobs(state);
    const uint64_t id = p.at("study").get<uint64_t>();
    fem::Study* s = &host.Require(id);
    if (s->type != fem::AnalysisType::Topology)
      throw std::runtime_error("not a topology study");
    const fem::StudyRuntime* rt = host.Results(id);
    if (!rt || rt->bodies.empty())
      throw std::runtime_error("study has no result to export");
    const fem::StudyBodyResult& b = rt->bodies.front();
    const double thr =
        p.contains("threshold") && p.at("threshold").get<double>() > 0
            ? p.at("threshold").get<double>()
            : fem::AutoIsoThreshold(b.mesh, b.density,
                                    rt->volumeFractionAchieved);
    const std::string path = p.at("path").get<std::string>();
    std::string err;
    fem::TopoStlStats stats;
    if (!fem::ExportTopoStl(b.mesh, b.density, thr, p.value("smooth", 3), path,
                            &err, &stats))
      throw std::runtime_error(err);
    // The same honesty export_stl gives: a non-watertight mesh still
    // exports, but flag it before anyone slices the file.
    return json{{"path", path},
                {"threshold", thr},
                {"triangles", stats.triangles},
                {"boundary_edges", stats.boundaryEdges},
                {"watertight", stats.watertight}};
  }
  if (m == "topo_to_body") {
    ui::ReapBackgroundJobs(state);
    topoOnly();
    const uint64_t id = p.at("study").get<uint64_t>();
    const fem::Study& s = host.Require(id);
    if (s.type != fem::AnalysisType::Topology)
      throw std::runtime_error("topo_to_body: not a topology study");
    const fem::StudyRuntime* rt = host.Results(id);
    if (!rt || rt->bodies.empty())
      throw std::runtime_error("topo_to_body: the study has no result");
    const fem::StudyBodyResult& b = rt->bodies.front();
    const double thr =
        p.contains("threshold") && p.at("threshold").get<double>() > 0
            ? p.at("threshold").get<double>()
            : fem::AutoIsoThreshold(b.mesh, b.density,
                                    rt->volumeFractionAchieved);
    const std::string path = p.at("path").get<std::string>();
    const std::string name = p.value("name", s.name + " shape");
    RequireFreshTabName(state, name);
    std::string material;
    if (b.bodyIndex >= 0 &&
        b.bodyIndex < static_cast<int>(doc->GetBodies().size()))
      material = doc->GetBodies()[b.bodyIndex].material;
    std::string err;
    fem::TopoStlStats stats;
    if (!fem::ExportTopoStl(b.mesh, b.density, thr, p.value("smooth", 3), path,
                            &err, &stats))
      throw std::runtime_error("topo_to_body: " + err);
    const uint64_t wasActive = state.parts[state.activePart].id;
    const size_t slot = ui::AddPart(state, name);
    const uint64_t partId = state.parts[slot].id;
    if (!ui::ImportAsFeature(state, path, nullptr, &err,
                             units::Length::Millimetre, /*keepOrigin=*/true))
      throw std::runtime_error("topo_to_body: " + err);
    Document* made = state.document.get();
    if (!material.empty() && made->FeatureCount() > 0) {
      Feature* imp = made->GetFeature(made->FeatureCount() - 1);
      const json before = DocumentIO::FeatureToJson(*imp);
      json after = before;
      after["material"] = material;
      state.commands->Execute(std::make_unique<ModifyFeatureCommand>(
                                  made->FeatureCount() - 1, before, after),
                              *made);
    }
    double volume = 0.0;
    bool valid = !made->GetBodies().empty();
    for (const Body& body : made->GetBodies()) {
      volume += body.shape.Volume();
      valid = valid && Measure::CheckGeometry(body.shape).valid;
    }
    for (size_t i = 0; i < state.parts.size(); ++i)
      if (state.parts[i].id == wasActive) {
        ui::SetActivePart(state, i);
        break;
      }
    return json{{"part", name},
                {"part_id", partId},
                {"path", path},
                {"triangles", stats.triangles},
                {"watertight", stats.watertight},
                {"threshold", thr},
                {"volume_mm3", volume},
                {"valid", valid}};
  }
  if (m == "enter_simulation") {
    // Enter the FEM simulation workspace (the GUI mode), optionally selecting a
    // study. The LLM drives the same mode a human enters from the toolbar.
    // params: {study?}
    ui::EnterSimulation(state);
    if (p.contains("study")) {
      const uint64_t id = p.at("study").get<uint64_t>();
      host.Require(id);
      state.simulation.activeStudyId = id;
    }
    return json{{"active", state.simulation.active},
                {"study", state.simulation.activeStudyId}};
  }
  if (m == "exit_simulation") {
    ui::ExitSimulation(state);
    return json::object();
  }
  if (m == "set_result_display") {
    // Show/warp a study's result overlay (display only, like set_explode).
    // params: {study?, field: "von_mises"|"displacement"|"shape"|"density"|
    //          "none", deform_scale?, mode?, threshold?}
    // shape/density are topology views; threshold (0..1, 0=auto) sets the
    // iso-surface level for the "shape" view.
    ui::EnterSimulation(state);
    if (p.contains("study")) {
      const uint64_t id = p.at("study").get<uint64_t>();
      host.Require(id);
      state.simulation.activeStudyId = id;
    }
    // An unknown field is refused, not drawn as von Mises: a misspelled
    // "displacment" would otherwise show a plausible stress plot.
    const std::string f = p.value("field", std::string("von_mises"));
    if (f == "von_mises")
      state.simulation.field = ui::SimulationState::Field::VonMises;
    else if (f == "displacement")
      state.simulation.field = ui::SimulationState::Field::Displacement;
    else if (f == "shape")
      state.simulation.field = ui::SimulationState::Field::Shape;
    else if (f == "density")
      state.simulation.field = ui::SimulationState::Field::Density;
    else if (f == "none")
      state.simulation.field = ui::SimulationState::Field::None;
    else
      throw std::runtime_error("unknown field: " + f +
                               "; fields: von_mises, displacement, shape, "
                               "density, none");
    if (p.contains("deform_scale"))
      state.simulation.deformScale =
          static_cast<float>(p.at("deform_scale").get<double>());
    if (p.contains("mode")) // modal: which mode shape to display (0-based)
      state.simulation.activeMode = p.at("mode").get<int>();
    if (p.contains("case")) {
      const fem::StudyRuntime* rt =
          host.Results(state.simulation.activeStudyId);
      if (!rt)
        throw std::runtime_error("`case`: the study has not been run");
      state.simulation.activeCase = CaseIndex(rt->caseNames, p.at("case"));
    }
    if (p.contains("threshold")) // topology iso-surface level (0..1, 0=auto)
      state.simulation.topoThreshold =
          static_cast<float>(p.at("threshold").get<double>());
    return json{{"field", f},
                {"deform_scale", state.simulation.deformScale},
                {"mode", state.simulation.activeMode},
                {"case", state.simulation.activeCase},
                {"threshold", state.simulation.topoThreshold}};
  }
  return std::nullopt;
}

} // namespace remote
