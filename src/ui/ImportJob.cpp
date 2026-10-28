// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/ImportJob.h"

#include "common/Logger.h"
#include "core/io/Autosaver.h"
#include "ui/AppState.h"
#include "ui/AssemblyActions.h"
#include "ui/BackgroundJobs.h"
#include "ui/DocumentSlots.h"
#include "ui/ImportActions.h"

#include "core/Feature.h"
#include "core/io/ReadContext.h"

#include "cad/CADKernel.h" // ArmOccSignalHandlers
#include <Standard_Failure.hxx>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <memory>
#include <utility>

namespace ui {

namespace {

// What the worker reads the file as: the request's unit and representation,
// and whether a recognised mesh past the triangle limit may still be sewn as
// facets -- only when the caller said so.
ImportFeature::ReadOptions ReadOptionsOf(const ImportRequest& rq) {
  ImportFeature::ReadOptions o;
  o.unit = rq.unit;
  o.representation = rq.representation.value_or(mesh::Representation::Facets);
  o.minSides = rq.minSides;
  o.facetsPastLimit = rq.allowLargeMesh;
  o.largeMeshOverride = rq.overrideName;
  return o;
}

} // namespace

namespace {

// How many part tabs to create per frame while materializing. Each is an
// AddPart + an ImportFeature regen off a small .brep — around 7 ms on the
// 299-part file, so 8 keeps a frame near 60 ms: slow enough to be one visible
// progress step, fast enough that 299 parts land in ~2.5 s.
constexpr size_t kPartsPerFrame = 8;

// HoldImportWorkerForTesting's gate.
std::atomic<bool> g_holdWorker{false};

// Seconds of CPU this thread has used: the cost law is fitted in CPU time,
// because the machine it was measured on is shared and wall time is not the
// import's alone.
double ThreadCpuSeconds() {
#if defined(CLOCK_THREAD_CPUTIME_ID)
  timespec ts{};
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0)
    return static_cast<double>(ts.tv_sec) + ts.tv_nsec * 1e-9;
#endif
  return 0.0;
}

// Where each phase of a mesh read sits on the one bar. What OpenCASCADE
// reports covers the first third of a read: the file, the faces, and the
// sewing's own stages. Most of a large mesh's time is the step after those,
// the sewing joining the faces into shells (BRepTools_Quilt), which reports
// nothing and is what grows faster than the count
// (docs/interchange/README.md#what-a-mesh-costs). The rest of the bar is the
// time spent against the time the cost law expects (Pace, below).
double MeshFraction(CADKernel::ImportProgress::Phase phase, double f) {
  using Phase = CADKernel::ImportProgress::Phase;
  const double in = f < 0.0 ? 0.0 : std::min(f, 1.0);
  switch (phase) {
  case Phase::ReadingFile:
    return 0.1 * in;
  case Phase::Building:
    return 0.1 + 0.1 * in;
  case Phase::Sewing:
    return 0.2 + 0.1 * in;
  case Phase::Transferring:
    break;
  }
  return f;
}

// The bar through the part of a mesh read that reports nothing: the CPU time
// spent against what the law expects, held under the end until it lands.
// Never moves the bar back.
void Pace(ImportJob* job, BackgroundJob* bg, double expectedSeconds,
          const std::atomic<bool>& stop) {
  const auto t0 = std::chrono::steady_clock::now();
  while (!stop.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const double spent =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
            .count();
    const double f = std::min(0.95, spent / std::max(expectedSeconds, 1e-3));
    if (f > job->fraction.load(std::memory_order_relaxed)) {
      job->fraction.store(f, std::memory_order_relaxed);
      bg->fraction.store(f);
    }
  }
}

// The worker's view of the job: forwards OCC's progress into the atomics and
// lets `cancel` abort the read. Lives on the worker thread; the main thread
// only ever reads the atomics.
class JobProgress : public CADKernel::ImportProgress {
public:
  JobProgress(ImportJob* job, BackgroundJob* bg) : m_job(job), m_bg(bg) {}

  void OnPhase(Phase phase, double fraction) override {
    ImportJob::Phase mapped = ImportJob::Phase::Transferring;
    switch (phase) {
    case Phase::ReadingFile:
      mapped = ImportJob::Phase::Reading;
      break;
    case Phase::Transferring:
      mapped = ImportJob::Phase::Transferring;
      break;
    case Phase::Building:
      mapped = ImportJob::Phase::Building;
      break;
    case Phase::Sewing:
      mapped = ImportJob::Phase::Sewing;
      break;
    }
    // A mesh's phases are one bar; a STEP's reading has no fraction to give.
    const double shown = m_job->mesh ? MeshFraction(phase, fraction) : fraction;
    m_job->phase.store(static_cast<int>(mapped), std::memory_order_relaxed);
    m_job->fraction.store(shown, std::memory_order_relaxed);
    m_bg->fraction.store(shown);
  }

