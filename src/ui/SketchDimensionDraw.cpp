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
#include "core/sketch/SketchOrientation.h"
#include "core/sketch/SketchProfile.h"
#include "core/sketch/SketchSelection.h"
#include "core/sketch/SketchSolver.h"
#include "render/Camera.h"
#include "render/MeshRenderer.h" // LineVertex3D
#include "ui/DocumentSlots.h"
#include "ui/ExprComplete.h"   // variable completion in the dim editors
#include "ui/LiteralActions.h" // TakenVariableNames (name = value)
#include "ui/SceneOverlays.h"
#include "ui/SketchOverlay.h"   // SelectFeature
#include "ui/SketchToolbar.h"   // BeginSketchExtrude/Revolve (Shift+E/R)
#include "ui/Theme.h"           // DimensionColor, DrivenDimensionColor
#include "ui/VariableActions.h" // name = value: a project variable

#include "imgui.h"
#include "imgui_internal.h" // GetInputTextState (re-read after a completion)

#include <algorithm>
#include <cstdlib>
#include <map>
#include <optional>
#include <vector>

#include "ui/SketchInteractionInternal.h"

// Drawing dimensions and constraint glyphs over the viewport: witness lines,
// arrows, the pill with the value in it, and the inline editor that opens
// when you click one.
// Split out of SketchInteraction.cpp
// (docs/architecture.md#the-ui-actions-layer). The 843-line viewport router
// stays there; these are the blocks it calls into, and
// SketchInteractionInternal.h is that interface written down.

