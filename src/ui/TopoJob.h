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
#include <thread>

namespace ui {

struct TopoJob {
  uint64_t studyId = 0;
  int maxIterations = 0;

  // Published by the worker each iteration, read by the main thread.
  std::atomic<int> iteration{0};
  std::atomic<double> compliance{0.0};
  std::atomic<double> maxChange{0.0};
  std::atomic<double> volumeFraction{0.0};
  std::atomic<bool> cancel{false}; // main sets -> worker stops (keeps best)
  std::atomic<bool> done{false};   // worker sets when finished/cancelled/failed

  // The immutable input the worker optimizes over; the result/error it writes.
  // Read by the main thread only after `done` (and the join).
  fem::TopoInput input;
  std::optional<fem::TopoResult> result;
  std::string error;

  // The linear solver for the run (the GPU seam), created on the MAIN thread
  // in StartTopologyRun (so solverName is set before the worker exists), then
  // used only by the worker — Vulkan allows the handoff as long as a single
  // thread uses the context at a time. Null = the built-in CPU path.
  std::unique_ptr<fem::TopoLinearSolver> solver;
  std::string solverName; // "gpu: <device>" or "cpu (<why>)" — set pre-launch
  std::chrono::steady_clock::time_point started;

  std::thread thread;

  // On teardown (or replacing the job), stop the worker and join — a joinable
  // std::thread that is destroyed would std::terminate the process.
  ~TopoJob() {
    cancel.store(true);
    if (thread.joinable())
      thread.join();
  }
};

// A running static/modal solve on a worker thread (the async path that keeps
// a minutes-long CG solve from blocking the app and timing out remote
// clients). Same immutable-snapshot rule as TopoJob: PrepareStudy runs on
// the main thread, the worker touches only pure fem data. NOT cancellable —
// the linear solver has no interruption points; teardown blocks on the join.
struct StudyJob {
  uint64_t studyId = 0;
  fem::StudyJobInput input;
  fem::StudyRuntime runtime; // read by main only after `done` + join
  std::string error;
  std::atomic<bool> done{false};
  std::chrono::steady_clock::time_point started;
  std::thread thread;

  ~StudyJob() {
    if (thread.joinable())
      thread.join();
  }
};

} // namespace ui