  bool Cancelled() override { return m_bg->cancel.load(); }

private:
  ImportJob* m_job;
  BackgroundJob* m_bg;
};

bool IsStructured(const std::string& path) {
  std::string ext = std::filesystem::path(path).extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
  return ext == ".step" || ext == ".stp" || ext == ".igs" || ext == ".iges";
}

// The worker. Touches its own job and the filesystem, nothing else — no
// AppState, no Document. Same rule as TopoJob's immutable snapshot.
void Run(ImportJob* job, BackgroundJob* bg, int holdMs, bool holdDeaf) {
  // OCC_CONVERT_SIGNALS turns hardware signals into Standard_Failure, and the
  // handler is installed PER THREAD. Without this a fault inside the reader on
  // this thread is a raw SIGSEGV rather than a catchable failure.
  ArmOccSignalHandlers();

  while (g_holdWorker.load() && !bg->cancel.load())
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  // LOBOLE_TEST_IMPORT_HOLD_MS (tests only): a read that takes this long, or,
  // with LOBOLE_TEST_IMPORT_HOLD_DEAF, one that cannot be stopped -- what an
  // OpenCASCADE call with no interruption point is.
  const auto holdUntil =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(holdMs);
  while (std::chrono::steady_clock::now() < holdUntil &&
         (holdDeaf || !bg->cancel.load()))
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

  const auto wall0 = std::chrono::steady_clock::now();
  const double cpu0 = ThreadCpuSeconds();
  const auto measured = [&] {
    job->workerSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - wall0)
            .count();
    job->workerCpuSeconds = ThreadCpuSeconds() - cpu0;
  };
  const ImportRequest& rq = job->request;
  std::atomic<bool> paced{false};
  std::thread pacer;
  if (job->mesh)
    pacer = std::thread(Pace, job, bg, job->mesh->expectedSeconds,
                        std::cref(paced));
  struct StopPacer {
    std::atomic<bool>& stop;
    std::thread& thread;
    ~StopPacer() {
      stop.store(true);
      if (thread.joinable())
        thread.join();
    }
  } stopPacer{paced, pacer};
  try {
    JobProgress progress(job, bg);
    CADKernel::ImportedModel model;
    std::string why;
    if (bg->cancel.load() ||
        !ImportFeature::ReadModel(rq.path, ReadOptionsOf(rq), model, why,
                                  &progress)) {
      // Cancelled: not an error, and reported as none.
      job->error = bg->cancel.load() ? "" : why;
      measured();
      return;
    }

    // The part-vs-assembly decision ImportCADFile makes synchronously. It
    // needs the whole product structure, which is exactly the 12 s this
    // worker exists to move off the UI thread — so the decision moves with it.
    const bool assembly = rq.mode == ImportRequest::Mode::Assembly ||
                          (rq.mode == ImportRequest::Mode::Auto &&
                           IsStructured(rq.path) && model.instances.size() > 1);
    if (!assembly) {
      job->kind = ImportJob::Kind::Feature;
      // Measured here rather than by the feature on the main thread: a
      // 100k-face mesh's volume is seconds of Gauss integration.
      for (CADKernel::ImportedShape& product : model.products)
        if (product.shape.IsValid() && !bg->cancel.load())
          product.volume = product.shape.Volume();
      // Carried over rather than dropped: the feature would otherwise parse
      // the same file again on its first regen.
      job->model = std::move(model);
      measured();
      return;
    }

    job->partsTotal.store(static_cast<int>(model.products.size()),
                          std::memory_order_relaxed);
    job->phase.store(static_cast<int>(ImportJob::Phase::Extracting),
                     std::memory_order_relaxed);
    job->fraction.store(0.0, std::memory_order_relaxed);

    job->plan = interchange::BuildAssemblyImportPlan(
        model, job->stepPath, job->destDir,
        [job, bg](size_t doneCount, size_t total) {
          job->partsDone.store(static_cast<int>(doneCount),
                               std::memory_order_relaxed);
          const double f = total > 0 ? static_cast<double>(doneCount) /
                                           static_cast<double>(total)
                                     : -1.0;
          job->fraction.store(f, std::memory_order_relaxed);
          bg->fraction.store(f);
          return !bg->cancel.load();
        },
        rq.up, rq.turnDeg);
    if (job->error.empty())
      job->error = job->plan.error;
  } catch (const Standard_Failure& e) {
    job->error = std::string("import failed: ") + e.GetMessageString();
  } catch (const std::exception& e) {
    job->error = std::string("import failed: ") + e.what();
  }
  measured();
}

