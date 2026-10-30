// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "remote/RemoteSimulation.h"

#include "cad/CADMaterials.h"
#include "core/Command.h"
#include "core/Document.h"
#include "core/InputHash.h"
#include "core/Measure.h"
#include "core/Occurrence.h"
#include "core/Units.h"
#include "core/fem/Calculix.h"
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

#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <Bnd_Box.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Pnt.hxx>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using nlohmann::json;

namespace remote {

namespace {

// A study whose loads make a stress to settle and whose supports may be
// pins: a static one, and a buckling one, whose base state is a static
// solve (docs/simulation/README.md#buckling).
bool SettlesStress(fem::AnalysisType t) {
  return t == fem::AnalysisType::Static || t == fem::AnalysisType::Buckling;
}
// A study whose run refines round a peak until it settles: a stress's, or
// a thermal study's temperature rise.
bool Converges(fem::AnalysisType t) {
  return SettlesStress(t) || t == fem::AnalysisType::Thermal;
}

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
         kind == "mesh" || kind == "check" || kind == "requirements" ||
         kind == "design_sweep";
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
    const int k = IntParam(v, "case");
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

// A printed part's build direction, `layer_normal` [x, y, z] in the part's
// frame (docs/simulation/README.md#printed-layers), as a unit vector.
// Refused unless it is three finite numbers that are not all zero.
glm::dvec3 LayerNormalOf(const json& v) {
  const char* what = "`layer_normal` is the build direction [x, y, z] in the "
                     "part's frame: the normal of the printed layers, which "
                     "need not be unit but cannot be zero";
  if (!v.is_array() || v.size() != 3)
    throw std::runtime_error(what);
  glm::dvec3 n(0.0);
  for (int k = 0; k < 3; ++k) {
    if (!v.at(k).is_number())
      throw std::runtime_error(what);
    n[k] = v.at(k).get<double>();
  }
  const double len = glm::length(n);
  if (!std::isfinite(len) || len <= 1e-12)
    throw std::runtime_error(what);
  return n / len;
}

// A static study's `converge` (docs/simulation/README.md
// #a-peak-that-has-settled): one of the three, or refused naming them.
// One body's nonlinear run, as study_results reports it.
json NonlinearBodyJson(const fem::StudyBodyResult::Nonlinear& n,
                       double yieldMPa) {
  json curve = json::array();
  for (const auto& [f, u] : n.curve)
    curve.push_back({{"factor", f}, {"max_displacement_mm", u}});
  json out = {{"factor", n.factor},
              {"increments", n.increments},
              {"ccx_runs", n.ccxRuns},
              {"curve", std::move(curve)},
              {"equilibrium",
               {{"residual_criterion", n.residualCriterion},
                {"imbalance", n.imbalance},
                {"tolerance", fem::kNonlinearBalance}}}};
  if (n.firstYieldFactor > 0)
    out["first_yield_factor"] = n.firstYieldFactor;
  else if (yieldMPa > 0)
    out["first_yield_factor"] = nullptr;
  // A contact run (docs/simulation/README.md#contact).
  out["large_deformation"] = n.largeDeformation;
  if (n.stabilization > 0)
    out["stabilization"] = n.stabilization;
  // A plastic run (docs/simulation/README.md#plasticity).
  if (n.plasticity) {
    json p = {{"unloaded", n.unloaded},
              {"strain_limit", n.strainLimit},
              {"collapsed", n.collapsed}};
    if (n.collapsed)
      p["collapse_bracket"] = {n.factor, n.collapseHigh};
    out["plasticity"] = std::move(p);
  }
  if (n.snapped)
    out["snapped"] = {{"from_factor", n.snappedFrom},
                      {"to_factor", n.snappedTo}};
  if (n.snapThrough) {
    out["max_factor"] = n.maxFactor;
    out["linear_factor"] = n.linearFactor;
    out["imperfection_mm"] = n.imperfectionMm;
    out["imperfection"] = n.imperfectionGiven
                              ? "stated"
                              : "derived: a tenth of the wall the first mode "
                                "bends (a thousandth of the part's size where "
                                "no wall is measured)";
    out["deflection_limit_mm"] = n.deflectionLimitMm;
    out["peak_at"] = n.peakAt;
    if (n.peakAt == "none") {
      out["peak_factor"] = nullptr;
      out["why_none"] = n.none;
    } else {
      out["peak_factor"] = n.peakFactor;
      out["peak_bracket"] = {n.peakLow, n.peakHigh};
    }
  }
  return out;
}

// set_study's `nonlinear` (docs/simulation/README.md#nonlinear-static):
// null or false is linear; an object states the whole of it.
fem::Nonlinear NonlinearOf(const json& v) {
  fem::Nonlinear nl;
  if (v.is_null() || (v.is_boolean() && !v.get<bool>()))
    return nl;
  if (!v.is_object())
    throw std::runtime_error(
        "nonlinear is {large_deformation?, plasticity?, increments?, "
        "snap_through?, imperfection_mm?, deflection_limit_mm?}, or null for "
        "a linear study");
  for (const auto& [k, _] : v.items())
    if (k != "large_deformation" && k != "plasticity" && k != "increments" &&
        k != "snap_through" && k != "imperfection_mm" &&
        k != "deflection_limit_mm")
      throw std::runtime_error("nonlinear has no key '" + k +
                               "' (it takes large_deformation, plasticity, "
                               "increments, snap_through, imperfection_mm, "
                               "deflection_limit_mm)");
  const auto flag = [&](const char* k) {
    if (!v.contains(k))
      return false;
    if (!v.at(k).is_boolean())
      throw std::runtime_error(std::string("nonlinear ") + k +
                               " is true or false");
    return v.at(k).get<bool>();
  };
  const auto mm = [&](const char* k, bool& has) {
    has = v.contains(k);
    if (!has)
      return 0.0;
    if (!v.at(k).is_number())
      throw std::runtime_error(std::string("nonlinear ") + k +
                               " is a number of millimetres");
    return v.at(k).get<double>();
  };
  nl.largeDeformation = flag("large_deformation");
  nl.plasticity = flag("plasticity");
  nl.snapThrough = flag("snap_through");
  if (v.contains("increments")) {
    nl.increments = IntParam(v.at("increments"), "nonlinear increments");
    if (nl.increments < 1)
      throw std::runtime_error("nonlinear increments is at least 1");
  }
  nl.imperfectionMm = mm("imperfection_mm", nl.hasImperfection);
  nl.deflectionLimitMm = mm("deflection_limit_mm", nl.hasDeflectionLimit);
  return nl;
}

// The faces of a solved mesh that lie on `face` (world coordinates, as an
// assembly study's placed bodies are): every boundary facet whose six nodes
// are on it, within a millionth of the mesh's size (the placement's float
// rounding) and never under 1e-4 mm. How an assembly study's probe finds a
// part's face in the imprinted fuse it meshed, whose faces are not the
// part's.
std::vector<int> MeshFacesOn(const fem::FemMesh& mesh,
                             const TopoDS_Shape& face) {
  std::vector<int> out;
  if (mesh.nodes.empty())
    return out;
  glm::dvec3 lo = mesh.nodes[0], hi = mesh.nodes[0];
  for (const glm::dvec3& n : mesh.nodes) {
    lo = glm::min(lo, n);
    hi = glm::max(hi, n);
  }
  const double tol = std::max(1e-4, 1e-6 * glm::length(hi - lo));
  Bnd_Box box;
  BRepBndLib::Add(face, box);
  box.Enlarge(tol);
  std::map<int, bool> onIt; // node -> on the face
  const auto nodeOn = [&](int n) {
    if (const auto it = onIt.find(n); it != onIt.end())
      return it->second;
    const glm::dvec3& q = mesh.nodes[n];
    bool on = false;
    if (!box.IsOut(gp_Pnt(q.x, q.y, q.z))) {
      BRepExtrema_DistShapeShape d(
          BRepBuilderAPI_MakeVertex(gp_Pnt(q.x, q.y, q.z)).Vertex(), face);
      on = d.IsDone() && d.Value() <= tol;
    }
    onIt[n] = on;
    return on;
  };
  for (const fem::FemMesh::BoundaryTri& t : mesh.boundary) {
    if (std::find(out.begin(), out.end(), t.faceIndex) != out.end())
      continue;
    bool all = true;
    for (int n : t.nodes)
      all = all && nodeOn(n);
    if (all)
      out.push_back(t.faceIndex);
  }
  return out;
}

// set_study's thermal_load: {from_study, stress_free_c?}, {uniform_c,
// stress_free_c}, or null. A from_study that names no thermal study of the
// part is refused here, naming those it has; on an assembly (no `doc`) a
// from_study is refused outright, since an assembly solves no heat.
fem::ThermalLoad ThermalLoadOf(const json& v, const Document* doc,
                               uint64_t self) {
  fem::ThermalLoad tl;
  if (v.is_null())
    return tl;
  if (!v.is_object())
    throw std::runtime_error(
        "thermal_load is {from_study, stress_free_c?} (a thermal study's "
        "temperature), {uniform_c, stress_free_c}, or null for none");
  for (const auto& [key, _] : v.items())
    if (key != "from_study" && key != "uniform_c" && key != "stress_free_c")
      throw std::runtime_error("thermal_load does not take `" + key +
                               "`; it takes from_study, uniform_c and "
                               "stress_free_c");
  if (v.contains("from_study") == v.contains("uniform_c"))
    throw std::runtime_error("thermal_load is EITHER {from_study} (a thermal "
                             "study's temperature) OR {uniform_c, "
                             "stress_free_c} (one temperature everywhere)");
  const auto number = [&](const char* key) {
    if (!v.at(key).is_number())
      throw std::runtime_error(std::string("thermal_load ") + key +
                               " is a number, degrees C");
    return v.at(key).get<double>();
  };
  if (v.contains("from_study")) {
    tl.kind = fem::ThermalLoad::Kind::FromStudy;
    if (doc == nullptr) {
      fem::Study probe;
      probe.thermalLoad = tl;
      throw std::runtime_error(fem::AssemblyThermalRefusal(probe));
    }
    tl.fromStudy = v.at("from_study").get<uint64_t>();
    const fem::Study* src = doc->FindStudy(tl.fromStudy);
    if (src == nullptr || src->type != fem::AnalysisType::Thermal ||
        tl.fromStudy == self) {
      std::string names;
      for (const fem::Study& st : doc->GetStudies())
        if (st.type == fem::AnalysisType::Thermal)
          names += (names.empty() ? "" : ", ") + std::to_string(st.id) + " ('" +
                   st.name + "')";
      throw std::runtime_error(
          "thermal_load from_study " + std::to_string(tl.fromStudy) +
          " is not a thermal study of this part (it has: " +
          (names.empty() ? "none" : names) + ")");
    }
  } else {
    tl.kind = fem::ThermalLoad::Kind::Uniform;
    tl.uniformC = number("uniform_c");
  }
  if (v.contains("stress_free_c")) {
    tl.hasStressFree = true;
    tl.stressFreeC = number("stress_free_c");
  } else if (tl.kind == fem::ThermalLoad::Kind::FromStudy &&
             !fem::DerivedStressFree(*doc->FindStudy(tl.fromStudy))) {
    throw std::runtime_error(
        "thermal_load needs stress_free_c: thermal study " +
        std::to_string(tl.fromStudy) +
        "'s convections do not share one ambient for the part to have been "
        "at before it was heated");
  }
  return tl;
}

fem::ConvergeMode ConvergeOf(const json& v) {
  fem::ConvergeMode mode;
  if (!v.is_string() ||
      !fem::ConvergeModeFromString(v.get<std::string>(), mode))
    throw std::runtime_error("converge " + v.dump() +
                             " is not one of: " + fem::kConvergeModeNames);
  return mode;
}

// Why a result's peak is not settled, in words; empty when it is.
std::string NotConvergedWhy(const fem::Convergence& c) {
  if (!c.checked)
    return "not checked: " +
           (c.why.empty() ? std::string("no check ran") : c.why);
  if (c.converged)
    return {};
  if (c.stopped == "time") {
    char buf[160];
    std::snprintf(buf, sizeof buf,
                  "stopped for time: a step took %.1f s, past "
                  "converge_auto_s",
                  c.stoppedSeconds);
    return buf;
  }
  if (c.stopped == "failed")
    return "a refinement failed: " + c.why;
  char buf[160];
  std::snprintf(buf, sizeof buf, "still moving %.1f %% after %d refinements",
                c.lastChangePct, static_cast<int>(c.steps.size()) - 1);
  return buf;
}

// What a static result says of its convergence check, `convergence`
// {status: converged | not converged | not checked, ...}.
json ConvergenceJson(const fem::Convergence& c) {
  json out;
  if (!c.checked) {
    out["status"] = "not checked";
    out["why"] = c.why.empty() ? "the result was solved without a "
                                 "convergence check (saved before it, or "
                                 "a static study of a topology result)"
                               : c.why;
    out["reason"] = "not checked: " + out["why"].get<std::string>();
    return out;
  }
  out["status"] = c.converged ? "converged" : "not converged";
  out["converged"] = c.converged;
  if (!c.converged)
    out["reason"] = NotConvergedWhy(c);
  if (!c.stopped.empty())
    out["stopped"] = c.stopped;
  if (c.stopped == "time")
    out["stopped_seconds"] = c.stoppedSeconds;
  if (!c.why.empty())
    out["why"] = c.why;
  out["last_change_pct"] = c.lastChangePct;
  if (c.hasLayerNormal)
    out["layer_normal"] = {c.layerNormal.x, c.layerNormal.y, c.layerNormal.z};
  json steps = json::array();
  for (const fem::ConvergenceStep& st : c.steps) {
    json refined = json::array();
    for (const fem::ConvergenceRefinement& r : st.refined)
      refined.push_back(
          {{"body", r.body}, {"face", r.face}, {"size_mm", r.sizeMm}});
    json peaks = json::array();
    for (const fem::ConvergencePeak& pk : st.peaks)
      peaks.push_back({{"body", pk.body},
                       {"quantity", pk.quantity},
                       {"away_from_support_edge", pk.away},
                       {"mpa", pk.mpa}});
    steps.push_back({{"refined", std::move(refined)},
                     {"nodes", st.nodes},
                     {"seconds", st.seconds},
                     {"peaks", std::move(peaks)}});
  }
  out["steps"] = std::move(steps);
  return out;
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
     "Add a study {type: static|modal|topology|buckling|thermal, name?} "
     "(thermal: steady heat conduction, a part study's; "
     "docs/simulation/README.md#thermal). `kind` is "
     "accepted for the same thing — the enum, the file and `list_studies` "
     "all say `type`, and so do add_mate/add_load/add_cam_op/add_relation. "
     "A new study's solver is auto (in-house); set_study `solver` changes "
     "it. A part that carries a load gets a static study of each way it is "
     "loaded AND a buckling study of each load that compresses something "
     "(struts, ribs, thin walls, tubes, levers in compression), with "
     "requirements on factor_of_safety and buckling_factor: a part that "
     "buckles passes its static study (docs/simulation/README.md#buckling).",
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
     "studies:{converge?:string, filter_radius?:number, has_results:bool, "
     "id:int, "
     "keep_faces?:int, keep_outs?:object[], load_cases?:object[], "
     "loads:object[], max_iterations?:int, mesh_size:number, "
     "name:string, num_modes?:int, refinements:int, stale:bool, type:string, "
     "volume_fraction?:number, shape_from?:object, solver?:string, "
     "nonlinear?:object, participants?:object[]}[]"},
    {"set_study",
     "Edit study settings (mesh size, materials, solver options). mesh_size "
     "and filter_radius are MILLIMETRES; `gravity` is metres per second "
     "squared (m/s^2) as a [x, y, z] vector, so Earth down is [0, 0, -9.81] "
     "with Z up. volume_fraction is a fraction of 1 and the iteration counts "
     "are counts. Takes `id` (or `study`, the same thing here) — see "
     "remove_study.\n"
     "On an ASSEMBLY, `participants` is the study: a list of {occurrence, "
     "role: meshed|bonded|contact|rigid|ignored, mesh_size?, friction?}. "
     "Exactly one occurrence "
     "is `meshed`; `bonded` is meshed AND glued to it where they touch, with "
     "its own material; `contact` is meshed APART from it and in contact "
     "with it where their faces face each other -- it presses on it and may "
     "lift off or slide, with `friction` (the coefficient, unitless; default "
     "0, frictionless), solved on CalculiX "
     "(docs/simulation/README.md#contact); `rigid` is not meshed and holds "
     "the meshed part where they touch — but only once CLAMPED, which is a "
     "load (add_load {type: fixed, contact_with}) or rests it on it "
     "(add_load {type: contact, contact_with}), not a consequence of the "
     "role. "
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
     "(docs/topopt/README.md#a-static-study-of-the-optimised-shape). "
     "`num_modes` is a modal study's (its frequencies) or a buckling "
     "study's (its factors; default 3, as a symmetric section has two "
     "equal first modes), and `volume_fraction`, "
     "`filter_radius` and `max_iterations` a topology study's: each is "
     "refused on another type unless the same call sets that type (type "
     "is applied first). `solver` is auto|in_house|calculix: auto and "
     "in_house solve every type in-house; calculix solves a STATIC study on "
     "CalculiX (ccx, an external program: refused on modal or topology, and "
     "at run_study when no ccx is found, naming how to install one) on the "
     "same mesh, loads and supports, as a second, independent answer whose "
     "displacement is checked against this study's own stiffness "
     "(docs/simulation/README.md#calculix); it solves a BUCKLING study "
     "there too (*BUCKLE). A buckling study takes no `load_cases` (a factor "
     "scales one load set). `converge` is a static or buckling (its base "
     "state) study's: auto (the default) | always | never -- whether a run "
     "refines round its peak stress until it settles "
     "(docs/simulation/README.md#a-peak-that-has-settled); anything else is "
     "refused, naming the three.\n"
     "`nonlinear` solves a STATIC study past linear, on CalculiX (auto picks "
     "it; in_house is refused): {large_deformation? (NLGEOM), plasticity? "
     "(each material past its yield along its library hardening curve; a "
     "material with none -- every one but steel today -- is refused, never "
     "run perfectly plastic; docs/simulation/README.md#plasticity), at least "
     "one of the two (or a contact), increments? (the step's largest increment "
     "is its load over this many; "
     "default 10, 20 on a snap-through run), snap_through? (seed the shape "
     "with the first linear buckling mode and step the load up to its PEAK: "
     "the answer for an arch or a curved wall loaded across its curve, "
     "which linear buckling cannot see), imperfection_mm? (the seed's peak "
     "motion, MILLIMETRES; default a tenth of the wall the mode bends; 0 = "
     "the perfect shape), deflection_limit_mm? (MILLIMETRES of motion past "
     "which the part counts as failed; default a tenth of its size)}, or "
     "null for linear. Large deformation steps the load up on the deformed "
     "part: a pressure follows its face, a force keeps its direction, a "
     "torque or remote load is carried from a reference node "
     "(docs/simulation/README.md#nonlinear-static). Refused with load "
     "cases, on another analysis type, with an unknown key, and with "
     "converge always on a snap-through or plastic run. snap_through needs "
     "large_deformation. A study with a CONTACT (a "
     "contact support, or a contact participant) is nonlinear by that "
     "alone, and takes `nonlinear` {increments?} without large_deformation "
     "(solved at small deformation) or with it; it is refused in_house, on "
     "another analysis, with load cases, with snap_through, and with "
     "converge always (docs/simulation/README.md#contact).\n"
     "`thermal_load` expands a STATIC study under a "
     "temperature: {from_study: <a thermal study's id>, stress_free_c?} "
     "re-solves that study's heat on this study's own mesh (both must load "
     "the same bodies; stress_free_c, degrees C, defaults to its one "
     "convection ambient and is required when it has none or several), or "
     "{uniform_c, stress_free_c} (degrees C, one temperature everywhere), "
     "or null for none. The thermal strain is alpha (T - T0) with the "
     "material's expansion coefficient (one without is refused, naming the "
     "materials that have one); every load case carries it, as gravity is. "
     "Refused on any other study (thermal buckling is not solved yet), with "
     "snap_through, "
     "beside a contact participant, and with an unknown key "
     "(docs/simulation/README.md#the-thermal-load-on-a-static-study). On an "
     "ASSEMBLY study only the uniform form is taken: each meshed or bonded "
     "member expands by its own material's coefficient (a bimetal), and "
     "from_study is refused, since an assembly solves no heat "
     "(docs/simulation/README.md#a-thermal-load-on-an-assembly). A "
     "THERMAL study takes converge (on its peak temperature rise) and "
     "solver (calculix solves it with *HEAT TRANSFER as a cross-check), and "
     "refuses load_cases, gravity, nonlinear, shape_from and thermal_load.",
     "converge, filter_radius, gravity, id, load_cases, max_iterations, "
     "mesh_size, name, nonlinear, num_modes, participants, shape_from, "
     "solver, study, thermal_load, type, "
     "volume_fraction"},
    {"add_load",
     "Add a load/fixture to a study {study, type: fixed|force|pressure|"
     "torque|remote|pin|contact, face, ...}. force/pressure take `magnitude`: "
     "NEWTONS "
     "for a force -- the total on that face, spread over its area -- and MPa "
     "for a pressure, which pushes along the inward normal; `direction?`. "
     "`fixed` takes neither. A torque takes `torque_nmm` (N*mm, right-handed "
     "about the axis) and turns the face about `axis_origin_mm` + "
     "`axis_dir` -- both omitted on a cylindrical face, which lends its own "
     "axis (signed so its largest component is positive); a torque on any "
     "other face without them is refused. It is the traction (T/J) a x "
     "(x - o): exactly T about the axis, and no net force on a face "
     "symmetric about it. A remote load takes `force_n` (N), `direction?` "
     "and `point_mm` (required, [x, y, z] mm): that force acting at that "
     "point, carried by the face as its share F/A plus the linear traction "
     "that carries the moment (p - c) x F about the face's centroid, the "
     "way an RBE3 spreads it. A key another type takes is refused, naming "
     "the right one. On an assembly study the axis and point are WORLD "
     "coordinates (docs/simulation/README.md#torque-and-remote-loads). "
     "Every result scales linearly with these numbers and a wrong unit is "
     "invisible in the answer, so it is stated here rather than left to be "
     "inferred from the solver. An omitted force `direction` pushes into "
     "the picked face (the same default a GUI placement takes). A `face` is "
     "{feature_id, element}, or just the element name — the body that has "
     "it is then found for you. On a study with load cases a force, "
     "pressure, torque or remote load names its `case`; a support names "
     "none (it holds in every case). `region` puts the load or the support "
     "on PART of its face, on a part study: {normal, from_mm, to_mm} the "
     "band of the face between the planes n.p = from_mm and n.p = to_mm "
     "(n the unit normal, mm in the part's frame), or {center_mm, "
     "radius_mm} the face within that radius of a point; applied to the "
     "face's mesh facets whose centroids are in it, the force the whole "
     "`magnitude` over them (a seat on a shaft, without a step to make it "
     "a face; docs/simulation/README.md#loads-on-part-of-a-face). A `pin` "
     "is a support on a CYLINDRICAL face (a bore, or a shaft in a bearing; "
     "any other face is refused): a rigid pin on the face's axis that holds "
     "it radially and axially only where the part presses on it -- "
     "compression only, found by a short iteration of solves -- and lets it "
     "turn about the axis unless `fix_rotation: true`. A part free to turn "
     "on one pin, or one the loads lift off its pins, is refused, naming "
     "the free motion. Static and buckling part studies only (a buckling "
     "study holds the contact its base state settled on, at every factor); "
     "it takes no magnitude or "
     "direction (docs/simulation/README.md#pin-supports). A `contact` is a "
     "COMPRESSION-ONLY support solved on CalculiX: the face rests on the "
     "ground -- the plane a FLAT face lies on (a curved one is refused) -- "
     "or, on an assembly study, with `contact_with` a rigid participant, on "
     "that occurrence where their faces face each other; it pushes and "
     "never pulls, the part may lift off it, and `friction` is its "
     "coefficient (unitless, 0 or more; default 0, frictionless). It takes "
     "no magnitude, direction or region, and is refused where a nonlinear "
     "study is (in_house, another analysis, load cases) and beside a pin; "
     "study_results reports each contact's closed and open area, its "
     "pressure and its force (docs/simulation/README.md#contact). A THERMAL "
     "study's loads (each refused on any other study, and every structural "
     "load refused on a thermal one): `temperature` {face, temperature_c} "
     "holds the face at that many degrees C; `heat_flux` {face, power_w | "
     "flux_w_m2} puts heat into it -- exactly one of the total in watts, "
     "spread uniformly, or watts per square metre (negative: out); "
     "`convection` {face, film_w_m2k, ambient_c} cools it by a fluid at "
     "ambient_c with that film coefficient, W/(m^2 K) (Incropera's Table "
     "1.1: free convection in gases 2-25, forced 25-250, liquids higher); "
     "`heat_generation` {body, power_w} makes that many watts through the "
     "body named, spread over its volume. None has a default: each value "
     "is required, a temperature below -273.15 C and a film coefficient "
     "that is not positive are refused. All but heat_generation take "
     "`region` (docs/simulation/README.md#thermal).",
     "ambient_c, axis_dir, axis_origin_mm, body, case, contact_with, "
     "direction, face, film_w_m2k, fix_rotation, flux_w_m2, force_n, "
     "friction, magnitude, occurrence, point_mm, power_w, region, study, "
     "temperature_c, torque_nmm, type"},
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
     "job?:int, kind?:string, outlives_client?:bool, running?:bool, "
     "shape_study:int, solid_study:int, started?:bool, "
     "cases?:{displacement_ratio:number, name:string, shape:object, "
     "solid:object}[], dropped_tets?:int, islands?:{components:int, "
     "keep_faces:any[], load_path_broken:bool, tets:int, "
     "volume_mm3:number}, reused?:bool, threshold?:number, "
     "threshold_lowered_from?:number, topology?:int"},
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
     "runs anyway. A static run checks its peak has settled "
     "(docs/simulation/README.md#a-peak-that-has-settled): it halves the "
     "element size on the face each governing peak lies on (von Mises; for "
     "a printed material also the stress across the layers along "
     "`layer_normal` [x, y, z], the build direction in the part's frame, "
     "unitless -- without one, the largest principal stress), re-meshes "
     "and re-solves until every peak moves under 2 % or four refinements "
     "are spent; a peak on a support's edge is read away from it. "
     "`converge`: auto | always | never overrides the study's for this run "
     "(auto runs the check while a solve takes under the converge_auto_s "
     "preference, seconds). The refinements belong to the run and never to "
     "the study; the answer's `convergence` says what happened. Both are "
     "inputs, so a run with them is not answered from one without. Takes "
     "`id` (or `study`, the same thing here) — see remove_study. A study "
     "whose solver is calculix runs ccx in its own directory under the "
     "cache ($XDG_CACHE_HOME/lobole/calculix/<pid>-<n>, removed on success, "
     "kept and named in the refusal on failure); it is refused before "
     "anything starts when no ccx is found (naming every way to install "
     "one) or when its memory estimate exceeds what the machine has free, "
     "and a ccx failure is refused with ccx's first *ERROR line. "
     "`timeout_s` (seconds, CalculiX runs only; refused on an in-house "
     "study) kills ccx past it and refuses the run "
     "(docs/simulation/README.md#calculix). A NONLINEAR study (set_study "
     "nonlinear) runs on CalculiX the same way; one whose solve stops "
     "converging before its load is refused with the fraction of the load it "
     "reached and nothing stored; a snap-through run stops at its peak, and "
     "a jump or the deflection limit is bracketed by a second ccx run "
     "(docs/simulation/README.md#nonlinear-static).",
     "id?:int>=0, study?:int>=0, async?:bool, outlive_client?:bool, "
     "recompute?:bool, timeout_s?:number>0, converge?:string, "
     "layer_normal?:number[]"},
    {"cancel_study",
     "Cancel a running study: {job} (the id run_study, run_motion_study, "
     "mesh_study, check_requirements or design_sweep answered -- any study "
     "job, motion included; a design sweep stops between points, keeping "
     "them) or `id` (or "
     "`study`, the same thing here; a part or assembly study's running "
     "job). A topology run keeps the best shape so far; a static or modal "
     "run stops at its next boundary (after meshing, or within a few "
     "iterations of an iterative solve; a direct factorization has no "
     "interruption points and ends first) and drops its answer. A job "
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
     "are. A running CalculiX study also says `memory_estimate_bytes` once "
     "its mesh is made, and its `solver` names the phase (writing the deck, "
     "solving, checking the answer); a nonlinear one also `load_fraction` "
     "(the load reached over the load it runs to, from ccx's .sta, as each "
     "increment lands). A running in-house static study says `iterations`: "
     "its iterative solve's so far, on the CPU or the GPU (0 for a direct "
     "one).",
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
     "`study`, the same thing here) — see remove_study. A static study also "
     "reports max_principal_mpa {value, at, body}: the largest TENSILE "
     "principal stress, MPa, which a brittle material (a printed plastic) "
     "breaks on where von Mises is the yield check. `layer_normal` [x, y, z] "
     "(a direction in the part's frame, unitless; a part study only) is a "
     "printed part's BUILD direction: the answer adds max_across_layers_mpa "
     "{value, at, body} (the largest normal stress on the layer planes, n·σ·n, "
     "tension positive) and interlayer_factor_of_safety against the "
     "material's interlayer strength (pla 17, petg 18 MPa). The study is "
     "isotropic, so one solve answers every orientation "
     "(docs/simulation/README.md#printed-layers). Refused on a material with "
     "no interlayer strength, and on a result saved before v172, which kept "
     "no stress tensor. A static body (and each case) also carries "
     "`reactions` [{support, type, force_n [x,y,z] N -- what the support "
     "exerts on the part; moment_n_mm [x,y,z] N mm, its moment about the "
     "frame's origin (the part's; world on an assembly study), absent on a "
     "result saved before it was kept; a pin's contact_area_mm2 of "
     "face_area_mm2}], and, "
     "when its peak von Mises sits within one element of a support's edge "
     "(a clamp's rim, where the stress is a singularity that grows with the "
     "mesh), `peak_at_support_edge` {support, type, max_von_mises_away_mpa, "
     "away_at, factor_of_safety_away}: the largest stress farther than the "
     "support's reach from every support's edge (a quarter of a pin's bore "
     "radius; a quarter of a fixed face's largest extent; never less than "
     "one element). With layer_normal it adds max_across_layers_away_mpa "
     "and across_layers_away_at, the stress across the layers read the "
     "same way, and factor_of_safety_away is the governing (smaller) of the "
     "two, named by `governing`. The top level carries the flag of its own "
     "peak, and the factor of safety's receipt says so "
     "(docs/simulation/README.md#a-peak-on-a-supports-edge). A static "
     "result names its `solver`; one solved on CalculiX says \"calculix "
     "2.NN\", carries `calculix` {residual, residual_floor, "
     "energy_residual} (its displacement's |K u - f| / |f| against this "
     "study's own stiffness, the part of it the printed precision accounts "
     "for, and |u . (K u - f)| / |u . f|, which the precision barely moves "
     "and which is held to 1e-6), and, when the "
     "session holds an in-house result of the same study, "
     "`in_house_agreement` {max_displacement_rel, max_von_mises_rel}, or "
     "{not_compared: why} when the two runs ended on different meshes: "
     "reported beside the answer, never folded into its receipt, which "
     "stays a prediction with oracle calculix/static "
     "(docs/simulation/README.md#calculix). A static "
     "result also carries `convergence` {status: converged | not converged "
     "| not checked, converged, stopped? (time | steps | failed), "
     "stopped_seconds?, why?, last_change_pct, layer_normal?, steps "
     "[{refined [{body, face, size_mm}], nodes, seconds, peaks [{body, "
     "quantity, away_from_support_edge, mpa}]}]} "
     "(docs/simulation/README.md#a-peak-that-has-settled), and the receipt "
     "says whether its peak converged. A BUCKLING result "
     "(docs/simulation/README.md#buckling) carries, per body, "
     "`buckling_factors` (every factor found, signed -- a negative one "
     "buckles under the reversed load -- ascending by magnitude), "
     "`buckling_factor` (the smallest positive; null with `why_none` when "
     "nothing in the load buckles), `knockdown` {value, class: flat | "
     "member | cylinder | cone | sphere, face?, radius_mm?, wall_mm?} (NASA "
     "SP-8007 on a curved wall the first mode peaks on; 1 on a flat face "
     "or a member), `yield_factor` (yield / peak von Mises of the base "
     "state, factor 1), `interlayer_factor` (a printed material: interlayer "
     "strength / peak tension, `interlayer_from` max_principal, or "
     "across_layers with `layer_normal`), `governing_factor` (1/g = "
     "1/(knockdown x buckling) + 1/min(yield, interlayer): Rankine-Gordon), "
     "`base_state` {max_von_mises_mpa, max_displacement_mm}, and the flags "
     "`mode_at_load_edge` {load, type, face} (the first mode is local "
     "where a load enters: crippling, not member buckling) and "
     "`through_thickness` (the mode bends a wall with one element through "
     "it). The top level has buckling_factor, governing_factor, "
     "factor_of_safety (= governing_factor), `notes`, `warnings`, "
     "`convergence` of the base state, and a receipt with oracle "
     "in_house/buckle (calculix/buckle on CalculiX), basis predicted. A "
     "NONLINEAR result (set_study nonlinear) carries per body `nonlinear` "
     "{factor (the load factor its fields are at: 1, or the peak), "
     "increments, ccx_runs, curve [{factor, max_displacement_mm}] (every "
     "converged increment), equilibrium {residual_criterion (ccx's largest "
     "residual over the mean nodal force), imbalance (the supports' total "
     "reaction against the load applied there, a pressure on the deformed "
     "faces, over the load carried), tolerance}, first_yield_factor (the "
     "factor the peak von Mises first reaches yield at; null when it does "
     "not), snapped? {from_factor, to_factor} (a large-deformation run "
     "that jumped to another branch on its way: warned), and on a "
     "snap-through run linear_factor, imperfection_mm, imperfection, "
     "deflection_limit_mm, max_factor (the cap: twice the linear factor), "
     "peak_at (limit_point | snap | deflection_limit | none), peak_factor, "
     "peak_bracket [low, high] (null with why_none at none)}; the top level "
     "has `nonlinear` {kind: large_deformation | snap_through, peak_factor, "
     "governing_factor (the peak, or the first yield when earlier), peak_at, "
     "body}, `notes` and `warnings`. A snap-through study's "
     "factor_of_safety is its governing factor, with a receipt claim "
     "\"peak load factor >= 1\" (oracle calculix/nonlinear); a "
     "large-deformation one's is yield over the Cauchy stress at the load "
     "(docs/simulation/README.md#nonlinear-static). A study with a CONTACT "
     "also carries per body `contacts` [{support | occurrence (the contact "
     "support's load id, or the contact participant's occurrence), kind: "
     "support | participant, name (ground, or the partner), friction "
     "(unitless), penalty_mpa_per_mm (the derived contact stiffness: "
     "pressure per overclosure), area_mm2 (the pair's faces on the body's "
     "side), closed_area_mm2 and open_area_mm2 (of those, in contact and "
     "not), force_n [x,y,z] N and moment_n_mm [x,y,z] N mm (what the "
     "partner exerts on that side, the moment about the frame's origin), "
     "normal_force_n, shear_force_n, max_pressure_mpa, mean_pressure_mpa "
     "(normal force over the closed area)}], a contact support's "
     "`reactions` row its contact_area_mm2 of face_area_mm2, `nonlinear` "
     "{large_deformation, stabilization? (what the weak springs holding a "
     "part held only by a contact carried, over the load)}, and the top "
     "level `nonlinear` {kind: contact} (docs/simulation/README.md#contact). "
     "A PLASTIC study (nonlinear plasticity: true) carries per body "
     "max_plastic_strain (the peak equivalent plastic strain, unitless), "
     "max_residual_displacement_mm (what is left once the load is taken off "
     "again; absent when it collapsed), and in `nonlinear` `plasticity` "
     "{unloaded, strain_limit (where its hardening data ends), collapsed, "
     "collapse_bracket? [low, high]}; the top level max_plastic_strain "
     "{value, at, body}, max_residual_displacement_mm {value, at, body}, "
     "and `nonlinear` {kind: plasticity, collapse_factor?}. Its stress is "
     "CalculiX's own at each Gauss point, carried to the nodes. A run that "
     "stops converging as its stiffness runs out has COLLAPSED below its "
     "load: that factor is its factor_of_safety (receipt \"collapse load "
     "factor >= 1\"); one that carries its load keeps yield over the peak "
     "von Mises. A plastic strain past strain_limit is warned "
     "(docs/simulation/README.md#plasticity). A THERMAL result carries per "
     "body max_temperature_c, min_temperature_c and max_heat_flux_w_m2 "
     "{value, at}, `flows` [{load, type, watts_out}] (the heat through each "
     "load, W, positive out of the part: a held face's from K_T T - Q at its "
     "nodes, a convecting face's integral of h (T - T_amb), a flux face's "
     "and a source's their applied power), `balance` {in_w, out_w, "
     "imbalance_w}, and where the material has a service temperature "
     "temperature_margin_k, service_max_c and service_basis; the top level "
     "the peaks {value, at, body}, temperature_margin_k {value, body, "
     "service_max_c, basis}, `convergence` (on the peak temperature rise), "
     "`warnings` (a printed material's conductivity is its bulk resin's), "
     "and a receipt claiming \"peak temperature below the material's "
     "service temperature\" (oracle in_house/heat or calculix/heat). A "
     "static result under a thermal_load adds `temperature` {min_c, max_c, "
     "stress_free_c, from_study?} and warns of a body past its service "
     "temperature (docs/simulation/README.md#thermal).",
     "id, layer_normal, study"},
    {"set_result_display",
     "Choose the result field the VIEWPORT draws, and how far it exaggerates "
     "the deformation {study, field: von_mises|displacement|shape|density|"
     "temperature|heat_flux|none, deform_scale?, mode? (0-based, which mode "
     "shape for a modal or buckling study), threshold?} (temperature: a "
     "thermal study's, or a static one's under a thermal_load, its legend in "
     "degrees C; heat_flux: a thermal study's, W/m^2). It colours the raster "
     "viewport, which is "
     "what "
     "`screenshot` captures. It does NOT reach `render_still`: the path "
     "tracer renders the MODEL, so a still taken after this shows the "
     "undeformed part in its own material and looks like the call silently "
     "did nothing. A static study with load cases shows `case` (its name or "
     "0-based index).",
     "case, deform_scale, field, mode, study, threshold"},
    {"probe_study",
     "How one FACE moved and how hard it is stressed under a solved static "
     "study {id (or study), face, occurrence?, at?}; on an ASSEMBLY study "
     "`occurrence` names the placed part the face is on, and the facets of "
     "the meshed fuse that lie on it are read (at and the answer in world "
     "coordinates). Stress: von_mises_max_mpa (the "
     "largest nodal von Mises on the face, MPa) at von_mises_max_at_mm, and "
     "von_mises_mean_mpa (area-weighted over the face) -- a tooth root or a "
     "fillet, where the global peak may sit in a sharp corner whose value "
     "grows with the mesh. Motion, as a rigid body: the least-squares "
     "small-displacement fit "
     "of the face's mesh nodes, u(p) = translation + rotation x (p - "
     "centroid), each node weighted by the area it stands for. Returns "
     "{nodes, area_mm2, von_mises_max_mpa, von_mises_max_at_mm, "
     "von_mises_mean_mpa, centroid_mm, translation_mm (the centroid's "
     "displacement, MILLIMETRES), rotation_rad (a small-angle rotation "
     "vector, RADIANS), rotation_magnitude_deg, rms_residual_mm (what the "
     "rigid fit leaves: the face's own distortion)}. `at` is a point "
     "[x, y, z] in MILLIMETRES (part coordinates): the fitted motion carried "
     "there comes back as displacement_at_mm -- where whatever the face "
     "holds rigidly goes, a bearing seat's shaft, the lens on that shaft. "
     "The maxima study_results reports are somewhere on the part; this is "
     "what one seat did. Refused on a stale study (re-run it: the face "
     "named now may not be the face that was meshed), an assembly study "
     "without `occurrence` (or a part study with one), "
     "or a modal or topology one (docs/simulation/README.md#probing-a-face). "
     "A study with load cases takes `case` (its name or 0-based index; "
     "default the first). Also principal_max_mpa, principal_max_at_mm and "
     "principal_mean_mpa: the face's largest TENSILE principal stress; and "
     "with `layer_normal` [x, y, z] (the build direction, unitless, as "
     "study_results takes it) across_layers_max_mpa, "
     "across_layers_max_at_mm and across_layers_mean_mpa: the tension "
     "pulling the face's printed layers apart "
     "(docs/simulation/README.md#printed-layers). A plastic result adds "
     "plastic_strain_max (and _at_mm) and plastic_strain_mean (the "
     "equivalent plastic strain on the face, unitless), and "
     "residual_translation_mm / residual_rotation_rad: the face's rigid "
     "motion once the load is off (docs/simulation/README.md#plasticity).",
     "at, case, face, id, layer_normal, occurrence, study"},
    {"export_study_inp",
     "Write the CalculiX deck a STATIC, BUCKLING or THERMAL study would run "
     "{id (or study), "
     "path, body?, case?} -- a nonlinear one's large-deformation deck "
     "(NLGEOM steps, a pressure as a follower *DLOAD, a torque or remote load "
     "on a *COUPLING reference node; a snap-through run's is refused, its "
     "seed and cap being found when it runs): our Netgen mesh as C3D10 "
     "elements (one ELSET per "
     "bonded member), a material per domain (E and density in MPa and "
     "t/mm^3: the deck is mm, N, MPa), the assembled nodal forces as *CLOAD "
     "(every load kind, gravity included, as the in-house solve applies "
     "them), fixed supports as *BOUNDARY and a pin's nodes in a cylindrical "
     "*TRANSFORM held where the in-house contact iteration found the pin "
     "pressing. Meshes now; needs no ccx. A study meshing several bodies "
     "names one with `body`; one with load cases names `case` (its name or "
     "0-based index): each is its own deck. A thermal study's is its heat "
     "deck (*HEAT TRANSFER, STEADY STATE: *CONDUCTIVITY, held nodes on "
     "degree of freedom 11, the nodal heat as *CFLUX, *FILM on the "
     "convecting faces; mm, mW, degrees C); a static study's under a "
     "thermal_load carries *EXPANSION and the nodes' *TEMPERATURE. Refused "
     "on a modal or topology study (docs/simulation/README.md#calculix).",
     "body, case, id, path, study", remote::NotInBatch,
     "body:string, bytes:int, elements:int, nodes:int, path:string"},
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
     "part:string, part_id:int, path:string, threshold:number, "
     "triangles:int, valid:bool, volume_mm3:number, watertight:bool"},
};
std::span<const MethodDoc> SimulationMethodDocs() { return SimulationMethods; }

