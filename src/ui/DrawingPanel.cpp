#include "ui/DrawingPanel.h"

#include "core/drawing/DrawingExport.h" // TitleBlockRows
#include "core/io/Format.h"
#include "ui/DockLayout.h" // PanelHeading
#include "ui/DocumentSlots.h"
#include "ui/DrawingActions.h"
#include "ui/ExprInput.h" // expression-capable numeric fields
#include "ui/PanelLayout.h"
#include "ui/PanelTitles.h"
#include "ui/PreviewFeature.h" // BeginToolWindowFrame
#include "ui/Widgets.h"        // ListRow

#include "IconsFontAwesome6.h"

#include "imgui.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <utility>

namespace ui {

namespace {

// Drafting convention: reductions read 1:n, enlargements n:1. (An old label
// divided unconditionally, so a 20x view read "1:0.05".)
std::string ScaleText(double scale) {
  const double sc = scale > 0 ? scale : 1.0;
  char ratio[32];
  if (sc >= 1.0)
    std::snprintf(ratio, sizeof(ratio), "%g:1", sc);
  else
    std::snprintf(ratio, sizeof(ratio), "1:%g", 1.0 / sc);
  return ratio;
}

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

// A dashed line, in sheet-mm units so the pattern is stable under zoom (the
// design's CPU-dashing, in sheet space). The pattern comes from
// drawing::StyleOf, the same table the exporters draw from.
void StrokeDashed(ImDrawList* dl, AppState& state,
                  const std::vector<glm::dvec2>& pts, ImU32 col, float px,
                  std::span<const double> pattern) {
  for (size_t i = 1; i < pts.size(); ++i) {
    const glm::dvec2 a = pts[i - 1], b = pts[i];
    const double len = glm::length(b - a);
    if (len < 1e-9)
      continue;
    const glm::dvec2 dir = (b - a) / len;
    double t = 0.0;
    for (bool draw = true; t < len;) {
      for (double step : pattern) {
        const double t2 = std::min(t + step, len);
        if (draw)
          dl->AddLine(SheetToScreen(state, a + dir * t),
                      SheetToScreen(state, a + dir * t2), col, px);
        draw = !draw;
        t = t2;
        if (t >= len)
          break;
      }
    }
  }
}

// How many refs a dimension kind needs before it can be created.
// Refs a dim kind needs before it commits; -1 = open-ended (2+ picks,
// Enter finishes — the pattern callout's pick-a-row flow).
int NeededRefs(DrawingDimKind k) {
  if (k == DrawingDimKind::Radius || k == DrawingDimKind::Diameter ||
      k == DrawingDimKind::HoleCallout || k == DrawingDimKind::FastenerCallout)
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
    // Auto-place clear of the views, title block and existing labels (drag
    // to move); the picked view is in drawingRenders by construction — the
    // user just clicked its curves.
    d.labelOffset = AutoDimOffset(state, d);
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

// Text turned counter-clockwise on the SHEET by `angle` about `pivot` (a
// screen point): set flat, then its vertices turned. ImGui sets no turned
// text of its own, and the sheet's turn is clockwise on a Y-down screen.
void AddTurnedText(ImDrawList* dl, ImFont* font, float px, ImVec2 topLeft,
                   ImVec2 pivot, double angle, ImU32 col, const char* text) {
  const int first = dl->VtxBuffer.Size;
  dl->AddText(font, px, topLeft, col, text);
  if (angle == 0.0)
    return;
  const float c = static_cast<float>(std::cos(-angle));
  const float s = static_cast<float>(std::sin(-angle));
  for (int i = first; i < dl->VtxBuffer.Size; ++i) {
    ImVec2& v = dl->VtxBuffer[i].pos;
    const float dx = v.x - pivot.x, dy = v.y - pivot.y;
    v = ImVec2(pivot.x + dx * c - dy * s, pivot.y + dx * s + dy * c);
  }
}

// Sheet furniture (drawing::SheetGraphics) on screen: the primitives the
// exporters draw, so a balloon or a hatch looks here as it prints.
void DrawSheetGraphics(ImDrawList* dl, AppState& state,
                       const drawing::SheetGraphics& g, ImU32 col) {
  const float pxPerMM = static_cast<float>(state.sheetView.scale);
  for (const auto& f : g.knockouts) {
    std::vector<ImVec2> pts;
    for (const glm::dvec2& p : f)
      pts.push_back(SheetToScreen(state, p));
    if (pts.size() >= 3)
      dl->AddConvexPolyFilled(pts.data(), static_cast<int>(pts.size()),
                              IM_COL32(250, 250, 250, 255));
  }
  for (const auto& l : g.lines) {
    const float px = std::max(1.0f, static_cast<float>(l.widthMM) * pxPerMM);
    if (l.chain)
      StrokeDashed(dl, state, {l.a, l.b}, col, px, drawing::kBendDashMM);
    else if (l.dashed)
      StrokeDashed(dl, state, {l.a, l.b}, col, px, drawing::kHiddenDashMM);
    else
      dl->AddLine(SheetToScreen(state, l.a), SheetToScreen(state, l.b), col,
                  px);
  }
  for (const auto& f : g.fills) {
    std::vector<ImVec2> pts;
    for (const glm::dvec2& p : f)
      pts.push_back(SheetToScreen(state, p));
    if (pts.size() >= 3)
      dl->AddConvexPolyFilled(pts.data(), static_cast<int>(pts.size()), col);
  }
  ImFont* font = ImGui::GetFont();
  for (const auto& t : g.texts) {
    const float fontPx = static_cast<float>(t.heightMM * 0.85) * pxPerMM;
    if (fontPx < 4.0f)
      continue; // illegible at this zoom
    const ImVec2 size =
        font->CalcTextSizeA(fontPx, FLT_MAX, 0.0f, t.text.c_str());
    // pos is the baseline; ImGui places text by its top edge.
    const ImVec2 pivot = SheetToScreen(state, t.pos);
    ImVec2 at = pivot;
    at.y -= size.y * 0.8f;
    if (t.center)
      at.x -= size.x * 0.5f;
    AddTurnedText(dl, font, fontPx, at, pivot, t.angleRad, col, t.text.c_str());
  }
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
      const glm::dvec2 end = drawing::LeaderEnd(d.rim, d.labelPos, d.text);
      if (glm::length(end - d.rim) > 1.0) // on to a value off the circle
        dl->AddLine(SheetToScreen(state, d.rim), SheetToScreen(state, end), ink,
                    1.2f);
    } else if (d.leader) {
      // Callout: a leader from the circle rim to the label, arrow at the rim.
      dl->AddLine(
          SheetToScreen(state, d.rim),
          SheetToScreen(state, drawing::LeaderEnd(d.rim, d.labelPos, d.text)),
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
    ImVec2 tsz = font->CalcTextSizeA(fontPx >= 4.0f ? fontPx : 11.0f, FLT_MAX,
                                     0.0f, d.text.c_str());
    ImVec2 org(tp.x - tsz.x * 0.5f, tp.y - tsz.y * 0.5f);
    if (d.aligned) {
      // Written along its line, as it prints: the exporters' description
      // of it, and its turned box as the drag handle.
      drawing::SheetGraphics g;
      const auto q = drawing::DimTextQuad(d);
      g.knockouts.push_back({q.begin(), q.end()});
      const glm::dvec2 up(-std::sin(d.textAngle), std::cos(d.textAngle));
      g.texts.push_back(
          {d.labelPos - up * 1.25, d.text, true, 3.5, d.textAngle});
      if (fontPx >= 4.0f)
        DrawSheetGraphics(dl, state, g, ink);
      glm::dvec2 mn, mx;
      drawing::DimLabelBox(d, mn, mx);
      const ImVec2 a = SheetToScreen(state, mn), b = SheetToScreen(state, mx);
      org = ImVec2(std::min(a.x, b.x) + 2.0f, std::min(a.y, b.y) + 1.0f);
      tsz = ImVec2(std::abs(b.x - a.x) - 4.0f, std::abs(b.y - a.y) - 2.0f);
    } else if (fontPx >= 4.0f) {
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
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
          state.dimDrag = 0;
          // The offset is saved with the drawing: without the bump the drag
          // was an undo step and never an unsaved change.
          state.drawing->BumpVersion();
        }
      }
    }
  }

