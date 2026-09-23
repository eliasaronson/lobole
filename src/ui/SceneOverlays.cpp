#include "ui/SceneOverlays.h"
#include "ui/RendererSync.h"

#include "core/Units.h"
#include "ui/AppActionsInternal.h" // SharedBody / PartRenderCacheStore
#include "ui/CamPanel.h"           // RefreshCamToolpaths (job undo)
#include "ui/CamTool.h"            // RunCamVerification (job undo)
#include "ui/JobActions.h"

#include "imgui.h" // IsAnyItemActive (the undo guard coalesces a drag)         // JobBodies / JobRenderVersion (a job tab)
#include <chrono>
#include <utility>

#include "cad/CADMaterials.h"
#include "cad/CADMeshConverter.h"
#include "cad/FaceSelector.h"
#include "common/Logger.h"
#include "core/AssemblySolve.h"
#include "core/Command.h"
#include "core/ElementHistory.h" // topo::ResolveEdgeOnBody (blend edge picks)
#include "core/Expression.h"
#include "core/Feature.h"
#include "core/MassProps.h"
#include "core/Measure.h"
#include "core/drawing/DrawingDimension.h"
#include "core/drawing/DrawingExport.h"
#include "core/drawing/DrawingProjection.h"
#include "core/drawing/DrawingRender.h"
#include "core/features/BooleanFeature.h"
#include "core/features/CurveFeature.h"
#include "core/features/DatumAxisFeature.h"
#include "core/features/DatumPlaneFeature.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/FilletFeature.h"
#include "core/features/FrameFeature.h"
#include "core/fem/FemTopo.h"
#include "core/io/Autosaver.h"
#include "core/io/GitVault.h"
#include "core/io/ProjectStamp.h"
#include "core/io/RecentProjects.h"
#include "core/io/Serialization.h"
#include "render/Camera.h"
#include "render/FieldRenderer.h"
#include "render/IBLTextures.h"
#include "render/MeshRenderer.h"
#include "render/OutlineRenderer.h"
#include "render/RayTracingRenderer.h"
#include "render/SketchRenderer.h"

#include "core/features/DraftFeature.h"
#include "core/features/LoftFeature.h"
#include "core/features/PatternFeature.h"
#include "core/features/PipeFeature.h"
#include "core/features/RefineFeature.h"
#include "core/features/RevolveFeature.h"
#include "core/features/RibFeature.h"
#include "core/features/ShellFeature.h"
#include "core/features/SketchFeature.h"
#include "core/features/SplitFeature.h"
#include "core/features/SweepFeature.h"
#include "core/features/ThreadFeature.h"
#include "core/sketch/SketchProfile.h"
#include "ui/AssemblyActions.h"
#include "ui/CamPlayback.h"   // SyncCamPlayback (SyncRenderersToDocument)
#include "ui/FastenerTool.h"  // SyncFastenerGhost (SyncRenderersToDocument)
#include "ui/FeatureEditor.h" // PickSlot (thread face slot)
#include "ui/Placement.h"
#include "ui/PreviewFeature.h"
#include "ui/SelectionPipeline.h" // ResolveFaceItem (Hole select-first)
#include "ui/SimulationActions.h"
#include "ui/SketchInteraction.h" // SketchUVToScreen
#include <nlohmann/json.hpp>

#include "thirdparty/stb_image.h" // stbi_info_from_memory (impl in IBL.cpp)

#include <glm/gtc/matrix_transform.hpp> // glm::translate
#include <glm/gtc/quaternion.hpp>       // glm::mat4_cast

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <set>
#include <unordered_map>

// The line geometry the 3D scene draws that is not a body: the datum planes
// and their grid, the origin gizmo, the centre-of-mass marker, datum axes,
// frames, and 3D curves.
//
// Overlays.cpp is the neighbouring half and says where the line was drawn:
// text and pills go on ImGui's foreground draw list, and THIS is the vertex
// geometry the renderers consume, which needs no ImGui at all.
// Split out of AppActions.cpp (docs/architecture.md#the-ui-actions-layer).
// AppActions.h still
// declares everything, so no caller changed.

