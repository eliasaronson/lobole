#include "ui/DrawingPanel.h"
#include "ui/AppActions.h"
#include "ui/PanelLayout.h"
#include "ui/PanelTitles.h"

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

// Sheet extent in millimetres for the current orientation (shared chrome).
glm::dvec2 SheetExtent(const Sheet& sheet) { return ChromeOf(sheet).extent; }

// The screen pixel at the centre of the viewport region.
ImVec2 ViewportCenter(const AppState& state) {
  return ImVec2(state.viewportPos.x + state.viewportSize.x * 0.5f,
                state.viewportPos.y + state.viewportSize.y * 0.5f);
}

// Sheet-mm (origin bottom-left, +Y up) -> screen pixels (+Y down).
ImVec2 SheetToScreen(const AppState& state, glm::dvec2 mm) {
  const ImVec2 c = ViewportCenter(state);
  const double s = state.sheetView.scale;
  return ImVec2(
      static_cast<float>(c.x + (mm.x - state.sheetView.offset.x) * s),
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
  if (!state.drawing)
    return;
  const glm::dvec2 ext = SheetExtent(state.drawing->GetSheet());
  const double vpW = state.viewportSize.x, vpH = state.viewportSize.y;
  if (ext.x <= 0 || ext.y <= 0 || vpW <= 0 || vpH <= 0) {
    state.sheetView.scale = 1.0;
  } else {
    state.sheetView.scale = 0.9 * std::min(vpW / ext.x, vpH / ext.y);
  }
  state.sheetView.offset = ext * 0.5; // sheet centre at the viewport centre
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
  const double dash = 1.6, gap = 1.2; // sheet mm
  for (size_t i = 1; i < pts.size(); ++i) {
    const glm::dvec2 a = pts[i - 1], b = pts[i];
    const double len = glm::length(b - a);
    if (len < 1e-9)
      continue;
    const glm::dvec2 dir = (b - a) / len;
    for (double t = 0.0; t < len; t += dash + gap) {
      const double t2 = std::min(t + dash, len);
      dl->AddLine(SheetToScreen(state, a + dir * t),
                  SheetToScreen(state, a + dir * t2), col, px);
    }
  }
}

// How many refs a dimension kind needs before it can be created.
// Refs a dim kind needs before it commits; -1 = open-ended (2+ picks,
// Enter finishes — the pattern callout's pick-a-row flow).
int NeededRefs(DrawingDimKind k) {
  if (k == DrawingDimKind::Radius || k == DrawingDimKind::Diameter ||
      k == DrawingDimKind::HoleCallout)
    return 1;
  if (k == DrawingDimKind::PatternCallout)
    return -1;
  return 2;
}

double PointSegDist(glm::dvec2 p, glm::dvec2 a, glm::dvec2 b) {
  const glm::dvec2 ab = b - a;
  const double len2 = glm::dot(ab, ab);
  const double t =
      len2 < 1e-12 ? 0.0 : glm::clamp(glm::dot(p - a, ab) / len2, 0.0, 1.0);
  return glm::length(p - (a + t * ab));
}

// The projected curve nearest `cursor` (sheet mm) across all views, within
// `maxDist`, that carries a dimensionable ref. Sets `viewIdOut` to its view.
const drawing::SheetCurve* PickCurve(AppState& state, glm::dvec2 cursor,
                                     double maxDist, uint64_t& viewIdOut) {
  const drawing::SheetCurve* best = nullptr;
  double bestDist = maxDist;
  for (const DrawingView& view : state.drawing->GetViews()) {
    const auto it = state.drawingRenders.find(view.id);
    if (it == state.drawingRenders.end())
      continue;
    for (const drawing::SheetCurve& c : it->second.curves) {
      if (!c.edge.IsSet() && !c.silFace.IsSet())
        continue;
      for (size_t i = 1; i < c.points.size(); ++i) {
        const double d = PointSegDist(cursor, c.points[i - 1], c.points[i]);
        if (d < bestDist) {
          bestDist = d;
          best = &c;
          viewIdOut = view.id;
        }
      }
    }
  }
  return best;
}

ProjRef RefOfCurve(const drawing::SheetCurve& c, uint64_t viewId) {
  ProjRef r;
  r.viewId = viewId;
  r.occurrenceId = c.sourceId;
  if (c.edge.IsSet())
    r.edge = c.edge;
  else
    r.silFace = c.silFace;
  return r;
}

// Dimension-placement interaction (D4): highlights the projected edge under the
// cursor, picks it on click (all refs share one view), and creates the dim once
// enough are picked. Called from DrawSheet while the tool is armed.
void HandleDimTool(ImDrawList* dl, AppState& state) {
  if (!state.dimTool.active || !state.drawing)
    return;
  ImGuiIO& io = ImGui::GetIO();
  if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    state.dimTool = AppState::DimTool{};
    return;
  }
  if (io.WantCaptureMouse)
    return; // cursor is over a panel

  const glm::dvec2 cursor = ScreenToSheet(state, io.MousePos);
  const double maxDist =
      state.sheetView.scale > 0 ? 5.0 / state.sheetView.scale : 5.0;
  uint64_t viewId = 0;
  const drawing::SheetCurve* hit = PickCurve(state, cursor, maxDist, viewId);
  // Once the first ref fixes a view, only that view's geometry is pickable.
  if (hit && state.dimTool.viewId && viewId != state.dimTool.viewId)
    hit = nullptr;

  const auto commitDim = [&] {
    DrawingDim d;
    d.kind = state.dimTool.kind;
    d.viewId = state.dimTool.viewId;
    d.refs = state.dimTool.refs;
    // Auto-place clear of the view (drag to move); the picked view is in
    // drawingRenders by construction — the user just clicked its curves.
    const auto rit = state.drawingRenders.find(d.viewId);
    d.labelOffset = rit != state.drawingRenders.end()
                        ? drawing::DefaultDimOffset(d, rit->second)
                        : glm::dvec2{0.0, -12.0};
    SnapshotDrawing(state);
    state.drawing->AddDim(d);
    state.dimTool = AppState::DimTool{}; // done — disarm
  };

  // Open-ended kinds (pattern callout): Enter commits once 2+ refs are in.
  const int needed = NeededRefs(state.dimTool.kind);
  if (needed < 0 && state.dimTool.refs.size() >= 2 &&
      ImGui::IsKeyPressed(ImGuiKey_Enter)) {
    commitDim();
    return;
  }

  if (hit) {
    const ImU32 hl = IM_COL32(80, 170, 255, 255);
    for (size_t i = 1; i < hit->points.size(); ++i)
      dl->AddLine(SheetToScreen(state, hit->points[i - 1]),
                  SheetToScreen(state, hit->points[i]), hl, 2.6f);
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        !state.sheetView.panning) {
      state.dimTool.refs.push_back(RefOfCurve(*hit, viewId));
      state.dimTool.viewId = viewId;
      if (needed > 0 && static_cast<int>(state.dimTool.refs.size()) >= needed)
        commitDim();
    }
  }
}

