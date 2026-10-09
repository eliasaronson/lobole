// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

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
// that follows it (docs/interchange/README.md#importing-an-assembly).
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
  interchange::AssemblyImportPlan plan;
  Kind kind = Kind::Assembly;
  CADKernel::ImportedModel model;
  std::string error;
  // The main-thread half: set once the worker's result has been read and the
  // sliced materialize has begun (from then on cancel is refused).
  bool materializing = false;
  size_t materializedParts = 0;
  std::vector<uint64_t> partIds;
};

// The thread, `cancel` and `done` live on the registry's "import"
// BackgroundJob since A3 phase 4 (ui/BackgroundJobs.h); this struct is the
// worker's whole world plus what the modal reads while it runs. The sliced
// materialize is the job's `finish`, returning false until the last part.

bool StartImport(AppState& state, const std::string& path, std::string* error);

// Is an import in flight (so the modal should be up and the file dialog should
// refuse a second one)?
bool ImportRunning(const AppState& state);

// A line for the modal: what is happening right now.
std::string ImportStatusLine(const AppState& state);

// TEST ONLY: while held, a STEP import's worker waits before it reads the
// file (a cancel still ends the wait). A small file imports in a frame or
// two, and the modal's Cancel button exists only until the worker is done
// (the materialize drops it), so a GUI test asserting the modal "was seen"
// raced the worker and lost under load. Holding the worker makes "the modal
// is up while the import runs" a state the test reaches, not a race.
void HoldImportWorkerForTesting(bool hold);

} // namespace ui
