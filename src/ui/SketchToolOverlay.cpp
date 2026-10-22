// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/Overlays.h"
#include "ui/SelectionPipeline.h"
#include "ui/SketchInteraction.h"

#include "cad/FaceSelector.h"
#include "common/Logger.h"
#include "core/Expression.h"
#include "core/features/DatumPlaneFeature.h"
#include "core/features/SketchFeature.h"
#include "core/io/Serialization.h"
#include "core/sketch/SketchInference.h"
#include "core/sketch/SketchProfile.h"
#include "core/sketch/SketchSelection.h"
#include "core/sketch/SketchSolver.h"
#include "core/sketch/SketchText.h"
#include "render/Camera.h"
#include "render/MeshRenderer.h" // LineVertex3D
#include "ui/DocumentSlots.h"
#include "ui/ExprComplete.h" // variable completion in the dim editors
#include "ui/SceneOverlays.h"
#include "ui/SketchOverlay.h" // SelectFeature
#include "ui/SketchToolbar.h" // BeginSketchExtrude/Revolve (Shift+E/R)

#include "imgui.h"
#include "imgui_internal.h" // GetInputTextState (re-read after a completion)

#include <algorithm>
#include <cstdlib>
#include <map>
#include <optional>
#include <vector>

#include "ui/SketchInteractionInternal.h"

// What the sketch session LOOKS like while a tool is armed: the rubber-band
// preview, the selection box, the snap glyphs, and the prompt pill that says
// what the tool expects next.
//
// Distinct from SketchOverlay.cpp, which turns the finished sketch into
// vertices: this is the transient half, and most of it draws through ImGui.
// Split out of SketchInteraction.cpp
// (docs/architecture.md#the-ui-actions-layer). The 843-line viewport router
// stays there; these are the blocks it calls into, and
// SketchInteractionInternal.h is that interface written down.