namespace ui {

namespace {
// Constrains a dragged dimension-label offset to the dimension's own axis
// (Phase 3.5) so the indicator stays aligned with what it measures: distance
// dims keep only the component that moves the dimension line AWAY from the
// span (the along-span slide shears the line past its extension lines);
// angle dims are handled radially at the drag site; radius/diameter leaders
// stay free (the leader legitimately follows the label anywhere).
glm::dvec2 ConstrainLabelOffset(const SketchFeature& sk,
                                const sketch::Constraint& con, glm::dvec2 off) {
  using K = sketch::ConstraintKind;
  switch (con.kind) {
  case K::Distance: {
    const auto a = RefUV(sk, con.a), b = RefUV(sk, con.b);
    if (!a || !b || glm::length(*b - *a) < 1e-6)
      return off;
    const glm::dvec2 d = glm::normalize(*b - *a);
    return off - d * glm::dot(off, d); // perpendicular-to-span only
  }
  case K::DistancePL: {
    // The measured span runs point → foot, perpendicular to the line; the
    // allowed offset is therefore along the LINE's direction.
    const auto la = RefUV(sk, {con.b.entity, 0});
    const auto lb = RefUV(sk, {con.b.entity, 1});
    if (!la || !lb || glm::length(*lb - *la) < 1e-6)
      return off;
    const glm::dvec2 d = glm::normalize(*lb - *la);
    return d * glm::dot(off, d);
  }
  case K::ArcLength: {
    // Radial from the arc's middle: the dimension arc stays concentric.
    const size_t ci = sk.IndexOfEntity(con.a.entity);
    const auto* arc =
        ci == SIZE_MAX ? nullptr : std::get_if<sketch::Arc>(&sk.entities[ci]);
    if (arc == nullptr)
      return off;
    const double mid = 0.5 * (arc->startAngle + arc->endAngle);
    const glm::dvec2 d(std::cos(mid), std::sin(mid));
    return d * std::max(glm::dot(off, d), -0.9 * arc->radius);
  }
  case K::DistanceX:
    return {0.0, off.y}; // the dimension line only moves vertically
  case K::DistanceY:
    return {off.x, 0.0};
  default:
    return off;
  }
}

// A filled arrowhead at `tip` pointing along `dir` (screen space).
void Arrow(ImDrawList* dl, ImVec2 tip, glm::dvec2 dir, ImU32 col) {
  glm::dvec2 n{-dir.y, dir.x};
  const double len = 9.0, w = 3.5;
  ImVec2 base(tip.x - dir.x * len, tip.y - dir.y * len);
  dl->AddTriangleFilled(tip, ImVec2(base.x + n.x * w, base.y + n.y * w),
                        ImVec2(base.x - n.x * w, base.y - n.y * w), col);
}

// Reflects a DistancePL constraint's point across its line, so the next
// solve's side pin (derived from geometry at Load) lands on the OPPOSITE
// side. Supports point / line-endpoint / circle-centre refs; arcs and datum
// refs return false (no flip).
bool ReflectPLPoint(SketchFeature& sk, const sketch::Constraint& con) {
  const auto p = RefUV(sk, con.a);
  const auto la = RefUV(sk, {con.b.entity, 0});
  const auto lb = RefUV(sk, {con.b.entity, 1});
  if (!p || !la || !lb)
    return false;
  const glm::dvec2 dv = *lb - *la;
  const double len = glm::length(dv);
  if (len < 1e-6)
    return false;
  const glm::dvec2 d = dv / len;
  const glm::dvec2 n(-d.y, d.x);
  const glm::dvec2 target = *p - 2.0 * n * glm::dot(*p - *la, n);
  const size_t idx = sk.IndexOfEntity(con.a.entity);
  if (idx == SIZE_MAX)
    return false; // datum refs can't move
  sketch::Entity& e = sk.entities[idx];
  if (auto* pt = std::get_if<sketch::Point>(&e))
    pt->p = target;
  else if (auto* l = std::get_if<sketch::Line>(&e))
    (con.a.which != 0 ? l->b : l->a) = target;
  else if (auto* c = std::get_if<sketch::Circle>(&e))
    c->center = target;
  else
    return false; // arc points imply angles — flip unsupported there
  return true;
}

// Commits an edited dimension value: sets it (plus the optional driving
// expression, empty = literal), re-solves, one undo record. A value that
// cannot be met, or would turn the sketch over (Solver::SolveValueEdit), is
// refused: nothing changes and the prompt bar says why ("Not changed: …").
//
// A NEGATIVE value flips the dimension's direction (the usual CAD
// convention) instead of being stored: signed kinds (DistanceX/Y, Angle)
// swap their refs for literals — the stored value stays positive — while a
// negative-EVALUATING expression keeps its raw signed value, because
// regeneration re-evaluates it (a ref swap would flip back every regen; the
// sign itself must carry the direction, and the displays show |v|).
// DistancePL's side pin re-derives from geometry each solve, so the flip
// seeds the solve on the other side by reflecting the point. Unsigned kinds
// (Distance, Radius, Diameter) have no direction: |v|, silently.
//
// The sketch's FIRST length dimension scales the drawing to it before the
// edit applies (sketch::RescaleForFirstDimension), in the same undo step.
// `newVariable` non-empty (`name = value`): the project variable is created
// with `newVariableExpr` and drives the dimension (`expr` names it), one
// undo step on the project-variable history; a refused edit creates none.
void ApplyDimensionEdit(AppState& state, SketchFeature& sketch, size_t index,
                        int dimIdx, double value, const std::string& expr = {},
                        const std::string& newVariable = {},
                        const std::string& newVariableExpr = {}) {
  if (dimIdx < 0 || dimIdx >= (int)sketch.constraints.size())
    return;
  if (sketch.constraints[dimIdx].driven) {
    state.sketchEdit.actionMsg = "A driven dimension measures the sketch; "
                                 "make it driving to edit it";
    return;
  }
  nlohmann::json before = DocumentIO::FeatureToJson(sketch);
  sketch::Constraint& con = sketch.constraints[dimIdx];
  // A refused edit restores them (the rescale scales every label offset).
  const std::vector<sketch::Constraint> oldConstraints = sketch.constraints;
  const std::vector<sketch::Entity> oldEntities = sketch.entities;
  using K = sketch::ConstraintKind;
  if (value < 0.0) {
    switch (con.kind) {
    case K::DistanceX:
    case K::DistanceY:
    case K::Angle:
      if (expr.empty()) {
        std::swap(con.a, con.b); // the other side / the mirrored wedge
        value = -value;          // keep the positive-value invariant
      }
      break; // expression: raw signed value (see above)
    case K::DistancePL:
      ReflectPLPoint(sketch, con);
      if (expr.empty())
        value = -value; // GCS abs()es the magnitude; store it positive
      break;
    default:
      if (expr.empty())
        value = -value; // unsigned kinds: |v|, no direction to flip
      break; // expression: raw value; the solver clamps unsigned kinds
    }
  }
  // A sketch drawn by eye keeps its shape at the size its first length
  // dimension gives it (docs/sketching/README.md#the-first-dimension).
  const sketch::RescaleOutcome rescale = sketch::RescaleForFirstDimension(
      sketch, con, value, static_cast<size_t>(dimIdx));
  // The orientations are read from this seed -- after the ref swap and the
  // reflection -- so a flip the user asked for is the side that is kept.
  const double oldValue = oldConstraints[dimIdx].value;
  const auto from = sketch::MeasureDimension(sketch.entities, con);
  con.value = value;
  con.valueExpr = expr;
  sketch::Solver solver;
  solver.Load(sketch);
  const sketch::EditOutcome edit = solver.SolveValueEdit(
      {{static_cast<size_t>(dimIdx), from.value_or(oldValue), value}});
  if (!edit.Accepted()) {
    // Refused: the old value, expression, refs and geometry, and no undo
    // record (docs/sketching/README.md#solving).
    sketch.constraints = oldConstraints;
    sketch.entities = oldEntities;
    state.sketchEdit.actionMsg = "Not changed: " + edit.refusal;
    return;
  }
  solver.WriteBack(sketch.entities);
  sketch.RegenerateDerived();
  if (!newVariable.empty()) {
    // The variable and the dimension it drives: one entry on the
    // project-variable history, which puts the sketch back too (the
    // extract-variables recipe, LiteralActions.cpp). Edited in place, so
    // the caller's pointer stays good.
    PushProjectVarUndoWithFeatures(
        state, {{state.parts[static_cast<size_t>(state.activePart)].id,
                 sketch.GetId(), before}});
    state.projectParameters.push_back(
        {newVariable, newVariableExpr, "made by a sketch dimension", 0.0, {}});
    SyncProjectScope(state); // regenerates every part against it
  } else {
    state.document->Regenerate();
    nlohmann::json after = DocumentIO::FeatureToJson(sketch);
    // Already applied to the live object — record only. Execute would
    // ReplaceFeature with an identical copy, dangling the caller's pointer
    // (DrawDimensions keeps iterating this sketch's constraints).
    state.commands->PushApplied(
        std::make_unique<ModifyFeatureCommand>(index, before, after));
  }
  if (!rescale.why.empty())
    state.sketchEdit.actionMsg = rescale.why;
}

// `name = expr` typed in a dimension field: the name and the expression,
// or nullopt when there is no `=`. The expression language has no `=` of
// its own, so the first one splits.
struct NamedValue {
  std::string name, expr;
};
std::optional<NamedValue> SplitNamedValue(const std::string& text) {
  const size_t eq = text.find('=');
  if (eq == std::string::npos)
    return std::nullopt;
  const auto trim = [](std::string t) {
    const size_t a = t.find_first_not_of(" \t");
    const size_t b = t.find_last_not_of(" \t");
    return a == std::string::npos ? std::string() : t.substr(a, b - a + 1);
  };
  return NamedValue{trim(text.substr(0, eq)), trim(text.substr(eq + 1))};
}

// Why `name = ...` cannot make the variable `name`: not a name, or one that
// is taken (never silently redefined). Empty when it can.
std::string NewVariableRefusal(AppState& state, const std::string& name) {
  if (!ExpressionEngine::Default().IsValidParameterName(name))
    return "'" + name +
           "' is not a variable name: a letter or _ first, then letters, "
           "digits or _, and not a unit or a built-in name";
  if (!TakenVariableNames(state).contains(name))
    return {};
  double v = 0.0;
  if (const Parameter* p = FindProjectParameter(state, name))
    v = p->value;
  else if (state.document != nullptr)
    for (const Parameter& p : state.document->GetParameters())
      if (p.name == name)
        v = p.value;
  char buf[64];
  std::snprintf(buf, sizeof buf, "%g", v);
  return "'" + name + "' already exists (= " + buf + "); type " + name +
         " alone to use it";
}
} // namespace

// UV position of a point ref (line endpoint / point / circle centre / datum).
std::optional<glm::dvec2> RefUV(const SketchFeature& sk,
                                const sketch::PointRef& r) {
  if (sketch::IsDatum(r.entity))
    return sketch::DatumPointUV(r.entity, r.which);
  size_t idx = sk.IndexOfEntity(r.entity);
  if (idx == SIZE_MAX)
    return std::nullopt;
  return sketch::EntityPointUV(sk.entities[idx], r.which);
}

namespace {

// The inline dimension editors take an expression over the document's
// variables, so they complete like every other expression field (u14). They
// already re-focus themselves every frame, which is most of what a pick
// needs; what is left is the ordering. The popup is drawn FIRST, because the
// click that picks a row also ends the edit and the verdict has to be known
// before the field reports Enter. The picked text is written AFTER the
// InputText, because a deactivating one writes its own text out on the way
// past — and ImGui is told to re-read the buffer on the frame after that,
// or its copy wins and the field commits the prefix.
bool CompletingDimInput(AppState& state, const char* id, char* buf,
                        size_t bufSize, double width) {
  static ImGuiID reloadId = 0;
  static int reloadCaret = -1;
  const ImGuiID fieldId = ImGui::GetID(id);
  const ExprPick pick = ExprCompletePopup(state, fieldId, buf, bufSize);
  if (reloadId == fieldId && reloadCaret >= 0) {
    if (ImGuiInputTextState* st = ImGui::GetInputTextState(fieldId)) {
      st->WantReloadUserBuf = true;
      st->ReloadSelectionStart = st->ReloadSelectionEnd = reloadCaret;
    }
    reloadId = 0;
    reloadCaret = -1;
  }
  ImGui::SetNextItemWidth(width);
  ImGui::SetKeyboardFocusHere();
  const bool commit =
      ImGui::InputText(id, buf, bufSize, ImGuiInputTextFlags_EnterReturnsTrue);
  if (pick.picked) {
    snprintf(buf, bufSize, "%s", pick.text.c_str());
    reloadId = fieldId;
    reloadCaret = pick.caret;
    return false; // a pick is not a commit
  }
  ExprCompleteRefresh(state, fieldId, buf);
  return commit;
}

} // namespace

namespace {} // namespace

void DrawDimensions(AppState& state) {
  SketchEditState& edit = state.sketchEdit;
  if (!edit.IsEditing())
    return;
  size_t index = 0;
  SketchFeature* sketch = ResolveActiveSketch(state, &index);
  if (!sketch)
    return;

  ImDrawList* dl = ViewportDrawList();
  ImGuiIO& io = ImGui::GetIO();
  const ImU32 colDriving = ImGui::ColorConvertFloat4ToU32(DimensionColor());
  const ImU32 colDriven =
      ImGui::ColorConvertFloat4ToU32(DrivenDimensionColor());
  const ImU32 colHot = IM_COL32(255, 220, 130, 255);
  // The label whose context menu is open (right-click on it), and a Driven
  // toggle picked there: applied after the loop, since it replaces the
  // sketch feature (one undo step) and `sketch` would dangle.
  static int s_ctxDim = -1;
  int toggleDriven = -1, toDelete = -1;
  // The dimensions a driven one repeats, outlined while the status offers
  // to replace them (docs/sketching/README.md#gui).
  const SketchEditState::ReplaceOffer* offer = LiveReplaceOffer(state);
  const auto offered = [&](int i) {
    return offer != nullptr &&
           std::find(offer->old.begin(), offer->old.end(),
                     static_cast<size_t>(i)) != offer->old.end();
  };
  auto scr = [&](glm::dvec2 uv) {
    glm::dvec2 s = SketchUVToScreen(state, sketch->plane, uv);
    return ImVec2(s.x, s.y);
  };
  // Screen-pixel delta → sketch-UV delta around `at` (the plane can sit
  // rotated/foreshortened on screen): invert the local 2x2 screen basis.
  auto uvDelta = [&](const ImVec2& d, glm::dvec2 at) -> glm::dvec2 {
    const ImVec2 o = scr(at);
    const ImVec2 px = scr(at + glm::dvec2(1.0, 0.0));
    const ImVec2 py = scr(at + glm::dvec2(0.0, 1.0));
    const glm::dvec2 bx(px.x - o.x, px.y - o.y), by(py.x - o.x, py.y - o.y);
    const double det = bx.x * by.y - bx.y * by.x;
    if (std::abs(det) < 1e-9)
      return {0.0, 0.0};
    return {(d.x * by.y - d.y * by.x) / det, (bx.x * d.y - bx.y * d.x) / det};
  };

  edit.dimHot = -1; // refreshed below; the canvas reads it next frame
  if (edit.selectedConstraint >= (int)sketch->constraints.size())
    edit.selectedConstraint = -1; // the list changed under it (undo etc.)

  // Placement ghost (Phase 3.7): while the Dimension tool is placing, a
  // transient constraint rides the list for THIS draw only — rendered like
  // any dimension, popped before return, never serialized (StartPlacement
  // closes the inline editor, the only in-loop committer). Pill interaction
  // is disabled for every dim meanwhile so the placement click reaches the
  // viewport.
  bool ghostPushed = false;
  if (edit.dimPlacing) {
    std::optional<sketch::Constraint> ghost;
    if (edit.dimPlacePair) {
      if (const auto p = ClassifyPairPlacement(
              *sketch, edit.dimPlaceA, edit.dimPlaceB, edit.hoverPoint)) {
        sketch::Constraint g;
        g.kind = p->kind;
        g.a = p->a;
        g.b = p->b;
        g.value = p->value;
        g.labelOffset = p->labelOffset;
        ghost = g;
      }
    } else {
      sketch::Constraint g;
      g.kind = PlacedArcKind(*sketch, edit.dimPlaceKind, edit.dimPlaceA,
                             edit.hoverPoint);
      g.a = edit.dimPlaceA;
      g.b = edit.dimPlaceB;
      g.value = g.kind == sketch::ConstraintKind::ArcLength
                    ? sketch::MeasureDimension(sketch->entities, g)
                          .value_or(edit.dimPlaceValue)
                    : edit.dimPlaceValue;
      g.labelOffset =
          FixedPlacementLabelOffset(*sketch, g.kind, g.a, g.b, edit.hoverPoint);
      ghost = g;
    }
    if (ghost) {
      sketch->constraints.push_back(*ghost);
      ghostPushed = true;
    } else {
      edit.dimPlacing = false; // a target vanished (undo) — drop the stage
    }
  }

  for (int i = 0; i < (int)sketch->constraints.size(); i++) {
    sketch::Constraint& con = sketch->constraints[i]; // drag writes labelOffset
    if (!sketch::IsDimension(con.kind))
      continue;
    // A driven dimension shows what the geometry measures, muted and in
    // parentheses (docs/sketching/README.md#gui); its stored value is not
    // its truth.
    const ImU32 col = con.driven ? colDriven : colDriving;
    const double conValue =
        con.driven ? sketch::MeasureDimension(sketch->entities, con)
                         .value_or(con.value)
                   : con.value;

    char label[48] = "";
    ImVec2 labelPos;
    // The offset actually drawn: the stored one, or — while it is still
    // zero (files from before offsets were written) — a per-kind legible
    // default. A drag materializes the drawn offset into the constraint
    // first, so the label never jumps when grabbed.
    glm::dvec2 effOff = con.labelOffset;
    using K = sketch::ConstraintKind;

    if (con.kind == K::Radius || con.kind == K::Diameter) {
      size_t ci = sketch->IndexOfEntity(con.a.entity);
      if (ci == SIZE_MAX)
        continue;
      const auto& ent = sketch->entities[ci];
      glm::dvec2 center;
      double radius;
      glm::dvec2 dir(0.7071, 0.7071); // circle: 45° leader
      if (auto* circ = std::get_if<sketch::Circle>(&ent)) {
        center = circ->center;
        radius = circ->radius;
      } else if (auto* arc = std::get_if<sketch::Arc>(&ent)) {
        center = arc->center;
        radius = arc->radius;
        double mid =
            0.5 * (arc->startAngle + arc->endAngle); // within the sweep
        dir = {(std::cos(mid)), (std::sin(mid))};
      } else {
        continue;
      }
      if (glm::length(effOff) < 1e-4)
        effOff = SeedLabelOffset(*sketch, con);
      // The label sits at rim + offset; the rim anchor follows the label
      // around the circle so the leader is always radial, with the arrowhead
      // touching the rim from the label's side.
      glm::dvec2 labelUV = center + dir * radius + effOff;
      const glm::dvec2 fromCenter = labelUV - center;
      const glm::dvec2 rdir =
          glm::length(fromCenter) > 1e-5 ? glm::normalize(fromCenter) : dir;
      const glm::dvec2 rimUV = center + rdir * radius;
      const ImVec2 rimS = scr(rimUV), labS = scr(labelUV);
      dl->AddLine(rimS, labS, col, 1.4);
      const glm::dvec2 lead(rimS.x - labS.x, rimS.y - labS.y);
      if (glm::length(lead) > 1e-3)
        Arrow(dl, rimS, glm::normalize(lead), col);
      // |value|: a negative-evaluating expr means a flip, never a "-R".
      snprintf(label, sizeof(label),
               con.kind == K::Diameter ? "\xC3\x98%.3g" : "R%.3g",
               std::abs(conValue));
      labelPos = labS;
    } else if (con.kind == K::Angle) {
      snprintf(label, sizeof(label), "%.1f\xC2\xB0",
               std::abs(conValue) * 57.2957795);
      const auto a0 = RefUV(*sketch, {con.a.entity, 0});
      const auto a1 = RefUV(*sketch, {con.a.entity, 1});
      const auto b0 = RefUV(*sketch, {con.b.entity, 0});
      const auto b1 = RefUV(*sketch, {con.b.entity, 1});
      if (!a0 || !a1 || !b0 || !b1)
        continue;
      const glm::dvec2 da = *a1 - *a0, db = *b1 - *b0;
      const double cross = da.x * db.y - da.y * db.x;
      if (std::abs(cross) < 1e-9 || glm::length(da) < 1e-6 ||
          glm::length(db) < 1e-6) {
        // (Near-)parallel lines: no usable vertex — plain label fallback.
        labelPos = scr(*a0 + effOff + glm::dvec2(0.3, 0.3));
      } else {
        // Vertex = the infinite lines' intersection.
        const double t =
            ((b0->x - a0->x) * db.y - (b0->y - a0->y) * db.x) / cross;
        const glm::dvec2 inter = *a0 + da * t;
        if (glm::length(effOff) < 1e-4)
          effOff = SeedLabelOffset(*sketch, con);
        const double rUV = std::max(glm::length(effOff), 0.35);
        // Pick the direction signs whose wedge best matches the stored
        // angle, preferring the side the label offset points to — dragging
        // the label across the vertex flips the measured wedge, the way
        // other CAD flips dimension quadrants.
        const glm::dvec2 offDir = glm::normalize(effOff);
        glm::dvec2 u = glm::normalize(da), v = glm::normalize(db);
        double bestScore = 1e30;
        glm::dvec2 bu = u, bv = v;
        for (int su = -1; su <= 1; su += 2)
          for (int sv = -1; sv <= 1; sv += 2) {
            const glm::dvec2 cu = u * (double)su, cv = v * (double)sv;
            const double ang =
                std::acos(std::clamp(glm::dot(cu, cv), -1.0, 1.0));
            const glm::dvec2 mid = cu + cv;
            const double midDot = glm::length(mid) > 1e-4
                                      ? glm::dot(glm::normalize(mid), offDir)
                                      : 0.0;
            const double score =
                std::abs(ang - std::abs(conValue)) * 4.0 - midDot;
            if (score < bestScore) {
              bestScore = score;
              bu = cu;
              bv = cv;
            }
          }
        // Sweep from bu to bv the short way; sample the arc in UV so the
        // curve stays correct under the plane's screen projection.
        const double aStart = std::atan2(bu.y, bu.x);
        double sweep = std::atan2(bv.y, bv.x) - aStart;
        while (sweep > glm::pi<double>())
          sweep -= 2.0 * glm::pi<double>();
        while (sweep < -glm::pi<double>())
          sweep += 2.0 * glm::pi<double>();
        const int kSegs = 24;
        ImVec2 prev = scr(inter + bu * rUV);
        for (int s = 1; s <= kSegs; s++) {
          const double ang = aStart + sweep * (double)s / kSegs;
          const ImVec2 p =
              scr(inter + glm::dvec2(std::cos(ang), std::sin(ang)) * rUV);
          dl->AddLine(prev, p, col, 1.4);
          prev = p;
        }
        // Arrowheads at the arc ends, tangent to the arc and pointing away
        // from its body; thin extension rays from the vertex so short lines
        // still meet their dimension.
        const double sgn = sweep >= 0.0 ? 1.0 : -1.0;
        const auto arcArrow = [&](double ang, double outSign) {
          const glm::dvec2 p =
              inter + glm::dvec2(std::cos(ang), std::sin(ang)) * rUV;
          const glm::dvec2 tUV =
              glm::dvec2(-std::sin(ang), std::cos(ang)) * sgn * outSign;
          const ImVec2 ps = scr(p), qs = scr(p + tUV * 0.1);
          const glm::dvec2 d(qs.x - ps.x, qs.y - ps.y);
          if (glm::length(d) > 1e-3)
            Arrow(dl, ps, glm::normalize(d), col);
        };
        arcArrow(aStart, -1.0);
        arcArrow(aStart + sweep, 1.0);
        dl->AddLine(scr(inter), scr(inter + bu * rUV * 1.08), col, 0.8);
        dl->AddLine(scr(inter), scr(inter + bv * rUV * 1.08), col, 0.8);
        // Label on the arc's midpoint.
        const double aMid = aStart + sweep * 0.5;
        labelPos =
            scr(inter + glm::dvec2(std::cos(aMid), std::sin(aMid)) * rUV);
      }
    } else if (con.kind == K::ArcLength) {
      // A dimension line parallel to the arc: concentric, through the label,
      // across the arc's sweep, with radial extension lines from its ends.
      size_t ci = sketch->IndexOfEntity(con.a.entity);
      const auto* arc = ci == SIZE_MAX
                            ? nullptr
                            : std::get_if<sketch::Arc>(&sketch->entities[ci]);
      if (arc == nullptr)
        continue;
      if (glm::length(effOff) < 1e-4)
        effOff = SeedLabelOffset(*sketch, con);
      const double a0 = arc->startAngle;
      double sweep = std::fmod(arc->endAngle - arc->startAngle, 2.0 * M_PI);
      if (sweep <= 0.0)
        sweep += 2.0 * M_PI;
      const double mid = a0 + 0.5 * sweep;
      const glm::dvec2 dirMid(std::cos(mid), std::sin(mid));
      const double rDim = std::max(
          0.05 * arc->radius, glm::length(arc->center + dirMid * arc->radius +
                                          effOff - arc->center));
      const auto at = [&](double ang, double r) {
        return scr(arc->center + glm::dvec2(std::cos(ang), std::sin(ang)) * r);
      };
      const int kSegs = std::max(8, static_cast<int>(48 * sweep / M_PI));
      ImVec2 prev = at(a0, rDim);
      for (int k = 1; k <= kSegs; k++) {
        const ImVec2 p = at(a0 + sweep * k / kSegs, rDim);
        dl->AddLine(prev, p, col, 1.4);
        prev = p;
      }
      for (const double ang : {a0, a0 + sweep}) {
        const double beyond =
            rDim + (rDim > arc->radius ? 1.0 : -1.0) * 0.04 * arc->radius;
        dl->AddLine(at(ang, arc->radius), at(ang, beyond), col, 1.0);
      }
      // Arrowheads at both ends, along the dimension arc and outward.
      const auto arcArrow = [&](double ang, double outSign) {
        const ImVec2 ps = at(ang, rDim);
        const ImVec2 qs = at(ang + outSign * 1e-3, rDim);
        const glm::dvec2 d(qs.x - ps.x, qs.y - ps.y);
        if (glm::length(d) > 1e-6)
          Arrow(dl, ps, glm::normalize(d), col);
      };
      arcArrow(a0, -1.0);
      arcArrow(a0 + sweep, 1.0);
      // A plain number, as a distance's: the concentric dimension arc says
      // what it measures (the UI font has no arc symbol, U+2312).
      snprintf(label, sizeof(label), "%.3g", std::abs(conValue));
      labelPos = at(mid, rDim);
    } else { // Distance / DistanceX / DistanceY / DistancePL (points a, b)
      auto a = RefUV(*sketch, con.a), b = RefUV(*sketch, con.b);
      if (con.kind == K::DistancePL && a) {
        // Renders like a distance dim between the point and its FOOT on the
        // line (the perpendicular projection onto the infinite line).
        const auto la = RefUV(*sketch, {con.b.entity, 0});
        const auto lb = RefUV(*sketch, {con.b.entity, 1});
        b = std::nullopt;
        if (la && lb) {
          const glm::dvec2 d = *lb - *la;
          const double len2 = glm::dot(d, d);
          if (len2 > 1e-12)
            b = *la + d * (glm::dot(*a - *la, d) / len2);
        }
      }
      if (!a || !b)
        continue;
      if (glm::length(effOff) < 1e-4)
        effOff = SeedLabelOffset(*sketch, con);
      glm::dvec2 da = *a + effOff, db = *b + effOff;
      if (con.kind == K::DistanceX)
        da.y = db.y = std::min(a->y, b->y) + effOff.y;
      else if (con.kind == K::DistanceY)
        da.x = db.x = std::min(a->x, b->x) + effOff.x;
      ImVec2 sa = scr(*a), sb = scr(*b), sda = scr(da), sdb = scr(db);
      // Extension lines overshoot the dimension line a touch (drafting
      // convention); they start just off the measured points.
      const auto extend = [&](ImVec2 from, ImVec2 to) {
        glm::dvec2 d(to.x - from.x, to.y - from.y);
        const double len = glm::length(d);
        if (len < 1e-3)
          return to;
        d /= len;
        return ImVec2(to.x + d.x * 4.0, to.y + d.y * 4.0);
      };
      dl->AddLine(sa, extend(sa, sda), col, 1.0);
      dl->AddLine(sb, extend(sb, sdb), col, 1.0);
      dl->AddLine(sda, sdb, col, 1.4); // dimension line
      double v = con.kind == K::DistanceX   ? std::abs(b->x - a->x)
                 : con.kind == K::DistanceY ? std::abs(b->y - a->y)
                                            : glm::length(*b - *a);
      snprintf(label, sizeof(label), "%.3g", v);
      glm::dvec2 d{sdb.x - sda.x, sdb.y - sda.y};
      double L = std::sqrt(d.x * d.x + d.y * d.y);
      if (L > 1e-3) {
        d /= L;
        // Arrows inside when the value + heads fit between the extension
        // lines; flipped outside (with short tails) when the span is tight.
        const double need = ImGui::CalcTextSize(label).x + 2.0 * 9.0 + 14.0;
        if (L >= need) {
          Arrow(dl, sda, d, col);
          Arrow(dl, sdb, {-d.x, -d.y}, col);
        } else {
          Arrow(dl, sda, {-d.x, -d.y}, col);
          Arrow(dl, sdb, d, col);
          dl->AddLine(sda, ImVec2(sda.x - d.x * 16.0, sda.y - d.y * 16.0), col,
                      1.4);
          dl->AddLine(sdb, ImVec2(sdb.x + d.x * 16.0, sdb.y + d.y * 16.0), col,
                      1.4);
        }
      }
      labelPos = ImVec2((sda.x + sdb.x) * 0.5, (sda.y + sdb.y) * 0.5);
    }

    // Value pill: rounded, highlighted under the cursor — the affordance for
    // dragging the label into place and double-click editing the value.
    // Expression-driven dims show the formula next to the value ("h/2 = 11")
    // so a driving expression is visible at a glance.
    std::string shown = label;
    if (!con.valueExpr.empty())
      shown = con.valueExpr + " = " + shown;
    if (con.driven)
      shown = "(" + shown + ")";
    const ImVec2 tsz = ImGui::CalcTextSize(shown.c_str());
    ImVec2 tp(labelPos.x - tsz.x * 0.5, labelPos.y - tsz.y * 0.5);
    const ImVec2 pmin(tp.x - 5.0, tp.y - 2.0);
    const ImVec2 pmax(tp.x + tsz.x + 5.0, tp.y + tsz.y + 2.0);
    const bool overLabel = io.MousePos.x >= pmin.x && io.MousePos.x <= pmax.x &&
                           io.MousePos.y >= pmin.y && io.MousePos.y <= pmax.y;
    // Pills are inert during placement — the ghost must not latch dimHot
    // (which would eat the placement click) and real dims can't drag.
    const bool interactive = !edit.dimPlacing;
    const bool dragging = edit.dimDrag == i;
    if (interactive && (overLabel || dragging))
      edit.dimHot = i;
    const bool hot = interactive && (overLabel || dragging);
    // Selected pill: accent fill + a heavier ring, distinct from hover.
    const bool selCon = edit.selectedConstraint == i;
    const double rounding = (pmax.y - pmin.y) * 0.5;
    dl->AddRectFilled(pmin, pmax,
                      selCon ? IM_COL32(84, 66, 26, 250)
                      : hot  ? IM_COL32(45, 62, 82, 245)
                             : IM_COL32(20, 24, 30, 220),
                      rounding);
    dl->AddRect(pmin, pmax,
                (hot || selCon) ? colHot
                : con.driven    ? IM_COL32(96, 112, 128, 120)
                                : IM_COL32(96, 150, 200, 150),
                rounding, 0, selCon ? 2.2 : 1.2);
    if (offered(i) && !selCon && !hot)
      dl->AddRect(ImVec2(pmin.x - 3, pmin.y - 3),
                  ImVec2(pmax.x + 3, pmax.y + 3), IM_COL32(255, 205, 120, 255),
                  rounding + 3, 0, 2.0);
    dl->AddText(tp, (hot || selCon) ? IM_COL32(255, 240, 200, 255) : col,
                shown.c_str());

    // Drag places the label (writes the serialized labelOffset; one undo
    // record on release). The pre-drag json lives across frames.
    static nlohmann::json s_dragBefore;
    static bool s_dragMoved = false;
    if (interactive && overLabel && edit.editingDim != i && edit.dimDrag < 0 &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      edit.dimDrag = i;
      s_dragBefore = DocumentIO::FeatureToJson(*sketch);
      s_dragMoved = false;
      con.labelOffset = effOff; // materialize a seeded default before moving
    }
    if (dragging) {
      if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (std::abs(io.MouseDelta.x) + std::abs(io.MouseDelta.y) > 0.0) {
          const glm::dvec2 prev = con.labelOffset;
          glm::dvec2 next = prev + uvDelta(io.MouseDelta, glm::dvec2(0.0));
          if (con.kind == K::Angle) {
            // Radial only: the label slides along its current direction
            // (changing the arc radius), never around the vertex.
            const double len = glm::length(prev);
            if (len > 1e-4) {
              const glm::dvec2 dir = prev / len;
              next = dir * std::max(glm::dot(next, dir), 0.15);
            }
          } else {
            next = ConstrainLabelOffset(*sketch, con, next);
          }
          con.labelOffset = next;
          s_dragMoved = true;
        }
      } else {
        edit.dimDrag = -1;
        if (s_dragMoved) {
          const nlohmann::json after = DocumentIO::FeatureToJson(*sketch);
          if (after != s_dragBefore)
            state.commands->PushApplied(std::make_unique<ModifyFeatureCommand>(
                index, s_dragBefore, after));
        } else {
          // Released without moving: the click SELECTS the dimension
          // (Delete/Backspace removes it); entity selection clears so
          // Delete stays unambiguous.
          edit.selectedConstraint = i;
          edit.selected.clear();
        }
      }
    }

    if (interactive && overLabel &&
        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
      edit.dimDrag = -1; // the second press began a drag; editing wins
      if (con.driven)
        edit.actionMsg = "A driven dimension measures the sketch; make it "
                         "driving to edit it";
      else
        edit.editingDim = i;
    }
    // Right-click (a release that did not orbit the view) opens the label's
    // menu: the Driven toggle and Delete.
    if (interactive && overLabel &&
        ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
      const ImVec2 d = io.MouseDragMaxDistanceAbs[ImGuiMouseButton_Right];
      if (d.x * d.x + d.y * d.y < 16.0f) {
        s_ctxDim = i;
        ImGui::OpenPopup("##dimctx");
      }
    }

    if (edit.editingDim == i) {
      static char buf[128];
      static int lastEditing = -1;
      static std::string editError;
      if (lastEditing != i) {
        // Seed with the driving expression when there is one, else the value
        // (degrees in the UI for angles, as always).
        if (!con.valueExpr.empty())
          snprintf(buf, sizeof(buf), "%s", con.valueExpr.c_str());
        else if (con.kind == K::Angle)
          snprintf(buf, sizeof(buf), "%.4g", glm::degrees(con.value));
        else
          snprintf(buf, sizeof(buf), "%.4g", con.value);
        lastEditing = i;
        editError.clear();
      }
      ImGui::SetNextWindowPos(ImVec2(tp.x - 4, tp.y - 4));
      ImGui::SetNextWindowBgAlpha(0.95);
      ImGui::Begin("##dimedit", nullptr,
                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                       ImGuiWindowFlags_NoMove |
                       ImGuiWindowFlags_AlwaysAutoResize |
                       ImGuiWindowFlags_NoSavedSettings);
      bool commit = CompletingDimInput(state, "##v", buf, sizeof(buf), 120.0);
      if (!editError.empty())
        ImGui::TextColored(ImVec4(0.95, 0.45, 0.45, 1.0), "%s",
                           editError.c_str());
      ImGui::End();
      const auto named = commit ? SplitNamedValue(buf) : std::nullopt;
      if (named) {
        // `name = expr`: a new project variable drives the dimension
        // (docs/ui/numeric-input.md#a-new-variable). A taken or invalid
        // name keeps the editor open with the reason.
        editError = NewVariableRefusal(state, named->name);
        std::optional<double> ev;
        if (editError.empty()) {
          const auto evaluated = ExpressionEngine::Default().EvaluateText(
              named->expr, state.document->Symbols());
          if (evaluated.has_value())
            ev = *evaluated;
          else
            editError = evaluated.error().message;
        }
        if (!ev) {
          commit = false;
        } else {
          const double v = con.kind == K::Angle ? glm::radians(*ev) : *ev;
          ApplyDimensionEdit(state, *sketch, index, i, v, named->name,
                             named->name, named->expr);
          edit.editingDim = -1;
          lastEditing = -1;
        }
      } else if (commit) {
        // A plain number stays a literal; anything else is an expression
        // over the document's variables (angles speak degrees, stored as
        // radians). Invalid expressions keep the editor open with the error.
        char* end = nullptr;
        double v = std::strtod(buf, &end);
        while (end != nullptr && *end == ' ')
          end++;
        const bool isNumber = end != buf && end != nullptr && *end == '\0';
        if (isNumber) {
          if (con.kind == K::Angle)
            v = glm::radians(v);
          ApplyDimensionEdit(state, *sketch, index, i, v);
        } else {
          const auto evaluated = ExpressionEngine::Default().EvaluateText(
              buf, state.document->Symbols());
          if (!evaluated.has_value()) {
            editError = evaluated.error().message;
            commit = false; // stay open, show the error
          } else {
            v = con.kind == K::Angle ? glm::radians(*evaluated) : *evaluated;
            ApplyDimensionEdit(state, *sketch, index, i, v, buf);
          }
        }
        if (commit) {
          edit.editingDim = -1;
          lastEditing = -1;
        }
      } else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        edit.editingDim = -1;
        lastEditing = -1;
      }
    }
  }

  if (ghostPushed)
    sketch->constraints.pop_back(); // the placement ghost was render-only

  if (ImGui::BeginPopup("##dimctx")) {
    if (s_ctxDim >= 0 && s_ctxDim < (int)sketch->constraints.size() &&
        sketch::IsDimension(sketch->constraints[s_ctxDim].kind)) {
      const bool driven = sketch->constraints[s_ctxDim].driven;
      if (ImGui::MenuItem("Driven", nullptr, driven))
        toggleDriven = s_ctxDim;
      if (ImGui::MenuItem("Delete", "Del"))
        toDelete = s_ctxDim;
    } else {
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  if (toggleDriven >= 0) {
    const bool driven = sketch->constraints[toggleDriven].driven;
    SetConstraintDriven(state, static_cast<size_t>(toggleDriven), !driven);
    s_ctxDim = -1; // `sketch` is replaced: nothing below may touch it
  } else if (toDelete >= 0) {
    edit.selectedConstraint = -1; // indices shift
    edit.editingDim = -1;
    DeleteConstraint(state, static_cast<size_t>(toDelete));
    s_ctxDim = -1; // same: `sketch` is replaced
  }
}

void DrawDimensionOnCreate(AppState& state) {
  SketchEditState& edit = state.sketchEdit;
  if (!edit.IsEditing())
    return;
  using T = SketchTool;
  const bool drawTool = edit.tool == T::Line || edit.tool == T::Circle ||
                        edit.tool == T::Rectangle ||
                        edit.tool == T::RectangleCenter ||
                        edit.tool == T::Arc || edit.tool == T::ArcThreePoint;
  if (!drawTool || edit.lastDrawn.empty()) {
    edit.dimOnCreateOpen = false;
    return;
  }
  SketchFeature* sketch = ResolveActiveSketch(state);
  if (!sketch)
    return;
  for (uint32_t id : edit.lastDrawn) {
    if (sketch->IndexOfEntity(id) == SIZE_MAX) { // undone or deleted
      edit.lastDrawn.clear();
      edit.dimOnCreateOpen = false;
      return;
    }
  }

  // What the next typed number dimensions.
  const bool rect = edit.lastDrawn.size() == 4;
  if (rect && edit.lastDrawnStage >= 2) {
    edit.dimOnCreateOpen = false; // width and height both applied
    return;
  }
  sketch::ConstraintKind kind = sketch::ConstraintKind::Distance;
  std::vector<sketch::PointRef> refs;
  const char* what = "Length";
  if (rect) {
    // lastDrawn = {bottom, right, top, left}: width first, then height.
    const uint32_t side = edit.lastDrawn[edit.lastDrawnStage == 0 ? 0 : 1];
    refs = {{side, 0}, {side, 1}};
    what = edit.lastDrawnStage == 0 ? "Width" : "Height";
  } else {
    const uint32_t id = edit.lastDrawn[0];
    const size_t ei = sketch->IndexOfEntity(id);
    if (std::holds_alternative<sketch::Line>(sketch->entities[ei])) {
      refs = {{id, 0}, {id, 1}};
    } else { // circle / arc (the only other kinds recorded)
      kind = sketch::ConstraintKind::Radius;
      refs = {{id, 0}};
      what = "Radius";
    }
  }

  ImGuiIO& io = ImGui::GetIO();
  static char buf[64];
  static ImVec2 s_pos;
  static std::string editError;
  if (!edit.dimOnCreateOpen) {
    if (io.WantTextInput)
      return; // another field (e.g. the inline dim editor) owns typing
    // A typed digit (or '.' / '-') opens the field, seeded with it.
    std::string seed;
    for (int i = 0; i < io.InputQueueCharacters.Size; i++) {
      const ImWchar c = io.InputQueueCharacters[i];
      if ((c >= '0' && c <= '9') || c == '.' || c == '-')
        seed += static_cast<char>(c);
    }
    if (seed.empty())
      return;
    snprintf(buf, sizeof(buf), "%s", seed.c_str());
    s_pos = ImVec2(io.MousePos.x + 14.0, io.MousePos.y + 14.0);
    editError.clear();
    edit.dimOnCreateOpen = true;
    return; // draw + focus from the next frame
  }

  ImGui::SetNextWindowPos(s_pos);
  ImGui::SetNextWindowBgAlpha(0.95);
  ImGui::Begin("##dimoncreate", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize |
                   ImGuiWindowFlags_NoSavedSettings);
  ImGui::TextUnformatted(what);
  ImGui::SameLine();
  bool commit = CompletingDimInput(state, "##docv", buf, sizeof(buf), 110.0);
  if (!editError.empty())
    ImGui::TextColored(ImVec4(0.95, 0.45, 0.45, 1.0), "%s", editError.c_str());
  ImGui::End();

  ConstraintExtras extras;
  extras.rescaleFirst = true; // the first dimension keeps the drawn shape
  if (const auto named = commit ? SplitNamedValue(buf) : std::nullopt) {
    // `name = expr`: a new project variable drives the dimension.
    editError = NewVariableRefusal(state, named->name);
    if (editError.empty()) {
      const auto evaluated = ExpressionEngine::Default().EvaluateText(
          named->expr, state.document->Symbols());
      if (!evaluated.has_value()) {
        editError = evaluated.error().message;
      } else {
        extras.newVariable = named->name;
        extras.newVariableExpr = named->expr;
        edit.dimOnCreateOpen = false;
        edit.selected = refs;
        // Replaces nothing (edited in place) but regenerates every part.
        ApplySketchConstraint(state, kind, std::abs(*evaluated), named->name,
                              nullptr, OverConstrained::Refuse, &extras);
        if (rect)
          edit.lastDrawnStage++;
        else
          edit.lastDrawn.clear();
        return;
      }
    }
    commit = false; // stay open, show why
  }
  if (commit) {
    // A plain number stays a literal; anything else is an expression over
    // the document's variables (the inline dim editor's convention). The
    // kinds created here (Distance, Radius) are unsigned — a negative
    // input silently means its magnitude (the flip convention has no
    // direction to act on); negative-evaluating exprs keep their raw value
    // (the solver clamps unsigned kinds).
    char* end = nullptr;
    double v = std::strtod(buf, &end);
    while (end != nullptr && *end == ' ')
      end++;
    const bool isNumber = end != buf && end != nullptr && *end == '\0';
    if (isNumber)
      v = std::abs(v);
    std::string expr;
    if (!isNumber) {
      const auto evaluated = ExpressionEngine::Default().EvaluateText(
          buf, state.document->Symbols());
      if (!evaluated.has_value()) {
        editError = evaluated.error().message;
        commit = false; // stay open, show the error
      } else {
        v = *evaluated;
        expr = buf;
      }
    }
    if (commit) {
      edit.dimOnCreateOpen = false;
      editError.clear();
      edit.selected = refs;
      // Replaces the sketch feature through the command system — the local
      // pointer is dead after this call.
      ApplySketchConstraint(state, kind, v, expr, nullptr,
                            OverConstrained::Refuse, &extras);
      if (rect)
        edit.lastDrawnStage++; // width applied → height next (then done)
      else
        edit.lastDrawn.clear(); // one dimension per entity
    }
  } else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    // Dismiss without a dimension; the draw tool's staged state is untouched
    // (the viewport Escape cascade defers to this field while it is open).
    edit.dimOnCreateOpen = false;
    editError.clear();
  }
}

