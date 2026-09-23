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
glm::vec2 ConstrainLabelOffset(const SketchFeature& sk,
                               const sketch::Constraint& con, glm::vec2 off) {
  using K = sketch::ConstraintKind;
  switch (con.kind) {
  case K::Distance: {
    const auto a = RefUV(sk, con.a), b = RefUV(sk, con.b);
    if (!a || !b || glm::length(*b - *a) < 1e-6f)
      return off;
    const glm::vec2 d = glm::normalize(*b - *a);
    return off - d * glm::dot(off, d); // perpendicular-to-span only
  }
  case K::DistancePL: {
    // The measured span runs point → foot, perpendicular to the line; the
    // allowed offset is therefore along the LINE's direction.
    const auto la = RefUV(sk, {con.b.entity, 0});
    const auto lb = RefUV(sk, {con.b.entity, 1});
    if (!la || !lb || glm::length(*lb - *la) < 1e-6f)
      return off;
    const glm::vec2 d = glm::normalize(*lb - *la);
    return d * glm::dot(off, d);
  }
  case K::DistanceX:
    return {0.0f, off.y}; // the dimension line only moves vertically
  case K::DistanceY:
    return {off.x, 0.0f};
  default:
    return off;
  }
}

// A filled arrowhead at `tip` pointing along `dir` (screen space).
void Arrow(ImDrawList* dl, ImVec2 tip, glm::vec2 dir, ImU32 col) {
  glm::vec2 n{-dir.y, dir.x};
  const float len = 9.0f, w = 3.5f;
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
  const glm::vec2 dv = *lb - *la;
  const float len = glm::length(dv);
  if (len < 1e-6f)
    return false;
  const glm::vec2 d = dv / len;
  const glm::vec2 n(-d.y, d.x);
  const glm::vec2 target = *p - 2.0f * n * glm::dot(*p - *la, n);
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
// expression, empty = literal), re-solves, one undo record.
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
void ApplyDimensionEdit(AppState& state, SketchFeature& sketch, size_t index,
                        int dimIdx, double value,
                        const std::string& expr = {}) {
  if (dimIdx < 0 || dimIdx >= (int)sketch.constraints.size())
    return;
  nlohmann::json before = DocumentIO::FeatureToJson(sketch);
  sketch::Constraint& con = sketch.constraints[dimIdx];
  using K = sketch::ConstraintKind;
  std::vector<sketch::Entity> preSolve; // reflection seed rollback (below)
  bool reflected = false;
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
      preSolve = sketch.entities;
      reflected = ReflectPLPoint(sketch, con);
      if (expr.empty())
        value = -value; // GCS abs()es the magnitude; store it positive
      break;
    default:
      if (expr.empty())
        value = -value; // unsigned kinds: |v|, no direction to flip
      break; // expression: raw value; the solver clamps unsigned kinds
    }
  }
  con.value = value;
  con.valueExpr = expr;
  sketch::Solver solver;
  solver.Load(sketch);
  if (solver.Solve().ok) {
    solver.WriteBack(sketch.entities);
  } else if (reflected) {
    // The flip couldn't solve (over-constrained): drop the reflection seed
    // so the geometry isn't left at the hand-placed mirror position.
    sketch.entities = std::move(preSolve);
  }
  sketch.RegenerateDerived();
  state.document->Regenerate();
  nlohmann::json after = DocumentIO::FeatureToJson(sketch);
  // Already applied to the live object — record only. Execute would
  // ReplaceFeature with an identical copy, dangling the caller's pointer
  // (DrawDimensions keeps iterating this sketch's constraints).
  state.commands->PushApplied(
      std::make_unique<ModifyFeatureCommand>(index, before, after));
}
} // namespace

