#pragma once

#include "core/MotionStudy.h"
#include "ui/BackgroundJobs.h"

#include <glm/glm.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace ui {

struct AppState;

// Motion studies on the active assembly tab
// (docs/assemblies/README.md#motion-studies).
//
// A run is a job (kind "motion") on the A3 registry: the worker solves a COPY
// of the assembly against COPIES of its parts, with every collision mesh
// built on the main thread before the worker starts, so nothing it touches
// is shared with the tab. Its result lands in `state.motionResults` on
// finish and is keyed by the study AND the document it was run against —
// a result never outlives a tab switch to a different assembly.

// What ONE run checks, instead of what the study says: the panel's Preview
// (no collisions) and Validate (exact) are runs, not edits. The stored study
// is never touched, and the result's staleness is still judged against it —
// so a preview of an unchanged study is fresh.
struct RunOverride {
  bool checkClash = true;
  std::string clashMode = "exact"; // "mesh" | "exact"
};

// Start an async run. False (with `error`) if one is already running or
// the study cannot run (WhyNot). `override` (may be null) changes what this
// run checks without editing the study.
//
// A run whose input hash (motion::InputKey) matches a result the session
// holds is answered from it and starts nothing: `job->reused` says so,
// unless `job->recompute` asked for a run anyway
// (docs/simulation/README.md#studies-as-jobs). Otherwise `job->jobId` is
// the started job's id, and `job->owner` the connection it dies with.
bool StartMotionRun(AppState& state, uint64_t studyId, std::string* error,
                    const RunOverride* override = nullptr,
                    JobStart* job = nullptr);

// Run on the main thread, now. The same result as the job, for a caller
// that would rather wait than poll; reused as StartMotionRun says. It stops
// (cancelled) when `state.rpcCallerGone` says its caller has gone.
motion::Result RunMotionNow(AppState& state, uint64_t studyId,
                            JobStart* job = nullptr);

// Leave the live assembly where sample `sample` of `result` has it: every
// placement and moving mate value the run recorded there, written in, and
// ONE solve to settle it -- not a walk from wherever the assembly is. With
// `value` (the driver, radians or mm), the driver is set to it after the
// sample's pose and the one solve steps there: the nearest sample is less
// than a spacing away, so a slider-crank stays on the run's branch. Refuses
// a sample that did not solve: there is no pose to leave it in.
bool ApplyMotionSample(AppState& state, uint64_t studyId,
                       const motion::Result& result, size_t sample,
                       const double* value, std::string* error);

// The stored result for a study of the ACTIVE assembly, or null.
const motion::Result* MotionResultFor(const AppState& state, uint64_t studyId);
// Forget a study's result: its definition changed, or it is gone. The
// assembly's saved copy goes with it.
void DropMotionResult(AppState& state, uint64_t studyId);
// The active assembly's saved motion results, installed as the ones
// MotionResultFor answers: current when their inputs hash as they did when
// they ran, stale otherwise, and kept by their hash for a later run.
void AdoptSavedMotionResults(AppState& state);
// Conservative revision check; retained results remain available as evidence.
bool MotionResultStale(AppState& state, uint64_t studyId);

// Drive the study's mate to parameter t (0..1) and re-solve: the scrub.
// Deliberately NO undo snapshot, like the explode slider — a scrub is a look,
// not an edit, and 48 snapshots per drag would bury the real history.
bool ApplyMotionPose(AppState& state, uint64_t studyId, double t,
                     std::string* error);

// Leave the live assembly at a driver VALUE (radians or mm) between the
// samples: walk to sample `sample` exactly as the scrub does, then step from
// it to `value` — less than one sample spacing away, so the last solve
// starts where the run's own did and a slider-crank stays on its branch.
bool ApplyMotionValue(AppState& state, uint64_t studyId, int sample,
                      double value, std::string* error);

// Write the run's animation as a GLB. Refuses without a result, and refuses
// when any sample is unsolved: an animation that quietly holds a frame is
// the substitution this project does not make.
bool ExportMotionGlb(AppState& state, uint64_t studyId, const std::string& path,
                     std::string* error, size_t* nodesOut = nullptr,
                     size_t* framesOut = nullptr);

// --- Video (docs/assemblies/README.md#motion-video) -------------------------
//
// The study as a MOVIE, played the way the Motion window's Play plays it:
// the run's poses at the playback's rate, interpolated between samples with
// the playback's own interpolation, and each frame the viewport's own render
// (FrameCapture: the model's renderers at the viewport's settings, no
// overlays, at the requested size, in the windowed and offscreen app alike).
// It needs no stored run: without a fresh one it makes its own, collision-
// free like Preview, and keeps it to itself. mp4 (H.264), webm (VP9) and gif
// (a palette pass over the whole animation) are encoded by ffmpeg as an
// external program, fed raw RGB on its stdin; png writes numbered frames
// into a folder and needs nothing. A job of kind "motion-video", sliced on
// the main thread (posing and capturing need the documents and the
// renderers); the encoding is on a worker. The document is never posed, so
// the assembly is where it was.

