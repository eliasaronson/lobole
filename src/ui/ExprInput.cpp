#include "ui/ExprInput.h"

#include "core/Expression.h"
#include "ui/ExprComplete.h"

#include "imgui.h"
#include "imgui_internal.h" // GetInputTextState / ImParseFormatTrimDecorations

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

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
int g_exprCaret = -1;    // where to put the caret on refocus (-1: select all)

constexpr int kArmFrames = 8;

// "12", " 3.5 " — a value, not an expression. The distinction is what decides
// whether there is any TEXT worth keeping.
bool IsPlainNumber(const char* text) {
  char* end = nullptr;
  std::strtod(text, &end);
  if (end == text || end == nullptr)
    return false;
  while (*end == ' ')
    end++;
  return *end == '\0';
}

void LeaveExprMode() {
  ExprCompleteClose();
  g_exprId = 0;
  g_exprArm = 0;
  g_exprLive = false;
  g_exprCaret = -1;
}

// True while `label`'s field is standing in as a text field. Also the place
// a field abandoned mid-edit is cleaned up: if the stand-in stopped being
// drawn (its panel closed, its feature was deleted), the next expression
// field to come along releases the slot rather than inheriting it.
// What a typed value resolves against. The part's table on a part tab (it
// chains to the project's); the PROJECT's on any other kind of tab. This used
// to be `state.document->Symbols()` behind a null check that simply refused
// to commit -- so on a job tab every numeric field took the typed text and
// then snapped back to the old value on Enter, with the error going to a
// prompt bar that tab does not draw. A tool diameter that cannot be changed.
const SymbolTable& SymbolsFor(const AppState& state) {
  return state.document != nullptr ? state.document->Symbols()
                                   : state.projectSymbols;
}

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

