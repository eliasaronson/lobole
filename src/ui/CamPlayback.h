#pragma once

#include "render/SketchRenderer.h" // PrimVertex
#include "ui/AppState.h"

#include <vector>

// The cutting simulation's session: arm it, run it, draw it, end it
// (docs/cam/simulation.md, S4–S5).
//
// The transport is a FLOATING window over the viewport (DrawCamTransportWindow,
// since 2026-09-02 -- it used to be a section of the CAM panel, which made
// that panel the busiest thing on screen); this owns the state machine behind
// it. It is deliberately ONE begin and ONE end, because
// playback hides things — the bodies go translucent and a stock block stands
// in front of them — and a mode with several exits is the U11 bug class this
// codebase has already paid for once (docs/cam/gui.md).
namespace ui {

// Posts the cached toolpaths, parses the file back, builds the timeline and
// arms the stock. False + `state.camPlaybackRefusal` when there is nothing to
// play, which is a refusal the panel prints where the button is rather than a
// button that does nothing.
bool BeginCamPlayback(AppState& state);

// THE way out. Restores the bodies, drops the stock and the tool, and leaves
// no session state behind. Safe to call when nothing is playing.
void EndCamPlayback(AppState& state);

// Advances the cursor while playing. `now` is the caller's monotonic clock.
void TickCamPlayback(AppState& state, double now);

// Moves the cursor, re-cutting as needed. Pausing is the caller's business —
// scrubbing while playing is a real thing to want.
void SeekCamPlayback(AppState& state, cam::SimCursor to);

// Pushes what the cursor implies at the renderers: the stock surface (rebuilt
// only when the simulation actually moved), the tool where it is, and the
// bodies ghosted underneath. Cheap and idempotent — call it every frame.
void SyncCamPlayback(AppState& state);

// The transport, as its own floating window along the bottom of the viewport
// while a playback is open: play/pause, step, the scrub bar, speed, the
// readout naming the operation, tool, feed and G-code line under the cursor,
// and Close. Draws nothing when nothing is playing -- the way IN is the job
// toolbar's Simulate (`job.simulate`), and its refusal is printed by the CAM
// panel under Generate.
void DrawCamTransportWindow(AppState& state);

// The path, coloured by what has already been cut. Replaces the plain
// backplot while a playback is open — during playback the line drawn IS the
// file being played, so the two cannot drift.
[[nodiscard]] bool CamPlaybackDrawsPath(const AppState& state);
void AppendCamPlaybackPath(AppState& state, std::vector<PrimVertex>& out);

} // namespace ui