// UV position of a point ref (line endpoint / point / circle centre / datum).
std::optional<glm::vec2> RefUV(const SketchFeature& sk,
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
                        size_t bufSize, float width) {
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
  const ImU32 col = IM_COL32(120, 200, 255, 235);
  const ImU32 colHot = IM_COL32(255, 220, 130, 255);
  auto scr = [&](glm::vec2 uv) {
    glm::vec2 s = SketchUVToScreen(state, sketch->plane, uv);
    return ImVec2(s.x, s.y);
  };
  // Screen-pixel delta → sketch-UV delta around `at` (the plane can sit
  // rotated/foreshortened on screen): invert the local 2x2 screen basis.
  auto uvDelta = [&](const ImVec2& d, glm::vec2 at) -> glm::vec2 {
    const ImVec2 o = scr(at);
    const ImVec2 px = scr(at + glm::vec2(1.0f, 0.0f));
    const ImVec2 py = scr(at + glm::vec2(0.0f, 1.0f));
    const glm::vec2 bx(px.x - o.x, px.y - o.y), by(py.x - o.x, py.y - o.y);
    const float det = bx.x * by.y - bx.y * by.x;
    if (std::abs(det) < 1e-9f)
      return {0.0f, 0.0f};
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
      g.kind = edit.dimPlaceKind;
      g.a = edit.dimPlaceA;
      g.b = edit.dimPlaceB;
      g.value = edit.dimPlaceValue;
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

    char label[48] = "";
    ImVec2 labelPos;
    // The offset actually drawn: the stored one, or — while it is still
    // zero (files from before offsets were written) — a per-kind legible
    // default. A drag materializes the drawn offset into the constraint
    // first, so the label never jumps when grabbed.
    glm::vec2 effOff = con.labelOffset;
    using K = sketch::ConstraintKind;

    if (con.kind == K::Radius || con.kind == K::Diameter) {
      size_t ci = sketch->IndexOfEntity(con.a.entity);
      if (ci == SIZE_MAX)
        continue;
      const auto& ent = sketch->entities[ci];
      glm::vec2 center;
      float radius;
      glm::vec2 dir(0.7071f, 0.7071f); // circle: 45° leader
      if (auto* circ = std::get_if<sketch::Circle>(&ent)) {
        center = circ->center;
        radius = static_cast<float>(circ->radius);
      } else if (auto* arc = std::get_if<sketch::Arc>(&ent)) {
        center = arc->center;
        radius = static_cast<float>(arc->radius);
        double mid =
            0.5 * (arc->startAngle + arc->endAngle); // within the sweep
        dir = {static_cast<float>(std::cos(mid)),
               static_cast<float>(std::sin(mid))};
      } else {
        continue;
      }
      if (glm::length(effOff) < 1e-4f)
        effOff = SeedLabelOffset(*sketch, con);
      // The label sits at rim + offset; the rim anchor follows the label
      // around the circle so the leader is always radial, with the arrowhead
      // touching the rim from the label's side.
      glm::vec2 labelUV = center + dir * radius + effOff;
      const glm::vec2 fromCenter = labelUV - center;
      const glm::vec2 rdir =
          glm::length(fromCenter) > 1e-5f ? glm::normalize(fromCenter) : dir;
      const glm::vec2 rimUV = center + rdir * radius;
      const ImVec2 rimS = scr(rimUV), labS = scr(labelUV);
      dl->AddLine(rimS, labS, col, 1.4f);
      const glm::vec2 lead(rimS.x - labS.x, rimS.y - labS.y);
      if (glm::length(lead) > 1e-3f)
        Arrow(dl, rimS, glm::normalize(lead), col);
      // |value|: a negative-evaluating expr means a flip, never a "-R".
      snprintf(label, sizeof(label),
               con.kind == K::Diameter ? "\xC3\x98%.3g" : "R%.3g",
               std::abs(con.value));
      labelPos = labS;
    } else if (con.kind == K::Angle) {
      snprintf(label, sizeof(label), "%.1f\xC2\xB0",
               std::abs(con.value) * 57.2957795f);
      const auto a0 = RefUV(*sketch, {con.a.entity, 0});
      const auto a1 = RefUV(*sketch, {con.a.entity, 1});
      const auto b0 = RefUV(*sketch, {con.b.entity, 0});
      const auto b1 = RefUV(*sketch, {con.b.entity, 1});
      if (!a0 || !a1 || !b0 || !b1)
        continue;
      const glm::vec2 da = *a1 - *a0, db = *b1 - *b0;
      const float cross = da.x * db.y - da.y * db.x;
      if (std::abs(cross) < 1e-9f || glm::length(da) < 1e-6f ||
          glm::length(db) < 1e-6f) {
        // (Near-)parallel lines: no usable vertex — plain label fallback.
        labelPos = scr(*a0 + effOff + glm::vec2(0.3f, 0.3f));
      } else {
        // Vertex = the infinite lines' intersection.
        const float t =
            ((b0->x - a0->x) * db.y - (b0->y - a0->y) * db.x) / cross;
        const glm::vec2 inter = *a0 + da * t;
        if (glm::length(effOff) < 1e-4f)
          effOff = SeedLabelOffset(*sketch, con);
        const float rUV = std::max(glm::length(effOff), 0.35f);
        // Pick the direction signs whose wedge best matches the stored
        // angle, preferring the side the label offset points to — dragging
        // the label across the vertex flips the measured wedge, the way
        // other CAD flips dimension quadrants.
        const glm::vec2 offDir = glm::normalize(effOff);
        glm::vec2 u = glm::normalize(da), v = glm::normalize(db);
        float bestScore = 1e30f;
        glm::vec2 bu = u, bv = v;
        for (int su = -1; su <= 1; su += 2)
          for (int sv = -1; sv <= 1; sv += 2) {
            const glm::vec2 cu = u * (float)su, cv = v * (float)sv;
            const float ang =
                std::acos(std::clamp(glm::dot(cu, cv), -1.0f, 1.0f));
            const glm::vec2 mid = cu + cv;
            const float midDot = glm::length(mid) > 1e-4f
                                     ? glm::dot(glm::normalize(mid), offDir)
                                     : 0.0f;
            const float score =
                std::abs(ang - std::abs((float)con.value)) * 4.0f - midDot;
            if (score < bestScore) {
              bestScore = score;
              bu = cu;
              bv = cv;
            }
          }
        // Sweep from bu to bv the short way; sample the arc in UV so the
        // curve stays correct under the plane's screen projection.
        const float aStart = std::atan2(bu.y, bu.x);
        float sweep = std::atan2(bv.y, bv.x) - aStart;
        while (sweep > glm::pi<float>())
          sweep -= 2.0f * glm::pi<float>();
        while (sweep < -glm::pi<float>())
          sweep += 2.0f * glm::pi<float>();
        const int kSegs = 24;
        ImVec2 prev = scr(inter + bu * rUV);
        for (int s = 1; s <= kSegs; s++) {
          const float ang = aStart + sweep * (float)s / kSegs;
          const ImVec2 p =
              scr(inter + glm::vec2(std::cos(ang), std::sin(ang)) * rUV);
          dl->AddLine(prev, p, col, 1.4f);
          prev = p;
        }
        // Arrowheads at the arc ends, tangent to the arc and pointing away
        // from its body; thin extension rays from the vertex so short lines
        // still meet their dimension.
        const float sgn = sweep >= 0.0f ? 1.0f : -1.0f;
        const auto arcArrow = [&](float ang, float outSign) {
          const glm::vec2 p =
              inter + glm::vec2(std::cos(ang), std::sin(ang)) * rUV;
          const glm::vec2 tUV =
              glm::vec2(-std::sin(ang), std::cos(ang)) * sgn * outSign;
          const ImVec2 ps = scr(p), qs = scr(p + tUV * 0.1f);
          const glm::vec2 d(qs.x - ps.x, qs.y - ps.y);
          if (glm::length(d) > 1e-3f)
            Arrow(dl, ps, glm::normalize(d), col);
        };
        arcArrow(aStart, -1.0f);
        arcArrow(aStart + sweep, 1.0f);
        dl->AddLine(scr(inter), scr(inter + bu * rUV * 1.08f), col, 0.8f);
        dl->AddLine(scr(inter), scr(inter + bv * rUV * 1.08f), col, 0.8f);
        // Label on the arc's midpoint.
        const float aMid = aStart + sweep * 0.5f;
        labelPos = scr(inter + glm::vec2(std::cos(aMid), std::sin(aMid)) * rUV);
      }
    } else { // Distance / DistanceX / DistanceY / DistancePL (points a, b)
      auto a = RefUV(*sketch, con.a), b = RefUV(*sketch, con.b);
      if (con.kind == K::DistancePL && a) {
        // Renders like a distance dim between the point and its FOOT on the
        // line (the perpendicular projection onto the infinite line).
        const auto la = RefUV(*sketch, {con.b.entity, 0});
        const auto lb = RefUV(*sketch, {con.b.entity, 1});
        b = std::nullopt;
        if (la && lb) {
          const glm::vec2 d = *lb - *la;
          const float len2 = glm::dot(d, d);
          if (len2 > 1e-12f)
            b = *la + d * (glm::dot(*a - *la, d) / len2);
        }
      }
      if (!a || !b)
        continue;
      if (glm::length(effOff) < 1e-4f)
        effOff = SeedLabelOffset(*sketch, con);
      glm::vec2 da = *a + effOff, db = *b + effOff;
      if (con.kind == K::DistanceX)
        da.y = db.y = std::min(a->y, b->y) + effOff.y;
      else if (con.kind == K::DistanceY)
        da.x = db.x = std::min(a->x, b->x) + effOff.x;
      ImVec2 sa = scr(*a), sb = scr(*b), sda = scr(da), sdb = scr(db);
      // Extension lines overshoot the dimension line a touch (drafting
      // convention); they start just off the measured points.
      const auto extend = [&](ImVec2 from, ImVec2 to) {
        glm::vec2 d(to.x - from.x, to.y - from.y);
        const float len = glm::length(d);
        if (len < 1e-3f)
          return to;
        d /= len;
        return ImVec2(to.x + d.x * 4.0f, to.y + d.y * 4.0f);
      };
      dl->AddLine(sa, extend(sa, sda), col, 1.0f);
      dl->AddLine(sb, extend(sb, sdb), col, 1.0f);
      dl->AddLine(sda, sdb, col, 1.4f); // dimension line
      double v = con.kind == K::DistanceX   ? std::abs(b->x - a->x)
                 : con.kind == K::DistanceY ? std::abs(b->y - a->y)
                                            : glm::length(*b - *a);
      snprintf(label, sizeof(label), "%.3g", v);
      glm::vec2 d{sdb.x - sda.x, sdb.y - sda.y};
      float L = std::sqrt(d.x * d.x + d.y * d.y);
      if (L > 1e-3f) {
        d /= L;
        // Arrows inside when the value + heads fit between the extension
        // lines; flipped outside (with short tails) when the span is tight.
        const float need = ImGui::CalcTextSize(label).x + 2.0f * 9.0f + 14.0f;
        if (L >= need) {
          Arrow(dl, sda, d, col);
          Arrow(dl, sdb, {-d.x, -d.y}, col);
        } else {
          Arrow(dl, sda, {-d.x, -d.y}, col);
          Arrow(dl, sdb, d, col);
          dl->AddLine(sda, ImVec2(sda.x - d.x * 16.0f, sda.y - d.y * 16.0f),
                      col, 1.4f);
          dl->AddLine(sdb, ImVec2(sdb.x + d.x * 16.0f, sdb.y + d.y * 16.0f),
                      col, 1.4f);
        }
      }
      labelPos = ImVec2((sda.x + sdb.x) * 0.5f, (sda.y + sdb.y) * 0.5f);
    }

    // Value pill: rounded, highlighted under the cursor — the affordance for
    // dragging the label into place and double-click editing the value.
    // Expression-driven dims show the formula next to the value ("h/2 = 11")
    // so a driving expression is visible at a glance.
    std::string shown = label;
    if (!con.valueExpr.empty())
      shown = con.valueExpr + " = " + shown;
    const ImVec2 tsz = ImGui::CalcTextSize(shown.c_str());
    ImVec2 tp(labelPos.x - tsz.x * 0.5f, labelPos.y - tsz.y * 0.5f);
    const ImVec2 pmin(tp.x - 5.0f, tp.y - 2.0f);
    const ImVec2 pmax(tp.x + tsz.x + 5.0f, tp.y + tsz.y + 2.0f);
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
    const float rounding = (pmax.y - pmin.y) * 0.5f;
    dl->AddRectFilled(pmin, pmax,
                      selCon ? IM_COL32(84, 66, 26, 250)
                      : hot  ? IM_COL32(45, 62, 82, 245)
                             : IM_COL32(20, 24, 30, 220),
                      rounding);
    dl->AddRect(pmin, pmax,
                (hot || selCon) ? colHot : IM_COL32(96, 150, 200, 150),
                rounding, 0, selCon ? 2.2f : 1.2f);
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
        if (std::abs(io.MouseDelta.x) + std::abs(io.MouseDelta.y) > 0.0f) {
          const glm::vec2 prev = con.labelOffset;
          glm::vec2 next = prev + uvDelta(io.MouseDelta, glm::vec2(0.0f));
          if (con.kind == K::Angle) {
            // Radial only: the label slides along its current direction
            // (changing the arc radius), never around the vertex.
            const float len = glm::length(prev);
            if (len > 1e-4f) {
              const glm::vec2 dir = prev / len;
              next = dir * std::max(glm::dot(next, dir), 0.15f);
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
      edit.editingDim = i;
      edit.dimDrag = -1; // the second press began a drag; editing wins
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
      ImGui::SetNextWindowBgAlpha(0.95f);
      ImGui::Begin("##dimedit", nullptr,
                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                       ImGuiWindowFlags_NoMove |
                       ImGuiWindowFlags_AlwaysAutoResize |
                       ImGuiWindowFlags_NoSavedSettings);
      bool commit = CompletingDimInput(state, "##v", buf, sizeof(buf), 120.0f);
      if (!editError.empty())
        ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.45f, 1.0f), "%s",
                           editError.c_str());
      ImGui::End();
      if (commit) {
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
    s_pos = ImVec2(io.MousePos.x + 14.0f, io.MousePos.y + 14.0f);
    editError.clear();
    edit.dimOnCreateOpen = true;
    return; // draw + focus from the next frame
  }

  ImGui::SetNextWindowPos(s_pos);
  ImGui::SetNextWindowBgAlpha(0.95f);
  ImGui::Begin("##dimoncreate", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize |
                   ImGuiWindowFlags_NoSavedSettings);
  ImGui::TextUnformatted(what);
  ImGui::SameLine();
  bool commit = CompletingDimInput(state, "##docv", buf, sizeof(buf), 110.0f);
  if (!editError.empty())
    ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.45f, 1.0f), "%s",
                       editError.c_str());
  ImGui::End();

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
      ApplySketchConstraint(state, kind, v, expr);
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
                      2.0f);
    if (selCon)
      dl->AddRect(rmin, rmax, colSel, 2.0f);
    dl->AddText(p, selCon ? colSel : col, text);
    return rmax;
  };

  for (const auto& [entity, items] : byEntity) {
    auto uv = RefUV(*sketch, {entity, 0});
    if (!uv)
      continue;
    const glm::vec2 s = SketchUVToScreen(state, sketch->plane, *uv);
    const ImVec2 base(s.x + 10.0f, s.y - 16.0f);
    const bool expanded = edit.glyphExpandedEntity == entity;
    // A selected constraint keeps its entity's stack expanded too, so the
    // accent doesn't hide inside a summary badge.
    bool holdsSelection = false;
    for (const BadgeItem& it : items)
      holdsSelection |= (edit.selectedConstraint == it.ci);

    if (items.size() <= 2 || expanded || holdsSelection) {
      // Fan out horizontally with real widths; track the union rect so a
      // hover-expansion stays open while the cursor is over the fan.
      float x = base.x;
      ImVec2 unionMin(base.x - 4, base.y - 4), unionMax(base.x, base.y);
      for (const BadgeItem& it : items) {
        const ImVec2 rmax = drawOne(ImVec2(x, base.y), it.text, it.ci,
                                    /*summary=*/false);
        unionMax.x = std::max(unionMax.x, rmax.x + 4);
        unionMax.y = std::max(unionMax.y, rmax.y + 4);
        x = rmax.x + 6.0f;
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
