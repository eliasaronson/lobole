// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/CreateActions.h"
#include "ui/RendererSync.h"
#include "ui/SketchToolbar.h" // ClosedProfileSketch (a base flange takes one)

#include "core/Units.h"
#include "ui/AppActionsInternal.h" // SharedBody / PartRenderCacheStore
#include "ui/CamPanel.h"           // RefreshCamToolpaths (job undo)
#include "ui/CamTool.h"            // RunCamVerification (job undo)
#include "ui/JobActions.h"

#include "imgui.h" // IsAnyItemActive (the undo guard coalesces a drag)         // JobBodies / JobRenderVersion (a job tab)
#include <algorithm>
#include <chrono>
#include <cmath>
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
#include "core/features/BaseFlangeFeature.h"
#include "core/features/BooleanFeature.h"
#include "core/features/CornerReliefFeature.h"
#include "core/features/CurveFeature.h"
#include "core/features/DatumAxisFeature.h"
#include "core/features/DatumPlaneFeature.h"
#include "core/features/EdgeFlangeFeature.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/FilletFeature.h"
#include "core/features/FlatPatternFeature.h"
#include "core/features/FrameFeature.h"
#include "core/features/HemFeature.h"
#include "core/features/JogFeature.h"
#include "core/features/NormalCutFeature.h"
#include "core/features/RefoldFeature.h"
#include "core/features/UnfoldFeature.h"
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

#include "core/DirectEdit.h"
#include "core/features/DeleteFaceFeature.h"
#include "core/features/DraftFeature.h"
#include "core/features/LoftFeature.h"
#include "core/features/MoveFaceFeature.h"
#include "core/features/OffsetFaceFeature.h"
#include "core/features/PatternFeature.h"
#include "core/features/PipeFeature.h"
#include "core/features/RefineFeature.h"
#include "core/features/ReplaceFaceFeature.h"
#include "core/features/RevolveFeature.h"
#include "core/features/RibFeature.h"
#include "core/features/ShellFeature.h"
#include "core/features/SketchFeature.h"
#include "core/features/SplitFeature.h"
#include "core/features/SweepFeature.h"
#include "core/features/ThreadFeature.h"
#include "core/features/WormFeature.h"
#include "core/sketch/SketchProfile.h"
#include "ui/AssemblyActions.h"
#include "ui/CamPlayback.h"   // SyncCamPlayback (SyncRenderersToDocument)
#include "ui/FastenerTool.h"  // SyncFastenerGhost (SyncRenderersToDocument)
#include "ui/FeatureEditor.h" // PickSlot (thread face slot)
#include "ui/Placement.h"
#include "ui/PreviewFeature.h"
#include "ui/PrimitiveTool.h"
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

// The creation tools: what a Box, a Loft, a Pattern or a Hole IS when the
// toolbar asks for one with no arguments.
//
// Every default here is derived from what it acts on -- the document's
// reach, the newest body's bbox, the last closed sketch -- never a literal.
// The defaults/* GUI suite enforces exactly that, so this file is what that
// suite is about (docs/ui/tool-sessions.md#defaults-and-refusals).
// Declared in ui/CreateActions.h; the layer is described in
// docs/architecture.md#the-ui-actions-layer.