namespace ui {

namespace {

constexpr glm::vec3 kActiveSketchColor{0.55f, 0.85f, 1.00f};
} // namespace

// Hoisted to `ui` scope by the stage-3 split: SketchOverlay.cpp draws the
// ACTIVE sketch and this file draws every other one, and both want the same
// entity-to-line-vertices walk. Declared in AppActionsInternal.h.

// LineVertex lived here as a second copy of the inline one in
// AppActionsInternal.h — harmless while this TU did not include that header,
// ambiguous the moment it did. The header's is the one every other
// AppActions* TU already uses.

// Appends a sketch feature's entities as world-space line segments.
void AppendSketchLines(const SketchFeature& sk, const glm::vec3& color,
                       std::vector<LineVertex3D>& out) {
  for (const auto& entity : sk.entities) {
    // Points render as a small world-space cross
    if (std::holds_alternative<sketch::Point>(entity)) {
      glm::vec2 uv = std::get<sketch::Point>(entity).p;
      glm::vec3 c = sk.plane.PlaneToWorld(uv);
      glm::vec3 u = sk.plane.GetUAxis() * 0.05f;
      glm::vec3 v = sk.plane.GetVAxis() * 0.05f;
      out.push_back(LineVertex(c - u, color));
      out.push_back(LineVertex(c + u, color));
      out.push_back(LineVertex(c - v, color));
      out.push_back(LineVertex(c + v, color));
      continue;
    }
    std::vector<glm::vec2> poly = sketch::TessellateEntity(entity);
    for (size_t i = 0; i + 1 < poly.size(); i++) {
      out.push_back(LineVertex(sk.plane.PlaneToWorld(poly[i]), color));
      out.push_back(LineVertex(sk.plane.PlaneToWorld(poly[i + 1]), color));
    }
  }
}

namespace {
// How far the document's own geometry gets from the world origin. The
// bounding box behind this is an OCC pass per body (BRepBndLib, no
// triangulation), and reference geometry asks for it every frame the planes
// are up, so the answer is cached against the document's version.
float DocumentReach(AppState& state) {
  const Document* doc = state.document.get();
  if (!doc)
    return 0.0f;
  if (doc == state.sceneReachDoc && doc->Version() == state.sceneReachVersion)
    return state.sceneReach;
  float reach = 0.0f;
  for (const auto& body : doc->GetBodies()) {
    glm::vec3 mn, mx;
    body.shape.GetBoundingBox(mn, mx);
    for (int i = 0; i < 3; i++)
      reach = std::max({reach, std::abs(mn[i]), std::abs(mx[i])});
  }
  state.sceneReachDoc = doc;
  state.sceneReachVersion = doc->Version();
  state.sceneReach = reach;
  return reach;
}

// The next 1-2-5 step at or above `raw` — the decade ladder a ruler uses, so
// a cell is always a number you can count in.
float NiceStep(float raw) {
  const float mag =
      std::pow(10.0f, std::floor(std::log10(std::max(raw, 1e-4f))));
  const float n = raw / mag;
  return mag * (n <= 1.0f ? 1.0f : n <= 2.0f ? 2.0f : n <= 5.0f ? 5.0f : 10.0f);
}
} // namespace

float ScenePlaneHalfExtent(AppState& state) {
  // Just past the geometry: a plane you sketch on wants to reach the part's
  // silhouette, not stop inside it.
  return std::max(kDefaultPlaneHalfExtent, 1.1f * DocumentReach(state));
}

SketchGridMetrics SketchGridFor(AppState& state, const SketchFeature& sketch) {
  float reach = 0.0f;
  for (const sketch::Entity& e : sketch.entities)
    for (const glm::vec2& p : sketch::TessellateEntity(e))
      reach = std::max({reach, std::abs(p.x), std::abs(p.y)});
  SketchGridMetrics g;
  g.half = std::max({kSketchBackgroundHalfExtent, 1.25f * reach,
                     ScenePlaneHalfExtent(state)});
  // ~50 cells from the origin out, whatever the scale: the 50 mm default
  // background keeps exactly its old 1 mm cells.
  g.step = NiceStep(g.half / 50.0f);
  return g;
}

// Builds translucent quads for the three default planes (Front/Top/Right),
// tinted by axis; the hovered plane (0/1/2, -1 = none) is brighter.
std::vector<LineVertex3D> BuildPlaneQuads(bool show, int hoveredPlane,
                                          float half) {
  std::vector<LineVertex3D> quads;
  if (!show)
    return quads;

  const float s = half;
  struct PlaneDef {
    glm::vec3 u, v;
    glm::vec3 color;
  };
  const PlaneDef defs[3] = {
      {{1, 0, 0}, {0, 1, 0}, {0.35f, 0.45f, 0.75f}}, // XY (Top)
      {{1, 0, 0}, {0, 0, 1}, {0.40f, 0.65f, 0.45f}}, // XZ (Front)
      {{0, 1, 0}, {0, 0, 1}, {0.70f, 0.40f, 0.42f}}, // YZ (Right)
  };
  for (int i = 0; i < 3; i++) {
    glm::vec3 color = defs[i].color * (i == hoveredPlane ? 2.0f : 1.0f);
    glm::vec3 a = -defs[i].u * s - defs[i].v * s;
    glm::vec3 b = defs[i].u * s - defs[i].v * s;
    glm::vec3 c = defs[i].u * s + defs[i].v * s;
    glm::vec3 d = -defs[i].u * s + defs[i].v * s;
    for (const glm::vec3& p : {a, b, c, a, c, d}) {
      quads.push_back(LineVertex(p, color));
    }
  }
  return quads;
}

void AppendPlaneQuad(const SketchPlane& plane, const glm::vec3& color,
                     float half, std::vector<LineVertex3D>& quads) {
  glm::vec3 o = plane.GetOrigin();
  glm::vec3 u = plane.GetUAxis() * half;
  glm::vec3 v = plane.GetVAxis() * half;
  glm::vec3 a = o - u - v, b = o + u - v, c = o + u + v, d = o - u + v;
  for (const glm::vec3& p : {a, b, c, a, c, d}) {
    quads.push_back(LineVertex(p, color));
  }
}

void AppendSketchGrid(const SketchPlane& plane, const SketchGridMetrics& grid,
                      std::vector<LineVertex3D>& lines) {
  const float step = grid.step;
  const int half = std::max(1, static_cast<int>(std::lround(grid.half / step)));
  const glm::vec3 minor(0.30f, 0.34f, 0.42f);
  const glm::vec3 major(0.45f, 0.52f, 0.65f);
  const glm::vec3 uAxisColor(0.85f, 0.40f, 0.42f); // red = U (like X)
  const glm::vec3 vAxisColor(0.45f, 0.80f, 0.50f); // green = V (like Y)

  auto world = [&](float u, float v) {
    return plane.PlaneToWorld(glm::vec2(u, v));
  };
  const float extent = half * step;
  for (int i = -half; i <= half; i++) {
    const float t = i * step;
    // Lines parallel to V (constant U) and parallel to U (constant V)
    const glm::vec3 cU = (i == 0) ? vAxisColor : (i % 5 == 0 ? major : minor);
    lines.push_back(LineVertex(world(t, -extent), cU));
    lines.push_back(LineVertex(world(t, extent), cU));
    const glm::vec3 cV = (i == 0) ? uAxisColor : (i % 5 == 0 ? major : minor);
    lines.push_back(LineVertex(world(-extent, t), cV));
    lines.push_back(LineVertex(world(extent, t), cV));
  }
}

void AppendOriginGizmo(const AppState& state,
                       std::vector<LineVertex3D>& lines) {
  // 0.065 of the orbit distance is the 1.5 mm arrow the empty workspace has
  // always drawn (it opens at ~22 mm out), and it stays that arrow on a part
  // a thousand times the size.
  const float L = state.camera
                      ? std::max(0.065f * state.camera->GetDistance(), 0.1f)
                      : 1.5f;
  const float h = L * 0.147f; // arrowhead, in the same proportion as before
  struct Axis {
    glm::vec3 dir, side, color;
  };
  const Axis axes[3] = {
      {{1, 0, 0}, {0, 1, 0}, {0.90f, 0.32f, 0.32f}}, // X red
      {{0, 1, 0}, {1, 0, 0}, {0.35f, 0.82f, 0.38f}}, // Y green
      {{0, 0, 1}, {1, 0, 0}, {0.38f, 0.55f, 0.95f}}, // Z blue
  };
  for (const Axis& a : axes) {
    const glm::vec3 tip = a.dir * L;
    lines.push_back(LineVertex(glm::vec3(0.0f), a.color));
    lines.push_back(LineVertex(tip, a.color));
    // Two-line arrowhead in the axis/side plane
    lines.push_back(LineVertex(tip, a.color));
    lines.push_back(LineVertex(tip - a.dir * h + a.side * h, a.color));
    lines.push_back(LineVertex(tip, a.color));
    lines.push_back(LineVertex(tip - a.dir * h - a.side * h, a.color));
  }
}

void AppendCogMarker(const AppState& state, std::vector<LineVertex3D>& lines) {
  if (!state.showCog)
    return;
  // A small screen-stable 3-axis cross at the center of mass. Drawn in the
  // no-depth gizmo channel so it stays visible inside solid geometry.
  const float L =
      state.camera ? std::max(0.05f * state.camera->GetDistance(), 0.1f) : 1.0f;
  const glm::vec3 c = state.cogWorld;
  const glm::vec3 col(0.98f, 0.80f, 0.20f); // amber, distinct from the axes
  const glm::vec3 ax[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  for (const glm::vec3& a : ax) {
    lines.push_back(LineVertex(c - a * L, col));
    lines.push_back(LineVertex(c + a * L, col));
  }
}

// Whether datum quads should render this frame. Unlike base planes, datums are
// user-created reference geometry, so they follow showPlanes but do NOT
// auto-hide once bodies exist; always shown while picking a plane.
bool DatumPlanesVisible(const AppState& state) {
  return state.sketchEdit.pickingPlane || state.sketchEdit.IsEditing() ||
         state.showPlanes;
}

void AppendDatumPlaneQuads(AppState& state, std::vector<LineVertex3D>& quads) {
  if (!state.document || !DatumPlanesVisible(state))
    return;
  const glm::vec3 kDatumTint(0.85f, 0.62f, 0.28f); // amber, distinct from bases
  // A datum quad has always been the smaller, secondary reference (3 against
  // a base plane's 8); keep that ratio now that both are derived.
  const float kDatumHalfFraction = 3.0f / kDefaultPlaneHalfExtent;
  for (size_t i = 0; i < state.document->FeatureCount(); i++) {
    auto* dp = dynamic_cast<DatumPlaneFeature*>(state.document->GetFeature(i));
    if (!dp)
      continue;
    const bool emphasized = dp->GetId() == state.sketchEdit.hoveredDatumId ||
                            dp->GetId() == state.pendingFeatureId ||
                            dp->GetId() == SelectedFeatureId(state);
    AppendPlaneQuad(dp->OutputPlane(), kDatumTint * (emphasized ? 1.8f : 1.0f),
                    kDatumHalfFraction * ScenePlaneHalfExtent(state), quads);
  }
}

void AppendDatumAxisLines(AppState& state, std::vector<LineVertex3D>& lines) {
  if (!state.document || !DatumPlanesVisible(state))
    return;
  const glm::vec3 kAxisTint(0.90f, 0.75f, 0.35f); // brighter amber than planes
  const float half = 3.0f;                        // half the drawn length
  for (size_t i = 0; i < state.document->FeatureCount(); i++) {
    auto* da = dynamic_cast<DatumAxisFeature*>(state.document->GetFeature(i));
    if (!da)
      continue;
    const bool emphasized = da->GetId() == state.pendingFeatureId ||
                            da->GetId() == SelectedFeatureId(state);
    const glm::vec3 c = kAxisTint * (emphasized ? 1.4f : 1.0f);
    const glm::vec3 o = da->Origin();
    const glm::vec3 d = da->Direction();
    lines.push_back(LineVertex(o - d * half, c));
    lines.push_back(LineVertex(o + d * half, c));
  }
}

float FrameTriadLength(AppState& state) {
  // Half a datum quad's half-extent: a triad is a point of reference, not a
  // surface, so it reads smaller — and it scales with the scene the way the
  // datum quads do (3 against a base plane's 8, then the document's reach).
  return 0.5f * (3.0f / kDefaultPlaneHalfExtent) * ScenePlaneHalfExtent(state);
}

void AppendFrameLines(AppState& state, std::vector<LineVertex3D>& lines) {
  if (!state.document || !DatumPlanesVisible(state))
    return;
  const float len = FrameTriadLength(state);
  for (size_t i = 0; i < state.document->FeatureCount(); i++) {
    auto* fr = dynamic_cast<FrameFeature*>(state.document->GetFeature(i));
    if (!fr || !fr->GetError().empty())
      continue;
    const bool emphasized = fr->GetId() == state.pendingFeatureId ||
                            fr->GetId() == SelectedFeatureId(state);
    const float k = emphasized ? 1.4f : 1.0f;
    const glm::vec3 o = fr->Origin();
    const glm::vec3 axes[3] = {fr->XAxis(), fr->YAxis(), fr->ZAxis()};
    const glm::vec3 tints[3] = {glm::vec3(0.90f, 0.30f, 0.30f),
                                glm::vec3(0.35f, 0.85f, 0.35f),
                                glm::vec3(0.35f, 0.55f, 1.00f)};
    for (int a = 0; a < 3; a++) {
      lines.push_back(LineVertex(o, tints[a] * k));
      lines.push_back(LineVertex(o + axes[a] * len, tints[a] * k));
    }
  }
}

void AppendCurve3DLines(AppState& state, std::vector<LineVertex3D>& lines) {
  if (!state.document)
    return;
  const glm::vec3 kCurveTint(0.45f, 0.85f, 1.0f); // cyan, distinct from datums
  // A curve another feature is BUILT FROM is construction, not result. Drawn
  // regardless, six helices swept into a barley twist stayed on screen over
  // the part they had already become -- and ran far past it, because each
  // one starts a fraction of a pitch early to phase the flute it cuts. The
  // same auto-hide a plane gets once the part has geometry, for the same
  // reason: the thing you are looking at is the part.
  //
  // `VisitFeatureRefs` IS the dependency graph (it is pure virtual, and
  // test_conventions fails a feature that does not implement it), so this
  // cannot drift out of step with what actually consumes a curve.
  std::set<uint64_t> consumed;
  for (size_t i = 0; i < state.document->FeatureCount(); i++)
    state.document->GetFeature(i)->VisitFeatureRefs(
        [&consumed](uint64_t id) { consumed.insert(id); });
  for (size_t i = 0; i < state.document->FeatureCount(); i++) {
    auto* cv = dynamic_cast<CurveFeature*>(state.document->GetFeature(i));
    if (!cv)
      continue;
    // ...unless it is the one being worked on, which is the whole reason to
    // be able to see a curve at all.
    if (consumed.count(cv->GetId()) != 0 &&
        cv->GetId() != state.pendingFeatureId &&
        cv->GetId() != SelectedFeatureId(state))
      continue;
    const auto& poly = cv->Polyline();
    if (poly.size() < 2)
      continue;
    const bool emphasized = cv->GetId() == state.pendingFeatureId ||
                            cv->GetId() == SelectedFeatureId(state);
    const glm::vec3 c = kCurveTint * (emphasized ? 1.3f : 1.0f);
    for (size_t k = 1; k < poly.size(); k++) {
      lines.push_back(LineVertex(poly[k - 1], c));
      lines.push_back(LineVertex(poly[k], c));
    }
  }
}

bool EffectivePlaneVisibility(const AppState& state) {
  if (state.sketchEdit.pickingPlane || state.sketchEdit.IsEditing())
    return true;
  const bool hasBodies = state.document && !state.document->GetBodies().empty();
  return state.showPlanes && !hasBodies; // auto-hide once the part has geometry
}

} // namespace ui
