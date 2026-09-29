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

// Turning a screen position into a sketch entity: the projections both ways,
// and the candidate ranking that decides what a click at 4 px landed on.
// Split out of SketchInteraction.cpp
// (docs/architecture.md#the-ui-actions-layer). The 843-line viewport router
// stays there; these are the blocks it calls into, and
// SketchInteractionInternal.h is that interface written down.

namespace ui {

glm::vec2 WorldToScreen(AppState& state, const glm::vec3& world) {
  glm::vec4 clip = state.camera->GetProjectionMatrix() *
                   state.camera->GetViewMatrix() * glm::vec4(world, 1.0f);
  glm::vec3 ndc = glm::vec3(clip) / clip.w;
  return state.viewportPos +
         glm::vec2((ndc.x + 1.0f) * 0.5f * state.viewportSize.x,
                   (ndc.y + 1.0f) * 0.5f * state.viewportSize.y);
}

glm::vec2 SketchUVToScreen(AppState& state, const SketchPlane& plane,
                           glm::vec2 uv) {
  return WorldToScreen(state, plane.PlaneToWorld(uv));
}

// At `ui` scope, not file-static: SketchTools.cpp sets up a body drag from
// the same complete set of solver points, and two copies of "which points
// does this entity expose" would drift. Declared in
// SketchInteractionInternal.h.
// Endpoints exposed by an entity for picking/constraints.
std::vector<std::pair<uint8_t, glm::vec2>>
EntityPoints(const sketch::Entity& e) {
  std::vector<std::pair<uint8_t, glm::vec2>> pts;
  if (auto* p = std::get_if<sketch::Point>(&e)) {
    pts = {{0, p->p}};
  } else if (auto* l = std::get_if<sketch::Line>(&e)) {
    pts = {{0, l->a}, {1, l->b}};
  } else if (auto* c = std::get_if<sketch::Circle>(&e)) {
    pts = {{0, c->center}};
  } else if (auto* a = std::get_if<sketch::Arc>(&e)) {
    pts = {{0, a->center},
           {1, sketch::ArcPointUV(*a, 1)},
           {2, sketch::ArcPointUV(*a, 2)}};
  } else if (auto* el = std::get_if<sketch::Ellipse>(&e)) {
    pts = {{0, el->center}};
  } else if (auto* ea = std::get_if<sketch::EllipseArc>(&e)) {
    pts = {{0, ea->center},
           {1, sketch::EllipseArcPointUV(*ea, 1)},
           {2, sketch::EllipseArcPointUV(*ea, 2)}};
  } else if (auto* bs = std::get_if<sketch::BSpline>(&e)) {
    const size_t nPoles = std::min<size_t>(bs->poles.size(), 256);
    for (size_t k = 0; k < nPoles; k++)
      pts.push_back({static_cast<uint8_t>(k), bs->poles[k]});
  } else if (std::holds_alternative<sketch::Text>(e)) {
    // The anchor, then the box corners (docs/sketching/README.md#text).
    for (uint8_t k = 0; k <= 4; k++)
      pts.push_back({k, sketch::EntityPointUV(e, k)});
  }
  return pts;
}

SketchPick PickSketchPoint(AppState& state, const SketchFeature& sketch,
                           glm::vec2 mousePos, float tolPx) {
  SketchPick best;
  float bestDist = tolPx;
  for (uint32_t i = 0; i < sketch.entities.size(); i++) {
    const uint32_t id = sketch::EntityId(sketch.entities[i]);
    for (auto& [which, uv] : EntityPoints(sketch.entities[i])) {
      glm::vec2 screen = SketchUVToScreen(state, sketch.plane, uv);
      float d = glm::length(screen - mousePos);
      if (d < bestDist) {
        bestDist = d;
        best.hit = true;
        best.ref = {id, which};
      }
    }
  }
  return best;
}

namespace {
// Distance in pixels from p to segment [a,b].
float DistToSegment(glm::vec2 p, glm::vec2 a, glm::vec2 b) {
  glm::vec2 ab = b - a;
  float len2 = glm::dot(ab, ab);
  float t =
      len2 > 1e-6f ? glm::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
  return glm::length(p - (a + t * ab));
}
} // namespace

std::vector<SketchPick> PickSketchCandidates(AppState& state,
                                             const SketchFeature& sketch,
                                             glm::vec2 mousePos, float tolPx) {
  using Ranked = std::pair<float, SketchPick>;
  std::vector<Ranked> points, bodies, axes;

  // Endpoints, within a slightly LARGER tolerance than bodies so a point is
  // never shadowed by the line it terminates.
  const float pointTolPx = tolPx + 4.0f;
  for (uint32_t i = 0; i < sketch.entities.size(); i++) {
    const uint32_t id = sketch::EntityId(sketch.entities[i]);
    for (auto& [which, uv] : EntityPoints(sketch.entities[i])) {
      const float d =
          glm::length(SketchUVToScreen(state, sketch.plane, uv) - mousePos);
      if (d < pointTolPx)
        points.push_back({d, SketchPick{true, false, {id, which}}});
    }
  }

  // Entity bodies (line segment / circle rim / arc polyline).
  for (const auto& e : sketch.entities) {
    const uint32_t id = sketch::EntityId(e);
    float d = 1e30f;
    if (auto* l = std::get_if<sketch::Line>(&e)) {
      d = DistToSegment(mousePos, SketchUVToScreen(state, sketch.plane, l->a),
                        SketchUVToScreen(state, sketch.plane, l->b));
    } else if (auto* c = std::get_if<sketch::Circle>(&e)) {
      glm::vec2 center = SketchUVToScreen(state, sketch.plane, c->center);
      glm::vec2 rim = SketchUVToScreen(
          state, sketch.plane,
          c->center + glm::vec2(static_cast<float>(c->radius), 0.0f));
      float rPx = glm::length(rim - center);
      d = std::abs(glm::length(mousePos - center) - rPx);
    } else if (std::holds_alternative<sketch::Arc>(e)) {
      // Distance to the tessellated arc polyline — respects the sweep (only
      // the drawn span is hit) and works under any projection.
      std::vector<glm::vec2> uv = sketch::TessellateEntity(e);
      for (size_t k = 0; k + 1 < uv.size(); k++)
        d = std::min(
            d, DistToSegment(mousePos,
                             SketchUVToScreen(state, sketch.plane, uv[k]),
                             SketchUVToScreen(state, sketch.plane, uv[k + 1])));
    } else if (std::holds_alternative<sketch::Text>(e)) {
      // Any glyph's outline (or stroke) picks the whole label.
      for (const auto& uv : sketch::TessellateEntityPolylines(e))
        for (size_t k = 0; k + 1 < uv.size(); k++)
          d = std::min(
              d, DistToSegment(
                     mousePos, SketchUVToScreen(state, sketch.plane, uv[k]),
                     SketchUVToScreen(state, sketch.plane, uv[k + 1])));
    }
    if (d < tolPx)
      bodies.push_back({d, SketchPick{true, true, {id, sketch::kWholeEntity}}});
  }

  // Datum axes are pickable (e.g. as a mirror axis) but rank below real
  // geometry, so a click only takes one when nothing user-drawn is in range
  // (or cycling walked past everything else).
  for (uint32_t axisId : {sketch::kAxisX, sketch::kAxisY}) {
    const auto ep = sketch::DatumAxisEndpoints(axisId);
    const glm::vec2 dir = ep.second - ep.first;
    const glm::vec2 a = ep.first - dir * 100.0f, b = ep.first + dir * 100.0f;
    const float d =
        DistToSegment(mousePos, SketchUVToScreen(state, sketch.plane, a),
                      SketchUVToScreen(state, sketch.plane, b));
    if (d < tolPx)
      axes.push_back({d, SketchPick{true, true, {axisId, 0}}});
  }

  const auto byDist = [](const Ranked& x, const Ranked& y) {
    return x.first < y.first;
  };
  std::sort(points.begin(), points.end(), byDist);
  std::sort(bodies.begin(), bodies.end(), byDist);
  std::sort(axes.begin(), axes.end(), byDist);

  std::vector<SketchPick> out;
  out.reserve(points.size() + bodies.size() + axes.size());
  for (const auto& [d, p] : points)
    out.push_back(p);
  for (const auto& [d, p] : bodies)
    out.push_back(p);
  for (const auto& [d, p] : axes)
    out.push_back(p);
  return out;
}

SketchPick PickSketchEntity(AppState& state, const SketchFeature& sketch,
                            glm::vec2 mousePos, float tolPx) {
  const std::vector<SketchPick> candidates =
      PickSketchCandidates(state, sketch, mousePos, tolPx);
  return candidates.empty() ? SketchPick{} : candidates.front();
}

} // namespace ui
