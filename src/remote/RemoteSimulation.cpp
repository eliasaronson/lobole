#include "remote/RemoteSimulation.h"

#include "cad/CADMaterials.h"
#include "core/Command.h"
#include "core/Document.h"
#include "core/fem/FemSolver.h"
#include "core/fem/FemStudy.h"
#include "core/fem/FemTopo.h"
#include "core/io/Serialization.h"
#include "remote/RemoteHelpers.h"
#include "ui/AppState.h"
#include "ui/BackgroundJobs.h"
#include "ui/SimulationActions.h"

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
// describe: one row per `m ==` branch below — extend together.
static constexpr MethodDoc SimulationMethods[] = {
    {"enter_simulation", "Enter simulation mode for the active part.", "study",
     remote::NeedsPart},
    {"exit_simulation", "Leave simulation mode.", "", remote::NeedsPart},
    {"add_study", "Add a study {kind: static|modal|topology, name?}.",
     "kind, name", remote::NeedsPart},
    {"remove_study", "Remove a study by id.", "study", remote::NeedsPart},
    {"list_studies", "List studies with status.", "", remote::NeedsPart},
    {"set_study", "Edit study settings (mesh size, materials, solver options).",
     "filter_radius, gravity, max_iterations, mesh_size, name, num_modes, "
     "study, type, volume_fraction",
     remote::NeedsPart},
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
     "direction, face, magnitude, study, type", remote::NeedsPart},
    {"remove_load", "Remove a load by index.", "load, study",
     remote::NeedsPart},
    {"add_refinement", "Add a local mesh refinement on an element.",
     "face, size, study", remote::NeedsPart},
    {"remove_refinement", "Remove a refinement.", "face, study",
     remote::NeedsPart},
    {"add_keep_face", "Topology optimization: mark a face to keep.",
     "face, study", remote::NeedsPart},
    {"remove_keep_face", "Unmark a kept face.", "face, study",
     remote::NeedsPart},
    {"mesh_study",
     "Mesh the study (Netgen tets); returns element counts/quality. "
     "Synchronous by default — a fine mesh takes minutes, so either give the "
     "client a long timeout (the server finishes even if the client gives "
     "up) or pass `async: true`: the mesh then runs on a background job and "
     "study_status answers {running, meshing: true} until it lands, after "
     "which its `mesh` carries the same bodies/nodes/elements/worst_quality.",
     "async, study", remote::NeedsPart},
    {"run_study",
     "Solve the study (blocking, or `async` and poll study_status). Async "
     "meshes AND solves off the frame; the result is stored into the part "
     "the study belongs to even if another tab is active when it lands.",
     "async, study", remote::NeedsPart},
    {"cancel_study", "Cancel a running study.", "study", remote::NeedsPart},
    {"study_status",
     "Status/progress of a study: {running, meshing?, iteration…} while a "
     "job runs, else the cached result's state plus `mesh` from the last "
     "mesh_study {async}.",
     "study", remote::NeedsPart},
    {"study_results",
     "Result summary: {elements, nodes, max_displacement_mm: {value, at}, "
     "max_von_mises_mpa: {value, at}, factor_of_safety} for a static study, "
     "`frequencies_hz` for a modal one, and the optimization summary for a "
     "topology one. Refused with \"study has not been run\" when there is "
     "nothing to report -- a solved result is DERIVED and no project keeps "
     "one, so a study reopened from disk carries its load cases and no "
     "answers (`has_results`, `stale` in list_studies).",
     "study", remote::NeedsPart},
    {"set_result_display", "Choose the displayed result field/deform scale.",
     "deform_scale, field, mode, study, threshold", remote::NeedsPart},
    {"set_topo_ghost", "Toggle the topology-optimization ghost overlay.",
     "on, study", remote::NeedsPart},
    {"export_topo_stl", "Export the optimized topology as STL.",
     "path, smooth, study, threshold", remote::NeedsPart},
};
std::span<const MethodDoc> SimulationMethodDocs() { return SimulationMethods; }

