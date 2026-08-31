#pragma once

#include "cad/CADKernel.h"
#include "core/io/ImportAssembly.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace ui {

struct AppState;

// A STEP import running off the UI thread, plus the sliced materialization
// that follows it (interchange I2, docs/interchange/import-plan.md).
//
// Importing a real assembly STEP is the app's second genuinely long-running
// operation. A 48 MB / 299-product file froze the window for ~38 s with no
// indication that anything was happening. The split here follows the same rule
// TopoJob and SyncJob follow: the worker touches ONLY pure data, and every
// Document mutation stays on the main thread.
//
//   worker (Phase::Reading, Phase::Transferring, Phase::Extracting)
//     LoadSTEPStructured + BuildAssemblyImportPlan — parse, bbox, write the
//     .brep sidecars. No AppState, no Document. ~14 s of the import.
//   main thread (Phase::Materializing), a slice per frame
//     create the part tabs and the assembly. ~2 s, and it must be sliced
//     because it is exactly the thing the progress bar exists to survive.
//
// One at a time; StartAssemblyImport refuses while another runs.
struct ImportJob {
  enum class Phase {
    Reading,       // ReadFile — no progress available from OCC
    Transferring,  // XCAF transfer — real percentage
    Extracting,    // per-product bbox + SaveBREP
    Materializing, // main thread: tabs + occurrences, sliced
    Done,
  };

  // What the parse decided this file is — the same question ImportCADFile
  // answers synchronously, moved onto the worker because ANSWERING it is the
  // expensive part (it needs the whole product structure).
  enum class Kind {
    Assembly, // several placed instances: part tabs + an assembly
    Feature,  // one product: an ImportFeature in the active part
  };

  // Copied in before the worker starts; never written by the main thread
  // while it runs.
  std::string stepPath;
  std::string destDir;
  std::string assemblyName;

  // Published by the worker, read by the main thread every frame. `fraction`
  // is < 0 while a phase cannot report a percentage.
  std::atomic<int> phase{static_cast<int>(Phase::Reading)};
  std::atomic<double> fraction{-1.0};
  std::atomic<int> partsDone{0};
  std::atomic<int> partsTotal{0};
  std::atomic<bool> cancel{false}; // main sets -> worker stops
  std::atomic<bool> done{false};   // worker sets last

  // Written by the worker, read by the main thread only after `done` (and the
  // join). The plan is what materialization consumes.
  interchange::AssemblyImportPlan plan;
  Kind kind = Kind::Assembly;
  // Kind::Feature only: the geometry the worker already parsed, handed to the
  // ImportFeature so its first regen does not read the file a second time.
  CADKernel::ImportedModel model;
  std::string error;

  // Main-thread materialization cursor: how many of plan.parts have become
  // tabs. Not atomic — only the main thread touches it, after the join.
  bool joined = false;
  size_t materializedParts = 0;
  std::vector<uint64_t> partIds;

  std::thread worker;

  // A joinable thread that destructs calls std::terminate, so quitting
  // mid-import would take the app down rather than close it. Cancel and wait:
  // the worker checks `cancel` between products and inside the XCAF transfer,
  // so this is bounded by one product's extraction.
  ~ImportJob() {
    cancel.store(true, std::memory_order_relaxed);
    if (worker.joinable())
      worker.join();
  }
  ImportJob() = default;
  ImportJob(const ImportJob&) = delete;
  ImportJob& operator=(const ImportJob&) = delete;
};

// The background form of ui::ImportCADFile — THE import entry point for the
// GUI. Starts a worker for .step/.stp (the only format whose read is slow) and
// returns immediately; PollImport applies the result. Anything else (.brep,
// .stl) is fast and imports synchronously here, exactly as before.
//
// False with `error` set if an import is already running or the file could not
// be started. A true return does NOT mean the import succeeded — only that it
// is under way; failures surface through AppState::importError.
bool StartImport(AppState& state, const std::string& path, std::string* error);

// Called once a frame from the main loop. Joins a finished worker and
// advances the materialization by one slice. No-op when nothing is running.
void PollImport(AppState& state);

// Is an import in flight (so the modal should be up and the file dialog should
// refuse a second one)?
bool ImportRunning(const AppState& state);

// A line for the modal: what is happening right now.
std::string ImportStatusLine(const AppState& state);

} // namespace ui
