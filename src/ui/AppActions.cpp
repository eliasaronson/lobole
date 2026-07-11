#include "ui/AppActions.h"

#include "CADMaterials.h"
#include "CADMeshConverter.h"
#include "Camera.h"
#include "IBLTextures.h"
#include "RayTracingRenderer.h"
#include "FaceSelector.h"
#include "Logger.h"
#include "MeshRenderer.h"
#include "OutlineRenderer.h"
#include "SketchRenderer.h"
#include "core/BooleanFeature.h"
#include "core/Command.h"
#include "core/DatumAxisFeature.h"
#include "core/DatumPlaneFeature.h"
#include "core/ExtrudeFeature.h"
#include "core/Feature.h"
#include "core/FilletFeature.h"
#include "core/Serialization.h"
#include "core/PatternFeature.h"
#include "core/RevolveFeature.h"
#include "core/SketchFeature.h"
#include "core/SketchProfile.h"
#include "ui/PreviewFeature.h"
#include "ui/SketchInteraction.h" // SketchUVToScreen

#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <filesystem>
#include <cmath>
#include <set>

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

void AppendOriginGizmo(std::vector<Vertex3D>& lines) {
  const float L = 1.5f;  // axis length
  const float h = 0.22f; // arrowhead size
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

namespace {
// Placement offset so successive primitives don't overlap.
glm::vec3 NextSpot(const AppState& state) {
  const int n =
      state.document ? static_cast<int>(state.document->FeatureCount()) : 0;
  return glm::vec3(-4.0f + n * 2.5f, 0.0f, 0.0f);
}
std::string NextName(const AppState& state, const char* base) {
  const int n =
      state.document ? static_cast<int>(state.document->FeatureCount()) + 1 : 1;
  return std::string(base) + " " + std::to_string(n);
}
} // namespace

void CreateBox(AppState& state) {
  BeginPreviewFeature(state, std::make_unique<BoxFeature>(
                                 NextName(state, "Box"), NextSpot(state), 2.0,
                                 2.0, 2.0));
}

void CreateSphere(AppState& state) {
  BeginPreviewFeature(state, std::make_unique<SphereFeature>(
                                 NextName(state, "Sphere"), NextSpot(state),
                                 1.0));
}

void CreateCylinder(AppState& state) {
  BeginPreviewFeature(
      state, std::make_unique<CylinderFeature>(
                 NextName(state, "Cylinder"),
                 NextSpot(state) + glm::vec3(0.0f, -1.0f, 0.0f),
                 glm::vec3(0.0f, 1.0f, 0.0f), 0.8, 2.0));
}

void CreateDatumPlane(AppState& state) {
  auto dp = std::make_unique<DatumPlaneFeature>(NextName(state, "Plane"),
                                                DatumPlaneKind::Offset);
  dp->offset = 2.0; // scene scale (~2-unit boxes, half-extent-3 plane quads)
  // Build on the selected datum if one is selected, else the XY base plane.
  if (state.document && state.selectedFeature >= 0) {
    Feature* sel = state.document->GetFeature(
        static_cast<size_t>(state.selectedFeature));
    if (auto* parent = dynamic_cast<DatumPlaneFeature*>(sel))
      dp->parentId = parent->GetId();
  }
  BeginPreviewFeature(state, std::move(dp));
}

void CreateDatumAxis(AppState& state) {
  auto ax = std::make_unique<DatumAxisFeature>(NextName(state, "Axis"),
                                               DatumAxisKind::TwoPoints);
  ax->p0 = glm::vec3(0.0f);
  ax->p1 = glm::vec3(3.0f, 0.0f, 0.0f); // along X at scene scale
  BeginPreviewFeature(state, std::move(ax));
}

namespace {
// Does this feature create a body (vs. modify or produce none)?
bool ProducesBody(const Feature& f) {
  if (dynamic_cast<const BoxFeature*>(&f) ||
      dynamic_cast<const SphereFeature*>(&f) ||
      dynamic_cast<const CylinderFeature*>(&f) ||
      dynamic_cast<const ImportFeature*>(&f) ||
      dynamic_cast<const BooleanFeature*>(&f)) {
    return true;
  }
  if (auto* e = dynamic_cast<const ExtrudeFeature*>(&f)) {
    return e->mode == ExtrudeMode::NewBody;
  }
  if (auto* r = dynamic_cast<const RevolveFeature*>(&f)) {
    return r->mode == ExtrudeMode::NewBody;
  }
  return false;
}
} // namespace

std::vector<std::pair<uint64_t, std::string>> BodyProducers(const Document& doc,
                                                            size_t beforeIndex) {
  std::vector<std::pair<uint64_t, std::string>> out;
  const auto& features = doc.GetFeatures();
  for (size_t i = 0; i < beforeIndex && i < features.size(); i++) {
    if (ProducesBody(*features[i])) {
      out.emplace_back(features[i]->GetId(), features[i]->GetName());
    }
  }
  return out;
}

void CreateBoolean(AppState& state) {
  if (!state.document) {
    return;
  }
  auto producers = BodyProducers(*state.document, state.document->FeatureCount());
  if (producers.size() < 2) {
    return; // the menu gates this, but guard anyway
  }
  // Default: the two most-recent producers, older as Target so subtract reads
  // as "the earlier body minus the newer one".
  const uint64_t tool = producers.back().first;
  const uint64_t target = producers[producers.size() - 2].first;
  BeginPreviewFeature(state, std::make_unique<BooleanFeature>(
                                 NextName(state, "Boolean"), target, tool,
                                 BooleanOp::Union));
}

void CreateMirror(AppState& state) {
  if (!state.document) {
    return;
  }
  auto producers = BodyProducers(*state.document, state.document->FeatureCount());
  if (producers.empty()) {
    return;
  }
  auto mir = std::make_unique<PatternFeature>(NextName(state, "Mirror"),
                                              PatternKind::Mirror,
                                              producers.back().first);
  mir->basePlane = PlaneType::YZ; // reflect across x by default
  BeginPreviewFeature(state, std::move(mir));
}

void CreatePattern(AppState& state) {
  if (!state.document) {
    return;
  }
  auto producers = BodyProducers(*state.document, state.document->FeatureCount());
  if (producers.empty()) {
    return;
  }
  auto pat = std::make_unique<PatternFeature>(NextName(state, "Pattern"),
                                              PatternKind::Linear,
                                              producers.back().first);
  pat->axis.kind = AxisRef::Kind::X;
  pat->count = 3;
  pat->spacing = 3.0; // scene scale
  BeginPreviewFeature(state, std::move(pat));
}

namespace {
// Shared by CreateFillet/CreateChamfer: start the preview and arm edge
// picking so viewport clicks collect edges on the pending feature.
void CreateEdgeBlend(AppState& state, EdgeBlendOp op) {
  if (!state.document || state.document->GetBodies().empty()) {
    return; // the menu gates this, but guard anyway
  }
  const char* base = op == EdgeBlendOp::Chamfer ? "Chamfer" : "Fillet";
  const uint64_t id = BeginPreviewFeature(
      state, std::make_unique<FilletFeature>(NextName(state, base),
                                             std::vector<ElementRef>{}, 0.5,
                                             op));
  state.edgePick = {};
  state.edgePick.filletId = id;
}
} // namespace

void CreateFillet(AppState& state) {
  CreateEdgeBlend(state, EdgeBlendOp::Fillet);
}

void CreateChamfer(AppState& state) {
  CreateEdgeBlend(state, EdgeBlendOp::Chamfer);
}

void StartNewSketch(AppState& state) {
  state.sketchEdit = SketchEditState{};
  state.sketchEdit.pickingPlane = true;
}

// Whether datum quads should render this frame. Unlike base planes, datums are
// user-created reference geometry, so they follow showPlanes but do NOT
// auto-hide once bodies exist; always shown while picking a plane.
static bool DatumPlanesVisible(const AppState& state) {
  return state.sketchEdit.pickingPlane || state.sketchEdit.IsEditing() ||
         state.showPlanes;
}

void AppendDatumPlaneQuads(AppState& state, std::vector<Vertex3D>& quads) {
  if (!state.document || !DatumPlanesVisible(state))
    return;
  const glm::vec3 kDatumTint(0.85f, 0.62f, 0.28f); // amber, distinct from bases
  for (size_t i = 0; i < state.document->FeatureCount(); i++) {
    auto* dp = dynamic_cast<DatumPlaneFeature*>(state.document->GetFeature(i));
    if (!dp)
      continue;
    const bool emphasized =
        dp->GetId() == state.sketchEdit.hoveredDatumId ||
        dp->GetId() == state.pendingFeatureId ||
        static_cast<int>(i) == state.selectedFeature;
    AppendPlaneQuad(dp->OutputPlane(), kDatumTint * (emphasized ? 1.8f : 1.0f),
                    3.0f, quads);
  }
}

void AppendDatumAxisLines(AppState& state, std::vector<Vertex3D>& lines) {
  if (!state.document || !DatumPlanesVisible(state))
    return;
  const glm::vec3 kAxisTint(0.90f, 0.75f, 0.35f); // brighter amber than planes
  const float half = 3.0f;                        // half the drawn length
  for (size_t i = 0; i < state.document->FeatureCount(); i++) {
    auto* da = dynamic_cast<DatumAxisFeature*>(state.document->GetFeature(i));
    if (!da)
      continue;
    const bool emphasized = da->GetId() == state.pendingFeatureId ||
                            static_cast<int>(i) == state.selectedFeature;
    const glm::vec3 c = kAxisTint * (emphasized ? 1.4f : 1.0f);
    const glm::vec3 o = da->Origin();
    const glm::vec3 d = da->Direction();
    lines.push_back(LineVertex(o - d * half, c));
    lines.push_back(LineVertex(o + d * half, c));
  }
}

void DrawDatumLabels(AppState& state) {
  if (!state.document || !DatumPlanesVisible(state))
    return;
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const ImU32 col = IM_COL32(230, 180, 90, 230);
  for (size_t i = 0; i < state.document->FeatureCount(); i++) {
    Feature* f = state.document->GetFeature(i);
    if (auto* dp = dynamic_cast<DatumPlaneFeature*>(f)) {
      // Label near a corner of the 3-unit quad.
      glm::vec2 s = SketchUVToScreen(state, dp->OutputPlane(),
                                     glm::vec2(2.6f, 2.6f));
      dl->AddText(ImVec2(s.x, s.y), col, dp->GetName().c_str());
    } else if (auto* da = dynamic_cast<DatumAxisFeature*>(f)) {
      const glm::vec3 end = da->Origin() + da->Direction() * 3.0f;
      glm::vec2 s = WorldToScreen(state, end);
      dl->AddText(ImVec2(s.x, s.y), col, da->GetName().c_str());
    }
  }
}

void DrawElementNameOverlay(AppState& state) {
  if (!state.showElementNames || !state.document || !state.faceSelector)
    return;
  // While edge picking, label the hovered edge instead of a face.
  if (state.edgePick.hasHover) {
    const auto ref = EdgeElementRef(state, state.edgePick.hoverMeshId,
                                    state.edgePick.hoverEdgeIndex);
    const char* text = ref ? ref->name.c_str() : "(unnamed)";
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 sz = ImGui::CalcTextSize(text);
    dl->AddRectFilled(ImVec2(mouse.x + 12 - 3, mouse.y + 12 - 2),
                      ImVec2(mouse.x + 12 + sz.x + 3, mouse.y + 12 + sz.y + 2),
                      IM_COL32(20, 24, 30, 220), 3.0f);
    dl->AddText(ImVec2(mouse.x + 12, mouse.y + 12),
                IM_COL32(120, 230, 170, 255), text);
    return;
  }
  // Label the hovered face (live feedback), falling back to the selected one.
  const Face* face = state.faceSelector->GetHoveredFace();
  if (face == nullptr)
    face = state.faceSelector->GetSelectedFace();
  if (face == nullptr)
    return;

  // Resolve the mesh back to its Body, then that face's element name.
  const auto& meshes = state.documentMeshes;
  const auto it = std::find_if(meshes.begin(), meshes.end(),
                               [&](const DocumentRenderMesh& m) {
                                 return m.meshId == face->meshId;
                               });
  if (it == meshes.end() || it->bodyIndex >= state.document->GetBodies().size())
    return;
  const Body& body = state.document->GetBodies()[it->bodyIndex];
  const std::string* name =
      body.elementMap.NameOf(static_cast<int>(face->faceIndex));

  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const glm::vec3 center =
      state.faceSelector->GetFaceCenter(face->meshId, face->faceIndex);
  const glm::vec2 s = WorldToScreen(state, center);
  const char* text = name != nullptr ? name->c_str() : "(unnamed)";
  const ImVec2 sz = ImGui::CalcTextSize(text);
  dl->AddRectFilled(ImVec2(s.x - sz.x * 0.5f - 3, s.y - 2),
                    ImVec2(s.x + sz.x * 0.5f + 3, s.y + sz.y + 2),
                    IM_COL32(20, 24, 30, 220), 3.0f);
  dl->AddText(ImVec2(s.x - sz.x * 0.5f, s.y), IM_COL32(120, 230, 170, 255),
              text);
}

bool EffectivePlaneVisibility(const AppState& state) {
  if (state.sketchEdit.pickingPlane || state.sketchEdit.IsEditing())
    return true;
  const bool hasBodies =
      state.document && !state.document->GetBodies().empty();
  return state.showPlanes && !hasBodies; // auto-hide once the part has geometry
}

void ReplaceDocument(AppState& state, std::unique_ptr<Document> doc) {
  state.document = std::move(doc);
  state.commands = std::make_unique<CommandStack>();
  state.selectedFeature = -1;
  state.pendingFeatureId = 0;
  state.sketchEdit = SketchEditState{};
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
  size_t palette = 0;
  const auto& bodies = state.document->GetBodies();
  for (size_t bi = 0; bi < bodies.size(); bi++) {
    const auto& body = bodies[bi];
    CADShape::Mesh cadMesh = body.shape.GenerateMesh(0.1);
    if (cadMesh.vertices.empty())
      continue;

    std::vector<Vertex3D> vertices = CADMeshConverter::ConvertCADMeshToVertices(
        cadMesh, kBodyPalette[palette++ % std::size(kBodyPalette)]);
    uint32_t meshId = state.meshRenderer->AddMesh(vertices, cadMesh.indices);
    // A body with an assigned material overrides the palette color with the
    // material's base color and real PBR params; unassigned bodies keep the
    // palette color and the mesh's neutral-dielectric defaults.
    if (!body.material.empty()) {
      const auto& mat = CADMaterials::MaterialLibrary::GetMaterial(
          CADMaterials::MaterialLibrary::FromSlug(body.material));
      state.meshRenderer->SetMeshMaterial(
          meshId, mat.baseColor,
          glm::vec4(mat.metallic, mat.roughness, mat.specular, mat.reflectance));
    }
    uint32_t outlineId = 0;
    if (state.outlineRenderer)
      outlineId = state.outlineRenderer->AddMesh(vertices, cadMesh.indices);
    std::vector<std::vector<glm::vec3>> edges =
        body.shape.GenerateEdgePolylines(0.1);
    if (state.faceSelector)
      state.faceSelector->AddMesh(meshId, vertices, cadMesh.indices,
                                  cadMesh.triangleFaces, glm::mat4(1.0f),
                                  edges);
    state.documentMeshes.push_back(
        {meshId, outlineId, bi, std::move(edges)});
  }

  // Sketch line geometry (static inactive sketches + the active sketch's
  // expanded primitives) is rebuilt per-frame in UI.cpp, since it must react to
  // edit-mode and selection/hover changes that don't bump the document version.

  LOG_INFO("Document synced: {} features, {} bodies",
           state.document->FeatureCount(), state.document->GetBodies().size());
}

bool SyncRayTracingScene(AppState& state, bool groundPlane) {
  if (!state.rayTracer || !state.rayTracer->IsInitialized() || !state.document)
    return false;

  state.rayTracer->ClearGeometry();
  state.rayTracer->ClearMaterials();

  glm::vec3 bbMin(FLT_MAX), bbMax(-FLT_MAX);
  for (const auto& body : state.document->GetBodies()) {
    CADShape::Mesh cadMesh = body.shape.GenerateMesh(0.1);
    if (cadMesh.vertices.empty())
      continue;

    CADMaterials::MaterialType mt =
        body.material.empty()
            ? CADMaterials::MaterialType::DEFAULT
            : CADMaterials::MaterialLibrary::FromSlug(body.material);
    const auto& mat = CADMaterials::MaterialLibrary::GetMaterial(mt);

    RTMaterial rtm{};
    rtm.baseColor = mat.baseColor;
    rtm.metallic = mat.metallic;
    rtm.roughness = mat.roughness;
    rtm.emissive = glm::vec3(0.0f);
    rtm.specular = glm::vec3(mat.specular);
    rtm.transmission = 0.0f;
    uint32_t matId = state.rayTracer->AddMaterial(rtm);

    std::vector<Vertex3D> vertices =
        CADMeshConverter::ConvertCADMeshToVertices(cadMesh, mat.baseColor);
    for (const Vertex3D& v : vertices) {
      bbMin = glm::min(bbMin, v.position);
      bbMax = glm::max(bbMax, v.position);
    }
    state.rayTracer->AddGeometry(vertices, cadMesh.indices, matId);
  }

  if (state.rayTracer->GeometryCount() == 0)
    return false;

  // Shadow-catcher ground plane: a large quad just under the scene so soft
  // shadows and AO have a surface to land on. The closest-hit shader renders it
  // as the environment darkened by occlusion, so the backdrop stays the HDRI.
  if (groundPlane) {
    const glm::vec3 center = 0.5f * (bbMin + bbMax);
    const float diag = glm::length(bbMax - bbMin);
    const float half = 10.0f * diag;             // covers every camera angle
    const float y = bbMin.y - 0.001f * diag;     // just below: no coplanar fizz
    const glm::vec3 n(0.0f, 1.0f, 0.0f);
    const glm::vec3 white(1.0f);
    std::vector<Vertex3D> quad = {
        {{center.x - half, y, center.z - half}, n, white},
        {{center.x + half, y, center.z - half}, n, white},
        {{center.x + half, y, center.z + half}, n, white},
        {{center.x - half, y, center.z + half}, n, white},
    };
    std::vector<uint32_t> quadIdx = {0, 1, 2, 0, 2, 3};
    state.rayTracer->AddGeometry(quad, quadIdx, 0, RT_GEOM_SHADOW_CATCHER);
  }

  return state.rayTracer->BuildAccelerationStructures();
}

RTStillDesc MakeStillDesc(AppState& state, uint32_t width, uint32_t height,
                          uint32_t seed) {
  RTStillDesc desc;
  desc.width = width;
  desc.height = height;
  desc.seed = seed;
  if (state.camera) {
    const float aspect =
        height > 0 ? static_cast<float>(width) / static_cast<float>(height)
                   : 1.0f;
    desc.view = state.camera->GetViewMatrix();
    desc.proj = state.camera->GetProjectionMatrixForAspect(aspect);
    desc.cameraPos = state.camera->GetPosition();
  }
  if (state.ibl) {
    desc.environment = state.ibl->EnvironmentView();
    desc.irradiance = state.ibl->IrradianceView();
    desc.prefilter = state.ibl->PrefilterView();
    desc.brdfLut = state.ibl->BrdfLutView();
    desc.envSampler = state.ibl->EnvSampler();
    desc.lutSampler = state.ibl->LutSampler();
    desc.prefilterMips = state.ibl->PrefilterMips();
  }
  return desc;
}

bool RenderStillBlocking(AppState& state, uint32_t width, uint32_t height,
                         uint32_t samples, const std::string& path,
                         uint32_t seed, bool groundPlane, std::string* error) {
  auto fail = [&](const std::string& m) {
    if (error)
      *error = m;
    return false;
  };
  if (!state.rayTracer || !state.rayTracer->IsInitialized())
    return fail("ray tracing not available");
  if (!SyncRayTracingScene(state, groundPlane))
    return fail("no geometry to render");
  if (samples == 0)
    samples = 1;

  RTStillDesc desc = MakeStillDesc(state, width, height, seed);
  if (!desc.irradiance)
    return fail("IBL maps unavailable");
  if (!state.rayTracer->BeginStill(desc))
    return fail("BeginStill failed");

  // Trace in batches so a single command buffer never runs unbounded (TDR
  // safety); the RPC path just loops them back to back.
  const uint32_t kBatch = 32;
  for (uint32_t s = 0; s < samples; s += kBatch) {
    uint32_t n = std::min(kBatch, samples - s);
    if (!state.rayTracer->RenderBatch(s, n)) {
      state.rayTracer->EndStill();
      return fail("RenderBatch failed");
    }
  }

  std::string rbError;
  bool ok = state.rayTracer->ReadbackStill(samples, path, &rbError);
  state.rayTracer->EndStill();
  if (!ok)
    return fail(rbError.empty() ? "readback failed" : rbError);
  return true;
}

void PumpRenderJob(AppState& state) {
  RenderJob& job = state.renderJob;
  if (!job.active)
    return;

  auto finish = [&](bool ok, const std::string& err) {
    if (state.rayTracer && job.started)
      state.rayTracer->EndStill();
    job.active = false;
    job.started = false;
    job.finished = true;
    job.success = ok;
    job.error = ok ? std::string() : err;
    job.resultPath = ok ? job.outPath : std::string();
  };

  if (!state.rayTracer || !state.rayTracer->IsInitialized()) {
    finish(false, "ray tracing unavailable");
    return;
  }
  if (job.cancel) {
    finish(false, "cancelled");
    return;
  }

  if (!job.started) {
    // First frame of the job: build the scene + acceleration structures and set
    // up the still. Spread over its own frame so the modal shows immediately.
    if (!SyncRayTracingScene(state, job.groundPlane)) {
      finish(false, "no geometry to render");
      return;
    }
    RTStillDesc desc = MakeStillDesc(state, job.width, job.height, job.seed);
    if (!desc.irradiance) {
      finish(false, "IBL maps unavailable");
      return;
    }
    if (!state.rayTracer->BeginStill(desc)) {
      finish(false, "BeginStill failed");
      return;
    }
    job.started = true;
    job.done = 0;
    return;
  }

  if (job.done < job.totalSamples) {
    uint32_t n = std::min<uint32_t>(32, job.totalSamples - job.done);
    if (!state.rayTracer->RenderBatch(job.done, n)) {
      finish(false, "RenderBatch failed");
      return;
    }
    job.done += n;
    return;
  }

  // All samples traced: tonemap + write the PNG, then finish.
  std::string err;
  bool ok = state.rayTracer->ReadbackStill(job.totalSamples, job.outPath, &err);
  finish(ok, err.empty() ? "readback failed" : err);
}

std::vector<Vertex3D> BuildActiveSketchOverlay(AppState& state,
                                               uint64_t activeSketchId) {
  // Entity lines and selection markers now render through the expanded
  // primitive path (BuildActiveSketchPrimitives); the caller adds the
  // rubber-band preview, grid, and gizmo to this buffer.
  (void)state;
  (void)activeSketchId;
  return {};
}

namespace {
// Vertices exposed by an entity for dots (which, plane-UV).
std::vector<std::pair<uint8_t, glm::vec2>>
EntityDots(const sketch::Entity& e) {
  if (auto* p = std::get_if<sketch::Point>(&e))
    return {{0, p->p}};
  if (auto* l = std::get_if<sketch::Line>(&e))
    return {{0, l->a}, {1, l->b}};
  if (auto* c = std::get_if<sketch::Circle>(&e))
    return {{0, c->center}};
  if (auto* a = std::get_if<sketch::Arc>(&e))
    return {{0, a->center}, {1, sketch::ArcPointUV(*a, 1)},
            {2, sketch::ArcPointUV(*a, 2)}};
  if (auto* el = std::get_if<sketch::Ellipse>(&e))
    return {{0, el->center}};
  return {};
}

// A thick screen-space line segment (two triangles). The B-end negates
// corner.x so both ends offset to the same world side (no bowtie).
void AppendPrimLine(glm::vec3 a, glm::vec3 b, float halfPx,
                    const glm::vec4& color, std::vector<PrimVertex>& out) {
  PrimVertex am{a, b, {-1, 0}, {halfPx, 0}, color};
  PrimVertex ap{a, b, {1, 0}, {halfPx, 0}, color};
  PrimVertex bp{b, a, {-1, 0}, {halfPx, 0}, color}; // logical +side at B
  PrimVertex bm{b, a, {1, 0}, {halfPx, 0}, color};  // logical -side at B
  for (const PrimVertex& v : {am, ap, bp, am, bp, bm})
    out.push_back(v);
}

// A dashed line (world-space dashes) for construction/reference geometry.
void AppendPrimDashedLine(glm::vec3 a, glm::vec3 b, float halfPx,
                          const glm::vec4& color, std::vector<PrimVertex>& out) {
  glm::vec3 d = b - a;
  float len = glm::length(d);
  if (!std::isfinite(len) || len < 1e-6f)
    return; // skip degenerate / non-finite (e.g. a solve that blew up)
  glm::vec3 dir = d / len;
  const float dash = 0.15f, gap = 0.10f, period = dash + gap;
  // Cap the dash count: an under-constrained drag can fling a point to a huge
  // coordinate, and an unbounded `t < len` loop would push billions of vertices
  // and exhaust memory. Past the cap the (off-screen) line simply stops dashing.
  const int n = std::min(4096, static_cast<int>(len / period) + 1);
  for (int i = 0; i < n; i++) {
    const float t = i * period;
    AppendPrimLine(a + dir * t, a + dir * std::min(t + dash, len), halfPx, color,
                   out);
  }
}

// A constant-size dot (two triangles, SDF-round in the fragment shader).
void AppendPrimDot(glm::vec3 p, float radiusPx, const glm::vec4& color,
                   std::vector<PrimVertex>& out) {
  PrimVertex c00{p, p, {-1, -1}, {radiusPx, 1}, color};
  PrimVertex c10{p, p, {1, -1}, {radiusPx, 1}, color};
  PrimVertex c11{p, p, {1, 1}, {radiusPx, 1}, color};
  PrimVertex c01{p, p, {-1, 1}, {radiusPx, 1}, color};
  for (const PrimVertex& v : {c00, c10, c11, c00, c11, c01})
    out.push_back(v);
}
} // namespace

std::vector<Vertex3D> BuildStaticSketchLines(AppState& state,
                                             uint64_t activeSketchId) {
  std::vector<Vertex3D> lines;
  if (!state.document)
    return lines;
  for (const auto& feature : state.document->GetFeatures()) {
    if (auto* sk = dynamic_cast<const SketchFeature*>(feature.get())) {
      if (sk->GetId() == activeSketchId)
        continue; // active sketch renders through the primitive path
      AppendSketchLines(*sk, kSketchColor, lines);
    }
  }
  return lines;
}

std::vector<PrimVertex> BuildActiveSketchPrimitives(AppState& state,
                                                    uint64_t activeSketchId) {
  std::vector<PrimVertex> prims;
  if (activeSketchId == 0 || !state.document)
    return prims;
  auto* sk = dynamic_cast<const SketchFeature*>(
      state.document->FindFeatureById(activeSketchId));
  if (!sk)
    return prims;
  const SketchEditState& edit = state.sketchEdit;

  auto selectedEntity = [&](uint32_t id) {
    for (const auto& r : edit.selected)
      if (r.entity == id)
        return true;
    return false;
  };
  auto selectedVertex = [&](uint32_t id, uint8_t which) {
    for (const auto& r : edit.selected)
      if (r.entity == id && r.which == which)
        return true;
    return false;
  };
  // A line is emphasized on hover only when its *body* is hovered — hovering a
  // vertex highlights just that dot, not the whole line.
  auto hoveredEntityBody = [&](uint32_t id) {
    return edit.hasHover && edit.hoveredBody && edit.hoveredRef.entity == id;
  };

  // Fully-constrained sketches read in a neutral "solved" colour; under-
  // constrained ones stay blue (Onshape's black-vs-blue signal).
  const glm::vec4 base = edit.dof == 0 ? glm::vec4(0.90f, 0.92f, 0.96f, 1.0f)
                                       : glm::vec4(0.55f, 0.85f, 1.00f, 1.0f);
  const glm::vec4 sel(1.00f, 0.62f, 0.20f, 1.0f);
  const glm::vec4 hov(1.00f, 1.00f, 0.65f, 1.0f);
  const glm::vec4 trim(1.00f, 0.35f, 0.28f, 1.0f);        // trim-will-remove
  const glm::vec4 construct(0.55f, 0.60f, 0.70f, 0.85f);  // reference geometry
  const glm::vec4 conflict(1.00f, 0.30f, 0.30f, 1.0f);    // over-defined

  // Entities touched by a conflicting/redundant constraint, flagged red.
  std::set<uint32_t> conflictIds;
  for (size_t ci : edit.conflictingConstraints) {
    if (ci >= sk->constraints.size())
      continue;
    const auto& con = sk->constraints[ci];
    conflictIds.insert(con.a.entity);
    conflictIds.insert(con.b.entity);
    conflictIds.insert(con.c.entity);
  }

  // Entity lines. Construction geometry draws dashed + dimmed (unless
  // selected/hovered, where the emphasis colour reads better solid).
  for (const auto& e : sk->entities) {
    const uint32_t id = sketch::EntityId(e);
    const bool s = selectedEntity(id), h = hoveredEntityBody(id);
    const bool constr = sketch::IsConstruction(e);
    const bool bad = conflictIds.count(id) > 0;
    const glm::vec4 col = bad ? conflict
                        : s   ? sel
                        : h   ? hov
                        : (constr ? construct : base);
    const float halfPx = (s || h) ? 3.0f : (constr ? 1.3f : 1.8f);
    std::vector<glm::vec2> poly = sketch::TessellateEntity(e);
    for (size_t i = 0; i + 1 < poly.size(); i++) {
      glm::vec3 wa = sk->plane.PlaneToWorld(poly[i]);
      glm::vec3 wb = sk->plane.PlaneToWorld(poly[i + 1]);
      if (constr && !s && !h)
        AppendPrimDashedLine(wa, wb, halfPx, col, prims);
      else
        AppendPrimLine(wa, wb, halfPx, col, prims);
    }
  }

  // Vertex dots (endpoints, circle centers, points).
  for (const auto& e : sk->entities) {
    const uint32_t id = sketch::EntityId(e);
    for (auto& [which, uv] : EntityDots(e)) {
      const bool s = selectedVertex(id, which);
      const bool h = edit.hasHover && !edit.hoveredBody &&
                     edit.hoveredRef.entity == id && edit.hoveredRef.which == which;
      const glm::vec4 col = s ? sel : (h ? hov : base);
      const float rPx = (s || h) ? 5.8f : 3.8f;
      AppendPrimDot(sk->plane.PlaneToWorld(uv), rPx, col, prims);
    }
  }

  // Trim preview: the sub-segment the cursor would remove, drawn thick and in
  // a "delete" colour on top of the line so it's clear what a click deletes.
  if (edit.hasTrimPreview) {
    AppendPrimLine(sk->plane.PlaneToWorld(edit.trimPreviewA),
                   sk->plane.PlaneToWorld(edit.trimPreviewB), 3.4f, trim, prims);
  }
  return prims;
}

std::optional<ElementRef> EdgeElementRef(AppState& state, uint32_t meshId,
                                         uint32_t edgeIndex) {
  const auto& meshes = state.documentMeshes;
  const auto it = std::find_if(
      meshes.begin(), meshes.end(),
      [&](const DocumentRenderMesh& m) { return m.meshId == meshId; });
  if (it == meshes.end() || it->bodyIndex >= state.document->GetBodies().size())
    return std::nullopt;
  const Body& body = state.document->GetBodies()[it->bodyIndex];
  const std::string* name =
      body.elementMap.EdgeNameOf(static_cast<int>(edgeIndex));
  if (name == nullptr)
    return std::nullopt; // the edge has no stable name
  return ElementRef{body.createdBy, *name};
}

namespace {

// Adds/removes one edge ref on the blend. A pending (previewing) feature is
// edited directly — it isn't on the undo stack until accepted; an accepted
// one goes through ModifyFeatureCommand so the change is one undo step.
void ToggleBlendEdge(AppState& state, FilletFeature& fil,
                     const ElementRef& ref) {
  if (!fil.edges.empty() && fil.edges[0].featureId != ref.featureId) {
    state.sketchEdit.actionMsg =
        "Pick edges on the same body (or accept and start another blend).";
    return;
  }
  const bool pending = state.pendingFeatureId == fil.GetId();
  size_t index = 0;
  if (state.document->FindFeatureById(fil.GetId(), &index) == nullptr) {
    return;
  }
  nlohmann::json before = DocumentIO::FeatureToJson(fil);
  const auto it = std::find_if(fil.edges.begin(), fil.edges.end(),
                               [&](const ElementRef& e) {
                                 return e.featureId == ref.featureId &&
                                        e.name == ref.name;
                               });
  if (it != fil.edges.end()) {
    fil.edges.erase(it);
  } else {
    fil.edges.push_back(ref);
  }
  if (pending) {
    state.document->Regenerate();
    return;
  }
  state.commands->Execute(
      std::make_unique<ModifyFeatureCommand>(index, before,
                                             DocumentIO::FeatureToJson(fil)),
      *state.document);
}

} // namespace

bool HandleEdgePickInput(AppState& state, bool overViewport) {
  if (!state.edgePick.Active() || !state.faceSelector || !state.camera ||
      !state.document) {
    return false;
  }
  // The mode ends with its feature (deleted, preview resolved elsewhere).
  auto* fil = dynamic_cast<FilletFeature*>(
      state.document->FindFeatureById(state.edgePick.filletId));
  if (fil == nullptr) {
    state.edgePick = {};
    return false;
  }
  const bool pending = state.pendingFeatureId == fil->GetId();
  // Escape ends a re-pick session; while previewing it stays with the
  // preview's own cancel handling.
  if (!pending && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    state.edgePick = {};
    return true;
  }

  state.edgePick.hasHover = false;
  if (!overViewport) {
    return false;
  }

  ImGuiIO& io = ImGui::GetIO();
  const Ray ray = state.faceSelector->ScreenToWorldRay(
      io.MousePos.x - state.viewportPos.x, io.MousePos.y - state.viewportPos.y,
      static_cast<int>(state.viewportSize.x),
      static_cast<int>(state.viewportSize.y), *state.camera);
  const EdgeHit hit = state.faceSelector->PickEdge(ray);
  if (hit.hit) {
    state.edgePick.hasHover = true;
    state.edgePick.hoverMeshId = hit.meshId;
    state.edgePick.hoverEdgeIndex = hit.edgeIndex;
  }

  if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hit.hit) {
    if (auto ref = EdgeElementRef(state, hit.meshId, hit.edgeIndex)) {
      ToggleBlendEdge(state, *fil, *ref);
    } else {
      state.sketchEdit.actionMsg =
          "This edge has no stable name and can't be blended.";
    }
  }
  return true; // the mode owns viewport clicks and hover
}