// A filled arrowhead at `tip` pointing along unit `dir` (sheet mm), ported from
// the sketch Arrow but in sheet space so it scales with zoom.
void SheetArrow(ImDrawList* dl, AppState& state, glm::dvec2 tip, glm::dvec2 dir,
                ImU32 col) {
  const glm::dvec2 n(-dir.y, dir.x);
  const double len = 2.2, w = 0.8; // mm
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
  ImGuiIO& io = ImGui::GetIO();
  const bool overPanel = io.WantCaptureMouse;

  for (DrawingDim& dim : state.drawing->GetDims()) {
    const drawing::ResolvedDim d = ResolveDrawingDim(state, dim);
    if (!d.resolved) {
      // Dangling: anchor the marker at the view's bbox corner (labelOffset is
      // an OFFSET — drawing it as an absolute point parked the marker at the
      // sheet origin). Fall back to the view's position, then the offset.
      if (fontPx >= 4.0f) {
        glm::dvec2 anchor = dim.labelOffset;
        const auto rit = state.drawingRenders.find(dim.viewId);
        if (rit != state.drawingRenders.end() && rit->second.hasGeometry)
          anchor = {rit->second.bboxMin.x, rit->second.bboxMax.y + 4.0};
        else if (const DrawingView* dv = state.drawing->FindView(dim.viewId))
          anchor = dv->position;
        dl->AddText(font, fontPx, SheetToScreen(state, anchor), err,
                    "dangling dim");
      }
      continue;
    }
    if (d.radial) {
      dl->AddLine(SheetToScreen(state, d.center), SheetToScreen(state, d.rim),
                  ink, 1.2f);
      SheetArrow(dl, state, d.rim, glm::normalize(d.rim - d.center), ink);
    } else if (d.leader) {
      // Callout: a leader from the circle rim to the label, arrow at the rim.
      dl->AddLine(SheetToScreen(state, d.rim), SheetToScreen(state, d.labelPos),
                  ink, 1.2f);
      const glm::dvec2 back = d.rim - d.labelPos;
      if (glm::length(back) > 1e-9)
        SheetArrow(dl, state, d.rim, glm::normalize(back), ink);
    } else if (d.kind == DrawingDimKind::Angle) {
      // v1 angle: value only (rendered below).
    } else {
      dl->AddLine(SheetToScreen(state, d.ext1), SheetToScreen(state, d.dim1),
                  ink, 0.8f);
      dl->AddLine(SheetToScreen(state, d.ext2), SheetToScreen(state, d.dim2),
                  ink, 0.8f);
      dl->AddLine(SheetToScreen(state, d.dim1), SheetToScreen(state, d.dim2),
                  ink, 1.2f);
      const double L = glm::length(d.dim2 - d.dim1);
      if (L > 1e-6) {
        const glm::dvec2 u = (d.dim2 - d.dim1) / L;
        SheetArrow(dl, state, d.dim1, u, ink);
        SheetArrow(dl, state, d.dim2, -u, ink);
      }
    }
    // Value text with a white backing box; the box is a drag handle that moves
    // the label (dim.labelOffset), the sketch labelOffset pattern.
    const ImVec2 tp = SheetToScreen(state, d.labelPos);
    const ImVec2 tsz = font->CalcTextSizeA(fontPx >= 4.0f ? fontPx : 11.0f,
                                           FLT_MAX, 0.0f, d.text.c_str());
    const ImVec2 org(tp.x - tsz.x * 0.5f, tp.y - tsz.y * 0.5f);
    if (fontPx >= 4.0f) {
      dl->AddRectFilled(ImVec2(org.x - 2, org.y - 1),
                        ImVec2(org.x + tsz.x + 2, org.y + tsz.y + 1),
                        IM_COL32(250, 250, 250, 230), 2.0f);
      dl->AddText(font, fontPx, org, ink, d.text.c_str());
    }
    // Label drag (only when the dimension tool isn't picking).
    if (!state.dimTool.active && !overPanel) {
      const bool overLabel =
          io.MousePos.x >= org.x - 2 && io.MousePos.x <= org.x + tsz.x + 2 &&
          io.MousePos.y >= org.y - 1 && io.MousePos.y <= org.y + tsz.y + 1;
      if (overLabel && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
          !state.sheetView.panning) {
        SnapshotDrawing(state);
        state.dimDrag = dim.id;
      }
      if (state.dimDrag == dim.id) {
        const double s =
            state.sheetView.scale > 0 ? state.sheetView.scale : 1.0;
        dim.labelOffset.x += io.MouseDelta.x / s;
        dim.labelOffset.y -= io.MouseDelta.y / s; // screen +Y is sheet -Y
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
          state.dimDrag = 0;
      }
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
    if (it == state.drawingRenders.end())
      continue;
    const drawing::SheetView& sv = it->second;

    for (const drawing::SheetCurve& c : sv.curves) {
      if (c.points.size() < 2)
        continue;
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
        // Drafting convention: reductions read 1:n, enlargements n:1. The
        // old label divided unconditionally, so a 20x view read "1:0.05".
        const double sc = view.scale > 0 ? view.scale : 1.0;
        char ratio[32];
        if (sc >= 1.0)
          std::snprintf(ratio, sizeof(ratio), "%g:1", sc);
        else
          std::snprintf(ratio, sizeof(ratio), "1:%g", 1.0 / sc);
        char label[128];
        std::snprintf(label, sizeof(label), "%s  %s",
                      view.name.empty() ? "view" : view.name.c_str(), ratio);
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
      const glm::dvec2 at = sv.hasGeometry
                                ? glm::dvec2(sv.bboxMin.x, sv.bboxMax.y + 12.0)
                                : view.position;
      dl->AddText(font, fontPx, SheetToScreen(state, at), stale,
                  sv.error.empty() ? "stale - update" : sv.error.c_str());
    }
  }
}

} // namespace

