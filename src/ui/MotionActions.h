#pragma once

#include "core/MotionStudy.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>

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
bool StartMotionRun(AppState& state, uint64_t studyId, std::string* error,
                    const RunOverride* override = nullptr);

// Run on the main thread, now. The same result as the job, for a caller
// that would rather wait than poll.
motion::Result RunMotionNow(AppState& state, uint64_t studyId);

// The stored result for a study of the ACTIVE assembly, or null.
const motion::Result* MotionResultFor(const AppState& state, uint64_t studyId);
// Conservative revision check; retained results remain available as evidence.
bool MotionResultStale(AppState& state, uint64_t studyId);

// Drive the study's mate to parameter t (0..1) and re-solve: the scrub.
// Deliberately NO undo snapshot, like the explode slider — a scrub is a look,
// not an edit, and 48 snapshots per drag would bury the real history.
bool ApplyMotionPose(AppState& state, uint64_t studyId, double t,
                     std::string* error);

// Write the run's animation as a GLB. Refuses without a result, and refuses
// when any sample is unsolved: an animation that quietly holds a frame is
// the substitution this project does not make.
bool ExportMotionGlb(AppState& state, uint64_t studyId, const std::string& path,
                     std::string* error, size_t* nodesOut = nullptr,
                     size_t* framesOut = nullptr);

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
                         glm::mat4& world);

} // namespace ui
