// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/PreviewHandles.h"
#include "ui/Overlays.h" // ViewportDrawList

#include "ui/SketchOverlay.h"
#include "ui/ViewActions.h" // section helpers, VisibleSceneBounds

#include "cad/FaceSelector.h"
#include "core/AxisRef.h"
#include "core/DirectEdit.h"
#include "core/Document.h"
#include "core/Measure.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/FilletFeature.h"
#include "core/features/MoveFaceFeature.h"
#include "core/features/OffsetFaceFeature.h"
#include "core/features/RevolveFeature.h"
#include "core/features/SketchFeature.h"
#include "ui/DragMath.h"
#include "ui/PreviewFeature.h"
#include "ui/SketchInteraction.h"  // WorldToScreen
#include "ui/ViewportHandle.h"     // HitTestHandles
#include "ui/ViewportHandleDraw.h" // kHandleHalfPx

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

// UV bounding box of the sketch's profile entities. `ids` restricts to the
// extrude's picked regions (empty = whole sketch); construction geometry
// never counts. False for an empty sketch.
bool ProfileBoundsUV(const SketchFeature& sk,
                     const std::vector<std::vector<uint32_t>>& regions,
                     glm::vec2& lo, glm::vec2& hi) {
  std::vector<uint32_t> ids;
  for (const auto& region : regions)
    ids.insert(ids.end(), region.begin(), region.end());
  lo = glm::vec2(FLT_MAX);
  hi = glm::vec2(-FLT_MAX);
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
            const glm::dvec2 r(g.radius);
            add(g.center - r);
            add(g.center + r);
          } else if constexpr (std::is_same_v<T, sketch::Arc> ||
                               std::is_same_v<T, sketch::Ellipse> ||
                               std::is_same_v<T, sketch::EllipseArc>) {
            add(g.center); // the center is close enough for an anchor
          } else if constexpr (std::is_same_v<T, sketch::Text>) {
            add(g.anchor);
          } else { // BSpline: every pole
            for (const glm::vec2& pole : g.poles)
              add(pole);
          }
        },
        e);
  }
  return any;
}

// UV bounding-box center of the profile; the plane origin for an empty one.
glm::vec2 ProfileCenterUV(const SketchFeature& sk,
                          const std::vector<std::vector<uint32_t>>& regions) {
  glm::vec2 lo, hi;
  return ProfileBoundsUV(sk, regions, lo, hi) ? (lo + hi) * 0.5f
                                              : glm::vec2(0.0f);
}

// Which scalar of the pending feature a handle drags. It is also the
// handle's sub-id (HandleId(HandleOwner::Feature, param)), so the two
// handles of one preview — an extrude's arrow and its twist arc, or a
// two-sided extrude's two arrows — hover, highlight and drag apart.
enum class HandleParam : int {
  None = 0,
  Distance,  // extrude distance
  Distance2, // a two-sided extrude's backward distance
  Twist,     // extrude twist, degrees
  Radius,    // fillet / chamfer
  Angle,     // revolve, degrees
  Offset,    // Offset Face's distance, along the first face's normal
  MoveX,     // Move Face's translation, one arrow per axis
  MoveY,
  MoveZ,
};

// What one of the pending feature's handles looks like this frame.
// Recomputed per frame (cheap); a live drag keeps using its grab-time axis
// instead.
struct HandleSpec {
  HandleParam param = HandleParam::None;
  bool rotate = false;    // arc vs linear arrow
  glm::vec3 anchor{0};    // arrow base / arc center
  glm::vec3 dir{0, 0, 1}; // arrow direction / arc plane normal (unit)
  float arcRadius = 0.0f; // rotate only
  double value = 0.0;     // the parameter's current value
  bool degrees = false;   // readout formatting
};

// The sketch a feature extrudes/revolves, resolved by stable id.
const SketchFeature* SketchOf(const Document& doc, uint64_t sketchId) {
  for (const auto& f : doc.GetFeatures())
    if (f->GetId() == sketchId)
      return dynamic_cast<const SketchFeature*>(f.get());
  return nullptr;
}

// How far the parameter moves per unit the handle moves: a symmetric
// extrude's cap travels half its distance (and turns half its twist), so a
// drag maps 2:1 to keep the cap under the cursor.
double DragFactor(const Feature& f, HandleParam param) {
  const auto* ext = dynamic_cast<const ExtrudeFeature*>(&f);
  if (ext == nullptr || ext->endMode != ExtrudeEnd::Symmetric)
    return 1.0;
  if (param == HandleParam::Distance)
    return 2.0;
  if (param == HandleParam::Twist && !ext->herringbone)
    return 2.0;
  return 1.0;
}