void HandleSheetInput(AppState& state) {
  if (!state.drawing)
    return;
  ImGuiIO& io = ImGui::GetIO();
  if (state.sheetView.scale <= 0.0)
    FitSheet(state); // first paint

  const bool overViewport = !io.WantCaptureMouse;

  // Scroll-to-cursor zoom: keep the sheet point under the cursor fixed.
  // Shift+scroll and a horizontal wheel/swipe pan instead (trackpads).
  if (overViewport && (io.MouseWheel != 0.0f || io.MouseWheelH != 0.0f)) {
    const double s = state.sheetView.scale > 0 ? state.sheetView.scale : 1.0;
    const double wheelPanPx = 30.0; // one wheel notch ≈ 30 px of drag
    state.sheetView.offset.x -= io.MouseWheelH * wheelPanPx / s;
    if (io.KeyShift) {
      state.sheetView.offset.y -= io.MouseWheel * wheelPanPx / s;
    } else if (io.MouseWheel != 0.0f) {
      const glm::dvec2 before = ScreenToSheet(state, io.MousePos);
      state.sheetView.scale *= std::pow(1.1, io.MouseWheel);
      state.sheetView.scale = std::clamp(state.sheetView.scale, 0.02, 200.0);
      const glm::dvec2 after = ScreenToSheet(state, io.MousePos);
      state.sheetView.offset += before - after;
    }
  }

  // Middle-drag pan (no orbit camera on a sheet); Alt+left-drag for trackpads
  // without a usable middle button.
  const bool altLeft =
      io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
  if (overViewport &&
      (ImGui::IsMouseClicked(ImGuiMouseButton_Middle) || altLeft))
    state.sheetView.panning = true;
  if (ImGui::IsMouseReleased(ImGuiMouseButton_Middle) ||
      ImGui::IsMouseReleased(ImGuiMouseButton_Left))
    state.sheetView.panning = false;
  if (state.sheetView.panning &&
      (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f)) {
    const double s = state.sheetView.scale > 0 ? state.sheetView.scale : 1.0;
    state.sheetView.offset.x -= io.MouseDelta.x / s;
    state.sheetView.offset.y += io.MouseDelta.y / s; // screen +Y is sheet -Y
  }
}

