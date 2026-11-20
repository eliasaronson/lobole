// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/fem/SimStudy.h"

#include "core/Document.h"
#include "core/Occurrence.h"

#include <cmath>

namespace fem {

namespace {

// How a study is planned: as a part study of `doc`, or as an assembly study.
struct Route {
  bool part = false;
  const Document* doc = nullptr;
  Study study;
  AssemblyStudyContext asmCtx;
  // On an assembly with one meshed participant and no neighbour that still
  // takes the assembly path: why, said before what that path refuses.
  std::string why;
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

bool RouteOf(const SimContext& ctx, uint64_t studyId, Route& route,
             std::string* error) {
  if (ctx.sim == nullptr)
    return Refuse(error, "no simulation to plan a study of");
  const Study* study = ctx.sim->FindStudy(studyId);
  if (study == nullptr)
    return Refuse(error,
                  "the simulation has no study " + std::to_string(studyId));
  route.study = ctx.sim->Effective(*study);
  // What reads another study's result reads it from the document that held
  // both, and a simulation's results are not handed to the planners yet.
  if (route.study.shapeFrom.study != 0)
    return Refuse(error,
                  "a static study of a topology result reads that study's "
                  "result, which a simulation does not hand its planner yet; "
                  "run it as a study of the part");
  if (route.study.thermalLoad.kind == ThermalLoad::Kind::FromStudy)
    return Refuse(error,
                  "thermal_load from_study reads another study's heat, which "
                  "a simulation does not hand its planner yet; give the "
                  "thermal load as {uniform_c, stress_free_c}");

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
    return true;
  }

  if (ctx.assembly == nullptr || !ctx.placedBodies)
    return Refuse(
        error,
        "the simulation's assembly" +
            (subject.name.empty() ? std::string() : " '" + subject.name + "'") +
            " is missing");
  route.asmCtx.placedBodies = ctx.placedBodies;
  route.asmCtx.symbols = ctx.symbols;

  const Participant* meshed = nullptr;
  int meshedCount = 0;
  int neighbours = 0;
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
      return true;
    }
  }
  // The assembly planner does not read a load's region.
  for (const Load& l : route.study.loads)
    if (!l.region.IsWhole())
      return Refuse(error,
                    (route.why.empty() ? std::string()
                                       : route.why + ", so it is solved as an "
                                                     "assembly study, and: ") +
                        LoadName(l) +
                        " is on part of a face, which an assembly study does "
                        "not build; load the whole face");
  return true;
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
  const bool ok = route.part ? PlanStudy(*route.doc, route.study, out, &err)
                             : PlanAssemblyStudy(*ctx.assembly, route.study,
                                                 route.asmCtx, out, &err);
  if (!ok)
    return Refuse(error, route.why.empty()
                             ? err
                             : route.why +
                                   ", so it is solved as an assembly "
                                   "study, and: " +
                                   err);
  KeyMeshes(*ctx.sim, out);
  return true;
}

uint64_t SimStudyInputKey(const SimContext& ctx, uint64_t studyId) {
  Route route;
  if (!RouteOf(ctx, studyId, route, nullptr))
    return 0;
  return route.part
             ? StudyInputKey(*route.doc, route.study)
             : AssemblyStudyInputKey(*ctx.assembly, route.study, route.asmCtx);
}

bool RunSimStudy(SimDocument& sim, SimContext ctx, uint64_t studyId,
                 StudyRuntime& rt, std::string* error, int* meshesMade) {
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
  if (study->type == AnalysisType::Topology)
    return fail("a topology study runs through RunTopology, which a "
                "simulation does not drive yet; run it as a study of the "
                "part");
  StudyPlan plan;
  std::string err;
  if (!PlanSimStudy(ctx, studyId, plan, &err))
    return fail(err);
  ConvergeOptions converge;
  converge.mode = sim.Effective(*study).converge;
  StudyJobInput input;
  rt = StudyRuntime{};
  if (!SolveConverged(plan, input, rt, converge, &err))
    return fail(err);
  for (const auto& [key, mesh] : input.madeMeshes) {
    std::string body;
    for (const StudyBodyPlan& bp : plan.bodies)
      if (bp.meshKey == key)
        body = bp.bodyName;
    sim.StoreMesh(key, {mesh, body});
  }
  if (meshesMade)
    *meshesMade = static_cast<int>(input.madeMeshes.size());
  rt.inputHash = SimStudyInputKey(ctx, studyId);
  rt.solvedPartVersions = ctx.subjectStamps;
  study->error.clear();
  sim.StoreStudyResults(studyId, rt);
  return true;
}

} // namespace fem
