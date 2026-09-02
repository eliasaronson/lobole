#pragma once

// One long-running operation, as the registry sees it
// (docs/ui/long-operations.md, "The mechanism").
//
// Deliberately NOT a union of every job's needs. `fraction` and a `label`
// the domain writes are the common denominator; a topology run's compliance
// and an import's parts-done stay in their own structs, because a shared
// struct carrying every field any job might want is how the abstraction
// rots. The result type is likewise the domain's: it lives wherever the
// domain keeps it and is consumed by `finish`, which the registry runs on
// the main thread after the join. The registry never sees it.
//
// Cancellation is a REQUEST. A job that has no interruption points (libarea's
// Execute, the linear solver) ignores it and says so with `cancellable`;
// the request still stops whatever the domain would have started next.

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <thread>

namespace ui {

struct AppState;

struct BackgroundJob {
  std::string kind;  // "cam" | … — what `cancel_background_job` names
  std::string label; // "Clearing 'Pocket 1', pass 2 of 4" — shown as-is
  bool cancellable = false;
  std::atomic<double> fraction{-1.0}; // <0 = indeterminate, no bar
  std::atomic<bool> cancel{false};    // main sets; a job may ignore it
  std::atomic<bool> done{false};      // worker sets LAST
  std::string error;                  // read after done + join
  std::chrono::steady_clock::time_point started;
  std::thread thread;
  // Runs on the MAIN thread after the join. Returns false to be called again
  // next frame — which is how a sliced main-thread tail (ImportJob's
  // materialize) fits without a second mechanism.
  std::function<bool(AppState&)> finish;
  bool reaped = false; // finish returned true; dropped at the end of the poll

  BackgroundJob() = default;
  BackgroundJob(const BackgroundJob&) = delete;
  BackgroundJob& operator=(const BackgroundJob&) = delete;
  ~BackgroundJob() {
    // Closing the app or the tab mid-pass blocks for the remainder of THIS
    // job's work — one adaptive pass, not the operation — and is accepted
    // (docs/ui/long-operations.md, "Cancel, quit and close").
    cancel.store(true);
    if (thread.joinable())
      thread.join();
  }
};

} // namespace ui