void DrawSheet(AppState& state) {
  if (!state.drawing)
    return;
  if (state.sheetView.scale <= 0.0)
    FitSheet(state);
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
  // Drawing border + title block from the shared chrome (the validator and
  // both exporters measure against the same rects).
  const SheetChrome chrome = ChromeOf(sheet);
  const ImVec2 mbl = SheetToScreen(state, chrome.borderMin);
  const ImVec2 mtr = SheetToScreen(state, chrome.borderMax);
  dl->AddRect(mbl, mtr, ink, 0.0f, 0, 1.5f);

  const double tbW = chrome.titleBlockMax.x - chrome.titleBlockMin.x;
  const double tbH = chrome.titleBlockMax.y - chrome.titleBlockMin.y;
  const double x0 = chrome.titleBlockMin.x, y0 = chrome.titleBlockMin.y;
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
    const double ry = y0 + tbH - (i + 1) * rowH; // top row first
    if (i > 0)
      dl->AddLine(SheetToScreen(state, {x0, ry + rowH}),
                  SheetToScreen(state, {x0 + tbW, ry + rowH}), ink, 1.0f);
    if (fontPx >= 4.0f) { // legible only past a zoom threshold
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
  HandleDimTool(dl, state);

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
  // Label LEFT and the field filling the row: ImGui's default puts the label
  // where the 210 px dock clips it, which is how "Document" became "Docume".
  LeftLabel(label);
  const std::string id = std::string("###dwg_title_") + label;
  const bool changed = ImGui::InputText(id.c_str(), buf, sizeof(buf));
  if (ImGui::IsItemActivated())
    SnapshotDrawing(state);
  if (changed)
    value = buf;
  return changed;
}

const char* kOrientLabels[] = {"front", "back",   "left", "right",
                               "top",   "bottom", "iso"};

// The view list + an "add view" control (source part + orientation + scale)
// and the explicit Update button. Split out to keep DrawSheetPanel legible.
void DrawViewsSection(AppState& state) {
  ImGui::Spacing();
  ImGui::TextUnformatted("Views");
  ImGui::Separator();

  // Count views whose source changed since their last projection, or that
  // failed to project — the "needs update" signal.
  int stale = 0, errored = 0;
  for (const DrawingView& view : state.drawing->GetViews()) {
    const auto it = state.drawingRenders.find(view.id);
    if (it == state.drawingRenders.end() ||
        DrawingSourceVersion(state, view.sourceDocId) !=
            it->second.solvedSourceVersion)
      ++stale;
    if (it != state.drawingRenders.end() && !it->second.error.empty())
      ++errored;
  }
  const bool needsUpdate = stale > 0;
  if (needsUpdate) // amber Update button when anything is out of date
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.62f, 0.42f, 0.12f, 1.0f));
  if (ImGui::Button("Update views"))
    UpdateDrawingRenders(state);
  if (needsUpdate)
    ImGui::PopStyleColor();
  ImGui::SameLine();
  if (needsUpdate)
    ImGui::TextColored(ImVec4(0.85f, 0.62f, 0.25f, 1.0f), "%d stale", stale);
  else if (errored)
    ImGui::TextColored(ImVec4(0.82f, 0.31f, 0.27f, 1.0f), "%d error", errored);
  else
    ImGui::TextDisabled("up to date");

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
    if (srcIdx >= static_cast<int>(partSlots.size()))
      srcIdx = 0;
    LeftLabel("Source");
    if (ImGui::BeginCombo("###dwg_source", partSlots[srcIdx]->name.c_str())) {
      for (int i = 0; i < static_cast<int>(partSlots.size()); ++i)
        if (ImGui::Selectable(partSlots[i]->name.c_str(), i == srcIdx))
          srcIdx = i;
      ImGui::EndCombo();
    }
    LeftLabel("Orientation");
    ImGui::Combo("###dwg_orientation", &orientIdx, kOrientLabels,
                 IM_ARRAYSIZE(kOrientLabels));
    LeftLabel("Scale");
    ImGui::InputFloat("###dwg_scale", &addScale, 0.0f, 0.0f, "%.3f");
    if (ImGui::Button("Add view###dwg_add_view")) {
      DrawingView v;
      v.name = std::string(kOrientLabels[orientIdx]);
      v.sourceDocId = partSlots[srcIdx]->id;
      v.orientation = static_cast<ViewOrientation>(orientIdx);
      // A scale typed here is respected; 0 asks for the fitted standard one.
      v.scale = addScale > 0 ? addScale : 0.0;
      AddDrawingViewPlaced(state, v);
    }
  }

  // The view list: name, orientation, curve count / status, delete.
  for (const DrawingView& view : state.drawing->GetViews()) {
    ImGui::PushID(static_cast<int>(view.id));
    const auto it = state.drawingRenders.find(view.id);
    const char* status = "needs update";
    if (it != state.drawingRenders.end()) {
      if (!it->second.error.empty())
        status = it->second.error.c_str();
      else
        status = it->second.hasGeometry ? "ok" : "empty";
    }
    ImGui::BulletText(
        "%s [%s]  %zu curves - %s",
        view.name.empty() ? "view" : view.name.c_str(),
        ViewOrientationToString(view.orientation),
        it != state.drawingRenders.end() ? it->second.curves.size() : size_t{0},
        status);
    ImGui::SameLine();
    if (ImGui::SmallButton("x")) {
      SnapshotDrawing(state);
      state.drawing->RemoveView(view.id);
      UpdateDrawingRenders(state);
      ImGui::PopID();
      break; // container mutated; redraw next frame
    }
    ImGui::PopID();
  }
}

