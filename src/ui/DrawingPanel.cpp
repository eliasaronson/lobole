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

// A projected polyline in sheet mm -> screen, solid.
void StrokePolyline(ImDrawList* dl, AppState& state,
                    const std::vector<glm::dvec2>& pts, ImU32 col, float px) {
  for (size_t i = 1; i < pts.size(); ++i)
    dl->AddLine(SheetToScreen(state, pts[i - 1]), SheetToScreen(state, pts[i]),
                col, px);
}

// Hidden edges: dashed, in sheet-mm units so the dash pattern is stable under
// zoom (the design's CPU-dashing, in sheet space).
void StrokeDashed(ImDrawList* dl, AppState& state,
                  const std::vector<glm::dvec2>& pts, ImU32 col, float px) {
  const double dash = 1.6, gap = 1.2;  // sheet mm
  for (size_t i = 1; i < pts.size(); ++i) {
    const glm::dvec2 a = pts[i - 1], b = pts[i];
    const double len = glm::length(b - a);
    if (len < 1e-9) continue;
    const glm::dvec2 dir = (b - a) / len;
    for (double t = 0.0; t < len; t += dash + gap) {
      const double t2 = std::min(t + dash, len);
      dl->AddLine(SheetToScreen(state, a + dir * t),
                  SheetToScreen(state, a + dir * t2), col, px);
    }
  }
}

// A filled arrowhead at `tip` pointing along unit `dir` (sheet mm), ported from
// the sketch Arrow but in sheet space so it scales with zoom.
void SheetArrow(ImDrawList* dl, AppState& state, glm::dvec2 tip, glm::dvec2 dir,
                ImU32 col) {
  const glm::dvec2 n(-dir.y, dir.x);
  const double len = 2.2, w = 0.8;  // mm
  const glm::dvec2 base = tip - dir * len;
  dl->AddTriangleFilled(SheetToScreen(state, tip),
                        SheetToScreen(state, base + n * w),
                        SheetToScreen(state, base - n * w), col);
}

// Draws the active drawing's dimensions: extension/dimension lines + arrows +
// value text (linear), or a centre->rim leader (radial). A dangling dimension
// (a ref no longer resolves) draws its label in the error colour, never
// silently rebound — the toponaming T3 rule.
void DrawDims(ImDrawList* dl, AppState& state) {
  const ImU32 ink = IM_COL32(40, 60, 90, 255);
  const ImU32 err = IM_COL32(210, 80, 70, 255);
  const float fontPx = static_cast<float>(3.0 * state.sheetView.scale);
  ImFont* font = ImGui::GetFont();

  for (const DrawingDim& dim : state.drawing->GetDims()) {
    const drawing::ResolvedDim d = ResolveDrawingDim(state, dim);
    if (!d.resolved) {
      // Dangling: a red marker at the label offset from the (stored) offset.
      if (fontPx >= 4.0f)
        dl->AddText(font, fontPx, SheetToScreen(state, dim.labelOffset), err,
                    "dangling dim");
      continue;
    }
    if (d.radial) {
      dl->AddLine(SheetToScreen(state, d.center), SheetToScreen(state, d.rim),
                  ink, 1.2f);
      SheetArrow(dl, state, d.rim, glm::normalize(d.rim - d.center), ink);
    } else if (d.kind == DrawingDimKind::Angle) {
      // v1 angle: value only (rendered below).
    } else {
      dl->AddLine(SheetToScreen(state, d.ext1), SheetToScreen(state, d.dim1), ink, 0.8f);
      dl->AddLine(SheetToScreen(state, d.ext2), SheetToScreen(state, d.dim2), ink, 0.8f);
      dl->AddLine(SheetToScreen(state, d.dim1), SheetToScreen(state, d.dim2), ink, 1.2f);
      const double L = glm::length(d.dim2 - d.dim1);
      if (L > 1e-6) {
        const glm::dvec2 u = (d.dim2 - d.dim1) / L;
        SheetArrow(dl, state, d.dim1, u, ink);
        SheetArrow(dl, state, d.dim2, -u, ink);
      }
    }
    if (fontPx >= 4.0f) {
      const ImVec2 tp = SheetToScreen(state, d.labelPos);
      const ImVec2 tsz = font->CalcTextSizeA(fontPx, FLT_MAX, 0.0f, d.text.c_str());
      const ImVec2 org(tp.x - tsz.x * 0.5f, tp.y - tsz.y * 0.5f);
      dl->AddRectFilled(ImVec2(org.x - 2, org.y - 1),
                        ImVec2(org.x + tsz.x + 2, org.y + tsz.y + 1),
                        IM_COL32(250, 250, 250, 230), 2.0f);
      dl->AddText(font, fontPx, org, ink, d.text.c_str());
    }
  }
}