void DrawConstraintGlyphs(AppState& state) {
  SketchEditState& edit = state.sketchEdit;
  edit.glyphHot = -1; // refreshed below; the canvas reads it next frame
  if (!edit.IsEditing() || !edit.showConstraintGlyphs)
    return;
  size_t index = 0;
  SketchFeature* sketch = ResolveActiveSketch(state, &index);
  if (!sketch)
    return;
  ImDrawList* dl = ViewportDrawList();
  ImGuiIO& io = ImGui::GetIO();
  const ImU32 col = IM_COL32(150, 210, 170, 220);
  const ImU32 colSel = IM_COL32(255, 220, 130, 255);
  using K = sketch::ConstraintKind;

  // Collect the badges per entity first, then lay each entity's stack out
  // with real text widths (the old fixed 13 px advance overlapped wide
  // glyphs). Entities with more than two badges collapse to one "xN"
  // summary that fans out on hover — the mess in dense sketches was
  // every constraint of a vertex stamped on top of its neighbors.
  struct BadgeItem {
    int ci;
    const char* text;
  };
  std::map<uint32_t, std::vector<BadgeItem>> byEntity;
  auto badge = [&](int ci, uint32_t entity, const char* text) {
    byEntity[entity].push_back({ci, text});
  };

  for (int i = 0; i < (int)sketch->constraints.size(); i++) {
    const auto& c = sketch->constraints[i];
    switch (c.kind) {
    case K::Horizontal:
      badge(i, c.a.entity, "H");
      break;
    case K::Vertical:
      badge(i, c.a.entity, "V");
      break;
    case K::Parallel:
      badge(i, c.a.entity, "//");
      badge(i, c.b.entity, "//");
      break;
    case K::Perpendicular:
      badge(i, c.a.entity, "|_");
      break;
    case K::Equal:
      badge(i, c.a.entity, "=");
      badge(i, c.b.entity, "=");
      break;
    case K::Coincident:
      badge(i, c.a.entity, "co");
      break;
    case K::PointOnLine:
      badge(i, c.b.entity, "on");
      break;
    case K::Tangent:
      badge(i, c.a.entity, "T");
      break;
    case K::Curvature:
      badge(i, c.a.entity, "G2");
      break;
    case K::Symmetric:
      badge(i, c.c.entity, "sym");
      break;
    case K::Midpoint:
      badge(i, c.b.entity, "mid");
      break;
    case K::Fixed:
      badge(i, c.a.entity, "fix");
      break;
    case K::Concentric:
      badge(i, c.a.entity, "con");
      break;
    case K::Collinear:
      badge(i, c.a.entity, "L");
      badge(i, c.b.entity, "L");
      break;
    default:
      break; // dimensions render via DrawDimensions
    }
  }

  // Draw one entity's stack. Returns the union rect of what it drew (for
  // the hover-keeps-expanded test).
  uint32_t stillExpanded = 0;
  const auto drawOne = [&](const ImVec2& p, const char* text, int ci,
                           bool summary) -> ImVec2 {
    const ImVec2 sz = ImGui::CalcTextSize(text);
    const ImVec2 rmin(p.x - 2, p.y - 1);
    const ImVec2 rmax(p.x + sz.x + 2, p.y + sz.y + 1);
    const bool over = io.MousePos.x >= rmin.x && io.MousePos.x <= rmax.x &&
                      io.MousePos.y >= rmin.y && io.MousePos.y <= rmax.y;
    if (over && !summary)
      edit.glyphHot = ci;
    const bool selCon = !summary && edit.selectedConstraint == ci;
    dl->AddRectFilled(rmin, rmax,
                      selCon ? IM_COL32(84, 66, 26, 250)
                      : over ? IM_COL32(45, 62, 52, 235)
                             : IM_COL32(25, 32, 28, 200),
                      2.0);
    if (selCon)
      dl->AddRect(rmin, rmax, colSel, 2.0);
    dl->AddText(p, selCon ? colSel : col, text);
    return rmax;
  };

  for (const auto& [entity, items] : byEntity) {
    auto uv = RefUV(*sketch, {entity, 0});
    if (!uv)
      continue;
    const glm::dvec2 s = SketchUVToScreen(state, sketch->plane, *uv);
    const ImVec2 base(s.x + 10.0, s.y - 16.0);
    const bool expanded = edit.glyphExpandedEntity == entity;
    // A selected constraint keeps its entity's stack expanded too, so the
    // accent doesn't hide inside a summary badge.
    bool holdsSelection = false;
    for (const BadgeItem& it : items)
      holdsSelection |= (edit.selectedConstraint == it.ci);

    if (items.size() <= 2 || expanded || holdsSelection) {
      // Fan out horizontally with real widths; track the union rect so a
      // hover-expansion stays open while the cursor is over the fan.
      double x = base.x;
      ImVec2 unionMin(base.x - 4, base.y - 4), unionMax(base.x, base.y);
      for (const BadgeItem& it : items) {
        const ImVec2 rmax = drawOne(ImVec2(x, base.y), it.text, it.ci,
                                    /*summary=*/false);
        unionMax.x = std::max(unionMax.x, rmax.x + 4);
        unionMax.y = std::max(unionMax.y, rmax.y + 4);
        x = rmax.x + 6.0;
      }
      if (expanded && io.MousePos.x >= unionMin.x &&
          io.MousePos.x <= unionMax.x && io.MousePos.y >= unionMin.y &&
          io.MousePos.y <= unionMax.y) {
        stillExpanded = entity;
      }
    } else {
      char text[16];
      std::snprintf(text, sizeof(text), "x%d", (int)items.size());
      const ImVec2 rmax = drawOne(base, text, -1, /*summary=*/true);
      if (io.MousePos.x >= base.x - 4 && io.MousePos.x <= rmax.x + 4 &&
          io.MousePos.y >= base.y - 4 && io.MousePos.y <= rmax.y + 4) {
        stillExpanded = entity; // hover the summary: expand next frame
      }
    }
  }
  edit.glyphExpandedEntity = stillExpanded;
}

} // namespace ui
