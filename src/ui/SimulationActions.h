#pragma once

#include "render/MeshRenderer.h"
#include "render/SketchRenderer.h"
#include "ui/AppState.h"

// FEM/topology study runs — arming, starting, polling and
// cancelling them, plus the simulation display mode.
//
// Split out of ui/AppActions.h, which had grown to ~180 declarations spanning
// every domain in the app and is included by 40 translation units — so every
// panel and RPC module paid for all of it. See
// docs/architecture-review-2026-08-24.md.
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
};

// Enter/exit the FEM simulation workspace (a mode, like sketch mode). Enter
// arms `state.simulation.active` and selects (creating if none) a study; exit
// restores any hidden body instances and clears the mode.
// Arms a boundary-condition tool (the panel and the toolbar share this so
// they cannot drift): seeds an unset Force/Pressure magnitude from the part's
// own weight, so the tool never places a load of 0 that silently does
// nothing.
void SimulationArmTool(AppState& state, SimulationState::Tool tool);

void EnterSimulation(AppState& state);

void ExitSimulation(AppState& state);

// Meshes + solves study `id` on the active document and caches the results
// (stamped with the current Version). Returns false and fills *error on any
// failure — the one source of truth shared by the panel Run button and the
// run_study RPC. Defined in AppActions.cpp.
bool RunStudyAndCache(Document& doc, uint64_t studyId, std::string* error);

// Prepares the snapshot (meshes on the calling thread) and spawns the worker.
// Returns false + *error on a prep failure or if a run is already in progress.
bool StartTopologyRun(AppState& state, Document& doc, uint64_t studyId,
                      std::string* error);

// If a run has finished (converged, capped, cancelled, or errored), joins it
// and stores the result (or the study error). Cheap no-op otherwise — call it
// each frame and before reporting study status. Defined in
// SimulationActions.cpp.
void PollTopologyRun(AppState& state, Document& doc);

// Requests cancellation of the run for `studyId` (keeps best-so-far). Returns
// false if no such run is active.
bool CancelTopologyRun(AppState& state, uint64_t studyId);

TopoRunStatus GetTopologyStatus(const AppState& state, uint64_t studyId);

// The async static/modal counterparts (StudyJob): prepare-on-main +
// solve-on-worker, polled at the same sites as the topology run. A static
// solve is NOT cancellable (no interruption points in the linear solver).
bool StartStudyRun(AppState& state, Document& doc, uint64_t studyId,
                   std::string* error);

void PollStudyRun(AppState& state, Document& doc);

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