void ExtrudeHandleSpecs(AppState& state, const Document& doc,
                        const ExtrudeFeature& ext,
                        std::vector<HandleSpec>& out) {
  // Only plain scalars: an expression-driven field must not be clobbered by
  // a drag. Up-to-face and through-all have no distance to drag at all.
  const ExtrudeEnd end = ext.endMode;
  if (end == ExtrudeEnd::UpToFace || end == ExtrudeEnd::ThroughAll)
    return;
  const SketchFeature* sk = SketchOf(doc, ext.sketchId);
  if (!sk)
    return;
  const glm::vec3 base =
      sk->plane.PlaneToWorld(ProfileCenterUV(*sk, ext.regions));
  const glm::vec3 N = glm::normalize(sk->plane.GetNormal());
  // `reversed` flips a blind or two-sided extrude; a symmetric one is the
  // same either way.
  const glm::vec3 fwd = ext.reversed && end != ExtrudeEnd::Symmetric ? -N : N;
  const double capAt =
      end == ExtrudeEnd::Symmetric ? ext.distance * 0.5 : ext.distance;
  if (ext.distanceExpr.empty()) {
    HandleSpec s;
    s.param = HandleParam::Distance;
    s.anchor = base + fwd * static_cast<float>(capAt); // tracks the end cap
    s.dir = fwd;
    s.value = ext.distance;
    out.push_back(s);
  }
  if (end == ExtrudeEnd::TwoSided && ext.distance2Expr.empty()) {
    HandleSpec s; // the backward side, from its own cap
    s.param = HandleParam::Distance2;
    s.anchor = base - fwd * static_cast<float>(ext.distance2);
    s.dir = -fwd;
    s.value = ext.distance2;
    out.push_back(s);
  }
  // The twist: an arc about the twist axis (the sketch normal through the
  // sketch origin) at the end cap, once there is a twist to turn.
  if ((end == ExtrudeEnd::Blind || end == ExtrudeEnd::Symmetric) &&
      ext.twistExpr.empty() && ext.twistDeg != 0.0 && !ext.thin) {
    glm::vec2 lo, hi;
    float reach = 0.0f;
    if (ProfileBoundsUV(*sk, ext.regions, lo, hi))
      for (const glm::vec2 c :
           {lo, hi, glm::vec2(lo.x, hi.y), glm::vec2(hi.x, lo.y)})
        reach = std::max(reach, glm::length(c));
    const glm::vec3 origin = sk->plane.GetOrigin();
    HandleSpec s;
    s.param = HandleParam::Twist;
    s.rotate = true;
    s.degrees = true;
    s.anchor = origin + fwd * static_cast<float>(capAt);
    s.dir = fwd;
    s.arcRadius = std::max(1.15f * reach, 0.35f * GizmoScale(state));
    s.value = ext.twistDeg;
    out.push_back(s);
  }
}

