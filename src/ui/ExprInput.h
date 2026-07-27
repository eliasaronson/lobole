#pragma once

#include "ui/AppState.h"

namespace ui {

// Numeric drag widgets with expression support — THE shared way to take a
// number anywhere in the UI. They behave exactly like ImGui::DragX, plus:
// Ctrl+click the field and type any expression ("360/3", "width/2" using
// the document's variables); on commit the expression evaluates and the
// RESULT is written back. The expression text itself is not stored — fields
// whose model has a dedicated expr slot (feature dims) keep using
// FeatureEditor's ExprDim, which persists the text and re-evaluates on
// regeneration.
//
// Evaluation errors keep the old value and surface via the prompt bar's
// actionMsg channel.
bool ExprDragFloat(AppState& state, const char* label, float* v, float speed,
                   float vmin, float vmax, const char* fmt);
bool ExprDragDouble(AppState& state, const char* label, double* v, float speed,
                    double vmin, double vmax, const char* fmt);
bool ExprDragInt(AppState& state, const char* label, int* v, float speed,
                 int vmin, int vmax);
// InputDouble drop-in (typed entry rather than drag) with the same
// expression evaluation on commit.
bool ExprInputDouble(AppState& state, const char* label, double* v,
                     const char* fmt);
// Two components sharing one row + label (a DragFloat2 drop-in; the label's
// visible part is everything before "##").
bool ExprDragFloat2(AppState& state, const char* label, float v[2],
                    float speed);

} // namespace ui