namespace ui {

namespace {
std::string NextName(const AppState& state, const char* base) {
  const int n =
      state.document ? static_cast<int>(state.document->FeatureCount()) + 1 : 1;
  return std::string(base) + " " + std::to_string(n);
}
} // namespace

// The primitives stand on something: each arms a face/plane pick, and the
// pick opens the preview (ui/PrimitiveTool.h). They no longer make the stored
// Box/Sphere/Cylinder features, which kept a world position nothing followed.
void CreateBox(AppState& state) {
  StartPrimitiveTool(state, primitive::Kind::Box);
}

void CreateSphere(AppState& state) {
  StartPrimitiveTool(state, primitive::Kind::Sphere);
}

void CreateCylinder(AppState& state) {
  StartPrimitiveTool(state, primitive::Kind::Cylinder);
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
    const glm::dvec3 n = parentPlane->GetNormal();
    const glm::dvec3 o = parentPlane->GetOrigin();
    double lo = std::numeric_limits<double>::max();
    double hi = std::numeric_limits<double>::lowest();
    for (const Body& b :
         state.document ? state.document->GetBodies() : std::vector<Body>{}) {
      glm::dvec3 bmin, bmax;
      b.shape.GetBoundingBox(bmin, bmax);
      for (int c = 0; c < 8; c++) {
        const glm::dvec3 corner((c & 1) ? bmax.x : bmin.x,
                                (c & 2) ? bmax.y : bmin.y,
                                (c & 4) ? bmax.z : bmin.z);
        const double d = glm::dot(corner - o, n);
        lo = std::min(lo, d);
        hi = std::max(hi, d);
      }
    }
    if (hi > lo)
      dp->offset = hi + 0.25 * (hi - lo);
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
  // A base flange is a new body like a primitive is, so every body-op tool
  // that defaults to "the most recent producer" can reach it. Leaving it out
  // is not a missing feature but a silently wrong DEFAULT: the flat pattern
  // opened pointing at a body that did not exist.
  if (dynamic_cast<const BaseFlangeFeature*>(&f))
    return true;
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
    glm::dvec3 bmin, bmax;
    b.shape.GetBoundingBox(bmin, bmax);
    pat->spacing = std::max(3.0, (bmax.x - bmin.x) * 1.5);
    break;
  }
  BeginPreviewFeature(state, std::move(pat));
}

namespace {
// The body a body-op tool should default to: the most recent producer, as
// Pattern/Boolean do. 0 when the document has none.
uint64_t NewestBody(const AppState& state) {
  auto producers =
      BodyProducers(*state.document, state.document->FeatureCount());
  return producers.empty() ? 0 : producers.back().first;
}

// The body a blend or shell will act on before anything is picked: the one a
// pre-selected element of `kind` belongs to (the seed names it), else the
// newest.
uint64_t TargetBody(const AppState& state, SelectionItem::Kind kind) {
  for (const SelectionItem& item : state.selection.items)
    if (item.kind == kind && item.element.featureId != 0)
      return item.element.featureId;
  return NewestBody(state);
}

// A tenth of the body's thinnest extent, rounded down to 1, 2 or 5 x 10^n so
// the field reads as a number a person would type. The thinnest extent bounds
// what a blend or a wall can be on that body; a tenth leaves room for the
// smaller faces it meets. `fallback` stands when there is no body to measure.
double SizeFromBody(const AppState& state, uint64_t bodyId, double fallback) {
  for (const Body& b : state.document->GetBodies()) {
    if (b.createdBy != bodyId || !b.shape.IsValid())
      continue;
    glm::dvec3 lo, hi;
    b.shape.GetBoundingBox(lo, hi);
    const glm::dvec3 ext = hi - lo;
    const double thinnest = std::min({ext.x, ext.y, ext.z});
    if (!(thinnest > 0.0))
      return fallback;
    const double raw = thinnest / 10.0;
    const double decade = std::pow(10.0, std::floor(std::log10(raw)));
    const double mant = raw / decade;
    return decade * (mant >= 5.0 ? 5.0 : mant >= 2.0 ? 2.0 : 1.0);
  }
  return fallback;
}

// A shell's wall from the body's thinnest wall (ThinnestWall), a tenth of
// it rounded down to 1, 2 or 5 x 10^n: on a body already thin-walled the
// bounding box is not the wall, and a tenth of the box broke through it.
// SizeFromBody where no wall measures.
double SizeFromWall(const AppState& state, uint64_t bodyId, double fallback) {
  for (const Body& b : state.document->GetBodies()) {
    if (b.createdBy != bodyId || !b.shape.IsValid())
      continue;
    const auto wall = ThinnestWall(b);
    if (!wall || !(*wall > 0.0))
      break;
    const double raw = *wall / 10.0;
    const double decade = std::pow(10.0, std::floor(std::log10(raw)));
    const double mant = raw / decade;
    return decade * (mant >= 5.0 ? 5.0 : mant >= 2.0 ? 2.0 : 1.0);
  }
  return SizeFromBody(state, bodyId, fallback);
}

// Shared by CreateFillet/CreateChamfer: start the preview and arm the edge
// slot so viewport clicks collect edges on the pending feature.
void CreateEdgeBlend(AppState& state, EdgeBlendOp op) {
  if (!state.document || state.document->GetBodies().empty()) {
    return; // the menu gates this, but guard anyway
  }
  const char* base = op == EdgeBlendOp::Chamfer ? "Chamfer" : "Fillet";
  const double size =
      SizeFromBody(state, TargetBody(state, SelectionItem::Kind::Edge), 0.5);
  // BeginPreviewFeature seeds pre-selected edges and arms the edge slot.
  BeginPreviewFeature(
      state, std::make_unique<FilletFeature>(
                 NextName(state, base), std::vector<ElementRef>{}, size, op));
}
} // namespace

