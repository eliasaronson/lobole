// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// A simulation tab's studies run through the app
// (docs/simulation/README.md#running-a-simulations-study): the subject
// resolved from the project as it is now (SimContextFor), and every run keyed
// by the SIMULATION SLOT's id, so its answer lands in that simulation even
// when another tab is in front by then, and is dropped when the tab is gone.
// The same runs the part and the assembly have (SimulationActions.h): the
// synchronous one with reuse, remember, CalculiX and convergence, the run off
// the frame, a topology run, meshing alone, a check.

#include "core/fem/FemSolver.h"
#include "core/fem/SimStudy.h"

#include <cstdint>
#include <string>
#include <vector>

namespace fem {
class SimDocument;
}

namespace ui {

struct AppState;
struct JobStart;
struct MeshReport;

// The simulation a slot holds, active or stashed, by the slot's stable id.
// Null when the slot is gone or is not a simulation.
fem::SimDocument* SimBySlotId(AppState& state, uint64_t simSlotId);
// The slot id of the simulation tab in front; 0 on any other tab.
uint64_t ActiveSimSlotId(const AppState& state);

// What `sim`'s studies are planned against, read from the project now: the
// part subject in the simulation's configuration, or the assembly subject
// flattened once (placedBodies, partOf with each occurrence's placement in
// it); the symbols the subject's studies read; and every subject document's
// stamp (SimStampOf). The context reads `state` and the flattening it holds:
// use it before anything changes a tab, and do not keep it.
fem::SimContext SimContextFor(AppState& state, const fem::SimDocument& sim);
// The stamp a result of a simulation is solved against, for one subject
// document: a part's PartStamp, an assembly's version. 0 when it is gone.
uint64_t SimStampOf(AppState& state, uint64_t docId);
// Stale against its own study, the simulation's settings, and every subject
// document it was solved against.
bool SimStudyStale(AppState& state, const fem::SimDocument& sim,
                   uint64_t studyId);

// A run's input hash: SimStudyInputKey with a JobStart's convergence mode in
// place of the study's, the layer normal mixed in (RunInputKey). 0 when the
// study would be refused.
uint64_t SimStudyInputHash(AppState& state, const fem::SimDocument& sim,
                           uint64_t studyId, const JobStart* job = nullptr);
// The study's own result or the session's for these very inputs, stored on
// the simulation without a solve (ReuseStudyResult's rule). `job->reused`.
bool ReuseSimStudyResult(AppState& state, uint64_t simSlotId, uint64_t studyId,
                         JobStart* job);

// Solves on this thread, as RunStudyAndCache does: CalculiX where the study
// asks for it (timeoutSeconds bounds that run), the GPU offered, the run's
// convergence, the result remembered for reuse and stored on the simulation
// with every mesh it made. A topology study runs its optimisation.
bool RunSimStudyAndCache(AppState& state, uint64_t simSlotId, uint64_t studyId,
                         std::string* error, double timeoutSeconds = 0.0,
                         const JobStart* job = nullptr);
// Off the frame (StartStudyRun): planned here, meshed and solved on a
// worker, stored when it lands. A topology study is StartSimTopologyRun's.
bool StartSimStudyRun(AppState& state, uint64_t simSlotId, uint64_t studyId,
                      std::string* error, JobStart* jobStart = nullptr);
bool StartSimTopologyRun(AppState& state, uint64_t simSlotId, uint64_t studyId,
                         std::string* error, JobStart* jobStart = nullptr);

// Meshing alone (fem::MeshSimStudy): every mesh made stored on the simulation
// and the report kept under {simSlotId, studyId}, as a part's mesh is.
bool MeshSimStudyNow(AppState& state, uint64_t simSlotId, uint64_t studyId,
                     std::string* error);
bool StartSimMeshRun(AppState& state, uint64_t simSlotId, uint64_t studyId,
                     std::string* error, JobStart* jobStart = nullptr);
// What a kept mesh report of a simulation study is checked against: the
// study's effective mesh settings and the subject documents' stamps (cheap:
// read each frame the panel draws). Either moving makes the mesh stale.
std::string SimMeshKeyNow(AppState& state, const fem::SimDocument& sim,
                          const fem::Study& study);
const MeshReport* SimMeshReportFor(const AppState& state, uint64_t simSlotId,
                                   uint64_t studyId);

// Everything a run would refuse (fem::CheckSimStudy), against the stored
// mesh report when there is one.
std::vector<fem::StudyProblem>
CheckSimStudyNow(AppState& state, uint64_t simSlotId, uint64_t studyId);

// The panel's and the toolbar's Run on the simulation tab in front: a
// topology study through StartSimTopologyRun, any other StartSimStudyRun.
bool StartFrontSimRun(AppState& state, uint64_t studyId, std::string* error);

// check_topology on a simulation (EnsureTopologyCheckStudies' rule): the
// topology study's shape and solid static studies, made or brought up to
// date under SimSlotUndoGuard, then solved off the frame (kind "check")
// where no stored result answers them. `job->reused` when both are.
bool EnsureSimTopologyCheckStudies(AppState& state, uint64_t simSlotId,
                                   uint64_t topoId, double threshold,
                                   uint64_t* shapeId, uint64_t* solidId,
                                   std::string* error);
bool StartSimTopologyCheck(AppState& state, uint64_t simSlotId, uint64_t topoId,
                           double threshold, uint64_t* shapeId,
                           uint64_t* solidId, std::string* error,
                           JobStart* job = nullptr);

// The simulation tab analysing that document, its slot id: the first one,
// or (with `create`) a new one appended BEHIND the tab in front, named
// "<subject> simulation" made unique, in the subject's active configuration.
// 0 when there is none and none was made, or `docId` is not a part's or an
// assembly's tab. Nothing switches tabs.
uint64_t SimulationSlotFor(AppState& state, uint64_t docId, bool create);
// Every simulation tab analysing that document, by slot index.
std::vector<size_t> SimulationSlotsFor(const AppState& state, uint64_t docId);

// The open's move (docs/simulation/README.md#moving-studies-into-a-simulation):
// every part's and assembly's own studies go into new simulation tabs, added
// behind the tab in front (fem::SimulationsFromPart / SimulationsFromAssembly,
// ids and saved results kept), and the part's and assembly's lists are
// emptied. A study a simulation of that subject already holds (a recovery
// over a part saved before its studies moved) is not moved twice. Returns
// how many simulations it made. The caller stamps the project saved.
size_t MoveStudiesIntoSimulations(AppState& state);
// After an open: each simulation's stored results whose inputs hash as they
// did read current (fem::RestampSimByInputs against SimContextFor).
void AdoptStoredSimResults(AppState& state);

// The occurrence a body of SimSubjectBodies (the simulation tab's viewport
// bodies) belongs to: its top-level occurrence on an assembly subject, which
// is what a participant and a load's anchor name. 0 on a part subject, or
// out of range.
uint64_t SimBodyOccurrence(AppState& state, size_t bodyIndex);

} // namespace ui
