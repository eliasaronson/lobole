#include "ui/SketchOverlay.h"
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

// The sketch as VERTICES: the active sketch's entities, dimensions and
// snap markers expanded into the primitive channel, plus the edge-pick
// highlight a blend tool draws over a body's edges.
//
// SketchInteraction.cpp owns what a click MEANS; this owns what the result
// looks like to the renderer.
// Declared in ui/SketchOverlay.h; the layer is described in
// docs/architecture.md#the-ui-actions-layer.

namespace ui {

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

} // namespace ui
