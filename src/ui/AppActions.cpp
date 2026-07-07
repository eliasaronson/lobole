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
#include "ui/PreviewFeature.h"

#include <cfloat>
#include <filesystem>
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

void StartNewSketch(AppState& state) {
  state.sketchEdit = SketchEditState{};
  state.sketchEdit.pickingPlane = true;
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

  // Sketch line geometry (static inactive sketches + the active sketch's
  // expanded primitives) is rebuilt per-frame in UI.cpp, since it must react to
  // edit-mode and selection/hover changes that don't bump the document version.

  LOG_INFO("Document synced: {} features, {} bodies",
           state.document->FeatureCount(), state.document->GetBodies().size());
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
  if (len < 1e-6f)
    return;
  glm::vec3 dir = d / len;
  const float dash = 0.15f, gap = 0.10f;
  for (float t = 0.0f; t < len; t += dash + gap)
    AppendPrimLine(a + dir * t, a + dir * std::min(t + dash, len), halfPx, color,
                   out);
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