void CreateFillet(AppState& state) {
  CreateEdgeBlend(state, EdgeBlendOp::Fillet);
}

void CreateChamfer(AppState& state) {
  CreateEdgeBlend(state, EdgeBlendOp::Chamfer);
}

void CreateWorm(AppState& state) {
  if (!state.document || state.document->GetBodies().empty())
    return;
  BeginPreviewFeature(state, std::make_unique<WormFeature>(
                                 NextName(state, "Worm"), ElementRef{}));
}

void CreateWormWheel(AppState& state) {
  if (!state.document || state.document->GetBodies().empty())
    return;
  BeginPreviewFeature(state, std::make_unique<WormWheelFeature>(
                                 NextName(state, "Worm wheel"), ElementRef{}));
}

void CreateThread(AppState& state) {
  if (!state.document || state.document->GetBodies().empty())
    return;
  // BeginPreviewFeature seeds a pre-selected face and arms the face slot.
  BeginPreviewFeature(
      state, std::make_unique<ThreadFeature>(NextName(state, "Thread"),
                                             ElementRef{}, "M8", 1.25));
}

void CreateShell(AppState& state) {
  if (!state.document || state.document->GetBodies().empty())
    return;
  // The wall from the body's own thinnest wall: thin enough that OCC
  // accepts the offset on that body, at any scale and on a body already
  // hollow. A pre-selected open face names the body (the seed overrides
  // bodyId), so size from that one.
  const uint64_t body = TargetBody(state, SelectionItem::Kind::Face);
  BeginPreviewFeature(state, std::make_unique<ShellFeature>(
                                 NextName(state, "Shell"), NewestBody(state),
                                 std::vector<ElementRef>{},
                                 SizeFromWall(state, body, 0.25)));
}

void CreateDraft(AppState& state) {
  if (!state.document || state.document->GetBodies().empty())
    return;
  // bodyId stays 0: Draft has no body slot, so the first face pick names it.
  BeginPreviewFeature(
      state, std::make_unique<DraftFeature>(NextName(state, "Draft"), 0,
                                            std::vector<ElementRef>{}, 5.0));
}

namespace {

// 1, 2 or 5 x 10^n at or below `raw` (> 0): a number a person would type.
double RoundDown125(double raw) {
  const double decade = std::pow(10.0, std::floor(std::log10(raw)));
  const double mant = raw / decade;
  return decade * (mant >= 5.0 ? 5.0 : mant >= 2.0 ? 2.0 : 1.0);
}

const Body* BodyCreatedBy(const AppState& state, uint64_t id,
                          const std::string& face) {
  const Body* b =
      directedit::BodyHolding(state.document->GetBodies(), id, face);
  return b != nullptr && b->shape.IsValid() ? b : nullptr;
}

// The faces the selection holds, in order: what a direct edit seeds from.
std::vector<ElementRef> SelectedFaces(const AppState& state) {
  std::vector<ElementRef> out;
  for (const SelectionItem& item : state.selection.items)
    if (item.kind == SelectionItem::Kind::Face && item.element.featureId != 0)
      out.push_back(item.element);
  return out;
}

} // namespace

double SizeFromFace(const AppState& state, const ElementRef& face,
                    double fallback) {
  if (!state.document)
    return fallback;
  const Body* body = BodyCreatedBy(state, face.featureId, face.name);
  if (body == nullptr)
    return fallback;
  const auto at = body->elementMap.Resolve(face.name);
  double reach = directedit::ThinnestExtent(*body);
  if (at)
    if (const auto in = Measure::FaceInsetAt(*body, *at); in && in->hasInset)
      reach = std::min(reach, in->inset);
  if (!(reach > 0.0))
    return fallback;
  return RoundDown125(reach / 10.0);
}

