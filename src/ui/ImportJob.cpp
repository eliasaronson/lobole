#include "ui/ImportJob.h"

#include "common/Logger.h"
#include "core/io/Autosaver.h"
#include "ui/AppActions.h"
#include "ui/AppState.h"
#include "ui/AssemblyActions.h"

#include <OSD.hxx>
#include <Standard_Failure.hxx>

#include <algorithm>
#include <filesystem>
#include <utility>

namespace ui {

namespace {

// How many part tabs to create per frame while materializing. Each is an
// AddPart + an ImportFeature regen off a small .brep — around 7 ms on the
// 299-part file, so 8 keeps a frame near 60 ms: slow enough to be one visible
// progress step, fast enough that 299 parts land in ~2.5 s.
constexpr size_t kPartsPerFrame = 8;

// The worker's view of the job: forwards OCC's progress into the atomics and
// lets `cancel` abort the read. Lives on the worker thread; the main thread
// only ever reads the atomics.
class JobProgress : public CADKernel::ImportProgress {
public:
  explicit JobProgress(ImportJob* job) : m_job(job) {}

  void OnPhase(Phase phase, double fraction) override {
    const ImportJob::Phase mapped = phase == Phase::ReadingFile
                                        ? ImportJob::Phase::Reading
                                        : ImportJob::Phase::Transferring;
    m_job->phase.store(static_cast<int>(mapped), std::memory_order_relaxed);
    m_job->fraction.store(fraction, std::memory_order_relaxed);
  }