std::optional<json> HandleSimulationRpc(ui::AppState& state, Document& document,
                                        const std::string& m,
                                        const RpcParams& p) {
  Document* doc = &document; // keep the moved block's `doc->` verbatim
  if (m == "add_study") {
    // Optional `kind` sets the analysis type at creation (the doc row always
    // promised it; it used to require a second set_study call). Folding it
    // into the create keeps this one undo step.
    fem::AnalysisType type = fem::AnalysisType::Static;
    const bool hasKind = p.contains("kind");
    if (hasKind &&
        !fem::AnalysisTypeFromString(p.at("kind").get<std::string>(), type))
      throw std::runtime_error("study kind must be static, modal, or topology");
    auto cmd =
        std::make_unique<AddStudyCommand>(p.value("name", std::string{}));
    AddStudyCommand* raw = cmd.get();
    state.commands->Execute(std::move(cmd), *doc);
    fem::Study* s = doc->FindStudy(raw->StudyId());
    if (hasKind)
      s->type = type;
    return json{{"id", s->id},
                {"name", s->name},
                {"kind", fem::AnalysisTypeToString(s->type)}};
  }
  if (m == "remove_study") {
    const uint64_t id = p.at("study").get<uint64_t>();
    if (!doc->FindStudy(id))
      throw std::runtime_error("no such study");
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
        json lj = {
            {"id", l.id},
            {"type", fem::LoadTypeToString(l.type)},
            {"face",
             {{"feature_id", l.face.featureId}, {"element", l.face.name}}}};
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
    const fem::Study* cur = doc->FindStudy(id);
    if (!cur)
      throw std::runtime_error("no such study");
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
    l.id = doc->AllocateId();
    if (!fem::LoadTypeFromString(p.at("type").get<std::string>(), l.type))
      throw std::runtime_error("load type must be fixed, force, or pressure");
    l.face = studyFaceOf(p.at("face"));
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
      if (const auto into = fem::DefaultForceDirection(*doc, l.face))
        l.direction = *into;
    }
    modifyStudy(id, "Add load", [&](fem::Study& s) { s.loads.push_back(l); });
    return json{{"id", l.id}};
  }
  if (m == "remove_load") {
    const uint64_t id = p.at("study").get<uint64_t>();
    const uint64_t loadId = p.at("load").get<uint64_t>();
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
    const uint64_t id = p.at("study").get<uint64_t>();
    fem::Study* s = doc->FindStudy(id);
    if (!s)
      throw std::runtime_error("no such study");
    if (p.value("async", false)) {
      // Off the frame (docs/ui/long-operations.md, phase 3): the plan is
      // made here, the mesh on a worker; study_status says `meshing: true`
      // until it lands and then carries the same `bodies` this returns.
      std::string err;
      if (!ui::StartMeshRun(state, *doc, id, &err))
        throw std::runtime_error(err);
      return json{{"started", true}, {"running", true}, {"job", "mesh"}};
    }
    std::vector<fem::StudyBodyMesh> meshes;
    std::string err;
    if (!fem::MeshStudy(*doc, *s, meshes, &err))
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
    const uint64_t id = p.at("study").get<uint64_t>();
    fem::Study* s = doc->FindStudy(id);
    if (!s)
      throw std::runtime_error("no such study");

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
      const fem::StudyRuntime* rt = doc->StudyResults(id);
      json out{{"running", false},
               {"has_results", rt != nullptr},
               {"converged", rt && rt->converged},
               {"cancelled", rt && rt->cancelled},
               {"iterations", rt ? rt->iterations : 0},
               {"solver", rt ? rt->solver : ""},
               {"solve_seconds", rt ? rt->solveSeconds : 0.0},
               {"stale", doc->StudyResultsStale(id)}};
      if (const ui::MeshReport* mr = ui::MeshReportFor(state, *doc, id)) {
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
        if (!ui::StartTopologyRun(state, *doc, id, &err))
          throw std::runtime_error(err);
        return json{{"started", true}, {"running", true}};
      }
      // Static/modal: async on request (a big mesh can outlive a remote
      // client's socket timeout when solved synchronously).
      if (p.value("async", false)) {
        std::string err;
        if (!ui::StartStudyRun(state, *doc, id, &err))
          throw std::runtime_error(err);
        return json{{"started", true}, {"running", true}};
      }
      std::string err;
      if (!ui::RunStudyAndCache(*doc, id, &err))
        throw std::runtime_error(err);
    }

    const fem::StudyRuntime* rt = doc->StudyResults(id);
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
                  {"solver", rt->solver},
                  {"solve_seconds", rt->solveSeconds},
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
        for (const auto& mode : b.results.modes)
          freqs.push_back(mode.frequencyHz);
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
        {"stale", doc->StudyResultsStale(id)}};
    if (haveFoS)
      result["factor_of_safety"] = minFoS;
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
      const bool stale = doc->StudyResultsStale(id);
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
    if (!doc->FindStudy(id))
      throw std::runtime_error("no such study");
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
    fem::Study* s = doc->FindStudy(id);
    if (!s)
      throw std::runtime_error("no such study");
    if (s->type != fem::AnalysisType::Topology)
      throw std::runtime_error("not a topology study");
    const fem::StudyRuntime* rt = doc->StudyResults(id);
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
      if (!doc->FindStudy(id))
        throw std::runtime_error("no such study");
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
      if (!doc->FindStudy(id))
        throw std::runtime_error("no such study");
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