// The gestures that open text entry on the drag just submitted. ImGui's own
// DragScalar accepts three (imgui_widgets.cpp, "Tabbing or CTRL+click on Drag
// turns it into an InputText") and ImGuiSliderFlags_NoInput turns off all
// three, so all three are rebuilt here — DOUBLE-CLICK especially, which is
// the one people find without being told.
bool WantsExprMode() {
  const ImGuiIO& io = ImGui::GetIO();
  if (ImGui::IsItemClicked() && io.KeyCtrl)
    return true;
  // The second click of a double-click; the first started a drag, exactly as
  // it does in ImGui's version.
  if (io.MouseClickedCount[0] == 2 &&
      ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
    return true;
  // Keyboard: Enter on a nav-focused field. Activation without the mouse.
  return ImGui::IsItemActivated() && !ImGui::IsMouseDown(ImGuiMouseButton_Left);
}

// Hand the field to the stand-in, seeded with the value as it displays it.
void ArmExprMode(const char* seed) {
  if (!WantsExprMode())
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
bool DrawExprField(AppState& state, const char* label, double* out,
                   std::string* exprOut) {
  const ImGuiID id = g_exprId;
  // BEFORE the field: last frame's completion popup, and the pick it may
  // produce. A pick arrives on the same frame the click deactivates the
  // field, so it has to be known before the field is asked whether it
  // committed.
  const ExprPick pick =
      ExprCompletePopup(state, id, g_exprText, sizeof(g_exprText));
  if (pick.picked) {
    g_exprCaret = pick.caret;
    g_exprLive = false; // re-focus, and reload the field from our buffer
    g_exprArm = kArmFrames;
  }
  if (!g_exprLive) {
    // Asked for every frame until it lands, not once: a pick made with the
    // MOUSE requests focus while the button is still down, and the click
    // that made it has already claimed the active id for the popup row —
    // that first request is simply lost.
    ImGui::SetKeyboardFocusHere();
    g_exprArm--;
    // ImGui keeps ONE InputTextState, keyed by item id, and on re-activation
    // it restores the text IT holds — which for a field edited earlier in
    // the session is that earlier edit, written straight over the seed we
    // just seeded. (Without this the field reopens holding the last
    // expression typed into it, and a keystroke appends to that: "2.000"
    // seeded, "1+1+...+1" restored, "30" typed, 146 committed.) Ours is the
    // live buffer, and this is how you say so.
    if (ImGuiInputTextState* st = ImGui::GetInputTextState(g_exprId)) {
      st->ReloadUserBufAndSelectAll();
      if (g_exprCaret >= 0) {
        // ...except after a pick, where the caret belongs after the name
        // that was just inserted rather than around the whole expression.
        st->ReloadSelectionStart = st->ReloadSelectionEnd = g_exprCaret;
      }
    }
  }
  // AutoSelectAll for parity with the Ctrl+click field this replaces: the
  // value arrives selected, so the first keystroke overwrites it rather than
  // appending to it.
  ImGui::InputText(label, g_exprText, sizeof(g_exprText),
                   ImGuiInputTextFlags_AutoSelectAll);
  if (pick.picked) {
    // Applied AFTER the InputText, because a deactivating one writes its own
    // text into the buffer on its way out — but BEFORE the still-active
    // check, because only a mouse pick deactivates the field. A pick made
    // with Tab or Enter leaves it active, and an early return there would
    // drop the completion on the floor.
    std::snprintf(g_exprText, sizeof(g_exprText), "%s", pick.text.c_str());
    return false; // a pick is not a commit: the field stays open
  }
  ExprCompleteRefresh(state, id, g_exprText);
  if (ImGui::IsItemActive()) {
    g_exprLive = true;
    g_exprCaret = -1;
    return false; // still typing
  }
  if (!g_exprLive && g_exprArm > 0)
    return false; // focus asked for, not arrived yet

  // Leaving the field: Enter, a click elsewhere, Escape, or focus that never
  // came (the field then simply goes back to being a drag).
  const bool commit =
      g_exprLive && !ImGui::IsKeyPressed(ImGuiKey_Escape, false);
  LeaveExprMode();
  if (!commit || g_exprText[0] == '\0')
    return false;
  // No plain-number shortcut: the drag did NOT apply this one, we did.
  // EvaluateText handles a bare number (and a unit-suffixed one) anyway.
  const auto r =
      ExpressionEngine::Default().EvaluateText(g_exprText, SymbolsFor(state));
  if (!r.has_value()) {
    state.sketchEdit.actionMsg = r.error().message; // prompt-bar feedback
    return false;
  }
  *out = *r;
  if (exprOut != nullptr && !IsPlainNumber(g_exprText))
    *exprOut = g_exprText;
  return true;
}

// Mirrors the last-drawn item's text buffer while it is being edited, and on
// deactivate evaluates a non-plain-number buffer against the document
// variables. Returns true (with *out set) when an expression committed.
// (TextA is the live buffer — TextSrc is only valid inside InputText.)
//
// This is the path for ExprInputDouble, which is an ImGui InputScalar and
// keeps its own buffer; the drag fields above no longer come through here.
bool ExprCommitOnItem(AppState& state, ImGuiID id, double* out,
                      std::string* exprOut = nullptr) {
  if (ImGui::GetActiveID() == id) {
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
  const auto r =
      ExpressionEngine::Default().EvaluateText(g_buf, SymbolsFor(state));
  if (!r.has_value()) {
    state.sketchEdit.actionMsg = r.error().message; // prompt-bar feedback
    return false;
  }
  *out = *r;
  if (exprOut != nullptr)
    *exprOut = g_buf; // reached only for a non-plain-number buffer
  return true;
}

bool ExprCommitOnLastItem(AppState& state, double* out) {
  return ExprCommitOnItem(state, ImGui::GetItemID(), out);
}

// The id of the text field inside an InputScalar. With step buttons ImGui
// wraps it in PushID(label) + InputText(""), so the group's id is not the
// one to watch; without them the field IS the item. InputScalarN nests one
// more level, PushID(i), per component.
ImGuiID InnerFieldId(const char* label, int component = -1) {
  ImGui::PushID(label);
  if (component >= 0)
    ImGui::PushID(component);
  const ImGuiID id = ImGui::GetID("");
  if (component >= 0)
    ImGui::PopID();
  ImGui::PopID();
  return id;
}

// The drag flags every field here shares: clamp typed/dragged values, and
// leave the Ctrl+click text entry to us.
constexpr ImGuiSliderFlags kDragFlags =
    ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput;

} // namespace

bool ExprDragDouble(AppState& state, const char* label, double* v, float speed,
                    double vmin, double vmax, const char* fmt,
                    std::string* exprOut) {
  if (InExprMode(label)) {
    double expr = 0.0;
    if (!DrawExprField(state, label, &expr, exprOut))
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
    if (!DrawExprField(state, label, &expr, nullptr))
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
                 int vmin, int vmax, const char* fmt) {
  if (InExprMode(label)) {
    double expr = 0.0;
    if (!DrawExprField(state, label, &expr, nullptr))
      return false;
    *v = std::clamp(static_cast<int>(std::lround(expr)), vmin, vmax);
    return true;
  }
  const bool changed = ImGui::DragScalar(label, ImGuiDataType_S32, v, speed,
                                         &vmin, &vmax, fmt, kDragFlags);
  char seed[32];
  std::snprintf(seed, sizeof(seed), "%d", *v);
  ArmExprMode(seed);
  return changed;
}

bool ExprInputDouble(AppState& state, const char* label, double* v,
                     const char* fmt, std::string* exprOut) {
  bool changed = ImGui::InputDouble(label, v, 0.0, 0.0, fmt);
  double expr = 0.0;
  if (ExprCommitOnItem(state, ImGui::GetItemID(), &expr, exprOut)) {
    *v = expr;
    changed = true;
  }
  return changed;
}

namespace {

// N drag components sharing one row and one trailing label, laid out the way
// ImGui's DragFloatN does. ImGui's own DragScalarN cannot be reused: each
// component needs its own stand-in text field, and the stand-in REPLACES the
// drag rather than sitting beside it.
bool ExprDragFloatN(AppState& state, const char* label, float* v, int n,
                    float speed) {
  static const char* const kIds[3] = {"##x", "##y", "##z"};
  // Grouped exactly as ImGui's DragScalarN groups its components: EndGroup
  // forwards the active/deactivated status of whichever component the user
  // is in to the row as a whole, which is what the callers query
  // (IsItemActivated to snapshot for undo, IsItemDeactivatedAfterEdit to
  // commit). Without it a three-component row silently stops being undoable.
  ImGui::BeginGroup();
  ImGui::PushID(label);
  const float w = ImGui::CalcItemWidth();
  const float inner = ImGui::GetStyle().ItemInnerSpacing.x;
  const float each = std::max(1.0f, (w - inner * static_cast<float>(n - 1)) /
                                        static_cast<float>(n));
  bool changed = false;
  for (int i = 0; i < n; i++) {
    if (i > 0)
      ImGui::SameLine(0.0f, inner);
    ImGui::SetNextItemWidth(each);
    changed |=
        ExprDragFloat(state, kIds[i], &v[i], speed, -FLT_MAX, FLT_MAX, "%.3f");
  }
  const char* hidden = std::strstr(label, "##");
  const int visible = hidden != nullptr ? static_cast<int>(hidden - label)
                                        : static_cast<int>(std::strlen(label));
  if (visible > 0) {
    ImGui::SameLine(0.0f, inner);
    ImGui::TextUnformatted(label, label + visible);
  }
  ImGui::PopID();
  ImGui::EndGroup();
  return changed;
}

} // namespace

bool ExprDragFloat2(AppState& state, const char* label, float v[2],
                    float speed) {
  return ExprDragFloatN(state, label, v, 2, speed);
}

bool ExprDragFloat3(AppState& state, const char* label, float v[3],
                    float speed) {
  return ExprDragFloatN(state, label, v, 3, speed);
}

bool ExprInputFloat(AppState& state, const char* label, float* v,
                    const char* fmt) {
  bool changed = ImGui::InputFloat(label, v, 0.0f, 0.0f, fmt);
  double expr = 0.0;
  if (ExprCommitOnLastItem(state, &expr)) {
    *v = static_cast<float>(expr);
    changed = true;
  }
  return changed;
}

bool ExprInputInt(AppState& state, const char* label, int* v,
                  std::string* exprOut) {
  const ImGuiID field = InnerFieldId(label);
  bool changed = ImGui::InputInt(label, v);
  double expr = 0.0;
  if (ExprCommitOnItem(state, field, &expr, exprOut)) {
    *v = static_cast<int>(std::lround(expr));
    changed = true;
  }
  return changed;
}

bool ExprInputFloat3(AppState& state, const char* label, float v[3],
                     const char* fmt) {
  ImGuiID fields[3];
  for (int i = 0; i < 3; i++)
    fields[i] = InnerFieldId(label, i);
  bool changed = ImGui::InputFloat3(label, v, fmt);
  for (int i = 0; i < 3; i++) {
    double expr = 0.0;
    if (ExprCommitOnItem(state, fields[i], &expr)) {
      v[i] = static_cast<float>(expr);
      changed = true;
    }
  }
  return changed;
}

} // namespace ui
