#include "ui/AppActions.h"

#include "imgui.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

namespace ui {

namespace {

// Sheet extent in millimetres for the current orientation (portrait short/long
// from SheetExtentMM, swapped for landscape).
glm::dvec2 SheetExtent(const Sheet& sheet) {
  glm::dvec2 ext = SheetExtentMM(sheet.size);
  if (sheet.landscape) ext = {ext.y, ext.x};
  return ext;
}

// The screen pixel at the centre of the viewport region.
ImVec2 ViewportCenter(const AppState& state) {
  return ImVec2(state.viewportPos.x + state.viewportSize.x * 0.5f,
                state.viewportPos.y + state.viewportSize.y * 0.5f);
}

// Sheet-mm (origin bottom-left, +Y up) -> screen pixels (+Y down).
ImVec2 SheetToScreen(const AppState& state, glm::dvec2 mm) {
  const ImVec2 c = ViewportCenter(state);
  const double s = state.sheetView.scale;
  return ImVec2(static_cast<float>(c.x + (mm.x - state.sheetView.offset.x) * s),
                static_cast<float>(c.y - (mm.y - state.sheetView.offset.y) * s));
}

glm::dvec2 ScreenToSheet(const AppState& state, ImVec2 px) {
  const ImVec2 c = ViewportCenter(state);
  const double s = state.sheetView.scale;
  return {state.sheetView.offset.x + (px.x - c.x) / s,
          state.sheetView.offset.y - (px.y - c.y) / s};
}

// Centre the sheet in the viewport at ~90% fill — the scale==0 first-paint
// state, and the "fit" button.
void FitSheet(AppState& state) {
  if (!state.drawing) return;
  const glm::dvec2 ext = SheetExtent(state.drawing->GetSheet());
  const double vpW = state.viewportSize.x, vpH = state.viewportSize.y;
  if (ext.x <= 0 || ext.y <= 0 || vpW <= 0 || vpH <= 0) {
    state.sheetView.scale = 1.0;
  } else {
    state.sheetView.scale = 0.9 * std::min(vpW / ext.x, vpH / ext.y);
  }
  state.sheetView.offset = ext * 0.5;  // sheet centre at the viewport centre
}

const char* kSizeLabels[] = {"A4", "A3", "A2", "A1", "A0", "Letter"};

}  // namespace

void HandleSheetInput(AppState& state) {
  if (!state.drawing) return;
  ImGuiIO& io = ImGui::GetIO();
  if (state.sheetView.scale <= 0.0) FitSheet(state);  // first paint

  const bool overViewport = !io.WantCaptureMouse;

  // Scroll-to-cursor zoom: keep the sheet point under the cursor fixed.
  if (overViewport && io.MouseWheel != 0.0f) {
    const glm::dvec2 before = ScreenToSheet(state, io.MousePos);
    state.sheetView.scale *= std::pow(1.1, io.MouseWheel);
    state.sheetView.scale = std::clamp(state.sheetView.scale, 0.02, 200.0);
    const glm::dvec2 after = ScreenToSheet(state, io.MousePos);
    state.sheetView.offset += before - after;
  }

  // Middle-drag pan (no orbit camera on a sheet).
  if (overViewport && ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
    state.sheetView.panning = true;
  if (ImGui::IsMouseReleased(ImGuiMouseButton_Middle))
    state.sheetView.panning = false;
  if (state.sheetView.panning &&
      (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f)) {
    const double s = state.sheetView.scale > 0 ? state.sheetView.scale : 1.0;
    state.sheetView.offset.x -= io.MouseDelta.x / s;
    state.sheetView.offset.y += io.MouseDelta.y / s;  // screen +Y is sheet -Y
  }
}

void DrawSheet(AppState& state) {
  if (!state.drawing) return;
  if (state.sheetView.scale <= 0.0) FitSheet(state);
  const Sheet& sheet = state.drawing->GetSheet();
  const glm::dvec2 ext = SheetExtent(sheet);

  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  // Clip to the viewport region so the sheet never paints under the panels.
  dl->PushClipRect(ImVec2(state.viewportPos.x, state.viewportPos.y),
                   ImVec2(state.viewportPos.x + state.viewportSize.x,
                          state.viewportPos.y + state.viewportSize.y),
                   true);

  const ImVec2 bl = SheetToScreen(state, {0, 0});
  const ImVec2 tr = SheetToScreen(state, ext);
  // Paper: a white page with a soft drop shadow.
  dl->AddRectFilled(ImVec2(bl.x + 4, tr.y + 4), ImVec2(tr.x + 4, bl.y + 4),
                    IM_COL32(0, 0, 0, 60), 0.0f);
  dl->AddRectFilled(ImVec2(bl.x, tr.y), ImVec2(tr.x, bl.y),
                    IM_COL32(250, 250, 250, 255));

  const ImU32 ink = IM_COL32(30, 30, 30, 255);
  // Drawing border: a margin inset from the paper edge (ISO ~10 mm).
  const double margin = 10.0;
  const ImVec2 mbl = SheetToScreen(state, {margin, margin});
  const ImVec2 mtr = SheetToScreen(state, {ext.x - margin, ext.y - margin});
  dl->AddRect(mbl, mtr, ink, 0.0f, 0, 1.5f);

  // Title block: a fixed-size labelled table pinned to the bottom-right corner
  // of the border. Sheet-anchored, so it scales with zoom.
  const double tbW = std::min(160.0, ext.x - 2 * margin);
  const double tbH = 32.0;
  const double x0 = ext.x - margin - tbW, y0 = margin;
  const std::array<std::pair<const char*, std::string>, 4> fields = {{
      {"PROJECT", sheet.titleBlock.project},
      {"DOCUMENT", sheet.titleBlock.document},
      {"AUTHOR", sheet.titleBlock.author},
      {"DATE", sheet.titleBlock.date},
  }};
  dl->AddRect(SheetToScreen(state, {x0, y0}),
              SheetToScreen(state, {x0 + tbW, y0 + tbH}), ink, 0.0f, 0, 1.5f);
  const double rowH = tbH / fields.size();
  const float fontPx = static_cast<float>(3.0 * state.sheetView.scale);
  for (size_t i = 0; i < fields.size(); ++i) {
    const double ry = y0 + tbH - (i + 1) * rowH;  // top row first
    if (i > 0)
      dl->AddLine(SheetToScreen(state, {x0, ry + rowH}),
                  SheetToScreen(state, {x0 + tbW, ry + rowH}), ink, 1.0f);
    if (fontPx >= 4.0f) {  // legible only past a zoom threshold
      ImFont* font = ImGui::GetFont();
      const ImVec2 labelPos = SheetToScreen(state, {x0 + 3.0, ry + rowH - 3.0});
      dl->AddText(font, fontPx, labelPos, IM_COL32(120, 120, 120, 255),
                  fields[i].first);
      const ImVec2 valPos = SheetToScreen(state, {x0 + 42.0, ry + rowH - 3.0});
      dl->AddText(font, fontPx, valPos, ink, fields[i].second.c_str());
    }
  }

  dl->PopClipRect();
}

// ---- The sheet-settings panel (Model dock for a drawing tab) ---------------

namespace {

// A text field backed by a std::string (the OccurrencePanel idiom). Snapshots
// the drawing for undo when the field first gains focus, so one edit session
// is one undo step (InputText otherwise fires per keystroke). Returns true on
// any character change this frame.
bool TitleField(AppState& state, const char* label, std::string& value) {
  char buf[256];
  std::snprintf(buf, sizeof(buf), "%s", value.c_str());
  const bool changed = ImGui::InputText(label, buf, sizeof(buf));
  if (ImGui::IsItemActivated()) SnapshotDrawing(state);
  if (changed) value = buf;
  return changed;
}

}  // namespace

void DrawSheetPanel(AppState& state) {
  if (!state.drawing) return;
  ImGui::Begin("Drawing");

  Sheet& sheet = state.drawing->GetSheet();

  ImGui::TextUnformatted("Sheet");
  ImGui::Separator();

  int sizeIdx = static_cast<int>(sheet.size);
  if (ImGui::Combo("Size", &sizeIdx, kSizeLabels, IM_ARRAYSIZE(kSizeLabels))) {
    SnapshotDrawing(state);
    sheet.size = static_cast<SheetSize>(sizeIdx);
    state.drawing->BumpVersion();
    state.sheetView.scale = 0.0;  // refit for the new paper size
  }
  bool landscape = sheet.landscape;
  if (ImGui::Checkbox("Landscape", &landscape)) {
    SnapshotDrawing(state);
    sheet.landscape = landscape;
    state.drawing->BumpVersion();
    state.sheetView.scale = 0.0;
  }

  ImGui::Spacing();
  ImGui::TextUnformatted("Title block");
  ImGui::Separator();
  bool edited = false;
  edited |= TitleField(state, "Project", sheet.titleBlock.project);
  edited |= TitleField(state, "Document", sheet.titleBlock.document);
  edited |= TitleField(state, "Author", sheet.titleBlock.author);
  edited |= TitleField(state, "Date", sheet.titleBlock.date);
  if (edited) state.drawing->BumpVersion();

  ImGui::Spacing();
  if (ImGui::Button("Fit")) state.sheetView.scale = 0.0;
  ImGui::SameLine();
  if (ImGui::Button("Undo")) UndoDrawing(state);
  ImGui::SameLine();
  if (ImGui::Button("Redo")) RedoDrawing(state);

  ImGui::Spacing();
  ImGui::TextDisabled("%zu view(s), %zu dimension(s)", state.drawing->ViewCount(),
                      state.drawing->DimCount());
  ImGui::TextDisabled("Views and dimensions arrive in the next milestone.");

  ImGui::End();
}

}  // namespace ui