// The dimension tools + dimension list. Arming a kind puts the sheet into
// pick mode (HandleDimTool); the created dims are re-measured on every update.
void DrawDimsSection(AppState& state) {
  ImGui::Spacing();
  ImGui::TextUnformatted("Dimensions");
  ImGui::Separator();

  if (state.dimTool.active) {
    // The in-progress session (refs list, cancel) lives in the Tool window.
    ImGui::TextDisabled("Picking dimension refs…");
  } else {
    struct Tool {
      const char* label;
      DrawingDimKind kind;
    };
    static const Tool tools[] = {{"Distance", DrawingDimKind::Distance},
                                 {"Dist X", DrawingDimKind::DistanceX},
                                 {"Dist Y", DrawingDimKind::DistanceY},
                                 {"Radius", DrawingDimKind::Radius},
                                 {"Diameter", DrawingDimKind::Diameter},
                                 {"Angle", DrawingDimKind::Angle},
                                 {"Hole note", DrawingDimKind::HoleCallout},
                                 {"Pattern", DrawingDimKind::PatternCallout}};
    for (size_t i = 0; i < std::size(tools); ++i) {
      if (i % 3 != 0)
        ImGui::SameLine();
      if (ImGui::Button(tools[i].label)) {
        state.dimTool = AppState::DimTool{};
        state.dimTool.active = true;
        state.dimTool.kind = tools[i].kind;
      }
    }
  }

  for (const DrawingDim& dim : state.drawing->GetDims()) {
    ImGui::PushID(static_cast<int>(dim.id) + 100000);
    const drawing::ResolvedDim rd = ResolveDrawingDim(state, dim);
    if (rd.resolved)
      ImGui::BulletText("%s = %s", DrawingDimKindToString(dim.kind),
                        rd.text.c_str());
    else
      ImGui::TextColored(ImVec4(0.82f, 0.31f, 0.27f, 1.0f), "  %s (dangling)",
                         DrawingDimKindToString(dim.kind));
    ImGui::SameLine();
    if (ImGui::SmallButton("x")) {
      SnapshotDrawing(state);
      state.drawing->RemoveDim(dim.id);
      ImGui::PopID();
      break;
    }
    ImGui::PopID();
  }
}

} // namespace