namespace {

// export_study_inp (docs/simulation/README.md#calculix): the deck a static
// study would hand CalculiX, for body `body` and load case `case`, meshed
// here. Needs no ccx. Every parameter is read, and the deck written into
// memory, before the file is touched.
json ExportStudyInp(ui::AppState& state, Host& host, const RpcParams& p) {
  const uint64_t id =
      StudySubject(p.contains("id"), p.value("id", uint64_t{0}),
                   p.contains("study"), p.value("study", uint64_t{0}));
  fem::Study* s = &host.Require(id);
  if (s->type != fem::AnalysisType::Static &&
      s->type != fem::AnalysisType::Buckling &&
      s->type != fem::AnalysisType::Thermal)
    throw std::runtime_error(
        std::string("a CalculiX deck is written for a static, buckling or "
                    "thermal study; this one is ") +
        fem::AnalysisTypeToString(s->type));
  if (!p.contains("path") || !p.at("path").is_string() ||
      p.at("path").get<std::string>().empty())
    throw std::runtime_error("path is required: the .inp file to write");
  const std::filesystem::path path = p.at("path").get<std::string>();
  std::error_code ec;
  const std::filesystem::path parent =
      path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");
  if (!std::filesystem::is_directory(parent, ec))
    throw std::runtime_error("path: the folder " + parent.string() +
                             " does not exist");
  fem::StudyPlan plan;
  std::string err;
  const bool planned = host.IsAssembly()
                           ? fem::PlanAssemblyStudy(
                                 *host.asmDoc, *s,
                                 ui::AssemblyStudyContextFor(state), plan, &err)
                           : fem::PlanStudy(*host.part, *s, plan, &err);
  fem::StudyJobInput input;
  if (!planned || !fem::MeshPlannedStudy(plan, input, &err))
    throw std::runtime_error(err);
  // Which body: by name, or the only one.
  size_t body = 0;
  if (p.contains("body")) {
    const std::string want = p.at("body").get<std::string>();
    std::string names;
    bool found = false;
    for (size_t i = 0; i < input.bodies.size(); ++i) {
      names += (i ? ", " : "") + input.bodies[i].bodyName;
      if (input.bodies[i].bodyName == want) {
        body = i;
        found = true;
      }
    }
    if (!found)
      throw std::runtime_error("body '" + want +
                               "' is not one this study meshes: " + names);
  } else if (input.bodies.size() > 1) {
    std::string names;
    for (size_t i = 0; i < input.bodies.size(); ++i)
      names += (i ? ", " : "") + input.bodies[i].bodyName;
    throw std::runtime_error("this study meshes " +
                             std::to_string(input.bodies.size()) +
                             " bodies, each its own deck: name one with "
                             "`body` (" +
                             names + ")");
  }
  // Which case: by name or 0-based index, or the only one.
  size_t caseIndex = 0;
  if (p.contains("case")) {
    const json& c = p.at("case");
    bool found = false;
    for (size_t k = 0; k < input.caseNames.size(); ++k)
      if ((c.is_string() && c.get<std::string>() == input.caseNames[k]) ||
          (c.is_number_unsigned() && c.get<size_t>() == k)) {
        caseIndex = k;
        found = true;
      }
    if (!found)
      throw std::runtime_error(
          "case " + c.dump() +
          (input.caseNames.empty()
               ? std::string(" names a load case, and this study has none")
               : " is not one of this study's load cases"));
  } else if (input.caseNames.size() > 1) {
    std::string names;
    for (size_t k = 0; k < input.caseNames.size(); ++k)
      names += (k ? ", " : "") + input.caseNames[k];
    throw std::runtime_error("each load case is its own deck: name one with "
                             "`case` (" +
                             names + ")");
  }
  std::ostringstream deck;
  std::string title =
      "study '" + s->name + "', body '" + input.bodies[body].bodyName + "'";
  if (!input.caseNames.empty())
    title += ", load case '" + input.caseNames[caseIndex] + "'";
  if (!fem::WriteStudyDeck(input, body, caseIndex, deck, title, &err))
    throw std::runtime_error(err);
  const std::string text = deck.str();
  std::ofstream out(path, std::ios::binary);
  out << text;
  out.close();
  if (!out)
    throw std::runtime_error("could not write " + path.string());
  return json{{"path", path.string()},
              {"body", input.bodies[body].bodyName},
              {"nodes", input.bodies[body].mesh.nodes.size()},
              {"elements", input.bodies[body].mesh.tets.size()},
              {"bytes", text.size()}};
}

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
      throw std::runtime_error("study type '" + want +
                               "' is not one of: " + fem::kAnalysisTypeNames);
    if (hasKind && type == fem::AnalysisType::Topology)
      topoOnly();
    if (type == fem::AnalysisType::Thermal && host.IsAssembly())
      throw std::runtime_error(
          "a thermal study is a part study's: an assembly study does not "
          "solve heat yet (docs/simulation/README.md#not-done)");
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
        if (fem::IsThermalLoad(l.type)) {
          lj.erase("face");
          const auto given = [&](const char* key, double v) {
            if (!std::isnan(v))
              lj[key] = v;
          };
          given("temperature_c", l.temperatureC);
          given("power_w", l.powerW);
          given("flux_w_m2", l.fluxWm2);
          given("film_w_m2k", l.filmWm2K);
          given("ambient_c", l.ambientC);
          if (l.type == fem::LoadType::HeatGeneration)
            lj["body"] = l.body;
          else
            lj["face"] = {{"feature_id", l.face.featureId},
                          {"element", l.face.name}};
        } else if (!fem::IsSupport(l.type))
          lj["magnitude"] = l.magnitude;
        if (l.type == fem::LoadType::Pin)
          lj["fix_rotation"] = l.fixRotation;
        if (l.type == fem::LoadType::Contact)
          lj["friction"] = l.friction;
        if (l.type == fem::LoadType::Force || l.type == fem::LoadType::Remote)
          lj["direction"] = {l.direction.x, l.direction.y, l.direction.z};
        if (l.type == fem::LoadType::Torque) {
          lj["torque_nmm"] = l.magnitude;
          // The axis it turns about: its own, or its cylindrical face's.
          if (l.HasAxis()) {
            lj["axis_origin_mm"] = {l.axisOrigin.x, l.axisOrigin.y,
                                    l.axisOrigin.z};
            lj["axis_dir"] = {l.axisDirection.x, l.axisDirection.y,
                              l.axisDirection.z};
          } else {
            lj["axis_from_face"] = true;
          }
        }
        if (l.type == fem::LoadType::Remote) {
          lj["force_n"] = l.magnitude;
          lj["point_mm"] = {l.point.x, l.point.y, l.point.z};
        }
        if (!l.loadCase.empty())
          lj["case"] = l.loadCase;
        if (!l.region.IsWhole())
          lj["region"] = DocumentIO::StudyToJson([&] {
            fem::Study one;
            one.loads.push_back(l);
            return one;
          }())["loads"][0]["region"];
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
      if (Converges(s.type))
        sj["converge"] = fem::ConvergeModeToString(s.converge);
      if (s.thermalLoad.On()) {
        json t = json::object();
        if (s.thermalLoad.kind == fem::ThermalLoad::Kind::FromStudy)
          t["from_study"] = s.thermalLoad.fromStudy;
        else
          t["uniform_c"] = s.thermalLoad.uniformC;
        if (s.thermalLoad.hasStressFree)
          t["stress_free_c"] = s.thermalLoad.stressFreeC;
        sj["thermal_load"] = std::move(t);
      }
      // A contact makes a study nonlinear by itself
      // (docs/simulation/README.md#contact): its increments are listed,
      // and `contact` says why it solves on CalculiX.
      if (fem::IsNonlinear(s) || s.nonlinear.increments > 0) {
        json n = {{"large_deformation", s.nonlinear.largeDeformation}};
        if (s.nonlinear.plasticity)
          n["plasticity"] = true;
        if (fem::HasContact(s))
          n["contact"] = true;
        if (s.nonlinear.increments > 0)
          n["increments"] = s.nonlinear.increments;
        if (s.nonlinear.snapThrough)
          n["snap_through"] = true;
        if (s.nonlinear.hasImperfection)
          n["imperfection_mm"] = s.nonlinear.imperfectionMm;
        if (s.nonlinear.hasDeflectionLimit)
          n["deflection_limit_mm"] = s.nonlinear.deflectionLimitMm;
        sj["nonlinear"] = std::move(n);
      }
      if (s.type == fem::AnalysisType::Modal ||
          s.type == fem::AnalysisType::Buckling)
        sj["num_modes"] = s.numModes;
      if (!eval.participants.empty()) {
        json parts = json::array();
        for (const fem::Participant& pt : eval.participants) {
          json pj = {{"occurrence", pt.occurrenceId},
                     {"name", host.OccurrenceName(pt.occurrenceId)},
                     {"role", fem::ParticipantRoleToString(pt.role)}};
          if (pt.meshSize > 0.0)
            pj["mesh_size"] = pt.meshSize;
          if (pt.role == fem::ParticipantRole::Contact)
            pj["friction"] = pt.friction;
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
      if (s.solver != fem::Solver::Auto)
        sj["solver"] = fem::SolverToString(s.solver);
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
      const fem::AnalysisType typeWas = s.type;
      if (p.contains("type") &&
          !fem::AnalysisTypeFromString(p.at("type").get<std::string>(), s.type))
        throw std::runtime_error("analysis type '" +
                                 p.at("type").get<std::string>() +
                                 "' is not one of: " + fem::kAnalysisTypeNames);
      // A study made buckling takes its own default number of factors
      // unless the call gives one.
      if (s.type == fem::AnalysisType::Buckling &&
          typeWas != fem::AnalysisType::Buckling && !p.contains("num_modes"))
        s.numModes = fem::kBucklingFactorsDefault;
      if (s.type == fem::AnalysisType::Topology && host.IsAssembly())
        topoOnly();
      // Which program solves it (docs/simulation/README.md#calculix), after
      // `type`, so one call can set both; a pair that does not go together
      // is refused whichever key moved.
      if (p.contains("solver")) {
        const std::string want = p.at("solver").get<std::string>();
        if (!fem::SolverFromString(want, s.solver))
          throw std::runtime_error("solver '" + want +
                                   "' is not one of: " + fem::kSolverNames);
      }
      if (const std::string why = fem::SolverRefusal(s.solver, s.type);
          !why.empty())
        throw std::runtime_error(
            why + (p.contains("solver")
                       ? std::string()
                       : "; this study's solver is calculix, so set solver: "
                         "auto in the same call"));
      if (p.contains("converge")) {
        const fem::ConvergeMode mode = ConvergeOf(p.at("converge"));
        if (!Converges(s.type))
          throw std::runtime_error(
              std::string("converge applies to a static, buckling or "
                          "thermal study, and this one is ") +
              fem::AnalysisTypeToString(s.type) +
              "; set type: static (in this call or before it)");
        s.converge = mode;
      }
      // Assembly studies (S1): who takes part, and how. The whole list at
      // once — a role table is one statement, not a sequence of edits.
      if (p.contains("participants")) {
        const json& list = p.at("participants");
        if (!list.is_array())
          throw std::runtime_error(
              "participants is a list of {occurrence, role: "
              "meshed|bonded|contact|rigid|ignored, mesh_size?, friction?}");
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
                "' is not one of meshed, bonded, contact, rigid, ignored");
          if (pj.contains("friction")) {
            if (pt.role != fem::ParticipantRole::Contact)
              throw std::runtime_error(
                  "friction is a contact participant's (role: contact)");
            if (!pj.at("friction").is_number())
              throw std::runtime_error(
                  "friction is a number: the coefficient, unitless");
            pt.friction = pj.at("friction").get<double>();
          }
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
      // A modal study's alone: on another the save drops it, and the reader
      // refuses it in a file, so it is refused here, where it is said.
      // After `type`, so one call can make a study modal and size it.
      if (p.contains("num_modes")) {
        if (s.type != fem::AnalysisType::Modal &&
            s.type != fem::AnalysisType::Buckling)
          throw std::runtime_error(
              std::string("num_modes applies to a modal study (its "
                          "frequencies) or a buckling one (its factors), and "
                          "this one is ") +
              fem::AnalysisTypeToString(s.type) +
              "; set type: modal or buckling (in this call or before it)");
        const int n = IntParam(p.at("num_modes"), "num_modes");
        if (n < 1)
          throw std::runtime_error("num_modes is at least 1");
        s.numModes = n;
      }
      if (p.contains("gravity")) {
        s.gravity = remote::Vec3Param(p.at("gravity"), "gravity");
      }
      // Topology parameters (expression-or-literal, like mesh size). A
      // topology study's alone, as num_modes is a modal one's: on another
      // the save drops them and the reader refuses them in a file, so they
      // are refused here, after `type`.
      for (const char* k :
           {"volume_fraction", "filter_radius", "max_iterations"})
        if (p.contains(k) && s.type != fem::AnalysisType::Topology)
          throw std::runtime_error(
              std::string(k) +
              " applies to a topology study, and this one "
              "is " +
              fem::AnalysisTypeToString(s.type) +
              "; set type: topology (in this call or before it)");
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
        s.maxIterations = IntParam(p.at("max_iterations"), "max_iterations");
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
        if (!cases.empty() && s.type == fem::AnalysisType::Buckling)
          throw std::runtime_error(
              "a buckling factor scales ONE load set, so a buckling study "
              "takes no load cases: make a buckling study for each way the "
              "part is loaded");
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
      // Solved past linear (docs/simulation/README.md#nonlinear-static),
      // and checked against everything else this call left the study with.
      if (p.contains("nonlinear"))
        s.nonlinear = NonlinearOf(p.at("nonlinear"));
      if (const std::string why = fem::ContactRefusal(s); !why.empty())
        throw std::runtime_error(
            why + (p.contains("nonlinear") || !s.nonlinear.On()
                       ? std::string()
                       : "; this study is nonlinear, so set nonlinear: null "
                         "in the same call to make it linear"));
      // Thermal expansion under a temperature field
      // (docs/simulation/README.md#the-thermal-load-on-a-static-study).
      if (p.contains("thermal_load"))
        s.thermalLoad = ThermalLoadOf(p.at("thermal_load"), doc, s.id);
      if (host.IsAssembly())
        if (const std::string why = fem::AssemblyThermalRefusal(s);
            !why.empty())
          throw std::runtime_error(why);
      if (const std::string why = fem::ThermalRefusal(s); !why.empty())
        throw std::runtime_error(why);
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
    const int index = IntParam(p.at("index"), "index");
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
    // rpc-returns-from: src/remote/RemoteSimulation.cpp
    return CheckTopology(state, host, doc, p);
  }
  if (m == "export_study_inp") {
    // rpc-returns-from: src/remote/RemoteSimulation.cpp
    return ExportStudyInp(state, host, p);
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
      throw std::runtime_error("unknown load type '" +
                               p.at("type").get<std::string>() +
                               "' (accepted: " + fem::kLoadTypeNames + ")");
    // A thermal study's load (docs/simulation/README.md#thermal): its own
    // keys, each in the unit its name says, and nothing a structural load
    // takes.
    static constexpr const char* kThermalKeys[] = {
        "temperature_c", "power_w",   "flux_w_m2",
        "film_w_m2k",    "ambient_c", "body"};
    if (fem::IsThermalLoad(l.type)) {
      // rpc-params: ambient_c, film_w_m2k, flux_w_m2, power_w, temperature_c
      for (const char* key :
           {"magnitude", "direction", "torque_nmm", "force_n", "point_mm",
            "axis_origin_mm", "axis_dir", "fix_rotation", "friction", "case",
            "contact_with"})
        if (p.contains(key))
          throw std::runtime_error(
              std::string("a ") + fem::LoadTypeToString(l.type) +
              " load does not take `" + key + "`: it takes " +
              (l.type == fem::LoadType::Temperature ? "temperature_c"
               : l.type == fem::LoadType::HeatFlux  ? "power_w or flux_w_m2"
               : l.type == fem::LoadType::Convection
                   ? "film_w_m2k and ambient_c"
                   : "power_w and body"));
      if (host.IsAssembly())
        throw std::runtime_error(
            "thermal loads are a part study's: an assembly study does not "
            "solve heat yet (docs/simulation/README.md#not-done)");
      const auto number = [&](const char* key, double& into) {
        if (!p.contains(key))
          return;
        if (!p.at(key).is_number())
          throw std::runtime_error(std::string("`") + key + "` is a number");
        into = p.at(key).get<double>();
      };
      const auto only = [&](const char* key, fem::LoadType a, fem::LoadType b) {
        if (p.contains(key) && l.type != a && l.type != b)
          throw std::runtime_error(
              std::string("`") + key + "` is a " + fem::LoadTypeToString(a) +
              (a != b ? std::string(" or ") + fem::LoadTypeToString(b) : "") +
              " load's, not a " + fem::LoadTypeToString(l.type) + "'s");
      };
      using LT = fem::LoadType;
      only("temperature_c", LT::Temperature, LT::Temperature);
      only("power_w", LT::HeatFlux, LT::HeatGeneration);
      only("flux_w_m2", LT::HeatFlux, LT::HeatFlux);
      only("film_w_m2k", LT::Convection, LT::Convection);
      only("ambient_c", LT::Convection, LT::Convection);
      only("body", LT::HeatGeneration, LT::HeatGeneration);
      number("temperature_c", l.temperatureC);
      number("power_w", l.powerW);
      number("flux_w_m2", l.fluxWm2);
      number("film_w_m2k", l.filmWm2K);
      number("ambient_c", l.ambientC);
      if (l.type == LT::HeatGeneration) {
        if (p.contains("face") || p.contains("region"))
          throw std::runtime_error(
              "a heat_generation heats a whole body, named by `body`; it "
              "takes no face or region");
        if (!p.contains("body") || !p.at("body").is_string())
          throw std::runtime_error(
              "a heat_generation names the body it heats: `body`, its name");
        l.body = p.at("body").get<std::string>();
        std::string names;
        bool found = false;
        for (const Body& b : doc->GetBodies()) {
          if (b.reference)
            continue;
          names += (names.empty() ? "'" : ", '") + b.name + "'";
          found = found || b.name == l.body;
        }
        if (!found)
          throw std::runtime_error("the part has no body '" + l.body +
                                   "' (it has " + names + ")");
      } else {
        l.face = studyFaceOf(p.at("face"));
        fem::ResolvedFace rf;
        std::string why;
        if (!fem::ResolveFace(*doc, l.face, "load", rf, &why))
          throw std::runtime_error(why);
        if (p.contains("region"))
          l.region = DocumentIO::LoadRegionFromJson(p.at("region"), "add_load");
      }
      if (const std::string why = fem::ThermalLoadRefusal(l); !why.empty())
        throw std::runtime_error(why);
      fem::Study with = host.Require(id);
      with.loads.push_back(l);
      if (const std::string why = fem::ThermalRefusal(with); !why.empty())
        throw std::runtime_error(why);
      modifyStudy(id, "Add load", [&](fem::Study& s) { s.loads.push_back(l); });
      return json{{"id", l.id}};
    }
    for (const char* key : kThermalKeys)
      if (p.contains(key))
        throw std::runtime_error(std::string("`") + key +
                                 "` is a thermal load's (" +
                                 fem::kThermalLoadTypeNames + "), not a " +
                                 fem::LoadTypeToString(l.type) + "'s");
    // Each type's own keys; a key another type takes is refused rather than
    // ignored, because its unit is another unit (a torque's `magnitude`
    // read as newtons would be a thousand times off on a 1 m lever).
    const bool torque = l.type == fem::LoadType::Torque;
    const bool remote = l.type == fem::LoadType::Remote;
    const auto refuseKey = [&](const char* key, const char* instead) {
      if (p.contains(key))
        throw std::runtime_error(
            std::string("a ") + fem::LoadTypeToString(l.type) +
            " load does not take `" + key + "`" +
            (instead[0] ? std::string("; it takes `") + instead + "`"
                        : std::string()));
    };
    if (torque || remote)
      refuseKey("magnitude", torque ? "torque_nmm" : "force_n");
    if (!torque) {
      refuseKey("torque_nmm", "");
      refuseKey("axis_origin_mm", "");
      refuseKey("axis_dir", "");
    }
    if (!remote) {
      refuseKey("force_n", "");
      refuseKey("point_mm", "");
    }
    const bool pin = l.type == fem::LoadType::Pin;
    if (pin) {
      refuseKey("magnitude", "");
      refuseKey("direction", "");
      if (host.IsAssembly())
        throw std::runtime_error(
            "a pin support is a part study's; an assembly study holds its "
            "part with a fixed face or a rigid neighbour's contact");
      const fem::AnalysisType at = host.Require(id).type;
      if (!SettlesStress(at))
        throw std::runtime_error(
            std::string("a pin support is a static or buckling study's: a ") +
            fem::AnalysisTypeToString(at) +
            " study holds its part with fixed supports");
      if (p.contains("fix_rotation")) {
        if (!p.at("fix_rotation").is_boolean())
          throw std::runtime_error("fix_rotation is true or false");
        l.fixRotation = p.at("fix_rotation").get<bool>();
      }
    } else {
      refuseKey("fix_rotation", "");
    }
    // A compression-only support (docs/simulation/README.md#contact).
    const bool contact = l.type == fem::LoadType::Contact;
    if (contact) {
      refuseKey("magnitude", "");
      refuseKey("direction", "");
      refuseKey("region", "");
      if (p.contains("friction")) {
        if (!p.at("friction").is_number())
          throw std::runtime_error(
              "friction is a number: the coefficient, unitless");
        l.friction = p.at("friction").get<double>();
      }
    } else {
      refuseKey("friction", "");
    }
    if (torque && !p.contains("torque_nmm"))
      throw std::runtime_error("a torque load needs torque_nmm (N*mm, "
                               "right-handed about its axis)");
    if (remote && !p.contains("force_n"))
      throw std::runtime_error("a remote load needs force_n (N)");
    if (remote && !p.contains("point_mm"))
      throw std::runtime_error("a remote load needs point_mm, the point "
                               "[x, y, z] (mm) the force acts at");
    if (torque && p.contains("axis_origin_mm") != p.contains("axis_dir"))
      throw std::runtime_error("a torque's axis is axis_origin_mm AND "
                               "axis_dir; pass both, or neither on a "
                               "cylindrical face");
    const auto vec3 = [&](const char* key) {
      const json& v = p.at(key);
      if (!v.is_array() || v.size() != 3)
        throw std::runtime_error(std::string("`") + key + "` is [x, y, z]");
      return glm::dvec3(v.at(0).get<double>(), v.at(1).get<double>(),
                        v.at(2).get<double>());
    };
    if (torque && p.contains("axis_dir")) {
      l.axisOrigin = vec3("axis_origin_mm");
      l.axisDirection = vec3("axis_dir");
      if (!l.HasAxis())
        throw std::runtime_error("axis_dir is zero: a torque turns about a "
                                 "direction");
    }
    if (remote) {
      l.point = vec3("point_mm");
      l.hasPoint = true;
    }
    // An assembly study's anchor (S1): a face on a named occurrence, or —
    // for a fixed support — the CONTACT with a rigid occurrence, which has
    // no face to name because the patch exists in neither part.
    const Document* faceDoc = doc;
    if (p.contains("contact_with")) {
      if (l.type != fem::LoadType::Fixed && !contact)
        throw std::runtime_error(
            "only a fixed support or a contact support anchors on a contact; "
            "a force or pressure names a face (occurrence + face)");
      if (!host.IsAssembly())
        throw std::runtime_error(
            "contact_with names an occurrence of an assembly study; on a "
            "part a contact support rests on the ground under its face");
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
    // rpc-params: magnitude
    const char* magKey = torque   ? "torque_nmm"
                         : remote ? "force_n"
                                  : "magnitude";
    if (p.contains(magKey)) {
      const json& v = p.at(magKey);
      if (v.is_string())
        l.magnitudeExpr = v.get<std::string>();
      else
        l.magnitude = v.get<double>();
    }
    // A torque on a face that lends no axis is refused here, before the
    // study changes, as well as at the run (a file can carry one too).
    if (torque && !l.HasAxis() && l.contactWith == 0 &&
        !(faceDoc && fem::CylindricalFaceAxis(*faceDoc, l.face)))
      throw std::runtime_error(
          "face '" + l.face.name +
          "' is not cylindrical, so it has no axis of its own: a torque on "
          "it needs axis_origin_mm and axis_dir");
    // On the ground: the plane a flat face lies on.
    if (contact && l.contactWith == 0 && faceDoc) {
      fem::ResolvedFace rf;
      std::string why;
      if (!fem::ResolveFace(*faceDoc, l.face, "contact support", rf, &why))
        throw std::runtime_error(why);
      const auto info = Measure::DescribeFaceAt(
          faceDoc->GetBodies()[rf.bodyIndex], rf.faceIndex);
      if (!info || info->kind != "plane")
        throw std::runtime_error(
            "face '" + l.face.name +
            "' is not flat: a contact support without contact_with rests on "
            "the ground, the plane a flat face lies on; a curved face rests "
            "on a rigid occurrence in an assembly study (contact_with)");
    }
    if (pin && !(faceDoc && fem::CylindricalFaceAxis(*faceDoc, l.face)))
      throw std::runtime_error(
          "face '" + l.face.name +
          "' is not cylindrical: a pin support goes on a cylindrical face "
          "(a bore or a shaft), whose axis it holds");
    if (torque && p.contains("direction"))
      throw std::runtime_error("a torque load does not take `direction`; "
                               "it turns about axis_dir");
    if (p.contains("direction")) {
      l.direction = remote::Vec3Param(p.at("direction"), "direction");
    } else if (l.type == fem::LoadType::Force || remote) {
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
      if (fem::IsSupport(l.type))
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
    if (p.contains("region")) {
      if (host.IsAssembly())
        throw std::runtime_error(
            "region puts a load on part of a face of a PART study; on an "
            "assembly study load a whole face");
      if (host.Require(id).type == fem::AnalysisType::Topology)
        throw std::runtime_error(
            "region: a topology study freezes whole faces, so its loads "
            "are on whole faces");
      l.region = DocumentIO::LoadRegionFromJson(p.at("region"), "add_load");
    }
    // What the study would be with it: a contact refused where a nonlinear
    // study is (in-house, another analysis, load cases), a pin beside one; a
    // structural load on a thermal study.
    {
      fem::Study with = host.Require(id);
      with.loads.push_back(l);
      if (const std::string why = fem::ContactRefusal(with); !why.empty())
        throw std::runtime_error(why);
      if (const std::string why = fem::ThermalRefusal(with); !why.empty())
        throw std::runtime_error(why);
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
      if (srs.running) {
        json out{{"running", true},
                 {"job", jobId},
                 {"solver", srs.solver},
                 {"elapsed_seconds", srs.elapsedSeconds}};
        // A CalculiX run's memory estimate, once its mesh said how big
        // (docs/simulation/README.md#calculix).
        if (srs.memoryEstimateBytes > 0)
          out["memory_estimate_bytes"] = srs.memoryEstimateBytes;
        // A nonlinear run's progress (docs/simulation/README.md
        // #nonlinear-static): the load reached, from ccx's .sta.
        if (srs.loadFraction >= 0)
          out["load_fraction"] = srs.loadFraction;
        // An iterative solve's iterations so far, on the CPU or the GPU.
        out["iterations"] = srs.iterations;
        return out;
      }
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
      // A deadline bounds a CalculiX run only (an in-house solve has its
      // own cancel and no deadline), refused here before anything starts.
      if (p.contains("timeout_s")) {
        start.timeoutSeconds = p.at("timeout_s").get<double>();
        if (fem::EffectiveSolver(*s) != fem::Solver::Calculix)
          throw std::runtime_error(
              "timeout_s bounds a CalculiX run, and this study is solved "
              "in-house (set_study solver: calculix to run it on CalculiX)");
      }
      // This run's convergence check (docs/simulation/README.md
      // #a-peak-that-has-settled), read before anything runs.
      if (p.contains("converge") || p.contains("layer_normal")) {
        if (!SettlesStress(s->type))
          throw std::runtime_error(
              std::string("converge and layer_normal settle a static or "
                          "buckling study's peak; this study is ") +
              fem::AnalysisTypeToString(s->type));
        if (p.contains("converge")) {
          const fem::ConvergeMode mode = ConvergeOf(p.at("converge"));
          if (const std::string why = fem::NonlinearRefusal(
                  s->solver, s->type, s->nonlinear, s->loadCases.size(), mode,
                  fem::HasContact(*s));
              !why.empty())
            throw std::runtime_error(why);
          start.converge = static_cast<int>(mode);
        }
        if (p.contains("layer_normal")) {
          if (host.IsAssembly())
            throw std::runtime_error(
                "layer_normal is one PART's build direction, and an "
                "assembly study bonds several parts each printed its own "
                "way -- study the part, loaded as the assembly loads it");
          const glm::dvec3 n = LayerNormalOf(p.at("layer_normal"));
          start.hasLayerNormal = true;
          for (int k = 0; k < 3; ++k)
            start.layerNormal[k] = n[k];
        }
      }
      const auto started = [&](const char* kind) {
        return json{{"started", true},
                    {"running", true},
                    {"job", start.jobId},
                    {"kind", kind},
                    {"outlives_client", start.owner == 0}};
      };
      if (s->type == fem::AnalysisType::Topology) {
        // Read before the job starts, like every key: unread, it was refused
        // as unknown AFTER the optimisation had been set running.
        if (p.contains("async") && !p.at("async").get<bool>())
          throw std::runtime_error(
              "a topology study always runs off the frame (async); poll "
              "study_status for it, or leave `async` out");
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
                ? ui::RunAssemblyStudyAndCache(state, *host.asmDoc, id, &err,
                                               start.timeoutSeconds, &start)
                : ui::RunStudyAndCache(state, *doc, id, &err,
                                       start.timeoutSeconds, &start);
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
    // A printed part's layers (docs/simulation/README.md#printed-layers):
    // the build direction, in the part's frame. Read here, before any
    // answer is built, so a bad one refuses the whole call.
    std::optional<glm::dvec3> layerNormal;
    if (m == "study_results" && p.contains("layer_normal")) {
      if (!SettlesStress(s->type))
        throw std::runtime_error(std::string("layer_normal reads a static or "
                                             "buckling study's stresses; this "
                                             "study is ") +
                                 fem::AnalysisTypeToString(s->type));
      if (host.IsAssembly())
        throw std::runtime_error(
            "layer_normal is one PART's build direction, and an assembly "
            "study bonds several parts each printed its own way -- study the "
            "part, loaded as the assembly loads it");
      layerNormal = LayerNormalOf(p.at("layer_normal"));
    }
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

    // Thermal studies (docs/simulation/README.md#thermal): each body's
    // temperature range, its flux peak, the heat through each load and the
    // balance, and its margin to its material's service temperature.
    if (s->type == fem::AnalysisType::Thermal) {
      std::size_t nodes = 0, elements = 0;
      json bodies = json::array();
      const auto at = [](const glm::dvec3& v) { return json{v.x, v.y, v.z}; };
      bool first = true, haveMargin = false, bulk = false;
      json maxT, minT, maxFlux, margin;
      double maxV = 0, minV = 0, fluxV = 0, marginV = 0;
      std::vector<std::string> warnings;
      for (const fem::StudyBodyResult& b : rt->bodies) {
        nodes += b.mesh.stats.nodeCount;
        elements += b.mesh.stats.tetCount;
        const fem::StudyBodyResult::Thermal& t = b.thermal;
        json flows = json::array();
        for (const fem::HeatFlow& f : t.flows)
          flows.push_back({{"load", f.id},
                           {"type", fem::LoadTypeToString(f.type)},
                           {"watts_out", f.watts}});
        json bj = {
            {"body", b.bodyName},
            {"max_temperature_c", {{"value", t.maxC}, {"at", at(t.maxAt)}}},
            {"min_temperature_c", {{"value", t.minC}, {"at", at(t.minAt)}}},
            {"max_heat_flux_w_m2",
             {{"value", t.maxFluxWm2}, {"at", at(t.maxFluxAt)}}},
            {"flows", flows},
            {"balance",
             {{"in_w", t.inW},
              {"out_w", t.outW},
              {"imbalance_w", t.imbalanceW}}}};
        if (t.hasService) {
          bj["temperature_margin_k"] = t.marginK;
          bj["service_max_c"] = t.serviceMaxC;
          bj["service_basis"] = t.serviceBasis;
          if (t.marginK < 0)
            warnings.push_back("body '" + b.bodyName +
                               "' runs past its material's service "
                               "temperature (" +
                               t.serviceBasis + "): it softens there");
          if (!haveMargin || t.marginK < marginV) {
            haveMargin = true;
            marginV = t.marginK;
            margin = {{"value", t.marginK},
                      {"body", b.bodyName},
                      {"service_max_c", t.serviceMaxC},
                      {"basis", t.serviceBasis}};
          }
        }
        if (t.bulkConductivity) {
          bj["bulk_conductivity"] = true;
          bulk = true;
        }
        if (first || t.maxC > maxV) {
          maxV = t.maxC;
          maxT = {{"value", t.maxC}, {"at", at(t.maxAt)}, {"body", b.bodyName}};
        }
        if (first || t.minC < minV) {
          minV = t.minC;
          minT = {{"value", t.minC}, {"at", at(t.minAt)}, {"body", b.bodyName}};
        }
        if (first || t.maxFluxWm2 > fluxV) {
          fluxV = t.maxFluxWm2;
          maxFlux = {{"value", t.maxFluxWm2},
                     {"at", at(t.maxFluxAt)},
                     {"body", b.bodyName}};
        }
        first = false;
        bodies.push_back(std::move(bj));
      }
      if (bulk)
        warnings.push_back(
            "a printed material's conductivity is its bulk resin's, and a "
            "printed part conducts less (air between its roads and layers): "
            "its hot spots run hotter than predicted and its gradients "
            "steeper");
      json out{{"type", "thermal"},
               {"nodes", nodes},
               {"elements", elements},
               {"bodies", bodies},
               {"max_temperature_c", maxT},
               {"min_temperature_c", minT},
               {"max_heat_flux_w_m2", maxFlux},
               {"stale", host.Stale(id)},
               {"convergence", ConvergenceJson(rt->convergence)}};
      if (haveMargin)
        out["temperature_margin_k"] = margin;
      if (!rt->solver.empty())
        out["solver"] = rt->solver;
      const bool onCalculix = rt->solver.rfind("calculix", 0) == 0;
      if (onCalculix && rt->residual > 0)
        out["calculix"] = {{"residual", rt->residual},
                           {"residual_floor", rt->residualFloor},
                           {"energy_residual", rt->energyResidual}};
      out["notes"] = json::array(
          {"steady conduction with each material's room-temperature "
           "conductivity; heat flux peaks at a re-entrant edge, which "
           "refining never settles, so it is reported and not chased"});
      if (!warnings.empty())
        out["warnings"] = warnings;
      receipt::Receipt r;
      const receipt::Oracle solver =
          onCalculix ? receipt::Oracle{"calculix/heat",
                                       rt->solver.substr(
                                           std::string("calculix ").size())}
                     : receipt::Oracle{"in_house/heat",
                                       rt->solver.empty() ? "cpu" : rt->solver};
      if (host.Stale(id)) {
        r.Unverifiable("the results describe the current geometry", solver,
                       "the part changed after this study was solved — these "
                       "numbers are the previous geometry's. Re-run the study "
                       "before believing them");
      } else if (!haveMargin) {
        r.Unverifiable("peak temperature below the material's service "
                       "temperature",
                       solver,
                       "no body's material has a service temperature in the "
                       "library (the printed plastics carry their heat "
                       "deflection temperature)");
      } else {
        receipt::Claim c(
            "peak temperature below the material's service temperature", solver,
            marginV >= 0.0 ? receipt::Verdict::Pass : receipt::Verdict::Fail);
        c.basis = receipt::Basis::Predicted;
        c.value = marginV;
        c.unit = "K";
        c.note = "steady conduction on " + std::to_string(elements) +
                 " elements; the margin is against " +
                 margin.at("basis").get<std::string>();
        const std::string unsettled = NotConvergedWhy(rt->convergence);
        if (!unsettled.empty())
          c.note += ". Its peak is NOT converged (" + unsettled + ")";
        for (const std::string& w : warnings)
          c.note += ". " + w;
        r.Add(std::move(c));
      }
      AttachReceipt(out, r);
      return stamp(out);
    }

    // Buckling studies (docs/simulation/README.md#buckling): each body's
    // factors, the corrections that make the governing one, and the flags.
    if (s->type == fem::AnalysisType::Buckling) {
      std::size_t nodes = 0, elements = 0;
      json bodies = json::array();
      bool any = false, anyBuckles = false;
      double governing = 0.0, buckling = 0.0;
      std::string governingBody, bucklingBody, whyNone;
      std::vector<std::string> warnings;
      bool anyPin = false;
      for (const fem::Load& l : s->loads)
        anyPin = anyPin || l.type == fem::LoadType::Pin;
      const auto orNull = [](double v) { return v > 0 ? json(v) : json(); };
      for (const fem::StudyBodyResult& b : rt->bodies) {
        nodes += b.mesh.stats.nodeCount;
        elements += b.mesh.stats.tetCount;
        const fem::StudyBodyResult::Buckling& k = b.buckling;
        const auto& mp = CADMaterials::MaterialLibrary::GetMaterial(
            CADMaterials::MaterialLibrary::MaterialTypeFromString(
                host.BodyMaterial(*s, b.bodyIndex)));
        json factors = json::array();
        for (const auto& mode : b.results.modes)
          factors.push_back(mode.factor);
        // The stress across the layers, when the build direction is given;
        // the largest principal stress bounds it otherwise.
        double interlayer = k.interlayerFactor;
        std::string interlayerFrom = "max_principal";
        if (layerNormal && mp.interlayerMPa > 0 && !b.results.stress.empty()) {
          const fem::FieldPeak lk = fem::PeakOf(
              b.mesh, fem::AcrossLayersField(b.results, *layerNormal));
          interlayer =
              lk.node >= 0 && lk.value > 0 ? mp.interlayerMPa / lk.value : 0.0;
          interlayerFrom = "across_layers";
        }
        double plastic = k.yieldFactor;
        if (interlayer > 0 && (plastic == 0 || interlayer < plastic))
          plastic = interlayer;
        const double elastic = k.buckles ? k.knockdown * k.factor : 0.0;
        const double gov = elastic > 0 && plastic > 0
                               ? 1.0 / (1.0 / elastic + 1.0 / plastic)
                               : (elastic > 0 ? elastic : plastic);
        json bj = {
            {"body", b.bodyName},
            {"buckling_factors", factors},
            {"buckling_factor", k.buckles ? json(k.factor) : json()},
            {"knockdown", {{"value", k.knockdown}, {"class", k.shapeClass}}},
            {"yield_factor", orNull(k.yieldFactor)},
            {"interlayer_factor", orNull(interlayer)},
            {"governing_factor", orNull(gov)},
            {"base_state",
             {{"max_von_mises_mpa", b.results.maxVonMises},
              {"max_displacement_mm", b.results.maxDisplacement}}}};
        if (interlayer > 0)
          bj["interlayer_from"] = interlayerFrom;
        if (k.face >= 0)
          bj["knockdown"]["face"] = k.face;
        if (k.radiusMm > 0)
          bj["knockdown"]["radius_mm"] = k.radiusMm;
        if (k.wallMm > 0)
          bj["knockdown"]["wall_mm"] = k.wallMm;
        if (!k.buckles)
          bj["why_none"] = k.none;
        if (k.atLoadEdge) {
          bj["mode_at_load_edge"] = {
              {"load", k.edgeLoad}, {"type", k.edgeType}, {"face", k.edgeFace}};
          warnings.push_back(
              "body '" + b.bodyName +
              "': the first mode is local where a load enters (face " +
              std::to_string(k.edgeFace) +
              "): crippling at the load's edge, not the member buckling");
        }
        if (k.wallLayers > 0)
          bj["wall_layers"] = k.wallLayers;
        if (k.wallLayers == 1) {
          char wall[32];
          std::snprintf(wall, sizeof(wall), "%.3g", k.wallMm);
          bj["through_thickness"] =
              "the first mode bends a wall with one element through it";
          warnings.push_back(
              "body '" + b.bodyName + "': the first mode bends a " + wall +
              " mm wall (face " + std::to_string(k.face) +
              ") with one element through it, which reads a little stiff "
              "(within 1.3 % of two on a 1 mm plate); add_refinement on that "
              "face at half the wall confirms it");
        }
        if (!k.buckles && whyNone.empty())
          whyNone = k.none;
        if (k.buckles && (!anyBuckles || k.factor < buckling)) {
          buckling = k.factor;
          bucklingBody = b.bodyName;
          anyBuckles = true;
        }
        if (gov > 0 && (!any || gov < governing)) {
          governing = gov;
          governingBody = b.bodyName;
          any = true;
        }
        bodies.push_back(std::move(bj));
      }
      json out{{"type", "buckling"},
               {"nodes", nodes},
               {"elements", elements},
               {"bodies", bodies},
               {"buckling_factor", anyBuckles ? json(buckling) : json()},
               {"governing_factor", any ? json(governing) : json()},
               {"stale", host.Stale(id)},
               {"convergence", ConvergenceJson(rt->convergence)}};
      if (!anyBuckles)
        out["why_none"] = whyNone;
      // What a factor of safety reads on a buckling study: the governing
      // factor, buckling and yield together.
      if (any)
        out["factor_of_safety"] = governing;
      if (!rt->solver.empty())
        out["solver"] = rt->solver;
      const bool onCalculix = rt->solver.rfind("calculix", 0) == 0;
      if (onCalculix && rt->residual > 0)
        out["calculix"] = {{"residual", rt->residual},
                           {"residual_floor", rt->residualFloor},
                           {"energy_residual", rt->energyResidual}};
      json notes = json::array();
      notes.push_back("every load scales together by the factor" +
                      std::string(glm::length(s->gravity) > 0
                                      ? ", gravity with the rest"
                                      : ""));
      if (anyPin)
        notes.push_back("each pin is held where the base state presses on "
                        "it, at every factor: a linear factor scales the "
                        "load without turning it");
      out["notes"] = std::move(notes);
      if (!warnings.empty())
        out["warnings"] = warnings;
      if (!roles.empty())
        out["participants"] = roles;
      receipt::Receipt r;
      const receipt::Oracle solver =
          onCalculix ? receipt::Oracle{"calculix/buckle",
                                       rt->solver.substr(
                                           std::string("calculix ").size())}
                     : receipt::Oracle{"in_house/buckle",
                                       rt->solver.empty() ? "cpu" : rt->solver};
      if (host.Stale(id)) {
        r.Unverifiable("the results describe the current geometry", solver,
                       "the part changed after this study was solved — these "
                       "numbers are the previous geometry's. Re-run the study "
                       "before believing them");
      } else if (!any) {
        r.Unverifiable("governing buckling factor", solver,
                       whyNone.empty()
                           ? std::string("no factor was found")
                           : whyNone + ", and no material has a yield "
                                       "strength to bound it");
      } else {
        receipt::Claim c("governing buckling factor >= 1", solver,
                         governing >= 1.0 ? receipt::Verdict::Pass
                                          : receipt::Verdict::Fail);
        c.basis = receipt::Basis::Predicted;
        c.value = governing;
        c.unit = "x";
        c.note = "linear buckling on " + std::to_string(elements) +
                 " elements: an upper bound, which the imperfection "
                 "knockdown and the yield interaction (Rankine-Gordon) "
                 "correct for. Snap-through -- an arch or a curved wall "
                 "loaded across its curve -- needs a nonlinear run, which "
                 "linear buckling does not see: a static study with "
                 "nonlinear {large_deformation: true, snap_through: true} "
                 "reports its peak load factor";
        if (onCalculix)
          c.note += ". Solved by CalculiX " + solver.version +
                    " on this study's own mesh and loads: a second, "
                    "independent answer, not a stronger one";
        const std::string unsettled = NotConvergedWhy(rt->convergence);
        if (!unsettled.empty())
          c.note +=
              ". Its base state's peak is NOT converged (" + unsettled + ")";
        for (const std::string& w : warnings)
          c.note += ". " + w;
        r.Add(std::move(c));
      }
      AttachReceipt(out, r);
      return stamp(out);
    }

    // Aggregate mesh size and a factor of safety (min over bodies with yield
    // data) from the governing von Mises.
    std::size_t nodes = 0, elements = 0;
    double minFoS = 0.0;
    bool haveFoS = false;
    fem::FieldPeak principal, across;
    std::string principalBody, acrossBody;
    bool missingStress = false, anyInterlayer = false, haveIlFoS = false;
    double minIlFoS = 0.0;
    // What each support carried, and a peak that sits on a support's edge
    // (docs/simulation/README.md#a-peak-on-a-supports-edge).
    const auto reactionsJson = [](const fem::FemResults& r) {
      json out = json::array();
      for (const fem::FemResults::SupportReaction& sr : r.reactions) {
        json rj = {{"support", sr.support},
                   {"type", fem::LoadTypeToString(sr.type)},
                   {"force_n", {sr.forceN.x, sr.forceN.y, sr.forceN.z}}};
        if (sr.hasMoment)
          rj["moment_n_mm"] = {sr.momentNmm.x, sr.momentNmm.y, sr.momentNmm.z};
        if (sr.type == fem::LoadType::Pin ||
            sr.type == fem::LoadType::Contact) {
          rj["contact_area_mm2"] = sr.contactAreaMm2;
          rj["face_area_mm2"] = sr.faceAreaMm2;
        }
        out.push_back(std::move(rj));
      }
      return out;
    };
    const auto edgeJson = [](const fem::FemResults& r, double yield) {
      const fem::FemResults::SupportEdgePeak& e = r.supportEdgePeak;
      json j = {{"support", e.support},
                {"type", fem::LoadTypeToString(e.type)},
                {"max_von_mises_away_mpa", e.awayMPa},
                {"away_at", {e.awayAt.x, e.awayAt.y, e.awayAt.z}}};
      if (yield > 0 && e.awayMPa > 0)
        j["factor_of_safety_away"] = yield / e.awayMPa;
      return j;
    };
    // The stress across the layers read away from a support's edge, as von
    // Mises is (docs/simulation/README.md#a-peak-on-a-supports-edge): it
    // joins a body's (or the top level's) peak_at_support_edge, and
    // factor_of_safety_away is then the governing one of the two.
    struct AcrossAway {
      bool any = false;
      double peak = 0.0;
      double away = 0.0;
      glm::dvec3 at{0.0};
      fem::FemResults::SupportEdgePeak edge;
      double interlayer = 0.0;
    };
    const auto withAcross = [](json edge, const fem::FemResults& r,
                               double yield, const AcrossAway& a) {
      if (!a.any)
        return edge;
      if (edge.is_null()) {
        if (!a.edge.flagged)
          return edge;
        // Only the across-layers peak sits on an edge; von Mises does not.
        edge = {{"support", a.edge.support},
                {"type", fem::LoadTypeToString(a.edge.type)},
                {"max_von_mises_away_mpa", r.maxVonMises},
                {"away_at", {r.maxVmAt.x, r.maxVmAt.y, r.maxVmAt.z}}};
      }
      edge["max_across_layers_away_mpa"] = a.away;
      edge["across_layers_away_at"] = {a.at.x, a.at.y, a.at.z};
      const double vm = edge.value("max_von_mises_away_mpa", 0.0);
      double fos = 0.0;
      std::string governs;
      if (yield > 0 && vm > 0) {
        fos = yield / vm;
        governs = "von_mises";
      }
      if (a.interlayer > 0 && a.away > 0 &&
          (governs.empty() || a.interlayer / a.away < fos)) {
        fos = a.interlayer / a.away;
        governs = "across_layers";
      }
      if (!governs.empty()) {
        edge["factor_of_safety_away"] = fos;
        edge["governing"] = governs;
      }
      return edge;
    };
    AcrossAway topAcross;
    // The flag that goes with the top-level peak: the body and case it is in.
    json topEdge;
    double topEdgeVm = -1.0;
    const fem::FemResults* topEdgeResults = nullptr;
    double topEdgeYield = 0.0;
    json perBody = json::array();
    bool plasticPeak = false, residualPeak = false;
    double plasticMax = 0.0, residualMax = 0.0;
    glm::dvec3 plasticAt(0.0), residualAt(0.0);
    std::string plasticBody, residualBody;
    for (const fem::StudyBodyResult& b : rt->bodies) {
      nodes += b.mesh.stats.nodeCount;
      elements += b.mesh.stats.tetCount;
      json bj = {{"body", b.bodyName},
                 {"max_von_mises_mpa", b.results.maxVonMises},
                 {"max_displacement_mm", b.results.maxDisplacement}};
      const auto& mp = CADMaterials::MaterialLibrary::GetMaterial(
          CADMaterials::MaterialLibrary::MaterialTypeFromString(
              host.BodyMaterial(*s, b.bodyIndex)));
      if (!b.results.reactions.empty())
        bj["reactions"] = reactionsJson(b.results);
      if (b.nonlinear.solved)
        bj["nonlinear"] = NonlinearBodyJson(b.nonlinear, mp.yieldMPa);
      // A plastic body: its peak plastic strain, and what unloading left.
      if (!b.results.plasticStrain.empty()) {
        bj["max_plastic_strain"] = b.results.maxPlasticStrain;
        if (!plasticPeak || b.results.maxPlasticStrain > plasticMax) {
          plasticPeak = true;
          plasticMax = b.results.maxPlasticStrain;
          plasticAt = b.results.maxPeAt;
          plasticBody = b.bodyName;
        }
      }
      if (!b.results.residualDisplacement.empty()) {
        bj["max_residual_displacement_mm"] = b.results.maxResidualDisplacement;
        if (!residualPeak || b.results.maxResidualDisplacement > residualMax) {
          residualPeak = true;
          residualMax = b.results.maxResidualDisplacement;
          residualAt = b.results.maxResidualAt;
          residualBody = b.bodyName;
        }
      }
      // Each contact at the factor reported (docs/simulation/README.md
      // #contact): its pressure, and how much of its faces it closed.
      if (!b.contacts.empty()) {
        json cs = json::array();
        for (const fem::StudyBodyResult::ContactResult& c : b.contacts)
          cs.push_back(
              {{c.participant ? "occurrence" : "support", c.id},
               {"kind", c.participant ? "participant" : "support"},
               {"name", c.name},
               {"friction", c.friction},
               {"penalty_mpa_per_mm", c.penaltyMPaPerMm},
               {"area_mm2", c.areaMm2},
               {"closed_area_mm2", c.closedMm2},
               {"open_area_mm2", std::max(0.0, c.areaMm2 - c.closedMm2)},
               {"force_n", {c.forceN.x, c.forceN.y, c.forceN.z}},
               {"moment_n_mm", {c.momentNmm.x, c.momentNmm.y, c.momentNmm.z}},
               {"normal_force_n", c.normalN},
               {"shear_force_n", c.shearN},
               {"max_pressure_mpa", c.maxPressureMPa},
               {"mean_pressure_mpa", c.meanPressureMPa}});
        bj["contacts"] = std::move(cs);
      }
      for (size_t k = 0; k < std::max<size_t>(1, b.cases.size()); ++k) {
        const fem::FemResults& r = b.Case(static_cast<int>(k));
        if (r.maxVonMises > topEdgeVm) {
          topEdgeVm = r.maxVonMises;
          topEdgeResults = &r;
          topEdgeYield = mp.yieldMPa;
          topEdge =
              r.supportEdgePeak.flagged ? edgeJson(r, mp.yieldMPa) : json();
        }
      }
      AcrossAway bodyAcross;
      // Factor of safety against yield, when the body's material defines one.
      if (b.results.maxVonMises > 0) {
        const double yield = mp.yieldMPa;
        if (yield > 0) {
          const double fos = yield / b.results.maxVonMises;
          bj["factor_of_safety"] = fos;
          if (!haveFoS || fos < minFoS)
            minFoS = fos;
          haveFoS = true;
        }
      }
      // The largest tensile principal stress and the largest tension across
      // the layers, over every case, on the nodes the result keeps.
      for (size_t k = 0; k < std::max<size_t>(1, b.cases.size()); ++k) {
        const fem::FemResults& r = b.Case(static_cast<int>(k));
        if (r.stress.empty()) {
          missingStress = true;
          continue;
        }
        const fem::FieldPeak pk =
            fem::PeakOf(b.mesh, fem::MaxPrincipalField(r));
        if (pk.node >= 0 &&
            (principal.node < 0 || pk.value > principal.value)) {
          principal = pk;
          principalBody = b.bodyName;
        }
        if (pk.node >= 0 && (!bj.contains("max_principal_mpa") ||
                             pk.value > bj["max_principal_mpa"].get<double>()))
          bj["max_principal_mpa"] = pk.value;
        if (!layerNormal)
          continue;
        const std::vector<double> acrossField =
            fem::AcrossLayersField(r, *layerNormal);
        const fem::FieldPeak lk = fem::PeakOf(b.mesh, acrossField);
        if (lk.node < 0)
          continue;
        {
          AcrossAway a;
          a.any = true;
          a.peak = lk.value;
          a.interlayer = mp.interlayerMPa;
          a.edge = fem::PeakAwayFromEdges(b.mesh, acrossField, lk.node,
                                          b.supportEdgeZone, b.edgeOwners);
          a.away = a.edge.flagged ? a.edge.awayMPa : lk.value;
          a.at = a.edge.flagged ? a.edge.awayAt : lk.at;
          if (!bodyAcross.any || a.away > bodyAcross.away)
            bodyAcross = a;
          if (!topAcross.any || a.peak > topAcross.peak)
            topAcross = a;
        }
        if (across.node < 0 || lk.value > across.value) {
          across = lk;
          acrossBody = b.bodyName;
        }
        if (!bj.contains("max_across_layers_mpa") ||
            lk.value > bj["max_across_layers_mpa"].get<double>())
          bj["max_across_layers_mpa"] = lk.value;
      }
      {
        json edge = b.results.supportEdgePeak.flagged
                        ? edgeJson(b.results, mp.yieldMPa)
                        : json();
        edge = withAcross(std::move(edge), b.results, mp.yieldMPa, bodyAcross);
        if (!edge.is_null())
          bj["peak_at_support_edge"] = std::move(edge);
      }
      if (layerNormal && mp.interlayerMPa > 0) {
        anyInterlayer = true;
        const double v = bj.value("max_across_layers_mpa", 0.0);
        if (v > 0) {
          const double fos = mp.interlayerMPa / v;
          bj["interlayer_factor_of_safety"] = fos;
          if (!haveIlFoS || fos < minIlFoS)
            minIlFoS = fos;
          haveIlFoS = true;
        }
      }
      perBody.push_back(std::move(bj));
    }
    if (layerNormal && missingStress)
      throw std::runtime_error(
          "this result was saved without its stress tensor (before format "
          "v172), so the stress across layers cannot be read from it -- "
          "run_study with recompute: true");
    if (layerNormal && !anyInterlayer)
      throw std::runtime_error(
          "layer_normal asks how hard a PRINTED part's layers are pulled "
          "apart, and no body in this study is of a material with an "
          "interlayer strength (pla, petg)");

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
    if (plasticPeak)
      result["max_plastic_strain"] = {
          {"value", plasticMax},
          {"at", {plasticAt.x, plasticAt.y, plasticAt.z}},
          {"body", plasticBody}};
    if (residualPeak)
      result["max_residual_displacement_mm"] = {
          {"value", residualMax},
          {"at", {residualAt.x, residualAt.y, residualAt.z}},
          {"body", residualBody}};
    if (topEdgeResults != nullptr)
      topEdge = withAcross(std::move(topEdge), *topEdgeResults, topEdgeYield,
                           topAcross);
    if (!topEdge.is_null())
      result["peak_at_support_edge"] = topEdge;
    // Which program solved it (docs/simulation/README.md#calculix). A
    // CalculiX answer says how well it holds against our own K, and, when
    // the session has an in-house answer to the same study, how far the two
    // are apart: reported beside it, never folded into its receipt.
    const bool onCalculix = rt->solver.rfind("calculix", 0) == 0;
    if (!rt->solver.empty())
      result["solver"] = rt->solver;
    if (onCalculix && rt->residual > 0)
      result["calculix"] = {{"residual", rt->residual},
                            {"residual_floor", rt->residualFloor},
                            {"energy_residual", rt->energyResidual}};
    if (onCalculix && !fem::IsNonlinear(*s)) {
      fem::Study twin = *s;
      twin.solver = fem::Solver::Auto;
      const uint64_t twinHash =
          host.IsAssembly()
              ? fem::AssemblyStudyInputKey(*host.asmDoc, twin,
                                           ui::AssemblyStudyContextFor(state))
              : fem::StudyInputKey(*doc, twin);
      if (const auto own = twinHash != 0
                               ? state.studyResultsByInput.Find(twinHash)
                               : nullptr) {
        const auto rel = [](double a, double b) {
          const double m = std::max(std::abs(a), std::abs(b));
          return m > 0 ? std::abs(a - b) / m : 0.0;
        };
        // Agreement is a same-mesh comparison. A converging run refines
        // until its peak settles or its time runs out, so the two solvers
        // can end on different meshes; their difference is then the mesh's,
        // and is not reported as agreement.
        const auto nodes = [](const fem::StudyRuntime& r) {
          size_t n = 0;
          for (const auto& b : r.bodies)
            n += b.mesh.nodes.size();
          return n;
        };
        const size_t ours = nodes(*own), theirs = nodes(*rt);
        if (ours == theirs)
          result["in_house_agreement"] = {
              {"input_hash", inputhash::Hex(twinHash)},
              {"max_displacement_rel",
               rel(rt->maxDisplacement, own->maxDisplacement)},
              {"max_von_mises_rel", rel(rt->maxVonMises, own->maxVonMises)}};
        else
          result["in_house_agreement"] = {
              {"input_hash", inputhash::Hex(twinHash)},
              {"not_compared",
               "the two runs ended on different meshes (" +
                   std::to_string(ours) + " nodes in-house, " +
                   std::to_string(theirs) +
                   " on CalculiX): each refined until its peak settled or "
                   "its time ran out"}};
      }
    }
    // Whether the peak has settled (docs/simulation/README.md
    // #a-peak-that-has-settled).
    result["convergence"] = ConvergenceJson(rt->convergence);
    // Solved past linear (docs/simulation/README.md#nonlinear-static): how
    // the run went, and on a snap-through run the peak, which is then what
    // a factor of safety reads -- the first yield on the way, if earlier.
    std::vector<std::string> nlWarnings;
    bool haveCollapse = false;
    double collapse = 0.0, collapseHigh = 0.0;
    std::string collapseBody;
    bool snapRun = false, havePeak = false;
    double peak = 0.0, governing = 0.0;
    std::string peakBody, peakAt, whyNoPeak;
    for (const fem::StudyBodyResult& b : rt->bodies) {
      const fem::StudyBodyResult::Nonlinear& n = b.nonlinear;
      if (!n.solved)
        continue;
      if (n.snapped)
        nlWarnings.push_back(
            "body '" + b.bodyName + "' SNAPPED THROUGH between factor " +
            std::to_string(n.snappedFrom) + " and " +
            std::to_string(n.snappedTo) +
            " of the load: the fields are of the branch it landed on, and a "
            "snap-through run (nonlinear snap_through: true) reports the peak");
      if (n.firstYieldFactor > 0 && !n.plasticity)
        nlWarnings.push_back("body '" + b.bodyName + "' reaches yield at " +
                             std::to_string(n.firstYieldFactor) +
                             " of the load, past which this elastic answer "
                             "does not hold");
      if (n.plasticity && n.collapsed) {
        nlWarnings.push_back(
            "body '" + b.bodyName + "' COLLAPSED at " +
            std::to_string(n.factor) +
            " of the load: the solve stopped converging there as its "
            "stiffness ran out, and the fields are of the last increment "
            "before it");
        if (!haveCollapse || n.factor < collapse) {
          haveCollapse = true;
          collapse = n.factor;
          collapseHigh = n.collapseHigh;
          collapseBody = b.bodyName;
        }
      }
      if (n.plasticity && n.strainLimit > 0 &&
          b.results.maxPlasticStrain > n.strainLimit)
        nlWarnings.push_back(
            "body '" + b.bodyName + "' reaches a plastic strain of " +
            std::to_string(b.results.maxPlasticStrain) +
            ", past the end of its material's hardening data (" +
            std::to_string(n.strainLimit) +
            "), where CalculiX holds the stress at the curve's last point: "
            "past it the answer is the data's extrapolation, not the "
            "material's");
      if (!n.snapThrough)
        continue;
      snapRun = true;
      if (n.peakAt == "none") {
        if (whyNoPeak.empty())
          whyNoPeak = n.none;
        continue;
      }
      double g = n.peakFactor;
      if (n.firstYieldFactor > 0 && n.firstYieldFactor < g)
        g = n.firstYieldFactor;
      if (!havePeak || g < governing) {
        governing = g;
        peak = n.peakFactor;
        peakBody = b.bodyName;
        peakAt = n.peakAt;
        havePeak = true;
      }
    }
    if (snapRun) {
      json nj = {{"kind", "snap_through"},
                 {"peak_factor", havePeak ? json(peak) : json()},
                 {"governing_factor", havePeak ? json(governing) : json()}};
      if (havePeak) {
        nj["peak_at"] = peakAt;
        nj["body"] = peakBody;
        result["factor_of_safety"] = governing;
      } else {
        nj["why_none"] = whyNoPeak;
        result.erase("factor_of_safety");
      }
      result["nonlinear"] = std::move(nj);
    } else if (s->nonlinear.plasticity) {
      json nj = {{"kind", "plasticity"}};
      if (haveCollapse) {
        nj["collapse_factor"] = collapse;
        nj["collapse_bracket"] = {collapse, collapseHigh};
        nj["body"] = collapseBody;
        result["factor_of_safety"] = collapse;
      }
      result["nonlinear"] = std::move(nj);
    } else if (s->nonlinear.On()) {
      result["nonlinear"] = {{"kind", "large_deformation"}};
    } else if (fem::HasContact(*s)) {
      result["nonlinear"] = {{"kind", "contact"}};
    }
    if (fem::IsNonlinear(*s)) {
      json notes = json::array();
      if (s->nonlinear.plasticity)
        notes.push_back(
            "plasticity: each material past its yield along its library "
            "hardening curve (isotropic, von Mises), the stress and the "
            "equivalent plastic strain CalculiX's own at each Gauss point, "
            "carried to the nodes; the load is then taken off again, and "
            "what it leaves is the residual displacement. A run that stops "
            "converging as its stiffness runs out has collapsed there (load "
            "control cannot pass a peak)");
      if (s->nonlinear.largeDeformation) {
        notes.push_back(
            "large deformation (NLGEOM): the load is stepped up and each "
            "increment is solved on the part as it has deformed; the stress "
            "is the Cauchy stress of the deformed part");
        notes.push_back("a force keeps its direction (a dead load); a "
                        "pressure follows its face as it turns; a torque or "
                        "remote load is carried from a reference node by its "
                        "face (*COUPLING, *DISTRIBUTING)");
      }
      if (fem::HasContact(*s))
        notes.push_back(
            "contact: face-to-face penalty contact on CalculiX, pressed and "
            "never pulled, the load stepped up as each contact closes or "
            "opens; a part held by a contact alone is held by weak springs "
            "while it closes (their share of the load is `stabilization`); "
            "each contact's closed area, pressure and force are per body "
            "under `contacts`");
      for (const fem::Load& l : s->loads)
        if (l.type == fem::LoadType::Pin) {
          notes.push_back("each pin is held where the linear solve of the "
                          "same load presses on it, at every increment");
          break;
        }
      if (snapRun)
        notes.push_back(
            "a snap-through run seeds the shape with the first linear "
            "buckling mode and steps every load up together until the solve "
            "stops converging as the stiffness runs out, an increment jumps "
            "to another branch, or the motion passes the deflection limit; "
            "CalculiX has no arc-length method, so the path past a peak is "
            "not followed");
      result["notes"] = std::move(notes);
      if (!nlWarnings.empty())
        result["warnings"] = nlWarnings;
    }
    // A thermal load (docs/simulation/README.md
    // #the-thermal-load-on-a-static-study): the temperature range it put on
    // the part, and each body that runs past its service temperature, where
    // the room-temperature stiffness and strength it was solved with do not
    // hold.
    if (s->thermalLoad.On()) {
      bool firstT = true;
      double lo = 0, hi = 0, t0 = 0;
      for (const fem::StudyBodyResult& b : rt->bodies) {
        if (!b.thermal.load)
          continue;
        if (firstT || b.thermal.minC < lo)
          lo = b.thermal.minC;
        if (firstT || b.thermal.maxC > hi)
          hi = b.thermal.maxC;
        t0 = b.thermal.stressFreeC;
        firstT = false;
        if (b.thermal.hasService && b.thermal.marginK < 0)
          nlWarnings.push_back(
              "body '" + b.bodyName + "' reaches " +
              std::to_string(b.thermal.maxC) +
              " C, past its material's service temperature (" +
              b.thermal.serviceBasis +
              "): the stiffness and strength it was solved with are room "
              "temperature's and do not hold there");
      }
      json t = {{"min_c", lo}, {"max_c", hi}, {"stress_free_c", t0}};
      if (s->thermalLoad.kind == fem::ThermalLoad::Kind::FromStudy)
        t["from_study"] = s->thermalLoad.fromStudy;
      result["temperature"] = std::move(t);
      if (!nlWarnings.empty())
        result["warnings"] = nlWarnings;
    }
    const auto peakJson = [](const fem::FieldPeak& pk, const std::string& b) {
      return json{{"value", pk.value},
                  {"at", {pk.at.x, pk.at.y, pk.at.z}},
                  {"body", b}};
    };
    if (principal.node >= 0)
      result["max_principal_mpa"] = peakJson(principal, principalBody);
    if (layerNormal) {
      result["layer_normal"] = {layerNormal->x, layerNormal->y, layerNormal->z};
      if (across.node >= 0)
        result["max_across_layers_mpa"] = peakJson(across, acrossBody);
      if (haveIlFoS)
        result["interlayer_factor_of_safety"] = minIlFoS;
    }
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
        json caseEdge, caseReactions = json::array();
        for (const fem::StudyBodyResult& b : rt->bodies) {
          const fem::FemResults& r = b.Case(static_cast<int>(k));
          for (json& rj : reactionsJson(r)) {
            rj["body"] = b.bodyName;
            caseReactions.push_back(std::move(rj));
          }
          if (r.maxVonMises > vm) {
            vm = r.maxVonMises;
            vmAt = r.maxVmAt;
            caseEdge =
                r.supportEdgePeak.flagged
                    ? edgeJson(r,
                               CADMaterials::MaterialLibrary::GetMaterial(
                                   CADMaterials::MaterialLibrary::
                                       MaterialTypeFromString(
                                           host.BodyMaterial(*s, b.bodyIndex)))
                                   .yieldMPa)
                    : json();
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
        if (!caseEdge.is_null())
          cj["peak_at_support_edge"] = std::move(caseEdge);
        if (!caseReactions.empty())
          cj["reactions"] = std::move(caseReactions);
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
      // A CalculiX answer names its own oracle and version: a second,
      // independent answer, held to the same basis as an in-house one.
      const receipt::Oracle solver =
          onCalculix ? receipt::Oracle{"calculix/static",
                                       rt->solver.substr(
                                           std::string("calculix ").size())}
                     : receipt::Oracle{"designer/fem-static",
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
      } else if (snapRun) {
        const receipt::Oracle nlOracle{"calculix/nonlinear", solver.version};
        if (!havePeak) {
          r.Unverifiable("peak load factor", nlOracle,
                         whyNoPeak.empty() ? std::string("no peak was found")
                                           : whyNoPeak);
        } else {
          receipt::Claim c("peak load factor >= 1", nlOracle,
                           governing >= 1.0 ? receipt::Verdict::Pass
                                            : receipt::Verdict::Fail);
          c.basis = receipt::Basis::Predicted;
          c.value = governing;
          c.unit = "x";
          c.note = "snap-through run on CalculiX " + solver.version +
                   " (large deformation, the shape seeded with its first "
                   "buckling mode) on " +
                   std::to_string(elements) +
                   " elements: the load steps up "
                   "until the part stops carrying more (" +
                   peakAt + "). Elastic: the first yield on the way bounds it";
          for (const std::string& w : nlWarnings)
            c.note += ". " + w;
          r.Add(std::move(c));
        }
      } else if (haveCollapse) {
        const receipt::Oracle nlOracle{"calculix/nonlinear", solver.version};
        receipt::Claim c("collapse load factor >= 1", nlOracle,
                         collapse >= 1.0 ? receipt::Verdict::Pass
                                         : receipt::Verdict::Fail);
        c.basis = receipt::Basis::Predicted;
        c.value = collapse;
        c.unit = "x";
        c.note = "elastic-plastic run on CalculiX " + solver.version + " on " +
                 std::to_string(elements) +
                 " elements, each material along its library hardening "
                 "curve: the solve stopped converging as body '" +
                 collapseBody + "' ran out of stiffness, between factor " +
                 std::to_string(collapse) + " and " +
                 std::to_string(collapseHigh) +
                 " (load control cannot pass a peak)";
        for (const std::string& w : nlWarnings)
          c.note += ". " + w;
        r.Add(std::move(c));
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
        c.note = std::string(s->nonlinear.plasticity
                                 ? "elastic-plastic static solve (past yield "
                                   "the stress follows the hardening curve, "
                                   "so this reads how far past yield the peak "
                                   "is; max_plastic_strain says how much it "
                                   "yielded)"
                             : s->nonlinear.On()
                                 ? "large-deformation (NLGEOM) static solve"
                             : fem::HasContact(*s)
                                 ? "contact (nonlinear) static solve"
                                 : "linear-elastic static solve") +
                 " on " + std::to_string(elements) +
                 " elements, against the library yield strength. A prediction,"
                 " not a measurement";
        for (const std::string& w : nlWarnings)
          c.note += ". " + w;
        if (onCalculix)
          c.note += ". Solved by CalculiX " + solver.version +
                    " on this study's own mesh and loads: a second, "
                    "independent answer, not a stronger one";
        if (!topEdge.is_null())
          c.note +=
              ". The peak sits on support " +
              std::to_string(topEdge["support"].get<uint64_t>()) +
              "'s edge, where a clamp's stress grows with the mesh; "
              "away from every support's edge the largest is " +
              std::to_string(topEdge["max_von_mises_away_mpa"].get<double>()) +
              " MPa (peak_at_support_edge)";
        const std::string unsettled = NotConvergedWhy(rt->convergence);
        if (unsettled.empty())
          c.note += ". Its peak converged: it moved " +
                    std::to_string(rt->convergence.lastChangePct) +
                    " % on the last of " +
                    std::to_string(rt->convergence.steps.size() - 1) +
                    " refinements round it";
        else
          c.note += ". Its peak is NOT converged (" + unsettled +
                    "): a finer mesh may move it";
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
    if (host.IsAssembly() && !p.contains("occurrence"))
      throw std::runtime_error(
          "probe_study on an assembly study needs `occurrence`: the face is "
          "a part's, and the study meshed the imprinted fuse of its "
          "participants, so which placed part it is on says where it is");
    if (!host.IsAssembly() && p.contains("occurrence"))
      throw std::runtime_error(
          "probe_study takes `occurrence` on an assembly study only; a part "
          "study's face is the part's own");
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
    const fem::StudyBodyResult* body = nullptr;
    std::vector<int> faces; // the solved mesh's faces the face is
    if (!host.IsAssembly()) {
      fem::ResolvedFace rf;
      std::string why;
      if (!fem::ResolveFace(*doc, face, "probe", rf, &why))
        throw std::runtime_error(why);
      for (const fem::StudyBodyResult& b : rt->bodies)
        if (b.bodyIndex == rf.bodyIndex)
          body = &b;
      if (!body)
        throw std::runtime_error(
            "face '" + face.name +
            "' is on a body this study did not solve: a part study meshes "
            "only the bodies its loads are on");
      faces = {rf.faceIndex};
    } else {
      // An assembly study (docs/simulation/README.md#probing-a-face): the
      // part's face, placed, and the solved mesh's facets that lie on it.
      const Occurrence& occ = host.OccurrenceOf(p.at("occurrence"), "probe");
      const std::vector<Body> placed =
          ui::AssemblyStudyContextFor(state).placedBodies(occ.id);
      TopoDS_Shape onFace;
      for (const Body& b : placed) {
        if (face.featureId != 0 && b.createdBy != face.featureId)
          continue;
        if (const auto idx = b.elementMap.Resolve(face.name)) {
          int i = 0;
          for (TopExp_Explorer ex(b.shape.GetShape(), TopAbs_FACE); ex.More();
               ex.Next(), ++i)
            if (i == *idx)
              onFace = ex.Current();
          break;
        }
      }
      if (onFace.IsNull())
        throw std::runtime_error("face '" + face.name + "' is not on '" +
                                 occ.name + "' (list_elements names them)");
      for (const fem::StudyBodyResult& b : rt->bodies) {
        faces = MeshFacesOn(b.mesh, onFace);
        if (!faces.empty()) {
          body = &b;
          break;
        }
      }
      if (!body)
        throw std::runtime_error(
            "face '" + face.name + "' of '" + occ.name +
            "' is on no surface this study meshed: is the occurrence a "
            "meshed or bonded participant, and is the face on its outside?");
    }
    const int caseIdx =
        p.contains("case") ? CaseIndex(rt->caseNames, p.at("case")) : 0;
    const auto fm =
        fem::FitFaceMotion(body->mesh, body->Case(caseIdx).displacement, faces);
    if (!fm)
      throw std::runtime_error("face '" + face.name +
                               "' has no triangles in the study's mesh");
    std::optional<glm::dvec3> layerNormal;
    if (p.contains("layer_normal"))
      layerNormal = LayerNormalOf(p.at("layer_normal"));
    const fem::FemResults& caseResult = body->Case(caseIdx);
    if (layerNormal && caseResult.stress.empty())
      throw std::runtime_error(
          "this result was saved without its stress tensor (before format "
          "v172), so the stress across layers cannot be read from it -- "
          "run_study with recompute: true");
    const auto stress =
        fem::FaceStressOf(body->mesh, caseResult.vonMises, faces);
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
    if (stress) {
      out["von_mises_max_mpa"] = stress->maxMPa;
      out["von_mises_max_at_mm"] = vec(stress->maxAt);
      out["von_mises_mean_mpa"] = stress->meanMPa;
    }
    if (!caseResult.stress.empty()) {
      if (const auto pr = fem::FaceStressOf(
              body->mesh, fem::MaxPrincipalField(caseResult), faces)) {
        out["principal_max_mpa"] = pr->maxMPa;
        out["principal_max_at_mm"] = vec(pr->maxAt);
        out["principal_mean_mpa"] = pr->meanMPa;
      }
    }
    if (layerNormal) {
      out["layer_normal"] = vec(*layerNormal);
      if (const auto al = fem::FaceStressOf(
              body->mesh, fem::AcrossLayersField(caseResult, *layerNormal),
              faces)) {
        out["across_layers_max_mpa"] = al->maxMPa;
        out["across_layers_max_at_mm"] = vec(al->maxAt);
        out["across_layers_mean_mpa"] = al->meanMPa;
      }
    }
    if (fm->rotationDetermined) {
      out["rotation_rad"] = vec(fm->rotation);
      out["rotation_magnitude_deg"] = glm::degrees(glm::length(fm->rotation));
    }
    // A plastic result (docs/simulation/README.md#plasticity): the plastic
    // strain on the face, and how the face is left once the load is off.
    if (!caseResult.plasticStrain.empty()) {
      if (const auto pe =
              fem::FaceStressOf(body->mesh, caseResult.plasticStrain, faces)) {
        out["plastic_strain_max"] = pe->maxMPa;
        out["plastic_strain_max_at_mm"] = vec(pe->maxAt);
        out["plastic_strain_mean"] = pe->meanMPa;
      }
    }
    if (!caseResult.residualDisplacement.empty())
      if (const auto rm = fem::FitFaceMotion(
              body->mesh, caseResult.residualDisplacement, faces)) {
        out["residual_translation_mm"] = vec(rm->translation);
        if (rm->rotationDetermined)
          out["residual_rotation_rad"] = vec(rm->rotation);
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
    state.gpu.syncedDocumentVersion = UINT64_MAX; // force a renderer resync
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
    // A topology result is a smoothed density surface, not a tessellated
    // CAD part: there is nothing to recognise, so it stays as its facets
    // whatever the preference says.
    if (!ui::ImportAsFeature(state, path, nullptr, &err,
                             units::Length::Millimetre, /*keepOrigin=*/true,
                             interchange::UpAxis::Auto, 0.0,
                             mesh::Representation::Facets)) {
      // A refused call leaves no empty tab behind, and puts the caller back
      // where they were
      // (docs/scripting/rpc.md#a-refused-call-changes-nothing).
      for (size_t i = 0; i < state.parts.size(); ++i)
        if (state.parts[i].id == wasActive) {
          ui::SetActivePart(state, i);
          break;
        }
      for (size_t i = 0; i < state.parts.size(); ++i)
        if (state.parts[i].id == partId) {
          std::string why;
          (void)ui::DeletePart(state, i, &why);
          break;
        }
      throw std::runtime_error("topo_to_body: " + err);
    }
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
    // The study is checked BEFORE the mode is entered: entering makes a
    // default study on a part that has none, and a refused call used to
    // leave both the mode and that study behind.
    std::optional<uint64_t> study;
    if (p.contains("study")) {
      study = p.at("study").get<uint64_t>();
      host.Require(*study);
    }
    ui::EnterSimulation(state);
    if (study)
      state.simulation.activeStudyId = *study;
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
    //
    // Every key is read and checked before anything is set -- the mode
    // entered (which makes a default study on a part with none) or a single
    // display field: a bad `threshold` used to be refused with the field,
    // the scale and the mode already changed.
    std::optional<uint64_t> study;
    if (p.contains("study")) {
      study = p.at("study").get<uint64_t>();
      host.Require(*study);
    }
    // An unknown field is refused, not drawn as von Mises: a misspelled
    // "displacment" would otherwise show a plausible stress plot.
    const std::string f = p.value("field", std::string("von_mises"));
    ui::SimulationState::Field field = ui::SimulationState::Field::VonMises;
    if (f == "von_mises")
      field = ui::SimulationState::Field::VonMises;
    else if (f == "displacement")
      field = ui::SimulationState::Field::Displacement;
    else if (f == "shape")
      field = ui::SimulationState::Field::Shape;
    else if (f == "density")
      field = ui::SimulationState::Field::Density;
    else if (f == "temperature")
      field = ui::SimulationState::Field::Temperature;
    else if (f == "heat_flux")
      field = ui::SimulationState::Field::HeatFlux;
    else if (f == "none")
      field = ui::SimulationState::Field::None;
    else
      throw std::runtime_error("unknown field: " + f +
                               "; fields: von_mises, displacement, shape, "
                               "density, temperature, heat_flux, none");
    std::optional<float> deformScale, threshold;
    std::optional<int> mode, caseIndex;
    // Float on the display side: a scale past its range would be infinity,
    // answered as null (hostile_params_smoke).
    if (p.contains("deform_scale")) {
      const double v = p.at("deform_scale").get<double>();
      if (!(std::abs(v) <= 1e9))
        throw std::runtime_error("deform_scale must be within +-1e9; got " +
                                 p.at("deform_scale").dump());
      deformScale = static_cast<float>(v);
    }
    if (p.contains("mode")) { // modal: which mode shape to display (0-based)
      const json& v = p.at("mode");
      if (!v.is_number_integer() || v.get<long long>() < 0 ||
          v.get<long long>() > 100000)
        throw std::runtime_error("mode is a mode shape's 0-based index; got " +
                                 v.dump());
      mode = v.get<int>(); // checked above
    }
    if (p.contains("case")) {
      const fem::StudyRuntime* rt =
          host.Results(study.value_or(state.simulation.activeStudyId));
      if (!rt)
        throw std::runtime_error("`case`: the study has not been run");
      caseIndex = CaseIndex(rt->caseNames, p.at("case"));
    }
    if (p.contains("threshold")) { // topology iso-surface level (0..1, 0=auto)
      const double v = p.at("threshold").get<double>();
      if (!(v >= 0.0 && v <= 1.0))
        throw std::runtime_error("threshold is the iso-surface level in "
                                 "[0, 1] (0 = automatic); got " +
                                 p.at("threshold").dump());
      threshold = static_cast<float>(v);
    }

    ui::EnterSimulation(state);
    if (study)
      state.simulation.activeStudyId = *study;
    state.simulation.field = field;
    if (deformScale)
      state.simulation.deformScale = *deformScale;
    if (mode)
      state.simulation.activeMode = *mode;
    if (caseIndex)
      state.simulation.activeCase = *caseIndex;
    if (threshold)
      state.simulation.topoThreshold = *threshold;
    return json{{"field", f},
                {"deform_scale", state.simulation.deformScale},
                {"mode", state.simulation.activeMode},
                {"case", state.simulation.activeCase},
                {"threshold", state.simulation.topoThreshold}};
  }
  return std::nullopt;
}

} // namespace remote