  bool Cancelled() override {
    return m_job->cancel.load(std::memory_order_relaxed);
  }

private:
  ImportJob* m_job;
};

// The worker. Touches its own job and the filesystem, nothing else — no
// AppState, no Document. Same rule as TopoJob's immutable snapshot.
void Run(ImportJob* job) {
  // OCC_CONVERT_SIGNALS turns hardware signals into Standard_Failure, and the
  // handler is installed PER THREAD. Without this a fault inside the reader on
  // this thread is a raw SIGSEGV rather than a catchable failure.
  OSD::SetSignal(Standard_False);

  try {
    JobProgress progress(job);
    CADKernel::ImportedModel model;
    if (!CADKernel::Instance().LoadSTEPStructured(job->stepPath, model,
                                                  &progress)) {
      job->error = job->cancel.load(std::memory_order_relaxed)
                       ? "" // cancelled: not an error, and reported as none
                       : "could not read '" + job->stepPath + "' as STEP";
      job->done.store(true, std::memory_order_release);
      return;
    }

    // The part-vs-assembly decision ImportCADFile makes synchronously. It
    // needs the whole product structure, which is exactly the 12 s this
    // worker exists to move off the UI thread — so the decision moves with it.
    if (model.instances.size() <= 1) {
      job->kind = ImportJob::Kind::Feature;
      // Carried over rather than dropped: the feature would otherwise parse
      // the same file again on its first regen.
      job->model = std::move(model);
      job->done.store(true, std::memory_order_release);
      return;
    }

    job->partsTotal.store(static_cast<int>(model.products.size()),
                          std::memory_order_relaxed);
    job->phase.store(static_cast<int>(ImportJob::Phase::Extracting),
                     std::memory_order_relaxed);
    job->fraction.store(0.0, std::memory_order_relaxed);

    job->plan = interchange::BuildAssemblyImportPlan(
        model, job->stepPath, job->destDir,
        [job](size_t doneCount, size_t total) {
          job->partsDone.store(static_cast<int>(doneCount),
                               std::memory_order_relaxed);
          job->fraction.store(total > 0 ? static_cast<double>(doneCount) /
                                              static_cast<double>(total)
                                        : -1.0,
                              std::memory_order_relaxed);
          return !job->cancel.load(std::memory_order_relaxed);
        });
    if (job->error.empty())
      job->error = job->plan.error;
  } catch (const Standard_Failure& e) {
    job->error = std::string("STEP import failed: ") + e.GetMessageString();
  } catch (const std::exception& e) {
    job->error = std::string("STEP import failed: ") + e.what();
  }

  // Last, with release ordering: everything above must be visible to the main
  // thread before it sees this.
  job->done.store(true, std::memory_order_release);
}

} // namespace

bool ImportRunning(const AppState& state) { return state.importJob != nullptr; }

bool StartImport(AppState& state, const std::string& path, std::string* error) {
  const auto fail = [&](const std::string& msg) {
    if (error)
      *error = msg;
    LOG_WARN("Import: {}", msg);
    return false;
  };
  if (state.importJob)
    return fail("an import is already running");

  // Only STEP is slow enough to be worth a thread: a .brep or .stl of any
  // realistic size reads in milliseconds, and routing them through the job
  // would put a progress modal in front of an instant operation.
  std::string ext = std::filesystem::path(path).extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
  if (ext != ".step" && ext != ".stp")
    return ImportAsFeature(state, path, error);

  const std::string stepPath = path;

  // Same destination rule as the synchronous path (ImportStepAsAssembly): a
  // saved project keeps its sidecars, an unsaved one stages them for the next
  // save to adopt. Computed HERE, on the main thread, because it reads
  // state.projectPath.
  const std::string asmName = std::filesystem::path(stepPath).stem().string();
  const std::filesystem::path importRoot =
      state.projectPath.empty()
          ? std::filesystem::path(Autosaver::StateDir()) / "import-staging"
          : std::filesystem::path(state.projectPath) / "imported";
  std::filesystem::path destDir = importRoot / asmName;
  for (int n = 2; std::filesystem::exists(destDir); n++)
    destDir = importRoot / (asmName + "-" + std::to_string(n));

  auto job = std::make_unique<ImportJob>();
  job->stepPath = stepPath;
  job->destDir = destDir.string();
  job->assemblyName = asmName;
  ImportJob* raw = job.get();
  state.importJob = std::move(job);
  raw->worker = std::thread(Run, raw);
  LOG_INFO("Assembly import started in the background: '{}'", stepPath);
  return true;
}

void PollImport(AppState& state) {
  ImportJob* job = state.importJob.get();
  if (job == nullptr)
    return;

  // Phase 1: waiting on the worker.
  if (!job->joined) {
    if (!job->done.load(std::memory_order_acquire))
      return;
    // Join before reading anything the worker wrote: the release/acquire pair
    // orders the fields and the join orders the thread's exit.
    job->worker.join();
    job->joined = true;

    const bool cancelled = job->cancel.load(std::memory_order_relaxed);
    // One product: a feature in the active part. The worker's parsed model
    // goes with it (ImportFeature::SeedGeometry), so the first regen does not
    // re-read the file — a LATER regen still does, which is what keeps
    // editing the source file meaningful.
    if (!cancelled && job->error.empty() &&
        job->kind == ImportJob::Kind::Feature) {
      const std::string path = job->stepPath;
      CADKernel::ImportedModel parsed = std::move(job->model);
      std::string featureError;
      state.importJob.reset(); // joins; nothing below touches the job
      if (!ImportAsFeature(state, path, &parsed, &featureError)) {
        state.importError = featureError;
        LOG_WARN("Import failed: {}", featureError);
      } else {
        state.importError.clear();
      }
      return;
    }
    if (cancelled || !job->error.empty() || job->plan.parts.empty()) {
      if (!cancelled && !job->error.empty())
        LOG_WARN("Assembly import failed: {}", job->error);
      state.importError = cancelled ? std::string() : job->error;
      // Nothing was materialized yet, so there is nothing to unwind — the
      // only side effect is the .brep sidecars, and they live in a directory
      // named for this import that nothing else references.
      state.importJob.reset();
      return;
    }
    job->phase.store(static_cast<int>(ImportJob::Phase::Materializing),
                     std::memory_order_relaxed);
    job->partsTotal.store(static_cast<int>(job->plan.parts.size()),
                          std::memory_order_relaxed);
    job->partsDone.store(0, std::memory_order_relaxed);
    return; // let the modal paint the phase change before the first slice
  }

  // Phase 2: materialize on the MAIN thread, a slice per frame. Cancelling
  // here would leave a half-built assembly, so it is refused once this starts
  // (the modal drops its Cancel button at the same moment).
  const size_t total = job->plan.parts.size();
  const size_t end = std::min(job->materializedParts + kPartsPerFrame, total);
  for (; job->materializedParts < end; job->materializedParts++) {
    job->partIds.push_back(MaterializeImportedPart(
        state, job->plan.parts[job->materializedParts], job->assemblyName));
  }
  job->partsDone.store(static_cast<int>(job->materializedParts),
                       std::memory_order_relaxed);
  job->fraction.store(total > 0 ? static_cast<double>(job->materializedParts) /
                                      static_cast<double>(total)
                                : -1.0,
                      std::memory_order_relaxed);
  if (job->materializedParts < total)
    return;

  MaterializeImportedAssembly(state, job->plan, job->assemblyName,
                              job->partIds);
  LOG_INFO("Imported '{}' as assembly: {} parts, {} occurrences", job->stepPath,
           job->plan.parts.size(), job->plan.occurrences.size());
  state.importError.clear();
  state.importJob.reset();
}

std::string ImportStatusLine(const AppState& state) {
  const ImportJob* job = state.importJob.get();
  if (job == nullptr)
    return {};
  const int done = job->partsDone.load(std::memory_order_relaxed);
  const int total = job->partsTotal.load(std::memory_order_relaxed);
  switch (static_cast<ImportJob::Phase>(
      job->phase.load(std::memory_order_relaxed))) {
  case ImportJob::Phase::Reading:
    return "Reading file...";
  case ImportJob::Phase::Transferring:
    return "Reading product structure...";
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
