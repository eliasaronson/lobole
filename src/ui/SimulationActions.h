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
  // A nonlinear run's load reached over the load it runs to; -1 = none yet.
  double loadFraction = -1.0;
  // An in-house iterative solve's iterations so far, CPU or GPU; 0 before
  // one starts (and for a direct one).
  int iterations = 0;
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

// The stable id of the active tab's slot (what a background job keys on).
uint64_t ActiveSlotId(const AppState& state);

void EnterSimulation(AppState& state);

void ExitSimulation(AppState& state);
// The display half of ExitSimulation, for a tab change: puts back the bodies
// a field hid, removes the topology Shape instance, clears the field mesh
// and resets the mode -- leaving the Simulation window as it was.
void ResetSimulationDisplay(AppState& state);

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
// Keep a finished result by its input hash (a cancelled one is not kept):
// what ReuseSimStudyResult answers a run with.
void RememberStudyResult(AppState& state, const fem::StudyRuntime& rt);

// True when this build can hand a topology run to the GPU solver (fem_gpu
// compiled into the file that starts the run). A test holds it to the build
// option: for a month it was false in every build, and every run went to
// the CPU at a few minutes an iteration without a word.
bool TopologyGpuBuiltIn();

// Requests cancellation of the run for `studyId` (keeps best-so-far). Returns
// false if no such run is active.
bool CancelTopologyRun(AppState& state, uint64_t studyId);

// Status is keyed by (simulation slot, study): study ids are unique only
// within their document, so a run on one simulation never reads as another's.
TopoRunStatus GetTopologyStatus(const AppState& state, uint64_t studyId,
                                uint64_t simSlotId = 0);

struct MeshRunStatus {
  bool running = false;
  double elapsedSeconds = 0.0;
};
MeshRunStatus GetMeshRunStatus(const AppState& state, uint64_t studyId,
                               uint64_t simSlotId = 0);
StudyRunStatus GetStudyRunStatus(const AppState& state, uint64_t studyId,
                                 uint64_t simSlotId = 0);

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
