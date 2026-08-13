#pragma once

#include "ui/AppState.h"

namespace ui {

// Sketch-mode tool sessions in the floating Tool window (U8a): the
// plane-pick prompt, and the active tool's options/Apply for
// Mirror/Fillet/Pattern/Offset/Calibrate/Dimension. Called from
// DrawToolWindow; no-op for tools whose prompt pill is enough.
void DrawSketchToolWindow(AppState& state);

// Sketch sections at the bottom of the Model panel while editing (U8a —
// the deleted Sketch panel's persistent tenants): reference images,
// constraint list, linked patterns, linked copies.
void DrawSketchModelSections(AppState& state);

// Extrude/Revolve the active sketch: exits sketch mode and opens the
// feature preview. Default mode is Add once a body exists (New Body on the
// first solid). Shared by the Sketch panel buttons and Shift+E / Shift+R.
void BeginSketchExtrude(AppState& state);
void BeginSketchRevolve(AppState& state);

// The same previews for a sketch that is NOT being edited — the model
// toolbar's Extrude/Revolve, which act on the selected (else most recent)
// closed-profile sketch. `regions` empty = the whole sketch. Callers that
// are in a sketch session must exit it first (BeginSketchExtrude does).
void BeginExtrudeOfSketch(AppState& state, SketchFeature& sketch,
                          std::vector<std::vector<uint32_t>> regions = {});
void BeginRevolveOfSketch(AppState& state, SketchFeature& sketch,
                          std::vector<std::vector<uint32_t>> regions = {});

// Turns the active sketch's Point entities into a Hole preview: exits sketch
// mode, then BeginPreviewFeature (the BeginSketchExtrude recipe). No-ops with
// an actionMsg when the sketch has no points. Shared by the sketch toolbar's
// hole button (###tb_sk_hole / Shift+H), the ✓ of a Hole session, and Enter.
void BeginSketchHole(AppState& state);
bool CanBeginSketchHole(const AppState& state); // >= 1 Point (button gate)

// Ends a Hole session (Escape). The sketch the session CREATED is removed —
// one undoable step — when the user drew nothing in it; anything they did
// draw is kept. Only ever called while purpose == Hole, which proves this
// session created the sketch (###tb_sk_hole never sets the purpose).
void CancelHoleSession(AppState& state);

// The selected sketch if it has a closed profile, else the most recent one
// that does; null when the document has none. Backs both the model-toolbar
// Extrude/Revolve actions and their enable gates.
SketchFeature* ClosedProfileSketch(AppState& state);

} // namespace ui
