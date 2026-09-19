#include "remote/RemoteSimulation.h"

#include "cad/CADMaterials.h"
#include "core/Command.h"
#include "core/Document.h"
#include "core/Occurrence.h"
#include "core/fem/FemSolver.h"
#include "core/fem/FemStudy.h"
#include "core/fem/FemTopo.h"
#include "core/io/Serialization.h"
#include "remote/RemoteHelpers.h"
#include "ui/AppState.h"
#include "ui/AssemblyActions.h" // PartDocById, SnapshotAssembly, OccurrenceMatrix
#include "ui/BackgroundJobs.h"
#include "ui/SimulationActions.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
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

// Where the studies live: the active PART, or the active ASSEMBLY (S1,
// docs/simulation/assembly-studies-plan.md). One handler, two homes — the
// study record is the same, and the verbs are the same; what differs is how
// an edit becomes an undo step (a command on a part, a full-state snapshot
// on an assembly) and where a result's staleness is measured.
struct Host {
  ui::AppState& state;
  Document* part = nullptr;
  AssemblyDocument* asmDoc = nullptr;

  bool IsAssembly() const { return asmDoc != nullptr; }
  std::vector<fem::Study>& Studies() {
    return part ? part->GetStudies() : asmDoc->GetStudies();
  }
  fem::Study* Find(uint64_t id) {
    return part ? part->FindStudy(id) : asmDoc->FindStudy(id);
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
  fem::Study& Add(const std::string& name) {
    if (part) {
      auto cmd = std::make_unique<AddStudyCommand>(name);
      AddStudyCommand* raw = cmd.get();
      state.commands->Execute(std::move(cmd), *part);
      return *part->FindStudy(raw->StudyId());
    }
    ui::SnapshotAssembly(state);
    return asmDoc->AddStudy(name);
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
     "}"},
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
     "nothing meshed is refused by name.",
     "filter_radius, gravity, id, max_iterations, mesh_size, name, num_modes, "
     "participants, study, type, volume_fraction"},
    {"add_load",
     "Add a load/fixture to a study {study, type: fixed|force|pressure, face, "
     "magnitude, direction?}. `magnitude` is NEWTONS for a force -- the total "
     "on that face, spread over its area -- and MPa for a pressure, which "
     "pushes along the inward normal. `fixed` ignores it. Every result scales "
     "linearly with this number and a wrong unit is invisible in the answer, "
     "so it is stated here rather than left to be inferred from the solver. "
     "An omitted force `direction` pushes into the picked face (the same "
     "default a GUI placement takes). A `face` is {feature_id, element}, or "
     "just the element name — the body that has it is then found for you.",
     "contact_with, direction, face, magnitude, occurrence, study, type"},
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
    {"remove_keep_face", "Unmark a kept face.", "face, study"},
    {"mesh_study",
     "Mesh the study (Netgen tets); returns element counts/quality. "
     "Synchronous by default — a fine mesh takes minutes, so either give the "
     "client a long timeout (the server finishes even if the client gives "
     "up) or pass `async: true`: the mesh then runs on a background job and "
     "study_status answers {running, meshing: true} until it lands, after "
     "which its `mesh` carries the same bodies/nodes/elements/worst_quality. "
     "Takes `id` (or `study`, the same thing here) — see remove_study.",
     "async, id, study"},
    {"run_study",
     "Solve the study (blocking, or `async` and poll study_status). Async "
     "meshes AND solves off the frame; the result is stored into the part "
     "the study belongs to even if another tab is active when it lands. Takes "
     "`id` (or `study`, the same thing here) — see remove_study.",
     "async, id, study"},
    {"cancel_study",
     "Cancel a running study — takes `id` (or `study`, the same thing here) — "
     "see remove_study.",
     "id, study"},
    {"study_status",
     "Status/progress of a study: {running, meshing?, iteration…} while a "
     "job runs, else the cached result's state plus `mesh` from the last "
     "mesh_study {async}. Takes `id` (or `study`, the same thing here) — see "
     "remove_study.",
     "id, study"},
    {"study_results",
     "Result summary: {type, elements, nodes, stale} plus a `bodies` "
     "array, one entry PER BODY, because a study meshes and solves each body "
     "separately (there is no bonded contact between them). A body carries "
     "max_displacement_mm: {value, at}, max_von_mises_mpa: {value, at} and "
     "factor_of_safety for a static study, `frequencies_hz` for a modal one, "
     "and the optimization summary for a topology one; the static maxima are "
     "also aggregated at the top level. Read a modal result as "
     "`bodies[i].frequencies_hz` — this row used to name the per-body keys "
     "as though they were top-level, which is a KeyError rather than a wrong "
     "answer, but only after the solve has been paid for. Refused with "
     "\"study has not been run\" when there is "
     "nothing to report -- a solved result is DERIVED and no project keeps "
     "one, so a study reopened from disk carries its load cases and no "
     "answers (`has_results`, `stale` in list_studies). Takes `id` (or "
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
     "did nothing.",
     "deform_scale, field, mode, study, threshold"},
    {"set_topo_ghost", "Toggle the topology-optimization ghost overlay.",
     "on, study"},
    {"export_topo_stl", "Export the optimized topology as STL.",
     "path, smooth, study, threshold"},
};
std::span<const MethodDoc> SimulationMethodDocs() { return SimulationMethods; }

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
    fem::Study* s = &host.Add(p.value("name", std::string{}));
    if (hasKind)
      s->type = type;
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
      }
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
        faceDoc = ui::PartDocById(state, occ.partId);
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
              l.direction = glm::dvec3(glm::mat3_cast(occ->orientation) *
                                       glm::vec3(l.direction));
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
      // Off the frame (docs/ui/long-operations.md, phase 3): the plan is
      // made here, the mesh on a worker; study_status says `meshing: true`
      // until it lands and then carries the same `bodies` this returns.
      if (host.IsAssembly())
        throw std::runtime_error(
            "an assembly study meshes synchronously (run_study async: true "
            "meshes and solves off the frame)");
      std::string err;
      if (!ui::StartMeshRun(state, *doc, id, &err))
        throw std::runtime_error(err);
      return json{{"started", true}, {"running", true}, {"job", "mesh"}};
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

    if (m == "cancel_study") {
      if (ui::GetStudyRunStatus(state, id).running)
        throw std::runtime_error(
            "a static/modal solve cannot be cancelled (the linear solver "
            "has no interruption points) — wait for study_status running: "
            "false");
      return json{{"cancelled", ui::CancelTopologyRun(state, id)}};
    }
    if (m == "study_status") {
      const ui::MeshRunStatus mrs = ui::GetMeshRunStatus(state, id);
      if (mrs.running)
        return json{{"running", true},
                    {"meshing", true},
                    {"elapsed_seconds", mrs.elapsedSeconds}};
      const ui::StudyRunStatus srs = ui::GetStudyRunStatus(state, id);
      if (srs.running)
        return json{{"running", true},
                    {"solver", "cpu (Eigen CG-IC)"},
                    {"elapsed_seconds", srs.elapsedSeconds}};
      const ui::TopoRunStatus st = ui::GetTopologyStatus(state, id);
      if (st.running)
        return json{{"running", true},
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

    if (m == "run_study") {
      if (s->type == fem::AnalysisType::Topology) {
        std::string err;
        topoOnly();
        if (!ui::StartTopologyRun(state, *doc, id, &err))
          throw std::runtime_error(err);
        return json{{"started", true}, {"running", true}};
      }
      // Static/modal: async on request (a big mesh can outlive a remote
      // client's socket timeout when solved synchronously).
      if (p.value("async", false)) {
        std::string err;
        const bool started =
            host.IsAssembly()
                ? ui::StartAssemblyStudyRun(state, *host.asmDoc, id, &err)
                : ui::StartStudyRun(state, *doc, id, &err);
        if (!started)
          throw std::runtime_error(err);
        return json{{"started", true}, {"running", true}};
      }
      std::string err;
      const bool ran =
          host.IsAssembly()
              ? ui::RunAssemblyStudyAndCache(state, *host.asmDoc, id, &err)
              : ui::RunStudyAndCache(*doc, id, &err);
      if (!ran)
        throw std::runtime_error(err);
    }

    const fem::StudyRuntime* rt = host.Results(id);
    if (!rt)
      throw std::runtime_error("study has not been run");

    // Topology studies report the optimization summary (the density field is
    // read via set_result_display / export, not echoed here).
    if (s->type == fem::AnalysisType::Topology) {
      std::size_t nodes = 0, elements = 0;
      for (const fem::StudyBodyResult& b : rt->bodies) {
        nodes += b.mesh.stats.nodeCount;
        elements += b.mesh.stats.tetCount;
      }
      return json{{"type", "topology"},
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
      return out;
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
        const float yield =
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
    return result;
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
    const std::string f = p.value("field", std::string("von_mises"));
    state.simulation.field =
        f == "displacement" ? ui::SimulationState::Field::Displacement
        : f == "shape"      ? ui::SimulationState::Field::Shape
        : f == "density"    ? ui::SimulationState::Field::Density
        : f == "none"       ? ui::SimulationState::Field::None
                            : ui::SimulationState::Field::VonMises;
    if (p.contains("deform_scale"))
      state.simulation.deformScale =
          static_cast<float>(p.at("deform_scale").get<double>());
    if (p.contains("mode")) // modal: which mode shape to display (0-based)
      state.simulation.activeMode = p.at("mode").get<int>();
    if (p.contains("threshold")) // topology iso-surface level (0..1, 0=auto)
      state.simulation.topoThreshold =
          static_cast<float>(p.at("threshold").get<double>());
    return json{{"field", f},
                {"deform_scale", state.simulation.deformScale},
                {"mode", state.simulation.activeMode},
                {"threshold", state.simulation.topoThreshold}};
  }
  return std::nullopt;
}

} // namespace remote