  // Notes (v66): text + optional leader, same resolution as the exporters.
  for (const DrawingNote& note : state.drawing->GetNotes()) {
    const drawing::ResolvedNote rn = ResolveDrawingNote(state, note);
    const drawing::SheetGraphics g = drawing::NoteGraphics(rn);
    if (!g.empty()) { // a balloon, a datum, a table
      DrawSheetGraphics(dl, state, g, ink);
      continue;
    }
    if (rn.hasLeader) {
      dl->AddLine(
          SheetToScreen(state, rn.anchor),
          SheetToScreen(state, drawing::LeaderEnd(rn.anchor, rn.pos, rn.text)),
          ink, 1.2f);
      const glm::dvec2 back = rn.anchor - rn.pos;
      if (glm::length(back) > 1e-9)
        SheetArrow(dl, state, rn.anchor, glm::normalize(back), ink);
    }
    if (fontPx >= 4.0f) {
      ImVec2 org = SheetToScreen(state, rn.pos);
      const ImVec2 tsz =
          font->CalcTextSizeA(fontPx, FLT_MAX, 0.0f, rn.text.c_str());
      org.x -= tsz.x * 0.5f;
      org.y -= tsz.y * 0.5f;
      dl->AddRectFilled(ImVec2(org.x - 2, org.y - 1),
                        ImVec2(org.x + tsz.x + 2, org.y + tsz.y + 1),
                        IM_COL32(250, 250, 250, 230), 2.0f);
      dl->AddText(font, fontPx, org, ink, rn.text.c_str());
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
      const drawing::LineStyle style = drawing::StyleOf(c);
      if (style.dash.empty())
        StrokePolyline(dl, state, c.points, visible, 1.6f);
      else
        StrokeDashed(dl, state, c.points,
                     c.kind == drawing::CurveKind::Bend ? visible : hidden,
                     1.0f, style.dash);
    }
    // Centre marks on visible circles (v66) — same rule as the exporters.
    for (const drawing::SheetCurve& c : sv.curves) {
      if (!drawing::HasCentreMark(c))
        continue;
      const double leg = std::min(2.5, std::max(1.0, c.conicRadius * 0.35));
      dl->AddLine(
          SheetToScreen(state, {c.conicCenter.x - leg, c.conicCenter.y}),
          SheetToScreen(state, {c.conicCenter.x + leg, c.conicCenter.y}),
          visible, 1.0f);
      dl->AddLine(
          SheetToScreen(state, {c.conicCenter.x, c.conicCenter.y - leg}),
          SheetToScreen(state, {c.conicCenter.x, c.conicCenter.y + leg}),
          visible, 1.0f);
    }
    DrawSheetGraphics(dl, state, sv.extras, visible); // hatching, sections

    if (sv.hasGeometry) {
      // A light frame + the view name/scale label above the geometry.
      const glm::dvec2 pad{2.0, 2.0};
      dl->AddRect(SheetToScreen(state, sv.bboxMin - pad),
                  SheetToScreen(state, sv.bboxMax + pad), frame, 0.0f, 0, 1.0f);
      if (fontPx >= 4.0f) {
        // Drafting convention: reductions read 1:n, enlargements n:1. The
        // old label divided unconditionally, so a 20x view read "1:0.05".
        const std::string ratio = ScaleText(view.scale);
        char label[128];
        std::snprintf(label, sizeof(label), "%s  %s",
                      view.name.empty() ? "view" : view.name.c_str(),
                      ratio.c_str());
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
  // The same rows the exporters write (DrawingExport::TitleBlockRows), with
  // the blanks filled the way an export fills them — the sheet on screen and
  // the sheet sent to a shop have to agree about what its bare numbers are
  // in, and what it is made of.
  const std::vector<DrawingExport::TitleRow> fields =
      DrawingExport::TitleBlockRows(DrawingSheetForOutput(state),
                                    ActiveDrawingUnits(state));
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
                  fields[i].label);
      const ImVec2 valPos = SheetToScreen(state, {x0 + 42.0, ry + rowH - 3.0});
      dl->AddText(font, fontPx, valPos, ink, fields[i].value.c_str());
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

// A bend table for `view`, placed beside it: a line's height clear of the
// view, on the side that has room, and inside the border either way -- a table
// placed off the sheet is a table nobody can read.
void AddBendTable(AppState& state, const DrawingView& view,
                  const drawing::SheetView& render) {
  DrawingNote n;
  n.table = NoteTable::Bends;
  n.viewId = view.id;
  // Sized by its own text: a line's height clear of the view, on the
  // side of it that has room, and inside the border either way — a
  // table placed off the sheet is a table nobody can read.
  const drawing::ResolvedNote rn = ResolveDrawingNote(state, n);
  glm::dvec2 mn, mx;
  drawing::LabelBox(n.pos, rn.text, mn, mx);
  const glm::dvec2 size = mx - mn;
  const SheetChrome chrome = ChromeOf(state.drawing->GetSheet());
  const double gap = drawing::kLabelLineMM;
  double left = render.bboxMax.x + gap;
  if (left + size.x > chrome.borderMax.x)
    left = render.bboxMin.x - gap - size.x; // the other side
  left = std::clamp(left, chrome.borderMin.x,
                    std::max(chrome.borderMin.x, chrome.borderMax.x - size.x));
  double top = render.bboxMax.y;
  top = std::clamp(top, chrome.borderMin.y + size.y, chrome.borderMax.y);
  // ...and not on top of the title block, which owns its corner.
  if (left + size.x > chrome.titleBlockMin.x &&
      top - size.y < chrome.titleBlockMax.y)
    top = std::min(chrome.borderMax.y, chrome.titleBlockMax.y + size.y + gap);
  n.pos += glm::dvec2(left, top) - glm::dvec2(mn.x, mx.y);
  SnapshotDrawing(state);
  state.drawing->AddNote(std::move(n));
}

// Does `view` show a part with bends, so a bend table means something?
bool ViewHasBends(AppState& state, const DrawingView& view) {
  const Document* viewDoc = DrawingViewDocument(state, view);
  if (viewDoc == nullptr)
    return false;
  for (const Body& b : viewDoc->GetBodies())
    if (!b.reference && !b.bends.empty())
      return true;
  return false;
}

// The views, as rows in the feature list's style (ui::ListRow,
// docs/ui/panels.md#drawing-tab-the-drawing-panel): a glyph, the name, the
// orientation and scale as the tail, red when it failed to project. The row's
// menu sets the scale, adds a bend table and deletes it. Adding a view is the
// toolbar's (the Add view tool window), and so is Update views; this says
// whether one is needed.
void DrawViewsSection(AppState& state) {
  ImGui::SeparatorText("Views");

  // Count views whose source changed since their last projection, or that
  // failed to project -- the "needs update" signal.
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
  if (state.drawing->GetViews().empty())
    ImGui::TextDisabled("No views. Add one with the toolbar's Add view.");
  else if (stale > 0)
    ImGui::TextColored(ImVec4(0.85f, 0.62f, 0.25f, 1.0f),
                       "%d stale: Update views on the toolbar", stale);
  else if (errored)
    ImGui::TextColored(ImVec4(0.82f, 0.31f, 0.27f, 1.0f), "%d failed", errored);

  static const double kScales[] = {10.0, 5.0, 2.0,  1.0,  0.5,
                                   0.2,  0.1, 0.05, 0.02, 0.01};
  uint64_t removeView = 0;
  for (const DrawingView& view : state.drawing->GetViews()) {
    const auto it = state.drawingRenders.find(view.id);
    const bool rendered = it != state.drawingRenders.end();
    const bool failed = rendered && !it->second.error.empty();
    ListRowSpec row;
    row.glyph = ICON_FA_IMAGE;
    if (failed)
      row.glyphColor = ImVec4(0.82f, 0.31f, 0.27f, 1.0f);
    row.label = (view.name.empty() ? std::string("view") : view.name) +
                "###dwg_view_" + std::to_string(view.id);
    char tail[80];
    std::snprintf(tail, sizeof(tail), "%s  %s",
                  ViewOrientationToString(view.orientation),
                  ScaleText(view.scale).c_str());
    if (view.rotationDeg != 0.0)
      std::snprintf(tail + std::strlen(tail), sizeof(tail) - std::strlen(tail),
                    "  %g°", view.rotationDeg);
    row.tail = tail;
    row.tooltip = !rendered ? std::string("Needs an update.")
                  : failed
                      ? it->second.error
                      : std::to_string(it->second.curves.size()) + " curves" +
                            (it->second.hasGeometry ? "" : ", empty");
    row.menuId = "###dwg_view_ctx_" + std::to_string(view.id);
    const uint64_t viewId = view.id;
    const bool canBendTable =
        rendered && it->second.hasGeometry && ViewHasBends(state, view);
    row.menu = [&state, &removeView, viewId, canBendTable] {
      if (ImGui::BeginMenu("Scale###dwg_view_scale")) {
        DrawingView* v = state.drawing->FindView(viewId);
        for (const double scale : kScales) {
          const std::string label =
              ScaleText(scale) + "###dwg_scale_" + std::to_string(scale);
          if (ImGui::MenuItem(label.c_str(), nullptr,
                              v && std::abs(v->scale - scale) < 1e-12) &&
              v) {
            SnapshotDrawing(state);
            v->scale = scale;
            state.drawing->BumpVersion();
            UpdateDrawingRenders(state);
          }
        }
        ImGui::EndMenu();
      }
      // Turned on the sheet: a part modelled along X draws upright at 90.
      if (ImGui::BeginMenu("Rotate###dwg_view_rotate")) {
        DrawingView* v = state.drawing->FindView(viewId);
        for (const double deg : {0.0, 90.0, 180.0, 270.0}) {
          const std::string label = std::to_string(int(deg)) +
                                    "°###dwg_rotate_" +
                                    std::to_string(int(deg));
          if (ImGui::MenuItem(label.c_str(), nullptr,
                              v && std::abs(v->rotationDeg - deg) < 1e-9) &&
              v) {
            SnapshotDrawing(state);
            v->rotationDeg = deg;
            state.drawing->BumpVersion();
            UpdateDrawingRenders(state);
          }
        }
        ImGui::EndMenu();
      }
      if (canBendTable && ImGui::MenuItem("Add bend table###dwg_bend_table")) {
        const DrawingView* v = state.drawing->FindView(viewId);
        const auto r = state.drawingRenders.find(viewId);
        if (v && r != state.drawingRenders.end())
          AddBendTable(state, *v, r->second);
      }
      ImGui::Separator();
      if (ImGui::MenuItem(ICON_FA_TRASH_CAN " Delete###dwg_view_delete"))
        removeView = viewId;
    };
    ListRow(row);
  }
  if (removeView != 0) {
    SnapshotDrawing(state);
    state.drawing->RemoveView(removeView);
    UpdateDrawingRenders(state);
  }
}

// The dimensions, as rows: the kind and its value, red when a reference no
// longer resolves, and Delete in the row's menu. The dimension tools are the
// toolbar's (they were here as well, with Part note only here).
void DrawDimsSection(AppState& state) {
  ImGui::SeparatorText("Dimensions");
  if (state.dimTool.active)
    ImGui::TextDisabled("Picking dimension refs…"); // the session: Tool window
  else if (state.drawing->GetDims().empty())
    ImGui::TextDisabled("No dimensions. The toolbar's dimension tools add "
                        "them.");

  uint64_t removeDim = 0;
  for (const DrawingDim& dim : state.drawing->GetDims()) {
    const drawing::ResolvedDim rd = ResolveDrawingDim(state, dim);
    ListRowSpec row;
    row.glyph = ICON_FA_RULER_COMBINED;
    row.label = std::string(DrawingDimKindToString(dim.kind)) + "###dwg_dim_" +
                std::to_string(dim.id);
    if (rd.resolved) {
      row.tail = rd.text;
    } else {
      row.glyphColor = ImVec4(0.82f, 0.31f, 0.27f, 1.0f);
      row.tail = "dangling";
      row.tooltip = "A reference no longer resolves: the edge or face it "
                    "measured is gone from the view.";
    }
    row.menuId = "###dwg_dim_ctx_" + std::to_string(dim.id);
    const uint64_t dimId = dim.id;
    row.menu = [&removeDim, dimId] {
      if (ImGui::MenuItem(ICON_FA_TRASH_CAN " Delete###dwg_dim_delete"))
        removeDim = dimId;
    };
    ListRow(row);
  }
  if (removeDim != 0) {
    SnapshotDrawing(state);
    state.drawing->RemoveDim(removeDim);
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
  PanelHeading(kPanelModel);

  Sheet& sheet = state.drawing->GetSheet();

  ImGui::SeparatorText("Sheet");

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

  ImGui::SeparatorText("Title block");
  bool edited = false;
  edited |= TitleField(state, "Project", sheet.titleBlock.project);
  edited |= TitleField(state, "Document", sheet.titleBlock.document);
  edited |= TitleField(state, "Author", sheet.titleBlock.author);
  edited |= TitleField(state, "Date", sheet.titleBlock.date);
  if (edited)
    state.drawing->BumpVersion();

  DrawViewsSection(state);
  DrawDimsSection(state);

  ImGui::End();
}

void DrawAddViewToolWindow(AppState& state) {
  if (!state.drawing) {
    state.drawingAddViewOpen = false;
    return;
  }
  // The one way to add a view by hand
  // (docs/ui/panels.md#drawing-tab-the-drawing-panel): what the toolbar's popup
  // offered (a source and four orientations) and what the panel's form offered
  // (all seven, a configuration and a scale), in the tool window the toolbar's
  // Add view opens.
  BeginToolWindowFrame(state, "Add view");
  static int srcIdx = 0, orientIdx = 0;
  static double addScale = 1.0;
  static uint64_t addConfig = 0; // 0 = the part's active configuration
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
    // A part with configurations can be drawn in any of them — the Flat
    // blank of a sheet-metal part beside its folded views.
    const Document* srcDoc =
        DrawingSourceDocument(state, partSlots[srcIdx]->id);
    // Configuration ids are per document, so the id chosen for one part can
    // name a DIFFERENT configuration in the next one. Forget it when the
    // source changes rather than carrying a number across.
    static uint64_t addConfigFor = 0;
    if (addConfigFor != partSlots[srcIdx]->id) {
      addConfigFor = partSlots[srcIdx]->id;
      addConfig = 0;
    }
    if (srcDoc && srcDoc->FindConfiguration(addConfig) == nullptr)
      addConfig = 0;
    if (srcDoc && !srcDoc->GetConfigurations().empty()) {
      const Configuration* chosen = srcDoc->FindConfiguration(addConfig);
      LeftLabel("Configuration");
      if (ImGui::BeginCombo("###dwg_configuration",
                            chosen ? chosen->name.c_str() : "Active")) {
        if (ImGui::Selectable("Active", addConfig == 0))
          addConfig = 0;
        // Named by id as well: two configurations may share a name, and a
        // string id is one a GUI test can address.
        for (const Configuration& c : srcDoc->GetConfigurations()) {
          const std::string label = c.name + "##cfg" + std::to_string(c.id);
          if (ImGui::Selectable(label.c_str(), c.id == addConfig))
            addConfig = c.id;
        }
        ImGui::EndCombo();
      }
    } else {
      addConfig = 0;
    }
    LeftLabel("Orientation");
    ImGui::Combo("###dwg_orientation", &orientIdx, kOrientLabels,
                 IM_ARRAYSIZE(kOrientLabels));
    LeftLabel("Scale");
    ExprInputDouble(state, "###dwg_scale", &addScale, "%.3f");
    if (ImGui::Button("Add view###dwg_add_view")) {
      DrawingView v;
      v.name = std::string(kOrientLabels[orientIdx]);
      v.sourceDocId = partSlots[srcIdx]->id;
      v.orientation = static_cast<ViewOrientation>(orientIdx);
      v.configuration = srcDoc ? addConfig : 0;
      if (const Configuration* c =
              srcDoc ? srcDoc->FindConfiguration(addConfig) : nullptr)
        v.name += " (" + c->name + ")";
      // A scale typed here is respected; 0 asks for the fitted standard one.
      v.scale = addScale > 0 ? addScale : 0.0;
      AddDrawingViewPlaced(state, v);
    }
  }

  ImGui::Separator();
  // Close: each Add view is applied as it is pressed.
  if (ImGui::Button("Close###dwg_add_view_close"))
    state.drawingAddViewOpen = false;
  ImGui::End();
}

} // namespace ui
