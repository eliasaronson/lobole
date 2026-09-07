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
  // A fresh cylinder stands upright: base one unit below the spot, axis
  // along world up.
  BeginPreviewFeature(state, std::make_unique<CylinderFeature>(
                                 NextName(state, "Cylinder"),
                                 NextSpot(state) - conv::kWorldUpF,
                                 conv::kWorldUpF, 0.8, 2.0));
}

void CreateDatumPlane(AppState& state) {
  auto dp = std::make_unique<DatumPlaneFeature>(NextName(state, "Plane"),
                                                DatumPlaneKind::Offset);
  // Build on the selected datum if one is selected, else the XY base plane.
  if (auto* parent = dynamic_cast<DatumPlaneFeature*>(SelectedFeature(state)))
    dp->parentId = parent->GetId();

  // Clear of the model, along the PARENT's normal — not a fixed 2.0, which
  // is right only for a 2-unit box and lands on a face, inside a part, or
  // out of sight for anything else. (A plane coincident with a face is the
  // pathological case: it looks placed and refuses to cut.) A margin of a
  // quarter of the model's own extent keeps it visibly separate at any size.
  if (const auto parentPlane =
          ResolvePlaneForTool(state, dp->parentId, dp->basePlane)) {
    const glm::vec3 n = parentPlane->GetNormal();
    const glm::vec3 o = parentPlane->GetOrigin();
    float lo = std::numeric_limits<float>::max();
    float hi = std::numeric_limits<float>::lowest();
    for (const Body& b :
         state.document ? state.document->GetBodies() : std::vector<Body>{}) {
      glm::vec3 bmin, bmax;
      b.shape.GetBoundingBox(bmin, bmax);
      for (int c = 0; c < 8; c++) {
        const glm::vec3 corner((c & 1) ? bmax.x : bmin.x,
                               (c & 2) ? bmax.y : bmin.y,
                               (c & 4) ? bmax.z : bmin.z);
        const float d = glm::dot(corner - o, n);
        lo = std::min(lo, d);
        hi = std::max(hi, d);
      }
    }
    if (hi > lo)
      dp->offset = static_cast<double>(hi + 0.25f * (hi - lo));
    // else: an empty document has no size to derive from, so the feature's
    // own default (a base-plane grid square) stands.
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

void CreateFrame(AppState& state) {
  // The part origin, Z up: the one frame every part has, and the identity
  // no unit has to be guessed for. The editor turns it into an element or
  // offset frame; a script passes the definition outright.
  auto fr = std::make_unique<FrameFeature>(NextName(state, "Frame"),
                                           FrameKind::Explicit);
  BeginPreviewFeature(state, std::move(fr));
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

std::optional<SketchPlane>
ResolvePlaneForTool(AppState& state, uint64_t datumId, PlaneType basePlane) {
  if (!state.document)
    return std::nullopt;
  if (datumId == 0)
    return SketchPlane(basePlane);
  for (const auto& f : state.document->GetFeatures()) {
    if (f && f->GetId() == datumId) {
      if (auto* dp = dynamic_cast<DatumPlaneFeature*>(f.get()))
        return dp->OutputPlane();
      return std::nullopt;
    }
  }
  return std::nullopt;
}

void RecenterSplitOffset(AppState& state, SplitFeature& split) {
  if (!state.document || !split.offsetExpr.empty())
    return; // an expression is the user's, not ours to overwrite
  const auto plane = ResolvePlaneForTool(state, split.datumId, split.basePlane);
  if (!plane)
    return;
  // What to measure against depends on whether the split is CURRENTLY
  // cutting: on success the source body is gone, replaced by pieces stamped
  // with this feature's id (together they are the source); on failure the
  // source is still there untouched. Take the bounds over either, or a plane
  // change from a working split would find nothing to center on.
  glm::vec3 bmin(std::numeric_limits<float>::max());
  glm::vec3 bmax(std::numeric_limits<float>::lowest());
  bool found = false;
  for (const Body& b : state.document->GetBodies()) {
    if (b.createdBy != split.bodyId && b.createdBy != split.GetId())
      continue;
    glm::vec3 lo, hi;
    b.shape.GetBoundingBox(lo, hi);
    bmin = glm::min(bmin, lo);
    bmax = glm::max(bmax, hi);
    found = true;
  }
  if (found)
    split.offset =
        CenteredSplitOffset(plane->GetOrigin(), plane->GetNormal(), bmin, bmax);
}

void CreateSplit(AppState& state) {
  if (!state.document || state.document->GetBodies().empty())
    return;
  auto split = std::make_unique<SplitFeature>(NextName(state, "Split"),
                                              NewestBody(state));
  // A plane through the origin misses any body that does not sit there, and
  // the feature would fail before the user has touched anything. Land the
  // knife mid-body instead — along the PLANE'S normal, not along z: the same
  // derivation runs again whenever the plane changes (RecenterSplitOffset),
  // because an offset only means something relative to a given plane.
  RecenterSplitOffset(state, *split);
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

void ReplaceDocument(AppState& state, std::unique_ptr<Document> doc) {
  // A wholesale replacement is followed by state.parts.clear() and a reset of
  // nextPartId, so slot ids get REUSED. A cache entry keyed on an old id
  // would then look like a hit for an unrelated new part — and its stored
  // docVersion could match a fresh document's, which draws the previous
  // project's geometry. Drop it all here rather than at each of the four
  // parts.clear() sites, where a fifth would eventually be missed.
  ReleasePartRenderCache(state);
  state.document = std::move(doc);
  state.commands = std::make_unique<CommandStack>();
  // The active tab may have been an assembly or drawing: this replacement
  // makes the live tab a part, so drop the other-kind state — leaving
  // state.assembly set alongside state.document half-switches the UI (the
  // assembly toolbar keeps drawing and the part never syncs; File > New on
  // an assembly tab hit exactly that).
  state.assembly = nullptr;
  state.drawing = nullptr;
  state.job = nullptr;
  state.assemblyHistory = {};
  state.drawingHistory = {};
  state.jobHistory = {};
  state.drawingRenders.clear();
  state.drawingRendersVersion = 0;
  // The job's assembled bodies are keyed on document ids that this
  // replacement is about to reuse, so a stale cache reads as a hit for an
  // unrelated part -- the same trap ReleasePartRenderCache exists for.
  state.jobBodies.clear();
  state.jobBodiesKey = 0;
  state.jobPlacedBodies.clear();
  state.jobPlacedKey = 0;
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
  // The CAM job belonged to the document that just went away. Leaving this
  // behind made the panel claim "Z zero at 17.000" and offer to Save G-code
  // for a part that no longer existed — and an armed face pick survived into
  // the new document, where the button that would re-arm it is disabled
  // while a pick is live, so the panel was simply stuck.
  state.camToolpaths.clear();
  state.camVerdict.clear();
  state.camLeftover.clear();
  state.camActiveSetup = 0;
  state.camGhostOp = 0;
  if (state.slotPick.featureId == kCamToolSlot)
    state.slotPick = {};
  // Every other TOOL SESSION goes with the document too, for the same reason
  // the assembly/drawing state above does: a session left open is editing
  // something that no longer exists. `editFeatureId` is the sharp one — it
  // names a feature in the document that was just discarded — and camToolOpen
  // is the one that bit: it is a toggle, so a stale one turned OFF at the next
  // click on the toolbar button meant to turn it on.
  state.editFeatureId = 0;
  state.dimTool = AppState::DimTool{};
  state.mateTool = AppState::MateToolState{};
  state.camToolOpen = false;
  state.camVerb = AppState::CamVerb::None;
  state.camVerbOp = 0;
  // Force a resync even if the version counters happen to collide
  state.syncedDocumentVersion = UINT64_MAX;
}

namespace {

// Defined further down, beside the other tab-slot plumbing: a slot's document
// whether it is the active tab (AppState holds it) or a stashed one.

// A tab name nothing else has taken: "<source> copy", then "<source> copy 2".
// A copy cannot inherit its original's name — names ADDRESS tabs (the RPC
// refuses a duplicate outright) and each one becomes that document's directory
// when the project is saved, so two of them is two documents writing to one
// place.
std::string FreshCopyName(const AppState& state, const std::string& source) {
  const auto taken = [&state](const std::string& name) {
    for (const PartSlot& slot : state.parts)
      if (slot.name == name)
        return true;
    return false;
  };
  std::string candidate = source + " copy";
  for (int n = 2; taken(candidate); n++)
    candidate = source + " copy " + std::to_string(n);
  return candidate;
}
} // namespace

// Hoisted out of the anonymous namespace by the stage-3 split: the tab-slot
// accessors are the seam ProjectActions.cpp reaches the slots through, so
// they are declared in AppActionsInternal.h and defined here.

// Move the active tab's live model (part doc+commands OR assembly) back into
// its slot. Moving a null pointer is a no-op, so this handles both kinds.
void StashActive(AppState& state) {
  PartSlot& slot = state.parts[state.activePart];
  slot.doc = std::move(state.document);
  slot.assembly = std::move(state.assembly);
  slot.drawing = std::move(state.drawing);
  slot.job = std::move(state.job);
  slot.commands = std::move(state.commands);
  slot.history = std::move(state.assemblyHistory);
  slot.drawingHistory = std::move(state.drawingHistory);
  slot.jobHistory = std::move(state.jobHistory);
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
  state.job = std::move(slot.job);
  state.commands = std::move(slot.commands);
  state.assemblyHistory = std::move(slot.history);
  state.drawingHistory = std::move(slot.drawingHistory);
  state.jobHistory = std::move(slot.jobHistory);
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
  // ...and drops the measure session. Its picks are ElementRefs into the
  // document that was on screen; carried onto another tab they resolve
  // against nothing while the readout keeps displaying the old distance over
  // geometry that was never measured. Simulation was reset here from the
  // start and measure was simply missed.
  state.measure = MeasureState{};
  // A job tab's panel is the CAM panel, and it shares the left node with
  // Variables, History and the rest: without this the tab that was in front
  // on the part stays in front here, and the job's list is behind it.
  // Repeated frames, as simulation mode does (DockLayout.cpp).
  if (state.job) {
    state.focusPanel = "CAM";
    state.focusPanelFrames = 60;
  }
  state.syncedDocumentVersion = UINT64_MAX; // force a renderer resync
}

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
  state.job = nullptr;
  state.commands = std::make_unique<CommandStack>();
  state.lastSavedVersion = 0;
  state.solvedPartVersions.clear();
  PartSlot slot;
  slot.id = state.nextPartId++;
  slot.name = name;
  slot.kind = DocKind::Part;
  state.parts.push_back(std::move(slot));
  state.activePart = static_cast<int>(state.parts.size()) - 1;
  // Which documents the project HAS lives in project.json, not in any
  // document version — so without this the project reads clean with a brand
  // new (still empty) tab in it, and closing loses the tab with no prompt.
  state.projectMetaDirty = true;
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
  state.job = nullptr;
  state.commands = nullptr;
  state.lastSavedVersion = 0;
  state.solvedPartVersions.clear();
  PartSlot slot;
  slot.id = state.nextPartId++;
  slot.name = name;
  slot.kind = DocKind::Assembly;
  state.parts.push_back(std::move(slot));
  state.activePart = static_cast<int>(state.parts.size()) - 1;
  // Which documents the project HAS lives in project.json, not in any
  // document version — so without this the project reads clean with a brand
  // new (still empty) tab in it, and closing loses the tab with no prompt.
  state.projectMetaDirty = true;
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
  state.job = nullptr;
  state.commands = nullptr;
  state.lastSavedVersion = 0;
  state.solvedPartVersions.clear();
  PartSlot slot;
  slot.id = state.nextPartId++;
  slot.name = name;
  slot.kind = DocKind::Drawing;
  state.parts.push_back(std::move(slot));
  state.activePart = static_cast<int>(state.parts.size()) - 1;
  // Which documents the project HAS lives in project.json, not in any
  // document version — so without this the project reads clean with a brand
  // new (still empty) tab in it, and closing loses the tab with no prompt.
  state.projectMetaDirty = true;
  ResetActiveTabUiState(state);
  return state.parts.size() - 1;
}

const std::vector<Body>& ViewportBodies(AppState& state) {
  static const std::vector<Body> kNone;
  if (state.job)
    return JobPlacedBodies(state);
  return state.document ? state.document->GetBodies() : kNone;
}

size_t AddJob(AppState& state, const std::string& name) {
  EnsureParts(state);
  CancelPendingFeature(state); // before StashActive — see SetActivePart
  StashActive(state);
  // Create and activate the new job. Undo is a snapshot stack (the drawing
  // pattern): a job is a parts list and a few setups, small enough that a
  // full copy per edit beats generalizing the Document-only CommandStack.
  state.document = nullptr;
  state.assembly = nullptr;
  state.drawing = nullptr;
  state.job = std::make_unique<cam::JobDocument>();
  state.commands = nullptr;
  state.lastSavedVersion = 0;
  state.solvedPartVersions.clear();
  PartSlot slot;
  slot.id = state.nextPartId++;
  slot.name = name;
  slot.kind = DocKind::Job;
  state.parts.push_back(std::move(slot));
  state.activePart = static_cast<int>(state.parts.size()) - 1;
  // Which documents the project HAS lives in project.json, not in any
  // document version — so without this the project reads clean with a brand
  // new (still empty) tab in it, and closing loses the tab with no prompt.
  state.projectMetaDirty = true;
  ResetActiveTabUiState(state);
  return state.parts.size() - 1;
}

size_t DuplicateDocument(AppState& state, size_t index) {
  EnsureParts(state);
  if (index >= state.parts.size())
    return SIZE_MAX;
  const PartSlot& source = state.parts[index];

  PartSlot slot;
  slot.id = state.nextPartId++;
  slot.name = FreshCopyName(state, source.name);
  slot.kind = source.kind;
  // The copy lands where the original lives: a part duplicated inside an
  // imported assembly's folder belongs in that folder, not loose on the bar.
  slot.folder = source.folder;
  // Read through the slot accessors, so duplicating the tab you are LOOKING at
  // (its document lives in AppState, not in its slot) is the same code path as
  // duplicating any other.
  switch (source.kind) {
  case DocKind::Part: {
    const Document* doc = SlotDoc(state, index);
    if (!doc)
      return SIZE_MAX;
    // Deep copy: features cloned, geometry regenerated (Document's copy
    // constructor). The copy inherits the project scope pointers from the
    // original, so its expressions resolve project variables immediately.
    slot.doc = std::make_unique<Document>(*doc);
    // A fresh, EMPTY undo stack. The original's history describes edits to a
    // document that is not this one; offering to undo them here would undo
    // them into the copy.
    slot.commands = std::make_unique<CommandStack>();
    break;
  }
  case DocKind::Assembly: {
    const AssemblyDocument* asmDoc = SlotAssembly(state, index);
    if (!asmDoc)
      return SIZE_MAX;
    // Occurrences keep referencing the same PARTS (by stable id) — duplicating
    // an assembly copies the arrangement, it does not fork the parts in it.
    // Mates and exploded views reference occurrences inside the copy, so they
    // need no repointing.
    slot.assembly = std::make_unique<AssemblyDocument>(*asmDoc);
    break;
  }
  case DocKind::Drawing: {
    const DrawingDocument* dwg = SlotDrawing(state, index);
    if (!dwg)
      return SIZE_MAX;
    // Views carry a sourceDocId, so the copy draws the same part. Its
    // projected geometry is NOT copied: SetActivePart below projects a drawing
    // whose render cache is empty, which is one re-projection instead of a
    // second cache to keep honest.
    slot.drawing = std::make_unique<DrawingDocument>(*dwg);
    break;
  }
  }

  state.parts.push_back(std::move(slot));
  // Which documents the project HAS lives in project.json, not in any document
  // version — without this the project reads clean with a new tab in it.
  state.projectMetaDirty = true;
  const size_t added = state.parts.size() - 1;
  // Switch to it the way any other tab switch happens: solve, project, frame.
  SetActivePart(state, added);
  return added;
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

void PushJobUndo(AppState& state, const cam::JobDocument& before) {
  if (!state.job)
    return;
  state.jobHistory.undo.push_back(before);
  state.jobHistory.undoStamps.push_back(NextActionStamp());
  state.jobHistory.redo.clear();
  state.jobHistory.redoStamps.clear();
}

void SnapshotJob(AppState& state) {
  if (state.job)
    PushJobUndo(state, *state.job);
}

namespace {
// Put a restored copy in place: its version has to move PAST the current
// one, or a cache keyed on the version (the placed bodies, the render) can
// serve the state the copy was taken from. Then the toolpaths, which are a
// picture of the job that just changed.
void RestoreJob(AppState& state, cam::JobDocument restored) {
  const uint64_t current = state.job->Version();
  while (restored.Version() <= current)
    restored.BumpVersion();
  *state.job = std::move(restored);
  state.camActiveSetup = 0; // re-resolved: the setup may be gone
  state.camGhostOp = 0;
  state.camVerbOp = 0;
  state.syncedDocumentVersion = UINT64_MAX;
  state.projectMetaDirty = true;
  RefreshCamToolpaths(state);
  RunCamVerification(state);
}
} // namespace

bool UndoJob(AppState& state) {
  if (!state.job || state.jobHistory.undo.empty())
    return false;
  state.jobHistory.redo.push_back(*state.job);
  state.jobHistory.redoStamps.push_back(state.jobHistory.undoStamps.back());
  state.jobHistory.undoStamps.pop_back();
  cam::JobDocument restored = std::move(state.jobHistory.undo.back());
  state.jobHistory.undo.pop_back();
  RestoreJob(state, std::move(restored));
  return true;
}

bool RedoJob(AppState& state) {
  if (!state.job || state.jobHistory.redo.empty())
    return false;
  state.jobHistory.undo.push_back(*state.job);
  state.jobHistory.undoStamps.push_back(state.jobHistory.redoStamps.back());
  state.jobHistory.redoStamps.pop_back();
  cam::JobDocument restored = std::move(state.jobHistory.redo.back());
  state.jobHistory.redo.pop_back();
  RestoreJob(state, std::move(restored));
  return true;
}

JobUndoGuard::JobUndoGuard(AppState& state, bool coalesce)
    : m_state(state), m_coalesce(coalesce) {
  if (state.job) {
    m_before = *state.job;
    m_fingerprint = DocumentIO::JobFingerprint(*state.job);
  }
}

JobUndoGuard::~JobUndoGuard() {
  AppState& state = m_state;
  if (!state.job || !m_before)
    return;
  // Undo itself replaces the job under a guard (the panel is drawing while
  // Ctrl+Z lands): a history that just changed is not an edit to record.
  const std::string now = DocumentIO::JobFingerprint(*state.job);
  const bool moved = now != m_fingerprint;
  JobHistory& h = state.jobHistory;
  if (m_coalesce && ImGui::IsAnyItemActive()) {
    // Mid-drag: hold the FIRST pre-edit copy, whatever this frame did.
    if (moved && !h.pending) {
      h.pending = std::move(*m_before);
      h.pendingFingerprint = m_fingerprint;
    }
    return;
  }
  if (h.pending) {
    // The drag let go. One step, from before it started -- unless it came
    // back to exactly where it began.
    if (now != h.pendingFingerprint)
      PushJobUndo(state, *h.pending);
    h.pending.reset();
    h.pendingFingerprint.clear();
    return;
  }
  if (moved)
    PushJobUndo(state, *m_before);
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

  // First look at this tab: frame it. The camera is global, so arriving at a
  // 5 m assembly with a 20 mm part's framing puts you inside the geometry —
  // which reads as an empty scene, not as "you are too close". Only the first
  // visit; after that the framing is the user's.
  if (index < state.parts.size() && !state.parts[index].everFramed) {
    state.parts[index].everFramed = true;
    FitCameraToDocument(state);
  }
}

std::optional<size_t> SlotIndexById(const AppState& state, uint64_t slotId) {
  for (size_t i = 0; i < state.parts.size(); i++)
    if (state.parts[i].id == slotId)
      return i;
  return std::nullopt;
}

// ---- Simulation mode (FEM) ------------------------------------------------

namespace {
// Declared here, defined with its siblings further down: anonymous namespaces
// merge within a translation unit, so this is the same function.
} // namespace

std::string WhyPartUndeletable(AppState& state, size_t index) {
  EnsureParts(state);
  if (state.parts.size() <= 1)
    return "this is the last tab — a project always has one document";
  if (index >= state.parts.size())
    return "that tab is gone";
  // Refused while something references it — which `describe` has claimed
  // since A2 while nothing actually checked, so deleting a placed part left
  // every assembly holding a dangling reference. A5 widens the same question:
  // an assembly can now be referenced too.
  //
  // Split out of DeletePart so the MENU can ask before offering (F1 dogfood
  // round): the item used to be enabled, call DeletePart, and drop both the
  // false and the reason on the floor — an enabled control that silently does
  // nothing, which is the same defect that made File > Save look broken.
  const uint64_t doomedId = state.parts[index].id;
  std::string referencedBy;
  for (size_t i = 0; i < state.parts.size(); i++) {
    if (i == index || state.parts[i].kind != DocKind::Assembly)
      continue;
    const AssemblyDocument* asmDoc = SlotAssembly(state, i);
    if (asmDoc == nullptr)
      continue;
    for (const Occurrence& occ : asmDoc->GetOccurrences()) {
      if (occ.partId != doomedId)
        continue;
      referencedBy += (referencedBy.empty() ? "" : ", ") + state.parts[i].name;
      break;
    }
  }
  if (!referencedBy.empty())
    return "'" + state.parts[index].name + "' is placed in " + referencedBy +
           " — delete those occurrences first (right-click one in the "
           "assembly's Model panel)";
  return {};
}

bool DeletePart(AppState& state, size_t index, std::string* why) {
  if (const std::string blocked = WhyPartUndeletable(state, index);
      !blocked.empty()) {
    if (why)
      *why = blocked;
    return false;
  }
  const uint64_t doomedId = state.parts[index].id;
  const bool deletingActive = index == static_cast<size_t>(state.activePart);
  if (deletingActive) {
    // Activate a neighbor first so AppState always holds a live document.
    SetActivePart(state, index == 0 ? 1 : index - 1);
  }
  // The slot id is about to stop naming anything; release its cached
  // tessellation + GPU assets before it does, or they are held for the life
  // of the session with no way left to reach them.
  EvictPartRenderCache(state, doomedId);
  state.parts.erase(state.parts.begin() + static_cast<long>(index));
  // Fix up the active index after the erase.
  if (static_cast<size_t>(state.activePart) > index)
    state.activePart--;
  // As in AddPart: the deletion lives in project.json alone, and an unsaved
  // one would otherwise come back on the next open with nothing said.
  state.projectMetaDirty = true;
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

namespace {} // namespace

// Hoisted out of the anonymous namespace by the stage-3 split: the tab-slot
// accessors are the seam ProjectActions.cpp reaches the slots through, so
// they are declared in AppActionsInternal.h and defined here.

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

// A job slot's document, active or stashed. Null for every other kind.
cam::JobDocument* SlotJob(AppState& state, size_t i) {
  return static_cast<int>(i) == state.activePart ? state.job.get()
                                                 : state.parts[i].job.get();
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

namespace {
// Aim the camera at a world-space box: the shared tail of the document and
// selection fits.
void FitCameraToBox(AppState& state, const glm::vec3& min,
                    const glm::vec3& max) {
  const glm::vec3 center = (min + max) * 0.5f;

  // The vertical half-FOV is fixed; the horizontal one is tan(fovY/2)*aspect,
  // so in a portrait-ish viewport (aspect < 1 — e.g. the central region of a
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
  // CAM's verb window counts too. It was missing from this list, so fitting
  // the view with the verbs open parked the model under the very window that
  // was asking you to click it — the thing this whole branch exists to stop
  // (docs/cam/dogfood-round.md, round 2).
  const bool toolWindowOpen = HasPendingFeature(state) ||
                              state.editFeatureId != 0 ||
                              state.measure.active || state.dimTool.active ||
                              state.mateTool.active || state.camToolOpen;
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

  const float tanHalfH = std::max(tanHalfV * aspect * usableX, 0.05f);

  // Fit the box AS IT PROJECTS, not its bounding sphere. The sphere is drawn
  // on the box's DIAGONAL, so anything that is not a cube got framed by a
  // circle it never fills: the smart-speaker panel (1122 x 210 x 16) opened
  // at half the viewport height with the rest empty, and every real part
  // needed a scroll-wheel nudge before you could work on it.
  //
  // Aim and standoff are solved TOGETHER, because in perspective the
  // projected centroid of a box is not its 3D centre — the near corner
  // swings wide. Aiming at the centre and standing off far enough for the
  // worst corner leaves the part hugging one side with the fit "correct" and
  // half the screen empty. Each round re-centres the projection, then scales
  // the distance by how much of the frame it spans; four converge to well
  // under a pixel, and the projection is re-measured against the camera's
  // FINAL basis every time (SetTarget re-aims, so the basis moves under you).
  const float kMargin = 1.05f; // a hair of air around the silhouette
  const glm::vec3 corners[8] = {{min.x, min.y, min.z}, {max.x, min.y, min.z},
                                {min.x, max.y, min.z}, {max.x, max.y, min.z},
                                {min.x, min.y, max.z}, {max.x, min.y, max.z},
                                {min.x, max.y, max.z}, {max.x, max.y, max.z}};

  // The standoff at which no corner leaves the frame, for the aim the camera
  // currently holds. Exact, so it can never cut the model: a corner that
  // leans f toward the camera sees the frame at (D - f), which is what makes
  // the near end of a long part govern.
  const auto standoff = [&]() {
    const glm::vec3 fwd = cam.GetForward();
    const glm::vec3 right = cam.GetRight();
    const glm::vec3 up = cam.GetUp();
    const glm::vec3 aim = cam.GetTarget();
    float d = 1.0f;
    for (const glm::vec3& corner : corners) {
      const glm::vec3 v = corner - aim;
      const float toward = -glm::dot(v, fwd);
      d = std::max({d,
                    kMargin * std::abs(glm::dot(v, right)) / tanHalfH + toward,
                    kMargin * std::abs(glm::dot(v, up)) / tanHalfV + toward});
    }
    return d;
  };
  // Where the box lands on screen, in NDC, from that standoff.
  const auto projectedCentre = [&](float d) {
    const glm::vec3 fwd = cam.GetForward();
    const glm::vec3 right = cam.GetRight();
    const glm::vec3 up = cam.GetUp();
    const glm::vec3 eye = cam.GetTarget() - fwd * d;
    float xMin = FLT_MAX, xMax = -FLT_MAX, yMin = FLT_MAX, yMax = -FLT_MAX;
    for (const glm::vec3& corner : corners) {
      const glm::vec3 v = corner - eye;
      const float depth = std::max(glm::dot(v, fwd), 1e-3f);
      const float x = glm::dot(v, right) / (depth * tanHalfH);
      const float y = glm::dot(v, up) / (depth * tanHalfV);
      xMin = std::min(xMin, x);
      xMax = std::max(xMax, x);
      yMin = std::min(yMin, y);
      yMax = std::max(yMax, y);
    }
    return glm::vec2((xMin + xMax) * 0.5f, (yMin + yMax) * 0.5f);
  };

  // Aim and standoff are solved TOGETHER, because in perspective the
  // projected centroid of a box is not its 3D centre — the near corner swings
  // wide. Aiming at the centre and standing off far enough for the worst
  // corner left the part hugging one side with the fit "correct" and half the
  // screen empty (57% of the viewport for a speaker panel; 85% once centred).
  // Each round slides the aim to where the box actually lands and takes the
  // exact standoff for it, so the frame only ever tightens and no round of
  // this can crop the model. It is a PAN, not a re-aim: solving it with
  // SetTarget walked the speaker panel through 60 degrees of orbit while
  // "fitting" it.
  glm::vec3 target = center;
  float distance = 1.0f;
  for (int iter = 0; iter < 3; iter++) {
    cam.SetPivot(target); // slides the view; SetTarget would rotate it
    distance = standoff();
    const glm::vec2 off = projectedCentre(distance);
    target += cam.GetRight() * (off.x * tanHalfH * distance) +
              cam.GetUp() * (off.y * tanHalfV * distance);
  }
  cam.SetPivot(target);
  distance = standoff();
  cam.SetTarget(target);
  cam.SetDistance(distance);
  if (coveredFrac > 0.01f) {
    // Half the covered strip, in world units at the target's depth. The
    // target moves RIGHT so the model appears LEFT of it — away from the
    // window on the right. It moved the other way, which pushed the model
    // under the very window this branch exists to clear (found with the CAM
    // verbs open, docs/cam/dogfood-round.md).
    const glm::vec3 right = cam.GetRight();
    const float halfWidthWorld = distance * tanHalfV * aspect;
    // Off the SOLVED aim, not the box centre — the fit above re-centred the
    // projection, and re-deriving from the centre would throw that away.
    cam.SetPivot(target + right * (halfWidthWorld * coveredFrac));
  }
}
} // namespace

bool VisibleSceneBounds(AppState& state, glm::vec3& min, glm::vec3& max) {
  min = glm::vec3(FLT_MAX);
  max = glm::vec3(-FLT_MAX);
  bool any = false;
  if (state.assembly) {
    // Every visible leaf part of the tree, placed (A5). Bounding only the
    // top level would put a new section's default cut, and the camera, in
    // the wrong place for a nested model.
    for (const assembly::FlatInstance& inst : FlattenActiveAssembly(state)) {
      if (!inst.visible)
        continue;
      const Document* part = inst.part;
      // The top-level occurrence's explode offset moves the whole subtree,
      // so exploded extents are bounded too.
      const glm::mat4 xf =
          glm::translate(glm::mat4(1.0f),
                         ExplodeOffset(state, inst.rootOccurrenceId)) *
          inst.world;
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
  } else if (state.job) {
    // A job tab draws the parts LAID OUT, so the camera has to frame the
    // layout rather than one part -- otherwise the first look at a sheet is
    // from inside whichever part happens to sit at the origin.
    for (const Body& body : JobPlacedBodies(state)) {
      glm::vec3 bmin, bmax;
      body.shape.GetBoundingBox(bmin, bmax);
      min = glm::min(min, bmin);
      max = glm::max(max, bmax);
      any = true;
    }
  }
  return any;
}

namespace {
// 0 = XY (cut along Z), 1 = XZ (along Y), 2 = YZ (along X).
glm::vec3 SectionNormal(int basePlane) {
  switch (basePlane) {
  case 1:
    return {0, 1, 0};
  case 2:
    return {1, 0, 0};
  default:
    return {0, 0, 1};
  }
}
} // namespace

float CenteredSectionOffset(AppState& state, int basePlane) {
  glm::vec3 min, max;
  if (!VisibleSceneBounds(state, min, max))
    return 0.0f;
  return glm::dot((min + max) * 0.5f, SectionNormal(basePlane));
}

namespace {

// The source plane a section cuts along, BEFORE its offset and tilt: a base
// plane through the origin, a datum plane, or a picked planar face — the
// latter two resolved live against the document, so moving the datum or
// editing the face moves the cut with it (P2).
struct SectionFrame {
  glm::vec3 origin{0.0f};
  glm::vec3 normal{0.0f, 0.0f, 1.0f};
  glm::vec3 u{1.0f, 0.0f, 0.0f}; // in-plane axes, for the tilt
  glm::vec3 v{0.0f, 1.0f, 0.0f};
};

// A right-handed in-plane basis for a normal. Any choice is arbitrary; this
// one is stable (no flipping as the normal rotates through an axis), which
// matters because the tilt angles are expressed in it.
void PlaneBasis(const glm::vec3& n, glm::vec3& u, glm::vec3& v) {
  const glm::vec3 pick =
      std::abs(n.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
  u = glm::normalize(glm::cross(pick, n));
  v = glm::cross(n, u);
}

SectionFrame SectionSourceFrame(const AppState& state,
                                const AppState::SectionView& s,
                                bool* resolved = nullptr) {
  SectionFrame f;
  if (resolved != nullptr)
    *resolved = true;
  f.normal = SectionNormal(s.basePlane);

  Document* doc = state.document.get();
  if (doc != nullptr && s.face.IsSet()) {
    // A picked planar face. Not found (an edit removed it, or the tab is an
    // assembly with no part document) falls back to the base plane rather
    // than dropping the cut — a section that silently stops cutting reads as
    // a rendering bug.
    for (const Body& body : doc->GetBodies()) {
      if (body.createdBy != s.face.featureId)
        continue;
      const std::optional<int> idx = body.elementMap.Resolve(s.face.name);
      glm::vec3 origin(0.0f), normal(0.0f), uAxis(0.0f);
      if (idx && body.shape.FacePlane(*idx, origin, normal, uAxis)) {
        f.origin = origin;
        f.normal = glm::normalize(normal);
        f.u = glm::normalize(uAxis);
        f.v = glm::cross(f.normal, f.u);
        return f;
      }
    }
  } else if (doc != nullptr && s.planeId != 0) {
    for (size_t i = 0; i < doc->FeatureCount(); i++) {
      const auto* dp = dynamic_cast<DatumPlaneFeature*>(doc->GetFeature(i));
      if (dp == nullptr || dp->GetId() != s.planeId)
        continue;
      const SketchPlane& p = dp->OutputPlane();
      f.origin = p.GetOrigin();
      f.normal = glm::normalize(p.GetNormal());
      f.u = glm::normalize(p.GetUAxis());
      f.v = glm::cross(f.normal, f.u);
      return f;
    }
  }

  // A named source that did not resolve: the datum was deleted or replaced,
  // the face is gone, or this is an assembly tab, where a part's datum plane
  // has no meaning. The caller stops the cut rather than quietly cutting
  // somewhere else — see SectionSourceResolved.
  if (resolved != nullptr && (s.planeId != 0 || s.face.IsSet()))
    *resolved = false;
  PlaneBasis(f.normal, f.u, f.v);
  return f;
}

} // namespace

glm::vec4 SectionPlaneOf(const AppState& state,
                         const AppState::SectionView& s) {
  const SectionFrame f = SectionSourceFrame(state, s);

  // Offset first, along the SOURCE normal, then tilt about that offset point
  // — so dragging the arrow slides the cut and the angles pivot it, instead
  // of the two interacting.
  const glm::vec3 anchor = f.origin + f.normal * s.offset;
  glm::vec3 n = f.normal;
  if (s.tiltU != 0.0f)
    n = glm::angleAxis(glm::radians(s.tiltU), f.u) * n;
  if (s.tiltV != 0.0f)
    n = glm::angleAxis(glm::radians(s.tiltV), f.v) * n;
  n = glm::normalize(n);

  const float d = glm::dot(anchor, n);
  // Reverse keeps the other side: the same surface, the opposite half-space,
  // which is exactly {-n, -d}.
  return s.reverse ? glm::vec4(-n, -d) : glm::vec4(n, d);
}

void ArmSectionFacePick(AppState& state, uint64_t sectionId) {
  state.sectionActiveId = sectionId;
  state.slotPick = {kSectionToolSlot, static_cast<int>(PickSlot::SectionFace),
                    SelectionKindBit(SelectionItem::Kind::Face),
                    /*multi=*/false, "Pick a planar face to cut along"};
}

void SectionFacePicked(AppState& state, const SelectionItem& item) {
  AppState::SectionView* sec = FindSection(state, state.sectionActiveId);
  if (sec == nullptr || !item.element.IsSet()) {
    state.slotPick.miss = "that is not a face this cut can use";
    return;
  }
  // Refuse a curved face rather than cutting along some arbitrary tangent
  // plane: FacePlane only answers for planar faces, and a section along a
  // cylinder is not a thing the user can have meant.
  bool planar = false;
  if (state.document != nullptr) {
    for (const Body& body : state.document->GetBodies()) {
      if (body.createdBy != item.element.featureId)
        continue;
      const std::optional<int> idx = body.elementMap.Resolve(item.element.name);
      glm::vec3 o(0.0f), n(0.0f), u(0.0f);
      planar = idx && body.shape.FacePlane(*idx, o, n, u);
      break;
    }
  }
  if (!planar) {
    state.slotPick.miss = "that face is not planar — pick a flat one";
    return;
  }
  sec->face = item.element;
  sec->planeId = 0;
  // The offset is measured from the new source, so an offset that meant
  // "40 mm above the origin" must not survive as "40 mm off this face".
  sec->offset = 0.0f;
  state.slotPick = {};
}

bool SectionSourceResolved(const AppState& state,
                           const AppState::SectionView& s) {
  bool resolved = true;
  SectionSourceFrame(state, s, &resolved);
  return resolved;
}

std::optional<glm::vec4> SketchSlicePlane(const AppState& state) {
  if (!state.sketchSlice || state.sketchEdit.activeSketchId == 0 ||
      state.document == nullptr || state.camera == nullptr)
    return std::nullopt;
  const SketchFeature* sk = nullptr;
  for (const auto& f : state.document->GetFeatures())
    if (f->GetId() == state.sketchEdit.activeSketchId) {
      sk = dynamic_cast<const SketchFeature*>(f.get());
      break;
    }
  if (sk == nullptr)
    return std::nullopt;

  // Remove the half TOWARD the camera. The clip convention here is not the
  // GL one: mesh.vert writes a plain varying and the fragment stage discards
  // where dot(n, p) - w is POSITIVE (which is also why a disabled slot is
  // -1e9), so the normal points at the half that goes away — it must oppose
  // the view direction, not follow it.
  //
  // Camera-relative rather than fixed to the sketch normal because orbiting
  // to the far side of the plane would otherwise cut away everything you can
  // see instead of revealing it; Inventor's Slice Graphics and Fusion's Slice
  // both follow the view for the same reason. Edge-on (facing ~ 0) there is
  // no near half to speak of and either answer is as good.
  const glm::vec3 n0 = glm::normalize(sk->plane.GetNormal());
  const float facing = glm::dot(n0, state.camera->GetForward());
  const glm::vec3 n = facing > 0.0f ? -n0 : n0;
  return glm::vec4(n, glm::dot(sk->plane.GetOrigin(), n));
}

ClipPlaneArray SectionPlanes(const AppState& state) {
  ClipPlaneArray planes = NoClipPlanes();
  size_t slot = 0;
  for (const AppState::SectionView& s : state.sections) {
    if (!s.visible || slot >= planes.size())
      continue;
    // A cut whose source vanished STOPS, and the tool row says so. Cutting
    // along the base plane instead would move the cut without telling
    // anyone, which is the worse of the two failures.
    if (!SectionSourceResolved(state, s))
      continue;
    planes[slot++] = SectionPlaneOf(state, s);
  }
  // The sketch slice rides in the last free slot rather than in the section
  // list, so it cannot be edited or deleted as if it were a cut the user set
  // up. Passing through here is what gives it filled cut faces for free —
  // AppendSectionCaps reads this array, not state.sections. With all four
  // slots spoken for it does not silently not slice; CanSketchSlice() is
  // false and the toolbar says why.
  if (slot < planes.size())
    if (const auto slice = SketchSlicePlane(state))
      planes[slot++] = *slice;
  return planes;
}

bool CanSketchSlice(const AppState& state) {
  size_t used = 0;
  for (const AppState::SectionView& s : state.sections)
    if (s.visible && SectionSourceResolved(state, s))
      used++;
  return used < static_cast<size_t>(kMaxClipPlanes);
}

bool SectionActive(const AppState& state) {
  for (const AppState::SectionView& s : state.sections)
    if (s.visible)
      return true;
  // The sketch slice is a cut like any other to everything downstream of
  // here (cap fills, the "sectioned" state the exporters refuse on).
  return SketchSlicePlane(state).has_value();
}

AppState::SectionView* FindSection(AppState& state, uint64_t id) {
  for (AppState::SectionView& s : state.sections)
    if (s.id == id)
      return &s;
  return nullptr;
}

uint64_t AddSection(AppState& state, int basePlane) {
  if (state.sections.size() >= static_cast<size_t>(kMaxClipPlanes))
    return 0; // the renderer carries four; see render/ClipPlanes.h
  AppState::SectionView s;
  s.id = state.nextSectionId++;
  s.basePlane = basePlane;
  // Through the middle of what the tab shows, so a new section always cuts
  // something. Zero would be the world origin, which for a model built
  // anywhere else removes everything or nothing.
  s.offset = CenteredSectionOffset(state, basePlane);
  state.sections.push_back(s);
  state.sectionActiveId = s.id;
  return s.id;
}

void RemoveSection(AppState& state, uint64_t id) {
  std::erase_if(state.sections,
                [&](const AppState::SectionView& s) { return s.id == id; });
  if (state.sectionActiveId == id)
    state.sectionActiveId = state.sections.empty() ? 0 : state.sections[0].id;
}

void ClearSections(AppState& state) {
  state.sections.clear();
  state.sectionActiveId = 0;
}

void FitCameraToDocument(AppState& state) {
  if (!state.camera)
    return;
  glm::vec3 min, max;
  if (!VisibleSceneBounds(state, min, max))
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
} // namespace

void AppendCapMeshes(AppState& state, const std::vector<CADShape>& caps,
                     const std::vector<glm::vec3>& colors, const glm::mat4& xf,
                     const glm::vec3& worldNormal) {
  const glm::vec3 n = worldNormal;
  const float eps = 1e-3f; // nudge to the kept side; invisible at mm scale
  for (size_t bi = 0; bi < caps.size(); bi++) {
    if (!caps[bi].IsValid())
      continue;
    CADShape::Mesh cm = caps[bi].GenerateMesh(0.05);
    if (cm.vertices.empty())
      continue;
    std::vector<Vertex3D> base = CADMeshConverter::ConvertCADMeshToVertices(cm);
    // The caps are computed in the body's own space; an occurrence places
    // them. Baked into the vertices rather than carried as an instance
    // transform because the nudge and the winding below are decided in WORLD
    // space, against the plane.
    for (Vertex3D& v : base)
      v.position = glm::vec3(xf * glm::vec4(v.position, 1.0f));
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
        capId, bi < colors.size() ? colors[bi] : glm::vec3(0.7f));
    state.sectionCapMeshes.push_back(capId);
  }
}

namespace {} // namespace

// Hoisted out of the anonymous namespace by the stage-3 split:
// SyncDocumentRenderers draws these caps and now lives in
// RendererSync.cpp, so the declaration is in AppActionsInternal.h.

void AppendSectionCaps(AppState& state, const std::vector<Body>& bodies,
                       const std::vector<glm::vec3>& bodyColors) {
  const ClipPlaneArray planes = SectionPlanes(state);
  for (size_t i = 0; i < planes.size(); i++) {
    const glm::vec3 nRaw(planes[i]);
    if (glm::length(nRaw) < 1e-6f)
      continue;
    const glm::vec3 n = glm::normalize(nRaw);
    const glm::vec3 planeOrigin = n * planes[i].w; // dot(origin,n) = w
    // Every OTHER section trims this cap; without that, two cuts leave each
    // other's fill hanging in space past the corner.
    std::vector<glm::vec4> trim;
    for (size_t j = 0; j < planes.size(); j++)
      if (j != i && glm::length(glm::vec3(planes[j])) > 1e-6f)
        trim.push_back(planes[j]);
    AppendCapMeshes(state,
                    Measure::SectionCaps(bodies, planeOrigin, n, trim,
                                         &state.sectionCapCache),
                    bodyColors, glm::mat4(1.0f), n);
  }
}

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
    // A Derived ghost is not the part: a drawing of the gripper body is a
    // drawing of the body, not of the servo it was drawn on (and the HLR of
    // a 197-body camera ghost took the view past a minute).
    if (body.reference)
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
      // A5: project the leaf parts of the whole tree. Every projected body is
      // stamped with its TOP-LEVEL occurrence id, not the leaf's — a leaf
      // inside a sub-assembly has an id belonging to the SUB's document,
      // which this drawing's dimension refs could never resolve. So a
      // sub-assembly dimensions as one unit, and no ref can dangle; to
      // dimension an inner part, draw that assembly.
      auto flat = assembly::Flatten(*asmDoc, PartResolverFor(state),
                                    AssemblyResolverFor(state));
      if (!flat) {
        *error = flat.error();
        return false;
      }
      for (const assembly::FlatInstance& inst : *flat) {
        if (!inst.visible)
          continue;
        // Left off this SHEET, which is not the same as hidden in the model:
        // `hidden` is per view, so one sheet can drop the springs and another
        // keep them (F4).
        if (std::find(view.hidden.begin(), view.hidden.end(),
                      inst.rootOccurrenceId) != view.hidden.end())
          continue;
        AppendBodies(*inst.part, glm::dmat4(inst.world), inst.rootOccurrenceId,
                     out);
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

drawing::SheetView AssembleSheetView(AppState& state, const DrawingView& view,
                                     const std::vector<SourceBody>& bodies,
                                     const drawing::ProjectionResult& proj);

drawing::SheetView ProjectDrawingView(AppState& state, const DrawingView& view,
                                      std::function<bool()> cancelled) {
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
  opts.mode = view.quality == "fast" ? drawing::ProjectionMode::Poly
                                     : drawing::ProjectionMode::Exact;
  opts.cancelled = cancelled;
  auto projected = drawing::ProjectView(inputs, viewDir, up, opts, &error);
  if (!projected) {
    result.error = error;
    return result;
  }

  return AssembleSheetView(state, view, bodies, *projected);
}

// The cheap half of a view: place the projected curves on the sheet at the
// view's scale and folded position, and join the projection's index-space
// SourceRefs to stable ElementRefs.
//
// Split from the HLR so the expensive half can run OFF the main thread: this
// part needs the live documents (their ElementMaps) and the sheet layout, and
// the part it is split from needs neither (docs/agent-scale-followup-plan.md,
// W3).
drawing::SheetView
AssembleSheetView(AppState& state, const DrawingView& view,
                  const std::vector<SourceBody>& bodies,
                  const drawing::ProjectionResult& projection) {
  drawing::SheetView result;
  result.viewId = view.id;
  result.solvedSourceVersion = SourceVersionOf(state, view);
  result.updateMs = projection.stats.updateMs;
  result.hideMs = projection.stats.hideMs;
  result.dsEdges = projection.stats.dsEdges;
  result.bodies = static_cast<int>(bodies.size());
  const drawing::ProjectionResult* projected = &projection;

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
      // A fastener callout names the part an OCCURRENCE places, which is a
      // different document from the assembly the view is of. Resolved through
      // the VIEW's source assembly rather than through state.assembly: a
      // drawing is its own tab, so the assembly it draws is never the active
      // one while the drawing is on screen.
      const uint64_t sourceId = view->sourceDocId;
      ctx.occurrenceDoc = [&state,
                           sourceId](uint64_t occId) -> const Document* {
        const AssemblyDocument* asmDoc = AssemblyDocById(state, sourceId);
        if (asmDoc == nullptr)
          return nullptr;
        const Occurrence* occ = asmDoc->FindById(occId);
        return occ ? PartDocById(state, occ->partId) : nullptr;
      };
    }
  }
  return drawing::ResolveDim(dim, it->second, ctx);
}

glm::dvec2 AutoDimOffset(AppState& state, const DrawingDim& dim) {
  const auto it = state.drawingRenders.find(dim.viewId);
  if (it == state.drawingRenders.end() || !state.drawing)
    return {0.0, -12.0};
  drawing::DimPlaceContext ctx;
  const SheetChrome chrome = ChromeOf(state.drawing->GetSheet());
  ctx.borderMin = chrome.borderMin;
  ctx.borderMax = chrome.borderMax;
  ctx.avoid.push_back({chrome.titleBlockMin, chrome.titleBlockMax});
  for (const auto& [vid, sv] : state.drawingRenders)
    if (sv.hasGeometry)
      ctx.avoid.push_back({sv.bboxMin, sv.bboxMax});
  for (const DrawingDim& dm : state.drawing->GetDims()) {
    const drawing::ResolvedDim rd = ResolveDrawingDim(state, dm);
    if (!rd.resolved)
      continue;
    glm::dvec2 mn(0.0), mx(0.0);
    drawing::LabelBox(rd.labelPos, rd.text, mn, mx);
    ctx.avoid.push_back({mn, mx});
  }
  return drawing::PlaceDimOffset(dim, it->second, ctx);
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
  std::vector<drawing::ResolvedNote> notes;
  for (const DrawingNote& n : state.drawing->GetNotes()) {
    const auto nit = state.drawingRenders.find(n.viewId);
    notes.push_back(drawing::ResolveNote(
        n, nit != state.drawingRenders.end() ? &nit->second : nullptr));
  }
  std::string ext = std::filesystem::path(path).extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
  if (ext == ".svg")
    return DrawingExport::SaveSVG(*state.drawing, views, dims, path, error,
                                  notes);
  if (ext == ".pdf")
    return DrawingExport::SavePDF(*state.drawing, views, dims, path, error,
                                  notes);
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

namespace {

// One view's footprint at 1:1, anchored at the origin (the sheet anchor maps
// model -> position + scale * model).
struct ViewFootprint {
  glm::dvec2 modelMin, modelMax;
};

// Lays every view out from the sheet's top-left at one scale: a row to the
// right, wrapping below when the row is full. Returns false when a wrapped
// row would land on the title block or under the border — the sheet is full
// at this scale. `positions` receives one anchor per footprint.
bool FlowLayout(const SheetChrome& chrome, double scale,
                const std::vector<ViewFootprint>& items,
                std::vector<glm::dvec2>& positions) {
  positions.clear();
  double gapMax = 8.0;
  for (const ViewFootprint& f : items) {
    const glm::dvec2 size = (f.modelMax - f.modelMin) * scale;
    gapMax = std::max(gapMax, std::max(size.x, size.y) * 0.15);
  }
  const double gap = gapMax;
  double cursorX = chrome.borderMin.x + gap; // next left edge
  double rowTop = chrome.borderMax.y - gap;  // this row's top edge
  double rowBottom = rowTop;                 // lowest edge in this row
  for (const ViewFootprint& f : items) {
    const glm::dvec2 size = (f.modelMax - f.modelMin) * scale;
    if (cursorX + size.x > chrome.borderMax.x &&
        cursorX > chrome.borderMin.x + gap + 1e-9) {
      cursorX = chrome.borderMin.x + gap; // wrap
      rowTop = rowBottom - gap;
    }
    const double left = cursorX, top = rowTop;
    const double bottom = top - size.y;
    const Span rowX{left, left + size.x};
    const Span blockX{chrome.titleBlockMin.x, chrome.titleBlockMax.x};
    const double floorY =
        Overlaps(rowX, blockX) ? chrome.titleBlockMax.y : chrome.borderMin.y;
    if (bottom < floorY + gap || left + size.x > chrome.borderMax.x)
      return false; // does not fit at this scale
    positions.push_back(
        glm::dvec2(left - scale * f.modelMin.x, top - scale * f.modelMax.y));
    cursorX = left + size.x + gap;
    rowBottom = std::min(rowBottom, bottom);
  }
  return true;
}

bool BoxesOverlap(glm::dvec2 aMin, glm::dvec2 aMax, glm::dvec2 bMin,
                  glm::dvec2 bMax) {
  return Overlaps({aMin.x, aMax.x}, {bMin.x, bMax.x}) &&
         Overlaps({aMin.y, aMax.y}, {bMin.y, bMax.y});
}

} // namespace

uint64_t AddDrawingViewPlaced(AppState& state, DrawingView v) {
  if (!state.drawing)
    return 0;
  DrawingDocument& dwg = *state.drawing;
  const glm::dvec2 ext = ChromeOf(dwg.GetSheet()).extent;

  // Project a 1:1 copy anchored at the origin: its bbox IS the part's size in
  // millimetres, which is what both decisions below need.
  DrawingView probe = v;
  probe.id = 0;
  probe.scale = 1.0;
  probe.position = glm::dvec2(0.0);
  probe.baseViewId = 0; // a folded child would inherit its parent's place
  const drawing::SheetView sv = ProjectDrawingView(state, probe, nullptr);
  const bool measured = sv.hasGeometry;
  const glm::dvec2 size = measured ? sv.bboxMax - sv.bboxMin : glm::dvec2(0.0);

  // Scale (only when the caller did not choose one). Views of the SAME part
  // share a scale: a right view at 2:1 beside a front view at 1:1 is a
  // drafting error, and it is what the fit-each-view-alone rule below
  // produced for a plate (the right view is the smallest face, so it got
  // blown up until it collided with the top view). Matching first, fitting
  // only when this is the part's first view.
  if (v.scale <= 0.0) {
    for (const DrawingView& other : dwg.GetViews()) {
      if (other.sourceDocId == v.sourceDocId && other.scale > 0.0) {
        v.scale = other.scale;
        break;
      }
    }
  }
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

  // The row-and-wrap rule above clamps a wrapped row up to the border, and
  // when the views already there reach that low the new one lands ON them —
  // four auto-placed views of a 90 mm gripper body at 2:1 came out as one
  // pile (the D4 dogfood). A placement that overlaps a view or the title
  // block is not a placement: re-lay the whole sheet out from the top-left,
  // at this scale first and then each standard scale down, until it fits.
  // Every view of the sheet is moved (and, if needed, rescaled together —
  // one scale per sheet is the drafting rule the block above already keeps).
  if (measured) {
    const SheetChrome chrome = ChromeOf(dwg.GetSheet());
    const glm::dvec2 newMin = pos + v.scale * sv.bboxMin;
    const glm::dvec2 newMax = pos + v.scale * sv.bboxMax;
    bool collides =
        BoxesOverlap(newMin, newMax, chrome.titleBlockMin,
                     chrome.titleBlockMax) ||
        newMin.x < chrome.borderMin.x || newMin.y < chrome.borderMin.y ||
        newMax.x > chrome.borderMax.x || newMax.y > chrome.borderMax.y;
    for (const DrawingView& other : dwg.GetViews()) {
      const auto it = state.drawingRenders.find(other.id);
      if (it != state.drawingRenders.end() && it->second.hasGeometry &&
          BoxesOverlap(newMin, newMax, it->second.bboxMin, it->second.bboxMax))
        collides = true;
    }
    if (collides) {
      std::vector<DrawingView*> laid;
      std::vector<ViewFootprint> items;
      for (DrawingView& other : dwg.GetViews()) {
        const auto it = state.drawingRenders.find(other.id);
        if (it == state.drawingRenders.end() || !it->second.hasGeometry ||
            other.scale <= 0.0)
          continue;
        laid.push_back(&other);
        items.push_back({(it->second.bboxMin - other.position) / other.scale,
                         (it->second.bboxMax - other.position) / other.scale});
      }
      items.push_back({sv.bboxMin, sv.bboxMax});
      static const double kScales[] = {100.0, 50.0, 20.0, 10.0, 5.0,  2.0, 1.0,
                                       0.5,   0.2,  0.1,  0.05, 0.02, 0.01};
      std::vector<glm::dvec2> positions;
      for (double scale : kScales) {
        if (scale > v.scale + 1e-12)
          continue;
        if (!FlowLayout(chrome, scale, items, positions))
          continue;
        for (size_t i = 0; i < laid.size(); i++) {
          laid[i]->scale = scale;
          laid[i]->position = positions[i];
        }
        v.scale = scale;
        v.position = positions.back();
        break;
      }
    }
  }

  SnapshotDrawing(state);
  const uint64_t id = dwg.AddView(v);
  UpdateDrawingRenders(state);
  return id;
}

void UpdateDrawingRenders(AppState& state, double budgetSeconds) {
  if (!state.drawing)
    return;
  const auto started = std::chrono::steady_clock::now();
  const auto outOfBudget = [&] {
    if (budgetSeconds <= 0.0)
      return false;
    return std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                         started)
               .count() > budgetSeconds;
  };
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
    // Before starting, as well as during. A single body's Hide() is ONE
    // uninterruptible OCC call — the pairwise drive only has somewhere to
    // stop when there are several shapes to classify against each other — so
    // without this a sheet whose budget went on view one would still pay for
    // views two and three in full.
    drawing::SheetView sv;
    if (outOfBudget()) {
      sv.viewId = view.id;
      sv.solvedSourceVersion = SourceVersionOf(state, view);
      sv.error = "cancelled";
    } else {
      sv = ProjectDrawingView(state, view, outOfBudget);
    }
    if (sv.error == "cancelled") {
      // Out of budget rather than broken. Say which, and say the way through:
      // a `fast` view of the same geometry is a couple of hundred
      // milliseconds against a minute and a half.
      sv.error = "ran out of the update's time budget (" +
                 std::to_string(static_cast<int>(budgetSeconds)) +
                 " s) — exact HLR on this source is slower than that. Raise "
                 "the budget, or set this view's quality to \"fast\" "
                 "(polygonal HLR: orders of magnitude cheaper, and a look "
                 "rather than a dimensionable deliverable).";
    }
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
