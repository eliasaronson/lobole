#include "remote/RemoteSimulation.h"

#include "CADMaterials.h"
#include "core/Command.h"
#include "core/Document.h"
#include "core/FemSolver.h"
#include "core/FemStudy.h"
#include "core/FemTopo.h"
#include "core/Serialization.h"
#include "ui/AppActions.h"
#include "ui/AppState.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using nlohmann::json;

namespace remote {

// FEM study RPCs, carved out of RemoteServer::HandleSemantic. All act on the
// active part document. Returns the result json when `m` is one of ours,
// std::nullopt otherwise; throws std::runtime_error on error (the caller's
// try/catch turns it into an error response).
std::optional<json> HandleSimulationRpc(ui::AppState& state, Document& document,
                                        const std::string& m, const json& p) {
  Document* doc = &document;  // keep the moved block's `doc->` verbatim
  if (m == "add_study") {
    auto cmd = std::make_unique<AddStudyCommand>(p.value("name", std::string{}));
    AddStudyCommand* raw = cmd.get();
    state.commands->Execute(std::move(cmd), *doc);
    const fem::Study* s = doc->FindStudy(raw->StudyId());
    return json{{"id", s->id}, {"name", s->name}};
  }
  if (m == "remove_study") {
    const uint64_t id = p.at("study").get<uint64_t>();
    if (!doc->FindStudy(id)) throw std::runtime_error("no such study");
    state.commands->Execute(std::make_unique<RemoveStudyCommand>(id), *doc);
    return json::object();
  }
  if (m == "list_studies") {
    json studies = json::array();
    for (const fem::Study& s : doc->GetStudies()) {
      // Report evaluated sizes/magnitudes (against current parameters) without
      // mutating the stored study.
      fem::Study eval = s;
      fem::EvaluateStudyExprs(eval, doc->Symbols(), nullptr);
      json loads = json::array();
      for (const fem::Load& l : eval.loads) {
        json lj = {{"id", l.id},
                   {"type", fem::LoadTypeToString(l.type)},
                   {"face", {{"feature_id", l.face.featureId},
                             {"element", l.face.name}}}};
        if (l.type != fem::LoadType::Fixed) lj["magnitude"] = l.magnitude;
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
                 {"has_results", doc->StudyResults(s.id) != nullptr},
                 {"stale", doc->StudyResultsStale(s.id)}};
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
  auto studyFaceOf = [](const json& r) {
    return ElementRef{r.at("feature_id").get<uint64_t>(),
                      r.at("element").get<std::string>()};
  };
  // Runs one study mutation as a ModifyStudyCommand: snapshot before, apply
  // `mutate` to a copy, snapshot after, execute.
  auto modifyStudy = [&](uint64_t id, const std::string& desc,
                         const std::function<void(fem::Study&)>& mutate) {
    const fem::Study* cur = doc->FindStudy(id);
    if (!cur) throw std::runtime_error("no such study");
    json before = DocumentIO::StudyToJson(*cur);
    fem::Study copy = *cur;
    mutate(copy);
    json after = DocumentIO::StudyToJson(copy);
    state.commands->Execute(
        std::make_unique<ModifyStudyCommand>(desc, before, after), *doc);
  };

  if (m == "set_study") {
    const uint64_t id = p.at("study").get<uint64_t>();
    modifyStudy(id, "Edit study", [&](fem::Study& s) {
      if (p.contains("name")) s.name = p.at("name").get<std::string>();
      if (p.contains("mesh_size")) {
        const json& v = p.at("mesh_size");
        if (v.is_string()) {
          s.meshSizeExpr = v.get<std::string>();  // evaluated at mesh time
        } else {
          s.meshSize = v.get<double>();
          s.meshSizeExpr.clear();
        }
      }
      if (p.contains("type") &&
          !fem::AnalysisTypeFromString(p.at("type").get<std::string>(), s.type))
        throw std::runtime_error("analysis type must be static, modal, or topology");
      if (p.contains("num_modes")) s.numModes = p.at("num_modes").get<int>();
      if (p.contains("gravity")) {
        const auto& g = p.at("gravity");
        s.gravity = glm::dvec3(g.at(0).get<double>(), g.at(1).get<double>(),
                               g.at(2).get<double>());
      }
      // Topology parameters (expression-or-literal, like mesh size).
      if (p.contains("volume_fraction")) {
        const json& v = p.at("volume_fraction");
        if (v.is_string()) s.volumeFractionExpr = v.get<std::string>();
        else { s.volumeFraction = v.get<double>(); s.volumeFractionExpr.clear(); }
      }
      if (p.contains("filter_radius")) {
        const json& v = p.at("filter_radius");
        if (v.is_string()) s.filterRadiusExpr = v.get<std::string>();
        else { s.filterRadius = v.get<double>(); s.filterRadiusExpr.clear(); }
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
        s.keepFaces.erase(
            std::remove_if(s.keepFaces.begin(), s.keepFaces.end(),
                           [&](const ElementRef& f) {
                             return f.featureId == face.featureId &&
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
    const json& v = p.at("size");
    if (v.is_string()) r.sizeExpr = v.get<std::string>();
    else r.size = v.get<double>();
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
    l.id = doc->AllocateId();
    if (!fem::LoadTypeFromString(p.at("type").get<std::string>(), l.type))
      throw std::runtime_error("load type must be fixed, force, or pressure");
    l.face = studyFaceOf(p.at("face"));
    if (p.contains("magnitude")) {
      const json& v = p.at("magnitude");
      if (v.is_string()) l.magnitudeExpr = v.get<std::string>();
      else l.magnitude = v.get<double>();
    }
    if (p.contains("direction")) {
      const auto& d = p.at("direction");
      l.direction = glm::dvec3(d.at(0).get<double>(), d.at(1).get<double>(),
                               d.at(2).get<double>());
    }
    modifyStudy(id, "Add load", [&](fem::Study& s) { s.loads.push_back(l); });
    return json{{"id", l.id}};
  }
  if (m == "remove_load") {
    const uint64_t id = p.at("study").get<uint64_t>();
    const uint64_t loadId = p.at("load").get<uint64_t>();
    modifyStudy(id, "Remove load", [&](fem::Study& s) {
      s.loads.erase(std::remove_if(s.loads.begin(), s.loads.end(),
                                   [&](const fem::Load& l) { return l.id == loadId; }),
                    s.loads.end());
    });
    return json::object();
  }
  if (m == "mesh_study") {
    // Mesh (no solve) every body the study touches; a headless milestone
    // before the solver exists. params: {study}
    const uint64_t id = p.at("study").get<uint64_t>();
    fem::Study* s = doc->FindStudy(id);
    if (!s) throw std::runtime_error("no such study");
    std::vector<fem::StudyBodyMesh> meshes;
    std::string err;
    if (!fem::MeshStudy(*doc, *s, meshes, &err)) throw std::runtime_error(err);
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
    // Catch a finished async run first, so its result is stored before we read.
    ui::PollTopologyRun(state, *doc);
    const uint64_t id = p.at("study").get<uint64_t>();
    fem::Study* s = doc->FindStudy(id);
    if (!s) throw std::runtime_error("no such study");

    if (m == "cancel_study") {
      return json{{"cancelled", ui::CancelTopologyRun(state, id)}};
    }
    if (m == "study_status") {
      const ui::TopoRunStatus st = ui::GetTopologyStatus(state, id);
      if (st.running)
        return json{{"running", true},
                    {"iteration", st.iteration},
                    {"max_iterations", st.maxIterations},
                    {"compliance", st.compliance},
                    {"max_change", st.maxChange},
                    {"volume_fraction", st.volumeFraction}};
      // Not running — report the cached result's state (if any).
      const fem::StudyRuntime* rt = doc->StudyResults(id);
      return json{{"running", false},
                  {"has_results", rt != nullptr},
                  {"converged", rt && rt->converged},
                  {"cancelled", rt && rt->cancelled},
                  {"iterations", rt ? rt->iterations : 0},
                  {"stale", doc->StudyResultsStale(id)}};
    }

    if (m == "run_study") {
      if (s->type == fem::AnalysisType::Topology) {
        std::string err;
        if (!ui::StartTopologyRun(state, *doc, id, &err))
          throw std::runtime_error(err);
        return json{{"started", true}, {"running", true}};
      }
      std::string err;
      if (!ui::RunStudyAndCache(*doc, id, &err)) throw std::runtime_error(err);
    }

    const fem::StudyRuntime* rt = doc->StudyResults(id);
    if (!rt) throw std::runtime_error("study has not been run");

    // Topology studies report the optimization summary (the density field is
    // read via set_result_display / export, not echoed here).
    if (s->type == fem::AnalysisType::Topology) {
      std::size_t nodes = 0, elements = 0;
      for (const fem::StudyBodyResult& b : rt->bodies) {
        nodes += b.mesh.stats.nodeCount;
        elements += b.mesh.stats.tetCount;
      }
      return json{{"analysis", "topology"},
                  {"nodes", nodes},
                  {"elements", elements},
                  {"iterations", rt->iterations},
                  {"converged", rt->converged},
                  {"cancelled", rt->cancelled},
                  {"volume_fraction", rt->volumeFractionAchieved},
                  {"compliance", rt->complianceHistory.empty()
                                     ? 0.0
                                     : rt->complianceHistory.back()},
                  {"stale", doc->StudyResultsStale(id)}};
    }

    // Modal studies report per-body natural frequencies instead of stress.
    if (s->type == fem::AnalysisType::Modal) {
      std::size_t nodes = 0, elements = 0;
      json bodies = json::array();
      for (const fem::StudyBodyResult& b : rt->bodies) {
        nodes += b.mesh.stats.nodeCount;
        elements += b.mesh.stats.tetCount;
        json freqs = json::array();
        for (const auto& mode : b.results.modes) freqs.push_back(mode.frequencyHz);
        bodies.push_back({{"body", b.bodyName}, {"frequencies_hz", freqs}});
      }
      return json{{"analysis", "modal"},
              {"nodes", nodes},
              {"elements", elements},
              {"bodies", bodies},
              {"stale", doc->StudyResultsStale(id)}};
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
      if (b.bodyIndex >= 0 && b.bodyIndex < (int)doc->GetBodies().size() &&
          b.results.maxVonMises > 0) {
        const auto mt = CADMaterials::MaterialLibrary::FromSlug(
            doc->GetBodies()[b.bodyIndex].material);
        const float yield = CADMaterials::MaterialLibrary::GetMaterial(mt).yieldMPa;
        if (yield > 0) {
          const double fos = yield / b.results.maxVonMises;
          bj["factor_of_safety"] = fos;
          if (!haveFoS || fos < minFoS) minFoS = fos;
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
        {"stale", doc->StudyResultsStale(id)}};
    if (haveFoS) result["factor_of_safety"] = minFoS;
    return result;
  }
  if (m == "set_topo_ghost") {
    // Toggle the modeling-mode shape-guide ghost for a topology study: a
    // translucent render of its optimized shape, to re-model against.
    // params: {study, on?: true}
    const uint64_t id = p.at("study").get<uint64_t>();
    if (!doc->FindStudy(id)) throw std::runtime_error("no such study");
    const bool on = p.value("on", true);
    state.topoGhostStudyId = on ? id : 0;
    state.syncedDocumentVersion = UINT64_MAX;  // force a renderer resync
    return json{{"study", id}, {"on", on}};
  }
  if (m == "export_topo_stl") {
    // Export a topology result's optimized shape (iso-surface, Taubin-smoothed)
    // as a binary STL. params: {study, path, threshold?: 0..1 (0=auto),
    // smooth?: iterations (default 3)}
    ui::PollTopologyRun(state, *doc);
    const uint64_t id = p.at("study").get<uint64_t>();
    fem::Study* s = doc->FindStudy(id);
    if (!s) throw std::runtime_error("no such study");
    if (s->type != fem::AnalysisType::Topology)
      throw std::runtime_error("not a topology study");
    const fem::StudyRuntime* rt = doc->StudyResults(id);
    if (!rt || rt->bodies.empty())
      throw std::runtime_error("study has no result to export");
    const fem::StudyBodyResult& b = rt->bodies.front();
    const double thr = p.contains("threshold") && p.at("threshold").get<double>() > 0
                           ? p.at("threshold").get<double>()
                           : fem::AutoIsoThreshold(b.mesh, b.density,
                                                   rt->volumeFractionAchieved);
    const std::string path = p.at("path").get<std::string>();
    std::string err;
    if (!fem::ExportTopoStl(b.mesh, b.density, thr, p.value("smooth", 3), path, &err))
      throw std::runtime_error(err);
    return json{{"path", path}, {"threshold", thr}};
  }
  if (m == "enter_simulation") {
    // Enter the FEM simulation workspace (the GUI mode), optionally selecting a
    // study. The LLM drives the same mode a human enters from the toolbar.
    // params: {study?}
    ui::EnterSimulation(state);
    if (p.contains("study")) {
      const uint64_t id = p.at("study").get<uint64_t>();
      if (!doc->FindStudy(id)) throw std::runtime_error("no such study");
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
      if (!doc->FindStudy(id)) throw std::runtime_error("no such study");
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
      state.simulation.deformScale = static_cast<float>(p.at("deform_scale").get<double>());
    if (p.contains("mode"))  // modal: which mode shape to display (0-based)
      state.simulation.activeMode = p.at("mode").get<int>();
    if (p.contains("threshold"))  // topology iso-surface level (0..1, 0=auto)
      state.simulation.topoThreshold =
          static_cast<float>(p.at("threshold").get<double>());
    return json{{"field", f},
            {"deform_scale", state.simulation.deformScale},
            {"mode", state.simulation.activeMode},
            {"threshold", state.simulation.topoThreshold}};
  }
  return std::nullopt;
}

}  // namespace remote
