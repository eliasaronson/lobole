// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// A running (or just-finished) topology-optimization job: the worker thread
// plus the atomics it publishes progress through. The app's first genuinely
// long-running operation. The optimizer runs over an immutable `fem::TopoInput`
// snapshot (no Document access), so the only cross-thread surface is these
// atomics + a single join on the main thread (see SimulationActions:
// Start/Poll/Cancel TopologyRun). Owned by AppState (one run at a time).

#include "core/fem/FemSolver.h"
#include "core/fem/FemTopo.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ui {

// The FEM jobs' payloads (docs/ui/long-operations.md#fem-meshing-and-solving).
// The thread, `done` and `cancel` live on the registry's BackgroundJob; these
// are what the worker reads and writes and what the panel's status reads while
// it runs. Each remembers the PART SLOT it was started from, so the result is
// stored into that document even if another tab is active when it lands —
// the case step 0 could not fix while the polls took "the active document".
struct TopoJob {
  uint64_t studyId = 0;
  uint64_t partSlotId = 0;
  int maxIterations = 0;
  std::atomic<int> iteration{0};
  std::atomic<double> compliance{0.0};
  std::atomic<double> maxChange{0.0};
  std::atomic<double> volumeFraction{0.0};
  fem::TopoInput input;
  std::optional<fem::TopoResult> result;
  std::string error;
  std::unique_ptr<fem::TopoLinearSolver> solver;
  std::string solverName; // "gpu: <device>" or "cpu (<why>)" — set pre-launch
  // Set by the worker when the external solver is dropped mid-run: the
  // reason is written once, before `fellBack` is released, and never again.
  std::string fallbackReason;
  std::atomic<bool> fellBack{false};
  std::chrono::steady_clock::time_point started;
};

// A static/modal solve. The plan is made on the main thread (PlanStudy);
// the worker meshes it (MeshPlannedStudy) and solves — so a fine mesh no
// longer holds a frame either.
struct StudyJob {
  uint64_t studyId = 0;
  uint64_t partSlotId = 0;     // a part's study: the tab it ran on
  uint64_t assemblySlotId = 0; // an assembly's study (S1): its tab instead
  // An assembly study's stale record, stamped at plan time (the versions of
  // the parts the plan read) and stored with the result.
  std::vector<std::pair<uint64_t, uint64_t>> partVersions;
  // Written by main before the worker starts; only read after that, by both.
  fem::StudyPlan plan;
  // The worker's: it rebuilds it wholesale. Main reads it after the join.
  fem::StudyJobInput input;
  fem::StudyRuntime runtime; // read by main only after the join
  std::string error;
  std::chrono::steady_clock::time_point started;
  // The linear solver the worker chose, once the mesh said how big K is (a
  // static string; null until then), for the status of a running solve.
  std::atomic<const char*> solverNow{nullptr};
  // A CalculiX run's memory estimate (bytes), once its mesh said how big
  // it is (docs/simulation/README.md#calculix); 0 until then, and in-house.
  std::atomic<uint64_t> memoryEstimate{0};
};

// mesh_study off the frame: the same plan/mesh split, no solve. What it
// found is kept as a MeshReport keyed by (part slot, study) so study_status
// can answer for it.
struct MeshJob {
  uint64_t studyId = 0;
  uint64_t partSlotId = 0;
  fem::MeshPlan plan;
  std::vector<fem::StudyBodyMesh> meshes;
  std::string error;
  std::chrono::steady_clock::time_point started;
};

struct MeshReport {
  struct Body {
    std::string name;
    size_t nodes = 0;
    size_t elements = 0;
    double worstQuality = 0.0;
  };
  std::vector<Body> bodies;
  std::string error; // non-empty = the meshing failed, and why
  double seconds = 0.0;
};

} // namespace ui