void AppendEdgePickPrimitives(AppState& state, std::vector<PrimVertex>& out) {
  if (!state.edgePick.Active() || !state.document) {
    return;
  }
  const auto* fil = dynamic_cast<const FilletFeature*>(
      state.document->FindFeatureById(state.edgePick.filletId));
  if (fil == nullptr) {
    return;
  }
  const glm::vec4 kDim{0.55f, 0.62f, 0.72f, 0.85f};
  const glm::vec4 kHover{1.00f, 0.85f, 0.30f, 1.0f};
  const glm::vec4 kPicked{0.35f, 0.80f, 1.00f, 1.0f};

  for (const DocumentRenderMesh& m : state.documentMeshes) {
    if (m.bodyIndex >= state.document->GetBodies().size()) {
      continue;
    }
    const Body& body = state.document->GetBodies()[m.bodyIndex];
    for (size_t e = 0; e < m.edgePolylines.size(); e++) {
      const auto& poly = m.edgePolylines[e];
      if (poly.size() < 2) {
        continue;
      }
      const std::string* name = body.elementMap.EdgeNameOf(static_cast<int>(e));
      const bool picked =
          name != nullptr &&
          std::any_of(fil->edges.begin(), fil->edges.end(),
                      [&](const ElementRef& r) {
                        return r.featureId == body.createdBy &&
                               r.name == *name;
                      });
      const bool hovered = state.edgePick.hasHover &&
                           state.edgePick.hoverMeshId == m.meshId &&
                           state.edgePick.hoverEdgeIndex == e;
      glm::vec4 color = kDim;
      float halfPx = 1.2f;
      if (picked) {
        color = kPicked;
        halfPx = 2.6f;
      }
      if (hovered) {
        color = kHover;
        halfPx = 2.6f;
      }
      for (size_t i = 0; i + 1 < poly.size(); i++) {
        AppendPrimLine(poly[i], poly[i + 1], halfPx, color, out);
      }
    }
  }
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
