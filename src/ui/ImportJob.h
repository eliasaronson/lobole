// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "cad/CADKernel.h"
#include "core/Units.h"
#include "core/io/ImportAssembly.h"
#include "core/io/MeshImportCost.h"
#include "core/mesh/Representation.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace ui {

struct AppState;

// What to import and how: every import that is not a sketch's (File >
// Import, `import_step`, `--file`) is one of these, run as the "import" job
// (docs/ui/long-operations.md#import).
struct ImportRequest {
  enum class Mode {
    Auto,     // File > Import: a STEP/IGES with several instances is an
              // assembly, anything else a feature
    Feature,  // an Import feature in the part, whatever the file holds
    Assembly, // part tabs + an assembly (`as_assembly`)
  };
  std::string path;
  Mode mode = Mode::Auto;
  units::Length unit = units::Length::Millimetre; // a mesh's or BREP's
  bool keepOrigin = false;
  interchange::UpAxis up = interchange::UpAxis::Auto;
  double turnDeg = 0.0;
  // What a mesh becomes (ImportFeature::representation, minSides): absent,
  // the preference, which StartImport resolves before the worker reads.
  std::optional<mesh::Representation> representation;
  int minSides = mesh::kDefaultMinSides;
  // Past interchange::MeshTriangleLimit a mesh is refused unless this says
  // the caller knows what it costs.
  bool allowLargeMesh = false;
  // How the caller spells that override, for the refusal that names it.
  std::string overrideName = "--allow-large-mesh";
  // The RPC connection the job dies with (0: the GUI or --file).
  uint64_t owner = 0;
};

// An import running off the UI thread, plus the sliced materialization
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
    Building,      // a mesh's triangles made faces — real percentage
    Sewing,        // a mesh's faces sewn — real percentage
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
  ImportRequest request;
  std::string stepPath; // request.path
  std::string destDir;
  std::string assemblyName;
  // The part tab it was started from, which a feature import lands in
  // whichever tab is active when it lands; and how many tabs there were.
  uint64_t partId = 0;
  size_t tabsBefore = 0;
  // A mesh's count and expected cost, measured before the job started.
  std::optional<interchange::MeshCost> mesh;

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
  interchange::UpDecision decided; // an assembly's up axis, as applied

  // How it ended, written by the finish on the main thread (an RPC waiting
  // on the import reads it there too). `landedError` is why it failed.
  enum class Outcome { Running, Landed, Failed, Cancelled };
  Outcome outcome = Outcome::Running;
  std::string landedError;
  // Wall and CPU seconds the worker spent reading (logged, and the measure
  // the mesh cost law was fitted to).
  double workerSeconds = 0.0;
  double workerCpuSeconds = 0.0;
};

// The thread, `cancel` and `done` live on the registry's "import"
// BackgroundJob since A3 phase 4 (ui/BackgroundJobs.h); this struct is the
// worker's whole world plus what the modal reads while it runs. The sliced
// materialize is the job's `finish`, returning false until the last part.

// Starts `request` as the "import" job, or refuses, naming why: another
// import running, no part to land in, a feature limit, a mesh past the
// triangle limit without `allowLargeMesh`. A DXF or SVG goes into the open
// sketch at once, as before. `started` (optional) receives the job's payload.
bool StartImport(AppState& state, const ImportRequest& request,
                 std::string* error,
                 std::shared_ptr<ImportJob>* started = nullptr);
// File > Import's form: Auto, with the defaults.
bool StartImport(AppState& state, const std::string& path, std::string* error);
// File > Import from a person: as StartImport, except that a mesh past the
// limit is ASKED about (AppState::importAsk, the import modal) rather than
// refused.
bool RequestImport(AppState& state, const std::string& path,
                   std::string* error);

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
