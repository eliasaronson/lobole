// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// The one planner of a simulation's studies
// (docs/simulation/README.md#the-simulation-document-core): a part subject
// and an assembly subject go through the same entry, PlanSimStudy, and hash
// through the same SimStudyInputKey.
//
// A part subject is planned as a part study (PlanStudy). An assembly whose
// one meshed participant has no rigid, bonded or contact neighbour is the
// same problem, so it is planned as that part's study too: pins, the frame
// supports, thermal studies and loads on part of a face work there, and the
// part and the part as a one-participant assembly hash the same. Beside a
// neighbour the assembly planner (PlanAssemblyStudy) builds it, and what that
// planner does not build is refused naming why.
//
// Each planned body carries the key of the mesh it is made from (MeshKey:
// its shape and its settings) and the simulation's stored mesh under that
// key, which the run solves on instead of meshing again; a mesh the run
// does make is handed back for the simulation to keep.
//
// What reads another study -- a static study of a topology result
// (ShapeSource), a thermal_load from_study -- reads it from the simulation
// (StudySiblings, routed as the study itself is), so it plans and hashes as
// the same studies on the part do. Beside a neighbour neither is built: a
// topology result is one part's, and an assembly study does not solve heat.

#include "core/fem/FemCheck.h"
#include "core/fem/FemMeshReport.h"
#include "core/fem/FemSolver.h"
#include "core/fem/FemTopo.h"
#include "core/fem/SimDocument.h"

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class AssemblyDocument;
class Document;

