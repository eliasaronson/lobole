#include "ui/AssemblyActions.h"

#include "core/cam/CamToolpath.h" // StockBounds (the placement snap)
#include "imgui.h" // KeyCtrl: the placement drag snaps unless Ctrl says free
#include "ui/CamPanel.h" // ActiveCamSetup, RefreshCamToolpaths (the placement gizmo)
#include "ui/CamTool.h" // RunCamVerification
#include "ui/DocumentSlots.h"
#include "ui/JobActions.h" // JobPlacedBodies
#include "ui/ProjectActions.h"
#include "ui/SelectionPipeline.h"
#include "ui/ViewActions.h"

#include "ui/FeatureEditor.h" // PickSlot, kMateSessionSlot (U7c mate slots)

#include "cad/CADKernel.h" // CADKernel::ImportedModel (import)
#include "cad/CADMaterials.h"
#include "cad/CADMeshConverter.h"
#include "cad/FaceSelector.h"
#include "common/Logger.h"
#include "core/AssemblySolve.h"
#include "core/Command.h"
#include "core/Feature.h"
#include "core/MassProps.h"
#include "core/Measure.h"
#include "core/drawing/DrawingCallout.h"
#include "core/drawing/DrawingDimension.h"
#include "core/drawing/DrawingExport.h"
#include "core/drawing/DrawingProjection.h"
#include "core/drawing/DrawingRender.h"
#include "core/features/BooleanFeature.h"
#include "core/features/DatumAxisFeature.h"
#include "core/features/DatumPlaneFeature.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/FastenerFeature.h"
#include "core/features/FilletFeature.h"
#include "core/features/HoleFeature.h"
#include "core/io/Autosaver.h"
#include "core/io/GitVault.h"
#include "core/io/ImportAssembly.h"
#include "core/io/Serialization.h"
#include "core/parts/AxisStack.h"
#include "core/parts/FastenerProfile.h"
#include "core/parts/FastenerTable.h"
#include "render/Camera.h"
#include "render/FieldRenderer.h"
#include "render/IBLTextures.h"
#include "render/MeshRenderer.h"
#include "render/OutlineRenderer.h"
#include "render/RayTracingRenderer.h"
#include "render/SketchRenderer.h"
#include "render/VulkanCommon.h"
#include "ui/DragMath.h"

#include "core/features/PatternFeature.h"
#include "core/features/RevolveFeature.h"
#include "core/features/SketchFeature.h"
#include "core/sketch/SketchProfile.h"
#include "ui/PreviewFeature.h"
#include "ui/SketchInteraction.h" // SketchUVToScreen
#include <nlohmann/json.hpp>

#include <glm/gtc/matrix_transform.hpp> // glm::translate
#include <glm/gtc/quaternion.hpp>       // glm::mat4_cast

#include "ui/AppActionsInternal.h"
#include "ui/AppState.h"
#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include <OSD.hxx>              // per-thread signal handler (prewarm workers)
#include <Standard_Failure.hxx> // ...and what it converts faults into

#include <atomic>
#include <thread>

// Creating a mate: picking the two connectors, previewing the result live,
// and accepting or cancelling. The face pick that starts it lives here too,
// because what a connector IS is a face plus its occurrence.
// Split out of AssemblyActions.cpp (docs/architecture-review-2026-09-06.md,
// stage 6). AssemblyActions.h still declares everything it declared.

namespace ui {

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
    if (m.meshId == hit.meshId) {
      rm = &m;
      break;
    }
  if (!rm || rm->occurrenceId == 0)
    return false;

  const Occurrence* occ = state.assembly->FindById(rm->occurrenceId);
  if (!occ)
    return false;
  // A5: the face belongs to the LEAF part. For a sub-assembly occurrence
  // occ->partId is an assembly — no bodies, no faces — so resolving through
  // it returned false and a click inside a placed unit picked nothing.
  Document* part =
      PartDocById(state, rm->leafPartId ? rm->leafPartId : occ->partId);
  if (!part || rm->bodyIndex >= part->GetBodies().size())
    return false;