// Draws every projected view of the active drawing onto the sheet: visible
// edges solid, hidden edges dashed, a thin frame + label per view, and a stale
// badge when the source changed since the last projection.
void DrawViews(ImDrawList* dl, AppState& state) {
  const ImU32 visible = IM_COL32(20, 20, 24, 255);
  const ImU32 hidden = IM_COL32(120, 120, 130, 255);
  const ImU32 frame = IM_COL32(90, 120, 170, 130);
  const ImU32 stale = IM_COL32(210, 140, 40, 255);
  const float fontPx = static_cast<float>(2.6 * state.sheetView.scale);
  ImFont* font = ImGui::GetFont();

  for (const DrawingView& view : state.drawing->GetViews()) {
    const auto it = state.drawingRenders.find(view.id);
    if (it == state.drawingRenders.end()) continue;
    const drawing::SheetView& sv = it->second;

    for (const drawing::SheetCurve& c : sv.curves) {
      if (c.points.size() < 2) continue;
      if (c.visible)
        StrokePolyline(dl, state, c.points, visible, 1.6f);
      else
        StrokeDashed(dl, state, c.points, hidden, 1.0f);
    }

    if (sv.hasGeometry) {
      // A light frame + the view name/scale label above the geometry.
      const glm::dvec2 pad{2.0, 2.0};
      dl->AddRect(SheetToScreen(state, sv.bboxMin - pad),
                  SheetToScreen(state, sv.bboxMax + pad), frame, 0.0f, 0, 1.0f);
      if (fontPx >= 4.0f) {
        char label[128];
        std::snprintf(label, sizeof(label), "%s  1:%g",
                      view.name.empty() ? "view" : view.name.c_str(),
                      view.scale > 0 ? 1.0 / view.scale : 1.0);
        dl->AddText(font, fontPx,
                    SheetToScreen(state, {sv.bboxMin.x, sv.bboxMax.y + 6.0}),
                    frame, label);
      }
    }

    // Stale badge: the source advanced past this view's last projection, or
    // projection failed. Shown near the view's top-left.
    const bool isStale =
        DrawingSourceVersion(state, view.sourceDocId) != sv.solvedSourceVersion;
    if ((isStale || !sv.error.empty()) && fontPx >= 4.0f) {
      const glm::dvec2 at = sv.hasGeometry ? glm::dvec2(sv.bboxMin.x, sv.bboxMax.y + 12.0)
                                           : view.position;
      dl->AddText(font, fontPx, SheetToScreen(state, at), stale,
                  sv.error.empty() ? "stale - update" : sv.error.c_str());
    }
  }
}

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

  DrawViews(dl, state);
  DrawDims(dl, state);

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

const char* kOrientLabels[] = {"front", "back", "left", "right",
                               "top",   "bottom", "iso"};

// The view list + an "add view" control (source part + orientation + scale)
// and the explicit Update button. Split out to keep DrawSheetPanel legible.
void DrawViewsSection(AppState& state) {
  ImGui::Spacing();
  ImGui::TextUnformatted("Views");
  ImGui::Separator();

  if (ImGui::Button("Update views")) UpdateDrawingRenders(state);
  ImGui::SameLine();
  ImGui::TextDisabled("(re-projects; explicit)");

  // Add a view: pick a source part, orientation, scale.
  static int srcIdx = 0, orientIdx = 0;
  static float addScale = 1.0f;
  std::vector<const PartSlot*> partSlots;
  for (const PartSlot& s : state.parts)
    if (s.kind == DocKind::Part || s.kind == DocKind::Assembly)
      partSlots.push_back(&s);
  if (partSlots.empty()) {
    ImGui::TextDisabled("Add a part or assembly to project from.");
  } else {
    if (srcIdx >= static_cast<int>(partSlots.size())) srcIdx = 0;
    if (ImGui::BeginCombo("Source", partSlots[srcIdx]->name.c_str())) {
      for (int i = 0; i < static_cast<int>(partSlots.size()); ++i)
        if (ImGui::Selectable(partSlots[i]->name.c_str(), i == srcIdx))
          srcIdx = i;
      ImGui::EndCombo();
    }
    ImGui::Combo("Orientation", &orientIdx, kOrientLabels,
                 IM_ARRAYSIZE(kOrientLabels));
    ImGui::InputFloat("Scale", &addScale, 0.0f, 0.0f, "%.3f");
    if (ImGui::Button("Add view")) {
      glm::dvec2 ext = SheetExtentMM(state.drawing->GetSheet().size);
      if (state.drawing->GetSheet().landscape) ext = {ext.y, ext.x};
      DrawingView v;
      v.name = std::string(kOrientLabels[orientIdx]);
      v.sourceDocId = partSlots[srcIdx]->id;
      v.orientation = static_cast<ViewOrientation>(orientIdx);
      v.scale = addScale > 0 ? addScale : 1.0;
      v.position = ext * 0.5;
      SnapshotDrawing(state);
      state.drawing->AddView(v);
      UpdateDrawingRenders(state);
    }
  }

  // The view list: name, orientation, curve count / status, delete.
  for (const DrawingView& view : state.drawing->GetViews()) {
    ImGui::PushID(static_cast<int>(view.id));
    const auto it = state.drawingRenders.find(view.id);
    const char* status = "needs update";
    if (it != state.drawingRenders.end()) {
      if (!it->second.error.empty()) status = it->second.error.c_str();
      else status = it->second.hasGeometry ? "ok" : "empty";
    }
    ImGui::BulletText("%s [%s]  %zu curves - %s",
                      view.name.empty() ? "view" : view.name.c_str(),
                      ViewOrientationToString(view.orientation),
                      it != state.drawingRenders.end() ? it->second.curves.size()
                                                       : size_t{0},
                      status);
    ImGui::SameLine();
    if (ImGui::SmallButton("x")) {
      SnapshotDrawing(state);
      state.drawing->RemoveView(view.id);
      UpdateDrawingRenders(state);
      ImGui::PopID();
      break;  // container mutated; redraw next frame
    }
    ImGui::PopID();
  }
}

}  // namespace

void DrawSheetPanel(AppState& state) {
  if (!state.drawing) return;
  // Docks into the left "Model" node (the DockLayout slot the feature tree and
  // occurrence panel also use), so a drawing tab reserves the same panel space
  // instead of floating over the sheet.
  ImGui::Begin("Model");

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

  DrawViewsSection(state);

  ImGui::Spacing();
  ImGui::TextDisabled("%zu dimension(s) — dimensions arrive in D3.",
                      state.drawing->DimCount());

  ImGui::End();
}

}  // namespace ui
