#include "ui/AppActions.h"

#include "CADMaterials.h"
#include "CADMeshConverter.h"
#include "Camera.h"
#include "IBLTextures.h"
#include "RayTracingRenderer.h"
#include "FaceSelector.h"
#include "Logger.h"
#include "FieldRenderer.h"
#include "MeshRenderer.h"
#include "OutlineRenderer.h"
#include "SketchRenderer.h"
#include "core/AssemblySolve.h"
#include "core/Autosaver.h"
#include "core/MassProps.h"
#include "core/Measure.h"
#include "core/GitVault.h"
#include "core/BooleanFeature.h"
#include "core/Command.h"
#include "core/DatumAxisFeature.h"
#include "core/DatumPlaneFeature.h"
#include "core/ExtrudeFeature.h"
#include "core/Feature.h"
#include "core/FilletFeature.h"
#include "core/DrawingProjection.h"
#include "core/DrawingRender.h"
#include "core/Serialization.h"

#include <nlohmann/json.hpp>
#include "core/PatternFeature.h"
#include "core/RevolveFeature.h"
#include "core/SketchFeature.h"
#include "core/SketchProfile.h"
#include "ui/PreviewFeature.h"
#include "ui/SketchInteraction.h" // SketchUVToScreen

#include "imgui.h"

#include <glm/gtc/matrix_transform.hpp> // glm::translate
#include <glm/gtc/quaternion.hpp>       // glm::mat4_cast

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <set>
#include <unordered_map>