  // The face's persistent toponaming name → an ElementRef the mate stores.
  const Body& body = part->GetBodies()[rm->bodyIndex];
  const std::string* name =
      body.elementMap.NameOf(static_cast<int>(hit.faceIndex));
  if (!name)
    return false; // an unnamed face (e.g. a pattern output) can't be mated

  out.occurrenceId = rm->occurrenceId;
  out.innerPath = rm->innerPath;
  out.face = ElementRef{body.createdBy, *name};
  out.snapEdge = ElementRef{};
  out.snapEdge2 = ElementRef{};
  out.snapType = AssemblyFacePick::SnapType::FaceCenter;

  // Every named boundary edge contributes its characteristic point: a circle
  // center or another edge's midpoint. The face center competes as a candidate
  // too, so a click in the middle of a face does not always snap to an edge.
  //
  // ONE call for all of them. This was three — FaceEdgeIndices,
  // FaceCircleCenters, and EdgeSnapPoint once per edge — each of which mapped
  // every edge of the whole body, so a hover on a drilled face cost
  // O(face edges x body edges) every frame: 10 ms on a 144-hole plate,
  // 88 ms on a 400-hole one (docs/known-bugs.md).
  std::vector<CADShape::FaceSnap> snaps;
  body.shape.FaceSnapPoints(static_cast<int>(hit.faceIndex), snaps);
  // ...and the face's CORNERS (phase 4c), which join the same ranking. A
  // corner is named by the two edges meeting there, both already named;
  // pairs that meet twice name no single point and are not offered.
  std::vector<CADShape::FaceCorner> corners;
  body.shape.FaceCorners(static_cast<int>(hit.faceIndex), corners);
  const glm::mat4 xf = OccurrenceMatrix(*occ);
  glm::vec3 faceOrigin, faceNormal, faceXAxis;
  const bool hasFaceFrame =
      body.shape.FacePlane(static_cast<int>(hit.faceIndex), faceOrigin,
                           faceNormal, faceXAxis) ||
      body.shape.FaceAxis(static_cast<int>(hit.faceIndex), faceOrigin,
                          faceNormal, faceXAxis);
  const glm::vec3 worldFaceOrigin = glm::vec3(xf * glm::vec4(faceOrigin, 1.0f));
  float best =
      hasFaceFrame ? glm::length(worldFaceOrigin - hit.point) : FLT_MAX;
  for (const CADShape::FaceSnap& snap : snaps) {
    const std::string* edgeName = body.elementMap.EdgeNameOf(snap.edgeIndex);
    if (!edgeName) // an unnamed edge cannot be stored on the anchor
      continue;
    const glm::vec3 worldPoint = glm::vec3(xf * glm::vec4(snap.point, 1.0f));
    const float distance = glm::length(worldPoint - hit.point);
    if (distance >= best)
      continue;
    best = distance;
    out.snapEdge = ElementRef{body.createdBy, *edgeName};
    out.snapEdge2 = ElementRef{};
    out.snapType = snap.circleCenter ? AssemblyFacePick::SnapType::HoleCenter
                                     : AssemblyFacePick::SnapType::EdgeMidpoint;
  }
  for (const CADShape::FaceCorner& corner : corners) {
    const std::string* nameA = body.elementMap.EdgeNameOf(corner.edgeA);
    const std::string* nameB = body.elementMap.EdgeNameOf(corner.edgeB);
    if (!nameA || !nameB) // both halves must be nameable, or it is not stable
      continue;
    const glm::vec3 worldPoint = glm::vec3(xf * glm::vec4(corner.point, 1.0f));
    const float distance = glm::length(worldPoint - hit.point);
    if (distance >= best)
      continue;
    best = distance;
    out.snapEdge = ElementRef{body.createdBy, *nameA};
    out.snapEdge2 = ElementRef{body.createdBy, *nameB};
    out.snapType = AssemblyFacePick::SnapType::Corner;
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

// Arms one of the mate session's connector slots on the shared armed-slot
// state (kMateSessionSlot marks it as non-feature; SlotPickClicked routes
// back to MateConnectorPicked).
void ArmMateConnector(AppState& state, bool second) {
  state.slotPick = {kMateSessionSlot,
                    static_cast<int>(second ? PickSlot::MateConnectorB
                                            : PickSlot::MateConnectorA),
                    SelectionKindBit(SelectionItem::Kind::Connector),
                    /*multi=*/false,
                    second ? "Pick the second face (connector B)"
                           : "Pick the first face (connector A)"};
}

namespace {
// A pipeline Connector item as the mate-side Connector it names.
Connector ConnectorFromItem(const SelectionItem& item) {
  Connector c;
  c.occurrenceId = item.id;
  c.anchor.origin = false;
  c.anchor.face = item.element;
  c.anchor.snapEdge = item.snapElement;
  c.anchor.snapEdge2 = item.snapElement2;
  // A5/v57: a face inside a placed unit. The mate still positions the
  // top-level occurrence; the path just says which face it anchors on.
  c.innerPath = item.innerPath;
  return c;
}
} // namespace

void StartMateTool(AppState& state, MateType type) {
  if (!state.assembly)
    return;
  CancelMateTool(state); // discard any in-progress preview first
  state.mateTool = AppState::MateToolState{};
  state.mateTool.active = true;
  state.mateTool.type = type;
  SelectOccurrence(state, 0); // the tool owns clicks now
  ArmMateConnector(state, /*second=*/false);
}

void StartMateEdit(AppState& state, uint64_t mateId) {
  if (!state.assembly || state.assembly->FindMateById(mateId) == nullptr)
    return;
  CancelMateTool(state);
  state.mateTool = AppState::MateToolState{};
  state.mateTool.active = true;
  state.mateTool.editMateId = mateId;
  state.mateTool.message =
      "Click a connector row, then pick its new face in the viewport";
}

void MateConnectorPicked(AppState& state, const SelectionItem& item) {
  auto& tool = state.mateTool;
  if (!state.assembly || !tool.active ||
      item.kind != SelectionItem::Kind::Connector) {
    return;
  }
  const bool slotB =
      state.slotPick.slot == static_cast<int>(PickSlot::MateConnectorB);
  const Connector conn = ConnectorFromItem(item);

  // Re-pick session on an existing mate: rewrite the armed side and
  // re-solve; each pick is one undo step. The slot disarms after one pick
  // (click the other row to re-pick that side).
  if (tool.editMateId != 0) {
    Mate* m = state.assembly->FindMateById(tool.editMateId);
    if (m == nullptr) {
      CancelMateTool(state);
      return;
    }
    Connector& other = slotB ? m->a : m->b;
    if (conn.occurrenceId == other.occurrenceId) {
      tool.message = "Pick a face on a different occurrence";
      return;
    }
    SnapshotAssembly(state);
    (slotB ? m->b : m->a) = conn;
    state.assembly->BumpVersion();
    SolveActiveAssembly(state);
    tool.message.clear();
    state.slotPick = {};
    return;
  }

  if (!slotB) {
    tool.first = conn;
    tool.haveFirst = true;
    tool.message = conn.anchor.snapEdge.IsSet()
                       ? "Snapped to an edge — pick connector B"
                       : "";
    ArmMateConnector(state, /*second=*/true);
    return;
  }
  if (conn.occurrenceId == tool.first.occurrenceId) {
    tool.message = "Pick a face on a different occurrence";
    return; // stay armed
  }
  // Second connector: add a live preview mate (snapshot first, so accept is a
  // single undoable action and cancel just pops it) and solve.
  Mate mate;
  mate.type = tool.type;
  mate.name = "Mate";
  mate.a = tool.first;
  mate.b = conn;
  SnapshotAssembly(state);
  tool.previewMateId = state.assembly->AddMate(mate);
  const int errored = SolveActiveAssembly(state);
  // Rotate is offered for every type but Ball, whose twist about Z folds
  // into its own DOF — so the prompt must not name a control that is not
  // drawn beneath it.
  const bool canRotate = mate.type != MateType::Ball;
  if (errored)
    tool.message = canRotate
                       ? "Over-constrained — try Flip or Rotate, or Cancel"
                       : "Over-constrained — try Flip, or Cancel";
  else
    tool.message = canRotate ? "Accept, Flip, Rotate, or Cancel"
                             : "Accept, Flip, or Cancel";
  state.slotPick = {}; // both picked; the session shows Accept/Cancel
}

void AcceptMateTool(AppState& state) {
  if (!state.mateTool.active)
    return;
  const uint64_t id = state.mateTool.previewMateId;
  state.mateTool = AppState::MateToolState{}; // keep the mate; clear the tool
  if (state.slotPick.featureId == kMateSessionSlot)
    state.slotPick = {};
  if (id)
    SelectMate(state, id);
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
  if (state.slotPick.featureId == kMateSessionSlot)
    state.slotPick = {};
}

namespace {
void AppendSnapMarker(std::vector<LineVertex3D>& lines, const glm::mat4& f,
                      float scale, bool edgeSnap, bool midpoint, bool corner) {
  const glm::vec3 origin(f[3]);
  const glm::vec3 color = corner     ? glm::vec3(0.55f, 1.0f, 0.45f)
                          : midpoint ? glm::vec3(0.30f, 0.85f, 1.0f)
                          : edgeSnap ? glm::vec3(1.0f, 0.78f, 0.22f)
                                     : glm::vec3(0.82f, 0.82f, 0.60f);
  const glm::vec3 u = glm::normalize(glm::vec3(f[0])) * scale;
  const glm::vec3 v = glm::normalize(glm::vec3(f[1])) * scale;
  if (corner) {
    // An L, not a cross: a corner marker that reads as a corner, and does
    // not look identical to the three snaps that are not one.
    lines.push_back(LineVertex(origin, color));
    lines.push_back(LineVertex(origin + u, color));
    lines.push_back(LineVertex(origin, color));
    lines.push_back(LineVertex(origin + v, color));
    return;
  }
  lines.push_back(LineVertex(origin - u, color));
  lines.push_back(LineVertex(origin + u, color));
  lines.push_back(LineVertex(origin - v, color));
  lines.push_back(LineVertex(origin + v, color));
}

void AppendFrameTriad(std::vector<LineVertex3D>& lines, const glm::mat4& f,
                      float scale) {
  const glm::vec3 o(f[3]);
  const glm::vec3 cols[3] = {
      {0.90f, 0.32f, 0.32f}, {0.35f, 0.82f, 0.38f}, {0.38f, 0.55f, 0.95f}};
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
  if (state.mateTool.hasHover && state.mateTool.previewMateId == 0)
    if (const auto f = ConnectorWorldFrame(state, state.mateTool.hover))
      AppendSnapMarker(lines, *f, GizmoScale(state) * 0.12f,
                       state.mateTool.hover.anchor.snapEdge.IsSet(),
                       state.mateTool.hoverSnapIsMidpoint,
                       state.mateTool.hoverSnapIsCorner);
  if (state.mateTool.haveFirst)
    if (const auto f = ConnectorWorldFrame(state, state.mateTool.first))
      AppendFrameTriad(lines, *f, scale);
  if (const Mate* m =
          state.mateTool.previewMateId
              ? state.assembly->FindMateById(state.mateTool.previewMateId)
              : nullptr) {
    if (const auto fb = ConnectorWorldFrame(state, m->b))
      AppendFrameTriad(lines, *fb, scale);
  }
}

} // namespace ui