void Ended(AppState& state, ImportJob* job, ImportJob::Outcome outcome,
           const std::string& error) {
  job->outcome = outcome;
  job->landedError = error;
  state.importError =
      outcome == ImportJob::Outcome::Failed ? error : std::string();
  if (outcome == ImportJob::Outcome::Failed)
    LOG_WARN("Import of '{}' failed: {}", job->stepPath, error);
  else if (outcome == ImportJob::Outcome::Cancelled)
    LOG_INFO("Import of '{}' cancelled", job->stepPath);
  if (job->mesh)
    LOG_INFO(
        "Import of '{}': {}, read in {:.2f} s ({:.2f} s CPU; the law "
        "expected {:.2f} s)",
        job->stepPath, interchange::DescribeTriangles(job->mesh->triangles),
        job->workerSeconds, job->workerCpuSeconds, job->mesh->expectedSeconds);
  state.importJob.reset();
}

// The main-thread half, as the job's `finish`: called once after the join,
// then once per frame while the materialize slices. Returns true when the
// import is over, whichever way.
bool Finish(AppState& state, const std::shared_ptr<ImportJob>& jobPtr) {
  ImportJob* job = jobPtr.get();
  BackgroundJob* bg = FindBackgroundJob(state, "import");
  const bool cancelled = bg != nullptr && bg->cancel.load();

  if (!job->materializing) {
    if (cancelled) {
      // Nothing was materialized yet, so there is nothing to unwind — the
      // only side effect is the .brep sidecars, and they live in a directory
      // named for this import that nothing else references.
      Ended(state, job, ImportJob::Outcome::Cancelled, "");
      return true;
    }
    if (!job->error.empty()) {
      Ended(state, job, ImportJob::Outcome::Failed, job->error);
      return true;
    }
    // One product (or a feature asked for): a feature in the part it was
    // started from. The worker's parsed model goes with it
    // (ImportFeature::SeedGeometry), so the first regen does not re-read the
    // file — a LATER regen still does, which is what keeps editing the source
    // file meaningful.
    if (job->kind == ImportJob::Kind::Feature) {
      const std::optional<size_t> slot = SlotIndexById(state, job->partId);
      if (!slot) {
        Ended(state, job, ImportJob::Outcome::Failed,
              "the part the import was started in was closed");
        return true;
      }
      if (static_cast<int>(*slot) != state.activePart)
        SetActivePart(state, *slot);
      const ImportRequest& rq = job->request;
      CADKernel::ImportedModel parsed = std::move(job->model);
      std::string featureError;
      const bool ok = ImportAsFeature(state, rq.path, &parsed, &featureError,
                                      rq.unit, rq.keepOrigin, rq.up, rq.turnDeg,
                                      rq.representation, rq.minSides);
      Ended(state, job,
            ok ? ImportJob::Outcome::Landed : ImportJob::Outcome::Failed,
            featureError);
      return true;
    }
    if (job->plan.parts.empty()) {
      Ended(state, job, ImportJob::Outcome::Failed,
            "'" + job->stepPath + "' has no part to import");
      return true;
    }
    job->materializing = true;
    // Cancelling once tabs exist would leave a half-built assembly, so from
    // here the request is refused (the modal drops its button, and
    // `background_jobs` says cancellable: false).
    if (bg != nullptr)
      bg->cancellable = false;
    job->phase.store(static_cast<int>(ImportJob::Phase::Materializing),
                     std::memory_order_relaxed);
    job->partsTotal.store(static_cast<int>(job->plan.parts.size()),
                          std::memory_order_relaxed);
    job->partsDone.store(0, std::memory_order_relaxed);
    return false; // let the modal paint the phase change before the first slice
  }

  // Materialize on the MAIN thread, a slice per frame.
  const size_t total = job->plan.parts.size();
  const size_t end = std::min(job->materializedParts + kPartsPerFrame, total);
  for (; job->materializedParts < end; job->materializedParts++) {
    job->partIds.push_back(MaterializeImportedPart(
        state, job->plan.parts[job->materializedParts], job->assemblyName));
  }
  job->partsDone.store(static_cast<int>(job->materializedParts),
                       std::memory_order_relaxed);
  const double f = total > 0 ? static_cast<double>(job->materializedParts) /
                                   static_cast<double>(total)
                             : -1.0;
  job->fraction.store(f, std::memory_order_relaxed);
  if (bg != nullptr)
    bg->fraction.store(f);
  if (job->materializedParts < total)
    return false;

  MaterializeImportedAssembly(state, job->plan, job->assemblyName,
                              job->partIds);
  job->decided = job->plan.up;
  if (job->plan.up.requested == interchange::UpAxis::Auto &&
      job->plan.up.applied != interchange::UpAxis::None) {
    state.importNote =
        "Stood up (" +
        std::string(interchange::ToString(job->plan.up.applied)) +
        " was up in the file): " + job->plan.up.because +
        ". Re-import with another up axis to change it.";
    state.importNoteWarn = false;
    state.importNoteUntil = -1.0;
  }
  LOG_INFO("Imported '{}' as assembly: {} parts, {} occurrences", job->stepPath,
           job->plan.parts.size(), job->plan.occurrences.size());
  Ended(state, job, ImportJob::Outcome::Landed, "");
  return true;
}

