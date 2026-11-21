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

#include "core/fem/FemSolver.h"
#include "core/fem/SimDocument.h"

#include <functional>
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

// Plans and solves study `studyId` of `sim` (a static, modal, buckling or
// thermal study; a topology study is refused), storing the result on `sim`
// stamped with its input hash and `ctx.subjectStamps`, and every mesh it
// made under its key. `rt` is the run's whole answer, before the reduction
// a stored result gets (KeepWhatIsRead). `meshesMade`, when given, is how
// many bodies were meshed rather than read from the store. A study on the
// document's mesh (SimDocument::SharesMesh) sets the document's mesh keys
// to the ones it solved on. The study's error is written on `sim`
// (StudyForRun). `ctx.sim` is ignored.
bool RunSimStudy(SimDocument& sim, SimContext ctx, uint64_t studyId,
                 StudyRuntime& rt, std::string* error,
                 int* meshesMade = nullptr);

} // namespace fem
