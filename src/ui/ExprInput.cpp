#include "ui/ExprInput.h"

#include "core/Expression.h"

#include "imgui.h"
#include "imgui_internal.h" // GetInputTextState / ImParseFormatTrimDecorations

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ui {

namespace {

// One slot suffices: ImGui has at most one temp text input active.
ImGuiID g_trackId = 0;
char g_buf[64];

// --- Expression mode ------------------------------------------------------
// A drag field in expression mode draws OUR InputText in place of the
// DragScalar, over a buffer we own. ImGui's own Ctrl+click text entry
// (TempInputScalar) is turned off with ImGuiSliderFlags_NoInput because its
// buffer is a 32-byte stack array refilled from the numeric value every
// frame: past 31 characters it silently refuses the keystrokes, and nothing
// can be written into it from outside — which the completion popup needs.
ImGuiID g_exprId = 0;    // the field standing in as a text field, if any
char g_exprText[256];    // ...and the text it is editing
int g_exprArm = 0;       // frames left to wait for keyboard focus to arrive
bool g_exprLive = false; // the stand-in has held focus at least once
int g_exprFrame = 0;     // last frame the stand-in was drawn

constexpr int kArmFrames = 8;

void LeaveExprMode() {
  g_exprId = 0;
  g_exprArm = 0;
  g_exprLive = false;
}

// True while `label`'s field is standing in as a text field. Also the place
// a field abandoned mid-edit is cleaned up: if the stand-in stopped being
// drawn (its panel closed, its feature was deleted), the next expression
// field to come along releases the slot rather than inheriting it.
bool InExprMode(const char* label) {
  if (g_exprId == 0)
    return false;
  if (ImGui::GetFrameCount() > g_exprFrame + 1) {
    LeaveExprMode();
    return false;
  }
  if (g_exprId != ImGui::GetID(label))
    return false;
  g_exprFrame = ImGui::GetFrameCount();
  return true;
}

// Ctrl+click on the drag just submitted hands the field to the stand-in,
// seeded with the value as the field displays it.
void ArmExprMode(const char* seed) {
  if (!ImGui::IsItemClicked() || !ImGui::GetIO().KeyCtrl)
    return;
  std::snprintf(g_exprText, sizeof(g_exprText), "%s", seed);
  g_exprId = ImGui::GetItemID();
  g_exprFrame = ImGui::GetFrameCount();
  g_exprArm = kArmFrames;
  g_exprLive = false;
}

// The seed text, formatted the way the drag displays it: the format may
// carry decorations ("%.1f°"), which have to go or the field opens holding
// something that will not parse.
void SeedFromDouble(char* out, size_t n, const char* fmt, double v) {
  char trimmed[32];
  const char* f = ImParseFormatTrimDecorations(fmt, trimmed, sizeof(trimmed));
  std::snprintf(out, n, (f != nullptr && f[0] != '\0') ? f : "%g", v);
  ImStrTrimBlanks(out);
}

// Draws the stand-in. Returns true (with *out set) once it commits.
bool DrawExprField(AppState& state, const char* label, double* out) {
  if (!g_exprLive) {
    if (g_exprArm == kArmFrames)
      ImGui::SetKeyboardFocusHere();
    g_exprArm--;
    // ImGui keeps ONE InputTextState, keyed by item id, and on re-activation
    // it restores the text IT holds — which for a field edited earlier in
    // the session is that earlier edit, written straight over the seed we
    // just seeded. (Without this the field reopens holding the last
    // expression typed into it, and a keystroke appends to that: "2.000"
    // seeded, "1+1+...+1" restored, "30" typed, 146 committed.) Ours is the
    // live buffer, and this is how you say so.
    if (ImGuiInputTextState* st = ImGui::GetInputTextState(g_exprId))
      st->ReloadUserBufAndSelectAll();
  }
  // AutoSelectAll for parity with the Ctrl+click field this replaces: the
  // value arrives selected, so the first keystroke overwrites it rather than
  // appending to it.
  ImGui::InputText(label, g_exprText, sizeof(g_exprText),
                   ImGuiInputTextFlags_AutoSelectAll);
  if (ImGui::IsItemActive()) {
    g_exprLive = true;
    return false; // still typing
  }
  if (!g_exprLive && g_exprArm > 0)
    return false; // focus asked for, not arrived yet

  // Leaving the field: Enter, a click elsewhere, Escape, or focus that never
  // came (the field then simply goes back to being a drag).
  const bool commit =
      g_exprLive && !ImGui::IsKeyPressed(ImGuiKey_Escape, false);
  LeaveExprMode();
  if (!commit || g_exprText[0] == '\0' || state.document == nullptr)
    return false;
  // No plain-number shortcut: the drag did NOT apply this one, we did.
  // EvaluateText handles a bare number (and a unit-suffixed one) anyway.
  const auto r = ExpressionEngine::Default().EvaluateText(
      g_exprText, state.document->Symbols());
  if (!r.has_value()) {
    state.sketchEdit.actionMsg = r.error().message; // prompt-bar feedback
    return false;
  }
  *out = *r;
  return true;
}

// Mirrors the last-drawn item's text buffer while it is being edited, and on
// deactivate evaluates a non-plain-number buffer against the document
// variables. Returns true (with *out set) when an expression committed.
// (TextA is the live buffer — TextSrc is only valid inside InputText.)
//
// This is the path for ExprInputDouble, which is an ImGui InputScalar and
// keeps its own buffer; the drag fields above no longer come through here.
bool ExprCommitOnLastItem(AppState& state, double* out) {
  const ImGuiID id = ImGui::GetItemID();
  if (ImGui::IsItemActive()) {
    if (ImGuiInputTextState* st = ImGui::GetInputTextState(id)) {
      std::snprintf(g_buf, sizeof(g_buf), "%.*s", static_cast<int>(st->TextLen),
                    st->TextA.Data);
      g_trackId = id;
    }
    return false; // still typing
  }
  if (g_trackId != id)
    return false;
  // We tracked a text buffer on this item last frame and it is not active
  // now, so it just committed. Deciding that OURSELVES matters (u14):
  // ImGui::IsItemDeactivatedAfterEdit() is FALSE whenever the typed text did
  // not parse as a leading number that MOVED the value, and "width/2" never
  // does — so the text that most needed evaluating was the text that never
  // reached the evaluator.
  g_trackId = 0;
  // Escape reverts the field; it is not a commit. ImGui does not expose the
  // revert, so read the key that caused it.
  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
    return false;
  char* end = nullptr;
  std::strtod(g_buf, &end);
  if (end != nullptr && *end == '\0')
    return false; // a plain number: the widget already applied it
  if (state.document == nullptr)
    return false;
  const auto r = ExpressionEngine::Default().EvaluateText(
      g_buf, state.document->Symbols());
  if (!r.has_value()) {
    state.sketchEdit.actionMsg = r.error().message; // prompt-bar feedback
    return false;
  }
  *out = *r;
  return true;
}

// The drag flags every field here shares: clamp typed/dragged values, and
// leave the Ctrl+click text entry to us.
constexpr ImGuiSliderFlags kDragFlags =
    ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput;

} // namespace

bool ExprDragDouble(AppState& state, const char* label, double* v, float speed,
                    double vmin, double vmax, const char* fmt) {
  if (InExprMode(label)) {
    double expr = 0.0;
    if (!DrawExprField(state, label, &expr))
      return false;
    *v = std::clamp(expr, vmin, vmax);
    return true;
  }
  const bool changed = ImGui::DragScalar(label, ImGuiDataType_Double, v, speed,
                                         &vmin, &vmax, fmt, kDragFlags);
  char seed[64];
  SeedFromDouble(seed, sizeof(seed), fmt, *v);
  ArmExprMode(seed);
  return changed;
}

bool ExprDragFloat(AppState& state, const char* label, float* v, float speed,
                   float vmin, float vmax, const char* fmt) {
  if (InExprMode(label)) {
    double expr = 0.0;
    if (!DrawExprField(state, label, &expr))
      return false;
    *v = std::clamp(static_cast<float>(expr), vmin, vmax);
    return true;
  }
  const bool changed = ImGui::DragScalar(label, ImGuiDataType_Float, v, speed,
                                         &vmin, &vmax, fmt, kDragFlags);
  char seed[64];
  SeedFromDouble(seed, sizeof(seed), fmt, static_cast<double>(*v));
  ArmExprMode(seed);
  return changed;
}

bool ExprDragInt(AppState& state, const char* label, int* v, float speed,
                 int vmin, int vmax) {
  if (InExprMode(label)) {
    double expr = 0.0;
    if (!DrawExprField(state, label, &expr))
      return false;
    *v = std::clamp(static_cast<int>(std::lround(expr)), vmin, vmax);
    return true;
  }
  const bool changed = ImGui::DragScalar(label, ImGuiDataType_S32, v, speed,
                                         &vmin, &vmax, "%d", kDragFlags);
  char seed[32];
  std::snprintf(seed, sizeof(seed), "%d", *v);
  ArmExprMode(seed);
  return changed;
}

bool ExprInputDouble(AppState& state, const char* label, double* v,
                     const char* fmt) {
  bool changed = ImGui::InputDouble(label, v, 0.0, 0.0, fmt);
  double expr = 0.0;
  if (ExprCommitOnLastItem(state, &expr)) {
    *v = expr;
    changed = true;
  }
  return changed;
}

bool ExprDragFloat2(AppState& state, const char* label, float v[2],
                    float speed) {
  ImGui::PushID(label);
  const float w = ImGui::CalcItemWidth();
  const float inner = ImGui::GetStyle().ItemInnerSpacing.x;
  const float half = std::max(1.0f, (w - inner) * 0.5f);
  ImGui::SetNextItemWidth(half);
  bool changed =
      ExprDragFloat(state, "##x", &v[0], speed, -FLT_MAX, FLT_MAX, "%.3f");
  ImGui::SameLine(0.0f, inner);
  ImGui::SetNextItemWidth(half);
  changed |=
      ExprDragFloat(state, "##y", &v[1], speed, -FLT_MAX, FLT_MAX, "%.3f");
  const char* hidden = std::strstr(label, "##");
  const int visible = hidden != nullptr ? static_cast<int>(hidden - label)
                                        : static_cast<int>(std::strlen(label));
  if (visible > 0) {
    ImGui::SameLine(0.0f, inner);
    ImGui::TextUnformatted(label, label + visible);
  }
  ImGui::PopID();
  return changed;
}

} // namespace ui
