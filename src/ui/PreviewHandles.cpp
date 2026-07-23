#include "ui/PreviewHandles.h"

#include "cad/FaceSelector.h"
#include "core/AxisRef.h"
#include "core/Document.h"
#include "core/Measure.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/FilletFeature.h"
#include "core/features/RevolveFeature.h"
#include "core/features/SketchFeature.h"
#include "ui/AppActionsInternal.h" // LineVertex
#include "ui/DragMath.h"
#include "ui/PreviewFeature.h"
#include "ui/SketchInteraction.h" // WorldToScreen

#include "imgui.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <variant>

namespace ui {

namespace {

// Accent distinct from the gizmo's XYZ palette.
const glm::vec3 kHandleColor{1.0f, 0.72f, 0.20f};
constexpr double kMinLength = 0.01; // DragDim's field minimum

// UV bounding-box center of the sketch's profile entities. `ids` restricts to
// the extrude's picked regions (empty = whole sketch); construction geometry
// never counts. Falls back to the plane origin for an empty sketch.
glm::vec2 ProfileCenterUV(const SketchFeature& sk,
                          const std::vector<std::vector<uint32_t>>& regions) {
  std::vector<uint32_t> ids;
  for (const auto& region : regions)
    ids.insert(ids.end(), region.begin(), region.end());
  glm::vec2 lo(FLT_MAX), hi(-FLT_MAX);
  bool any = false;
  const auto add = [&](glm::vec2 p) {
    lo = glm::min(lo, p);
    hi = glm::max(hi, p);
    any = true;
  };
  for (const sketch::Entity& e : sk.entities) {
    if (sketch::IsConstruction(e))
      continue;
    if (!ids.empty() &&
        std::find(ids.begin(), ids.end(), sketch::EntityId(e)) == ids.end())
      continue;
    std::visit(
        [&](const auto& g) {
          using T = std::decay_t<decltype(g)>;
          if constexpr (std::is_same_v<T, sketch::Point>) {
            add(g.p);
          } else if constexpr (std::is_same_v<T, sketch::Line>) {
            add(g.a);
            add(g.b);
          } else if constexpr (std::is_same_v<T, sketch::Circle>) {
            const glm::vec2 r(static_cast<float>(g.radius));
            add(g.center - r);
            add(g.center + r);
          } else if constexpr (std::is_same_v<T, sketch::Arc> ||
                               std::is_same_v<T, sketch::Ellipse> ||
                               std::is_same_v<T, sketch::EllipseArc>) {
            add(g.center); // the center is close enough for an anchor
          } else { // BSpline: every pole
            for (const glm::vec2& pole : g.poles)
              add(pole);
          }
        },
        e);
  }
  return any ? (lo + hi) * 0.5f : glm::vec2(0.0f);
}

// What the pending feature's handle looks like this frame. Recomputed per
// frame (cheap); a live drag keeps using its grab-time axis instead.
struct HandleSpec {
  bool rotate = false;   // revolve arc vs linear arrow
  glm::vec3 anchor{0};   // arrow base / arc center
  glm::vec3 dir{0, 0, 1};// arrow direction / arc plane normal (unit)
  float arcRadius = 0.0f;// rotate only
  double value = 0.0;    // the parameter's current value
  bool degrees = false;  // readout formatting
};

// The sketch a feature extrudes/revolves, resolved by stable id.
const SketchFeature* SketchOf(const Document& doc, uint64_t sketchId) {
  for (const auto& f : doc.GetFeatures())
    if (f->GetId() == sketchId)
      return dynamic_cast<const SketchFeature*>(f.get());
  return nullptr;
}

bool ComputeHandleSpec(AppState& state, HandleSpec& spec) {
  if (!state.document || state.assembly)
    return false;
  size_t index = 0;
  Feature* f = ResolvePendingFeature(state, &index);
  if (!f)
    return false;
  Document& doc = *state.document;

  if (auto* ext = dynamic_cast<ExtrudeFeature*>(f)) {
    // Only the plain-scalar Blind/Symmetric distance; an expression-driven
    // field must not be clobbered by a drag, and the other end modes have no
    // single draggable scalar.
    if (!ext->distanceExpr.empty())
      return false;
    if (ext->endMode != ExtrudeEnd::Blind &&
        ext->endMode != ExtrudeEnd::Symmetric)
      return false;
    const SketchFeature* sk = SketchOf(doc, ext->sketchId);
    if (!sk)
      return false;
    const glm::vec3 base =
        sk->plane.PlaneToWorld(ProfileCenterUV(*sk, ext->regions));
    const glm::vec3 N = glm::normalize(sk->plane.GetNormal());
    const double end = ext->endMode == ExtrudeEnd::Symmetric
                           ? ext->distance * 0.5
                           : ext->distance;
    spec.anchor = base + N * static_cast<float>(end); // tracks the end cap
    spec.dir = N;
    spec.value = ext->distance;
    return true;
  }

  if (auto* fil = dynamic_cast<FilletFeature*>(f)) {
    if (!fil->radiusExpr.empty() || fil->edges.empty())
      return false;
    const ElementRef& ref = fil->edges.front();
    // The displayed body is already blended — its toponames carry the blend's
    // own tag, so the picked edge only resolves on the blend's INPUT body
    // (the multi-edge-fillet lesson; same source edge picking uses). Its
    // midpoint sits right at the blend surface: a fine handle anchor.
    if (index == 0)
      return false;
    const std::vector<Body>* input = doc.CachedBodiesAfter(index - 1);
    if (input == nullptr)
      return false;
    const auto b = Measure::ElementBounds(*input, ref);
    if (!b)
      return false;
    const glm::vec3 P = (b->min + b->max) * 0.5f;
    // Tangent from the edge box's dominant extent (exact for straight edges,
    // close enough for the handle on curved ones).
    const glm::vec3 ext = b->max - b->min;
    const glm::vec3 T =
        glm::length(ext) > 1e-6f ? glm::normalize(ext) : glm::vec3(0, 0, 1);
    // "Pull away = bigger": from the body center through the edge, projected
    // ⟂ to the edge tangent; camera-right fallback when that degenerates.
    glm::vec3 out(0.0f);
    for (const Body& body : *input) {
      if (body.createdBy != ref.featureId)
        continue;
      glm::vec3 bmin, bmax;
      body.shape.GetBoundingBox(bmin, bmax);
      out = P - (bmin + bmax) * 0.5f;
      out -= glm::dot(out, T) * T;
      break;
    }
    if (glm::length(out) < 1e-4f && state.camera)
      out = glm::cross(state.camera->GetForward(), T);
    if (glm::length(out) < 1e-4f)
      return false;
    spec.anchor = P;
    spec.dir = glm::normalize(out);
    spec.value = fil->radius;
    return true;
  }

  if (auto* rev = dynamic_cast<RevolveFeature*>(f)) {
    if (!rev->angleExpr.empty())
      return false;
    const SketchFeature* sk = SketchOf(doc, rev->sketchId);
    if (!sk)
      return false;
    // Resolve the axis exactly as regeneration does, against the features
    // before the pending one. RegenContext only reads here; the bodies ref
    // is non-const purely for regeneration's sake.
    glm::vec3 origin, dir;
    RegenContext ctx{const_cast<std::vector<Body>&>(doc.GetBodies()),
                     doc.GetFeatures(), index};
    if (!ResolveAxis(ctx, rev->axis, origin, dir))
      return false;
    const glm::vec3 c =
        sk->plane.PlaneToWorld(ProfileCenterUV(*sk, {}));
    // Arc centered on the axis at the profile's height, through the profile.
    const float along = glm::dot(c - origin, dir);
    spec.anchor = origin + dir * along;
    spec.dir = dir;
    spec.arcRadius =
        std::max(glm::length(c - spec.anchor), 0.35f * GizmoScale(state));
    spec.rotate = true;
    spec.value = rev->angleDeg;
    spec.degrees = true;
    return true;
  }

  return false;
}

// Writes the dragged value into whichever supported feature is pending and
// regenerates — byte-identical to what the preview dialog's widget does.
void ApplyHandleValue(AppState& state, Feature& f, double v) {
  if (auto* ext = dynamic_cast<ExtrudeFeature*>(&f))
    ext->distance = std::max(v, kMinLength);
  else if (auto* fil = dynamic_cast<FilletFeature*>(&f))
    fil->radius = std::max(v, kMinLength);
  else if (auto* rev = dynamic_cast<RevolveFeature*>(&f))
    rev->angleDeg = std::clamp(v, 1.0, 360.0);
  else
    return;
  state.document->Regenerate();
}

double CurrentHandleValue(const Feature& f) {
  if (auto* ext = dynamic_cast<const ExtrudeFeature*>(&f)) return ext->distance;
  if (auto* fil = dynamic_cast<const FilletFeature*>(&f)) return fil->radius;
  if (auto* rev = dynamic_cast<const RevolveFeature*>(&f)) return rev->angleDeg;
  return 0.0;
}

void AppendArrow(std::vector<LineVertex3D>& lines, const glm::vec3& base,
                 const glm::vec3& A, float L) {
  const float head = 0.18f * L;
  const glm::vec3 tip = base + A * L, side = PerpTo(A);
  lines.push_back(LineVertex(base, kHandleColor));
  lines.push_back(LineVertex(tip, kHandleColor));
  lines.push_back(LineVertex(tip, kHandleColor));
  lines.push_back(LineVertex(tip - A * head + side * head, kHandleColor));
  lines.push_back(LineVertex(tip, kHandleColor));
  lines.push_back(LineVertex(tip - A * head - side * head, kHandleColor));
}

} // namespace

void BuildPreviewHandleOverlay(AppState& state,
                               std::vector<LineVertex3D>& lines) {
  HandleSpec spec;
  if (!ComputeHandleSpec(state, spec))
    return;
  const float L = 0.9f * GizmoScale(state);

  if (!spec.rotate) {
    AppendArrow(lines, spec.anchor, spec.dir, L);
    // A blend/short-arrow base tick so the grab point reads on the geometry.
    const glm::vec3 s1 = PerpTo(spec.dir) * (0.09f * L);
    const glm::vec3 s2 = glm::cross(spec.dir, PerpTo(spec.dir)) * (0.09f * L);
    lines.push_back(LineVertex(spec.anchor - s1, kHandleColor));
    lines.push_back(LineVertex(spec.anchor + s1, kHandleColor));
    lines.push_back(LineVertex(spec.anchor - s2, kHandleColor));
    lines.push_back(LineVertex(spec.anchor + s2, kHandleColor));
  } else {
    const glm::vec3 b1 = PerpTo(spec.dir), b2 = glm::cross(spec.dir, b1);
    const int seg = 48;
    glm::vec3 prev = spec.anchor + b1 * spec.arcRadius;
    for (int s = 1; s <= seg; s++) {
      const float a = (float)s / seg * glm::two_pi<float>();
      const glm::vec3 p =
          spec.anchor + (std::cos(a) * b1 + std::sin(a) * b2) * spec.arcRadius;
      lines.push_back(LineVertex(prev, kHandleColor));
      lines.push_back(LineVertex(p, kHandleColor));
      prev = p;
    }
  }

  // Floating readout at the handle while dragging (foreground list: on top of
  // everything, viewport-space like the measure readout).
  if (state.handleDrag.active) {
    const glm::vec3 tipWorld = spec.rotate
                                   ? spec.anchor + PerpTo(spec.dir) * spec.arcRadius
                                   : spec.anchor + spec.dir * L;
    const glm::vec2 s = WorldToScreen(state, tipWorld);
    char buf[32];
    if (spec.degrees)
      snprintf(buf, sizeof(buf), "%.1f\xc2\xb0", spec.value);
    else
      snprintf(buf, sizeof(buf), "%.3f", spec.value);
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    const ImVec2 pos(s.x + 12.0f, s.y - ImGui::GetFontSize() - 4.0f);
    fg->AddText(ImVec2(pos.x + 1, pos.y + 1), IM_COL32(0, 0, 0, 200), buf);
    fg->AddText(pos, IM_COL32(255, 214, 90, 255), buf);
  }
}

bool HandlePreviewHandleInput(AppState& state, const Ray& ray, bool clicked,
                              bool down, bool released) {
  auto& h = state.handleDrag;

  if (h.active) {
    Feature* f = ResolvePendingFeature(state);
    if (released || !down || !f || f->GetId() != h.featureId) {
      h.active = false; // value stays where the drag left it
      return true;
    }
    double v = h.startValue;
    if (!h.rotate) {
      const float t =
          RayAxisParam(ray.origin, ray.direction, h.axisOrigin, h.axisDir);
      // Symmetric extrude: the end cap moves distance/2 per unit of distance,
      // so a drag maps 2:1 to keep the cap under the cursor.
      auto* ext = dynamic_cast<ExtrudeFeature*>(f);
      const double factor =
          ext && ext->endMode == ExtrudeEnd::Symmetric ? 2.0 : 1.0;
      v = h.startValue + (t - h.startParam) * factor;
    } else {
      float ang;
      if (!RotateAngle(ray.origin, ray.direction, h.axisOrigin, h.axisDir, ang))
        return true;
      v = h.startValue + glm::degrees(ang - h.startParam);
    }
    // Round to display precision; Ctrl snaps coarse (0.5 units / 5°).
    const bool deg = dynamic_cast<RevolveFeature*>(f) != nullptr;
    const double quantum =
        ImGui::GetIO().KeyCtrl ? (deg ? 5.0 : 0.5) : (deg ? 0.1 : 0.001);
    v = std::round(v / quantum) * quantum;
    ApplyHandleValue(state, *f, v);
    return true;
  }

  if (!clicked)
    return false;
  HandleSpec spec;
  if (!ComputeHandleSpec(state, spec)) {
    return false;
  }
  const float scale = GizmoScale(state);
  const float grab = 0.14f * scale, L = 0.9f * scale;

  bool hit = false;
  float startParam = 0.0f;
  if (!spec.rotate) {
    const float t = std::clamp(
        RayAxisParam(ray.origin, ray.direction, spec.anchor, spec.dir), 0.0f, L);
    hit = PointRayDist(spec.anchor + spec.dir * t, ray.origin, ray.direction) <
          grab;
    if (hit)
      startParam =
          RayAxisParam(ray.origin, ray.direction, spec.anchor, spec.dir);
  } else {
    glm::vec3 p;
    if (RayPlane(ray.origin, ray.direction, spec.anchor, spec.dir, p) &&
        std::abs(glm::length(p - spec.anchor) - spec.arcRadius) < grab) {
      float ang;
      hit = RotateAngle(ray.origin, ray.direction, spec.anchor, spec.dir, ang);
      startParam = ang;
    }
  }
  if (!hit) {
  }

  Feature* f = ResolvePendingFeature(state);
  h.active = true;
  h.featureId = f->GetId();
  h.rotate = spec.rotate;
  h.axisOrigin = spec.anchor;
  h.axisDir = spec.dir;
  h.startValue = CurrentHandleValue(*f);
  h.startParam = startParam;
  return true; // consumed: don't also run picking on this click
}

void AbortPreviewHandleDrag(AppState& state) {
  auto& h = state.handleDrag;
  if (!h.active)
    return;
  if (Feature* f = ResolvePendingFeature(state))
    if (f->GetId() == h.featureId)
      ApplyHandleValue(state, *f, h.startValue);
  h.active = false;
}

} // namespace ui
