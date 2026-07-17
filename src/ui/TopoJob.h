#pragma once

// A running (or just-finished) topology-optimization job: the worker thread
// plus the atomics it publishes progress through. The app's first genuinely
// long-running operation. The optimizer runs over an immutable `fem::TopoInput`
// snapshot (no Document access), so the only cross-thread surface is these
// atomics + a single join on the main thread (see SimulationActions:
// Start/Poll/Cancel TopologyRun). Owned by AppState (one run at a time).

#include "core/FemTopo.h"

#include <atomic>
#include <cstdint>
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
  std::atomic<bool> cancel{false};  // main sets -> worker stops (keeps best)
  std::atomic<bool> done{false};    // worker sets when finished/cancelled/failed

  // The immutable input the worker optimizes over; the result/error it writes.
  // Read by the main thread only after `done` (and the join).
  fem::TopoInput input;
  std::optional<fem::TopoResult> result;
  std::string error;

  std::thread thread;

  // On teardown (or replacing the job), stop the worker and join — a joinable
  // std::thread that is destroyed would std::terminate the process.
  ~TopoJob() {
    cancel.store(true);
    if (thread.joinable()) thread.join();
  }
};

}  // namespace ui
