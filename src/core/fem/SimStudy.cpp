// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/fem/SimStudy.h"

#include "core/Document.h"
#include "core/InputHash.h"
#include "core/Occurrence.h"

#include <cmath>
#include <map>
#include <set>

namespace fem {

namespace {

// How a study is planned: as a part study of `doc`, or as an assembly study.
struct Route {
  bool part = false;
  const Document* doc = nullptr;
  // On the part route with features suppressed for the analysis: the
  // analysis part `doc` points at (SimGeometry.h), and the part as it is.
  std::shared_ptr<const Document> analysis;
  const Document* subjectPart = nullptr;
  Study study;
  AssemblyStudyContext asmCtx;
  // On an assembly with one meshed participant and no neighbour that still
  // takes the assembly path: why, said before what that path refuses.
  std::string why;
  // On the assembly path: how many participants are meshed, and how many
  // are neighbours (rigid, bonded, contact).
  int meshed = 0;
  int neighbours = 0;
};

bool Refuse(std::string* error, const std::string& why) {
  if (error)
    *error = why;
  return false;
}

std::string LoadName(const Load& l) {
  return "load " + std::to_string(l.id) + " (" + LoadTypeToString(l.type) + ")";
}

bool IsIdentity(const glm::dmat4& m) {
  const glm::dmat4 one(1.0);
  for (int c = 0; c < 4; ++c)
    for (int r = 0; r < 4; ++r)
      if (std::abs(m[c][r] - one[c][r]) > 1e-12)
        return false;
  return true;
}

// The assembly study with one meshed occurrence `occ` and no neighbour, as a
// study of the part it shows: anchors lose their occurrence, and the
// participant's own mesh size is the study's.
bool AsPartStudy(const AssemblyDocument& asmDoc, const Study& in,
                 const Participant& meshed, Study& out, std::string* error) {
  out = in;
  out.participants.clear();
  if (meshed.meshSize.value > 0.0 || meshed.meshSize.Bound())
    out.meshSize = meshed.meshSize;
  const auto nameOf = [&](uint64_t id) {
    const Occurrence* o = asmDoc.FindById(id);
    return o ? "'" + o->name + "'" : "occurrence " + std::to_string(id);
  };
  for (Load& l : out.loads) {
    if (l.contactWith != 0)
      return Refuse(error, LoadName(l) + " rests on " + nameOf(l.contactWith) +
                               ", which does not take part in the study; "
                               "make it rigid to clamp the meshed part where "
                               "it touches");
    if (l.occurrenceId != 0 && l.occurrenceId != meshed.occurrenceId)
      return Refuse(error, LoadName(l) + " is on " + nameOf(l.occurrenceId) +
                               ", which is not meshed; only " +
                               nameOf(meshed.occurrenceId) + " is");
    l.occurrenceId = 0;
  }
  for (Refinement& r : out.refinements) {
    if (r.occurrenceId != 0 && r.occurrenceId != meshed.occurrenceId)
      return Refuse(error, "a refinement is on " + nameOf(r.occurrenceId) +
                               ", which is not meshed; only " +
                               nameOf(meshed.occurrenceId) + " is");
    r.occurrenceId = 0;
  }
  return true;
}

// The part route's geometry step: the study read on the analysis part, the
// part regenerated without the features suppressed in it (`document` the
// part's id in SimGeometry, 0 on a part subject).
bool UseAnalysisPart(const SimContext& ctx, Route& route, uint64_t document,
                     std::string* error) {
  route.subjectPart = route.doc;
  const std::vector<uint64_t> suppressed =
      ctx.sim->Geometry().SuppressedIn(document);
  if (suppressed.empty())
    return true;
  if (!StudyForAnalysis(*route.doc, route.study, suppressed, error))
    return false;
  std::string err;
  route.analysis = ctx.analysisParts
                       ? ctx.analysisParts->Get(*route.doc, suppressed, &err)
                       : AnalysisPart(*route.doc, suppressed, &err);
  if (!route.analysis)
    return Refuse(error, err);
  route.doc = route.analysis.get();
  return true;
}

// The symmetry plane, as the part route's analysis part has it: its face
// read through the suppressed features as the loads are.
bool SymmetryOf(const SimContext& ctx, const Route& route, CutPlane& out,
                std::string* error) {
  SymmetryPlane plane = ctx.sim->Geometry().symmetry;
  const uint64_t document =
      ctx.sim->Subject().kind == SubjectKind::Part ? 0 : ~uint64_t{0};
  const std::vector<uint64_t> suppressed =
      ctx.sim->Geometry().SuppressedIn(document);
  Study none;
  if (!suppressed.empty() &&
      !StudyForAnalysis(*route.subjectPart, none, suppressed, error, &plane))
    return false;
  return ResolveSymmetryPlane(*route.doc, plane, out, error);
}

bool RouteOf(const SimContext& ctx, uint64_t studyId, Route& route,
             std::string* error) {
  if (ctx.sim == nullptr)
    return Refuse(error, "no simulation to plan a study of");
  const Study* study = ctx.sim->FindStudy(studyId);
  if (study == nullptr)
    return Refuse(error,
                  "the simulation has no study " + std::to_string(studyId));
  route.study = ctx.sim->Effective(*study);

  const SimSubject& subject = ctx.sim->Subject();
  if (subject.kind == SubjectKind::Part) {
    if (ctx.part == nullptr)
      return Refuse(error,
                    "the simulation's part" +
                        (subject.name.empty() ? std::string()
                                              : " '" + subject.name + "'") +
                        " is missing");
    for (const Load& l : route.study.loads)
      if (l.occurrenceId != 0 || l.contactWith != 0)
        return Refuse(error, LoadName(l) +
                                 " names an occurrence, and the simulation's "
                                 "subject is a part");
    route.part = true;
    route.doc = ctx.part;
    return UseAnalysisPart(ctx, route, 0, error);
  }
  // A symmetry cut is a part's: its plane is a datum or a face of one part,
  // and an assembly's parts are each placed their own way.
  if (ctx.sim->Geometry().symmetry.On())
    return Refuse(error, "a symmetry cut is a part simulation's: an "
                         "assembly's simulation is solved whole; remove the "
                         "symmetry plane");

  if (ctx.assembly == nullptr || !ctx.placedBodies)
    return Refuse(
        error,
        "the simulation's assembly" +
            (subject.name.empty() ? std::string() : " '" + subject.name + "'") +
            " is missing");
  route.asmCtx.placedBodies = ctx.placedBodies;
  route.asmCtx.symbols = ctx.symbols;

  const Participant* meshed = nullptr;
  int& meshedCount = route.meshed;
  int& neighbours = route.neighbours;
  for (const Participant& p : route.study.participants) {
    if (p.role == ParticipantRole::Meshed) {
      meshed = &p;
      ++meshedCount;
    } else if (p.role != ParticipantRole::Ignored) {
      ++neighbours;
    }
  }
  if (meshedCount == 1 && neighbours == 0) {
    const Occurrence* occ = ctx.assembly->FindById(meshed->occurrenceId);
    const std::string name =
        occ ? "'" + occ->name + "'"
            : "occurrence " + std::to_string(meshed->occurrenceId);
    SimPart part;
    if (occ != nullptr && ctx.partOf)
      part = ctx.partOf(meshed->occurrenceId);
    if (occ == nullptr)
      route.why = name + " is not in the assembly";
    else if (part.doc == nullptr)
      route.why = name + " is not a part (a sub-assembly takes part whole)";
    else if (!IsIdentity(part.placement))
      // The part path solves the part in its own frame, and the loads'
      // directions, axes and points are the assembly's.
      route.why = name + " is placed away from its part's own frame";
    else if (!occ->material.empty())
      route.why = name + " is made of '" + occ->material +
                  "' in the assembly, not its part's own material";
    if (route.why.empty()) {
      Study partStudy;
      if (!AsPartStudy(*ctx.assembly, route.study, *meshed, partStudy, error))
        return false;
      route.study = std::move(partStudy);
      route.part = true;
      route.doc = part.doc;
      return UseAnalysisPart(ctx, route, part.partId, error);
    }
  }
  const std::string prefix =
      route.why.empty() ? std::string()
                        : route.why + ", so it is solved as an assembly "
                                      "study, and: ";
  // The assembly planner places each part's bodies as they are: a part's
  // features suppressed for the analysis are a part study's.
  if (!ctx.sim->Geometry().suppressed.empty())
    return Refuse(error, prefix +
                             "features suppressed for the analysis are a part "
                             "study's (one meshed participant with no rigid, "
                             "bonded or contact neighbour); beside a "
                             "neighbour each part is solved as it is: "
                             "unsuppress them, or solve the part alone");
  // A topology result is one part's (and the assembly planner would solve
  // the whole participant, not the shape cut from it).
  if (route.study.shapeFrom.study != 0)
    return Refuse(error, prefix +
                             "a study of a topology result is a part "
                             "study's: the topology study optimised one part, "
                             "and an assembly study does not take its shape "
                             "from one; mesh that part alone, with no "
                             "neighbour");
  // The assembly planner does not read a load's region.
  for (const Load& l : route.study.loads)
    if (!l.region.IsWhole())
      return Refuse(error, prefix + LoadName(l) +
                               " is on part of a face, which an assembly "
                               "study does not build; load the whole face");
  return true;
}

// Why a topology study does not run on the assembly route.
std::string TopologyRefusal(const Route& route) {
  std::string why = route.why;
  if (why.empty())
    why = route.meshed != 1 ? "the study meshes " +
                                  std::to_string(route.meshed) + " participants"
                            : "the meshed participant has " +
                                  std::to_string(route.neighbours) +
                                  " rigid, bonded or contact neighbour" +
                                  (route.neighbours == 1 ? "" : "s");
  return "a topology study optimises one part: " + why +
         "; mesh one participant with no neighbour, or optimise the part in "
         "a simulation of its own";
}

constexpr const char* kTopologySymmetry =
    "a topology study is not cut on a symmetry plane: the optimiser would "
    "shape a half it cannot hold to the other; optimise in a simulation "
    "with no symmetry plane";

// The studies a part study of the simulation reads (StudySiblings): the
// simulation's, each routed as the study itself is (the same subject and
// roles route every study of a simulation alike), so a sibling's definition
// is the one its own run plans. Reads `ctx` by reference: it lives no longer
// than the call that made it.
StudySiblings SiblingsOf(const SimContext& ctx) {
  auto routed = std::make_shared<std::map<uint64_t, Study>>();
  const auto stampOf = [&ctx](uint64_t docId) {
    for (const auto& [id, stamp] : ctx.subjectStamps)
      if (id == docId)
        return stamp;
    return uint64_t{0};
  };
  StudySiblings sib;
  sib.find = [&ctx, routed](uint64_t id) -> const Study* {
    if (const auto it = routed->find(id); it != routed->end())
      return &it->second;
    const Study* raw = ctx.sim->FindStudy(id);
    if (raw == nullptr)
      return nullptr;
    Route r;
    Study s = RouteOf(ctx, id, r, nullptr) && r.part ? std::move(r.study)
                                                     : ctx.sim->Effective(*raw);
    return &routed->emplace(id, std::move(s)).first->second;
  };
  sib.results = [&ctx](uint64_t id) { return ctx.sim->StudyResults(id); };
  sib.stale = [&ctx, stampOf](uint64_t id) {
    return ctx.sim->StudyResultsStale(id, stampOf);
  };
  sib.all = [&ctx]() {
    std::vector<const Study*> out;
    for (const Study& s : ctx.sim->GetStudies())
      out.push_back(&s);
    return out;
  };
  return sib;
}

// Each body's mesh key, and the simulation's stored mesh under it.
void KeyMeshes(const SimDocument& sim, StudyPlan& plan) {
  for (StudyBodyPlan& bp : plan.bodies) {
    if (bp.premeshed || bp.shape.IsNull())
      continue;
    bp.meshKey = MeshKey(bp.shape, bp.settings);
    if (const StoredMesh* stored = sim.FindMesh(bp.meshKey))
      bp.stored = stored->mesh;
  }
}

} // namespace

bool PlanSimStudy(const SimContext& ctx, uint64_t studyId, StudyPlan& out,
                  std::string* error) {
  out = StudyPlan{};
  Route route;
  if (!RouteOf(ctx, studyId, route, error))
    return false;
  std::string err;
  const StudySiblings siblings = SiblingsOf(ctx);
  const bool ok = route.part
                      ? PlanStudy(*route.doc, route.study, out, &err, &siblings)
                      : PlanAssemblyStudy(*ctx.assembly, route.study,
                                          route.asmCtx, out, &err);
  if (!ok)
    return Refuse(error, route.why.empty()
                             ? err
                             : route.why +
                                   ", so it is solved as an assembly "
                                   "study, and: " +
                                   err);
  if (route.part && ctx.sim->Geometry().symmetry.On()) {
    CutPlane plane;
    if (!SymmetryOf(ctx, route, plane, error) ||
        !CutPlanOnSymmetry(out, route.study.type, plane, error))
      return false;
  }
  KeyMeshes(*ctx.sim, out);
  return true;
}

uint64_t SimStudyInputKey(const SimContext& ctx, uint64_t studyId) {
  Route route;
  if (!RouteOf(ctx, studyId, route, nullptr))
    return 0;
  if (!route.part)
    return AssemblyStudyInputKey(*ctx.assembly, route.study, route.asmCtx);
  const StudySiblings siblings = SiblingsOf(ctx);
  const uint64_t key = StudyInputKey(*route.doc, route.study, &siblings);
  // The geometry step: what is suppressed is in the analysis part's
  // bodies already, and the cut is not; both are hashed, with the plane
  // where it is now. An empty step hashes as before.
  const SimGeometry& geometry = ctx.sim->Geometry();
  if (key == 0 || geometry.Empty())
    return key;
  inputhash::Hasher h;
  h.U64(key);
  h.U64(GeometryHash(geometry));
  if (geometry.symmetry.On()) {
    CutPlane plane;
    if (!SymmetryOf(ctx, route, plane, nullptr))
      return 0;
    for (int k = 0; k < 3; ++k) {
      h.F64(plane.origin[k]);
      h.F64(plane.normal[k]);
    }
  }
  return h.Digest();
}

void StoreSimStudyResult(
    SimDocument& sim, uint64_t studyId, StudyRuntime rt, uint64_t inputHash,
    std::vector<std::pair<uint64_t, uint64_t>> subjectStamps) {
  rt.inputHash = inputHash;
  rt.solvedPartVersions = std::move(subjectStamps);
  if (Study* study = sim.StudyForRun(studyId)) {
    study->error.clear();
    if (SimDocument::SharesMesh(*study) && !rt.meshKeys.empty())
      sim.SetDocumentMeshKeys(rt.meshKeys);
  }
  sim.StoreStudyResults(studyId, std::move(rt));
}

bool PrepareSimTopology(const SimContext& ctx, uint64_t studyId, TopoInput& out,
                        std::string* error) {
  out = TopoInput{};
  Route route;
  if (!RouteOf(ctx, studyId, route, error))
    return false;
  if (route.study.type != AnalysisType::Topology)
    return Refuse(error, "study " + std::to_string(studyId) + " is a " +
                             AnalysisTypeToString(route.study.type) +
                             " study, not a topology one");
  if (!route.part)
    return Refuse(error, TopologyRefusal(route));
  if (ctx.sim->Geometry().symmetry.On())
    return Refuse(error, kTopologySymmetry);
  return PrepareTopology(*route.doc, route.study, out, error);
}

int StoreSimMadeMeshes(SimDocument& sim, const StudyPlan& plan,
                       const StudyJobInput& input) {
  for (const auto& [key, mesh] : input.madeMeshes) {
    std::string body;
    for (const StudyBodyPlan& bp : plan.bodies)
      if (bp.meshKey == key)
        body = bp.bodyName;
    sim.StoreMesh(key, {mesh, body});
  }
  return static_cast<int>(input.madeMeshes.size());
}

bool RunSimStudy(SimDocument& sim, SimContext ctx, uint64_t studyId,
                 StudyRuntime& rt, std::string* error, int* meshesMade) {
  ConvergeOptions converge;
  if (const Study* study = sim.FindStudy(studyId))
    converge.mode = sim.Effective(*study).converge;
  return RunSimStudy(sim, std::move(ctx), studyId, rt, converge, SolveOptions{},
                     error, meshesMade);
}

bool RunSimStudy(SimDocument& sim, SimContext ctx, uint64_t studyId,
                 StudyRuntime& rt, const ConvergeOptions& converge,
                 const SolveOptions& options, std::string* error,
                 int* meshesMade) {
  ctx.sim = &sim;
  if (meshesMade)
    *meshesMade = 0;
  Study* study = sim.StudyForRun(studyId);
  const auto fail = [&](const std::string& why) {
    if (study)
      study->error = why;
    return Refuse(error, why);
  };
  if (study == nullptr)
    return Refuse(error,
                  "the simulation has no study " + std::to_string(studyId));
  std::string err;
  if (study->type == AnalysisType::Topology) {
    Route route;
    if (!RouteOf(ctx, studyId, route, &err))
      return fail(err);
    if (!route.part)
      return fail(TopologyRefusal(route));
    if (sim.Geometry().symmetry.On())
      return fail(kTopologySymmetry);
    rt = StudyRuntime{};
    if (!RunTopology(*route.doc, route.study, rt, &err))
      return fail(err);
    rt.inputHash = SimStudyInputKey(ctx, studyId);
    rt.solvedPartVersions = ctx.subjectStamps;
    StoreSimStudyResult(sim, studyId, rt, rt.inputHash, ctx.subjectStamps);
    return true;
  }
  StudyPlan plan;
  if (!PlanSimStudy(ctx, studyId, plan, &err))
    return fail(err);
  StudyJobInput input;
  rt = StudyRuntime{};
  if (!SolveConverged(plan, input, rt, converge, &err, options))
    return fail(err.empty() ? "solve failed" : err);
  const int made = StoreSimMadeMeshes(sim, plan, input);
  if (meshesMade)
    *meshesMade = made;
  rt.inputHash = SimStudyInputKey(ctx, studyId);
  rt.solvedPartVersions = ctx.subjectStamps;
  StoreSimStudyResult(sim, studyId, rt, rt.inputHash, ctx.subjectStamps);
  return true;
}

bool PlanSimMesh(const SimContext& ctx, uint64_t studyId, SimMeshRun& out,
                 std::string* error) {
  out = SimMeshRun{};
  out.studyId = studyId;
  if (ctx.sim != nullptr)
    if (const Study* s = ctx.sim->FindStudy(studyId)) {
      if (s->type == AnalysisType::Topology)
        // Its mesh is made inside its run (PrepareTopology), and kept with
        // its result, not under a key.
        return Refuse(error, "a topology study meshes inside its run: run "
                             "it to see the mesh it optimises on");
      out.sharesMesh = SimDocument::SharesMesh(*s);
    }
  return PlanSimStudy(ctx, studyId, out.plan, error);
}

bool MeshSimPlanned(SimMeshRun& run, std::string* error) {
  run.meshes.clear();
  run.reports.clear();
  StudyJobInput input;
  std::string err;
  if (!MeshPlannedStudy(run.plan, input, &err))
    return Refuse(error, err.empty() ? "meshing failed" : err);
  run.madeMeshes = std::move(input.madeMeshes);
  run.meshKeys = std::move(input.meshKeys);
  for (size_t i = 0; i < input.bodies.size(); ++i) {
    StudyJobBody& jb = input.bodies[i];
    // The plan's body is what was meshed: its settings are what the
    // report's refinements are read against.
    MeshSettings asked;
    double autoSize = 0.0;
    if (i < run.plan.bodies.size()) {
      asked = run.plan.bodies[i].settings;
      if (!run.plan.bodies[i].shape.IsNull())
        autoSize = AutoMeshSize(run.plan.bodies[i].shape);
    }
    run.reports.push_back(ReportMesh(jb.mesh, asked, autoSize));
    StudyBodyMesh bm;
    bm.bodyIndex = jb.bodyIndex;
    bm.bodyName = jb.bodyName;
    bm.mesh = std::move(jb.mesh);
    run.meshes.push_back(std::move(bm));
  }
  return true;
}

int StoreSimMesh(SimDocument& sim, const SimMeshRun& run) {
  for (const auto& [key, mesh] : run.madeMeshes) {
    std::string body;
    for (const StudyBodyPlan& bp : run.plan.bodies)
      if (bp.meshKey == key)
        body = bp.bodyName;
    sim.StoreMesh(key, {mesh, body});
  }
  if (run.sharesMesh && !run.meshKeys.empty())
    sim.SetDocumentMeshKeys(run.meshKeys);
  return static_cast<int>(run.madeMeshes.size());
}

bool MeshSimStudy(SimDocument& sim, SimContext ctx, uint64_t studyId,
                  std::vector<StudyBodyMesh>* out, std::string* error,
                  int* meshesMade, std::vector<MeshBodyReport>* reports) {
  ctx.sim = &sim;
  if (meshesMade)
    *meshesMade = 0;
  if (out)
    out->clear();
  if (reports)
    reports->clear();
  Study* study = sim.StudyForRun(studyId);
  if (study == nullptr)
    return Refuse(error,
                  "the simulation has no study " + std::to_string(studyId));
  const auto fail = [&](const std::string& why) {
    study->error = why;
    return Refuse(error, why);
  };
  SimMeshRun run;
  std::string err;
  if (!PlanSimMesh(ctx, studyId, run, &err))
    return fail(err);
  if (!MeshSimPlanned(run, &err))
    return fail(err);
  const int made = StoreSimMesh(sim, run);
  if (meshesMade)
    *meshesMade = made;
  if (out)
    *out = std::move(run.meshes);
  if (reports)
    *reports = std::move(run.reports);
  return true;
}

std::vector<StudyProblem> CheckSimStudy(const SimContext& ctx, uint64_t studyId,
                                        const std::vector<CheckMesh>* meshes,
                                        bool backstop) {
  std::vector<StudyProblem> out;
  const auto plan = [&](const std::string& why) {
    StudyProblem p;
    p.code = "plan";
    p.message = why;
    out.push_back(std::move(p));
    return out;
  };
  Route route;
  std::string err;
  if (!RouteOf(ctx, studyId, route, &err))
    return plan(err);
  if (route.part) {
    const StudySiblings siblings = SiblingsOf(ctx);
    // The symmetry cut refuses what it does not cut: said with the rest.
    if (ctx.sim->Geometry().symmetry.On()) {
      if (route.study.type == AnalysisType::Topology)
        return plan(kTopologySymmetry);
      CutPlane cut;
      if (!SymmetryOf(ctx, route, cut, &err))
        return plan(err);
      if (backstop) {
        StudyPlan scratch;
        Study copy = route.study;
        std::string planErr, cutErr;
        if (PlanStudy(*route.doc, copy, scratch, &planErr, &siblings) &&
            !CutPlanOnSymmetry(scratch, route.study.type, cut, &cutErr))
          plan(cutErr);
      }
    }
    std::vector<StudyProblem> rest =
        CheckStudy(*route.doc, route.study, meshes, backstop, &siblings);
    out.insert(out.end(), rest.begin(), rest.end());
    return out;
  }
  // What the assembly check reads (ui::CheckStudyNow).
  if (route.study.loads.empty()) {
    StudyProblem p;
    p.code = "no_loads";
    p.message = "the study has no loads or supports";
    p.fixMethod = "add_load";
    p.fixParams = {{"study", studyId},
                   {"type", "fixed"},
                   {"face", nullptr},
                   {"occurrence", nullptr}};
    p.fixNote = "face and occurrence: the face that is held and whose it "
                "is; or contact_with a rigid participant";
    out.push_back(std::move(p));
    return out;
  }
  if (route.study.type == AnalysisType::Topology)
    return plan(TopologyRefusal(route));
  if (backstop) {
    Study copy = route.study;
    StudyPlan scratch;
    if (!PlanAssemblyStudy(*ctx.assembly, copy, route.asmCtx, scratch, &err))
      return plan(route.why.empty() ? err
                                    : route.why +
                                          ", so it is solved as an assembly "
                                          "study, and: " +
                                          err);
  }
  return out;
}

bool CheckSimGeometry(const SimContext& ctx, uint64_t studyId,
                      GeometryCheck& out, std::string* error) {
  out = GeometryCheck{};
  Route route;
  if (!RouteOf(ctx, studyId, route, error))
    return false;
  if (!route.part)
    return Refuse(error, "the geometry check is a part study's: beside a "
                         "neighbour the mesh is the parts imprinted on "
                         "each other");
  if (route.study.shapeFrom.study != 0)
    return Refuse(error, "a study of a topology shape meshes the shape cut "
                         "from its result: there is no geometry to check");
  const StudySiblings siblings = SiblingsOf(ctx);
  StudyPlan plan;
  Study copy = route.study;
  std::string err;
  if (route.study.type == AnalysisType::Topology) {
    // Its mesh is made inside its run, at the study's size.
    copy.type = AnalysisType::Static;
  }
  CutPlane cut;
  const bool symmetric = ctx.sim->Geometry().symmetry.On();
  if (symmetric && !SymmetryOf(ctx, route, cut, error))
    return false;
  if (PlanStudy(*route.doc, copy, plan, &err, &siblings)) {
    for (const StudyBodyPlan& bp : plan.bodies) {
      if (bp.shape.IsNull() || bp.bodyIndex < 0 ||
          bp.bodyIndex >= static_cast<int>(route.doc->GetBodies().size()))
        continue;
      CheckBodyGeometry(*route.doc, route.doc->GetBodies()[bp.bodyIndex],
                        bp.shape, bp.settings, symmetric ? &cut : nullptr, out);
    }
    return true;
  }
  // A study not set up yet (no loads, a load that does not resolve) is
  // still meshed at its size: every body, with the refinements that
  // resolve on it. What is checked is the geometry, not the setup.
  Study s = route.study;
  if (!EvaluateStudyExprs(s, route.doc->Symbols(), &err))
    return Refuse(error, err);
  const std::vector<Body>& bodies = route.doc->GetBodies();
  for (size_t bi = 0; bi < bodies.size(); ++bi) {
    if (bodies[bi].reference || bodies[bi].shape.GetShape().IsNull())
      continue;
    MeshSettings settings;
    settings.maxSize = s.meshSize.value;
    for (const Refinement& r : s.refinements) {
      ResolvedFace rf;
      if (ResolveFace(*route.doc, r.face, "refinement", rf, nullptr) &&
          rf.bodyIndex == static_cast<int>(bi) && r.size.value > 0.0)
        settings.faceSizes.push_back({rf.faceIndex, r.size.value});
    }
    CheckBodyGeometry(*route.doc, bodies[bi], bodies[bi].shape.GetShape(),
                      settings, symmetric ? &cut : nullptr, out);
  }
  return true;
}

bool SimSymmetryPlane(const SimContext& ctx, uint64_t studyId, CutPlane& out,
                      std::string* error) {
  if (ctx.sim == nullptr || !ctx.sim->Geometry().symmetry.On())
    return Refuse(error, "the simulation has no symmetry plane");
  Route route;
  if (!RouteOf(ctx, studyId, route, error))
    return false;
  if (!route.part)
    return Refuse(error, "a symmetry cut is a part study's");
  return SymmetryOf(ctx, route, out, error);
}

std::string
SimGeometryRefusal(const SimContext& ctx, const SimGeometry& geometry,
                   const std::function<const Document*(uint64_t)>& partById) {
  if (ctx.sim == nullptr)
    return "no simulation";
  const bool assembly = ctx.sim->Subject().kind == SubjectKind::Assembly;
  if (assembly && geometry.symmetry.On())
    return "a symmetry cut is a part simulation's: an assembly's simulation "
           "is solved whole";
  // Each part's suppression on its own: a feature that is not its part's,
  // and one that leaves the part broken.
  std::set<uint64_t> documents;
  for (const SuppressedFeature& f : geometry.suppressed)
    documents.insert(f.document);
  for (const uint64_t document : documents) {
    const Document* part = partById ? partById(document) : nullptr;
    if (part == nullptr)
      return document == 0 ? std::string("the simulation's part is missing")
                           : "document " + std::to_string(document) +
                                 " is no part of this project";
    std::string err;
    const std::vector<uint64_t> ids = geometry.SuppressedIn(document);
    const auto made = ctx.analysisParts
                          ? ctx.analysisParts->Get(*part, ids, &err)
                          : AnalysisPart(*part, ids, &err);
    if (!made)
      return err;
  }
  // What each study's route says now, and would say with it.
  SimDocument with = *ctx.sim;
  with.SetGeometry(geometry);
  SimContext next = ctx;
  next.sim = &with;
  for (const Study& s : ctx.sim->GetStudies()) {
    Route before, after;
    std::string err;
    if (!RouteOf(ctx, s.id, before, nullptr))
      continue; // refused already, for another reason
    if (!RouteOf(next, s.id, after, &err))
      return "study '" + s.name + "': " + err;
    if (after.part && geometry.symmetry.On()) {
      CutPlane plane;
      if (!SymmetryOf(next, after, plane, &err))
        return "study '" + s.name + "': " + err;
    }
  }
  // With no study yet, the plane alone.
  if (geometry.symmetry.On() && !assembly && ctx.part != nullptr) {
    const std::vector<uint64_t> ids = geometry.SuppressedIn(0);
    std::shared_ptr<const Document> analysis;
    std::string err;
    if (!ids.empty())
      analysis = ctx.analysisParts
                     ? ctx.analysisParts->Get(*ctx.part, ids, &err)
                     : AnalysisPart(*ctx.part, ids, &err);
    SymmetryPlane plane = geometry.symmetry;
    Study none;
    if (!ids.empty() && !StudyForAnalysis(*ctx.part, none, ids, &err, &plane))
      return err;
    CutPlane cut;
    if (!ResolveSymmetryPlane(analysis ? *analysis : *ctx.part, plane, cut,
                              &err))
      return err;
  }
  return "";
}

int RestampSimByInputs(SimDocument& sim, SimContext ctx) {
  ctx.sim = &sim;
  const auto stampOf = [&](uint64_t docId) {
    for (const auto& [id, stamp] : ctx.subjectStamps)
      if (id == docId)
        return stamp;
    return uint64_t{0};
  };
  int stamped = 0;
  // A shape study's inputs and staleness read its topology result's: the
  // results it is cut from first (RestampByInputs).
  for (const bool shapes : {false, true})
    for (const uint64_t id : sim.ResultIds()) {
      const Study* study = sim.FindStudy(id);
      if (study == nullptr || (study->shapeFrom.study != 0) != shapes)
        continue;
      const StudyRuntime* rt = sim.StudyResults(id);
      if (rt == nullptr || rt->inputHash == 0 ||
          !sim.StudyResultsStale(id, stampOf))
        continue;
      if (SimStudyInputKey(ctx, id) == rt->inputHash &&
          sim.MarkStudyResultsCurrent(id, ctx.subjectStamps))
        ++stamped;
    }
  return stamped;
}

} // namespace fem