// Every handle the pending feature has, or none (false).
bool ComputeHandleSpecs(AppState& state, std::vector<HandleSpec>& out) {
  out.clear();
  if (!state.document || state.assembly)
    return false;
  size_t index = 0;
  Feature* f = ResolvePendingFeature(state, &index);
  if (!f)
    return false;
  Document& doc = *state.document;

  if (auto* ext = dynamic_cast<ExtrudeFeature*>(f)) {
    ExtrudeHandleSpecs(state, doc, *ext, out);
    return !out.empty();
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
    const glm::dvec3 P = (b->min + b->max) * 0.5;
    // Tangent from the edge box's dominant extent (exact for straight edges,
    // close enough for the handle on curved ones).
    const glm::dvec3 ext = b->max - b->min;
    const glm::dvec3 T =
        glm::length(ext) > 1e-6 ? glm::normalize(ext) : glm::dvec3(0, 0, 1);
    // "Pull away = bigger": from the body center through the edge, projected
    // ⟂ to the edge tangent; camera-right fallback when that degenerates.
    glm::dvec3 away(0.0);
    for (const Body& body : *input) {
      if (body.createdBy != ref.featureId)
        continue;
      glm::dvec3 bmin, bmax;
      BodyBounds(body, bmin, bmax);
      away = P - (bmin + bmax) * 0.5;
      away -= glm::dot(away, T) * T;
      break;
    }
    if (glm::length(away) < 1e-4 && state.camera)
      away = glm::cross(glm::dvec3(state.camera->GetForward()), T);
    if (glm::length(away) < 1e-4)
      return false;
    HandleSpec spec;
    spec.param = HandleParam::Radius;
    spec.anchor = P;
    spec.dir = glm::normalize(away);
    spec.value = fil->radius;
    out.push_back(spec);
    return true;
  }

  // The direct edits: the faces resolve on the edit's INPUT body, which is
  // where they were picked (the preview shows them already moved).
  if (auto* off = dynamic_cast<OffsetFaceFeature*>(f)) {
    if (!off->distanceExpr.empty() || off->faces.empty() || index == 0)
      return false;
    const std::vector<Body>* input = doc.CachedBodiesAfter(index - 1);
    if (input == nullptr)
      return false;
    if (const Body* held = directedit::BodyHolding(*input, off->bodyId,
                                                   off->faces.front().name)) {
      const Body& body = *held;
      const auto at = body.elementMap.Resolve(off->faces.front().name);
      if (!at)
        return false;
      const auto info = Measure::DescribeFaceAt(body, *at);
      const auto in = Measure::FaceInsetAt(body, *at);
      if (!info || glm::length(info->normal) < 1e-9)
        return false;
      const glm::dvec3 n = glm::normalize(info->normal);
      const glm::dvec3 on = in ? in->point : info->centroid;
      HandleSpec spec;
      spec.param = HandleParam::Offset;
      spec.anchor = on + n * off->distance; // rides on the moved face
      spec.dir = n;
      spec.value = off->distance;
      out.push_back(spec);
      return true;
    }
    return false;
  }
  if (auto* mv = dynamic_cast<MoveFaceFeature*>(f)) {
    if (mv->faces.empty() || index == 0)
      return false;
    const std::vector<Body>* input = doc.CachedBodiesAfter(index - 1);
    if (input == nullptr)
      return false;
    if (const Body* held = directedit::BodyHolding(*input, mv->bodyId,
                                                   mv->faces.front().name)) {
      const Body& body = *held;
      glm::dvec3 sum(0.0);
      double area = 0.0;
      for (const ElementRef& r : mv->faces)
        if (const auto at = body.elementMap.Resolve(r.name))
          if (const auto info = Measure::DescribeFaceAt(body, *at)) {
            sum += info->centroid * info->area;
            area += info->area;
          }
      if (!(area > 0.0))
        return false;
      const glm::dvec3 centre = sum / area + mv->translate;
      const HandleParam params[3] = {HandleParam::MoveX, HandleParam::MoveY,
                                     HandleParam::MoveZ};
      for (int a = 0; a < 3; a++) {
        if (!mv->translateExpr[a].empty())
          continue;
        HandleSpec spec;
        spec.param = params[a];
        spec.anchor = centre;
        spec.dir = glm::vec3(0.0f);
        spec.dir[a] = 1.0f;
        spec.value = mv->translate[a];
        out.push_back(spec);
      }
      return !out.empty();
    }
    return false;
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
    glm::dvec3 origin, dir;
    RegenContext ctx{const_cast<std::vector<Body>&>(doc.GetBodies()),
                     doc.GetFeatures(), index};
    if (!ResolveAxis(ctx, rev->axis, origin, dir))
      return false;
    const glm::dvec3 c = sk->plane.PlaneToWorld(ProfileCenterUV(*sk, {}));
    // Arc centered on the axis at the profile's height, through the profile.
    const double along = glm::dot(c - origin, dir);
    HandleSpec spec;
    spec.param = HandleParam::Angle;
    spec.anchor = origin + dir * along;
    spec.dir = dir;
    spec.arcRadius = std::max(glm::length(glm::vec3(c) - spec.anchor),
                              0.35f * GizmoScale(state));
    spec.rotate = true;
    spec.value = rev->angleDeg;
    spec.degrees = true;
    out.push_back(spec);
    return true;
  }

  return false;
}