namespace ui {

namespace {

// Sketch line color; the active sketch is drawn brighter.
constexpr glm::vec3 kSketchColor{0.40f, 0.72f, 1.00f};
constexpr glm::vec3 kActiveSketchColor{0.55f, 0.85f, 1.00f};

LineVertex3D LineVertex(const glm::vec3& pos, const glm::vec3& color) {
  LineVertex3D v;
  v.position = pos;
  v.color = color;
  return v;
}

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

} // namespace

// Builds translucent quads for the three default planes (Front/Top/Right),
// tinted by axis; the hovered plane (0/1/2, -1 = none) is brighter.
std::vector<LineVertex3D> BuildPlaneQuads(bool show, int hoveredPlane) {
  std::vector<LineVertex3D> quads;
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
                     std::vector<LineVertex3D>& quads) {
  glm::vec3 o = plane.GetOrigin();
  glm::vec3 u = plane.GetUAxis() * half;
  glm::vec3 v = plane.GetVAxis() * half;
  glm::vec3 a = o - u - v, b = o + u - v, c = o + u + v, d = o - u + v;
  for (const glm::vec3& p : {a, b, c, a, c, d}) {
    quads.push_back(LineVertex(p, color));
  }
}

void AppendSketchGrid(const SketchPlane& plane, std::vector<LineVertex3D>& lines) {
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

void AppendOriginGizmo(std::vector<LineVertex3D>& lines) {
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

void AppendCogMarker(const AppState& state, std::vector<LineVertex3D>& lines) {
  if (!state.showCog)
    return;
  // A small screen-stable 3-axis cross at the center of mass. Drawn in the
  // no-depth gizmo channel so it stays visible inside solid geometry.
  const float L = state.camera
                      ? std::max(0.05f * state.camera->GetDistance(), 0.1f)
                      : 1.0f;
  const glm::vec3 c = state.cogWorld;
  const glm::vec3 col(0.98f, 0.80f, 0.20f); // amber, distinct from the axes
  const glm::vec3 ax[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  for (const glm::vec3& a : ax) {
    lines.push_back(LineVertex(c - a * L, col));
    lines.push_back(LineVertex(c + a * L, col));
  }
}

void AppendStudyMarker(const AppState& state, std::vector<LineVertex3D>& lines) {
  const SimulationState& sim = state.simulation;
  if (!sim.IsActive() || sim.field == SimulationState::Field::None ||
      !state.document)
    return;
  const fem::StudyRuntime* rt = state.document->StudyResults(sim.activeStudyId);
  if (!rt) return;
  // A magenta cross at the peak of the displayed field (max von Mises or max
  // displacement), warped by the same deform scale so it tracks the overlay.
  const bool vm = sim.field == SimulationState::Field::VonMises;
  glm::vec3 at = vm ? glm::vec3(rt->maxVmAt) : glm::vec3(rt->maxDispAt);
  // Find the peak node's displacement to warp the marker with the mesh.
  for (const fem::StudyBodyResult& b : rt->bodies) {
    const int node = vm ? b.results.maxVmNode : b.results.maxDispNode;
    if (node >= 0 && node < (int)b.results.displacement.size() &&
        ((vm && b.bodyName == rt->maxVmBody) ||
         (!vm && b.bodyName == rt->maxDispBody))) {
      at += sim.deformScale * glm::vec3(b.results.displacement[node]);
      break;
    }
  }
  const float L = state.camera
                      ? std::max(0.06f * state.camera->GetDistance(), 0.1f)
                      : 1.0f;
  const glm::vec3 col(1.0f, 0.1f, 0.8f); // magenta, distinct from CoG amber
  const glm::vec3 ax[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  for (const glm::vec3& a : ax) {
    lines.push_back(LineVertex(at - a * L, col));
    lines.push_back(LineVertex(at + a * L, col));
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

void AppendDatumPlaneQuads(AppState& state, std::vector<LineVertex3D>& quads) {
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

namespace {
// Move the active tab's live model (part doc+commands OR assembly) back into
// its slot. Moving a null pointer is a no-op, so this handles both kinds.
void StashActive(AppState& state) {
  PartSlot& slot = state.parts[state.activePart];
  slot.doc = std::move(state.document);
  slot.assembly = std::move(state.assembly);
  slot.drawing = std::move(state.drawing);
  slot.commands = std::move(state.commands);
  slot.history = std::move(state.assemblyHistory);
  slot.drawingHistory = std::move(state.drawingHistory);
  slot.drawingRenders = std::move(state.drawingRenders);
  state.drawingRenders.clear(); // moved-from: guarantee empty
  slot.lastSavedVersion = state.lastSavedVersion;
  slot.solvedPartVersions = std::move(state.solvedPartVersions);
  state.solvedPartVersions.clear(); // moved-from: guarantee empty
}
// Move a slot's model into AppState as the active tab. Assumes AppState holds
// nothing live (call StashActive first, or after a clear).
void ActivateSlot(AppState& state, size_t index) {
  PartSlot& slot = state.parts[index];
  state.document = std::move(slot.doc);
  state.assembly = std::move(slot.assembly);
  state.drawing = std::move(slot.drawing);
  state.commands = std::move(slot.commands);
  state.assemblyHistory = std::move(slot.history);
  state.drawingHistory = std::move(slot.drawingHistory);
  state.drawingRenders = std::move(slot.drawingRenders);
  slot.drawingRenders.clear();
  state.lastSavedVersion = slot.lastSavedVersion;
  state.solvedPartVersions = std::move(slot.solvedPartVersions);
  slot.solvedPartVersions.clear();
  state.activePart = static_cast<int>(index);
}
// Shared reset after a tab switch/create.
void ResetActiveTabUiState(AppState& state) {
  state.selectedFeature = -1;
  state.selectedOccurrence = 0;
  state.pendingFeatureId = 0;
  state.sketchEdit = SketchEditState{};
  // Any tab change exits in-context editing (EnterInContextEdit re-arms it
  // after its SetActivePart returns).
  state.inContext = AppState::InContextEdit{};
  // Explode display state is per-tab and transient: reset on switch.
  state.activeExplodedView = 0;
  state.explodeFactor = 1.0f;
  // The 2D sheet pan/zoom is per-tab and transient: a fresh scale=0 makes the
  // next drawing paint fit-to-window (harmless for non-drawing tabs).
  state.sheetView = AppState::Sheet2DView{};
  // NOTE: drawingRenders is NOT cleared here — it is stashed/restored per slot
  // (StashActive/ActivateSlot) so a drawing keeps its projected geometry across
  // tab switches and only an explicit update re-projects.
  // Any tab change exits simulation mode (results themselves live on the
  // Document and survive; only the mode/display state resets).
  state.simulation = SimulationState{};
  state.syncedDocumentVersion = UINT64_MAX; // force a renderer resync
}
} // namespace

void EnsureParts(AppState& state, const std::string& firstName) {
  if (state.parts.empty()) {
    PartSlot slot;
    slot.id = state.nextPartId++;
    slot.name = firstName;
    slot.kind = DocKind::Part;
    slot.lastSavedVersion = state.lastSavedVersion;
    state.parts.push_back(std::move(slot)); // active: doc/commands stay in AppState
    state.activePart = 0;
  }
}

size_t AddPart(AppState& state, const std::string& name) {
  EnsureParts(state);
  StashActive(state);
  // Create and activate the new part.
  state.document = std::make_unique<Document>();
  state.assembly = nullptr;
  state.drawing = nullptr;
  state.commands = std::make_unique<CommandStack>();
  state.lastSavedVersion = 0;
  state.solvedPartVersions.clear();
  PartSlot slot;
  slot.id = state.nextPartId++;
  slot.name = name;
  slot.kind = DocKind::Part;
  state.parts.push_back(std::move(slot));
  state.activePart = static_cast<int>(state.parts.size()) - 1;
  ResetActiveTabUiState(state);
  return state.parts.size() - 1;
}

size_t AddAssembly(AppState& state, const std::string& name) {
  EnsureParts(state);
  StashActive(state);
  // Create and activate the new assembly. Occurrence undo is Phase 3, so an
  // assembly tab currently carries no CommandStack.
  state.document = nullptr;
  state.assembly = std::make_unique<AssemblyDocument>();
  state.drawing = nullptr;
  state.commands = nullptr;
  state.lastSavedVersion = 0;
  state.solvedPartVersions.clear();
  PartSlot slot;
  slot.id = state.nextPartId++;
  slot.name = name;
  slot.kind = DocKind::Assembly;
  state.parts.push_back(std::move(slot));
  state.activePart = static_cast<int>(state.parts.size()) - 1;
  ResetActiveTabUiState(state);
  return state.parts.size() - 1;
}

size_t AddDrawing(AppState& state, const std::string& name) {
  EnsureParts(state);
  StashActive(state);
  // Create and activate the new drawing. Undo is a snapshot stack (the
  // assembly pattern), so a drawing tab carries no CommandStack.
  state.document = nullptr;
  state.assembly = nullptr;
  state.drawing = std::make_unique<DrawingDocument>();
  state.commands = nullptr;
  state.lastSavedVersion = 0;
  state.solvedPartVersions.clear();
  PartSlot slot;
  slot.id = state.nextPartId++;
  slot.name = name;
  slot.kind = DocKind::Drawing;
  state.parts.push_back(std::move(slot));
  state.activePart = static_cast<int>(state.parts.size()) - 1;
  ResetActiveTabUiState(state);
  return state.parts.size() - 1;
}

void SnapshotDrawing(AppState& state) {
  if (!state.drawing) return;
  state.drawingHistory.undo.push_back(*state.drawing); // full-state copy
  state.drawingHistory.redo.clear();
}

bool UndoDrawing(AppState& state) {
  if (!state.drawing || state.drawingHistory.undo.empty()) return false;
  state.drawingHistory.redo.push_back(*state.drawing);
  *state.drawing = std::move(state.drawingHistory.undo.back());
  state.drawingHistory.undo.pop_back();
  state.syncedDocumentVersion = UINT64_MAX;
  return true;
}

bool RedoDrawing(AppState& state) {
  if (!state.drawing || state.drawingHistory.redo.empty()) return false;
  state.drawingHistory.undo.push_back(*state.drawing);
  *state.drawing = std::move(state.drawingHistory.redo.back());
  state.drawingHistory.redo.pop_back();
  state.syncedDocumentVersion = UINT64_MAX;
  return true;
}

uint64_t PartIdByName(const AppState& state, const std::string& name) {
  for (const PartSlot& slot : state.parts)
    if (slot.kind == DocKind::Part && slot.name == name) return slot.id;
  return 0;
}

void SnapshotAssembly(AppState& state) {
  if (!state.assembly) return;
  state.assemblyHistory.undo.push_back(*state.assembly); // full-state copy
  state.assemblyHistory.redo.clear();
}

bool UndoAssembly(AppState& state) {
  if (!state.assembly || state.assemblyHistory.undo.empty()) return false;
  state.assemblyHistory.redo.push_back(*state.assembly);
  *state.assembly = std::move(state.assemblyHistory.undo.back());
  state.assemblyHistory.undo.pop_back();
  state.syncedDocumentVersion = UINT64_MAX; // restored state may share a version
  return true;
}

bool RedoAssembly(AppState& state) {
  if (!state.assembly || state.assemblyHistory.redo.empty()) return false;
  state.assemblyHistory.undo.push_back(*state.assembly);
  *state.assembly = std::move(state.assemblyHistory.redo.back());
  state.assemblyHistory.redo.pop_back();
  state.syncedDocumentVersion = UINT64_MAX;
  return true;
}

void SelectOccurrence(AppState& state, uint64_t occId) {
  state.selectedOccurrence = occId;
  if (!state.meshRenderer) return;
  state.meshRenderer->ClearFaceHighlight();
  if (occId == 0) return;
  // Highlight every instance belonging to the occurrence (a multi-body part
  // has one instance per body).
  for (const DocumentRenderMesh& m : state.documentMeshes)
    if (m.occurrenceId == occId)
      state.meshRenderer->HighlightInstance(m.meshId);
}

void SetActivePart(AppState& state, size_t index) {
  EnsureParts(state);
  if (index >= state.parts.size() ||
      index == static_cast<size_t>(state.activePart)) {
    return;
  }
  StashActive(state);
  ActivateSlot(state, index);
  ResetActiveTabUiState(state);
  // Part→assembly propagation (A4): entering an assembly whose referenced
  // parts regenerated since its last solve re-solves it, so mates track part
  // edits without an explicit solve_assembly.
  if (state.assembly && AssemblyPartsStale(state))
    SolveActiveAssembly(state);
  // A drawing projects on the FIRST view of the tab (design §2): if the
  // restored cache is empty (never projected this session) fill it; otherwise
  // keep the stashed geometry so staleness is only cleared by explicit update.
  if (state.drawing && state.drawingRenders.empty() &&
      state.drawing->ViewCount() > 0)
    UpdateDrawingRenders(state);
}

std::optional<size_t> SlotIndexById(const AppState& state, uint64_t slotId) {
  for (size_t i = 0; i < state.parts.size(); i++)
    if (state.parts[i].id == slotId) return i;
  return std::nullopt;
}

bool EnterInContextEdit(AppState& state, uint64_t occurrenceId) {
  if (!state.assembly)
    return false;
  const Occurrence* occ = state.assembly->FindById(occurrenceId);
  if (!occ)
    return false;
  std::optional<size_t> partIndex;
  for (size_t i = 0; i < state.parts.size(); i++)
    if (state.parts[i].kind == DocKind::Part && state.parts[i].id == occ->partId)
      partIndex = i;
  if (!partIndex)
    return false; // dangling part reference
  const uint64_t asmSlotId = state.parts[state.activePart].id;
  SetActivePart(state, *partIndex); // clears inContext; re-arm after
  state.inContext = {asmSlotId, occurrenceId};
  return true;
}

// ---- Simulation mode (FEM) ------------------------------------------------

bool RunStudyAndCache(Document& doc, uint64_t studyId, std::string* error) {
  fem::Study* s = doc.FindStudy(studyId);
  if (!s) {
    if (error) *error = "no such study";
    return false;
  }
  fem::StudyRuntime rt;
  if (!fem::RunStudy(doc, *s, rt, error)) return false;
  rt.solvedAtVersion = doc.Version();
  doc.StoreStudyResults(studyId, std::move(rt));
  return true;
}

void EnterSimulation(AppState& state) {
  Document* doc = state.document.get();
  if (!doc) return; // parts only (an assembly tab has no studies)
  state.simulation.active = true;
  if (doc->GetStudies().empty() && state.commands)
    state.commands->Execute(std::make_unique<AddStudyCommand>(""), *doc);
  // Select a study to edit (the first, unless one is already active).
  if (!doc->FindStudy(state.simulation.activeStudyId) && !doc->GetStudies().empty())
    state.simulation.activeStudyId = doc->GetStudies().front().id;
}

void ExitSimulation(AppState& state) {
  // Restore any body instances the field overlay hid.
  if (state.meshRenderer)
    for (uint32_t meshId : state.simulation.hiddenMeshes)
      state.meshRenderer->SetInstanceVisible(meshId, true);
  if (state.fieldRenderer) state.fieldRenderer->SetMesh({}, {});
  state.simulation = SimulationState{};
  state.syncedDocumentVersion = UINT64_MAX; // force a clean renderer resync
}

void SyncFieldRenderer(AppState& state) {
  if (!state.fieldRenderer || !state.meshRenderer) return;
  SimulationState& sim = state.simulation;
  Document* doc = state.document.get();

  auto restoreBodies = [&]() {
    for (uint32_t meshId : sim.hiddenMeshes)
      state.meshRenderer->SetInstanceVisible(meshId, true);
    sim.hiddenMeshes.clear();
  };

  const fem::StudyRuntime* rt =
      doc ? doc->StudyResults(sim.activeStudyId) : nullptr;
  const bool show = sim.IsActive() &&
                    sim.field != SimulationState::Field::None && rt != nullptr;
  if (!show) {
    if (sim.fieldBuiltKey != 0) {
      restoreBodies();
      state.fieldRenderer->SetMesh({}, {});
      sim.fieldBuiltKey = 0;
    }
    return;
  }

  // Rebuild only when the field, study, or results generation changes; the
  // deform scale is a push constant (updated every frame in the main loop).
  const uint64_t key = (static_cast<uint64_t>(sim.field) * 1000003ull +
                        sim.activeStudyId) * 1000003ull +
                       doc->StudyResultsEpoch();
  if (key == sim.fieldBuiltKey) return;

  const bool vm = sim.field == SimulationState::Field::VonMises;
  const double fmax = vm ? rt->maxVonMises : rt->maxDisplacement;
  const double inv = fmax > 0 ? 1.0 / fmax : 0.0;

  std::vector<FieldVertex> verts;
  std::vector<uint32_t> indices;
  // tri6 -> 4 linear sub-triangles via the midside nodes (corners 0,1,2,
  // midsides 3,4,5): (0,3,5)(3,1,4)(5,4,2)(3,4,5).
  static const int kSub[4][3] = {{0, 3, 5}, {3, 1, 4}, {5, 4, 2}, {3, 4, 5}};
  for (const fem::StudyBodyResult& b : rt->bodies) {
    for (const auto& tri : b.mesh.boundary) {
      const uint32_t base = static_cast<uint32_t>(verts.size());
      for (int i = 0; i < 6; ++i) {
        const int n = tri.nodes[i];
        const glm::dvec3& p = b.mesh.nodes[n];
        const glm::dvec3& d = b.results.displacement[n];
        const double val = vm ? b.results.vonMises[n] : glm::length(d);
        FieldVertex fv;
        fv.position = glm::vec3(p);
        fv.displacement = glm::vec3(d);
        fv.scalar = static_cast<float>(std::clamp(val * inv, 0.0, 1.0));
        verts.push_back(fv);
      }
      for (const auto& s : kSub) {
        indices.push_back(base + s[0]);
        indices.push_back(base + s[1]);
        indices.push_back(base + s[2]);
      }
    }
  }
  state.fieldRenderer->SetMesh(std::move(verts), std::move(indices));

  // Hide the solved bodies' normal render so the overlay stands in for them.
  restoreBodies();
  for (const fem::StudyBodyResult& b : rt->bodies)
    for (const DocumentRenderMesh& m : state.documentMeshes)
      if (static_cast<int>(m.bodyIndex) == b.bodyIndex) {
        state.meshRenderer->SetInstanceVisible(m.meshId, false);
        sim.hiddenMeshes.push_back(m.meshId);
      }
  sim.fieldBuiltKey = key;
}

bool DeletePart(AppState& state, size_t index) {
  EnsureParts(state);
  if (state.parts.size() <= 1 || index >= state.parts.size()) {
    return false; // never delete the last tab
  }
  const bool deletingActive = index == static_cast<size_t>(state.activePart);
  if (deletingActive) {
    // Activate a neighbor first so AppState always holds a live document.
    SetActivePart(state, index == 0 ? 1 : index - 1);
  }
  state.parts.erase(state.parts.begin() + static_cast<long>(index));
  // Fix up the active index after the erase.
  if (static_cast<size_t>(state.activePart) > index)
    state.activePart--;
  return true;
}

namespace {
// A part's document, whether active (in AppState) or stashed (in its slot).
// Null for an assembly slot.
Document* SlotDoc(AppState& state, size_t i) {
  return static_cast<int>(i) == state.activePart ? state.document.get()
                                                 : state.parts[i].doc.get();
}
// An assembly slot's document, active or stashed. Null for a part slot.
AssemblyDocument* SlotAssembly(AppState& state, size_t i) {
  return static_cast<int>(i) == state.activePart ? state.assembly.get()
                                                 : state.parts[i].assembly.get();
}
// A drawing slot's document, active or stashed. Null for a part/assembly slot.
DrawingDocument* SlotDrawing(AppState& state, size_t i) {
  return static_cast<int>(i) == state.activePart ? state.drawing.get()
                                                 : state.parts[i].drawing.get();
}
} // namespace

bool SaveProject(AppState& state, const std::string& path,
                 std::string* error) {
  namespace fs = std::filesystem;
  EnsureParts(state);
  // A single part keeps the flat .dsgn form (backward compatible). A lone
  // assembly, or any multi-document project, uses the project container.
  if (state.parts.size() == 1 && state.parts[0].kind == DocKind::Part) {
    return DocumentIO::Save(*state.document, path, error);
  }
  // Multi-document: project.json (v2) + parts/<name>/ and assemblies/<name>/.
  try {
    const fs::path dir(path);
    nlohmann::json index;
    index["format"] = "designer-project";
    index["version"] = 2;
    index["active"] = state.activePart;
    index["documents"] = nlohmann::json::array();
    for (size_t i = 0; i < state.parts.size(); i++) {
      const PartSlot& slot = state.parts[i];
      const char* sub = slot.kind == DocKind::Assembly ? "assemblies/"
                        : slot.kind == DocKind::Drawing ? "drawings/"
                                                        : "parts/";
      const char* type = slot.kind == DocKind::Assembly ? "assembly"
                         : slot.kind == DocKind::Drawing ? "drawing"
                                                         : "part";
      const fs::path docDir = dir / sub / slot.name;
      fs::create_directories(docDir);
      bool ok = false;
      switch (slot.kind) {
        case DocKind::Assembly:
          ok = DocumentIO::SaveAssembly(*SlotAssembly(state, i),
                                        docDir.string(), error);
          break;
        case DocKind::Drawing:
          ok = DocumentIO::SaveDrawing(*SlotDrawing(state, i), docDir.string(),
                                       error);
          break;
        case DocKind::Part:
          ok = DocumentIO::Save(*SlotDoc(state, i), docDir.string(), error);
          break;
      }
      if (!ok) return false;
      index["documents"].push_back({{"id", slot.id},
                                    {"name", slot.name},
                                    {"dir", std::string(sub) + slot.name},
                                    {"type", type}});
    }
    std::ofstream out(dir / "project.json");
    if (!out) {
      if (error) *error = "cannot write project.json";
      return false;
    }
    out << index.dump(2) << "\n";
    return true;
  } catch (const std::exception& e) {
    if (error) *error = e.what();
    return false;
  }
}

bool OpenProject(AppState& state, const std::string& path,
                 std::string* error) {
  namespace fs = std::filesystem;
  const fs::path projFile = fs::path(path) / "project.json";
  if (!fs::exists(projFile)) {
    // Not a multi-document project: load as a single part.
    auto doc = DocumentIO::Load(path, error);
    if (!doc)
      return false;
    ReplaceDocument(state, std::move(doc));
    state.parts.clear();
    state.nextPartId = 1;
    EnsureParts(state, fs::path(path).stem().string());
    return true;
  }
  try {
    nlohmann::json index;
    std::ifstream(projFile) >> index;
    // v2 has a typed `documents` list; v1 had a flat `parts` list (all parts,
    // no ids/types). Normalize v1 into the same shape with defaulted reads.
    nlohmann::json docs;
    if (index.contains("documents")) {
      docs = index.at("documents");
    } else {
      docs = nlohmann::json::array();
      for (const auto& p : index.at("parts"))
        docs.push_back({{"name", p.at("name")}, {"dir", p.at("dir")},
                        {"type", "part"}});
    }

    // Load every document into its slot; AppState holds nothing live yet.
    state.document = nullptr;
    state.assembly = nullptr;
    state.drawing = nullptr;
    state.commands = nullptr;
    state.parts.clear();
    uint64_t maxId = 0;
    for (size_t i = 0; i < docs.size(); i++) {
      const std::string name = docs[i].at("name").get<std::string>();
      const std::string dir =
          (fs::path(path) / docs[i].at("dir").get<std::string>()).string();
      const std::string type = docs[i].value("type", std::string("part"));
      PartSlot slot;
      slot.id = docs[i].value("id", static_cast<uint64_t>(i + 1)); // v1: 1..n
      slot.name = name;
      if (type == "assembly") {
        slot.kind = DocKind::Assembly;
        slot.assembly = DocumentIO::LoadAssembly(dir, error);
        if (!slot.assembly) return false;
      } else if (type == "drawing") {
        slot.kind = DocKind::Drawing;
        slot.drawing = DocumentIO::LoadDrawing(dir, error);
        if (!slot.drawing) return false;
      } else {
        slot.kind = DocKind::Part;
        slot.doc = DocumentIO::Load(dir, error);
        if (!slot.doc) return false;
        slot.commands = std::make_unique<CommandStack>();
      }
      maxId = std::max(maxId, slot.id);
      state.parts.push_back(std::move(slot));
    }
    state.nextPartId = maxId + 1;

    size_t wantActive = static_cast<size_t>(index.value("active", 0));
    if (wantActive >= state.parts.size()) wantActive = 0;
    ActivateSlot(state, wantActive);
    ResetActiveTabUiState(state);
    // A4: an assembly active on open solves immediately (no stamps yet), so
    // mates re-solve on load — the format's stated contract.
    if (state.assembly && AssemblyPartsStale(state))
      SolveActiveAssembly(state);
    if (state.drawing && state.drawing->ViewCount() > 0)
      UpdateDrawingRenders(state);  // re-project on first view of an opened drawing
    if (state.document) FitCameraToDocument(state);
    return true;
  } catch (const std::exception& e) {
    if (error) *error = e.what();
    return false;
  }
}

void MarkProjectSaved(AppState& state, const std::string& path,
                      const std::string& message) {
  state.projectPath = path;
  state.lastSavedVersion = state.document ? state.document->Version() : 0;
  // The save supersedes both possible sidecars: this project's, and the
  // unsaved slot if the document lived there before its first save.
  Autosaver::Clear(path);
  Autosaver::Clear("");
  // Git-backed history (F5): a directory project is its own repository;
  // every clean save becomes a commit. No-ops without git.
  if (path.ends_with(".dsgn") && GitVault::Available()) {
    if (!GitVault::IsRepo(path)) {
      GitVault::Init(path);
    }
    std::string error;
    if (!GitVault::CommitAll(path, message, &error)) {
      LOG_WARN("Git commit failed: {}", error);
    }
  }
}

void MarkProjectOpened(AppState& state, const std::string& path) {
  state.projectPath = path;
  state.lastSavedVersion = state.document ? state.document->Version() : 0;
}

void MarkProjectRecovered(AppState& state, const std::string& path) {
  state.projectPath = path;
  // Never equal to the live version: the recovered changes are unsaved.
  state.lastSavedVersion =
      state.document ? state.document->Version() - 1 : UINT64_MAX;
}

namespace {
// An occurrence's placement as a model matrix (translate ∘ rotate).
glm::mat4 OccurrenceMatrix(const Occurrence& o) {
  return glm::translate(glm::mat4(1.0f), o.position) * glm::mat4_cast(o.orientation);
}

// Exploded-view display (A4): the occurrence's factor-scaled world offset in
// the active exploded view (zero when none / no step). PURE VIEW STATE —
// composed into rendered/picked transforms only, never into the document.
glm::vec3 ExplodeOffset(const AppState& state, uint64_t occId) {
  if (!state.activeExplodedView || !state.assembly)
    return glm::vec3(0.0f);
  const ExplodedView* v =
      state.assembly->FindExplodedViewById(state.activeExplodedView);
  if (!v)
    return glm::vec3(0.0f);
  for (const ExplodeStep& s : v->steps)
    if (s.occurrenceId == occId) return s.offset * state.explodeFactor;
  return glm::vec3(0.0f);
}

// What an occurrence renders (and picks) at: the solved placement, displaced
// by the active exploded view.
glm::mat4 DisplayMatrix(const AppState& state, const Occurrence& o) {
  return glm::translate(glm::mat4(1.0f), ExplodeOffset(state, o.id)) *
         OccurrenceMatrix(o);
}

// The part document an occurrence references, by stable id (active or stashed).
Document* PartDocById(AppState& state, uint64_t partId) {
  for (size_t i = 0; i < state.parts.size(); i++)
    if (state.parts[i].kind == DocKind::Part && state.parts[i].id == partId)
      return SlotDoc(state, i);
  return nullptr;
}
} // namespace

int SolveActiveAssembly(AppState& state) {
  if (!state.assembly)
    return 0;
  const assembly::PartResolver resolve =
      [&state](uint64_t partId) -> const Document* {
    return PartDocById(state, partId);
  };
  const assembly::SolveReport rep = assembly::Solve(*state.assembly, resolve);
  state.assembly->BumpVersion(); // solved transforms changed → renderer resync
  // Stamp the referenced parts' versions (A4): AssemblyPartsStale compares
  // against these, so a later part edit re-solves on tab activation. Stamped
  // at solve (not render sync) so propagation also works headless.
  state.solvedPartVersions.clear();
  for (const Occurrence& occ : state.assembly->GetOccurrences())
    if (const Document* part = PartDocById(state, occ.partId))
      state.solvedPartVersions[occ.partId] = part->Version();
  return rep.erroredMates;
}

bool AssemblyPartsStale(AppState& state) {
  if (!state.assembly)
    return false;
  for (const Occurrence& occ : state.assembly->GetOccurrences()) {
    const Document* part = PartDocById(state, occ.partId);
    if (!part)
      continue; // dangling reference — nothing to compare (solve won't fix it)
    const auto it = state.solvedPartVersions.find(occ.partId);
    if (it == state.solvedPartVersions.end() || it->second != part->Version())
      return true; // never stamped, or the part regenerated since the stamp
  }
  return false;
}

BomTable ComputeBom(AppState& state) {
  BomTable table;
  if (!state.assembly)
    return table;
  for (const Occurrence& occ : state.assembly->GetOccurrences()) {
    BomRow* row = nullptr;
    for (BomRow& r : table.rows)
      if (r.partId == occ.partId) { row = &r; break; }
    if (row) {
      row->quantity++;
      continue;
    }
    BomRow fresh;
    fresh.partId = occ.partId;
    fresh.quantity = 1;
    for (const PartSlot& slot : state.parts)
      if (slot.kind == DocKind::Part && slot.id == occ.partId)
        fresh.partName = slot.name;
    const Document* part = PartDocById(state, occ.partId);
    if (!part) {
      fresh.partName = "(missing part)"; // dangling reference — mass unknown
    } else if (const auto mass = Measure::MassOfBodies(part->GetBodies())) {
      fresh.unitVolume = mass->volume;
      fresh.massKnown = mass->massKnown;
      fresh.unitMassG = mass->massG;
      for (size_t i = 0; i < mass->bodies.size(); i++) {
        if (i == 0)
          fresh.material = mass->bodies[i].material;
        else if (mass->bodies[i].material != fresh.material) {
          fresh.material = "mixed";
          break;
        }
      }
    }
    table.rows.push_back(std::move(fresh));
  }
  for (const BomRow& r : table.rows) {
    if (r.massKnown)
      table.totalMassG += r.unitMassG * r.quantity;
    else
      table.massKnown = false;
  }
  if (!table.massKnown)
    table.totalMassG = 0.0;
  return table;
}

std::string BomToCsv(const BomTable& bom) {
  auto field = [](const std::string& s) {
    if (s.find_first_of(",\"\n") == std::string::npos) return s;
    std::string quoted = "\"";
    for (char c : s) { if (c == '"') quoted += '"'; quoted += c; }
    return quoted + "\"";
  };
  std::string csv = "part,quantity,material,unit_volume_mm3,unit_mass_g,total_mass_g\n";
  char buf[64];
  auto num = [&buf](double v) {
    snprintf(buf, sizeof buf, "%.6g", v);
    return std::string(buf);
  };
  for (const BomRow& r : bom.rows) {
    csv += field(r.partName) + "," + std::to_string(r.quantity) + "," +
           field(r.material) + "," + num(r.unitVolume) + ",";
    if (r.massKnown)
      csv += num(r.unitMassG) + "," + num(r.unitMassG * r.quantity);
    else
      csv += ",";
    csv += "\n";
  }
  csv += "total,,,,,";
  if (bom.massKnown) csv += num(bom.totalMassG);
  csv += "\n";
  return csv;
}

bool PickAssemblyFace(AppState& state, const Ray& ray, AssemblyFacePick& out) {
  if (!state.assembly || !state.faceSelector)
    return false;
  const RayHit hit = state.faceSelector->PickFace(ray);
  if (!hit.hit)
    return false;

  // meshId → the occurrence + which of its part's bodies was hit (both carried
  // on the render mesh precisely so an assembly pick can be named).
  const DocumentRenderMesh* rm = nullptr;
  for (const DocumentRenderMesh& m : state.documentMeshes)
    if (m.meshId == hit.meshId) { rm = &m; break; }
  if (!rm || rm->occurrenceId == 0)
    return false;

  const Occurrence* occ = state.assembly->FindById(rm->occurrenceId);
  if (!occ)
    return false;
  Document* part = PartDocById(state, occ->partId);
  if (!part || rm->bodyIndex >= part->GetBodies().size())
    return false;

  // The face's persistent toponaming name → an ElementRef the mate stores.
  const Body& body = part->GetBodies()[rm->bodyIndex];
  const std::string* name =
      body.elementMap.NameOf(static_cast<int>(hit.faceIndex));
  if (!name)
    return false; // an unnamed face (e.g. a pattern output) can't be mated

  out.occurrenceId = rm->occurrenceId;
  out.face = ElementRef{body.createdBy, *name};
  out.snapEdge = ElementRef{}; // default: face centroid

  // Inference snap (A2.5 phase 4): if the face has circular edges (holes/
  // bosses), snap the connector to the one nearest the click. Centers are in
  // part-local space; compare in world via the occurrence transform.
  std::vector<glm::vec3> centers;
  std::vector<int> edgeIdx;
  body.shape.FaceCircleCenters(static_cast<int>(hit.faceIndex), centers, edgeIdx);
  if (!centers.empty()) {
    const glm::mat4 xf = OccurrenceMatrix(*occ);
    float best = FLT_MAX;
    int bestI = -1;
    for (size_t k = 0; k < centers.size(); k++) {
      const glm::vec3 w = glm::vec3(xf * glm::vec4(centers[k], 1.0f));
      const float d = glm::length(w - hit.point);
      if (d < best) { best = d; bestI = static_cast<int>(k); }
    }
    if (bestI >= 0) {
      if (const std::string* en = body.elementMap.EdgeNameOf(edgeIdx[bestI]))
        out.snapEdge = ElementRef{body.createdBy, *en}; // named → persistable
    }
  }
  return true;
}

std::optional<glm::mat4> ConnectorWorldFrame(AppState& state,
                                             const Connector& c) {
  if (!state.assembly)
    return std::nullopt;
  const assembly::PartResolver resolve =
      [&state](uint64_t partId) -> const Document* {
    return PartDocById(state, partId);
  };
  const auto local = assembly::ConnectorLocalFrame(c, *state.assembly, resolve);
  if (!local)
    return std::nullopt;
  const Occurrence* occ = state.assembly->FindById(c.occurrenceId);
  if (!occ)
    return std::nullopt;
  return OccurrenceMatrix(*occ) * *local;
}

void StartMateTool(AppState& state, MateType type) {
  if (!state.assembly)
    return;
  CancelMateTool(state); // discard any in-progress preview first
  state.mateTool = AppState::MateToolState{};
  state.mateTool.active = true;
  state.mateTool.type = type;
  state.mateTool.message = "Pick the first face (connector A)";
  SelectOccurrence(state, 0); // the tool owns clicks now
}

void MateToolPick(AppState& state, const Ray& ray) {
  if (!state.assembly || !state.mateTool.active ||
      state.mateTool.previewMateId != 0)
    return;
  AssemblyFacePick pick;
  if (!PickAssemblyFace(state, ray, pick)) {
    state.mateTool.message = "No named face there — pick a face of a part";
    return;
  }
  auto& tool = state.mateTool;
  if (!tool.haveFirst) {
    tool.first.occurrenceId = pick.occurrenceId;
    tool.first.anchor.origin = false;
    tool.first.anchor.face = pick.face;
    tool.first.anchor.snapEdge = pick.snapEdge; // hole-center snap, if any
    tool.haveFirst = true;
    tool.message = pick.snapEdge.IsSet()
                       ? "Snapped to a hole center — pick connector B"
                       : "Pick the second face (connector B)";
    return;
  }
  if (pick.occurrenceId == tool.first.occurrenceId) {
    tool.message = "Pick a face on a different occurrence";
    return;
  }
  // Second connector: add a live preview mate (snapshot first, so accept is a
  // single undo step and cancel just pops it) and solve.
  Mate mate;
  mate.type = tool.type;
  mate.name = "Mate";
  mate.a = tool.first;
  mate.b.occurrenceId = pick.occurrenceId;
  mate.b.anchor.origin = false;
  mate.b.anchor.face = pick.face;
  mate.b.anchor.snapEdge = pick.snapEdge; // hole-center snap, if any
  SnapshotAssembly(state);
  tool.previewMateId = state.assembly->AddMate(mate);
  const int errored = SolveActiveAssembly(state);
  tool.message = errored ? "Over-constrained — try Flip, or Cancel"
                         : "Accept, Flip, or Cancel";
}

void AcceptMateTool(AppState& state) {
  if (!state.mateTool.active)
    return;
  const uint64_t id = state.mateTool.previewMateId;
  state.mateTool = AppState::MateToolState{}; // keep the mate; clear the tool
  if (id)
    state.selectedMate = id;
}

void CancelMateTool(AppState& state) {
  auto& tool = state.mateTool;
  if (tool.previewMateId && state.assembly) {
    state.assembly->RemoveMate(tool.previewMateId);
    if (!state.assemblyHistory.undo.empty())
      state.assemblyHistory.undo.pop_back(); // drop the snapshot we pushed
    SolveActiveAssembly(state);
  }
  tool = AppState::MateToolState{};
}

namespace {
void AppendFrameTriad(std::vector<LineVertex3D>& lines, const glm::mat4& f,
                      float scale) {
  const glm::vec3 o(f[3]);
  const glm::vec3 cols[3] = {{0.90f, 0.32f, 0.32f},
                             {0.35f, 0.82f, 0.38f},
                             {0.38f, 0.55f, 0.95f}};
  for (int a = 0; a < 3; a++) {
    lines.push_back(LineVertex(o, cols[a]));
    lines.push_back(LineVertex(o + glm::vec3(f[a]) * scale, cols[a]));
  }
}
} // namespace

void BuildMateToolOverlay(AppState& state, std::vector<LineVertex3D>& lines) {
  if (!state.assembly || !state.mateTool.active)
    return;
  const float scale = 1.2f;
  if (state.mateTool.haveFirst)
    if (const auto f = ConnectorWorldFrame(state, state.mateTool.first))
      AppendFrameTriad(lines, *f, scale);
  if (const Mate* m = state.mateTool.previewMateId
                          ? state.assembly->FindMateById(state.mateTool.previewMateId)
                          : nullptr) {
    if (const auto fb = ConnectorWorldFrame(state, m->b))
      AppendFrameTriad(lines, *fb, scale);
  }
}

// ---- Occurrence drag gizmo (A2.5) -----------------------------------------

namespace {
const glm::vec3 kGizmoAxis[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
const glm::vec3 kGizmoColor[3] = {
    {0.90f, 0.32f, 0.32f}, {0.35f, 0.82f, 0.38f}, {0.38f, 0.55f, 0.95f}};

// Roughly-constant on-screen gizmo size from the camera distance.
float GizmoScale(const AppState& state) {
  return state.camera ? std::max(0.13f * state.camera->GetDistance(), 0.1f)
                      : 1.0f;
}

// A deterministic unit vector perpendicular to `a`.
glm::vec3 PerpTo(const glm::vec3& a) {
  const glm::vec3 ref = std::abs(a.y) < 0.9f ? glm::vec3(0, 1, 0)
                                             : glm::vec3(1, 0, 0);
  return glm::normalize(glm::cross(a, ref));
}

// Coordinate `t` along the axis line (C + A·t) closest to the ray (O + D·s).
float RayAxisParam(const glm::vec3& O, const glm::vec3& D, const glm::vec3& C,
                   const glm::vec3& A) {
  const float b = glm::dot(D, A);
  const glm::vec3 w0 = O - C;
  const float d = glm::dot(D, w0), e = glm::dot(A, w0);
  const float denom = 1.0f - b * b;              // a=c=1 (D,A unit)
  if (std::abs(denom) < 1e-6f) return e;         // near-parallel: project O
  return (e - b * d) / denom;                    // axis coordinate tc
}

// Ray vs plane (point C, unit normal N). false if near-parallel / behind.
bool RayPlane(const glm::vec3& O, const glm::vec3& D, const glm::vec3& C,
              const glm::vec3& N, glm::vec3& hit) {
  const float dn = glm::dot(D, N);
  if (std::abs(dn) < 1e-6f) return false;
  const float t = glm::dot(C - O, N) / dn;
  if (t < 0.0f) return false;
  hit = O + D * t;
  return true;
}

// Perpendicular distance from point Q to the ray (O + D·s).
float PointRayDist(const glm::vec3& Q, const glm::vec3& O, const glm::vec3& D) {
  const glm::vec3 w = Q - O;
  return glm::length(w - glm::dot(w, D) * D);
}

// The angle of the ray's hit on the plane ⟂ A through C, in a fixed in-plane
// basis. Used for rotate handles.
bool RotateAngle(const glm::vec3& O, const glm::vec3& D, const glm::vec3& C,
                 const glm::vec3& A, float& angle) {
  glm::vec3 hit;
  if (!RayPlane(O, D, C, A, hit)) return false;
  const glm::vec3 b1 = PerpTo(A), b2 = glm::cross(A, b1);
  const glm::vec3 v = hit - C;
  angle = std::atan2(glm::dot(v, b2), glm::dot(v, b1));
  return true;
}

// The gizmo-anchored occurrence: selected, non-fixed, mate tool idle. null else.
Occurrence* GizmoOccurrence(AppState& state) {
  if (!state.assembly || !state.selectedOccurrence || state.mateTool.active)
    return nullptr;
  Occurrence* o = state.assembly->FindById(state.selectedOccurrence);
  if (!o) return nullptr;
  // Explode mode drags a view offset, not the placement — even a fixed base
  // part explodes.
  if (state.activeExplodedView) return o;
  return !o->fixed ? o : nullptr;
}

// The gizmo's frame + which handles show. A free occurrence gets world axes at
// its origin with all 6 handles; a mated occurrence gets its mate's connector-A
// world frame with only the mate's free DOF (drag-within-mate, phase 3).
struct GizmoSpec {
  glm::vec3 origin{0};
  glm::vec3 axis[3]{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  bool trans[3] = {false, false, false}; // translate arrow per axis
  bool rot[3] = {false, false, false};   // rotate arc per axis
};

// The mate whose placed (`b`) side is this occurrence, if any (its DOF the
// gizmo drives). Skips errored mates.
Mate* MateDrivingOccurrence(AssemblyDocument& asmDoc, uint64_t occId) {
  for (Mate& m : asmDoc.GetMates())
    if (m.b.occurrenceId == occId && m.error.empty()) return &m;
  return nullptr;
}

// Fills `spec` for the selected occurrence and reports the driving mate (null =
// free 6-DOF). Returns false if no gizmo should show (fastened mate / dangling
// connector).
bool BuildGizmoSpec(AppState& state, Occurrence* occ, GizmoSpec& spec,
                    Mate*& mate) {
  mate = nullptr;
  if (state.activeExplodedView) {
    // Explode authoring (A4): world-aligned translate arrows at the DISPLAYED
    // position; the drag writes the view's step offset. No rotation — an
    // exploded view is pure displacement.
    if (!state.assembly->FindExplodedViewById(state.activeExplodedView))
      return false;
    spec.origin = occ->position + ExplodeOffset(state, occ->id);
    for (int i = 0; i < 3; i++) {
      spec.axis[i] = kGizmoAxis[i];
      spec.trans[i] = true;
    }
    return true;
  }
  mate = MateDrivingOccurrence(*state.assembly, occ->id);
  if (!mate) {
    spec.origin = occ->position; // free: world axes, all handles
    for (int i = 0; i < 3; i++) {
      spec.axis[i] = kGizmoAxis[i];
      spec.trans[i] = spec.rot[i] = true;
    }
    return true;
  }
  const auto Wa = ConnectorWorldFrame(state, mate->a);
  if (!Wa) return false; // dangling connector → no meaningful DOF gizmo
  spec.origin = glm::vec3((*Wa)[3]);
  for (int i = 0; i < 3; i++)
    spec.axis[i] = glm::normalize(glm::vec3((*Wa)[i]));
  switch (mate->type) {
    case MateType::Revolute: spec.rot[2] = true; break;
    case MateType::Slider: spec.trans[2] = true; break;
    case MateType::Cylindrical: spec.rot[2] = spec.trans[2] = true; break;
    case MateType::Planar:
      spec.trans[0] = spec.trans[1] = spec.rot[2] = true;
      break;
    case MateType::Ball: spec.rot[0] = spec.rot[1] = spec.rot[2] = true; break;
    case MateType::Fastened: return false; // no DOF
  }
  return true;
}

// Ray vs the spec's enabled handles → (axis, rotate). Arrows win ties.
bool GizmoHitTest(const AppState& state, const GizmoSpec& spec, const Ray& ray,
                  int& axis, bool& rotate) {
  const float scale = GizmoScale(state);
  const float grab = 0.14f * scale, L = scale, R = 0.78f * scale;
  float best = grab;
  bool found = false;
  for (int i = 0; i < 3; i++) {
    const glm::vec3 A = spec.axis[i];
    if (spec.trans[i]) {
      float t = std::clamp(
          RayAxisParam(ray.origin, ray.direction, spec.origin, A), 0.15f * L, L);
      const float da =
          PointRayDist(spec.origin + A * t, ray.origin, ray.direction);
      if (da < best) { best = da; axis = i; rotate = false; found = true; }
    }
    if (spec.rot[i]) {
      glm::vec3 hit;
      if (RayPlane(ray.origin, ray.direction, spec.origin, A, hit)) {
        const float dr = std::abs(glm::length(hit - spec.origin) - R);
        if (dr < best) { best = dr; axis = i; rotate = true; found = true; }
      }
    }
  }
  return found;
}

// Push a dragged occurrence's live transform straight to the renderers (no
// version bump / resync) for every render mesh of the occurrence.
void PushOccurrenceLive(AppState& state, uint64_t occId) {
  const Occurrence* occ = state.assembly->FindById(occId);
  if (!occ) return;
  const glm::mat4 xf = DisplayMatrix(state, *occ);
  for (const DocumentRenderMesh& m : state.documentMeshes) {
    if (m.occurrenceId != occId) continue;
    if (state.meshRenderer) state.meshRenderer->SetInstanceTransform(m.meshId, xf);
    if (state.outlineRenderer) state.outlineRenderer->SetMeshTransform(m.outlineId, xf);
  }
}

// Appends one axis's handles (arrow and/or arc) per the spec.
void AppendGizmoHandles(const GizmoSpec& spec, float L,
                        std::vector<LineVertex3D>& lines) {
  const float R = 0.78f * L, head = 0.18f * L;
  for (int i = 0; i < 3; i++) {
    const glm::vec3 A = spec.axis[i], col = kGizmoColor[i], C = spec.origin;
    if (spec.trans[i]) {
      const glm::vec3 tip = C + A * L, side = PerpTo(A);
      lines.push_back(LineVertex(C, col));
      lines.push_back(LineVertex(tip, col));
      lines.push_back(LineVertex(tip, col));
      lines.push_back(LineVertex(tip - A * head + side * head, col));
      lines.push_back(LineVertex(tip, col));
      lines.push_back(LineVertex(tip - A * head - side * head, col));
    }
    if (spec.rot[i]) {
      const glm::vec3 b1 = PerpTo(A), b2 = glm::cross(A, b1);
      const int seg = 40;
      glm::vec3 prev = C + b1 * R;
      for (int s = 1; s <= seg; s++) {
        const float a = (float)s / seg * glm::two_pi<float>();
        const glm::vec3 p = C + (std::cos(a) * b1 + std::sin(a) * b2) * R;
        lines.push_back(LineVertex(prev, col));
        lines.push_back(LineVertex(p, col));
        prev = p;
      }
    }
  }
}
} // namespace

void BuildGizmoOverlay(AppState& state, std::vector<LineVertex3D>& lines) {
  Occurrence* occ = GizmoOccurrence(state);
  if (!occ) return;
  GizmoSpec spec;
  Mate* mate = nullptr;
  if (!BuildGizmoSpec(state, occ, spec, mate)) return;
  AppendGizmoHandles(spec, GizmoScale(state), lines);
}

bool HandleGizmoInput(AppState& state, const Ray& ray, bool clicked, bool down,
                      bool released) {
  auto& g = state.gizmoDrag;

  if (g.active) {
    Occurrence* occ =
        state.assembly ? state.assembly->FindById(state.selectedOccurrence)
                       : nullptr;
    Mate* mate = g.mateId ? state.assembly->FindMateById(g.mateId) : nullptr;
    if (released || !down || !occ || (g.mateId && !mate)) {
      g.active = false;
      if (state.assembly) {
        if (g.explodeView)
          state.assembly->BumpVersion(); // view edit: resync, nothing to solve
        else
          SolveActiveAssembly(state); // one undo step + resync
      }
      return true;
    }
    const glm::vec3 A = g.frameAxis[g.axis];
    if (g.explodeView) {
      // Explode authoring: the drag writes the step's offset so that the
      // DISPLAYED position follows the mouse (offset scales by 1/factor).
      ExplodedView* view = state.assembly->FindExplodedViewById(g.explodeView);
      ExplodeStep* step = nullptr;
      if (view)
        for (ExplodeStep& s : view->steps)
          if (s.occurrenceId == occ->id) { step = &s; break; }
      if (!step) { // view or step vanished mid-drag (undo?) — abort cleanly
        g.active = false;
        return true;
      }
      const float t = RayAxisParam(ray.origin, ray.direction, g.frameOrigin, A);
      const float f = std::max(state.explodeFactor, 0.05f); // never divide by ~0
      step->offset = g.startOffset + A * ((t - g.startParam) / f);
      PushOccurrenceLive(state, occ->id);
      return true;
    }
    if (!g.mateId) {
      // Free occurrence: drag its transform directly.
      if (!g.rotate) {
        const float t = RayAxisParam(ray.origin, ray.direction, g.frameOrigin, A);
        occ->position = g.startPos + A * (t - g.startParam);
      } else {
        float ang;
        if (RotateAngle(ray.origin, ray.direction, g.frameOrigin, A, ang))
          occ->orientation = glm::angleAxis(ang - g.startParam, A) * g.startRot;
      }
    } else {
      // Drag-within-mate: write the mate's DOF, then re-solve so the
      // occurrence follows.
      if (!g.rotate) {
        const float t = RayAxisParam(ray.origin, ray.direction, g.frameOrigin, A);
        const float v = g.startDof + (t - g.startParam);
        if (g.axis == 0) mate->dx = v;
        else if (g.axis == 1) mate->dy = v;
        else mate->offset = v;
      } else {
        float ang;
        if (RotateAngle(ray.origin, ray.direction, g.frameOrigin, A, ang)) {
          const float d = ang - g.startParam;
          if (mate->type == MateType::Ball)
            mate->ballRotation = glm::angleAxis(d, A) * g.startBall;
          else
            mate->rotation = g.startDof + d;
        }
      }
      SolveActiveAssembly(state);
      return true; // solve already repositioned + resynced
    }
    PushOccurrenceLive(state, occ->id);
    return true;
  }

  Occurrence* occ = GizmoOccurrence(state);
  if (!occ || !clicked) return false;
  GizmoSpec spec;
  Mate* mate = nullptr;
  if (!BuildGizmoSpec(state, occ, spec, mate)) return false;
  int axis = 0;
  bool rotate = false;
  if (!GizmoHitTest(state, spec, ray, axis, rotate)) return false;

  SnapshotAssembly(state); // one snapshot per drag
  g.active = true;
  g.axis = axis;
  g.rotate = rotate;
  g.frameOrigin = spec.origin;
  for (int i = 0; i < 3; i++) g.frameAxis[i] = spec.axis[i];
  g.mateId = mate ? mate->id : 0;
  g.explodeView = state.activeExplodedView;
  if (g.explodeView) {
    // Find-or-create this occurrence's step in the active view (after the
    // snapshot, so the whole drag is one undo step).
    ExplodedView* view = state.assembly->FindExplodedViewById(g.explodeView);
    ExplodeStep* step = nullptr;
    for (ExplodeStep& s : view->steps)
      if (s.occurrenceId == occ->id) { step = &s; break; }
    if (!step) {
      view->steps.push_back({occ->id, glm::vec3(0.0f)});
      step = &view->steps.back();
    }
    g.startOffset = step->offset;
  }
  g.startPos = occ->position;
  g.startRot = occ->orientation;
  const glm::vec3 A = spec.axis[axis];
  if (!rotate)
    g.startParam = RayAxisParam(ray.origin, ray.direction, spec.origin, A);
  else {
    float ang = 0.0f;
    RotateAngle(ray.origin, ray.direction, spec.origin, A, ang);
    g.startParam = ang;
  }
  if (mate) { // capture the DOF value at grab
    g.startDof = rotate ? mate->rotation
                        : (axis == 0 ? mate->dx : axis == 1 ? mate->dy
                                                            : mate->offset);
    g.startBall = mate->ballRotation;
  }
  return true; // consumed the click (don't also re-select)
}

bool AutoExplode(AppState& state, uint64_t viewId) {
  if (!state.assembly)
    return false;
  ExplodedView* view = state.assembly->FindExplodedViewById(viewId);
  if (!view)
    return false;

  // World-space center of every placed occurrence + the whole-assembly box.
  struct OccCenter { uint64_t id; glm::vec3 center; };
  std::vector<OccCenter> centers;
  glm::vec3 lo(FLT_MAX), hi(-FLT_MAX);
  for (const Occurrence& occ : state.assembly->GetOccurrences()) {
    Document* part = PartDocById(state, occ.partId);
    if (!part)
      continue;
    const glm::mat4 xf = OccurrenceMatrix(occ);
    glm::vec3 bmin(FLT_MAX), bmax(-FLT_MAX);
    bool any = false;
    for (const auto& body : part->GetBodies()) {
      glm::vec3 mn, mx;
      body.shape.GetBoundingBox(mn, mx);
      for (int c = 0; c < 8; c++) {
        const glm::vec3 corner((c & 1) ? mx.x : mn.x, (c & 2) ? mx.y : mn.y,
                               (c & 4) ? mx.z : mn.z);
        const glm::vec3 w = glm::vec3(xf * glm::vec4(corner, 1.0f));
        bmin = glm::min(bmin, w);
        bmax = glm::max(bmax, w);
        any = true;
      }
    }
    if (!any)
      continue;
    centers.push_back({occ.id, (bmin + bmax) * 0.5f});
    lo = glm::min(lo, bmin);
    hi = glm::max(hi, bmax);
  }
  if (centers.size() < 2)
    return false; // nothing to spread apart

  glm::vec3 centroid(0.0f);
  for (const OccCenter& c : centers) centroid += c.center;
  centroid /= static_cast<float>(centers.size());
  const float spread = 0.6f * glm::length(hi - lo);

  SnapshotAssembly(state);
  view->steps.clear();
  for (size_t i = 0; i < centers.size(); i++) {
    glm::vec3 dir = centers[i].center - centroid;
    if (glm::length(dir) < 1e-6f)
      dir = kGizmoAxis[i % 3]; // concentric parts: cycle world axes
    else
      dir = glm::normalize(dir);
    view->steps.push_back({centers[i].id, dir * spread});
  }
  state.assembly->BumpVersion();
  return true;
}

void ApplyExplodeTransforms(AppState& state) {
  if (!state.assembly)
    return;
  for (const Occurrence& occ : state.assembly->GetOccurrences())
    PushOccurrenceLive(state, occ.id);
}

void FitCameraToDocument(AppState& state) {
  if (!state.camera)
    return;

  glm::vec3 min(FLT_MAX), max(-FLT_MAX);
  bool any = false;
  if (state.assembly) {
    // Bound every visible occurrence's part bodies, transformed into place.
    for (const Occurrence& occ : state.assembly->GetOccurrences()) {
      if (!occ.visible)
        continue;
      Document* part = PartDocById(state, occ.partId);
      if (!part)
        continue;
      const glm::mat4 xf = DisplayMatrix(state, occ); // fit exploded extents
      for (const auto& body : part->GetBodies()) {
        glm::vec3 bmin, bmax;
        body.shape.GetBoundingBox(bmin, bmax);
        for (int c = 0; c < 8; c++) { // transform all 8 corners
          const glm::vec3 corner((c & 1) ? bmax.x : bmin.x,
                                 (c & 2) ? bmax.y : bmin.y,
                                 (c & 4) ? bmax.z : bmin.z);
          const glm::vec3 w = glm::vec3(xf * glm::vec4(corner, 1.0f));
          min = glm::min(min, w);
          max = glm::max(max, w);
          any = true;
        }
      }
    }
  } else if (state.document) {
    for (const auto& body : state.document->GetBodies()) {
      glm::vec3 bmin, bmax;
      body.shape.GetBoundingBox(bmin, bmax);
      min = glm::min(min, bmin);
      max = glm::max(max, bmax);
      any = true;
    }
  }
  if (!any)
    return;

  const glm::vec3 center = (min + max) * 0.5f;
  const float radius = glm::length(max - min) * 0.5f;
  state.camera->SetTarget(center);
  state.camera->SetDistance(std::max(radius * 2.5f, 1.0f));
}

namespace {
// Muted, theme-matched palette cycled per body so bodies/parts read apart.
const glm::vec3 kBodyPalette[] = {
    {0.58f, 0.66f, 0.78f}, // steel blue
    {0.80f, 0.62f, 0.42f}, // amber
    {0.55f, 0.74f, 0.62f}, // sage
    {0.76f, 0.58f, 0.70f}, // mauve
    {0.72f, 0.72f, 0.55f}, // olive
};

// One part body, tessellated once and cached so N occurrences of the same part
// share a single GPU asset (the instancing lever). CPU copies are kept for the
// outline + face-selector, whose geometry sharing is a later optimization.
struct SharedBody {
  uint32_t assetId = 0;
  std::vector<Vertex3D> vertices;
  std::vector<uint32_t> indices;
  std::vector<uint32_t> triangleFaces;
  std::vector<std::vector<glm::vec3>> edges;
  glm::vec3 palette;
  bool hasMaterial = false;
  glm::vec3 albedo;
  glm::vec4 pbr;
};

// Renders an assembly: one asset per referenced part body, one instance per
// occurrence. Assets are shared across occurrences of the same part.
void SyncAssemblyRenderers(AppState& state) {
  std::unordered_map<uint64_t, std::vector<SharedBody>> byPart; // partId -> bodies
  size_t palette = 0;

  for (const Occurrence& occ : state.assembly->GetOccurrences()) {
    if (!occ.visible)
      continue; // hidden: not rendered, not pickable
    Document* part = PartDocById(state, occ.partId);
    if (!part)
      continue; // dangling reference (part deleted) — skip

    auto it = byPart.find(occ.partId);
    if (it == byPart.end()) {
      // First time we see this part: tessellate its bodies + create assets.
      std::vector<SharedBody> shared;
      const auto& bodies = part->GetBodies();
      for (const auto& body : bodies) {
        SharedBody sb;
        CADShape::Mesh cadMesh = body.shape.GenerateMesh(0.1);
        if (cadMesh.vertices.empty()) {
          shared.push_back(std::move(sb)); // keep index alignment; assetId 0
          continue;
        }
        sb.vertices = CADMeshConverter::ConvertCADMeshToVertices(cadMesh);
        sb.indices = cadMesh.indices;
        sb.triangleFaces = cadMesh.triangleFaces;
        sb.edges = body.shape.GenerateEdgePolylines(0.1);
        sb.palette = kBodyPalette[palette++ % std::size(kBodyPalette)];
        sb.assetId = state.meshRenderer->CreateAsset(sb.vertices, sb.indices);
        if (!body.material.empty()) {
          const auto& mat = CADMaterials::MaterialLibrary::GetMaterial(
              CADMaterials::MaterialLibrary::FromSlug(body.material));
          sb.hasMaterial = true;
          sb.albedo = mat.baseColor;
          sb.pbr = glm::vec4(mat.metallic, mat.roughness, mat.specular,
                             mat.reflectance);
        }
        shared.push_back(std::move(sb));
      }
      it = byPart.emplace(occ.partId, std::move(shared)).first;
    }

    const glm::mat4 xf = DisplayMatrix(state, occ); // solved ∘ explode offset
    for (size_t bi = 0; bi < it->second.size(); bi++) {
      const SharedBody& sb = it->second[bi];
      if (sb.assetId == 0)
        continue; // empty body
      uint32_t meshId = state.meshRenderer->AddInstance(sb.assetId, xf);
      if (sb.hasMaterial)
        state.meshRenderer->SetInstanceMaterial(meshId, sb.albedo, sb.pbr);
      else
        state.meshRenderer->SetInstanceColor(meshId, sb.palette);
      uint32_t outlineId = 0;
      if (state.outlineRenderer) {
        // Untransformed vertices + a per-mesh model matrix (A2.5): the outline
        // draws at the occurrence transform via a push constant, so a live drag
        // is a SetMeshTransform matrix update, not a vertex re-upload/resync.
        outlineId = state.outlineRenderer->AddMesh(sb.vertices, sb.indices);
        state.outlineRenderer->SetMeshTransform(outlineId, xf);
      }
      if (state.faceSelector)
        state.faceSelector->AddMesh(meshId, sb.vertices, sb.indices,
                                    sb.triangleFaces, xf, sb.edges);
      state.documentMeshes.push_back(
          {meshId, outlineId, bi, occ.id, sb.edges});
    }
  }
  // Resync recreates instances (highlight reset), so re-apply the selection.
  if (state.selectedOccurrence)
    SelectOccurrence(state, state.selectedOccurrence);
  LOG_INFO("Assembly synced: {} occurrences",
           state.assembly->OccurrenceCount());
}

// In-context editing (A4): appends the context assembly's other occurrences
// as GHOST instances around the active part — alpha-blended, unpickable, no
// outlines. The part stays at identity; each ghost renders at
// OccMatrix(edited)⁻¹ · OccMatrix(other), so every spatial relationship reads
// the same as in the assembly with zero changes to part tooling. Before
// placing ghosts the context re-solves if any referenced part regenerated
// (this runs inside the version-gated resync, so a part edit updates the
// ghosts in the same frame — mates track live).
void SyncInContextGhosts(AppState& state) {
  if (!state.inContext.assemblySlotId)
    return;
  PartSlot* asmSlot = nullptr;
  for (PartSlot& s : state.parts)
    if (s.kind == DocKind::Assembly && s.id == state.inContext.assemblySlotId)
      asmSlot = &s;
  if (!asmSlot || !asmSlot->assembly)
    return; // the context assembly is gone; the mode quietly shows nothing
  AssemblyDocument& asmDoc = *asmSlot->assembly;
  const assembly::PartResolver resolve =
      [&state](uint64_t partId) -> const Document* {
    return PartDocById(state, partId);
  };

  // Stale check against the stashed slot's stamps (the active-tab machinery
  // in SolveActiveAssembly/AssemblyPartsStale can't see a stashed assembly).
  bool stale = false;
  for (const Occurrence& occ : asmDoc.GetOccurrences()) {
    const Document* part = PartDocById(state, occ.partId);
    if (!part) continue;
    const auto it = asmSlot->solvedPartVersions.find(occ.partId);
    if (it == asmSlot->solvedPartVersions.end() ||
        it->second != part->Version()) {
      stale = true;
      break;
    }
  }
  if (stale) {
    assembly::Solve(asmDoc, resolve);
    asmDoc.BumpVersion();
    asmSlot->solvedPartVersions.clear();
    for (const Occurrence& occ : asmDoc.GetOccurrences())
      if (const Document* part = PartDocById(state, occ.partId))
        asmSlot->solvedPartVersions[occ.partId] = part->Version();
  }

  const Occurrence* edited = asmDoc.FindById(state.inContext.occurrenceId);
  if (!edited)
    return;
  const glm::mat4 inv = glm::inverse(OccurrenceMatrix(*edited));

  // One shared asset per context part body (as in SyncAssemblyRenderers),
  // minus everything a ghost doesn't need: outlines, picking, materials.
  const glm::vec3 kGhostTint(0.55f, 0.60f, 0.68f);
  std::unordered_map<uint64_t, std::vector<uint32_t>> byPart; // partId->assets
  for (const Occurrence& occ : asmDoc.GetOccurrences()) {
    if (!occ.visible || occ.id == edited->id)
      continue;
    Document* part = PartDocById(state, occ.partId);
    if (!part)
      continue;
    auto it = byPart.find(occ.partId);
    if (it == byPart.end()) {
      std::vector<uint32_t> assets;
      for (const auto& body : part->GetBodies()) {
        CADShape::Mesh cadMesh = body.shape.GenerateMesh(0.1);
        if (cadMesh.vertices.empty()) {
          assets.push_back(0); // keep index alignment
          continue;
        }
        assets.push_back(state.meshRenderer->CreateAsset(
            CADMeshConverter::ConvertCADMeshToVertices(cadMesh),
            cadMesh.indices));
      }
      it = byPart.emplace(occ.partId, std::move(assets)).first;
    }
    const glm::mat4 xf = inv * OccurrenceMatrix(occ);
    for (size_t bi = 0; bi < it->second.size(); bi++) {
      if (it->second[bi] == 0)
        continue; // empty body
      const uint32_t meshId = state.meshRenderer->AddInstance(it->second[bi], xf);
      state.meshRenderer->SetInstanceColor(meshId, kGhostTint);
      state.meshRenderer->SetInstanceGhost(meshId); // after color: sets alpha
      state.documentMeshes.push_back({meshId, /*outlineId=*/0, bi, occ.id, {}});
    }
  }
}
} // namespace

namespace {
// A body's export appearance: its material's PBR, or a palette color (cycled
// by `paletteIndex`) with a neutral-plastic default when unassigned — the
// same rule the renderer uses, so an export looks like the viewport.
void FillItemAppearance(const Body& body, size_t paletteIndex,
                        MeshExport::Item& item) {
  if (!body.material.empty()) {
    const auto& mat = CADMaterials::MaterialLibrary::GetMaterial(
        CADMaterials::MaterialLibrary::FromSlug(body.material));
    item.baseColor = mat.baseColor;
    item.metallic = mat.metallic;
    item.roughness = mat.roughness;
  } else {
    item.baseColor = kBodyPalette[paletteIndex % std::size(kBodyPalette)];
    item.metallic = 0.0f;
    item.roughness = 0.5f;
  }
}
} // namespace

std::vector<MeshExport::Item> BuildVisualExportItems(AppState& state) {
  std::vector<MeshExport::Item> items;
  size_t palette = 0;
  if (state.assembly) {
    // One item per (occurrence × body) at the occurrence's SOLVED placement
    // (never exploded — an export is the real assembly). Occurrences of one
    // part reuse the part's shapes, so the shared TShapes let the writers
    // instance the geometry rather than copy it.
    for (const Occurrence& occ : state.assembly->GetOccurrences()) {
      if (!occ.visible)
        continue;
      Document* part = PartDocById(state, occ.partId);
      if (!part)
        continue;
      const glm::mat4 xf = OccurrenceMatrix(occ); // solved, not exploded
      const auto& bodies = part->GetBodies();
      for (size_t bi = 0; bi < bodies.size(); bi++) {
        if (!bodies[bi].shape.IsValid())
          continue;
        MeshExport::Item item;
        item.shape = bodies[bi].shape;
        const std::string label = occ.name.empty()
                                      ? ("occ" + std::to_string(occ.id))
                                      : occ.name;
        item.name = bodies.size() > 1
                        ? label + "-" + bodies[bi].name
                        : label;
        item.transform = xf;
        FillItemAppearance(bodies[bi], palette++, item);
        items.push_back(std::move(item));
      }
    }
  } else if (state.document) {
    const auto& bodies = state.document->GetBodies();
    for (size_t bi = 0; bi < bodies.size(); bi++) {
      if (!bodies[bi].shape.IsValid())
        continue;
      MeshExport::Item item;
      item.shape = bodies[bi].shape;
      item.name = bodies[bi].name;
      FillItemAppearance(bodies[bi], palette++, item);
      items.push_back(std::move(item));
    }
  }
  return items;
}

namespace {
// Section caps (I3 v2, C4): builds a filled cut face for each body that meets
// the active section plane and adds it as an opaque instance, so the
// sectioned part reads solid instead of hollow. The cap sits ON the plane; it
// is nudged to the kept side (`-normal`) so the global shader clip doesn't
// discard it, its winding is made consistent so back-face culling can't hide
// it, and its normal faces the viewer for correct lighting. Cap instance ids
// go in state.sectionCapMeshes (removed on the next resync).
void AppendSectionCaps(AppState& state, const std::vector<Body>& bodies,
                       const std::vector<glm::vec3>& bodyColors) {
  const glm::vec3 nRaw(state.sectionPlane);
  if (glm::length(nRaw) < 1e-6f)
    return; // no active section
  const glm::vec3 n = glm::normalize(nRaw);
  const glm::vec3 planeOrigin = n * state.sectionPlane.w; // dot(origin,n)=w
  const std::vector<CADShape> caps = Measure::SectionCaps(bodies, planeOrigin, n);
  const float eps = 1e-3f; // nudge to the kept side; invisible at mm scale
  for (size_t bi = 0; bi < caps.size(); bi++) {
    if (!caps[bi].IsValid())
      continue;
    CADShape::Mesh cm = caps[bi].GenerateMesh(0.05);
    if (cm.vertices.empty())
      continue;
    const std::vector<Vertex3D> base =
        CADMeshConverter::ConvertCADMeshToVertices(cm);
    // Double-sided: the cut is viewed from the open (removed) side, but the
    // user can orbit either way, so emit both faces. Front copy carries +n,
    // back copy -n; back-face culling then draws exactly the side facing the
    // camera, correctly lit, from whichever side. Both are nudged onto the
    // kept half so the global shader clip keeps them.
    std::vector<Vertex3D> vs;
    vs.reserve(base.size() * 2);
    for (const Vertex3D& v : base) {
      Vertex3D f = v;
      f.normal = n;
      f.position -= n * eps;
      vs.push_back(f);
    }
    for (const Vertex3D& v : base) {
      Vertex3D b = v;
      b.normal = -n;
      b.position -= n * eps;
      vs.push_back(b);
    }
    const uint32_t half = static_cast<uint32_t>(base.size());
    std::vector<uint32_t> idx;
    idx.reserve(cm.indices.size() * 2);
    for (size_t t = 0; t + 2 < cm.indices.size(); t += 3) {
      const uint32_t a = cm.indices[t], b = cm.indices[t + 1],
                     c = cm.indices[t + 2];
      const glm::vec3& p0 = base[a].position;
      const glm::vec3& p1 = base[b].position;
      const glm::vec3& p2 = base[c].position;
      // Order the +n copy so its geometric winding front-faces +n.
      const bool flip = glm::dot(glm::cross(p1 - p0, p2 - p0), n) < 0.0f;
      const uint32_t f1 = flip ? c : b, f2 = flip ? b : c;
      idx.push_back(a); idx.push_back(f1); idx.push_back(f2);
      // The -n copy is the opposite winding, on the second vertex block.
      idx.push_back(half + a); idx.push_back(half + f2); idx.push_back(half + f1);
    }
    const uint32_t capId = state.meshRenderer->AddMesh(vs, idx);
    state.meshRenderer->SetMeshColor(
        capId, bi < bodyColors.size() ? bodyColors[bi] : glm::vec3(0.7f));
    state.sectionCapMeshes.push_back(capId);
  }
}
} // namespace

// ---- Drawing view projection (workstream 13, D2) --------------------------

namespace {

// One body of a view's source document, ready to project. `input.sourceId` is
// a slot index into the gathered list (how ProjectView tags each curve so the
// join finds this entry); `refId` is the SEMANTIC source a dimension attaches
// to — a body's createdBy for a part, or the occurrence id for an assembly.
struct SourceBody {
  drawing::InputBody input;
  uint64_t featureId = 0;    // the body's createdBy (owner of its element names)
  uint64_t refId = 0;        // semantic source id -> SheetCurve.sourceId
  const ElementMap* map = nullptr;
};

// Appends one part document's bodies at the given world placement.
void AppendBodies(const Document& doc, const glm::dmat4& xf, uint64_t occId,
                  std::vector<SourceBody>& out) {
  for (const Body& body : doc.GetBodies()) {
    if (body.shape.GetShape().IsNull()) continue;
    SourceBody sb;
    sb.input.shape = body.shape.GetShape();
    sb.input.transform = xf;
    sb.input.sourceId = out.size();  // slot index: picks this entry on join
    sb.featureId = body.createdBy;
    sb.refId = occId ? occId : body.createdBy;  // occurrence, else body owner
    sb.map = &body.elementMap;
    out.push_back(std::move(sb));
  }
}

// Gathers the projectable bodies of a drawing view's source document — a part
// (identity placement) or an assembly (each visible occurrence's bodies at its
// solved transform, tagged with the occurrence id). Returns false and sets
// *error when the source can't be resolved.
bool GatherSourceBodies(AppState& state, const DrawingView& view,
                        std::vector<SourceBody>& out, uint64_t* sourceVersion,
                        std::string* error) {
  for (size_t i = 0; i < state.parts.size(); i++) {
    if (state.parts[i].id != view.sourceDocId) continue;
    if (state.parts[i].kind == DocKind::Part) {
      const Document* doc = SlotDoc(state, i);
      if (!doc) { *error = "source document is empty"; return false; }
      *sourceVersion = doc->Version();
      AppendBodies(*doc, glm::dmat4(1.0), 0, out);
      return true;
    }
    if (state.parts[i].kind == DocKind::Assembly) {
      const AssemblyDocument* asmDoc = SlotAssembly(state, i);
      if (!asmDoc) { *error = "source assembly is empty"; return false; }
      *sourceVersion = asmDoc->Version();
      for (const Occurrence& occ : asmDoc->GetOccurrences()) {
        if (!occ.visible) continue;
        const Document* part = PartDocById(state, occ.partId);
        if (!part) continue;  // dangling part reference — skip
        AppendBodies(*part, glm::dmat4(OccurrenceMatrix(occ)), occ.id, out);
      }
      return true;
    }
    *error = "a drawing cannot be a source for another drawing";
    return false;
  }
  *error = "source document not found";
  return false;
}

// Projects one drawing view: resolves its source, runs HLR, places the curves
// on the sheet at the view's scale + position, and joins the index-space
// source refs to stable ElementRefs. A failure leaves `error` set and no
// geometry (callers keep the previous SheetView so a stale view isn't blanked).
drawing::SheetView ProjectDrawingView(AppState& state, const DrawingView& view) {
  drawing::SheetView result;
  result.viewId = view.id;

  std::vector<SourceBody> bodies;
  std::string error;
  if (!GatherSourceBodies(state, view, bodies, &result.solvedSourceVersion,
                          &error)) {
    result.error = error;
    return result;
  }
  if (bodies.empty()) {
    result.error = "source has no geometry to project";
    return result;
  }

  std::vector<drawing::InputBody> inputs;
  inputs.reserve(bodies.size());
  for (const SourceBody& sb : bodies) inputs.push_back(sb.input);

  glm::dvec3 viewDir, up;
  OrientationAxes(view.orientation, viewDir, up);
  drawing::ProjectionOptions opts;
  opts.hiddenLines = view.showHidden;
  auto projected = drawing::ProjectView(inputs, viewDir, up, opts, &error);
  if (!projected) {
    result.error = error;
    return result;
  }

  // Map an index-space SourceRef back to a stable ElementRef via the owning
  // body's ElementMap (sourceId is the body index in `bodies`).
  const auto bodyOf = [&](uint64_t sourceId) -> const SourceBody* {
    return sourceId < bodies.size() ? &bodies[sourceId] : nullptr;
  };

  const double s = view.scale != 0.0 ? view.scale : 1.0;

  // Folding alignment: a projected child is kept aligned to its base along the
  // shared sheet axis (design §2). Two orthographic views share sheet-X when
  // they map the same world direction to it (equal `right` vectors), and
  // sheet-Y when their `up` vectors match; the shared coordinate locks to the
  // base's, leaving the fold axis free. Iso shares neither -> placed freely.
  glm::dvec2 effectivePos = view.position;
  if (view.baseViewId) {
    if (const DrawingView* base = state.drawing->FindView(view.baseViewId)) {
      glm::dvec3 vd, up, bvd, bup;
      OrientationAxes(view.orientation, vd, up);
      OrientationAxes(base->orientation, bvd, bup);
      const glm::dvec3 right = glm::normalize(glm::cross(vd, up));
      const glm::dvec3 bright = glm::normalize(glm::cross(bvd, bup));
      constexpr double eps = 1e-6;
      if (glm::length(right - bright) < eps) effectivePos.x = base->position.x;
      if (glm::length(up - bup) < eps) effectivePos.y = base->position.y;
    }
  }

  glm::dvec2 lo(1e30), hi(-1e30);
  for (const drawing::ProjectedCurve& pc : projected->curves) {
    drawing::SheetCurve sc;
    sc.kind = pc.kind;
    sc.visible = pc.visible;
    sc.curveType = pc.curveType;
    sc.points.reserve(pc.points.size());
    for (const glm::dvec2& p : pc.points) {
      const glm::dvec2 sheet = effectivePos + s * p;
      sc.points.push_back(sheet);
      lo = glm::min(lo, sheet);
      hi = glm::max(hi, sheet);
    }
    if (const SourceBody* sb = bodyOf(pc.source.sourceId)) {
      sc.sourceId = sb->refId;  // occurrence id (assembly) or body owner (part)
      if (pc.source.edgeIndex >= 0 && sb->map) {
        if (const std::string* n = sb->map->EdgeNameOf(pc.source.edgeIndex))
          sc.edge = ElementRef{sb->featureId, *n};
      }
      if (pc.source.faceIndex >= 0 && sb->map) {
        if (const std::string* n = sb->map->NameOf(pc.source.faceIndex))
          sc.silFace = ElementRef{sb->featureId, *n};
      }
    }
    result.curves.push_back(std::move(sc));
  }
  if (!result.curves.empty()) {
    result.hasGeometry = true;
    result.bboxMin = lo;
    result.bboxMax = hi;
  }
  return result;
}

} // namespace

uint64_t DrawingSourceVersion(AppState& state, uint64_t sourceDocId) {
  for (size_t i = 0; i < state.parts.size(); i++) {
    if (state.parts[i].id != sourceDocId) continue;
    if (state.parts[i].kind == DocKind::Part) {
      const Document* doc = SlotDoc(state, i);
      return doc ? doc->Version() : 0;
    }
    if (state.parts[i].kind == DocKind::Assembly) {
      const AssemblyDocument* asmDoc = SlotAssembly(state, i);
      return asmDoc ? asmDoc->Version() : 0;
    }
  }
  return 0;
}

void UpdateDrawingRenders(AppState& state) {
  if (!state.drawing) return;
  std::map<uint64_t, drawing::SheetView> fresh;
  for (const DrawingView& view : state.drawing->GetViews()) {
    drawing::SheetView sv = ProjectDrawingView(state, view);
    // Keep last-good geometry on failure (design §1): a failed view shows its
    // previous curves with an error, rather than blanking the sheet.
    if (!sv.error.empty() && !sv.hasGeometry) {
      const auto prev = state.drawingRenders.find(view.id);
      if (prev != state.drawingRenders.end() && prev->second.hasGeometry) {
        drawing::SheetView kept = prev->second;
        kept.error = sv.error;
        kept.solvedSourceVersion = sv.solvedSourceVersion;
        fresh[view.id] = std::move(kept);
        continue;
      }
    }
    fresh[view.id] = std::move(sv);
  }
  state.drawingRenders = std::move(fresh);
}

void SyncDocumentRenderers(AppState& state) {
  if (!state.meshRenderer)
    return;
  // Which active tab drives the sync, and its change counter.
  uint64_t activeVersion;
  if (state.assembly)
    activeVersion = state.assembly->Version();
  else if (state.drawing)
    activeVersion = state.drawing->Version();
  else if (state.document)
    activeVersion = state.document->Version();
  else
    return;
  // Section caps (C4): a change to the clip plane forces a rebuild so the
  // filled cut faces follow it. Gated on no widget being active so caps
  // settle when the offset slider is released, not every drag frame (the
  // shader clip itself previews live regardless).
  if (state.sectionPlane != state.syncedSectionPlane &&
      !ImGui::IsAnyItemActive()) {
    state.syncedSectionPlane = state.sectionPlane;
    state.syncedDocumentVersion = UINT64_MAX; // force the rebuild below
  }
  if (activeVersion == state.syncedDocumentVersion)
    return;
  state.syncedDocumentVersion = activeVersion;

  for (const auto& m : state.documentMeshes) {
    state.meshRenderer->RemoveMesh(m.meshId);
    if (state.outlineRenderer)
      state.outlineRenderer->RemoveMesh(m.outlineId);
    if (state.faceSelector)
      state.faceSelector->RemoveMesh(m.meshId);
  }
  state.documentMeshes.clear();
  // Section caps live outside documentMeshes (no outline/pick, and they must
  // not disturb the body↔index alignment); remove them on every resync.
  for (uint32_t capId : state.sectionCapMeshes)
    state.meshRenderer->RemoveMesh(capId);
  state.sectionCapMeshes.clear();

  // A drawing tab has no 3D geometry — the meshes were just cleared and the
  // sheet renders through the 2D viewport overlay (DrawSheet). Nothing more.
  if (state.drawing)
    return;

  if (state.assembly) {
    SyncAssemblyRenderers(state);
    return;
  }

  size_t palette = 0;
  const auto& bodies = state.document->GetBodies();
  std::vector<glm::vec3> bodyColors(bodies.size(), glm::vec3(0.7f));
  for (size_t bi = 0; bi < bodies.size(); bi++) {
    const auto& body = bodies[bi];
    CADShape::Mesh cadMesh = body.shape.GenerateMesh(0.1);
    if (cadMesh.vertices.empty())
      continue;

    std::vector<Vertex3D> vertices =
        CADMeshConverter::ConvertCADMeshToVertices(cadMesh);
    uint32_t meshId = state.meshRenderer->AddMesh(vertices, cadMesh.indices);
    // Albedo is per-instance state (not baked into vertices): palette color
    // by default; a body with an assigned material overrides it with the
    // material's base color and real PBR params.
    glm::vec3 albedo = kBodyPalette[palette++ % std::size(kBodyPalette)];
    state.meshRenderer->SetMeshColor(meshId, albedo);
    if (!body.material.empty()) {
      const auto& mat = CADMaterials::MaterialLibrary::GetMaterial(
          CADMaterials::MaterialLibrary::FromSlug(body.material));
      state.meshRenderer->SetMeshMaterial(
          meshId, mat.baseColor,
          glm::vec4(mat.metallic, mat.roughness, mat.specular, mat.reflectance));
      albedo = mat.baseColor;
    }
    bodyColors[bi] = albedo; // section caps reuse the body's color
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
        {meshId, outlineId, bi, /*occurrenceId=*/0, std::move(edges)});
  }

  AppendSectionCaps(state, bodies, bodyColors);

  // In-context editing (A4): ghost the context assembly around the part.
  // Appended AFTER the part bodies so documentMeshes[i] still aligns with
  // GetBodies()[i] for the part-tab consumers (get_state, face picking).
  SyncInContextGhosts(state);

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
        CADMeshConverter::ConvertCADMeshToVertices(cadMesh);
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
    std::vector<Vertex3D> quad = {
        {{center.x - half, y, center.z - half}, n},
        {{center.x + half, y, center.z - half}, n},
        {{center.x + half, y, center.z + half}, n},
        {{center.x - half, y, center.z + half}, n},
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

std::vector<LineVertex3D> BuildActiveSketchOverlay(AppState& state,
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

std::vector<LineVertex3D> BuildStaticSketchLines(AppState& state,
                                             uint64_t activeSketchId) {
  std::vector<LineVertex3D> lines;
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
  if (!state.document) // edge picking is a part-only flow; assembly tab: no-op
    return std::nullopt;
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

namespace {
// A measured element as a one-line readout.
std::string FormatElement(const Measure::Element& e) {
  char buf[128];
  if (e.kind == "face") {
    snprintf(buf, sizeof buf, "Face area: %.3f mm\xc2\xb2", e.area);
  } else if (e.hasRadius) {
    snprintf(buf, sizeof buf, "Edge length: %.3f mm   radius: %.3f mm",
             e.length, e.radius);
  } else {
    snprintf(buf, sizeof buf, "Edge length: %.3f mm", e.length);
  }
  return buf;
}
} // namespace

bool HandleMeasureInput(AppState& state, bool overViewport) {
  if (!state.measure.active)
    return false;
  // Escape exits the tool (clearing any in-progress pair + readout).
  if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    state.measure = MeasureState{};
    return true;
  }
  state.measure.hasHover = false;
  if (!state.document || !state.faceSelector || !overViewport)
    return true; // the mode owns the viewport even with nothing under-cursor

  ImGuiIO& io = ImGui::GetIO();
  const Ray ray = state.faceSelector->ScreenToWorldRay(
      io.MousePos.x - state.viewportPos.x, io.MousePos.y - state.viewportPos.y,
      static_cast<int>(state.viewportSize.x),
      static_cast<int>(state.viewportSize.y), *state.camera);
  const EdgeHit edge = state.faceSelector->PickEdge(ray);
  const RayHit face = state.faceSelector->PickFace(ray);
  // Prefer an edge when the cursor is near one and it isn't behind the face
  // under the cursor (edges sit on the surface, so their ray distance is ≈
  // the face's at the silhouette; a much larger distance means it's occluded).
  const bool useEdge =
      edge.hit && (!face.hit || edge.distance <= face.distance + 0.25f);
  if (useEdge) {
    state.measure.hasHover = true;
    state.measure.hoverIsEdge = true;
    state.measure.hoverMeshId = edge.meshId;
    state.measure.hoverIndex = edge.edgeIndex;
  } else if (face.hit) {
    state.measure.hasHover = true;
    state.measure.hoverIsEdge = false;
    state.measure.hoverMeshId = face.meshId;
    state.measure.hoverIndex = face.faceIndex;
  }

  if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && state.measure.hasHover) {
    const bool isEdge = state.measure.hoverIsEdge;
    const auto ref =
        isEdge ? EdgeElementRef(state, state.measure.hoverMeshId,
                                state.measure.hoverIndex)
               : FaceElementRef(state, state.measure.hoverMeshId,
                                state.measure.hoverIndex);
    if (!ref) {
      state.measure.readout =
          "That element has no stable name and can't be measured.";
      return true;
    }
    const auto& bodies = state.document->GetBodies();
    if (!state.measure.hasFirst) {
      // First pick: describe the element on its own.
      state.measure.first = *ref;
      state.measure.firstIsEdge = isEdge;
      state.measure.hasFirst = true;
      if (const auto e = Measure::Describe(bodies, *ref))
        state.measure.readout = FormatElement(*e);
      else
        state.measure.readout = "Could not measure that element.";
    } else {
      // Second pick: the distance between the two, then start fresh so a
      // click chain alternates describe → distance → describe → …
      const auto d = Measure::Distance(bodies, state.measure.first, *ref);
      char buf[96];
      if (d)
        snprintf(buf, sizeof buf, "Distance: %.3f mm", *d);
      else
        snprintf(buf, sizeof buf, "Could not measure the distance.");
      state.measure.readout = buf;
      state.measure.hasFirst = false;
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
