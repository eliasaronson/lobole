#include "ui/AppActions.h"

#include "CADMeshConverter.h"
#include "Camera.h"
#include "FaceSelector.h"
#include "Logger.h"
#include "MeshRenderer.h"
#include "OutlineRenderer.h"
#include "SketchRenderer.h"
#include "core/Feature.h"
#include "core/SketchFeature.h"
#include "core/SketchProfile.h"

#include <cfloat>
#include <filesystem>

namespace ui {

namespace {

// Sketch line color; the active sketch is drawn brighter.
constexpr glm::vec3 kSketchColor{0.40f, 0.72f, 1.00f};
constexpr glm::vec3 kActiveSketchColor{0.55f, 0.85f, 1.00f};

Vertex3D LineVertex(const glm::vec3& pos, const glm::vec3& color) {
  Vertex3D v;
  v.position = pos;
  v.normal = glm::vec3(0.0f);
  v.color = color;
  return v;
}

// Appends a sketch feature's entities as world-space line segments.
void AppendSketchLines(const SketchFeature& sk, const glm::vec3& color,
                       std::vector<Vertex3D>& out) {
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

} // namespace

// Builds translucent quads for the three default planes (Front/Top/Right),
// tinted by axis; the hovered plane (0/1/2, -1 = none) is brighter.
std::vector<Vertex3D> BuildPlaneQuads(bool show, int hoveredPlane) {
  std::vector<Vertex3D> quads;
  if (!show)
    return quads;

  const float s = 3.0f; // half-extent
  struct PlaneDef {
    glm::vec3 u, v;
    glm::vec3 color;
  };
  const PlaneDef defs[3] = {
      {{1, 0, 0}, {0, 1, 0}, {0.35f, 0.45f, 0.75f}}, // Front (XY)
      {{1, 0, 0}, {0, 0, 1}, {0.40f, 0.65f, 0.45f}}, // Top (XZ)
      {{0, 1, 0}, {0, 0, 1}, {0.70f, 0.40f, 0.42f}}, // Right (YZ)
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

void AppendPlaneQuad(const SketchPlane& plane, const glm::vec3& color, float half,
                     std::vector<Vertex3D>& quads) {
  glm::vec3 o = plane.GetOrigin();
  glm::vec3 u = plane.GetUAxis() * half;
  glm::vec3 v = plane.GetVAxis() * half;
  glm::vec3 a = o - u - v, b = o + u - v, c = o + u + v, d = o - u + v;
  for (const glm::vec3& p : {a, b, c, a, c, d}) {
    quads.push_back(LineVertex(p, color));
  }
}

void AppendSketchGrid(const SketchPlane& plane, std::vector<Vertex3D>& lines) {
  const int half = 5;      // grid extends -5..5 in plane units
  const float step = 1.0f; // one grid cell per unit
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

void ReplaceDocument(AppState& state, std::unique_ptr<Document> doc) {
  state.document = std::move(doc);
  state.commands = std::make_unique<CommandStack>();
  state.selectedFeature = -1;
  // Force a resync even if the version counters happen to collide
  state.syncedDocumentVersion = UINT64_MAX;
}

void FitCameraToDocument(AppState& state) {
  if (!state.document || !state.camera || state.document->GetBodies().empty())
    return;

  glm::vec3 min(FLT_MAX), max(-FLT_MAX);
  for (const auto& body : state.document->GetBodies()) {
    glm::vec3 bmin, bmax;
    body.shape.GetBoundingBox(bmin, bmax);
    min = glm::min(min, bmin);
    max = glm::max(max, bmax);
  }
  const glm::vec3 center = (min + max) * 0.5f;
  const float radius = glm::length(max - min) * 0.5f;
  state.camera->SetTarget(center);
  state.camera->SetDistance(std::max(radius * 2.5f, 1.0f));
}

void SyncDocumentRenderers(AppState& state) {
  if (!state.document || !state.meshRenderer)
    return;
  if (state.document->Version() == state.syncedDocumentVersion)
    return;
  state.syncedDocumentVersion = state.document->Version();

  for (const auto& m : state.documentMeshes) {
    state.meshRenderer->RemoveMesh(m.meshId);
    if (state.outlineRenderer)
      state.outlineRenderer->RemoveMesh(m.outlineId);
    if (state.faceSelector)
      state.faceSelector->RemoveMesh(m.meshId);
  }
  state.documentMeshes.clear();

  // Muted, theme-matched palette cycled per body so parts read apart
  static const glm::vec3 kBodyPalette[] = {
      {0.58f, 0.66f, 0.78f}, // steel blue
      {0.80f, 0.62f, 0.42f}, // amber
      {0.55f, 0.74f, 0.62f}, // sage
      {0.76f, 0.58f, 0.70f}, // mauve
      {0.72f, 0.72f, 0.55f}, // olive
  };
  size_t bodyIndex = 0;
  for (const auto& body : state.document->GetBodies()) {
    CADShape::Mesh cadMesh = body.shape.GenerateMesh(0.1);
    if (cadMesh.vertices.empty())
      continue;

    std::vector<Vertex3D> vertices = CADMeshConverter::ConvertCADMeshToVertices(
        cadMesh, kBodyPalette[bodyIndex++ % std::size(kBodyPalette)]);
    uint32_t meshId = state.meshRenderer->AddMesh(vertices, cadMesh.indices);
    uint32_t outlineId = 0;
    if (state.outlineRenderer)
      outlineId = state.outlineRenderer->AddMesh(vertices, cadMesh.indices);
    if (state.faceSelector)
      state.faceSelector->AddMesh(meshId, vertices, cadMesh.indices,
                                  cadMesh.triangleFaces);
    state.documentMeshes.push_back({meshId, outlineId});
  }

  // Rebuild static sketch line geometry (the active sketch is highlighted in
  // the per-frame overlay pass; here everything uses the base color).
  if (state.sketchRenderer) {
    std::vector<Vertex3D> lines;
    for (const auto& feature : state.document->GetFeatures()) {
      if (auto* sk = dynamic_cast<const SketchFeature*>(feature.get())) {
        AppendSketchLines(*sk, kSketchColor, lines);
      }
    }
    state.sketchRenderer->SetStaticLines(std::move(lines));
  }

  LOG_INFO("Document synced: {} features, {} bodies",
           state.document->FeatureCount(), state.document->GetBodies().size());
}

std::vector<Vertex3D> BuildActiveSketchOverlay(AppState& state,
                                               uint64_t activeSketchId) {
  std::vector<Vertex3D> lines;
  if (activeSketchId == 0 || !state.document)
    return lines;
  if (auto* sk = dynamic_cast<const SketchFeature*>(
          state.document->FindFeatureById(activeSketchId))) {
    AppendSketchLines(*sk, kActiveSketchColor, lines);
  }
  return lines;
}

void LoadSTEPFile(AppState& state, const std::string& filename) {
  if (!state.document || !state.commands)
    return;

  const std::string name = std::filesystem::path(filename).stem().string();
  auto feature = std::make_unique<ImportFeature>(name, filename);
  state.commands->Execute(
      std::make_unique<AddFeatureCommand>(std::move(feature)),
      *state.document);

  if (state.document->GetBodies().empty()) {
    LOG_ERROR("Import of '{}' produced no bodies", filename);
  } else {
    FitCameraToDocument(state);
  }
}

} // namespace ui