// The active section's drag arrow. Sections are VIEW state, so this path
// shares the arrow, the ray-axis math and the readout with the feature
// handles but touches no document: no feature to resolve, no regeneration,
// no undo entry. It is also the only handle allowed on an assembly tab —
// ComputeHandleSpecs refuses those outright, because every feature handle
// edits a part document.
bool ComputeSectionHandleSpec(AppState& state, HandleSpec& spec,
                              uint64_t* outId) {
  if (!state.sectionToolOpen || state.sections.empty())
    return false;
  AppState::SectionView* sec = FindSection(state, state.sectionActiveId);
  if (sec == nullptr)
    sec = &state.sections.front();
  if (!sec->visible)
    return false;
  glm::vec3 bmin, bmax;
  if (!VisibleSceneBounds(state, bmin, bmax))
    return false;
  const glm::vec4 plane = SectionPlaneOf(state, *sec);
  const glm::vec3 n = glm::normalize(glm::vec3(plane));
  // Anchor where the cut actually is: the scene centre projected onto the
  // plane, so the arrow sits on the cut face rather than out at the origin.
  const glm::vec3 c = (bmin + bmax) * 0.5f;
  spec.anchor = c - n * (glm::dot(c, n) - plane.w);
  spec.dir = n;
  // The value the drag edits is the section's own offset, which is measured
  // along the UNREVERSED normal — reversing flips the arrow, not the number.
  spec.value = sec->offset;
  spec.rotate = false;
  spec.degrees = false;
  if (outId)
    *outId = sec->id;
  return true;
}

// Writes the dragged value into the pending feature's `param` and
// regenerates — byte-identical to what the preview dialog's widget does.
void ApplyHandleValue(AppState& state, Feature& f, HandleParam param,
                      double v) {
  auto* ext = dynamic_cast<ExtrudeFeature*>(&f);
  auto* fil = dynamic_cast<FilletFeature*>(&f);
  auto* rev = dynamic_cast<RevolveFeature*>(&f);
  auto* off = dynamic_cast<OffsetFaceFeature*>(&f);
  auto* mv = dynamic_cast<MoveFaceFeature*>(&f);
  if (off && param == HandleParam::Offset)
    // Either way through the face, never onto it: zero offsets nothing.
    off->distance =
        std::abs(v) < kMinLength ? (v < 0 ? -kMinLength : kMinLength) : v;
  else if (mv && (param == HandleParam::MoveX || param == HandleParam::MoveY ||
                  param == HandleParam::MoveZ))
    mv->translate[static_cast<int>(param) -
                  static_cast<int>(HandleParam::MoveX)] = v;
  else if (ext && param == HandleParam::Distance)
    ext->distance = std::max(v, kMinLength);
  else if (ext && param == HandleParam::Distance2)
    ext->distance2 = std::max(v, kMinLength);
  else if (ext && param == HandleParam::Twist)
    ext->twistDeg = std::clamp(v, -36000.0, 36000.0); // either hand
  else if (fil && param == HandleParam::Radius)
    fil->radius = std::max(v, kMinLength);
  else if (rev && param == HandleParam::Angle)
    rev->angleDeg = std::clamp(v, 1.0, 360.0);
  else
    return;
  state.document->Regenerate();
}

double CurrentHandleValue(const Feature& f, HandleParam param) {
  if (auto* ext = dynamic_cast<const ExtrudeFeature*>(&f))
    return param == HandleParam::Distance2 ? ext->distance2
           : param == HandleParam::Twist   ? ext->twistDeg
                                           : ext->distance;
  if (auto* fil = dynamic_cast<const FilletFeature*>(&f))
    return fil->radius;
  if (auto* rev = dynamic_cast<const RevolveFeature*>(&f))
    return rev->angleDeg;
  if (auto* off = dynamic_cast<const OffsetFaceFeature*>(&f))
    return off->distance;
  if (auto* mv = dynamic_cast<const MoveFaceFeature*>(&f))
    if (param == HandleParam::MoveX || param == HandleParam::MoveY ||
        param == HandleParam::MoveZ)
      return mv->translate[static_cast<int>(param) -
                           static_cast<int>(HandleParam::MoveX)];
  return 0.0;
}

} // namespace