int EnvMs(const char* name) {
  const char* v = std::getenv(name);
  return v != nullptr ? std::max(0, std::atoi(v)) : 0;
}

} // namespace

bool ImportRunning(const AppState& state) {
  return BackgroundJobRunning(state, "import");
}

bool StartImport(AppState& state, const ImportRequest& request,
                 std::string* error, std::shared_ptr<ImportJob>* started) {
  const auto fail = [&](const std::string& msg) {
    if (error)
      *error = msg;
    LOG_WARN("Import: {}", msg);
    return false;
  };
  const std::string& path = request.path;
  // A DXF or SVG goes into a sketch, not a feature, and reads in a moment.
  if (request.mode == ImportRequest::Mode::Auto && IsSketchImportFile(path))
    return ImportSketchFile(state, path, error);
  if (ImportRunning(state)) {
    const BackgroundJob* running = FindBackgroundJob(state, "import");
    return fail(running != nullptr && running->cancel.load()
                    ? "the import cancelled a moment ago is still finishing "
                      "a step that cannot be interrupted (sewing a mesh's "
                      "faces into shells); try again once background_jobs "
                      "no longer lists it"
                    : "an import is already running");
  }
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec))
    return fail("'" + path + "' is not a file");

  // Everything that can refuse, before the job exists: a refused import
  // changes nothing (docs/scripting/rpc.md#a-refused-call-changes-nothing).
  const bool structured = IsStructured(path);
  const bool mayBeFeature =
      request.mode == ImportRequest::Mode::Feature ||
      (request.mode == ImportRequest::Mode::Auto && !structured);
  if (request.mode != ImportRequest::Mode::Assembly) {
    if (!state.document || !state.commands)
      return fail("no active part document");
    if (mayBeFeature)
      if (const std::string why = readctx::CheckFeatureRoom(*state.document, 1);
          !why.empty())
        return fail(why);
  }
  const std::optional<interchange::MeshCost> mesh =
      interchange::MeshImportCost(path);
  // The limit prices sewing every triangle; a mesh to be recognised sews
  // none, so it goes ahead, and the worker refuses it only if it would have
  // to be kept as facets (ImportFeature::ReadOptions::facetsPastLimit).
  const bool recognised =
      ResolveRepresentation(state, path, request.representation) ==
      mesh::Representation::Recognize;
  if (mesh && mesh->overLimit && !request.allowLargeMesh && !recognised)
    return fail(
        interchange::LargeMeshRefusal(path, *mesh, request.overrideName));

  // Same destination rule as the synchronous path (ImportStepAsAssembly): a
  // saved project keeps its sidecars, an unsaved one stages them for the next
  // save to adopt. Computed HERE, on the main thread, because it reads
  // state.projectPath.
  const std::string asmName = std::filesystem::path(path).stem().string();
  const std::filesystem::path importRoot =
      state.projectPath.empty()
          ? std::filesystem::path(Autosaver::StateDir()) / "import-staging"
          : std::filesystem::path(state.projectPath) / "imported";
  std::filesystem::path destDir = importRoot / asmName;
  for (int n = 2; std::filesystem::exists(destDir); n++)
    destDir = importRoot / (asmName + "-" + std::to_string(n));

  if (request.representation &&
      *request.representation != mesh::Representation::Facets &&
      !ImportFeature::IsMesh(path))
    return fail("'" + path + "' is not a mesh: `representation` is for STL");
  auto job = std::make_shared<ImportJob>();
  job->request = request;
  // Decided here, on the main thread, where the preference is: the worker
  // reads the file as the feature it seeds will.
  job->request.representation =
      ResolveRepresentation(state, path, request.representation);
  job->stepPath = path;
  job->destDir = destDir.string();
  job->assemblyName = asmName;
  job->mesh = mesh;
  job->tabsBefore = state.parts.size();
  if (state.activePart >= 0 &&
      static_cast<size_t>(state.activePart) < state.parts.size())
    job->partId = state.parts[static_cast<size_t>(state.activePart)].id;
  auto bg = std::make_unique<BackgroundJob>();
  bg->kind = "import";
  bg->label = "Importing " + asmName;
  if (mesh)
    bg->label += " (" + interchange::DescribeTriangles(mesh->triangles) + ", " +
                 interchange::DescribeSeconds(mesh->expectedSeconds) + ")";
  bg->cancellable = true; // until the materialize starts
  bg->owner = request.owner;
  BackgroundJob* raw = bg.get();
  ImportJob* payload = job.get();
  const int holdMs = EnvMs("LOBOLE_TEST_IMPORT_HOLD_MS");
  const bool holdDeaf = std::getenv("LOBOLE_TEST_IMPORT_HOLD_DEAF") != nullptr;
  bg->thread = std::thread([payload, job, raw, holdMs, holdDeaf] {
    Run(payload, raw, holdMs, holdDeaf);
    raw->done.store(true); // last: everything Run wrote is visible first
  });
  bg->finish = [job](AppState& s) { return Finish(s, job); };
  state.importJob = job;
  state.importError.clear();
  if (started != nullptr)
    *started = job;
  StartBackgroundJob(state, std::move(bg));
  LOG_INFO("Import started in the background: '{}'{}", path,
           mesh ? " (" + interchange::DescribeTriangles(mesh->triangles) +
                      ", expected " +
                      interchange::DescribeSeconds(mesh->expectedSeconds) + ")"
                : std::string());
  return true;
}