glm::dvec3 DefaultFaceMove(const AppState& state,
                           const std::vector<ElementRef>& faces) {
  if (!state.document)
    return glm::dvec3(0.0);
  const uint64_t id = faces.empty()
                          ? TargetBody(state, SelectionItem::Kind::Face)
                          : faces.front().featureId;
  const Body* body =
      BodyCreatedBy(state, id, faces.empty() ? "" : faces.front().name);
  if (body == nullptr)
    return glm::dvec3(0.0);
  std::vector<int> indices;
  for (const ElementRef& f : faces)
    if (const auto at = body->elementMap.Resolve(f.name))
      indices.push_back(*at);
  const auto host = directedit::HostNormal(*body, indices);
  glm::dvec3 lo, hi;
  body->shape.GetBoundingBox(lo, hi, /*tight=*/true);
  const glm::dvec3 ext = hi - lo;
  int axis = -1;
  for (int a = 0; a < 3; a++) {
    // In the host's plane: an axis the host's normal is not along.
    if (host && std::abs((*host)[a]) > 0.5)
      continue;
    if (axis < 0 || ext[a] > ext[axis])
      axis = a;
  }
  if (axis < 0 || !(ext[axis] > 0.0))
    return glm::dvec3(0.0);
  glm::dvec3 out(0.0);
  out[axis] = RoundDown125(ext[axis] / 10.0);
  return out;
}

void CreateDeleteFace(AppState& state) {
  if (!state.document || state.document->GetBodies().empty())
    return;
  // bodyId stays 0: the first face pick (or the seeded selection) names it.
  BeginPreviewFeature(
      state, std::make_unique<DeleteFaceFeature>(NextName(state, "Delete face"),
                                                 0, std::vector<ElementRef>{}));
}

void CreateOffsetFace(AppState& state) {
  if (!state.document || state.document->GetBodies().empty())
    return;
  // From the face when one is selected (the seed then re-derives it the
  // same way), else from the body the tool will most likely act on.
  const std::vector<ElementRef> pre = SelectedFaces(state);
  const double size =
      pre.empty()
          ? SizeFromBody(state, TargetBody(state, SelectionItem::Kind::Face),
                         1.0)
          : SizeFromFace(state, pre.front(), 1.0);
  BeginPreviewFeature(state, std::make_unique<OffsetFaceFeature>(
                                 NextName(state, "Offset face"), 0,
                                 std::vector<ElementRef>{}, size));
}

void CreateMoveFace(AppState& state) {
  if (!state.document || state.document->GetBodies().empty())
    return;
  BeginPreviewFeature(state, std::make_unique<MoveFaceFeature>(
                                 NextName(state, "Move face"), 0,
                                 std::vector<ElementRef>{},
                                 DefaultFaceMove(state, SelectedFaces(state))));
}

void CreateReplaceFace(AppState& state) {
  if (!state.document || state.document->GetBodies().empty())
    return;
  // No numeric default: the target IS the parameter, and the tool refuses
  // until one is picked.
  BeginPreviewFeature(state,
                      std::make_unique<ReplaceFaceFeature>(
                          NextName(state, "Replace face"), 0, ElementRef{}));
}

void CreateBaseFlange(AppState& state) {
  if (!state.document)
    return;
  SketchFeature* sk = ClosedProfileSketch(state);
  if (sk == nullptr)
    return;
  // No thickness argument: the wall is one gauge, and the gauge is the
  // document's. With no rules set the feature refuses visibly rather than
  // inventing a millimetre
  // (docs/conventions.md#defaults-derive-from-what-they-act-on).
  BeginPreviewFeature(state, std::make_unique<BaseFlangeFeature>(
                                 NextName(state, "Base flange"), sk->GetId()));
}