namespace fem {

// The part an occurrence shows, as an assembly subject's placed body is made
// from it: the part document (in the occurrence's configuration) and where
// the occurrence puts it in the assembly. `doc` null: not a part (a
// sub-assembly) or not resolved.
struct SimPart {
  const Document* doc = nullptr;
  glm::dmat4 placement{1.0};
};

// What a simulation's study is planned against: the subject, resolved by the
// caller (the simulation names it by stable id; the project owns it).
struct SimContext {
  const SimDocument* sim = nullptr;
  // A part subject: its document, in the simulation's configuration.
  const Document* part = nullptr;
  // An assembly subject, its occurrences' bodies placed in the assembly
  // (AssemblyStudyContext::placedBodies), and each occurrence's part.
  const AssemblyDocument* assembly = nullptr;
  std::function<std::vector<Body>(uint64_t occurrenceId)> placedBodies;
  std::function<SimPart(uint64_t occurrenceId)> partOf;
  // The project's variables, which an assembly subject's study reads (a part
  // subject's reads its document's own table). May be null.
  const SymbolTable* symbols = nullptr;
  // The subject documents' stamps now, {document id, stamp}: what a result
  // is solved against, so an edit to any of them reads it stale
  // (SimDocument::StudyResultsStale).
  std::vector<std::pair<uint64_t, uint64_t>> subjectStamps;
};

// Plans study `studyId` of `ctx.sim` (its effective form,
// SimDocument::Effective). False with the reason when it is refused.
bool PlanSimStudy(const SimContext& ctx, uint64_t studyId, StudyPlan& out,
                  std::string* error);

// The input hash of a run of study `studyId`: StudyInputKey on the part
// path, AssemblyStudyInputKey beside neighbours. 0 when it would be refused.
uint64_t SimStudyInputKey(const SimContext& ctx, uint64_t studyId);

// Stamps current every stored result of `sim` whose inputs hash as they did
// when it was solved (StudyRuntime::inputHash against SimStudyInputKey),
// against `ctx.subjectStamps`: what a reopened or migrated simulation is
// handed, so a study whose inputs did not change is not stale. A result
// whose inputs moved stays stale. Returns how many were stamped current.
// `ctx.sim` is ignored.
int RestampSimByInputs(SimDocument& sim, SimContext ctx);

// Plans and solves study `studyId` of `sim`, storing the result on `sim`
// stamped with its input hash and `ctx.subjectStamps`
// (StoreSimStudyResult), and every mesh it made under its key. A topology
// study runs RunTopology on the part it plans as (refused beside a
// neighbour: it optimises one part); it meshes for itself and keeps no
// mesh. `rt` is the run's whole answer, before the reduction
// a stored result gets (KeepWhatIsRead). `meshesMade`, when given, is how
// many bodies were meshed rather than read from the store. A study on the
// document's mesh (SimDocument::SharesMesh) sets the document's mesh keys
// to the ones it solved on. The study's error is written on `sim`
// (StudyForRun). `ctx.sim` is ignored.
bool RunSimStudy(SimDocument& sim, SimContext ctx, uint64_t studyId,
                 StudyRuntime& rt, std::string* error,
                 int* meshesMade = nullptr);
// The same under a run's convergence and solve options (the app's: its
// CalculiX run, the GPU offered, a cancel); the plain form uses the study's
// convergence and the defaults. A topology study ignores both.
bool RunSimStudy(SimDocument& sim, SimContext ctx, uint64_t studyId,
                 StudyRuntime& rt, const ConvergeOptions& converge,
                 const SolveOptions& options, std::string* error,
                 int* meshesMade = nullptr);
// Keeps every mesh a solve of `plan` made (StudyJobInput::madeMeshes) on
// `sim` under its key, named for its body: what a run off the frame does
// on the main thread when it lands. Returns how many.
int StoreSimMadeMeshes(SimDocument& sim, const StudyPlan& plan,
                       const StudyJobInput& input);

// Stores a finished run's result on `sim`: stamped with `inputHash` (the
// SimStudyInputKey taken when the run was planned) and the subject stamps
// it was solved against, the study's error cleared, and, for a study on the
// document's mesh, the document's mesh keys set to the ones it solved on.
// What an async caller does on the main thread when its worker is done.
void StoreSimStudyResult(
    SimDocument& sim, uint64_t studyId, StudyRuntime rt, uint64_t inputHash,
    std::vector<std::pair<uint64_t, uint64_t>> subjectStamps);

// A topology study off the frame: PrepareTopology on the part study
// `studyId` plans as (main thread), refused as RunSimStudy refuses it. The
// caller then runs TopologyOptimize on a worker, MakeTopoRuntime, and
// StoreSimStudyResult with the SimStudyInputKey it took here. On a refusal
// the caller writes `*error` onto the study (StudyForRun).
bool PrepareSimTopology(const SimContext& ctx, uint64_t studyId, TopoInput& out,
                        std::string* error);

// Meshing a study on its own, the meshes a run of it solves on
// (docs/simulation/README.md#one-mesh-shared), split at the thread boundary
// as the part's mesh is (docs/ui/long-operations.md#fem-meshing-and-solving):
//   PlanSimMesh   (main thread) PlanSimStudy, each body keyed and handed its
//                 stored mesh;
//   MeshSimPlanned (any thread) meshes what is not stored: plain fem data
//                 and the plan's own shape copies, nothing shared;
//   StoreSimMesh  (main thread) keeps every mesh made under its key and, for
//                 a study on the document's mesh, sets the document's mesh
//                 keys.
// A run after it meshes nothing it made.
struct SimMeshRun {
  uint64_t studyId = 0;
  bool sharesMesh = false; // SimDocument::SharesMesh, at the plan
  StudyPlan plan;
  // Per solved body, in the plan's order: the mesh (as the run solves on it)
  // and what it says about itself (ReportMesh).
  std::vector<StudyBodyMesh> meshes;
  std::vector<MeshBodyReport> reports;
  // What MeshSimPlanned meshed, by key, and every keyed body's key.
  std::vector<std::pair<uint64_t, std::shared_ptr<const FemMesh>>> madeMeshes;
  std::vector<uint64_t> meshKeys;
};
bool PlanSimMesh(const SimContext& ctx, uint64_t studyId, SimMeshRun& out,
                 std::string* error);
bool MeshSimPlanned(SimMeshRun& run, std::string* error);
// Returns how many meshes it stored.
int StoreSimMesh(SimDocument& sim, const SimMeshRun& run);
// The three in one, synchronous. `out` (and `reports`) the per-body meshes;
// `meshesMade` how many bodies were meshed rather than read from the store.
// On a refusal or a failed mesh the study's error is written on `sim`.
// `ctx.sim` is ignored.
bool MeshSimStudy(SimDocument& sim, SimContext ctx, uint64_t studyId,
                  std::vector<StudyBodyMesh>* out, std::string* error,
                  int* meshesMade = nullptr,
                  std::vector<MeshBodyReport>* reports = nullptr);

// Everything a run of study `studyId` would refuse
// (docs/simulation/README.md#checking-a-study). On the part route,
// CheckStudy on the part study it plans as (its siblings the
// simulation's); beside a neighbour what the assembly check reads -- a study
// with no loads, a topology study, and, with `backstop`, the assembly
// planner's refusal (it imprints, so a panel drawing each frame goes
// without). A refusal of the route itself is a `plan` problem.
std::vector<StudyProblem>
CheckSimStudy(const SimContext& ctx, uint64_t studyId,
              const std::vector<CheckMesh>* meshes = nullptr,
              bool backstop = true);

} // namespace fem