bool StartImport(AppState& state, const std::string& path, std::string* error) {
  ImportRequest request;
  request.path = path;
  return StartImport(state, request, error);
}

bool RequestImport(AppState& state, const std::string& path,
                   std::string* error) {
  // A mesh to be recognised is not priced by the sewing law, so nothing is
  // asked; one that falls back to facets past the limit is refused then.
  if (!IsSketchImportFile(path) && !ImportRunning(state) &&
      ResolveRepresentation(state, path, std::nullopt) !=
          mesh::Representation::Recognize)
    if (const auto mesh = interchange::MeshImportCost(path);
        mesh && mesh->overLimit) {
      state.importAsk = {path, mesh->triangles, mesh->expectedSeconds};
      return true; // asked; the modal starts it or drops it
    }
  return StartImport(state, path, error);
}

void HoldImportWorkerForTesting(bool hold) { g_holdWorker.store(hold); }

std::string ImportStatusLine(const AppState& state) {
  const ImportJob* job = state.importJob.get();
  if (job == nullptr)
    return {};
  const int done = job->partsDone.load(std::memory_order_relaxed);
  const int total = job->partsTotal.load(std::memory_order_relaxed);
  const std::string cost =
      job->mesh
          ? " (" + interchange::DescribeTriangles(job->mesh->triangles) + ", " +
                interchange::DescribeSeconds(job->mesh->expectedSeconds) +
                " in all)"
          : std::string();
  switch (static_cast<ImportJob::Phase>(
      job->phase.load(std::memory_order_relaxed))) {
  case ImportJob::Phase::Reading:
    return "Reading file..." + cost;
  case ImportJob::Phase::Transferring:
    return "Reading product structure...";
  case ImportJob::Phase::Building:
    return "Making a face of every triangle..." + cost;
  case ImportJob::Phase::Sewing:
    return "Sewing the faces into a shell..." + cost;
  case ImportJob::Phase::Extracting:
    return "Extracting part " + std::to_string(done) + " of " +
           std::to_string(total);
  case ImportJob::Phase::Materializing:
    return "Creating part " + std::to_string(done) + " of " +
           std::to_string(total);
  case ImportJob::Phase::Done:
    break;
  }
  return "Finishing...";
}

} // namespace ui
