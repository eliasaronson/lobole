// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "ui/BackgroundJobs.h"

#include "render/MeshRenderer.h"
#include "render/SketchRenderer.h"
#include "ui/AppState.h"

// FEM/topology study runs — arming, starting, polling and
// cancelling them, plus the simulation display mode.
//
// Split out of ui/AppActions.h, which had grown to ~180 declarations spanning
// every domain in the app and is included by 40 translation units — so every
// panel and RPC module paid for all of it. See
// docs/architecture.md#the-ui-actions-layer.
namespace ui {

// Live status of the active run. `running` is false when no job is running for
// `studyId` (finished/never-started — read the cached result instead).
struct TopoRunStatus {
  bool running = false;
  int iteration = 0;
  int maxIterations = 0;
  double compliance = 0.0;
  double maxChange = 0.0;
  double volumeFraction = 0.0;
  std::string solver;          // "gpu: <device>" or "cpu (<why>)"
  double elapsedSeconds = 0.0; // since the run started
};

// running=false when no static/modal job is active for `studyId`;
// elapsedSeconds since launch otherwise.
struct StudyRunStatus {
  bool running = false;
  double elapsedSeconds = 0.0;
  const char* solver = ""; // fem::CpuSolverName of the running study's type
  // A CalculiX run's memory estimate (bytes) once its mesh said how big it
  // is; 0 otherwise (docs/simulation/README.md#calculix).
  uint64_t memoryEstimateBytes = 0;
};

// Enter/exit the FEM simulation workspace (a mode, like sketch mode). Enter
// arms `state.simulation.active` and selects (creating if none) a study; exit
// restores any hidden body instances and clears the mode.
// Arms a boundary-condition tool (the panel and the toolbar share this so
// they cannot drift): seeds an unset Force/Pressure magnitude from the part's
// own weight, so the tool never places a load of 0 that silently does
// nothing.
void SimulationArmTool(AppState& state, SimulationState::Tool tool);
// The weight (N) of what the active study solves -- the part, or the meshed
// occurrence -- that the load tools seed from; 0 when no material says.
double SimulationWeightN(AppState& state);

// ---- Studies on an ASSEMBLY (docs/simulation/README.md#assembly-studies)
// The assembly document does not own its parts, so everything that places
// geometry or reads a part's version goes through the app state.
//
// The geometry source for fem::PlanAssemblyStudy: an occurrence's bodies in
// world space (OccurrenceSubtreeBodies) and the project's variables.
fem::AssemblyStudyContext AssemblyStudyContextFor(AppState& state);
// The version of every part the study's participants reference, which is
// what a result is stamped with and compared against.
std::vector<std::pair<uint64_t, uint64_t>>
AssemblyStudyPartVersions(AppState& state, const AssemblyDocument& asmDoc,
                          const fem::Study& study);
// Stale against the assembly AND its participants' parts.
bool AssemblyStudyStale(AppState& state, const AssemblyDocument& asmDoc,
                        uint64_t studyId);
// Plan + mesh + solve on this thread, stamping and storing the result.
// `timeoutSeconds` bounds a CalculiX run (0 = none).
bool RunAssemblyStudyAndCache(AppState& state, AssemblyDocument& asmDoc,
                              uint64_t studyId, std::string* error,
                              double timeoutSeconds = 0.0,
                              const JobStart* job = nullptr);
// The same on a worker: the plan (imprint included) is made here, the mesh
// and solve run on the thread, and the result lands on the assembly tab it
// was started from when the job is reaped.
bool StartAssemblyStudyRun(AppState& state, AssemblyDocument& asmDoc,
                           uint64_t studyId, std::string* error,
                           JobStart* job = nullptr);
// Plan + mesh only, for mesh_study on an assembly tab.
bool MeshAssemblyStudy(AppState& state, AssemblyDocument& asmDoc,
                       uint64_t studyId, std::vector<fem::StudyBodyMesh>& out,
                       std::string* error);
// The stable id of the active tab's slot (what a background job keys on).
uint64_t ActiveSlotId(const AppState& state);

void EnterSimulation(AppState& state);

void ExitSimulation(AppState& state);
// The display half of ExitSimulation, for a tab change: puts back the bodies
// a field hid, removes the topology Shape instance, clears the field mesh
// and resets the mode -- leaving the Simulation window as it was.
void ResetSimulationDisplay(AppState& state);

// Meshes + solves study `id` on the active document and caches the results
// (stamped with the current Version). Returns false and fills *error on any
// failure — the one source of truth shared by the panel Run button and the
// run_study RPC. Defined in AppActions.cpp.
bool RunStudyAndCache(Document& doc, uint64_t studyId, std::string* error,
                      const fem::ConvergeOptions& converge = {});
// The same with what a CalculiX study needs from the app (the solver found,
// its cache directory), `timeoutSeconds` (0 = none; CalculiX runs only) and
// the run's convergence options from `job` (ConvergeOptionsFor).
bool RunStudyAndCache(AppState& state, Document& doc, uint64_t studyId,
                      std::string* error, double timeoutSeconds,
                      const JobStart* job = nullptr);

// The CalculiX run study `s` needs (docs/simulation/README.md#calculix),
// into `options`: nothing for an in-house study; for a CalculiX one, the
// ccx found (the calculix_path setting, LOBOLE_CCX, beside the executable,
// PATH), its run directory under the cache, the thread budget and
// `timeoutSeconds`. False with `error` when it is refused: a solver that
// cannot solve this type, no ccx (naming every way to install it), or a
// timeout on an in-house study. Nothing is started either way.
bool CalculixRunFor(const AppState& state, const fem::Study& s,
                    double timeoutSeconds, fem::SolveOptions& options,
                    std::string* error);

// How a run of `study` checks its peak has settled
// (docs/simulation/README.md#a-peak-that-has-settled): the study's
// `converge` unless `job` overrides it, the converge_auto_s preference, and
// the job's layer normal.
fem::ConvergeOptions ConvergeOptionsFor(const AppState& state,
                                        const fem::Study& study,
                                        const JobStart* job);

// --- Results keyed by inputs (docs/simulation/README.md#studies-as-jobs) ---
//
// The input hash of study `studyId` on the part `doc` or, with `doc` null,
// the assembly `asmDoc` (fem::StudyInputKey / AssemblyStudyInputKey).
// A run's convergence options (`job`) are inputs too (fem::RunInputKey).
uint64_t StudyInputHash(AppState& state, const Document* doc,
                        const AssemblyDocument* asmDoc, uint64_t studyId,
                        const JobStart* job = nullptr);
// A result for exactly the study's current inputs -- its own (a reopened
// topology result carries the hash it was saved with) or one the session
// holds -- installed as the study's fresh result, so nothing is computed;
// `job->reused` says so. False (and nothing changed) when there is none, or
// `job->recompute` asked for a run anyway. Every Start…Run asks this first.
bool ReuseStudyResult(AppState& state, Document* doc, AssemblyDocument* asmDoc,
                      uint64_t studyId, JobStart* job);
// Keep a finished result by its input hash (a cancelled one is not kept).
void RememberStudyResult(AppState& state, const fem::StudyRuntime& rt);

// True when this build can hand a topology run to the GPU solver (fem_gpu
// compiled into the file that starts the run). A test holds it to the build
// option: for a month it was false in every build, and every run went to
// the CPU at a few minutes an iteration without a word.
bool TopologyGpuBuiltIn();

// Prepares the snapshot (meshes on the calling thread) and spawns the worker.
// Returns false + *error on a prep failure or if a run is already in progress.
bool StartTopologyRun(AppState& state, Document& doc, uint64_t studyId,
                      std::string* error, JobStart* job = nullptr);

// If a run has finished (converged, capped, cancelled, or errored), joins it
// and stores the result (or the study error). Cheap no-op otherwise — call it
// each frame and before reporting study status. Defined in
// SimulationActions.cpp.
// (No PollTopologyRun since A3 phase 3: the registry joins the worker and
// the job's own `finish` stores the result into the part it was started
// from, whichever tab is active.)

// Requests cancellation of the run for `studyId` (keeps best-so-far). Returns
// false if no such run is active.
bool CancelTopologyRun(AppState& state, uint64_t studyId);

TopoRunStatus GetTopologyStatus(const AppState& state, uint64_t studyId);

// The async static/modal counterparts (StudyJob): prepare-on-main +
// solve-on-worker, polled at the same sites as the topology run. A cancel
// lands at a boundary (after the meshing, inside an iterative solve; during
// a direct factorization, which has no interruption points, when it ends)
// and drops the answer.
bool StartStudyRun(AppState& state, Document& doc, uint64_t studyId,
                   std::string* error, JobStart* job = nullptr);

// The two static studies check_topology solves
// (docs/topopt/README.md#a-static-study-of-the-optimised-shape): '<name>:
// shape' (the result at `threshold`, 0 = volume-matched) and '<name>: solid'
// (its whole design domain), with the topology study's supports, loads and
// cases. Made when absent, refreshed when present (found by what they are,
// not by name), each one undoable edit. False + *error when `topoId` is not
// a topology study of `doc`.
bool EnsureTopologyCheckStudies(AppState& state, Document& doc, uint64_t topoId,
                                double threshold, uint64_t* shapeId,
                                uint64_t* solidId, std::string* error);

// check_topology as a job (kind "check"): makes the two studies above, and
// solves on a worker whichever of them is not answered by a stored result
// for its inputs -- the shape's, then the solid's, a cancel landing between
// them or after a factorization. When both are answered, nothing starts and
// `job->reused` is set. One study run at a time.
bool StartTopologyCheck(AppState& state, Document& doc, uint64_t topoId,
                        double threshold, uint64_t* shapeId, uint64_t* solidId,
                        std::string* error, JobStart* job = nullptr);

// After an open (and a tab switch to an assembly): every stored study and
// motion result whose inputs hash as they did when it was made reads
// current, and the active assembly's saved motion results are the ones
// MotionResultFor answers (docs/simulation/README.md#studies-as-jobs).
void AdoptStoredResults(AppState& state);

// mesh_study off the frame: plan on this thread, mesh on a worker, and keep
// what it found as a MeshReport for study_status (`meshing: true` while it
// runs, then `mesh: {...}`). One mesh run at a time, per study.
bool StartMeshRun(AppState& state, Document& doc, uint64_t studyId,
                  std::string* error, JobStart* job = nullptr);
struct MeshRunStatus {
  bool running = false;
  double elapsedSeconds = 0.0;
};
MeshRunStatus GetMeshRunStatus(const AppState& state, uint64_t studyId);
// The last finished mesh run's report for `studyId` of `doc`, or null.
const MeshReport* MeshReportFor(const AppState& state, const Document& doc,
                                uint64_t studyId);

StudyRunStatus GetStudyRunStatus(const AppState& state, uint64_t studyId);

// A world-space cross at the peak of the displayed result field (max von Mises
// or max displacement), warped by the deform scale. Pushed into the no-depth
// gizmo channel like the CoG marker. Defined in AppActions.cpp.
void AppendStudyMarker(const AppState& state, std::vector<LineVertex3D>& lines);

// Rebuilds the FEM field overlay from the active study's cached results and
// toggles the solved bodies' normal render (hidden while a field is shown,
// restored otherwise). Cheap and idempotent — the vertex rebuild is gated on
// a change signature; the deform scale rides a push constant (no rebuild).
// Called every frame. Defined in AppActions.cpp.
void SyncFieldRenderer(AppState& state);

} // namespace ui
