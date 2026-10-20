// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "core/SequenceTimeline.h"
#include "ui/MotionActions.h"

#include <glm/glm.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace ui {

struct AppState;

// An assembly sequence played and filmed
// (docs/assemblies/README.md#playing-and-filming-a-sequence).
//
// The timeline (core/SequenceTimeline.h) takes every step's path, its
// distance from clear and its way along it from what the sequence CHECK
// resolved -- the check is run for it, on a worker -- so the film and the
// check cannot disagree, and a sequence that does not check is refused
// naming its first failing step. Showing it is display-only, as motion
// playback is: DisplayMatrix and the renderer sync read the pose, a part
// not yet put in is hidden, and the solved placements never move.

// A timeline being made from the active assembly's sequence, or made.
struct SequencePoses {
  std::atomic<bool> done{false};
  std::atomic<bool> cancel{false};
  std::atomic<double> fraction{0.0};                  // through the check
  std::shared_ptr<const sequence::Timeline> timeline; // set before `done`
  std::string error; // why it cannot be played; set before `done`
  // What it was made of: the assembly, and its content's key then.
  const void* assembly = nullptr;
  uint64_t key = 0;
  size_t steps = 0;
  std::thread worker;
  ~SequencePoses() {
    cancel.store(true);
    if (worker.joinable())
      worker.join();
  }
};

// Read the active assembly's sequence on the frame and check it on a
// worker, laying out its timeline. Null (with `error`) for what the check
// refuses before it runs: no assembly, no sequence, a step it cannot read.
std::shared_ptr<SequencePoses> ResolveSequence(AppState& state,
                                               std::string* error);

// Where playback stands. One per app (state.sequenceShow); the resolved
// timeline is kept while the assembly is unchanged, so Play again does not
// check again.
struct SequenceShow {
  std::shared_ptr<SequencePoses> poses;
  bool showing = false; // the viewport shows the timeline's pose
  bool playing = false; // advancing (Play); false: held, or a video's
  bool pinned = false;  // a video is moving it, not the clock
  bool synced = false;  // the renderers have been resynced to show it
  double t = 0.0;       // seconds into the timeline
  double lastTick = -1.0;
  // What is shown in the highlight (the step's moving parts) and the pace
  // (a ratio: 2 plays the timeline twice as fast), as this showing has
  // them: Play takes the panel's, a video its request's.
  bool highlight = true;
  double speed = 1.0;
  // The panel's own, for the next Play.
  bool playHighlight = true;
  double playSpeed = 1.0;
  std::string refusal; // why the last Play could not play
};

// Play: check the sequence if its timeline is not at hand (the viewport
// waits on the model until it is), then play it from the start. False,
// with why, for a refusal the check makes before it runs.
bool StartSequencePlayback(AppState& state, std::string* why);
// Back to the document's placements; a check still running is stopped.
void StopSequencePlayback(AppState& state);
// Hold at `t` seconds (the scrub), once the timeline is made.
void SeekSequencePlayback(AppState& state, double t);
// Called every frame from TickMotionPlayback.
void TickSequencePlayback(AppState& state, double now);
// The timeline being shown, or null.
const sequence::Timeline* ShownSequence(const AppState& state);
// The pose playback shows a top-level occurrence at, while it shows one.
bool SequencePlaybackWorld(const AppState& state, uint64_t occurrenceId,
                           glm::dmat4& world);
// A part not yet put in: hidden while playback shows the timeline.
bool SequencePlaybackHides(const AppState& state, uint64_t occurrenceId);
// The occurrences the step on show is moving -- travelling home, or turned
// by a pose -- when the showing highlights them; else none.
std::vector<uint64_t> SequenceHighlighted(const AppState& state);
// Lights them (their meshes and section caps) in the selection's colour.
// Part of the selection projection (SyncSelectionHighlights), and of a
// video's frame, which the capture leaves untinted otherwise.
void HighlightSequenceStep(AppState& state);

// The assembly panel's sequence section: Play / Stop and a scrub.
void DrawSequenceSection(AppState& state);

// export_assembly_video: export_motion_video's request and job, filming
// the sequence's timeline. The check runs inside the job; a sequence that
// does not pass is the job's refusal.
bool StartAssemblyVideo(AppState& state, const MotionVideoRequest& req,
                        std::string* error, MotionVideoPlan* planOut);

} // namespace ui