// The formats, as a refusal names them.
inline constexpr const char* kMotionVideoFormats = "mp4, webm, gif, png";
inline constexpr const char* kMotionVideoQualities = "low, medium, high";

// Frames per second a video has unless asked: a display's.
inline constexpr double kMotionVideoFps = 60.0;
// Samples per second the Motion window plays a study at — and so the video:
// its length is what Play takes, (samples - 1) / this.
inline constexpr double kPlaybackSamplesPerSecond = 24.0;

struct MotionVideoRequest {
  uint64_t studyId = 0;
  std::string path;               // a file, or for png a FOLDER
  std::string format;             // empty = read from the path's extension
  uint32_t width = 0, height = 0; // pixels; 0 = the viewport's
  double fps = 0.0;               // frames per second; 0 = derived
  int frames = 0;                 // 0 = derived
  std::string quality;            // empty = medium; not for png
};

// What an export will do, every default resolved.
struct MotionVideoPlan {
  std::string format, path, quality;
  std::string ffmpeg; // its full path; empty for png
  uint32_t width = 0, height = 0;
  double fps = 0.0;
  int frames = 0;
  std::string studyName;
};

struct MotionVideoResult {
  bool ok = false;
  bool cancelled = false;
  std::string error;
  MotionVideoPlan plan;
  int framesWritten = 0;
  uint64_t bytes = 0; // the file, or the frames in the folder
  double seconds = 0.0;
  std::vector<std::string> warnings;
};

// Every refusal, and every default, without starting anything.
bool PlanMotionVideo(AppState& state, const MotionVideoRequest& req,
                     MotionVideoPlan* plan, std::string* error);
// Start the job (kind "motion-video"). False, with `error`, for any refusal
// PlanMotionVideo makes, a video already being exported, or an encoder that
// cannot be started.
bool StartMotionVideo(AppState& state, const MotionVideoRequest& req,
                      std::string* error, MotionVideoPlan* planOut = nullptr);
// ffmpeg's full path on PATH, or empty: what `ping` reports as a feature.
std::string FfmpegPath();

// The poses to play a study with when there may be no stored run to play:
// the stored result when playback could show it, else a collision-free run
// of the study (what Preview runs) on a worker, kept here and NOT stored as
// the study's result. `result` is set before `done`; a refusal is `error`.
struct MotionPoses {
  std::atomic<bool> done{false};
  std::atomic<bool> cancel{false};
  std::shared_ptr<const motion::Result> result;
  std::string error;
  std::thread worker;
  ~MotionPoses() {
    cancel.store(true);
    if (worker.joinable())
      worker.join();
  }
};
std::shared_ptr<MotionPoses> PosesToPlay(AppState& state, uint64_t studyId,
                                         std::string* error);
// Why these poses cannot be played (a sample did not solve, the run was
// cancelled or refused), or empty.
std::string MotionPosesRefusal(const motion::Result& r);
// Show `poses` at playback position `pos` (samples, 0 .. n-1) in the
// viewport: the playback's pose, its interpolation included, pushed to the
// renderers. The document is not touched. StopMotionPlayback ends it.
void ShowMotionPoses(AppState& state, uint64_t studyId,
                     const std::shared_ptr<const motion::Result>& poses,
                     double pos);

// PLAYBACK: the poses a run made, shown in the viewport without solving and
// without editing the document — display-only, like an exploded view. Refused
// (false, with why) with no result, a stale one, or an unsolved sample: the
// rule the GLB export has, for the same reason.
bool StartMotionPlayback(AppState& state, uint64_t studyId, std::string* why);
void StopMotionPlayback(AppState& state);
// Why playback (and the export) cannot use this study's result, or empty.
std::string MotionPlaybackRefusal(AppState& state, uint64_t studyId);
// Advance by wall clock (seconds); called once per frame from the main loop.
void TickMotionPlayback(AppState& state, double now);
// The playback pose of a TOP-LEVEL occurrence, if playback is showing one.
bool MotionPlaybackWorld(const AppState& state, uint64_t occurrenceId,
                         glm::dmat4& world);
// v129: a mesh INSIDE a flexible placement moves on its own. Given where the
// mesh sits in `root`'s frame now (`docLocal`, DocumentRenderMesh::localXf)
// and its path below root, the played place in root's PLAYED frame; false
// when playback has no pose of its own for it (it folds into the root).
bool MotionPlaybackLocal(AppState& state, const Occurrence& root,
                         const std::vector<uint64_t>& innerPath,
                         const glm::dmat4& docLocal, glm::dmat4& local);

} // namespace ui