void BuildPreviewHandleOverlay(AppState& state, std::vector<PrimVertex>& out) {
  std::vector<HandleSpec> specs;
  bool isSection = false;
  uint64_t sectionId = 0;
  if (!ComputeHandleSpecs(state, specs)) {
    HandleSpec spec;
    isSection = ComputeSectionHandleSpec(state, spec, &sectionId);
    if (!isSection)
      return;
    specs.push_back(spec);
  }
  const float L = 0.9f * GizmoScale(state);
  const uint64_t active = ActiveHandleId(state);

  for (const HandleSpec& spec : specs) {
    // Publish this handle for the shared hover + click hit test
    // (docs/ui/viewport-handles.md): one list, so what highlights under the
    // cursor is exactly what a click would grab.
    ViewportHandle vh;
    vh.id = isSection ? HandleId(HandleOwner::Section, sectionId)
                      : HandleId(HandleOwner::Feature,
                                 static_cast<uint64_t>(spec.param));
    vh.kind = spec.rotate ? HandleKind::Rotate : HandleKind::Linear;
    vh.origin = spec.anchor;
    vh.dir = spec.dir;
    vh.length = spec.rotate ? spec.arcRadius : L;
    vh.color = kHandleColor;
    state.viewportHandles.push_back(vh);
    // The handle itself is drawn by DrawViewportHandles from the record
    // above. The decorations below are drawn here, in the same tint, so they
    // follow the hover / drag highlight with the handle they belong to.
    const glm::vec4 tint(HandleTint(vh, state.hoveredHandle, active), 1.0f);

    // Where the cut plane is, as a square outline around the arrow's base —
    // an offset typed into the tool window is otherwise invisible until it
    // crosses geometry.
    if (isSection) {
      const glm::vec3 u = PerpTo(spec.dir), v = glm::cross(spec.dir, u);
      glm::vec3 bmin, bmax;
      const float r = VisibleSceneBounds(state, bmin, bmax)
                          ? 0.5f * glm::length(bmax - bmin)
                          : L;
      const glm::vec3 corners[4] = {
          spec.anchor + (u + v) * r, spec.anchor + (u - v) * r,
          spec.anchor - (u + v) * r, spec.anchor - (u - v) * r};
      for (int i = 0; i < 4; i++)
        AppendPrimLine(corners[i], corners[(i + 1) % 4], kHandleHalfPx, tint,
                       out);
    }

    if (!spec.rotate) {
      // A blend/short-arrow base tick so the grab point reads on the
      // geometry.
      const glm::vec3 s1 = PerpTo(spec.dir) * (0.09f * L);
      const glm::vec3 s2 = glm::cross(spec.dir, PerpTo(spec.dir)) * (0.09f * L);
      AppendPrimLine(spec.anchor - s1, spec.anchor + s1, kHandleHalfPx, tint,
                     out);
      AppendPrimLine(spec.anchor - s2, spec.anchor + s2, kHandleHalfPx, tint,
                     out);
    }

    // Floating readout at the handle being dragged (foreground list: on top
    // of everything, viewport-space like the measure readout).
    if (state.handleDrag.active && vh.id == active) {
      const glm::vec3 tipWorld =
          spec.rotate ? spec.anchor + PerpTo(spec.dir) * spec.arcRadius
                      : spec.anchor + spec.dir * L;
      const glm::vec2 s = WorldToScreen(state, tipWorld);
      char buf[32];
      if (spec.degrees)
        snprintf(buf, sizeof(buf), "%.1f\xc2\xb0", spec.value);
      else
        snprintf(buf, sizeof(buf), "%.3f", spec.value);
      ImDrawList* fg = ViewportDrawList();
      const ImVec2 pos(s.x + 12.0f, s.y - ImGui::GetFontSize() - 4.0f);
      fg->AddText(ImVec2(pos.x + 1, pos.y + 1), IM_COL32(0, 0, 0, 200), buf);
      fg->AddText(pos, IM_COL32(255, 214, 90, 255), buf);
    }
  }
}

bool HasViewportHandle(AppState& state) {
  std::vector<HandleSpec> specs;
  HandleSpec spec;
  return ComputeHandleSpecs(state, specs) ||
         ComputeSectionHandleSpec(state, spec, nullptr);
}

