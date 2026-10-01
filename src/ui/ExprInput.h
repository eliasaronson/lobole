#pragma once

#include "ui/AppState.h"

#include <string>

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
//
// The float field is for view state alone -- a section's offset and tilt.
// Anything the model holds is edited in double.
bool ExprDragFloat(AppState& state, const char* label, float* v, float speed,
                   float vmin, float vmax, const char* fmt);
// `exprOut`, where given, receives the expression TEXT that produced the
// commit, empty when a plain number was typed. Only a field with somewhere to
// STORE that text should pass it — a feature dimension keeps its `*Expr` slot
// alive that way, so "wall_d" stays a link to the variable instead of
// collapsing to the 20 it happened to evaluate to. Everything else evaluates
// and discards, which is all it can do without a slot of its own.
bool ExprDragDouble(AppState& state, const char* label, double* v, float speed,
                    double vmin, double vmax, const char* fmt,
                    std::string* exprOut = nullptr);
bool ExprDragInt(AppState& state, const char* label, int* v, float speed,
                 int vmin, int vmax, const char* fmt = "%d");
// InputDouble drop-in (typed entry rather than drag) with the same
// expression evaluation on commit.
bool ExprInputDouble(AppState& state, const char* label, double* v,
                     const char* fmt, std::string* exprOut = nullptr);
// Two or three components sharing one row + label (DragFloat2/3 drop-ins;
// the label's visible part is everything before "##"). Over doubles only:
// what they edit is the model, and a row that went through float would round
// it on every edit (docs/conventions.md#the-model-is-double).
bool ExprDragFloat2(AppState& state, const char* label, double v[2],
                    float speed);
bool ExprDragFloat3(AppState& state, const char* label, double v[3],
                    float speed);
// InputInt / InputScalarN drop-ins. These keep ImGui's own widget (they are
// text fields already, with a buffer big enough to type into) and only add
// the evaluation on commit.
bool ExprInputInt(AppState& state, const char* label, int* v,
                  std::string* exprOut = nullptr);
bool ExprInputDouble3(AppState& state, const char* label, double v[3],
                      const char* fmt);

} // namespace ui
