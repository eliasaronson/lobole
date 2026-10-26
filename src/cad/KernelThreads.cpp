// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "cad/KernelThreads.h"

#include "common/Logger.h"

#include <Standard_ErrorHandler.hxx>

#include <algorithm>
#include <cstdlib>
#include <thread>

namespace cad {

bool PerThreadErrorHandlers() {
  // Asked of the library this process runs, not of the version it was
  // built against or the patch list the install names: a handler made on
  // one thread is unlinked from another. A shared stack lets the other
  // thread find it and take it out, and its own thread no longer sees a
  // handler; a per-thread one does not hold it there, and its thread still
  // does. Either way the handler is then destroyed on its own thread, which
  // finds it gone (shared) or the top of its own stack (per thread).
  static const bool perThread = [] {
    bool stillThere = true;
    std::thread own([&] {
      Standard_ErrorHandler handler;
      std::thread other([&] { handler.Unlink(); });
      other.join();
      stillThere = Standard_ErrorHandler::IsInTryBlock();
    });
    own.join();
    if (!stillThere)
      LOG_INFO("OpenCASCADE keeps one error-handler stack for every thread: "
               "booleans run on half the hardware threads "
               "(docs/performance/README.md#the-kernels-error-handler-lock)");
    return stillThere;
  }();
  return perThread;
}

size_t BooleanWorkers(size_t budget, size_t hardwareThreads, bool perThread) {
  if (perThread)
    return budget;
  return std::max<size_t>(1, std::min(budget, (hardwareThreads + 1) / 2));
}

size_t BooleanWorkers(size_t budget) {
  // A pinned count is the caller's to choose: the gate that diffs one
  // thread against all of them, or a measurement.
  if (std::getenv("LOBOLE_SWEEP_THREADS") != nullptr)
    return budget;
  return BooleanWorkers(budget,
                        std::max(1u, std::thread::hardware_concurrency()),
                        PerThreadErrorHandlers());
}

} // namespace cad
