// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "imgui.h"
#include "imgui_internal.h" // GetCurrentWindowRead (the auto-resize case)

#include <algorithm>

// Shared layout for the dock panels (U4). ImGui's default puts a widget's
// label to its RIGHT, where the panel edge clips it — a 210 px dock turns
// "Document" into "Docume" and "Orientation" into "Orienta". These keep a
// label readable at any width, which is what makes the panels safe to
// resize freely.
//
// This lived inside FeatureEditor.cpp for a while and nothing else could
// reach it, so every other panel kept ImGui's clipping default. That is the
// whole reason the Drawing, Variables and Configurations panels were
// unreadable; there was never a second design, just an unshared one.
namespace ui {

// Label in a left column, widget filling the rest of the row. `reserveRight`
// keeps room for a trailing SmallButton ("fx"/"123") on the same row.
inline void LeftLabel(const char* label, float reserveRight = 0.0f) {
  // Auto-resize windows (the preview dialog): avail-based widths are circular
  // there (the window sizes to content), so use a fixed two-column layout —
  // this also keeps the dialog one row per field, not towering over the
  // viewport clicks the GUI tests make.
  if (ImGui::GetCurrentWindowRead()->Flags &
      ImGuiWindowFlags_AlwaysAutoResize) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(std::max(ImGui::GetFontSize() * 7.0f,
                             ImGui::CalcTextSize(label).x +
                                 ImGui::GetStyle().ItemSpacing.x * 2.0f));
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f - reserveRight);
    return;
  }
  const float avail = ImGui::GetContentRegionAvail().x;
  const float col = std::max(ImGui::GetFontSize() * 7.0f, avail * 0.40f);
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  // Two columns only when the field keeps a usable width; in a narrow panel
  // stack the label above the full-width field instead of squeezing it.
  if (avail - col >= ImGui::GetFontSize() * 8.0f)
    ImGui::SameLine(col);
  ImGui::SetNextItemWidth(reserveRight > 0.0f ? -reserveRight : -FLT_MIN);
}

// Width a SmallButton takes on the row, for LeftLabel's reserveRight.
inline float SmallButtonReserve(const char* text) {
  const ImGuiStyle& style = ImGui::GetStyle();
  return ImGui::CalcTextSize(text).x + style.FramePadding.x * 2.0f +
         style.ItemSpacing.x;
}

// Width of a Button / RadioButton with this label, ignoring the "###id" half
// (CalcTextSize's hide-after-double-hash does that).
inline float ButtonWidth(const char* label) {
  return ImGui::CalcTextSize(label, nullptr, true).x +
         ImGui::GetStyle().FramePadding.x * 2.0f;
}
inline float RadioWidth(const char* label) {
  const ImGuiStyle& style = ImGui::GetStyle();
  return ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetFrameHeight() +
         style.ItemInnerSpacing.x;
}

// A row of buttons that WRAPS at the panel edge instead of running past it.
// ImGui::SameLine() never wraps, so three buttons in a 210 px dock drew the
// third one outside the panel, where it cannot be clicked or even seen — the
// simulation panel lost a whole boundary-condition type (Pressure) and one of
// its two result fields (Displacement) that way.
//
// Usage: call Next(width) before each widget; it emits the SameLine when the
// widget fits on the current line and starts a new one when it does not.
class WrapRow {
public:
  WrapRow() : m_avail(ImGui::GetContentRegionAvail().x) {}
  void Next(float width) {
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    if (m_x > 0.0f && m_x + spacing + width <= m_avail) {
      ImGui::SameLine();
      m_x += spacing + width;
    } else {
      m_x = width; // first on the line, or wrapped onto a fresh one
    }
  }

private:
  float m_avail = 0.0f;
  float m_x = 0.0f;
};

// A labelled widget whose label sits LEFT of it and never clips. The label
// doubles as the widget's addressable name, so pass "Orientation###dwg_
// orientation" style ids the way the rest of the app does — the visible half
// is drawn here, the id half goes to the widget.
//
// Usage: LeftLabel("Scale"); ExprInputDouble(state, "###dwg_scale", &v,
// "%.3f");
//
// Kept as two calls rather than a wrapper per widget type: ImGui has thirty
// input widgets and wrapping each one is how a UI layer stops being ImGui.

} // namespace ui