void CreateEdgeFlange(AppState& state) {
  if (!state.document || state.document->GetBodies().empty())
    return;
  // The wall's default length is the shortest one the brake can make: half a
  // die opening (8t, the air-bending default) plus a thickness. Derived from
  // the stock, so it changes with the gauge and never lands under what
  // check_sheet_metal will ask for.
  double wall = 10.0;
  if (const auto& rules = state.document->SheetMetal())
    wall = 5.0 * rules->thickness;
  auto flange = std::make_unique<EdgeFlangeFeature>(
      NextName(state, "Flange"), std::vector<ElementRef>{}, wall, 90.0);
  BeginPreviewFeature(state, std::move(flange));
}

void CreateFlatPattern(AppState& state) {
  if (!state.document || state.document->GetBodies().empty())
    return;
  BeginPreviewFeature(state,
                      std::make_unique<FlatPatternFeature>(
                          NextName(state, "Flat pattern"), NewestBody(state)));
}

void CreateUnfold(AppState& state) {
  if (!state.document || state.document->GetBodies().empty())
    return;
  BeginPreviewFeature(state, std::make_unique<UnfoldFeature>(
                                 NextName(state, "Unfold"), NewestBody(state)));
}

void CreateRefold(AppState& state) {
  if (!state.document)
    return;
  // The Unfold to close is the most recent one still OPEN — the one whose
  // creases are flat on a body right now. Anything older is either closed
  // already or would fold up underneath a newer flat, which is not what a
  // "fold it back" button means.
  uint64_t target = 0;
  for (const Body& body : state.document->GetBodies())
    for (const BendRegion& b : body.bends)
      if (b.flat && b.unfoldedBy != 0) {
        const Feature* f = state.document->FindFeatureById(b.unfoldedBy);
        if (f != nullptr && f->TypeName() == "Unfold")
          target = std::max(target, b.unfoldedBy);
      }
  BeginPreviewFeature(state, std::make_unique<RefoldFeature>(
                                 NextName(state, "Refold"), target));
}

void CreateHem(AppState& state) {
  if (!state.document || state.document->GetBodies().empty())
    return;
  double ret = 6.0;
  if (const auto& rules = state.document->SheetMetal())
    ret = 4.0 * rules->thickness;
  BeginPreviewFeature(
      state, std::make_unique<HemFeature>(NextName(state, "Hem"),
                                          std::vector<ElementRef>{}, ret));
}

void CreateJog(AppState& state) {
  if (!state.document)
    return;
  uint64_t line = 0;
  for (const auto& f : state.document->GetFeatures()) {
    const auto* sk = dynamic_cast<const SketchFeature*>(f.get());
    if (sk == nullptr)
      continue;
    int lines = 0;
    for (const sketch::Entity& e : sk->entities)
      if (const auto* l = std::get_if<sketch::Line>(&e);
          l != nullptr && !l->construction)
        ++lines;
    if (lines == 1)
      line = sk->GetId();
  }
  // The least height the two bends allow, and one thickness of wall
  // between them — derived from the stock, never a literal millimetre.
  double height = 10.0;
  if (const auto& rules = state.document->SheetMetal())
    height = 2.0 * rules->EffectiveBendRadius() + 2.0 * rules->thickness;
  BeginPreviewFeature(state, std::make_unique<JogFeature>(
                                 NextName(state, "Jog"), line, height));
}

void CreateNormalCut(AppState& state) {
  if (!state.document || state.document->GetBodies().empty())
    return;
  BeginPreviewFeature(state, std::make_unique<NormalCutFeature>(
                                 NextName(state, "Normal cut"), 0));
}

void CreateCornerRelief(AppState& state) {
  if (!state.document || state.document->GetBodies().empty())
    return;
  BeginPreviewFeature(state,
                      std::make_unique<CornerReliefFeature>(
                          NextName(state, "Corner relief"), NewestBody(state)));
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
  glm::dvec3 bmin(std::numeric_limits<double>::max());
  glm::dvec3 bmax(std::numeric_limits<double>::lowest());
  bool found = false;
  for (const Body& b : state.document->GetBodies()) {
    if (b.createdBy != split.bodyId && b.createdBy != split.GetId())
      continue;
    glm::dvec3 lo, hi;
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
    // A modelled thread that cut its groove shows it as geometry; drawing
    // the convention over the real flanks would only hide them.
    if (th == nullptr || !th->overlay.valid || th->overlay.cut ||
        th->IsSuppressed())
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

} // namespace ui
