#pragma once

#include "core/MotionStudy.h"

#include <cstdint>
#include <string>

namespace ui {

struct AppState;

// Motion studies on the active assembly tab (docs/agent-scale-plan.md, G2).
//
// A run is a job (kind "motion") on the A3 registry: the worker solves a COPY
// of the assembly against COPIES of its parts, with every collision mesh
// built on the main thread before the worker starts, so nothing it touches
// is shared with the tab. Its result lands in `state.motionResults` on
// finish and is keyed by the study AND the document it was run against —
// a result never outlives a tab switch to a different assembly.

// Start an async run. False (with `error`) if one is already running or
// the study cannot run (WhyNot).
bool StartMotionRun(AppState& state, uint64_t studyId, std::string* error);

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

} // namespace ui
