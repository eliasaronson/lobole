#pragma once

#include "render/SketchRenderer.h" // PrimVertex
#include "ui/AppState.h"

#include <vector>

// What a measurement LOOKS like in the viewport (M2,
// docs/inspect/measure-v2.md).
//
// Before this the whole visualization was one 2 px line between OCC's
// extrema pair, with the value in a panel in the opposite corner of the
// screen. Three things were wrong with that and all three are geometry, not
// taste: the number was not on the thing it measured, the line landed
// wherever the extrema solver stopped (an arbitrary corner, for any parallel
// pair), and it was depth-tested, so any measurement through material —
// plate thickness, bore to outer wall — was drawn inside the solid and
// invisible.
//
// Both halves read `state.measure.dimension`, which UpdateMeasureReadout
// fills from the same numbers as the text: the picture and the readout are
// one computation, so they cannot disagree.
namespace ui {

// The dimension line, its arrowheads and witness lines, and the angle arc.
// Appends to the ON-TOP primitive channel (SetOverlayPrimitives) — a
// measurement describes the solid rather than living on it.
void AppendMeasureOverlay(AppState& state, std::vector<PrimVertex>& out);

// The values, in pills on the geometry: the distance at the dimension's
// midpoint, the angle on its arc. Foreground draw list, like the CAM depth
// label and the datum names.
void DrawMeasureLabels(AppState& state);

} // namespace ui
