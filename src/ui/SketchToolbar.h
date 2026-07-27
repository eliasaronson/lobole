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

} // namespace ui