void DrawSheetPanel(AppState& state) {
  if (!state.drawing)
    return;
  // Docks into the left "Model" node (the DockLayout slot the feature tree and
  // occurrence panel also use), so a drawing tab reserves the same panel space
  // instead of floating over the sheet.
  ImGui::Begin(kPanelModel);

  Sheet& sheet = state.drawing->GetSheet();

  ImGui::TextUnformatted("Sheet");
  ImGui::Separator();

  int sizeIdx = static_cast<int>(sheet.size);
  LeftLabel("Sheet size");
  if (ImGui::Combo("###dwg_sheet_size", &sizeIdx, kSizeLabels,
                   IM_ARRAYSIZE(kSizeLabels))) {
    SnapshotDrawing(state);
    sheet.size = static_cast<SheetSize>(sizeIdx);
    state.drawing->BumpVersion();
    state.sheetView.scale = 0.0; // refit for the new paper size
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
  if (edited)
    state.drawing->BumpVersion();

  ImGui::Spacing();
  if (ImGui::Button("Fit"))
    state.sheetView.scale = 0.0;
  ImGui::SameLine();
  if (ImGui::Button("Undo"))
    UndoDrawing(state);
  ImGui::SameLine();
  if (ImGui::Button("Redo"))
    RedoDrawing(state);

  DrawViewsSection(state);
  DrawDimsSection(state);

  ImGui::End();
}

} // namespace ui