namespace ui {

void BuildSketchOverlay(AppState& state, std::vector<LineVertex3D>& out) {
  SketchEditState& edit = state.sketchEdit;
  if (!edit.IsEditing())
    return;
  size_t index = 0;
  SketchFeature* sketch = ResolveActiveSketch(state, &index);
  if (!sketch)
    return;

  // Mirror preview: in the axis-pick step, ghost each selected source reflected
  // across the hovered line so the result is visible before committing.
  if (edit.tool == SketchTool::Mirror && edit.mirrorAxisPhase &&
      edit.hasHover) {
    const size_t ax = sketch->IndexOfEntity(edit.hoveredRef.entity);
    if (ax != SIZE_MAX) {
      if (auto* axl = std::get_if<sketch::Line>(&sketch->entities[ax])) {
        for (const auto& r : edit.selected) {
          if (r.entity == edit.hoveredRef.entity)
            continue;
          const size_t si = sketch->IndexOfEntity(r.entity);
          if (si == SIZE_MAX)
            continue;
          const sketch::Entity refl =
              sketch::ReflectEntity(sketch->entities[si], axl->a, axl->b);
          std::vector<glm::dvec2> poly = sketch::TessellateEntity(refl);
          for (size_t i = 0; i + 1 < poly.size(); i++) {
            out.push_back(LineVertex(sketch->plane.PlaneToWorld(poly[i])));
            out.push_back(LineVertex(sketch->plane.PlaneToWorld(poly[i + 1])));
          }
        }
      }
    }
    return; // no pending-point preview while picking a mirror axis
  }

  // Unique selected entity ids (used by the pattern/offset previews below).
  auto uniqueSources = [&]() {
    std::vector<uint32_t> ids;
    for (const auto& r : edit.selected) {
      bool seen = false;
      for (uint32_t s : ids)
        seen |= (s == r.entity);
      if (!seen)
        ids.push_back(r.entity);
    }
    return ids;
  };
  auto emitPoly = [&](const std::vector<glm::dvec2>& poly) {
    for (size_t i = 0; i + 1 < poly.size(); i++) {
      out.push_back(LineVertex(sketch->plane.PlaneToWorld(poly[i])));
      out.push_back(LineVertex(sketch->plane.PlaneToWorld(poly[i + 1])));
    }
  };

  // Pattern preview: ghost each generated instance for the current params.
  if (edit.tool == SketchTool::Pattern && !edit.selected.empty()) {
    // Ghost every instance; the linear grid previews BOTH directions (v50).
    const int rows = edit.patternCircular ? 1 : std::max(1, edit.patternCount2);
    const int total =
        edit.patternCircular ? edit.patternCount : edit.patternCount * rows;
    for (uint32_t sid : uniqueSources()) {
      const size_t si = sketch->IndexOfEntity(sid);
      if (si == SIZE_MAX)
        continue;
      for (int k = 1; k < total; k++) {
        sketch::Entity inst;
        if (edit.patternCircular) {
          inst = sketch::RotateEntity(sketch->entities[si], edit.patternCenter,
                                      glm::radians(edit.patternAngleDeg) * k);
        } else {
          const int col = k % edit.patternCount;
          const int row = k / edit.patternCount;
          inst = sketch::TranslateEntity(
              sketch->entities[si],
              static_cast<double>(col) * edit.patternStep +
                  static_cast<double>(row) * edit.patternStep2);
        }
        emitPoly(sketch::TessellateEntity(inst));
      }
    }
    return;
  }

  // Offset preview: ghost the offset of the selected closed loop.
  if (edit.tool == SketchTool::Offset && !edit.selected.empty()) {
    std::vector<sketch::Entity> chain;
    for (uint32_t sid : uniqueSources()) {
      const size_t si = sketch->IndexOfEntity(sid);
      if (si != SIZE_MAX)
        chain.push_back(sketch->entities[si]);
    }
    for (const auto& e : sketch::OffsetLoop(
             sketch->plane, chain, edit.offsetDistance, edit.offsetJoinArc))
      emitPoly(sketch::TessellateEntity(e));
    return;
  }

  // Spline preview: the curve through the placed poles plus the cursor (the
  // pole the next click would add), and the straight control polygon.
  if (edit.tool == SketchTool::Spline && !edit.splinePoles.empty()) {
    sketch::BSpline bs;
    bs.poles = edit.splinePoles;
    bs.poles.push_back(edit.hoverPoint);
    if (bs.poles.size() >= 2)
      emitPoly(sketch::BSplineSampleUV(bs));
    emitPoly(bs.poles); // control polygon
    return;
  }

  // Text: the label a click would place, ghosted at the cursor.
  if (edit.tool == SketchTool::Text && edit.textHeight > 0.0) {
    sketch::Text t;
    t.anchor = edit.hoverPoint;
    t.text = edit.textString;
    const std::vector<std::string>& fonts = sketch::TextFontNames();
    t.font = fonts[static_cast<size_t>(
        std::clamp(edit.textFont, 0, static_cast<int>(fonts.size()) - 1))];
    t.height = edit.textHeight;
    t.halign =
        static_cast<sketch::TextHAlign>(std::clamp(edit.textHAlign, 0, 2));
    t.valign =
        static_cast<sketch::TextVAlign>(std::clamp(edit.textVAlign, 0, 3));
    for (const auto& poly : sketch::TessellateEntityPolylines(t))
      for (size_t i = 0; i + 1 < poly.size(); i++) {
        out.push_back(LineVertex(sketch->plane.PlaneToWorld(poly[i])));
        out.push_back(LineVertex(sketch->plane.PlaneToWorld(poly[i + 1])));
      }
    return;
  }

  if (!edit.hasPendingPoint)
    return;

  const glm::dvec2 a = edit.pendingPoint;
  const glm::dvec2 b = edit.hoverPoint;
  std::vector<glm::dvec2> poly;
  switch (edit.tool) {
  case SketchTool::Line:
    poly = sketch::TessellateEntity(sketch::Line{a, b});
    break;
  case SketchTool::Circle:
    poly = sketch::TessellateEntity(sketch::Circle{a, glm::length(b - a)});
    break;
  case SketchTool::Rectangle:
    // Rectangles aren't an entity anymore; draw the outline directly.
    poly = {a, {b.x, a.y}, b, {a.x, b.y}, a};
    break;
  case SketchTool::RectangleCenter: {
    // Centre placed: ghost the rectangle mirrored about it, cursor = corner.
    const glm::dvec2 c1 = 2.0 * a - b;
    poly = {c1, {b.x, c1.y}, b, {c1.x, b.y}, c1};
    break;
  }
  case SketchTool::Arc:
    if (!edit.hasPendingPoint2) // center placed: preview the radius ring
      poly = sketch::TessellateEntity(sketch::Circle{a, glm::length(b - a)});
    else // center + start placed: live arc to the cursor
      poly = sketch::TessellateEntity(
          sketch::ArcCenterStartEnd(a, edit.pendingPoint2, b));
    break;
  case SketchTool::ArcThreePoint:
    if (!edit.hasPendingPoint2) // start placed: preview the chord
      poly = {a, b};
    else if (auto arc = sketch::ArcFromThreePoints(a, b, edit.pendingPoint2))
      poly = sketch::TessellateEntity(*arc);
    else
      poly = {a, edit.pendingPoint2}; // collinear: fall back to the chord
    break;
  case SketchTool::Ellipse:
    if (!edit.hasPendingPoint2) // centre placed: preview the major axis
      poly = {a, b};
    else // major axis placed: live ellipse to the cursor's minor radius
      poly = sketch::TessellateEntity(
          sketch::EllipseFromClicks(a, edit.pendingPoint2, b));
    break;
  case SketchTool::EllipseArc:
    if (!edit.hasPendingPoint2) {
      poly = {a, b}; // centre placed: preview the major axis
    } else if (!edit.hasPendingPoint3) {
      poly = sketch::TessellateEntity( // major placed: live ellipse (minor)
          sketch::EllipseFromClicks(a, edit.pendingPoint2, b));
    } else {
      // Ellipse is fixed; show the whole ellipse until the start is picked,
      // then the live start→cursor arc.
      const sketch::Ellipse el =
          sketch::EllipseFromClicks(a, edit.pendingPoint2, edit.pendingPoint3);
      if (!edit.hasPendingPoint4) {
        poly = sketch::TessellateEntity(el);
      } else {
        double sp = sketch::EllipseParamOfPoint(
            el.center, el.rMajor, el.rMinor, el.rotation, edit.pendingPoint4);
        double ep = sketch::EllipseParamOfPoint(el.center, el.rMajor, el.rMinor,
                                                el.rotation, b);
        while (ep <= sp)
          ep += glm::two_pi<double>();
        poly = sketch::TessellateEntity(sketch::EllipseArc{
            el.center, el.rMajor, el.rMinor, el.rotation, sp, ep});
      }
    }
    break;
  default:
    return;
  }
  for (size_t i = 0; i + 1 < poly.size(); i++) {
    out.push_back(LineVertex(sketch->plane.PlaneToWorld(poly[i])));
    out.push_back(LineVertex(sketch->plane.PlaneToWorld(poly[i + 1])));
  }
}

void DrawSelectionBox(AppState& state) {
  SketchEditState& edit = state.sketchEdit;
  if (!edit.IsEditing() || !edit.boxActive)
    return;
  if (glm::length(edit.boxEnd - edit.boxStart) < 2.0f)
    return;
  const bool crossing = edit.boxEnd.x < edit.boxStart.x; // right→left
  ImDrawList* dl = ViewportDrawList();
  const ImVec2 a(std::min(edit.boxStart.x, edit.boxEnd.x),
                 std::min(edit.boxStart.y, edit.boxEnd.y));
  const ImVec2 b(std::max(edit.boxStart.x, edit.boxEnd.x),
                 std::max(edit.boxStart.y, edit.boxEnd.y));
  const ImU32 edge = crossing ? IM_COL32(110, 210, 130, 230) // crossing: green
                              : IM_COL32(90, 160, 255, 230); // window: blue
  const ImU32 fill =
      crossing ? IM_COL32(110, 210, 130, 40) : IM_COL32(90, 160, 255, 40);
  dl->AddRectFilled(a, b, fill);
  if (crossing) { // dashed outline for crossing (CAD convention)
    const float dash = 6.0f;
    for (float x = a.x; x < b.x; x += 2 * dash) {
      dl->AddLine(ImVec2(x, a.y), ImVec2(std::min(x + dash, b.x), a.y), edge);
      dl->AddLine(ImVec2(x, b.y), ImVec2(std::min(x + dash, b.x), b.y), edge);
    }
    for (float y = a.y; y < b.y; y += 2 * dash) {
      dl->AddLine(ImVec2(a.x, y), ImVec2(a.x, std::min(y + dash, b.y)), edge);
      dl->AddLine(ImVec2(b.x, y), ImVec2(b.x, std::min(y + dash, b.y)), edge);
    }
  } else {
    dl->AddRect(a, b, edge);
  }
}

void DrawSnapGlyphs(AppState& state) {
  SketchEditState& edit = state.sketchEdit;
  if (!edit.IsEditing() || !edit.hasCurrentSnap)
    return;
  SketchFeature* sketch = ResolveActiveSketch(state);
  if (!sketch)
    return;

  const glm::vec2 p =
      SketchUVToScreen(state, sketch->plane, edit.currentSnap.uv);
  ImDrawList* dl = ViewportDrawList();
  const ImU32 accent = IM_COL32(255, 220, 120, 255);

  // Alignment guides: a dashed inference line from the shared vertex through
  // the snapped point, extended a little past it.
  auto guide = [&](const std::optional<sketch::PointRef>& ref) {
    if (!ref)
      return;
    glm::dvec2 targetUV;
    if (sketch::IsDatum(ref->entity)) {
      targetUV = sketch::DatumPointUV(ref->entity, ref->which);
    } else {
      const size_t idx = sketch->IndexOfEntity(ref->entity);
      if (idx == SIZE_MAX)
        return;
      targetUV = sketch::EntityPointUV(sketch->entities[idx], ref->which);
    }
    const glm::vec2 v = SketchUVToScreen(state, sketch->plane, targetUV);
    const glm::vec2 d = p - v;
    const float len = glm::length(d);
    if (len < 1e-3f)
      return;
    const glm::vec2 dir = d / len;
    const float full = len + 24.0f;
    const ImU32 col = IM_COL32(150, 190, 255, 200);
    for (float t = 0.0f; t < full; t += 12.0f) {
      const float t1 = std::min(t + 6.0f, full);
      dl->AddLine(ImVec2(v.x + dir.x * t, v.y + dir.y * t),
                  ImVec2(v.x + dir.x * t1, v.y + dir.y * t1), col, 1.5f);
    }
  };
  guide(edit.currentSnap.alignShareX);
  guide(edit.currentSnap.alignShareY);

  dl->AddCircle(ImVec2(p.x, p.y), 7.0f, accent, 0, 2.0f); // "grabbed" ring

  float gx = p.x + 12.0f, gy = p.y - 24.0f;
  for (sketch::GlyphKind k : edit.currentSnap.glyphs) {
    const char* label = "";
    switch (k) {
    case sketch::GlyphKind::Coincident:
      label = "coincident";
      break;
    case sketch::GlyphKind::PointOnLine:
      label = "on line";
      break;
    case sketch::GlyphKind::Horizontal:
      label = "H";
      break;
    case sketch::GlyphKind::Vertical:
      label = "V";
      break;
    case sketch::GlyphKind::Parallel:
      label = "parallel";
      break;
    case sketch::GlyphKind::Perpendicular:
      label = "perpendicular";
      break;
    }
    const ImVec2 sz = ImGui::CalcTextSize(label);
    dl->AddRectFilled(ImVec2(gx - 3, gy - 1),
                      ImVec2(gx + sz.x + 3, gy + sz.y + 1),
                      IM_COL32(25, 28, 34, 210), 3.0f);
    dl->AddText(ImVec2(gx, gy), accent, label);
    gy += sz.y + 4.0f;
  }
}

ToolPrompt SketchToolPrompt(const SketchEditState& edit) {
  using T = SketchTool;
  ToolPrompt p;
  // Op re-pick modes outrank the active tool's prompt.
  if (edit.opPickSources) {
    p.text = "Operation sources: click or box-select, tap empty space to "
             "confirm";
    return p;
  }
  if (edit.opPickAxis) {
    p.text = "Click the new mirror axis (a line or an X/Y axis)";
    return p;
  }
  auto set = [&](const char* name, int step, int count, const char* what) {
    p.step = step;
    p.stepCount = count;
    p.text = name;
    if (count > 0)
      p.text +=
          " (Step " + std::to_string(step) + "/" + std::to_string(count) + ")";
    p.text += std::string(": ") + what;
  };
  // A Hole session owns the prompt while placing centres, so the user is
  // told how the session ENDS (Enter/✓) — the Point tool's own prompt does
  // not mention that. Other tools inside the session keep theirs.
  if (edit.purpose == SketchPurpose::Hole &&
      (edit.tool == T::Point || edit.tool == T::Select)) {
    set("Hole", 2, 3, "click the hole centres - Enter for the hole options");
    return p;
  }
  // For 3-click tools (arcs / ellipse), the current step from the pending
  // state.
  const int click3 = edit.hasPendingPoint2 ? 3 : (edit.hasPendingPoint ? 2 : 1);
  switch (edit.tool) {
  case T::Select:
    break; // default tool: no prompt
  case T::Point:
    set("Point", 0, 0, "click to place a point");
    break;
  case T::Line:
    set("Line", edit.hasPendingPoint ? 2 : 1, 2,
        edit.hasPendingPoint ? "click the next point (Esc to finish)"
                             : "click the start point");
    break;
  case T::Circle:
    set("Circle", edit.hasPendingPoint ? 2 : 1, 2,
        edit.hasPendingPoint ? "click to set the radius" : "click the centre");
    break;
  case T::Rectangle:
    set("Rectangle", edit.hasPendingPoint ? 2 : 1, 2,
        edit.hasPendingPoint ? "click the opposite corner"
                             : "click the first corner");
    break;
  case T::RectangleCenter:
    set("Center Rectangle", edit.hasPendingPoint ? 2 : 1, 2,
        edit.hasPendingPoint ? "click a corner" : "click the centre");
    break;
  case T::Arc:
    set("Arc", click3, 3,
        click3 == 1   ? "click the centre"
        : click3 == 2 ? "click the start point"
                      : "click the end point");
    break;
  case T::ArcThreePoint:
    set("3-Point Arc", click3, 3,
        click3 == 1   ? "click the start point"
        : click3 == 2 ? "click the end point"
                      : "click a point on the arc");
    break;
  case T::Ellipse:
    set("Ellipse", click3, 3,
        click3 == 1   ? "click the centre"
        : click3 == 2 ? "click the major-axis end"
                      : "click to set the minor radius");
    break;
  case T::Spline:
    set("Spline", 0, 0,
        edit.splinePoles.empty()
            ? "click to place control poles"
            : "click for more poles (Enter or double-click to finish)");
    break;
  case T::Trim:
    set("Trim", 0, 0, "click a segment to remove it");
    break;
  case T::Text:
    set("Text", 0, 0,
        edit.textEditId != 0 ? "click to place another label (Esc to finish)"
                             : "click to place the label");
    break;
  case T::Mirror:
    set("Mirror", edit.mirrorAxisPhase ? 2 : 1, 2,
        edit.mirrorAxisPhase
            ? "click a line to mirror across"
            : "click or box-select geometry (tap empty space to continue)");
    break;
  case T::Fillet:
    set("Fillet", 0, 0, "click a corner where two lines meet");
    break;
  case T::Pattern:
    set("Pattern", edit.selected.empty() ? 1 : 2, 2,
        edit.selected.empty()
            ? "click or box-select the geometry to pattern"
            : "set count/spacing, then tap empty space (or Apply)");
    break;
  case T::Offset:
    set("Offset", edit.selected.empty() ? 1 : 2, 2,
        edit.selected.empty()
            ? "click or box-select a closed loop (its edges)"
            : "set the distance, then tap empty space (or Apply)");
    break;
  case T::Dimension:
    if (edit.dimPlacing)
      set("Dimension", 3, 3,
          !edit.dimPlacePair ? "click to place the label"
          : edit.dimPlaceFromLine != 0
              ? "place the dimension (position picks horizontal/vertical/"
                "aligned), or pick a second target"
              : "place the dimension — position picks horizontal/vertical/"
                "aligned");
    else
      set("Dimension", edit.selected.empty() ? 1 : 2, 3,
          edit.selected.empty()
              ? "pick a point, line, or circle"
              : "pick a second target, or click empty space for length");
    break;
  case T::Project:
    set("Project", 0, 0, "click a body edge or face to project it");
    break;
  case T::CalibrateImage:
    set("Calibrate Image", edit.calibStage + 1, 3,
        edit.calibStage == 0 ? "click the first point on the image"
        : edit.calibStage == 1
            ? "click the second point (a known distance apart)"
            : "enter the real distance in the Sketch panel, then Apply");
    break;
  case T::EllipseArc: {
    const int st = edit.hasPendingPoint4   ? 5
                   : edit.hasPendingPoint3 ? 4
                   : edit.hasPendingPoint2 ? 3
                   : edit.hasPendingPoint  ? 2
                                           : 1;
    set("Elliptic Arc", st, 5,
        st == 1   ? "click the centre"
        : st == 2 ? "click the major-axis end"
        : st == 3 ? "click to set the minor radius"
        : st == 4 ? "click the arc's start point"
                  : "click the arc's end point");
    break;
  }
  }
  return p;
}

void DrawToolPrompt(AppState& state) {
  SketchEditState& edit = state.sketchEdit;
  if (!edit.IsEditing())
    return;
  const ToolPrompt tp = SketchToolPrompt(edit);
  float y = state.viewportPos.y + 34.0f; // clear of the docked panel tab bar
  // Constraint status first — always visible while sketching, the "am I
  // done constraining?" answer at a glance. One shared pill renderer with
  // the model-space prompt bar (U8b).
  if (edit.dof == 0)
    DrawPromptPill(state, y, "Fully defined", PromptPill::StatusGood);
  else if (edit.dof > 0)
    DrawPromptPill(state, y, std::to_string(edit.dof) + " degrees of freedom",
                   PromptPill::Status);
  DrawPromptPill(state, y, tp.text, PromptPill::Prompt);
  const float statusY = y;
  DrawPromptPill(state, y, edit.actionMsg, PromptPill::Warning);
  // A dimension added driven because another one already fixes it: beside
  // the status that says so (below it when the row is full, where it can
  // cover sketch labels), the swap most people meant
  // (docs/sketching/README.md#gui).
  const SketchEditState::ReplaceOffer* offer = LiveReplaceOffer(state);
  if (offer == nullptr || offer->old.size() != 1)
    return;
  const char* kLabel = "Use this one instead";
  const ImVec2 msg = ImGui::CalcTextSize(edit.actionMsg.c_str());
  const glm::vec4 pill =
      PromptPillRect(state, statusY, glm::vec2(msg.x, msg.y));
  const float buttonW = ImGui::CalcTextSize(kLabel).x +
                        2.0f * (ImGui::GetStyle().FramePadding.x +
                                ImGui::GetStyle().WindowPadding.x);
  if (pill.z + 6.0f + buttonW <= state.viewportPos.x + state.viewportSize.x)
    ImGui::SetNextWindowPos(ImVec2(pill.z + 6.0f, (pill.y + pill.w) * 0.5f),
                            ImGuiCond_Always, ImVec2(0.0f, 0.5f));
  else
    ImGui::SetNextWindowPos(ImVec2((pill.x + pill.z) * 0.5f, y),
                            ImGuiCond_Always, ImVec2(0.5f, 0.0f));
  ImGui::SetNextWindowBgAlpha(0.85f);
  const ImGuiWindowFlags flags =
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
      ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoFocusOnAppearing;
  if (ImGui::Begin("##dimreplace", nullptr, flags)) {
    if (ImGui::SmallButton(kLabel))
      ReplaceWithDrivenDimension(state);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Delete the outlined dimension and make the new one "
                        "driving");
  }
  ImGui::End();
}

} // namespace ui