bool HandlePreviewHandleInput(AppState& state, const Ray& ray, bool clicked,
                              bool down, bool released) {
  auto& h = state.handleDrag;

  // A section drag: the same ray-axis projection, writing the section's
  // offset instead of a feature field.
  if (h.active && h.sectionId != 0) {
    AppState::SectionView* sec = FindSection(state, h.sectionId);
    if (released || !down || sec == nullptr) {
      h.active = false;
      h.sectionId = 0;
      return true;
    }
    const float t =
        RayAxisParam(ray.origin, ray.direction, h.axisOrigin, h.axisDir);
    // Reversed sections drag with the arrow, which points the other way.
    const float sign = sec->reverse ? -1.0f : 1.0f;
    sec->offset = static_cast<float>(h.startValue) + (t - h.startParam) * sign;
    return true;
  }

  if (h.active) {
    Feature* f = ResolvePendingFeature(state);
    if (released || !down || !f || f->GetId() != h.featureId) {
      h.active = false; // value stays where the drag left it
      return true;
    }
    const auto param = static_cast<HandleParam>(h.param);
    const double factor = DragFactor(*f, param);
    double v = h.startValue;
    if (!h.rotate) {
      const float t =
          RayAxisParam(ray.origin, ray.direction, h.axisOrigin, h.axisDir);
      v = h.startValue + (t - h.startParam) * factor;
    } else {
      float ang;
      if (!RotateAngle(ray.origin, ray.direction, h.axisOrigin, h.axisDir, ang))
        return true;
      v = h.startValue + glm::degrees(ang - h.startParam) * factor;
    }
    // Snap to the round value unless Ctrl says free — the app's one drag
    // polarity (ui/DragMath.h). This used to be the OTHER way round here,
    // Ctrl quantizing to 0.5/5° while the default ran free, so the same
    // modifier meant opposite things on a part tab and an assembly tab.
    // The capture window is what makes snap-by-default safe: a value that is
    // not near a round one is left exactly where the drag put it.
    const bool deg = param == HandleParam::Angle || param == HandleParam::Twist;
    const bool snap = !ImGui::GetIO().KeyCtrl;
    v = deg ? SnapAngleDeg(v, snap) : SnapLength(v, snap);
    // ...and whatever survives that still reads at display precision.
    const double quantum = deg ? 0.1 : 0.001;
    v = std::round(v / quantum) * quantum;
    ApplyHandleValue(state, *f, param, v);
    return true;
  }

  if (!clicked)
    return false;
  std::vector<HandleSpec> specs;
  uint64_t sectionId = 0;
  if (!ComputeHandleSpecs(state, specs)) {
    // No feature handle: the section arrow gets the click instead.
    HandleSpec spec;
    if (!ComputeSectionHandleSpec(state, spec, &sectionId))
      return false;
    specs.push_back(spec);
  }
  const float scale = GizmoScale(state);
  const float grab = 0.14f * scale, L = 0.9f * scale;

  // The shared hit test over the same records hover uses, so the handle a
  // click takes is the one that was highlighted.
  std::vector<ViewportHandle> records;
  for (const HandleSpec& spec : specs) {
    ViewportHandle vh;
    vh.kind = spec.rotate ? HandleKind::Rotate : HandleKind::Linear;
    vh.origin = spec.anchor;
    vh.dir = spec.dir;
    vh.length = spec.rotate ? spec.arcRadius : L;
    records.push_back(vh);
  }
  const int hit = HitTestHandles(records, ray, grab);
  if (hit < 0)
    return false; // let selection and armed tools consume a missed click
  const HandleSpec& spec = specs[static_cast<size_t>(hit)];
  float startParam = 0.0f;
  if (!spec.rotate) {
    startParam = RayAxisParam(ray.origin, ray.direction, spec.anchor, spec.dir);
  } else if (!RotateAngle(ray.origin, ray.direction, spec.anchor, spec.dir,
                          startParam)) {
    return false;
  }

  h.active = true;
  h.rotate = spec.rotate;
  h.axisOrigin = spec.anchor;
  h.axisDir = spec.dir;
  h.startParam = startParam;
  h.sectionId = sectionId;
  h.param = static_cast<int>(spec.param);
  if (sectionId != 0) {
    h.featureId = 0;
    h.startValue = spec.value; // the section's offset; no document involved
  } else {
    Feature* f = ResolvePendingFeature(state);
    h.featureId = f->GetId();
    h.startValue = CurrentHandleValue(*f, spec.param);
  }
  return true; // consumed: don't also run picking on this click
}

void AbortPreviewHandleDrag(AppState& state) {
  auto& h = state.handleDrag;
  if (!h.active)
    return;
  if (Feature* f = ResolvePendingFeature(state))
    if (f->GetId() == h.featureId)
      ApplyHandleValue(state, *f, static_cast<HandleParam>(h.param),
                       h.startValue);
  h.active = false;
}

} // namespace ui
