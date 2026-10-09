// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// The expensive steps of a generate, off the frame
// (docs/ui/long-operations.md#cam-toolpath-passes).
//
// A generate on a job tab DEFERS its adaptive passes: `cam::Generate` hands
// back the requests it did not run and marks those toolpaths pending; this
// file queues them, cuts them one at a time on a "cam" BackgroundJob, and
// puts each result in `state.camAdaptiveCache` — after which the next
// generate is a cache hit and the toolpath fills in. Content keys, not
// operation ids: a pass that lands after its parameters moved on is simply
// an entry nobody reads, and a pass that lands while another tab is active
// is picked up by whichever job tab regenerates next.

#include "core/cam/CamAdaptive.h"
#include "core/cam/CamJob.h"
#include "core/cam/CamPicks.h" // OutlineRequest
#include "core/cam/CamSurface.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ui {

struct AppState;

// Queue what a deferred generate skipped and start the worker if it is idle.
// A key already cached, queued or in flight is not queued twice, which is
// what keeps a slider drag to one pass per value that survived to a frame.
void QueueAdaptivePasses(AppState& state,
                         std::vector<cam::AdaptiveRequest> pending);

// The same, for the surfacing operations a deferred generate skipped. One
// entry per OPERATION rather than per Z level, onto the same worker and the
// same batch counters, so the progress row and every `pending` refusal work
// unchanged (docs/ui/long-operations.md#cam-toolpath-passes).
void QueueSurfacePasses(AppState& state,
                        std::vector<cam::SurfaceRequest> pending);

// Put the disk store behind `state.camSurfaceCache`: finished surfacing
// passes are kept under the user's cache directory (`$XDG_CACHE_HOME/
// designer/cam`) and read back by a later run instead of being cut again
// (docs/ui/long-operations.md#passes-kept-on-disk). The app calls it at
// startup; a test that wants it points XDG_CACHE_HOME at a scratch
// directory first, or builds its own `cam::SurfaceStore`.
void OpenCamSurfaceStore(AppState& state);

// The same, for the projected outlines a deferred profile skipped
// (docs/cam/README.md#background-work). Started before the other two: an
// outline is seconds and every profile waits on its own.
void QueueOutlinePasses(AppState& state,
                        std::vector<cam::OutlineRequest> pending);

// Whether any toolpath in `state.camToolpaths` is still pending, and why in
// words — post, verify and simulate refuse on this rather than acting on a
// list that is not yet the job.
[[nodiscard]] bool CamToolpathsPending(const AppState& state);
[[nodiscard]] std::string CamToolpathsPendingWhy(const AppState& state);

// Synchronous: wait for the pass in flight, run the rest of the queue, and
// regenerate — the RPC reader's path ("settle first, then answer"). Never
// from the frame loop.
void SettleCamToolpaths(AppState& state);

// Every enabled operation of `setup`, generated NOW, through the cache —
// the same list `SettleCamToolpaths` leaves in state, for an RPC that wants
// its own copy of a setup that may not be the active one.
[[nodiscard]] std::vector<cam::Toolpath>
GenerateCamToolpathsNow(AppState& state, const cam::Setup& setup);

// Another setup's toolpaths from the same caches WITHOUT computing on the
// frame: a pass not yet cut is queued on the workers and `pending` set. What
// the panel's verification reads the job's other setups with
// (docs/cam/simulation.md#leftover-another-setup-removes).
[[nodiscard]] std::vector<cam::Toolpath>
CamToolpathsQueued(AppState& state, const cam::Setup& setup, bool& pending);

// What an RPC that must not compute on the frame asks instead
// (docs/cam/README.md#requests-that-wait): queue every pass `setup` reads
// that is not cached -- deferred regardless of `camDeferPasses` -- and say
// whether any is still owed (queued or cutting).
[[nodiscard]] bool QueueCamPassesFor(AppState& state, const cam::Setup& setup);

// Whether the "cam" worker has anything queued or cutting. A synchronous
// reader joins every "cam" job, so it only runs without stalling when this
// is false.
[[nodiscard]] bool CamPassesBusy(const AppState& state);

// Bumped by every CancelCamPasses: a request waiting for passes compares it
// to learn that what it was waiting for will not land.
[[nodiscard]] uint64_t CamPassCancellations();

// Drop the queue. The pass now cutting finishes (libarea has no interruption
// points; its result is cached, it was paid for) and the toolpaths that were
// waiting stop being pending and say they were cancelled — the next edit or
// Generate queues them again.
void CancelCamPasses(AppState& state);

// The row's and the label's numbers: passes landed / passes in this batch.
struct CamPassProgress {
  bool running = false;
  size_t done = 0;
  size_t total = 0;
  std::string label; // the job's label, or empty
};
[[nodiscard]] CamPassProgress CamPassProgressOf(const AppState& state);

} // namespace ui
