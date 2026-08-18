#include "ui/AppActions.h"
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
#include "core/fem/FemTopo.h"
#include "core/io/Autosaver.h"
#include "core/io/GitVault.h"
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
#include "ui/FeatureEditor.h" // PickSlot (thread face slot)
#include "ui/Placement.h"
#include "ui/PreviewFeature.h"
#include "ui/SelectionPipeline.h" // ResolveFaceItem (Hole select-first)
#include "ui/SketchInteraction.h" // SketchUVToScreen
#include <nlohmann/json.hpp>

#include "thirdparty/stb_image.h" // stbi_info_from_memory (impl in IBL.cpp)

#include "imgui.h"

#include <glm/gtc/matrix_transform.hpp> // glm::translate
#include <glm/gtc/quaternion.hpp>       // glm::mat4_cast

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <unordered_map>

namespace ui {

// Defined in AssemblyActions.cpp (assembly domain, extracted from this TU);
// called by SyncDocumentRenderers below.
void SyncAssemblyRenderers(AppState& state);
void SyncInContextGhosts(AppState& state);

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

  // A new workspace starts farther out, so keep the default references large
  // enough to be immediately usable without making the origin glyph dominant.
  const float s = kDefaultPlaneHalfExtent;
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

void AppendSketchGrid(const SketchPlane& plane,
                      std::vector<LineVertex3D>& lines) {
  const int half = static_cast<int>(kSketchBackgroundHalfExtent);
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

namespace {
// Placement offset so successive primitives don't overlap.
// Where a new primitive goes. Fixed-size primitives (every one is 2 units),
// so this ladder is equivalent to the shared clear-spot rule; anything whose
// size varies should use ui/Placement.h's ClearOffset instead of inventing a
// fourth answer (U10).
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
  BeginPreviewFeature(
      state, std::make_unique<BoxFeature>(NextName(state, "Box"),
                                          NextSpot(state), 2.0, 2.0, 2.0));
}

void CreateSphere(AppState& state) {
  BeginPreviewFeature(state,
                      std::make_unique<SphereFeature>(NextName(state, "Sphere"),
                                                      NextSpot(state), 1.0));
}

void CreateCylinder(AppState& state) {
  BeginPreviewFeature(state, std::make_unique<CylinderFeature>(
                                 NextName(state, "Cylinder"),
                                 NextSpot(state) + glm::vec3(0.0f, -1.0f, 0.0f),
                                 glm::vec3(0.0f, 1.0f, 0.0f), 0.8, 2.0));
}

void CreateDatumPlane(AppState& state) {
  auto dp = std::make_unique<DatumPlaneFeature>(NextName(state, "Plane"),
                                                DatumPlaneKind::Offset);
  dp->offset = 2.0; // scene scale (~2-unit boxes, half-extent-3 plane quads)
  // Build on the selected datum if one is selected, else the XY base plane.
  if (auto* parent = dynamic_cast<DatumPlaneFeature*>(SelectedFeature(state)))
    dp->parentId = parent->GetId();
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
// Ids of earlier features usable as a sweep/loft profile or section (closed
// sketches) resp. as a path/guide (a sketch chain or a Curve3D). Used to seed
// Create* defaults; the editors offer the full combo.
// The most recent sketch with a CLOSED loop. "The most recent sketch" (what
// this used to be, despite the comment above) seeded a sweep's profile with
// whatever was drawn last — which is the path the user just drew, so the
// sweep opened reporting "profile has no single closed loop". Found by the
// U10 defaults gate.
uint64_t LastClosedSketchId(const AppState& state) {
  if (!state.document)
    return 0;
  for (size_t i = state.document->FeatureCount(); i-- > 0;) {
    auto* sk = dynamic_cast<SketchFeature*>(state.document->GetFeature(i));
    if (sk != nullptr && !sketch::BuildLoops(sk->entities).empty())
      return sk->GetId();
  }
  return 0;
}
// The most recent thing a sweep/pipe can follow: a 3D curve, or a sketch with
// an open chain. `exclude` keeps a sweep from using its own profile as the
// path when one sketch happens to qualify as both.
uint64_t LastPathId(const AppState& state, uint64_t exclude = 0) {
  if (!state.document)
    return 0;
  for (size_t i = state.document->FeatureCount(); i-- > 0;) {
    Feature* f = state.document->GetFeature(i);
    if (f->GetId() == exclude)
      continue;
    if (dynamic_cast<CurveFeature*>(f))
      return f->GetId();
    if (auto* sk = dynamic_cast<SketchFeature*>(f)) {
      if (!sketch::BuildOpenPathWire(sk->plane, sk->entities).IsNull())
        return sk->GetId();
    }
  }
  return 0;
}
} // namespace

void CreateCurve3D(AppState& state) {
  // Default to a helix: it needs no references and is valid immediately, so the
  // preview shows a real curve. The editor switches kind / edits points.
  auto cv = std::make_unique<CurveFeature>(NextName(state, "Curve"),
                                           Curve3DKind::Helix);
  cv->radius = 3.0;
  cv->pitch = 2.0;
  cv->turns = 4.0;
  BeginPreviewFeature(state, std::move(cv));
}

void CreatePipe(AppState& state) {
  // Seed the path with the most recent curve/sketch (if any); the editor's
  // path combo lets the user change it.
  auto pi = std::make_unique<PipeFeature>(NextName(state, "Pipe"),
                                          LastPathId(state), 2.0);
  BeginPreviewFeature(state, std::move(pi));
}

void CreateSweep(AppState& state) {
  // Profile = the latest CLOSED sketch, path = the latest open chain or curve
  // that is not that profile; both editable in the dialog's combos.
  const uint64_t profile = LastClosedSketchId(state);
  BeginPreviewFeature(
      state, std::make_unique<SweepFeature>(NextName(state, "Sweep"), profile,
                                            LastPathId(state, profile)));
}

void CreateLoft(AppState& state) {
  // Start empty; the editor's "Add section" combo collects 2+ profiles (and an
  // optional path + guide for a guided loft).
  BeginPreviewFeature(state,
                      std::make_unique<LoftFeature>(NextName(state, "Loft"),
                                                    std::vector<uint64_t>{}));
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

size_t SketchFeatureCount(const Document& doc) {
  size_t n = 0;
  for (const auto& f : doc.GetFeatures())
    if (dynamic_cast<const SketchFeature*>(f.get()))
      n++;
  return n;
}

bool HasCurveOrSketch(const Document& doc) {
  for (const auto& f : doc.GetFeatures())
    if (dynamic_cast<const SketchFeature*>(f.get()) ||
        dynamic_cast<const CurveFeature*>(f.get()))
      return true;
  return false;
}

std::vector<std::pair<uint64_t, std::string>>
BodyProducers(const Document& doc, size_t beforeIndex) {
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
  auto producers =
      BodyProducers(*state.document, state.document->FeatureCount());
  if (producers.size() < 2) {
    return; // the menu gates this, but guard anyway
  }
  // Default: the two most-recent producers, older as Target so subtract reads
  // as "the earlier body minus the newer one".
  const uint64_t tool = producers.back().first;
  const uint64_t target = producers[producers.size() - 2].first;
  BeginPreviewFeature(
      state, std::make_unique<BooleanFeature>(NextName(state, "Boolean"),
                                              target, tool, BooleanOp::Union));
}

void CreateMirror(AppState& state) {
  if (!state.document) {
    return;
  }
  auto producers =
      BodyProducers(*state.document, state.document->FeatureCount());
  if (producers.empty()) {
    return;
  }
  auto mir = std::make_unique<PatternFeature>(
      NextName(state, "Mirror"), PatternKind::Mirror, producers.back().first);
  mir->basePlane = PlaneType::YZ; // reflect across x by default
  BeginPreviewFeature(state, std::move(mir));
}

void CreatePattern(AppState& state) {
  if (!state.document) {
    return;
  }
  auto producers =
      BodyProducers(*state.document, state.document->FeatureCount());
  if (producers.empty()) {
    return;
  }
  auto pat = std::make_unique<PatternFeature>(
      NextName(state, "Pattern"), PatternKind::Linear, producers.back().first);
  pat->axis.kind = AxisRef::Kind::X;
  pat->count = 3;
  // Default spacing clears the source body along the axis — a fixed constant
  // made copies of anything wider than it overlap and fuse into one blob,
  // which read as "the pattern did nothing".
  pat->spacing = 3.0;
  for (const Body& b : state.document->GetBodies()) {
    if (b.createdBy != pat->sourceId)
      continue;
    glm::vec3 bmin, bmax;
    b.shape.GetBoundingBox(bmin, bmax);
    pat->spacing = std::max(3.0, (bmax.x - bmin.x) * 1.5);
    break;
  }
  BeginPreviewFeature(state, std::move(pat));
}

namespace {
// Shared by CreateFillet/CreateChamfer: start the preview and arm the edge
// slot so viewport clicks collect edges on the pending feature.
void CreateEdgeBlend(AppState& state, EdgeBlendOp op) {
  if (!state.document || state.document->GetBodies().empty()) {
    return; // the menu gates this, but guard anyway
  }
  const char* base = op == EdgeBlendOp::Chamfer ? "Chamfer" : "Fillet";
  // BeginPreviewFeature seeds pre-selected edges and arms the edge slot.
  BeginPreviewFeature(
      state, std::make_unique<FilletFeature>(
                 NextName(state, base), std::vector<ElementRef>{}, 0.5, op));
}
} // namespace

void CreateFillet(AppState& state) {
  CreateEdgeBlend(state, EdgeBlendOp::Fillet);
}

void CreateChamfer(AppState& state) {
  CreateEdgeBlend(state, EdgeBlendOp::Chamfer);
}

void CreateThread(AppState& state) {
  if (!state.document || state.document->GetBodies().empty())
    return;
  // BeginPreviewFeature seeds a pre-selected face and arms the face slot.
  BeginPreviewFeature(
      state, std::make_unique<ThreadFeature>(NextName(state, "Thread"),
                                             ElementRef{}, "M8", 1.25));
}

namespace {
// The body a body-op tool should default to: the most recent producer, as
// Pattern/Boolean do. 0 when the document has none.
uint64_t NewestBody(const AppState& state) {
  auto producers =
      BodyProducers(*state.document, state.document->FeatureCount());
  return producers.empty() ? 0 : producers.back().first;
}
} // namespace

void CreateShell(AppState& state) {
  if (!state.document || state.document->GetBodies().empty())
    return;
  // 0.25 on the ~2-unit default scene scale: thin enough that OCC accepts the
  // offset on a small body, which "1.0" (the field default) does not.
  BeginPreviewFeature(state, std::make_unique<ShellFeature>(
                                 NextName(state, "Shell"), NewestBody(state),
                                 std::vector<ElementRef>{}, 0.25));
}

void CreateDraft(AppState& state) {
  if (!state.document || state.document->GetBodies().empty())
    return;
  // bodyId stays 0: Draft has no body slot, so the first face pick names it.
  BeginPreviewFeature(
      state, std::make_unique<DraftFeature>(NextName(state, "Draft"), 0,
                                            std::vector<ElementRef>{}, 5.0));
}

void CreateSplit(AppState& state) {
  if (!state.document || state.document->GetBodies().empty())
    return;
  auto split = std::make_unique<SplitFeature>(NextName(state, "Split"),
                                              NewestBody(state));
  // A base plane through the origin misses any body that does not sit there,
  // and the feature fails with "the plane missed the body" before the user has
  // touched anything. Offset the default XY knife to the body's mid-height so
  // the preview shows an actual split.
  for (const Body& b : state.document->GetBodies()) {
    if (b.createdBy != split->bodyId)
      continue;
    glm::vec3 bmin, bmax;
    b.shape.GetBoundingBox(bmin, bmax);
    split->offset = (bmin.z + bmax.z) * 0.5;
    break;
  }
  BeginPreviewFeature(state, std::move(split));
}

void CreateRefine(AppState& state) {
  if (!state.document || state.document->GetBodies().empty())
    return;
  BeginPreviewFeature(state, std::make_unique<RefineFeature>(
                                 NextName(state, "Refine"), NewestBody(state)));
}

void AppendThreadOverlays(AppState& state, std::vector<LineVertex3D>& lines) {
  if (!state.document)
    return;
  const glm::vec3 col(0.82f, 0.74f, 0.45f); // muted brass, drafting-thin
  for (size_t i = 0; i < state.document->FeatureCount(); i++) {
    auto* th = dynamic_cast<ThreadFeature*>(state.document->GetFeature(i));
    if (th == nullptr || !th->overlay.valid || th->IsSuppressed())
      continue;
    const auto& ov = th->overlay;
    const glm::vec3 ref =
        std::abs(ov.dir.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
    const glm::vec3 b1 = glm::normalize(glm::cross(ov.dir, ref));
    const glm::vec3 b2 = glm::cross(ov.dir, b1);
    // Dashed thread-depth circle at each end of the span: 36 segments,
    // two drawn + one skipped.
    for (const float v : {ov.v0, ov.v1}) {
      const glm::vec3 c = ov.origin + ov.dir * v;
      for (int s = 0; s < 36; s++) {
        if (s % 3 == 2)
          continue;
        const float a0 = (float)s / 36.0f * glm::two_pi<float>();
        const float a1 = (float)(s + 1) / 36.0f * glm::two_pi<float>();
        lines.push_back(LineVertex(
            c + (std::cos(a0) * b1 + std::sin(a0) * b2) * ov.radius, col));
        lines.push_back(LineVertex(
            c + (std::cos(a1) * b1 + std::sin(a1) * b2) * ov.radius, col));
      }
    }
    // Two dashed crest lines along the span (±b1), thread-depth radius.
    const float span = ov.v1 - ov.v0;
    const int dashes = std::max(2, (int)(span / (1.5f * 1.0f)));
    for (const float side : {1.0f, -1.0f}) {
      const glm::vec3 off = b1 * (side * ov.radius);
      for (int dsh = 0; dsh < dashes; dsh++) {
        const float t0 = ov.v0 + span * (dsh + 0.15f) / dashes;
        const float t1 = ov.v0 + span * (dsh + 0.85f) / dashes;
        lines.push_back(LineVertex(ov.origin + ov.dir * t0 + off, col));
        lines.push_back(LineVertex(ov.origin + ov.dir * t1 + off, col));
      }
    }
  }
}

void CreateRib(AppState& state) {
  if (!state.document || state.document->GetBodies().empty())
    return;
  // The profile: the selected sketch if it has an open chain, else the most
  // recent sketch that does.
  const auto openSketch = [&](Feature* f) -> SketchFeature* {
    auto* sk = dynamic_cast<SketchFeature*>(f);
    if (sk == nullptr)
      return nullptr;
    return sketch::BuildOpenPathWire(sk->plane, sk->entities).IsNull() ? nullptr
                                                                       : sk;
  };
  SketchFeature* profile = openSketch(SelectedFeature(state));
  for (size_t i = state.document->FeatureCount(); profile == nullptr && i > 0;
       i--)
    profile = openSketch(state.document->GetFeature(i - 1));
  if (profile == nullptr)
    return; // no open profile anywhere: nothing to rib (menu gates this)
  BeginPreviewFeature(state,
                      std::make_unique<RibFeature>(NextName(state, "Rib"),
                                                   profile->GetId(), 1.0));
}

void StartNewSketch(AppState& state) {
  state.sketchEdit = SketchEditState{};
  state.sketchEdit.pickingPlane = true;
}

void StartHoleTool(AppState& state) {
  if (!state.document || state.document->GetBodies().empty())
    return; // a hole is a cut: nothing to cut into yet
  // Select-first (U7b): one planar face already picked IS the face to drill.
  std::string refusal;
  if (const SelectionItem* item =
          state.selection.First(SelectionItem::Kind::Face)) {
    uint32_t meshId = 0, faceIndex = 0;
    if (ResolveFaceItem(state, *item, meshId, faceIndex)) {
      if (BeginHoleSketchOnFace(state, meshId, faceIndex))
        return; // straight to placing centres
      refusal = "Hole: that face is curved - pick a flat one.";
    }
  }
  state.sketchEdit = SketchEditState{}; // reset FIRST, then arm
  state.sketchEdit.pickingPlane = true;
  state.sketchEdit.purpose = SketchPurpose::Hole;
  state.sketchEdit.actionMsg = refusal;
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
    const bool emphasized = dp->GetId() == state.sketchEdit.hoveredDatumId ||
                            dp->GetId() == state.pendingFeatureId ||
                            dp->GetId() == SelectedFeatureId(state);
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
                            da->GetId() == SelectedFeatureId(state);
    const glm::vec3 c = kAxisTint * (emphasized ? 1.4f : 1.0f);
    const glm::vec3 o = da->Origin();
    const glm::vec3 d = da->Direction();
    lines.push_back(LineVertex(o - d * half, c));
    lines.push_back(LineVertex(o + d * half, c));
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

void DrawDatumLabels(AppState& state) {
  if (!state.document || !DatumPlanesVisible(state))
    return;
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const ImU32 col = IM_COL32(230, 180, 90, 230);
  for (size_t i = 0; i < state.document->FeatureCount(); i++) {
    Feature* f = state.document->GetFeature(i);
    if (auto* dp = dynamic_cast<DatumPlaneFeature*>(f)) {
      // Label near a corner of the 3-unit quad.
      glm::vec2 s =
          SketchUVToScreen(state, dp->OutputPlane(), glm::vec2(2.6f, 2.6f));
      dl->AddText(ImVec2(s.x, s.y), col, dp->GetName().c_str());
    } else if (auto* da = dynamic_cast<DatumAxisFeature*>(f)) {
      const glm::vec3 end = da->Origin() + da->Direction() * 3.0f;
      glm::vec2 s = WorldToScreen(state, end);
      dl->AddText(ImVec2(s.x, s.y), col, da->GetName().c_str());
    }
  }
}

void DrawPromptPill(AppState& state, float& y, const std::string& text,
                    PromptPill style) {
  if (text.empty())
    return;
  ImU32 bg = 0, fg = 0;
  switch (style) {
  case PromptPill::StatusGood:
    bg = IM_COL32(24, 46, 32, 230);
    fg = IM_COL32(140, 235, 165, 255);
    break;
  case PromptPill::Status:
    bg = IM_COL32(26, 36, 52, 230);
    fg = IM_COL32(150, 200, 255, 255);
    break;
  case PromptPill::Prompt:
    bg = IM_COL32(28, 32, 40, 225);
    fg = IM_COL32(226, 232, 240, 255);
    break;
  case PromptPill::Warning:
    bg = IM_COL32(60, 44, 24, 235);
    fg = IM_COL32(255, 205, 120, 255);
    break;
  }
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const float cx = state.viewportPos.x + state.viewportSize.x * 0.5f;
  const float padX = 10.0f, padY = 5.0f;
  const ImVec2 sz = ImGui::CalcTextSize(text.c_str());
  const ImVec2 a(cx - sz.x * 0.5f - padX, y);
  const ImVec2 b(cx + sz.x * 0.5f + padX, y + sz.y + 2.0f * padY);
  dl->AddRectFilled(a, b, bg, 5.0f);
  dl->AddText(ImVec2(cx - sz.x * 0.5f, y + padY), fg, text.c_str());
  y = b.y + 4.0f;
}

void DrawElementNameOverlay(AppState& state) {
  if (!state.showElementNames || !state.document || !state.faceSelector)
    return;
  // While an edge is hovered, label it instead of a face.
  if (state.pick.edgeHover) {
    const auto ref = EdgeElementRef(state, state.pick.edgeHoverMesh,
                                    state.pick.edgeHoverIndex);
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
  const auto it = std::find_if(
      meshes.begin(), meshes.end(),
      [&](const DocumentRenderMesh& m) { return m.meshId == face->meshId; });
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
  const bool hasBodies = state.document && !state.document->GetBodies().empty();
  return state.showPlanes && !hasBodies; // auto-hide once the part has geometry
}

void ReplaceDocument(AppState& state, std::unique_ptr<Document> doc) {
  state.document = std::move(doc);
  state.commands = std::make_unique<CommandStack>();
  // The active tab may have been an assembly or drawing: this replacement
  // makes the live tab a part, so drop the other-kind state — leaving
  // state.assembly set alongside state.document half-switches the UI (the
  // assembly toolbar keeps drawing and the part never syncs; File > New on
  // an assembly tab hit exactly that).
  state.assembly = nullptr;
  state.drawing = nullptr;
  state.assemblyHistory = {};
  state.drawingHistory = {};
  state.drawingRenders.clear();
  state.drawingRendersVersion = 0;
  state.solvedPartVersions.clear();
  // Phase V: a wholesale replacement (File > New, Open flat, checkpoint/history
  // restore) collapses the project to this single part, so ITS parameters
  // become the project scope (one part = the whole project). The project-var
  // undo history belonged to the old model — discard it, then regenerate the
  // document against the (now project) scope.
  state.projectParameters = state.document->TakeParameters();
  state.projectVarHistory = {};
  state.document->SetProjectSymbols(&state.projectSymbols);
  state.document->SetProjectParameters(&state.projectParameters);
  EvaluateParameterTable(state.projectParameters, state.projectSymbols, nullptr,
                         {});
  state.document->Regenerate();
  state.selection.Clear();
  state.pendingFeatureId = 0;
  state.sketchEdit = SketchEditState{};
  // Simulation is a MODE, and the studies it edits belonged to the document
  // that just went away. File > New in simulation mode used to leave the
  // simulation toolbar up over a brand-new part, editing the new document's
  // (empty) study list — the same half-switched UI the assembly/drawing
  // clearing above exists to prevent.
  if (state.simulation.active)
    ExitSimulation(state);
  state.measure = MeasureState{};
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
  slot.drawingRendersVersion = state.drawingRendersVersion;
  state.drawingRendersVersion = 0;
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
  state.drawingRendersVersion = slot.drawingRendersVersion;
  state.lastSavedVersion = slot.lastSavedVersion;
  state.solvedPartVersions = std::move(slot.solvedPartVersions);
  slot.solvedPartVersions.clear();
  state.activePart = static_cast<int>(index);
}
// Shared reset after a tab switch/create.
void ResetActiveTabUiState(AppState& state) {
  state.selection.Clear(); // features, occurrences and mates alike
  state.slotPick = {};
  state.hiddenBodies.clear();
  state.pendingFeatureId = 0;
  state.editFeatureId = 0;
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
  state.dimTool = AppState::DimTool{}; // transient dimension-pick state
  state.dimDrag = 0;
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
    state.parts.push_back(
        std::move(slot)); // active: doc/commands stay in AppState
    state.activePart = 0;
  }
}

size_t AddPart(AppState& state, const std::string& name) {
  EnsureParts(state);
  CancelPendingFeature(state); // before StashActive — see SetActivePart
  StashActive(state);
  // Create and activate the new part.
  state.document = std::make_unique<Document>();
  // Phase V: the new part shares the project scope (its features can reference
  // project variables immediately).
  state.document->SetProjectSymbols(&state.projectSymbols);
  state.document->SetProjectParameters(&state.projectParameters);
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
  CancelPendingFeature(state); // before StashActive — see SetActivePart
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
  CancelPendingFeature(state); // before StashActive — see SetActivePart
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
  if (!state.drawing)
    return;
  state.drawingHistory.undo.push_back(*state.drawing); // full-state copy
  state.drawingHistory.undoStamps.push_back(NextActionStamp());
  state.drawingHistory.redo.clear();
  state.drawingHistory.redoStamps.clear();
}

bool UndoDrawing(AppState& state) {
  if (!state.drawing || state.drawingHistory.undo.empty())
    return false;
  state.drawingHistory.redo.push_back(*state.drawing);
  state.drawingHistory.redoStamps.push_back(
      state.drawingHistory.undoStamps.back());
  state.drawingHistory.undoStamps.pop_back();
  *state.drawing = std::move(state.drawingHistory.undo.back());
  state.drawingHistory.undo.pop_back();
  state.syncedDocumentVersion = UINT64_MAX;
  return true;
}

bool RedoDrawing(AppState& state) {
  if (!state.drawing || state.drawingHistory.redo.empty())
    return false;
  state.drawingHistory.undo.push_back(*state.drawing);
  state.drawingHistory.undoStamps.push_back(
      state.drawingHistory.redoStamps.back());
  state.drawingHistory.redoStamps.pop_back();
  *state.drawing = std::move(state.drawingHistory.redo.back());
  state.drawingHistory.redo.pop_back();
  state.syncedDocumentVersion = UINT64_MAX;
  return true;
}

uint64_t PartIdByName(const AppState& state, const std::string& name) {
  for (const PartSlot& slot : state.parts)
    if (slot.kind == DocKind::Part && slot.name == name)
      return slot.id;
  return 0;
}

void SetActivePart(AppState& state, size_t index) {
  EnsureParts(state);
  if (index >= state.parts.size() ||
      index == static_cast<size_t>(state.activePart)) {
    return;
  }
  // A pending preview feature was added straight to the Document (not through
  // a command), so stashing the tab would strand it in that document forever,
  // off the undo stack. Discard it while `state.document` still owns it —
  // ResetActiveTabUiState only zeroes the id, which is too late.
  CancelPendingFeature(state);
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
    if (state.parts[i].id == slotId)
      return i;
  return std::nullopt;
}

// ---- Simulation mode (FEM) ------------------------------------------------

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

void CreateFolder(AppState& state, const std::string& folder) {
  if (folder.empty() ||
      std::find(state.tabFolders.begin(), state.tabFolders.end(), folder) !=
          state.tabFolders.end())
    return;
  state.tabFolders.push_back(folder);
  state.projectMetaDirty = true;
}

void MoveToFolder(AppState& state, size_t index, const std::string& folder) {
  if (index >= state.parts.size() || state.parts[index].folder == folder)
    return;
  CreateFolder(state, folder); // no-op for "" / already-registered
  state.parts[index].folder = folder;
  state.projectMetaDirty = true;
}

void RenameFolder(AppState& state, const std::string& from,
                  const std::string& to) {
  if (from.empty() || to.empty() || from == to)
    return;
  for (PartSlot& slot : state.parts)
    if (slot.folder == from)
      slot.folder = to;
  // Registry: rename in place; renaming onto an existing folder merges (drop
  // the old entry instead of duplicating the surviving one).
  auto it = std::find(state.tabFolders.begin(), state.tabFolders.end(), from);
  if (it != state.tabFolders.end()) {
    if (std::find(state.tabFolders.begin(), state.tabFolders.end(), to) !=
        state.tabFolders.end())
      state.tabFolders.erase(it);
    else
      *it = to;
  }
  state.projectMetaDirty = true;
}

void DissolveFolder(AppState& state, const std::string& folder) {
  if (folder.empty())
    return;
  for (PartSlot& slot : state.parts)
    if (slot.folder == folder)
      slot.folder.clear();
  std::erase(state.tabFolders, folder);
  state.projectMetaDirty = true;
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
  return static_cast<int>(i) == state.activePart
             ? state.assembly.get()
             : state.parts[i].assembly.get();
}
// A drawing slot's document, active or stashed. Null for a part/assembly slot.
DrawingDocument* SlotDrawing(AppState& state, size_t i) {
  return static_cast<int>(i) == state.activePart ? state.drawing.get()
                                                 : state.parts[i].drawing.get();
}
} // namespace

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
  return state.commands ? state.commands->TopRedoStamp() : 0;
}
void ActiveTabUndo(AppState& state) {
  if (state.assembly)
    UndoAssembly(state);
  else if (state.drawing)
    UndoDrawing(state);
  else if (state.commands && state.commands->CanUndo())
    state.commands->Undo(*state.document);
}
void ActiveTabRedo(AppState& state) {
  if (state.assembly)
    RedoAssembly(state);
  else if (state.drawing)
    RedoDrawing(state);
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

namespace {
// Imports made before the project had a home stage their .brep sidecars
// under the app state dir (ImportStepAsAssembly). A save gives them a home:
// copy each staged file into <project>/imported/ (keeping the per-import
// subfolder) and repoint the features, so the saved project is
// self-contained and the staging area gets reclaimed.
void AdoptStagedImports(AppState& state, const std::filesystem::path& dir) {
  namespace fs = std::filesystem;
  const fs::path staging = fs::path(Autosaver::StateDir()) / "import-staging";
  std::error_code ec;
  for (size_t i = 0; i < state.parts.size(); i++) {
    Document* doc = SlotDoc(state, i);
    if (!doc)
      continue;
    for (size_t f = 0; f < doc->FeatureCount(); f++) {
      auto* imp = dynamic_cast<ImportFeature*>(doc->GetFeature(f));
      if (!imp)
        continue;
      const fs::path src(imp->path);
      const fs::path rel = src.lexically_relative(staging);
      if (rel.empty() || rel.native().starts_with(".."))
        continue; // not staged: an original STEP, or already project-owned
      const fs::path dest = dir / "imported" / rel;
      fs::create_directories(dest.parent_path(), ec);
      ec.clear();
      if (!fs::copy_file(src, dest, fs::copy_options::overwrite_existing, ec) ||
          ec) {
        LOG_WARN("Could not adopt staged import '{}': {}", imp->path,
                 ec.message());
        ec.clear();
        continue;
      }
      imp->path = dest.string();
      fs::remove(src, ec);
      fs::remove(src.parent_path(), ec); // rmdir-if-empty; failure is fine
      ec.clear();
    }
  }
}
} // namespace

bool SaveProject(AppState& state, const std::string& path, std::string* error) {
  namespace fs = std::filesystem;
  EnsureParts(state);
  // Directory-form targets can own import sidecars; adopt any still staged
  // before the write so the saved features point inside the project.
  if (path.ends_with(".dsgn") || fs::is_directory(path))
    AdoptStagedImports(state, fs::path(path));
  // A single part keeps the flat .dsgn form (backward compatible). A lone
  // assembly, or any multi-document project, uses the project container.
  // Phase V: a single part IS the whole project, so its file carries the
  // PROJECT-scope variables. We fall back to the container form only when the
  // part also holds part-local variables (both scopes then need distinct homes
  // — flat has room for one `parameters` list).
  if (state.parts.size() == 1 && state.parts[0].kind == DocKind::Part &&
      state.document->GetParameters().empty() && state.tabFolders.empty() &&
      state.parts[0].folder.empty()) {
    // Fold the project scope into the doc for the write (they reload as project
    // scope), then restore the (empty) local table.
    std::vector<Parameter> localBackup = state.document->TakeParameters();
    state.document->SetParametersRaw(state.projectParameters);
    auto r = DocumentIO::Save(*state.document, path);
    state.document->SetParametersRaw(std::move(localBackup));
    if (!r && error)
      *error = r.error();
    return r.has_value();
  }
  // Multi-document: project.json (v2) + parts/<name>/ and assemblies/<name>/.
  try {
    const fs::path dir(path);
    nlohmann::json index;
    index["format"] = "designer-project";
    index["version"] = 2;
    index["active"] = state.activePart;
    // Phase V: project-scope variables live in the project envelope, beside
    // active/documents. Per-part index.json `parameters` are that part's LOCAL
    // scope (usually empty now that set_variable defaults to project).
    if (!state.projectParameters.empty()) {
      index["parameters"] = nlohmann::json::array();
      for (const Parameter& pv : state.projectParameters) {
        nlohmann::json pj = {{"name", pv.name}, {"expression", pv.expression}};
        if (!pv.comment.empty())
          pj["comment"] = pv.comment;
        index["parameters"].push_back(std::move(pj));
      }
    }
    // Folder registry (display order; may hold empty folders). Written only
    // when non-empty so pre-folder projects keep byte-identical indexes.
    if (!state.tabFolders.empty())
      index["folders"] = state.tabFolders;
    index["documents"] = nlohmann::json::array();
    for (size_t i = 0; i < state.parts.size(); i++) {
      const PartSlot& slot = state.parts[i];
      const char* sub = slot.kind == DocKind::Assembly  ? "assemblies/"
                        : slot.kind == DocKind::Drawing ? "drawings/"
                                                        : "parts/";
      const char* type = slot.kind == DocKind::Assembly  ? "assembly"
                         : slot.kind == DocKind::Drawing ? "drawing"
                                                         : "part";
      const fs::path docDir = dir / sub / slot.name;
      fs::create_directories(docDir);
      std::expected<void, std::string> r;
      switch (slot.kind) {
      case DocKind::Assembly:
        r = DocumentIO::SaveAssembly(*SlotAssembly(state, i), docDir.string());
        break;
      case DocKind::Drawing:
        r = DocumentIO::SaveDrawing(*SlotDrawing(state, i), docDir.string());
        break;
      case DocKind::Part:
        r = DocumentIO::Save(*SlotDoc(state, i), docDir.string());
        break;
      }
      if (!r) {
        if (error)
          *error = r.error();
        return false;
      }
      nlohmann::json entry = {{"id", slot.id},
                              {"name", slot.name},
                              {"dir", std::string(sub) + slot.name},
                              {"type", type}};
      if (!slot.folder.empty()) // display grouping; omit at top level
        entry["folder"] = slot.folder;
      index["documents"].push_back(std::move(entry));
    }
    std::ofstream out(dir / "project.json");
    if (!out) {
      if (error)
        *error = "cannot write project.json";
      return false;
    }
    out << index.dump(2) << "\n";
    return true;
  } catch (const std::exception& e) {
    if (error)
      *error = e.what();
    return false;
  }
}

bool OpenProject(AppState& state, const std::string& path, std::string* error) {
  namespace fs = std::filesystem;
  const fs::path projFile = fs::path(path) / "project.json";
  if (!fs::exists(projFile)) {
    // Not a multi-document project: load as a single part.
    auto loaded = DocumentIO::Load(path);
    if (!loaded) {
      if (error)
        *error = loaded.error();
      return false;
    }
    ReplaceDocument(state, std::move(*loaded));
    state.parts.clear();
    state.tabFolders.clear();
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
        docs.push_back(
            {{"name", p.at("name")}, {"dir", p.at("dir")}, {"type", "part"}});
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
      slot.folder = docs[i].value("folder", std::string{});
      if (type == "assembly") {
        slot.kind = DocKind::Assembly;
        auto r = DocumentIO::LoadAssembly(dir);
        if (!r) {
          if (error)
            *error = r.error();
          return false;
        }
        slot.assembly = std::move(*r);
      } else if (type == "drawing") {
        slot.kind = DocKind::Drawing;
        auto r = DocumentIO::LoadDrawing(dir);
        if (!r) {
          if (error)
            *error = r.error();
          return false;
        }
        slot.drawing = std::move(*r);
      } else {
        slot.kind = DocKind::Part;
        auto r = DocumentIO::Load(dir);
        if (!r) {
          if (error)
            *error = r.error();
          return false;
        }
        slot.doc = std::move(*r);
        slot.commands = std::make_unique<CommandStack>();
      }
      maxId = std::max(maxId, slot.id);
      state.parts.push_back(std::move(slot));
    }
    state.nextPartId = maxId + 1;
    // Folder registry: the saved list (empty folders and their order), plus
    // any slot folder it doesn't know (pre-registry saves, hand-edits).
    state.tabFolders.clear();
    for (const auto& f : index.value("folders", nlohmann::json::array()))
      if (f.is_string() && !f.get<std::string>().empty())
        state.tabFolders.push_back(f.get<std::string>());
    for (const PartSlot& slot : state.parts)
      if (!slot.folder.empty() &&
          std::find(state.tabFolders.begin(), state.tabFolders.end(),
                    slot.folder) == state.tabFolders.end())
        state.tabFolders.push_back(slot.folder);

    size_t wantActive = static_cast<size_t>(index.value("active", 0));
    if (wantActive >= state.parts.size())
      wantActive = 0;
    ActivateSlot(state, wantActive);
    ResetActiveTabUiState(state);

    // Phase V: load the project-scope variables from the envelope, wire every
    // part's Document to them, and regenerate all (Load regenerated each part
    // WITHOUT the project scope, so a feature referencing a project var errored
    // — SyncProjectScope re-evaluates it correctly).
    state.projectParameters.clear();
    state.projectVarHistory = {};
    for (const auto& pj : index.value("parameters", nlohmann::json::array()))
      state.projectParameters.push_back({pj.at("name").get<std::string>(),
                                         pj.at("expression").get<std::string>(),
                                         pj.value("comment", std::string{}),
                                         0.0,
                                         {}});
    SyncProjectScope(state);
    // A4: an assembly active on open solves immediately (no stamps yet), so
    // mates re-solve on load — the format's stated contract.
    if (state.assembly && AssemblyPartsStale(state))
      SolveActiveAssembly(state);
    if (state.drawing && state.drawing->ViewCount() > 0)
      UpdateDrawingRenders(
          state); // re-project on first view of an opened drawing
    if (state.document)
      FitCameraToDocument(state);
    return true;
  } catch (const std::exception& e) {
    if (error)
      *error = e.what();
    return false;
  }
}

namespace {
// The version a slot's dirty check compares against: whichever document kind
// the slot holds. 0 for an empty slot.
uint64_t SlotVersion(AppState& state, size_t i) {
  if (Document* d = SlotDoc(state, i))
    return d->Version();
  if (AssemblyDocument* a = SlotAssembly(state, i))
    return a->Version();
  if (DrawingDocument* dr = SlotDrawing(state, i))
    return dr->Version();
  return 0;
}
// SaveProject writes every slot, so a clean save stamps every slot — the
// active one through state.lastSavedVersion (its slot copy is stale while
// active; StashActive writes it back on tab switch).
void StampAllSaved(AppState& state) {
  for (size_t i = 0; i < state.parts.size(); i++) {
    const uint64_t v = SlotVersion(state, i);
    if (static_cast<int>(i) == state.activePart)
      state.lastSavedVersion = v;
    else
      state.parts[i].lastSavedVersion = v;
  }
  if (state.parts.empty())
    state.lastSavedVersion = state.document ? state.document->Version() : 0;
  state.projectMetaDirty = false;
}
} // namespace

bool ProjectDirty(AppState& state) {
  if (state.projectMetaDirty)
    return true;
  if (state.parts.empty())
    return state.document &&
           state.document->Version() != state.lastSavedVersion;
  for (size_t i = 0; i < state.parts.size(); i++) {
    const uint64_t saved = static_cast<int>(i) == state.activePart
                               ? state.lastSavedVersion
                               : state.parts[i].lastSavedVersion;
    if (SlotVersion(state, i) != saved)
      return true;
  }
  return false;
}

std::string WindowTitle(AppState& state) {
  std::string name = "Untitled";
  if (!state.projectPath.empty())
    name = std::filesystem::path(state.projectPath).stem().string();
  return name + (ProjectDirty(state) ? "*" : "") + " - Designer";
}

void MarkProjectSaved(AppState& state, const std::string& path,
                      const std::string& message) {
  state.projectPath = path;
  RecentProjects::Remember(path);
  StampAllSaved(state);
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
  RecentProjects::Remember(path); // ignores "" (the unsaved slot)
  StampAllSaved(state);
}

void MarkProjectRecovered(AppState& state, const std::string& path) {
  state.projectPath = path;
  // Never equal to the live version: the recovered changes are unsaved.
  state.lastSavedVersion =
      state.document ? state.document->Version() - 1 : UINT64_MAX;
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

// The part document an occurrence references, by stable id (active or stashed).
Document* PartDocById(AppState& state, uint64_t partId) {
  for (size_t i = 0; i < state.parts.size(); i++)
    if (state.parts[i].kind == DocKind::Part && state.parts[i].id == partId)
      return SlotDoc(state, i);
  return nullptr;
}

namespace {
// Aim the camera at a world-space box: the shared tail of the document and
// selection fits.
void FitCameraToBox(AppState& state, const glm::vec3& min,
                    const glm::vec3& max) {
  const glm::vec3 center = (min + max) * 0.5f;
  const float radius = glm::length(max - min) * 0.5f;

  // Distance that fits the bounding sphere in the LIMITING screen axis. The
  // vertical half-FOV is fixed; the horizontal one is tan(fovY/2)*aspect, so
  // in a portrait-ish viewport (aspect < 1 — e.g. the central region of a
  // tall tiled window) the width is what gates the fit. The old radius*2.5
  // constant was exactly the vertical fit for the 45-degree FOV and silently
  // assumed a landscape viewport.
  Camera& cam = *state.camera;
  const float tanHalfV = std::tan(0.5f * glm::radians(cam.GetFOV()));
  const float aspect =
      cam.GetAspectRatio() > 0.0f ? cam.GetAspectRatio() : 1.0f;

  // An open tool window covers the right of the viewport, and fitting used to
  // park the model underneath the very window that asked you to click it
  // (U10). Treat the covered strip as unusable: the model fits the band that
  // is left, and the camera pans left by half the strip so it is centred in
  // THAT band rather than in the whole viewport.
  // Ask the session state whether a window is open, not the rect: the remote
  // `fit_view` runs before the UI draws, so the rect is always a frame behind
  // and a just-closed window kept shoving the model aside.
  const bool toolWindowOpen =
      HasPendingFeature(state) || state.editFeatureId != 0 ||
      state.measure.active || state.dimTool.active || state.mateTool.active;
  float coveredFrac = 0.0f;
  if (toolWindowOpen && state.viewportSize.x > 1.0f &&
      state.toolWindowRect.z > state.toolWindowRect.x) {
    const float vpRight = state.viewportPos.x + state.viewportSize.x;
    const float overlap = std::max(
        0.0f, std::min(vpRight, state.toolWindowRect.z) -
                  std::max(state.viewportPos.x, state.toolWindowRect.x));
    coveredFrac = std::clamp(overlap / state.viewportSize.x, 0.0f, 0.45f);
  }
  const float usableX = 1.0f - coveredFrac;

  const float tanHalfLimit = tanHalfV * std::min(1.0f, aspect * usableX);
  const float distance =
      std::max(1.05f * radius / std::max(tanHalfLimit, 0.05f), 1.0f);
  cam.SetTarget(center);
  cam.SetMaxDistance(std::max(20.0f * distance, 1000.0f)); // let it zoom out
  cam.SetDistance(distance);
  if (coveredFrac > 0.01f) {
    // Half the covered strip, in world units at the target's depth.
    const glm::vec3 fwd = glm::normalize(center - cam.GetPosition());
    const glm::vec3 right = glm::normalize(glm::cross(fwd, cam.GetUp()));
    const float halfWidthWorld = distance * tanHalfV * aspect;
    cam.SetTarget(center - right * (halfWidthWorld * coveredFrac));
  }
}
} // namespace

void FitCameraToDocument(AppState& state) {
  if (!state.camera)
    return;

  glm::vec3 min(FLT_MAX), max(-FLT_MAX);
  bool any = false;
  if (state.assembly) {
    // Bound every visible occurrence's part bodies, transformed into place.
    for (const Occurrence& occ : state.assembly->GetOccurrences()) {
      if (!occ.visible || state.assembly->OccurrenceSuppressed(occ.id))
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
  FitCameraToBox(state, min, max);
}

void FitCameraToWorldBox(AppState& state, const glm::vec3& min,
                         const glm::vec3& max) {
  FitCameraToBox(state, min, max);
}

void FitCameraToSelection(AppState& state) {
  if (!state.camera)
    return;
  glm::vec3 min(FLT_MAX), max(-FLT_MAX);
  bool any = false;
  const auto addBox = [&](const glm::vec3& bmin, const glm::vec3& bmax,
                          const glm::mat4& xf) {
    for (int c = 0; c < 8; c++) { // transform all 8 corners
      const glm::vec3 corner((c & 1) ? bmax.x : bmin.x,
                             (c & 2) ? bmax.y : bmin.y,
                             (c & 4) ? bmax.z : bmin.z);
      const glm::vec3 w = glm::vec3(xf * glm::vec4(corner, 1.0f));
      min = glm::min(min, w);
      max = glm::max(max, w);
      any = true;
    }
  };
  // A feature's or body's geometry: the document bodies it created.
  const auto addBodiesOf = [&](uint64_t createdBy) {
    if (!state.document)
      return;
    for (const auto& body : state.document->GetBodies()) {
      if (body.createdBy != createdBy)
        continue;
      glm::vec3 bmin, bmax;
      body.shape.GetBoundingBox(bmin, bmax);
      addBox(bmin, bmax, glm::mat4(1.0f));
    }
  };
  for (const SelectionItem& it : state.selection.items) {
    switch (it.kind) {
    case SelectionItem::Kind::Face:
    case SelectionItem::Kind::Edge: {
      // Assembly picks carry their occurrence in `id` (0 = part context):
      // resolve against the part's bodies, place by the display transform.
      const std::vector<Body>* bodies = nullptr;
      glm::mat4 xf(1.0f);
      if (it.id != 0 && state.assembly) {
        const Occurrence* occ = state.assembly->FindById(it.id);
        Document* part = occ ? PartDocById(state, occ->partId) : nullptr;
        if (!part)
          break;
        bodies = &part->GetBodies();
        xf = DisplayMatrix(state, *occ);
      } else if (state.document) {
        bodies = &state.document->GetBodies();
      }
      if (bodies == nullptr)
        break;
      if (const auto b = Measure::ElementBounds(*bodies, it.element))
        addBox(b->min, b->max, xf);
      break;
    }
    case SelectionItem::Kind::Body:
    case SelectionItem::Kind::Feature:
      addBodiesOf(it.id);
      break;
    case SelectionItem::Kind::Occurrence: {
      if (!state.assembly)
        break;
      const Occurrence* occ = state.assembly->FindById(it.id);
      Document* part = occ ? PartDocById(state, occ->partId) : nullptr;
      if (!part)
        break;
      const glm::mat4 xf = DisplayMatrix(state, *occ);
      for (const auto& body : part->GetBodies()) {
        glm::vec3 bmin, bmax;
        body.shape.GetBoundingBox(bmin, bmax);
        addBox(bmin, bmax, xf);
      }
      break;
    }
    default:
      break; // mates/regions have no zoomable extent
    }
  }
  // Nothing selected (or nothing resolved): fit the whole document instead —
  // the natural meaning of "zoom to selection" with an empty selection.
  if (!any) {
    FitCameraToDocument(state);
    return;
  }
  FitCameraToBox(state, min, max);
}

// Muted, theme-matched palette cycled per body so bodies/parts read apart.
// External linkage (declared in AppActionsInternal.h): the assembly
// renderers moved to AssemblyActions.cpp still cycle this shared palette.
extern const glm::vec3 kBodyPalette[] = {
    {0.58f, 0.66f, 0.78f}, // steel blue
    {0.80f, 0.62f, 0.42f}, // amber
    {0.55f, 0.74f, 0.62f}, // sage
    {0.76f, 0.58f, 0.70f}, // mauve
    {0.72f, 0.72f, 0.55f}, // olive
};

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
  const std::vector<CADShape> caps =
      Measure::SectionCaps(bodies, planeOrigin, n);
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
      idx.push_back(a);
      idx.push_back(f1);
      idx.push_back(f2);
      // The -n copy is the opposite winding, on the second vertex block.
      idx.push_back(half + a);
      idx.push_back(half + f2);
      idx.push_back(half + f1);
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
  uint64_t featureId = 0; // the body's createdBy (owner of its element names)
  uint64_t refId = 0;     // semantic source id -> SheetCurve.sourceId
  const ElementMap* map = nullptr;
};

// Appends one part document's bodies at the given world placement.
void AppendBodies(const Document& doc, const glm::dmat4& xf, uint64_t occId,
                  std::vector<SourceBody>& out) {
  for (const Body& body : doc.GetBodies()) {
    if (body.shape.GetShape().IsNull())
      continue;
    SourceBody sb;
    sb.input.shape = body.shape.GetShape();
    sb.input.transform = xf;
    sb.input.sourceId = out.size(); // slot index: picks this entry on join
    sb.featureId = body.createdBy;
    sb.refId = occId ? occId : body.createdBy; // occurrence, else body owner
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
    if (state.parts[i].id != view.sourceDocId)
      continue;
    if (state.parts[i].kind == DocKind::Part) {
      const Document* doc = SlotDoc(state, i);
      if (!doc) {
        *error = "source document is empty";
        return false;
      }
      *sourceVersion = doc->Version();
      AppendBodies(*doc, glm::dmat4(1.0), 0, out);
      return true;
    }
    if (state.parts[i].kind == DocKind::Assembly) {
      const AssemblyDocument* asmDoc = SlotAssembly(state, i);
      if (!asmDoc) {
        *error = "source assembly is empty";
        return false;
      }
      *sourceVersion = asmDoc->Version();
      for (const Occurrence& occ : asmDoc->GetOccurrences()) {
        if (!occ.visible || asmDoc->OccurrenceSuppressed(occ.id))
          continue;
        const Document* part = PartDocById(state, occ.partId);
        if (!part)
          continue; // dangling part reference — skip
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
// The current version of whatever document a view projects (a part or an
// assembly); 0 when it cannot be resolved, which forces a re-projection.
uint64_t SourceVersionOf(AppState& state, const DrawingView& view) {
  if (Document* doc = PartDocById(state, view.sourceDocId))
    return doc->Version();
  for (const PartSlot& slot : state.parts) {
    if (slot.id != view.sourceDocId || slot.kind != DocKind::Assembly)
      continue;
    if (state.assembly && state.activePart < state.parts.size() &&
        state.parts[state.activePart].id == slot.id) {
      return state.assembly->Version();
    }
    if (slot.assembly)
      return slot.assembly->Version();
  }
  return 0;
}

drawing::SheetView ProjectDrawingView(AppState& state,
                                      const DrawingView& view) {
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
  for (const SourceBody& sb : bodies)
    inputs.push_back(sb.input);

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

  // Folding alignment (the one computation lives in DrawingFoldedPosition).
  const glm::dvec2 effectivePos = DrawingFoldedPosition(state, view).pos;

  glm::dvec2 lo(1e30), hi(-1e30);
  for (const drawing::ProjectedCurve& pc : projected->curves) {
    drawing::SheetCurve sc;
    sc.kind = pc.kind;
    sc.visible = pc.visible;
    sc.curveType = pc.curveType;
    sc.closed = pc.closed;
    if (pc.conicRadius > 0.0) { // place the conic on the sheet too
      sc.conicCenter = effectivePos + s * pc.conicCenter;
      sc.conicRadius = s * pc.conicRadius;
    }
    sc.points.reserve(pc.points.size());
    for (const glm::dvec2& p : pc.points) {
      const glm::dvec2 sheet = effectivePos + s * p;
      sc.points.push_back(sheet);
      lo = glm::min(lo, sheet);
      hi = glm::max(hi, sheet);
    }
    if (const SourceBody* sb = bodyOf(pc.source.sourceId)) {
      sc.sourceId = sb->refId; // occurrence id (assembly) or body owner (part)
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

FoldedPosition DrawingFoldedPosition(AppState& state, const DrawingView& view) {
  // A projected child is kept aligned to its base along the shared sheet
  // axis (design §2). Two orthographic views share sheet-X when they map the
  // same world direction to it (equal `right` vectors), and sheet-Y when
  // their `up` vectors match; the shared coordinate locks to the base's,
  // leaving the fold axis free. Iso shares neither -> placed freely.
  FoldedPosition out{view.position, false, false};
  if (!view.baseViewId || !state.drawing)
    return out;
  const DrawingView* base = state.drawing->FindView(view.baseViewId);
  if (!base)
    return out;
  glm::dvec3 vd, up, bvd, bup;
  OrientationAxes(view.orientation, vd, up);
  OrientationAxes(base->orientation, bvd, bup);
  const glm::dvec3 right = glm::normalize(glm::cross(vd, up));
  const glm::dvec3 bright = glm::normalize(glm::cross(bvd, bup));
  constexpr double eps = 1e-6;
  if (glm::length(right - bright) < eps) {
    out.pos.x = base->position.x;
    out.xLocked = true;
  }
  if (glm::length(up - bup) < eps) {
    out.pos.y = base->position.y;
    out.yLocked = true;
  }
  return out;
}

drawing::ResolvedDim ResolveDrawingDim(AppState& state, const DrawingDim& dim) {
  const auto it = state.drawingRenders.find(dim.viewId);
  if (it == state.drawingRenders.end()) {
    drawing::ResolvedDim rd; // the view isn't projected: unresolved
    rd.id = dim.id;
    rd.kind = dim.kind;
    return rd;
  }
  // Callout kinds need the source part document (hole provenance) and the
  // view scale (sheet mm -> model mm); legacy kinds ignore the context.
  drawing::DimContext ctx;
  if (state.drawing) {
    if (const DrawingView* view = state.drawing->FindView(dim.viewId)) {
      ctx.sourceDoc = DrawingSourceDocument(state, view->sourceDocId);
      ctx.viewScale = view->scale;
    }
  }
  return drawing::ResolveDim(dim, it->second, ctx);
}

bool ExportActiveDrawing(AppState& state, const std::string& path,
                         std::string* error) {
  if (!state.drawing) {
    if (error)
      *error = "no active drawing";
    return false;
  }
  UpdateDrawingRenders(state); // export reflects current geometry
  std::vector<drawing::SheetView> views;
  for (const DrawingView& v : state.drawing->GetViews()) {
    const auto it = state.drawingRenders.find(v.id);
    if (it != state.drawingRenders.end())
      views.push_back(it->second);
  }
  std::vector<drawing::ResolvedDim> dims;
  for (const DrawingDim& dm : state.drawing->GetDims())
    dims.push_back(ResolveDrawingDim(state, dm));
  std::string ext = std::filesystem::path(path).extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
  if (ext == ".svg")
    return DrawingExport::SaveSVG(*state.drawing, views, dims, path, error);
  if (ext == ".pdf")
    return DrawingExport::SavePDF(*state.drawing, views, dims, path, error);
  if (error)
    *error = "unsupported drawing format '" + ext + "' (use .svg or .pdf)";
  return false;
}

uint64_t DrawingSourceVersion(AppState& state, uint64_t sourceDocId) {
  for (size_t i = 0; i < state.parts.size(); i++) {
    if (state.parts[i].id != sourceDocId)
      continue;
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

Document* DrawingSourceDocument(AppState& state, uint64_t sourceDocId) {
  for (size_t i = 0; i < state.parts.size(); i++) {
    if (state.parts[i].id == sourceDocId &&
        state.parts[i].kind == DocKind::Part)
      return SlotDoc(state, i);
  }
  return nullptr;
}

void AddDrawingViewPlaced(AppState& state, DrawingView v) {
  if (!state.drawing)
    return;
  DrawingDocument& dwg = *state.drawing;
  const glm::dvec2 ext = ChromeOf(dwg.GetSheet()).extent;

  // Project a 1:1 copy anchored at the origin: its bbox IS the part's size in
  // millimetres, which is what both decisions below need.
  DrawingView probe = v;
  probe.id = 0;
  probe.scale = 1.0;
  probe.position = glm::dvec2(0.0);
  probe.baseViewId = 0; // a folded child would inherit its parent's place
  const drawing::SheetView sv = ProjectDrawingView(state, probe);
  const bool measured = sv.hasGeometry;
  const glm::dvec2 size = measured ? sv.bboxMax - sv.bboxMin : glm::dvec2(0.0);

  // Scale (only when the caller did not choose one): the largest standard
  // ratio that keeps the view inside ~45% of the sheet, so a few views fit
  // side by side. Without this a 2 mm part is a 2 mm dot at 1:1.
  if (v.scale <= 0.0) {
    v.scale = 1.0;
    if (measured && size.x > 1e-9 && size.y > 1e-9) {
      static const double kStandard[] = {100.0, 50.0, 20.0, 10.0, 5.0,
                                         2.0,   1.0,  0.5,  0.2,  0.1,
                                         0.05,  0.02, 0.01};
      const glm::dvec2 budget = ext * 0.45;
      v.scale = kStandard[std::size(kStandard) - 1];
      for (double s : kStandard) {
        if (size.x * s <= budget.x && size.y * s <= budget.y) {
          v.scale = s;
          break;
        }
      }
    }
  }

  // Position: sheet centre for the first view, else clear to the right of
  // everything already placed by the shared rule (ui/Placement.h), wrapping
  // below when the row is full. The sheet anchor maps model -> pos + scale *
  // model, so a left edge at `x` means pos.x = x - scale * bboxMin.x.
  glm::dvec2 pos = ext * 0.5;
  if (measured) {
    const SheetChrome chrome = ChromeOf(dwg.GetSheet());
    std::vector<Span> takenX;
    double lowest = 1e30, firstMidY = ext.y * 0.5;
    bool anyPlaced = false;
    for (const DrawingView& other : dwg.GetViews()) {
      const auto it = state.drawingRenders.find(other.id);
      if (it == state.drawingRenders.end() || !it->second.hasGeometry)
        continue;
      if (!anyPlaced)
        firstMidY = (it->second.bboxMin.y + it->second.bboxMax.y) * 0.5;
      anyPlaced = true;
      takenX.push_back({it->second.bboxMin.x, it->second.bboxMax.x});
      lowest = std::min(lowest, it->second.bboxMin.y);
    }
    const glm::dvec2 placed = size * v.scale;
    const Span incoming{v.scale * sv.bboxMin.x, v.scale * sv.bboxMax.x};
    const double gap = std::max(8.0, std::max(placed.x, placed.y) * 0.15);
    if (!anyPlaced) {
      pos = ext * 0.5 - v.scale * (sv.bboxMin + sv.bboxMax) * 0.5;
    } else if (const double dx = ClearOffset(takenX, incoming, 8.0);
               dx + v.scale * sv.bboxMax.x <= chrome.borderMax.x) {
      pos.x = dx; // same row, to the right of everything placed
      pos.y = firstMidY - v.scale * (sv.bboxMin.y + sv.bboxMax.y) * 0.5;
    } else {
      // A new row BELOW the others (sheet Y grows upward), starting at the
      // left margin — and stopping above the title block, which a wrapped
      // row used to land on top of.
      pos.x = chrome.borderMin.x + gap - v.scale * sv.bboxMin.x;
      pos.y = lowest - gap - v.scale * sv.bboxMax.y;
      const Span rowX{pos.x + v.scale * sv.bboxMin.x,
                      pos.x + v.scale * sv.bboxMax.x};
      const Span blockX{chrome.titleBlockMin.x, chrome.titleBlockMax.x};
      const double floorY =
          Overlaps(rowX, blockX) ? chrome.titleBlockMax.y : chrome.borderMin.y;
      if (pos.y + v.scale * sv.bboxMin.y < floorY + gap)
        pos.y = floorY + gap - v.scale * sv.bboxMin.y;
    }
  }
  v.position = pos;

  SnapshotDrawing(state);
  dwg.AddView(v);
  UpdateDrawingRenders(state);
}

void UpdateDrawingRenders(AppState& state) {
  if (!state.drawing)
    return;
  // Re-project only what can have gone stale. HLR is the expensive thing on a
  // sheet (about half a second per view of a 50-occurrence assembly), and
  // export calls this before writing so the file cannot be stale — which made
  // exporting an unchanged 12-view sheet cost 6 s of pure recomputation. A
  // view is still good when the drawing itself has not changed since the
  // cache was built AND its source document is at the version it was
  // projected from. (The drawing's version covers scale/orientation/position
  // edits, which are properties of the view rather than of its source.)
  const uint64_t drawingVersion = state.drawing->Version();
  const bool drawingChanged = drawingVersion != state.drawingRendersVersion;
  std::map<uint64_t, drawing::SheetView> fresh;
  for (const DrawingView& view : state.drawing->GetViews()) {
    if (!drawingChanged) {
      const auto prev = state.drawingRenders.find(view.id);
      if (prev != state.drawingRenders.end() && prev->second.hasGeometry &&
          prev->second.error.empty() &&
          prev->second.solvedSourceVersion == SourceVersionOf(state, view)) {
        fresh[view.id] = prev->second;
        continue;
      }
    }
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
  state.drawingRendersVersion = drawingVersion;
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
  // The topology shape-guide ghost is likewise rebuilt each resync.
  if (state.topoGhostInstance) {
    state.meshRenderer->RemoveMesh(state.topoGhostInstance);
    state.topoGhostInstance = 0;
  }

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
    if (state.hiddenBodies.count(body.createdBy) != 0)
      continue; // hidden (body context menu): no mesh/outline/picking
    CADShape::Mesh cadMesh = body.shape.GenerateMesh(0.1);
    if (cadMesh.vertices.empty())
      continue;

    std::vector<Vertex3D> vertices =
        CADMeshConverter::ConvertCADMeshToVertices(cadMesh);
    uint32_t meshId = state.meshRenderer->AddMesh(vertices, cadMesh.indices);
    // Albedo is per-instance state (not baked into vertices): palette color
    // by default; a body with an assigned material overrides it with the
    // material's base color and real PBR params.
    glm::vec3 albedo = body.hasDisplayColor
                           ? body.displayColor
                           : kBodyPalette[palette++ % std::size(kBodyPalette)];
    state.meshRenderer->SetMeshColor(meshId, albedo);
    // Resolve order: appearance > material > displayColor > palette. The
    // override edits the CPU-resolved params, so the 128-byte push constant
    // never grows (the appearance-plan constraint).
    if (!body.material.empty() || body.appearance.Any()) {
      CADMaterials::MaterialProperties mat =
          CADMaterials::MaterialLibrary::GetMaterial(
              CADMaterials::MaterialLibrary::FromSlug(body.material));
      if (body.material.empty()) {
        // No physical material: a neutral dielectric carrying the palette
        // color, so a color-only override doesn't inherit DEFAULT's metal.
        mat.baseColor = albedo;
        mat.metallic = 0.0f;
        mat.roughness = 0.6f;
        mat.specular = 0.5f;
        mat.reflectance = 0.04f;
        mat.procPattern = 0;
      }
      if (body.appearance.color)
        mat.baseColor = *body.appearance.color;
      if (body.appearance.roughness)
        mat.roughness = *body.appearance.roughness;
      if (body.appearance.metallic)
        mat.metallic = *body.appearance.metallic;
      state.meshRenderer->SetMeshMaterial(meshId, mat.baseColor,
                                          glm::vec4(mat.metallic, mat.roughness,
                                                    mat.specular,
                                                    mat.reflectance));
      // Procedural wood/plywood grain (pattern 0 = flat, so non-wood materials
      // pass through unchanged). An overridden color KEEPS the grain — that
      // is stained wood, the desired reading.
      state.meshRenderer->SetMeshProcedural(meshId, mat.procPattern,
                                            mat.grainScale, mat.grainContrast);
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

  // Topology shape-guide ghost (TO4): outside simulation mode, render the
  // optimized iso-surface translucent so the user can re-model against it.
  // The instance is tracked apart from documentMeshes, so remove any stale
  // one FIRST — otherwise it survives tab switches and haunts other parts
  // (whose documents can't resolve the study, so the re-add never runs).
  if (state.topoGhostInstance != 0) {
    state.meshRenderer->RemoveInstance(state.topoGhostInstance);
    state.topoGhostInstance = 0;
  }
  if (state.topoGhostStudyId && !state.simulation.active) {
    const fem::StudyRuntime* rt =
        state.document->StudyResults(state.topoGhostStudyId);
    if (rt && rt->topology) {
      std::vector<glm::vec3> gpos, gnrm;
      std::vector<uint32_t> gidx;
      for (const fem::StudyBodyResult& b : rt->bodies) {
        const double t = fem::AutoIsoThreshold(b.mesh, b.density,
                                               rt->volumeFractionAchieved);
        fem::ExtractIsoSurface(b.mesh, b.density, t, gpos, gnrm, gidx);
      }
      if (!gpos.empty()) {
        std::vector<Vertex3D> gv(gpos.size());
        for (size_t i = 0; i < gpos.size(); ++i) {
          gv[i].position = gpos[i];
          gv[i].normal = gnrm[i];
        }
        state.topoGhostInstance = state.meshRenderer->AddMesh(gv, gidx);
        state.meshRenderer->SetMeshColor(state.topoGhostInstance,
                                         glm::vec3(0.45f, 0.75f, 1.0f));
        state.meshRenderer->SetInstanceGhost(state.topoGhostInstance, 0.35f);
      }
    }
  }

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
  if (!state.rayTracer || !state.rayTracer->IsInitialized())
    return false;
  if (!state.document && !state.assembly)
    return false;

  // What to render: a part's bodies at identity, or — on an assembly tab —
  // every visible occurrence's bodies at its SOLVED placement (the
  // BuildVisualExportItems walk; never exploded).
  struct RTItem {
    const Body* body;
    glm::mat4 xf;
  };
  std::vector<RTItem> rtItems;
  if (state.assembly) {
    for (const Occurrence& occ : state.assembly->GetOccurrences()) {
      if (!occ.visible || state.assembly->OccurrenceSuppressed(occ.id))
        continue;
      Document* part = PartDocById(state, occ.partId);
      if (!part)
        continue;
      const glm::mat4 xf = OccurrenceMatrix(occ);
      for (const auto& body : part->GetBodies())
        rtItems.push_back({&body, xf});
    }
  } else {
    for (const auto& body : state.document->GetBodies())
      rtItems.push_back({&body, glm::mat4(1.0f)});
  }

  state.rayTracer->ClearGeometry();
  state.rayTracer->ClearMaterials();

  glm::vec3 bbMin(FLT_MAX), bbMax(-FLT_MAX);
  for (const RTItem& it : rtItems) {
    const Body& body = *it.body;
    CADShape::Mesh cadMesh = body.shape.GenerateMesh(0.1);
    if (cadMesh.vertices.empty())
      continue;

    CADMaterials::MaterialType mt =
        body.material.empty()
            ? CADMaterials::MaterialType::DEFAULT
            : CADMaterials::MaterialLibrary::FromSlug(body.material);
    CADMaterials::MaterialProperties mat =
        CADMaterials::MaterialLibrary::GetMaterial(mt);
    // Appearance override, same merge as the raster paths.
    if (body.appearance.Any()) {
      if (body.material.empty()) {
        mat.metallic = 0.0f;
        mat.roughness = 0.6f;
        mat.specular = 0.5f;
        mat.procPattern = 0;
      }
      if (body.appearance.color)
        mat.baseColor = *body.appearance.color;
      if (body.appearance.roughness)
        mat.roughness = *body.appearance.roughness;
      if (body.appearance.metallic)
        mat.metallic = *body.appearance.metallic;
    }

    std::vector<Vertex3D> vertices =
        CADMeshConverter::ConvertCADMeshToVertices(cadMesh);
    if (it.xf != glm::mat4(1.0f)) {
      // Occurrence placement is rigid: positions by the matrix, normals by
      // its rotation. RT geometry carries no per-instance transform, so the
      // bake happens CPU-side (a still render, not a per-frame path).
      const glm::mat3 rot(it.xf);
      for (Vertex3D& v : vertices) {
        v.position = glm::vec3(it.xf * glm::vec4(v.position, 1.0f));
        v.normal = glm::normalize(rot * v.normal);
      }
    }

    // Per-body local bbox: feeds the scene bbox (ground plane) and the grain
    // axis. RT geometry goes in untransformed, so this is object/part-local.
    glm::vec3 lo(FLT_MAX), hi(-FLT_MAX);
    for (const Vertex3D& v : vertices) {
      lo = glm::min(lo, v.position);
      hi = glm::max(hi, v.position);
    }
    bbMin = glm::min(bbMin, lo);
    bbMax = glm::max(bbMax, hi);

    RTMaterial rtm{};
    rtm.baseColor = mat.baseColor;
    rtm.metallic = mat.metallic;
    rtm.roughness = mat.roughness;
    rtm.emissive = glm::vec3(0.0f);
    rtm.specular = glm::vec3(mat.specular);
    rtm.transmission = 0.0f;
    if (mat.procPattern > 0) {
      // Same rule as the raster path (MeshRenderer draw loop): solid wood runs
      // the grain along the longest extent, plywood lays plies across the
      // thinnest. Each body gets its own material here, so the axis rides in
      // it.
      const glm::vec3 ext = hi - lo;
      const bool plywood = mat.procPattern == 2;
      int axis = 0;
      for (int a = 1; a < 3; ++a) {
        if (plywood ? (ext[a] < ext[axis]) : (ext[a] > ext[axis]))
          axis = a;
      }
      rtm.procParams =
          glm::vec4(static_cast<float>(mat.procPattern), mat.grainScale,
                    mat.grainContrast, static_cast<float>(axis));
    }
    uint32_t matId = state.rayTracer->AddMaterial(rtm);

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
    const float half = 10.0f * diag;         // covers every camera angle
    const float y = bbMin.y - 0.001f * diag; // just below: no coplanar fizz
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

// A thick screen-space line segment (two triangles). The B-end negates
// corner.x so both ends offset to the same world side (no bowtie). Public:
// the selection pipeline draws its edge overlays with it too.
void AppendPrimLine(glm::vec3 a, glm::vec3 b, float halfPx,
                    const glm::vec4& color, std::vector<PrimVertex>& out) {
  PrimVertex am{a, b, {-1, 0}, {halfPx, 0}, color};
  PrimVertex ap{a, b, {1, 0}, {halfPx, 0}, color};
  PrimVertex bp{b, a, {-1, 0}, {halfPx, 0}, color}; // logical +side at B
  PrimVertex bm{b, a, {1, 0}, {halfPx, 0}, color};  // logical -side at B
  for (const PrimVertex& v : {am, ap, bp, am, bp, bm})
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

namespace {
// The bodies + rigid world transform (+ a display prefix) one measured
// element resolves against: the active part's bodies at identity, or — on an
// assembly tab — the occurrence's part bodies at the SOLVED occurrence
// transform (real geometry, not the exploded display offset).
struct MeasureSource {
  const std::vector<Body>* bodies = nullptr;
  glm::mat4 xf{1.0f};
  std::string prefix; // "OccurrenceName: " on assemblies, "" on parts
};
std::optional<MeasureSource>
ResolveMeasureSource(AppState& state, const MeasureState::Element& e) {
  if (e.occurrenceId == 0) {
    if (!state.document)
      return std::nullopt;
    return MeasureSource{&state.document->GetBodies(), glm::mat4(1.0f), ""};
  }
  if (!state.assembly)
    return std::nullopt;
  const Occurrence* occ = state.assembly->FindById(e.occurrenceId);
  if (!occ)
    return std::nullopt;
  Document* part = PartDocById(state, occ->partId);
  if (!part)
    return std::nullopt;
  return MeasureSource{&part->GetBodies(), OccurrenceMatrix(*occ),
                       occ->name + ": "};
}
} // namespace

void UpdateMeasureReadout(AppState& state) {
  auto& m = state.measure;
  m.hasDistanceLine = false;
  if ((!state.document && !state.assembly) || m.picked.empty()) {
    m.readout.clear();
    return;
  }
  if (m.picked.size() == 1) {
    const auto src = ResolveMeasureSource(state, m.picked[0]);
    if (src) {
      if (const auto e = Measure::Describe(*src->bodies, m.picked[0].ref)) {
        m.readout = src->prefix + FormatElement(*e);
        return;
      }
    }
    m.readout = "Could not measure that element.";
    return;
  }
  if (m.picked.size() == 2) {
    const auto srcA = ResolveMeasureSource(state, m.picked[0]);
    const auto srcB = ResolveMeasureSource(state, m.picked[1]);
    std::optional<Measure::DistanceResult> d;
    if (srcA && srcB)
      d = Measure::DistanceDetailed(*srcA->bodies, m.picked[0].ref, srcA->xf,
                                    *srcB->bodies, m.picked[1].ref, srcB->xf);
    char buf[192];
    if (d) {
      const glm::vec3 delta = d->pointB - d->pointA;
      snprintf(buf, sizeof buf,
               "Distance: %.3f mm (nearest)\ndx %.3f  dy %.3f  dz %.3f mm",
               d->distance, std::abs(delta.x), std::abs(delta.y),
               std::abs(delta.z));
      m.hasDistanceLine = true;
      m.distA = d->pointA;
      m.distB = d->pointB;
    } else {
      snprintf(buf, sizeof buf, "Could not measure the distance.");
    }
    m.readout = buf;
    return;
  }
  m.readout = std::to_string(m.picked.size()) +
              " elements selected - keep two for a distance";
}

void MeasureElementClicked(AppState& state, const ElementRef& ref, bool isEdge,
                           uint64_t occurrenceId) {
  if (!state.measure.active || (!state.document && !state.assembly))
    return;
  auto& picked = state.measure.picked;
  const auto it = std::find_if(
      picked.begin(), picked.end(), [&](const MeasureState::Element& e) {
        return e.ref.featureId == ref.featureId && e.ref.name == ref.name &&
               e.occurrenceId == occurrenceId;
      });
  if (it != picked.end())
    picked.erase(it); // clicking a measured element again deselects it
  else
    picked.push_back({ref, isEdge, occurrenceId});
  UpdateMeasureReadout(state);
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
// feature) so they survive undo/redo; see docs/sketch/reference-image-plan.md.
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
  if (!state.document || !state.commands) {
    if (error)
      *error = "no active part document";
    return false;
  }

  const size_t before = state.document->GetBodies().size();
  const std::string name = std::filesystem::path(filename).stem().string();
  auto feature = std::make_unique<ImportFeature>(name, filename);
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
