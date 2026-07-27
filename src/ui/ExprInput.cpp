#include "ui/ExprInput.h"

#include "core/Expression.h"

#include "imgui.h"
#include "imgui_internal.h" // TempInputIsActive / GetInputTextState

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

// Mirrors the last-drawn item's text buffer while it is being edited (a
// drag's Ctrl+click temp input, or an InputScalar's field); on deactivate,
// evaluates a non-plain-number buffer against the document variables.
// Returns true (with *out set) when an expression committed. (TextA is the
// live buffer — TextSrc is only valid inside InputText.)
bool ExprCommitOnLastItem(AppState& state, double* out) {
  const ImGuiID id = ImGui::GetItemID();
  if (ImGui::IsItemActive()) {
    if (ImGuiInputTextState* st = ImGui::GetInputTextState(id)) {
      std::snprintf(g_buf, sizeof(g_buf), "%.*s", static_cast<int>(st->TextLen),
                    st->TextA.Data);
      g_trackId = id;
    }
  }
  if (g_trackId != id || !ImGui::IsItemDeactivatedAfterEdit())
    return false;
  g_trackId = 0;
  char* end = nullptr;
  std::strtod(g_buf, &end);
  if (end != nullptr && *end == '\0')
    return false; // a plain number: the drag already applied it
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

} // namespace

bool ExprDragDouble(AppState& state, const char* label, double* v, float speed,
                    double vmin, double vmax, const char* fmt) {
  bool changed = ImGui::DragScalar(label, ImGuiDataType_Double, v, speed, &vmin,
                                   &vmax, fmt, ImGuiSliderFlags_AlwaysClamp);
  double expr = 0.0;
  if (ExprCommitOnLastItem(state, &expr)) {
    *v = std::clamp(expr, vmin, vmax);
    changed = true;
  }
  return changed;
}

bool ExprDragFloat(AppState& state, const char* label, float* v, float speed,
                   float vmin, float vmax, const char* fmt) {
  bool changed = ImGui::DragFloat(label, v, speed, vmin, vmax, fmt);
  double expr = 0.0;
  if (ExprCommitOnLastItem(state, &expr)) {
    *v = std::clamp(static_cast<float>(expr), vmin, vmax);
    changed = true;
  }
  return changed;
}

bool ExprDragInt(AppState& state, const char* label, int* v, float speed,
                 int vmin, int vmax) {
  bool changed = ImGui::DragInt(label, v, speed, vmin, vmax);
  double expr = 0.0;
  if (ExprCommitOnLastItem(state, &expr)) {
    *v = std::clamp(static_cast<int>(std::lround(expr)), vmin, vmax);
    changed = true;
  }
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
