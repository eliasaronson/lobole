#include "ui/AppActions.h"

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
#include "ui/CamPlayback.h"  // SyncCamPlayback (SyncRenderersToDocument)
#include "ui/FastenerTool.h" // SyncFastenerGhost (SyncRenderersToDocument)
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

namespace ui {

// SyncAssemblyRenderers / SyncInContextGhosts / EvictPartRenderCache are
// defined in AssemblyActions.cpp (the assembly domain, extracted from this TU)
// and declared in AppActionsInternal.h, which this TU now includes for the
// render-cache types — so the hand-copied declarations that used to sit here
// are gone, and cannot drift from the real ones.

uint64_t SelectedFeatureId(const AppState& state) {
  const SelectionItem* it = state.selection.First(SelectionItem::Kind::Feature);
  return it != nullptr ? it->id : 0;
}

Feature* SelectedFeature(AppState& state, size_t* indexOut) {
  const uint64_t id = SelectedFeatureId(state);
  if (id == 0 || !state.document)
    return nullptr;
  return state.document->FindFeatureById(id, indexOut);
}

int SelectedFeatureIndexOrMinus1(AppState& state) {
  size_t index = 0;
  return SelectedFeature(state, &index) != nullptr ? static_cast<int>(index)
                                                   : -1;
}

void SelectFeature(AppState& state, uint64_t featureId) {
  state.selection.Replace({SelectionItem::Kind::Feature, featureId, {}});
}

void OpenFeatureEditor(AppState& state, uint64_t featureId) {
  if (!state.document || state.document->FindFeatureById(featureId) == nullptr)
    return;
  SelectFeature(state, featureId);
  state.editFeatureId = featureId; // the Tool window shows the editor session
}

void SelectFeatureByIndex(AppState& state, size_t index) {
  if (!state.document || index >= state.document->FeatureCount())
    return;
  SelectFeature(state, state.document->GetFeature(index)->GetId());
}

uint64_t SelectedOccurrenceId(const AppState& state) {
  const SelectionItem* it =
      state.selection.First(SelectionItem::Kind::Occurrence);
  return it != nullptr ? it->id : 0;
}

std::vector<uint64_t> SelectedOccurrenceIds(const AppState& state) {
  // The selection has always been a SET — the viewport has ctrl-clicked
  // occurrences into it since U1d — and SelectedOccurrenceId returning only
  // the FIRST is what made every consumer single-select. This is the same
  // question asked without that narrowing.
  std::vector<uint64_t> out;
  for (const SelectionItem& it : state.selection.items)
    if (it.kind == SelectionItem::Kind::Occurrence && it.id != 0)
      out.push_back(it.id);
  return out;
}

uint64_t SelectedMateId(const AppState& state) {
  const SelectionItem* it = state.selection.First(SelectionItem::Kind::Mate);
  return it != nullptr ? it->id : 0;
}

namespace {
// Replace only the given kind's items (occurrence and mate selections are
// independent, as their pre-U1 fields were); id 0 just removes the kind.
void ReplaceKind(Selection& sel, SelectionItem::Kind kind, uint64_t id) {
  auto& items = sel.items;
  items.erase(
      std::remove_if(items.begin(), items.end(),
                     [&](const SelectionItem& it) { return it.kind == kind; }),
      items.end());
  if (id != 0)
    items.push_back({kind, id, {}});
}
} // namespace

void SelectMate(AppState& state, uint64_t mateId) {
  ReplaceKind(state.selection, SelectionItem::Kind::Mate, mateId);
}

void PruneSelection(AppState& state) {
  auto& items = state.selection.items;
  items.erase(
      std::remove_if(
          items.begin(), items.end(),
          [&](const SelectionItem& it) {
            switch (it.kind) {
            case SelectionItem::Kind::Feature:
              return !state.document ||
                     state.document->FindFeatureById(it.id) == nullptr;
            case SelectionItem::Kind::Occurrence:
              return !state.assembly ||
                     state.assembly->FindById(it.id) == nullptr;
            case SelectionItem::Kind::Mate:
              return !state.assembly ||
                     state.assembly->FindMateById(it.id) == nullptr;
            case SelectionItem::Kind::Operation: {
              if (!state.job)
                return true;
              for (const cam::Setup& s : state.job->GetSetups())
                for (const cam::Operation& op : s.operations)
                  if (op.id == it.id)
                    return false;
              return true;
            }
            default:
              return false; // geometry refs resolve lazily; stale = no-op
            }
          }),
      items.end());
}

namespace {

// Sketch line color; the active sketch is drawn brighter.
constexpr glm::vec3 kSketchColor{0.40f, 0.72f, 1.00f};
constexpr glm::vec3 kActiveSketchColor{0.55f, 0.85f, 1.00f};

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

} // namespace

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
  for (size_t i = 0; i < state.document->FeatureCount(); i++) {
    auto* cv = dynamic_cast<CurveFeature*>(state.document->GetFeature(i));
    if (!cv)
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

// ---- Project-wide variables (Phase V) -------------------------------------

void SyncProjectScope(AppState& state) {
  // Re-evaluate the project table (project vars may reference each other — the
  // shared topo-sort orders them), then regenerate every part against it. A
  // changed project value bakes into each part's feature JSON, so dependent
  // features across all tabs invalidate through the regen cache naturally.
  EvaluateParameterTable(state.projectParameters, state.projectSymbols, nullptr,
                         {});
  for (size_t i = 0; i < state.parts.size(); i++) {
    if (Document* d = SlotDoc(state, i)) {
      d->SetProjectSymbols(&state.projectSymbols);
      d->SetProjectParameters(&state.projectParameters);
      d->Regenerate();
    }
  }
  state.syncedDocumentVersion = UINT64_MAX; // a cross-tab edit; force resync
}

const Parameter* FindProjectParameter(const AppState& state,
                                      const std::string& name) {
  for (const Parameter& p : state.projectParameters)
    if (p.name == name)
      return &p;
  return nullptr;
}

std::vector<std::string> ProjectVarReferences(AppState& state,
                                              const std::string& name) {
  const auto& engine = ExpressionEngine::Default();
  const auto references = [&](const std::string& text) {
    if (text.empty())
      return false;
    const auto compiled = engine.Compile(text);
    return compiled.has_value() && (*compiled)->Dependencies().contains(name);
  };
  std::vector<std::string> refs;
  for (const Parameter& p : state.projectParameters)
    if (p.name != name && references(p.expression))
      refs.push_back("project variable '" + p.name + "'");
  // A project variable's references span every part (walk all slots, not just
  // the active doc); a part with a same-named LOCAL variable shadows it, so its
  // references belong to the local one and don't block the project delete.
  for (size_t i = 0; i < state.parts.size(); i++) {
    Document* d = SlotDoc(state, i);
    if (d == nullptr || d->FindParameter(name) != nullptr)
      continue;
    for (const std::string& r : d->FindParameterReferences(name))
      refs.push_back(state.parts[i].name + ": " + r);
  }
  return refs;
}

namespace {
// Applies a project-var undo/redo snapshot: replay the cross-part reference
// rewrite (rename only), restore the table, regenerate all.
void ApplyProjectVarSnapshot(AppState& state, const ProjectVarSnapshot& s) {
  if (!s.docRefFrom.empty()) {
    for (size_t i = 0; i < state.parts.size(); i++)
      if (Document* d = SlotDoc(state, i))
        d->RewriteProjectVarRefs(s.docRefFrom, s.docRefTo);
  }
  state.projectParameters = s.params;
  SyncProjectScope(state);
}
// Pushes a project-var undo entry capturing the PRE-edit table (restored on
// undo) plus the reference rewrite undo must replay across parts (empty for
// set/delete; the reverse rename for a rename). Clears the redo stack.
void PushProjectVarUndo(AppState& state, std::vector<Parameter> preEdit,
                        std::string undoRefFrom, std::string undoRefTo) {
  ProjectVarSnapshot s;
  s.params = std::move(preEdit);
  s.stamp = NextActionStamp();
  s.docRefFrom = std::move(undoRefFrom);
  s.docRefTo = std::move(undoRefTo);
  state.projectVarHistory.undo.push_back(std::move(s));
  state.projectVarHistory.redo.clear();
}
} // namespace

bool SetProjectVariable(AppState& state, const std::string& name,
                        const std::string& expression,
                        const std::string& comment) {
  if (!ExpressionEngine::Default().IsValidParameterName(name))
    return false;
  PushProjectVarUndo(state, state.projectParameters, {}, {});
  const auto it = std::find_if(
      state.projectParameters.begin(), state.projectParameters.end(),
      [&](const Parameter& p) { return p.name == name; });
  if (it != state.projectParameters.end()) {
    it->expression = expression;
    it->comment = comment;
  } else {
    state.projectParameters.push_back({name, expression, comment, 0.0, {}});
  }
  SyncProjectScope(state);
  return true;
}

bool RemoveProjectVariable(AppState& state, const std::string& name,
                           std::vector<std::string>* outRefs) {
  const auto it = std::find_if(
      state.projectParameters.begin(), state.projectParameters.end(),
      [&](const Parameter& p) { return p.name == name; });
  if (it == state.projectParameters.end())
    return false;
  std::vector<std::string> refs = ProjectVarReferences(state, name);
  if (!refs.empty()) {
    if (outRefs != nullptr)
      *outRefs = std::move(refs);
    return false;
  }
  PushProjectVarUndo(state, state.projectParameters, {}, {});
  state.projectParameters.erase(std::find_if(
      state.projectParameters.begin(), state.projectParameters.end(),
      [&](const Parameter& p) { return p.name == name; }));
  SyncProjectScope(state);
  return true;
}

bool RenameProjectVariable(AppState& state, const std::string& oldName,
                           const std::string& newName) {
  if (FindProjectParameter(state, oldName) == nullptr ||
      FindProjectParameter(state, newName) != nullptr ||
      !ExpressionEngine::Default().IsValidParameterName(newName)) {
    return false;
  }
  // Undo restores the pre-edit table AND reverses the ref rewrite (newName ->
  // oldName) across every part.
  PushProjectVarUndo(state, state.projectParameters, newName, oldName);
  const auto& engine = ExpressionEngine::Default();
  for (Parameter& p : state.projectParameters) {
    if (p.name == oldName)
      p.name = newName;
    p.expression = engine.RenameIdentifier(p.expression, oldName, newName);
  }
  for (size_t i = 0; i < state.parts.size(); i++)
    if (Document* d = SlotDoc(state, i))
      d->RewriteProjectVarRefs(oldName, newName);
  SyncProjectScope(state);
  return true;
}

bool UndoProjectVars(AppState& state) {
  if (state.projectVarHistory.undo.empty())
    return false;
  ProjectVarSnapshot entry = std::move(state.projectVarHistory.undo.back());
  state.projectVarHistory.undo.pop_back();
  // Build the redo entry from the CURRENT (post-edit) state before restoring;
  // its ref rewrite is the inverse of the undo entry's.
  ProjectVarSnapshot redoEntry;
  redoEntry.params = state.projectParameters;
  redoEntry.stamp = entry.stamp;
  redoEntry.docRefFrom = entry.docRefTo;
  redoEntry.docRefTo = entry.docRefFrom;
  state.projectVarHistory.redo.push_back(std::move(redoEntry));
  ApplyProjectVarSnapshot(state, entry);
  return true;
}

bool RedoProjectVars(AppState& state) {
  if (state.projectVarHistory.redo.empty())
    return false;
  ProjectVarSnapshot entry = std::move(state.projectVarHistory.redo.back());
  state.projectVarHistory.redo.pop_back();
  ProjectVarSnapshot undoEntry;
  undoEntry.params = state.projectParameters;
  undoEntry.stamp = entry.stamp;
  undoEntry.docRefFrom = entry.docRefTo;
  undoEntry.docRefTo = entry.docRefFrom;
  state.projectVarHistory.undo.push_back(std::move(undoEntry));
  ApplyProjectVarSnapshot(state, entry);
  return true;
}

// ---- Global undo routing (Phase V) ----------------------------------------
// A project-variable edit spans every tab, so it can't ride any one document's
// CommandStack. Rather than merge the (separate, per-tab) stacks, every
// undoable action is stamped from one monotonic action clock (NextActionStamp);
// the global Undo/Redo route to whichever source holds the most recent stamp.
// This keeps each per-tab stack intact and correct while making Ctrl+Z undo the
// user's genuinely-last action even when it crossed into project scope.

namespace {
uint64_t ActiveTabTopUndoStamp(const AppState& state) {
  if (state.assembly)
    return state.assemblyHistory.undoStamps.empty()
               ? 0
               : state.assemblyHistory.undoStamps.back();
  if (state.drawing)
    return state.drawingHistory.undoStamps.empty()
               ? 0
               : state.drawingHistory.undoStamps.back();
  if (state.job)
    return state.jobHistory.undoStamps.empty()
               ? 0
               : state.jobHistory.undoStamps.back();
  return state.commands ? state.commands->TopUndoStamp() : 0;
}
uint64_t ActiveTabTopRedoStamp(const AppState& state) {
  if (state.assembly)
    return state.assemblyHistory.redoStamps.empty()
               ? 0
               : state.assemblyHistory.redoStamps.back();
  if (state.drawing)
    return state.drawingHistory.redoStamps.empty()
               ? 0
               : state.drawingHistory.redoStamps.back();
  if (state.job)
    return state.jobHistory.redoStamps.empty()
               ? 0
               : state.jobHistory.redoStamps.back();
  return state.commands ? state.commands->TopRedoStamp() : 0;
}
void ActiveTabUndo(AppState& state) {
  if (state.assembly)
    UndoAssembly(state);
  else if (state.drawing)
    UndoDrawing(state);
  else if (state.job)
    UndoJob(state);
  else if (state.commands && state.commands->CanUndo())
    state.commands->Undo(*state.document);
}
void ActiveTabRedo(AppState& state) {
  if (state.assembly)
    RedoAssembly(state);
  else if (state.drawing)
    RedoDrawing(state);
  else if (state.job)
    RedoJob(state);
  else if (state.commands && state.commands->CanRedo())
    state.commands->Redo(*state.document);
}
} // namespace

bool CanGlobalUndo(const AppState& state) {
  return !state.projectVarHistory.undo.empty() ||
         ActiveTabTopUndoStamp(state) != 0;
}
bool CanGlobalRedo(const AppState& state) {
  return !state.projectVarHistory.redo.empty() ||
         ActiveTabTopRedoStamp(state) != 0;
}
void GlobalUndo(AppState& state) {
  const uint64_t proj = state.projectVarHistory.undo.empty()
                            ? 0
                            : state.projectVarHistory.undo.back().stamp;
  // Undo the most recent action: the larger stamp wins (stamps are unique).
  if (proj != 0 && proj > ActiveTabTopUndoStamp(state))
    UndoProjectVars(state);
  else
    ActiveTabUndo(state);
}
void GlobalRedo(AppState& state) {
  uint64_t proj = state.projectVarHistory.redo.empty()
                      ? UINT64_MAX
                      : state.projectVarHistory.redo.back().stamp;
  uint64_t tab = ActiveTabTopRedoStamp(state);
  if (tab == 0)
    tab = UINT64_MAX;
  if (proj == UINT64_MAX && tab == UINT64_MAX)
    return;
  // Redo the action undone most recently: the SMALLER redo-top stamp wins.
  if (proj < tab)
    RedoProjectVars(state);
  else
    ActiveTabRedo(state);
}

// An occurrence's placement as a model matrix (translate ∘ rotate).
glm::mat4 OccurrenceMatrix(const Occurrence& o) {
  return glm::translate(glm::mat4(1.0f), o.position) *
         glm::mat4_cast(o.orientation);
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
    if (s.occurrenceId == occId)
      return s.offset * state.explodeFactor;
  return glm::vec3(0.0f);
}

// What an occurrence renders (and picks) at: the solved placement, displaced
// by the active exploded view.
glm::mat4 DisplayMatrix(const AppState& state, const Occurrence& o) {
  return glm::translate(glm::mat4(1.0f), ExplodeOffset(state, o.id)) *
         OccurrenceMatrix(o);
}

glm::mat4 DisplayMatrixOf(const AppState& state, const Occurrence& o) {
  return DisplayMatrix(state, o);
}

// The part document an occurrence references, by stable id (active or stashed).
Document* PartDocById(AppState& state, uint64_t partId) {
  for (size_t i = 0; i < state.parts.size(); i++)
    if (state.parts[i].kind == DocKind::Part && state.parts[i].id == partId)
      return SlotDoc(state, i);
  return nullptr;
}

// The ASSEMBLY document an occurrence references (A5), by the same stable slot
// id — an occurrence's `partId` resolves against parts or assemblies
// depending on its refKind. Null when the id is not an assembly slot.
AssemblyDocument* AssemblyDocById(AppState& state, uint64_t slotId) {
  for (size_t i = 0; i < state.parts.size(); i++)
    if (state.parts[i].kind == DocKind::Assembly && state.parts[i].id == slotId)
      return SlotAssembly(state, i);
  return nullptr;
}

// The two resolvers Flatten wants, bound to this AppState. Every walker that
// needs the leaf parts of the active assembly goes through here, so none of
// them has to know how slots are stored.
assembly::PartResolver PartResolverFor(AppState& state) {
  return [&state](uint64_t id) -> const Document* {
    return PartDocById(state, id);
  };
}
assembly::AssemblyResolver AssemblyResolverFor(AppState& state) {
  return [&state](uint64_t id) -> const AssemblyDocument* {
    return AssemblyDocById(state, id);
  };
}

// The leaf parts of the active assembly, placed. Empty (with the reason
// logged once) when the tree cannot be walked — a cycle, or nesting past the
// depth cap. Callers render/aggregate what they get; refusing to draw
// anything at all would hide the model AND the message.
std::vector<assembly::FlatInstance> FlattenActiveAssembly(AppState& state) {
  if (!state.assembly)
    return {};
  auto flat = assembly::Flatten(*state.assembly, PartResolverFor(state),
                                AssemblyResolverFor(state));
  if (!flat) {
    LOG_ERROR("Assembly cannot be walked: {}", flat.error());
    return {};
  }
  return *flat;
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

// A thick screen-space line segment (two triangles). corner.x is the SIDE of
// the line, the same side at both ends: the vertex shader's normal is
// canonical for the segment whichever end a vertex sits at (sketch_prim.vert).
// It used to negate corner.x at the B end to undo a per-end normal, which
// kept the geometry right but twisted the anti-aliasing coordinate. Public:
// the selection pipeline draws its edge overlays with it too.
void AppendPrimLine(glm::vec3 a, glm::vec3 b, float halfPx,
                    const glm::vec4& color, std::vector<PrimVertex>& out) {
  PrimVertex am{a, b, {-1, 0}, {halfPx, 0}, color};
  PrimVertex ap{a, b, {1, 0}, {halfPx, 0}, color};
  PrimVertex bp{b, a, {1, 0}, {halfPx, 0}, color};
  PrimVertex bm{b, a, {-1, 0}, {halfPx, 0}, color};
  for (const PrimVertex& v : {am, ap, bp, am, bp, bm})
    out.push_back(v);
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

namespace {
// Vertices exposed by an entity for dots (which, plane-UV).
std::vector<std::pair<uint8_t, glm::vec2>> EntityDots(const sketch::Entity& e) {
  if (auto* p = std::get_if<sketch::Point>(&e))
    return {{0, p->p}};
  if (auto* l = std::get_if<sketch::Line>(&e))
    return {{0, l->a}, {1, l->b}};
  if (auto* c = std::get_if<sketch::Circle>(&e))
    return {{0, c->center}};
  if (auto* a = std::get_if<sketch::Arc>(&e))
    return {{0, a->center},
            {1, sketch::ArcPointUV(*a, 1)},
            {2, sketch::ArcPointUV(*a, 2)}};
  if (auto* el = std::get_if<sketch::Ellipse>(&e))
    return {{0, el->center}};
  if (auto* ea = std::get_if<sketch::EllipseArc>(&e))
    return {{0, ea->center},
            {1, sketch::EllipseArcPointUV(*ea, 1)},
            {2, sketch::EllipseArcPointUV(*ea, 2)}};
  if (auto* bs = std::get_if<sketch::BSpline>(&e)) {
    std::vector<std::pair<uint8_t, glm::vec2>> dots;
    const size_t nPoles = std::min<size_t>(bs->poles.size(), 256);
    for (size_t k = 0; k < nPoles; k++)
      dots.push_back({static_cast<uint8_t>(k), bs->poles[k]});
    return dots;
  }
  return {};
}

// A dashed line (world-space dashes) for construction/reference geometry.
void AppendPrimDashedLine(glm::vec3 a, glm::vec3 b, float halfPx,
                          const glm::vec4& color,
                          std::vector<PrimVertex>& out) {
  glm::vec3 d = b - a;
  float len = glm::length(d);
  if (!std::isfinite(len) || len < 1e-6f)
    return; // skip degenerate / non-finite (e.g. a solve that blew up)
  glm::vec3 dir = d / len;
  const float dash = 0.15f, gap = 0.10f, period = dash + gap;
  // Cap the dash count: an under-constrained drag can fling a point to a huge
  // coordinate, and an unbounded `t < len` loop would push billions of vertices
  // and exhaust memory. Past the cap the (off-screen) line simply stops
  // dashing.
  const int n = std::min(4096, static_cast<int>(len / period) + 1);
  for (int i = 0; i < n; i++) {
    const float t = i * period;
    AppendPrimLine(a + dir * t, a + dir * std::min(t + dash, len), halfPx,
                   color, out);
  }
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

  // The ENTITY itself, not merely something that mentions it: a selected
  // vertex used to light its whole line, because a body pick and a vertex-0
  // pick both stored {id, 0} and this could only compare the entity. Clicking
  // a rectangle corner therefore highlighted the corner AND one of its lines.
  auto selectedEntity = [&](uint32_t id) {
    for (const auto& r : edit.selected)
      if (r.entity == id && r.which == sketch::kWholeEntity)
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
  // constrained ones stay blue (the black-vs-blue signal CAD sketchers use).
  const glm::vec4 base = edit.dof == 0 ? glm::vec4(0.90f, 0.92f, 0.96f, 1.0f)
                                       : glm::vec4(0.55f, 0.85f, 1.00f, 1.0f);
  const glm::vec4 sel(1.00f, 0.62f, 0.20f, 1.0f);
  const glm::vec4 hov(1.00f, 1.00f, 0.65f, 1.0f);
  const glm::vec4 trim(1.00f, 0.35f, 0.28f, 1.0f);       // trim-will-remove
  const glm::vec4 construct(0.55f, 0.60f, 0.70f, 0.85f); // reference geometry
  const glm::vec4 conflict(1.00f, 0.30f, 0.30f, 1.0f);   // over-defined
  const glm::vec4 defined(0.45f, 0.90f, 0.60f, 1.0f);    // fully constrained
  const glm::vec4 linked(0.72f, 0.55f, 0.95f, 1.0f);     // linked-copy output
  const std::set<uint32_t> underDef(edit.underDefined.begin(),
                                    edit.underDefined.end());
  std::set<uint32_t> linkedIds;
  for (const auto& op : sk->derived)
    if (op.kind == sketch::DerivedKind::LinkedCopy)
      for (const auto& inst : op.instances)
        linkedIds.insert(inst.genId);
  // Expression-driven entities (Decision 2A) share the violet solver-fixed
  // convention: their driven coordinates are owned by the parameter table,
  // not the mouse (v1: no in-canvas editing of the expressions).
  for (const auto& [eid, fields] : sk->entityExprs)
    for (const auto& [field, expr] : fields)
      if (!expr.empty()) {
        linkedIds.insert(eid);
        break;
      }

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
    const bool isLinked = linkedIds.count(id) != 0;
    const bool isDefined =
        !constr && (edit.dof == 0 || underDef.count(id) == 0);
    const glm::vec4 col = bad         ? conflict
                          : s         ? sel
                          : h         ? hov
                          : isLinked  ? linked
                          : constr    ? construct
                          : isDefined ? defined
                                      : base;
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
                     edit.hoveredRef.entity == id &&
                     edit.hoveredRef.which == which;
      const glm::vec4 col = s ? sel : (h ? hov : base);
      const float rPx = (s || h) ? 5.8f : 3.8f;
      AppendPrimDot(sk->plane.PlaneToWorld(uv), rPx, col, prims);
    }
  }

  // Trim preview: the sub-segment the cursor would remove, drawn thick and in
  // a "delete" colour on top of the line so it's clear what a click deletes.
  if (edit.hasTrimPreview) {
    AppendPrimLine(sk->plane.PlaneToWorld(edit.trimPreviewA),
                   sk->plane.PlaneToWorld(edit.trimPreviewB), 3.4f, trim,
                   prims);
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

// The viewport picks edges on the DISPLAYED body — which, once the collecting
// blend has an edge, is the already-blended result whose toponames embed the
// blend's own tag (e.g. "2/g1;M<id>;U2"). Those names don't exist on the
// blend's INPUT body, so storing them errors the next regen ("edge no longer
// exists") — the multi-edge-pick bug. Remap the picked ref onto the blend's
// input (the regen cache's snapshot before the blend): by name when it still
// resolves there, else by edge geometry (point-to-polyline match). Returns
// nullopt for an edge the blend itself created — those have no pre-blend
// counterpart and can't be blended by it.
std::optional<ElementRef> RefOnBlendInput(AppState& state,
                                          const FilletFeature& fil,
                                          const ElementRef& picked,
                                          uint32_t meshId, uint32_t edgeIndex) {
  Document& doc = *state.document;
  size_t filletIndex = 0;
  if (doc.FindFeatureById(fil.GetId(), &filletIndex) == nullptr ||
      filletIndex == 0) {
    return picked;
  }
  const std::vector<Body>* before = doc.CachedBodiesAfter(filletIndex - 1);
  if (before == nullptr)
    return picked; // no snapshot (failed regen): keep the old behavior
  const auto input =
      std::find_if(before->begin(), before->end(), [&](const Body& b) {
        return b.createdBy == picked.featureId;
      });
  if (input == before->end())
    return picked;

  // The displayed edge's interior midpoint anchors the geometric fallback — an
  // arc-length midpoint, NOT poly[size/2] (an endpoint for a 2-point straight
  // edge; see PolylineArcMidpoint).
  const auto& meshes = state.documentMeshes;
  const auto mit = std::find_if(
      meshes.begin(), meshes.end(),
      [&](const DocumentRenderMesh& m) { return m.meshId == meshId; });
  if (mit == meshes.end() || edgeIndex >= mit->edgePolylines.size() ||
      mit->edgePolylines[edgeIndex].size() < 2) {
    return std::nullopt;
  }
  const auto& poly = mit->edgePolylines[edgeIndex];
  return topo::ResolveEdgeOnBody(*input, picked.name,
                                 topo::PolylineArcMidpoint(poly));
}

// Adds/removes one edge ref on the blend. A pending (previewing) feature is
// edited directly — it isn't on the undo stack until accepted; an accepted
// one goes through ModifyFeatureCommand so the change is one undo step.
// `regenerate=false` defers the pending-preview regen (seeding batches
// several toggles and regenerates once at the end).
void ToggleBlendEdge(AppState& state, FilletFeature& fil, const ElementRef& ref,
                     bool regenerate = true) {
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
  const auto it = std::find_if(
      fil.edges.begin(), fil.edges.end(), [&](const ElementRef& e) {
        return e.featureId == ref.featureId && e.name == ref.name;
      });
  if (it != fil.edges.end()) {
    fil.edges.erase(it);
  } else {
    fil.edges.push_back(ref);
  }
  if (pending) {
    if (regenerate)
      state.document->Regenerate();
    return;
  }
  state.commands->Execute(std::make_unique<ModifyFeatureCommand>(
                              index, before, DocumentIO::FeatureToJson(fil)),
                          *state.document);
}

} // namespace

void ApplyBlendEdgePick(AppState& state, FilletFeature& fil,
                        const ElementRef& displayed, uint32_t meshId,
                        uint32_t edgeIndex, bool regenerate) {
  if (!state.document)
    return;
  if (auto mapped = RefOnBlendInput(state, fil, displayed, meshId, edgeIndex)) {
    ToggleBlendEdge(state, fil, *mapped, regenerate);
  } else {
    state.sketchEdit.actionMsg =
        "That edge was created by this blend - pick edges of the "
        "original body.";
  }
}

void AppendEdgePickPrimitives(AppState& state, std::vector<PrimVertex>& out) {
  const uint64_t blendId = ArmedBlendFeatureId(state);
  if (blendId == 0 || !state.document) {
    return;
  }
  const auto* fil = dynamic_cast<const FilletFeature*>(
      state.document->FindFeatureById(blendId));
  if (fil == nullptr) {
    return;
  }
  const glm::vec4 kDim{0.55f, 0.62f, 0.72f, 0.85f};
  const glm::vec4 kHover{1.00f, 0.85f, 0.30f, 1.0f};
  const glm::vec4 kPicked{0.35f, 0.80f, 1.00f, 1.0f};

  // The feature stores input-body refs, because its output is regenerated on
  // every added edge. A selected edge can be entirely replaced by the bevel,
  // so there is no output edge to colour. Draw the selected INPUT edge as an
  // overlay; it is precisely the line the user clicked.
  size_t filletIndex = 0;
  const std::vector<Body>* inputBodies =
      state.document->FindFeatureById(fil->GetId(), &filletIndex) != nullptr &&
              filletIndex > 0
          ? state.document->CachedBodiesAfter(filletIndex - 1)
          : nullptr;

  for (const DocumentRenderMesh& m : state.documentMeshes) {
    for (size_t e = 0; e < m.edgePolylines.size(); e++) {
      const auto& poly = m.edgePolylines[e];
      if (poly.size() < 2)
        continue;
      const bool hovered = state.pick.edgeHover &&
                           state.pick.edgeHoverMesh == m.meshId &&
                           state.pick.edgeHoverIndex == e;
      const glm::vec4 color = hovered ? kHover : kDim;
      const float halfPx = hovered ? 2.6f : 1.2f;
      for (size_t i = 0; i + 1 < poly.size(); i++)
        AppendPrimLine(poly[i], poly[i + 1], halfPx, color, out);
    }
  }

  if (inputBodies == nullptr)
    return;
  for (const ElementRef& ref : fil->edges) {
    const auto body = std::find_if(
        inputBodies->begin(), inputBodies->end(),
        [&](const Body& b) { return b.createdBy == ref.featureId; });
    if (body == inputBodies->end())
      continue;
    const auto edgeIndex = body->elementMap.ResolveEdge(ref.name);
    if (!edgeIndex)
      continue;
    const auto polylines = body->shape.GenerateEdgePolylines();
    const size_t e = static_cast<size_t>(*edgeIndex);
    if (e >= polylines.size() || polylines[e].size() < 2)
      continue;
    const auto& poly = polylines[e];
    for (size_t i = 0; i + 1 < poly.size(); i++)
      AppendPrimLine(poly[i], poly[i + 1], 2.6f, kPicked, out);
  }
}

// Reference images (v49): read + decode dims, append a centered ReferenceImage
// to `sk`, and stash the raw bytes in the document asset store. Returns the new
// image id, or 0 on read failure. The pixel bytes live on the Document (not the
// feature) so they survive undo/redo; see
// docs/sketching/reference-image-plan.md.
static uint32_t AppendReferenceImage(AppState& state, SketchFeature* sk,
                                     const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return 0;
  std::string data((std::istreambuf_iterator<char>(in)),
                   std::istreambuf_iterator<char>());
  if (data.empty())
    return 0;
  std::vector<unsigned char> bytes(data.begin(), data.end());

  sketch::ReferenceImage img;
  int w = 0, h = 0, comp = 0;
  if (stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w,
                            &h, &comp)) {
    img.widthPx = w;
    img.heightPx = h;
  }
  img.unitsPerPixel = w > 0 ? 4.0 / w : 0.01; // ~4 units wide until calibrated
  img.opacity = 0.5;
  const uint32_t id = sk->AddImage(img);
  sketch::ReferenceImage& ri = sk->images[sk->IndexOfImage(id)];
  std::string ext = std::filesystem::path(path).extension().string();
  if (ext.empty())
    ext = ".png";
  ri.asset = std::to_string(sk->GetId()) + "-" + std::to_string(id) + ext;
  ri.origin = -0.5f * glm::vec2(float(ri.widthPx), float(ri.heightPx)) *
              float(ri.unitsPerPixel); // centre on the plane origin
  state.document->SetAsset(ri.asset, std::move(bytes));
  return id;
}

uint32_t AddReferenceImageToSketch(AppState& state, const std::string& path) {
  if (!state.document || !state.commands)
    return 0;
  size_t index = 0;
  SketchFeature* sk = ResolveActiveSketch(state, &index);
  if (!sk) {
    state.sketchEdit.actionMsg =
        "Open a sketch for editing before adding a reference image.";
    return 0;
  }
  nlohmann::json before = DocumentIO::FeatureToJson(*sk);
  const uint32_t id = AppendReferenceImage(state, sk, path);
  if (id == 0) {
    state.sketchEdit.actionMsg = "Could not read image: " + path;
    return 0;
  }
  state.document->Regenerate();
  state.commands->Execute(std::make_unique<ModifyFeatureCommand>(
                              index, before, DocumentIO::FeatureToJson(*sk)),
                          *state.document);
  state.sketchEdit.selectedImageId = id;
  return id;
}

void CalibrateReferenceImageAction(AppState& state, uint32_t imageId,
                                   glm::vec2 p1, glm::vec2 p2, double distance,
                                   bool alignAxis) {
  if (!state.document || !state.commands)
    return;
  size_t index = 0;
  SketchFeature* sk = ResolveActiveSketch(state, &index);
  if (!sk)
    return;
  nlohmann::json before = DocumentIO::FeatureToJson(*sk);
  if (!sk->CalibrateImage(imageId, p1, p2, distance, alignAxis ? 0 : -1)) {
    state.sketchEdit.actionMsg =
        "Calibration needs two distinct points and a positive distance.";
    return;
  }
  state.document->Regenerate();
  state.commands->Execute(std::make_unique<ModifyFeatureCommand>(
                              index, before, DocumentIO::FeatureToJson(*sk)),
                          *state.document);
}

void DeleteReferenceImageAction(AppState& state, uint32_t imageId) {
  if (!state.document || !state.commands)
    return;
  size_t index = 0;
  SketchFeature* sk = ResolveActiveSketch(state, &index);
  if (!sk)
    return;
  nlohmann::json before = DocumentIO::FeatureToJson(*sk);
  if (!sk->DeleteImage(imageId))
    return;
  state.document->Regenerate();
  state.commands->Execute(std::make_unique<ModifyFeatureCommand>(
                              index, before, DocumentIO::FeatureToJson(*sk)),
                          *state.document);
  if (state.sketchEdit.selectedImageId == imageId)
    state.sketchEdit.selectedImageId = 0;
}

bool ImportAsFeature(AppState& state, const std::string& filename,
                     std::string* error) {
  return ImportAsFeature(state, filename, nullptr, error);
}

bool ImportAsFeature(AppState& state, const std::string& filename,
                     CADKernel::ImportedModel* parsed, std::string* error) {
  if (!state.document || !state.commands) {
    if (error)
      *error = "no active part document";
    return false;
  }

  const size_t before = state.document->GetBodies().size();
  const std::string name = std::filesystem::path(filename).stem().string();
  auto feature = std::make_unique<ImportFeature>(name, filename);
  // The import worker already read this file to decide it was a single
  // product; hand that over rather than parse it again (ImportFeature::
  // SeedGeometry). Only the first Execute uses it.
  if (parsed != nullptr)
    feature->SeedGeometry(std::move(*parsed));
  state.commands->Execute(
      std::make_unique<AddFeatureCommand>(std::move(feature)), *state.document);

  if (state.document->GetBodies().size() <= before) {
    // The feature carries the specific reason (bad read, empty file, …).
    std::string why = "produced no bodies";
    if (const size_t n = state.document->FeatureCount())
      if (const Feature* f = state.document->GetFeature(n - 1);
          f && !f->GetError().empty())
        why = f->GetError();
    if (error)
      *error = "import of '" + filename + "': " + why;
    LOG_ERROR("Import of '{}': {}", filename, why);
    return false;
  }
  FitCameraToDocument(state);
  return true;
}

} // namespace ui
