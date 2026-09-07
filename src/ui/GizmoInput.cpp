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

// Dragging an occurrence: the gizmo's spec, its hit test, the drag itself,
// and the same gizmo on a job tab moving a placement instead. Exploded
// views are here as well -- an explode is a transform per occurrence, which
// is what this file is about.
// Split out of AssemblyActions.cpp (docs/architecture-review-2026-09-06.md,
// stage 6). AssemblyActions.h still declares everything it declared.

namespace ui {

// ---- Occurrence drag gizmo (A2.5) -----------------------------------------

namespace {
const glm::vec3 kGizmoAxis[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
const glm::vec3 kGizmoColor[3] = {
    {0.90f, 0.32f, 0.32f}, {0.35f, 0.82f, 0.38f}, {0.38f, 0.55f, 0.95f}};

// Ray/axis/plane/arc math shared with the preview handles: ui/DragMath.h
// (GizmoScale, PerpTo, RayAxisParam, RayPlane, PointRayDist, RotateAngle).

// The gizmo-anchored occurrence: selected, non-fixed, mate tool idle. null
// else.
Occurrence* GizmoOccurrence(AppState& state) {
  const uint64_t occId = SelectedOccurrenceId(state);
  if (!state.assembly || occId == 0 || state.mateTool.active)
    return nullptr;
  Occurrence* o = state.assembly->FindById(occId);
  if (!o)
    return nullptr;
  // Explode mode drags a view offset, not the placement — even a fixed base
  // part explodes.
  if (state.activeExplodedView)
    return o;
  // Something that RIDES on another part has a DERIVED pose: dragging it
  // would be overwritten by the next ApplyAttachments, which is the shape of
  // a control that appears to work and does not. This is why a placed
  // fastener could be pulled out of its hole (docs/parts/f1-loose-ends.md).
  if (o->attachedTo != 0)
    return nullptr;
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
    if (m.b.occurrenceId == occId && m.error.empty())
      return &m;
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
  if (!Wa)
    return false; // dangling connector → no meaningful DOF gizmo
  for (int i = 0; i < 3; i++)
    spec.axis[i] = glm::normalize(glm::vec3((*Wa)[i]));
  // Where the DRIVEN part is, not where the mate's A frame is: the axial
  // offset seats B up or down the axis (a gear on a plate sits 6.5 mm above
  // the hole's mid-depth), and a ring drawn at the hole read as the gear
  // floating above it — reported as "the pinion moves away from the gears".
  spec.origin = glm::vec3((*Wa)[3]) + spec.axis[2] * mate->offset;
  switch (mate->type) {
  case MateType::Revolute:
    spec.rot[2] = true;
    break;
  case MateType::Slider:
    spec.trans[2] = true;
    break;
  case MateType::Cylindrical:
    spec.rot[2] = spec.trans[2] = true;
    break;
  case MateType::Planar:
    spec.trans[0] = spec.trans[1] = spec.rot[2] = true;
    break;
  case MateType::Ball:
    spec.rot[0] = spec.rot[1] = spec.rot[2] = true;
    break;
  case MateType::Fastened:
    return false; // no DOF
  }
  return true;
}

// The angle turned since the grab, unwrapped: the per-frame delta is
// wrapped into (-pi, pi] and accumulated, so crossing atan2's seam is a small
// step like any other rather than a full turn (see AppState::GizmoDrag).
float UnwrappedTurn(AppState::GizmoDrag& g, float angleNow) {
  float d = angleNow - g.lastAngle;
  while (d > glm::pi<float>())
    d -= 2.0f * glm::pi<float>();
  while (d <= -glm::pi<float>())
    d += 2.0f * glm::pi<float>();
  g.turned += d;
  g.lastAngle = angleNow;
  return g.turned;
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
      float t =
          std::clamp(RayAxisParam(ray.origin, ray.direction, spec.origin, A),
                     0.15f * L, L);
      const float da =
          PointRayDist(spec.origin + A * t, ray.origin, ray.direction);
      if (da < best) {
        best = da;
        axis = i;
        rotate = false;
        found = true;
      }
    }
    if (spec.rot[i]) {
      glm::vec3 hit;
      if (RayPlane(ray.origin, ray.direction, spec.origin, A, hit)) {
        const float dr = std::abs(glm::length(hit - spec.origin) - R);
        if (dr < best) {
          best = dr;
          axis = i;
          rotate = true;
          found = true;
        }
      }
    }
  }
  return found;
}

} // namespace

// Push a dragged occurrence's live transform straight to the renderers (no
// version bump / resync) for every render mesh of the occurrence.
void PushOccurrenceLive(AppState& state, uint64_t occId) {
  const Occurrence* occ = state.assembly->FindById(occId);
  if (!occ)
    return;
  const glm::mat4 base = DisplayMatrix(state, *occ);
  for (const DocumentRenderMesh& m : state.documentMeshes) {
    if (m.occurrenceId != occId)
      continue;
    // A5: compose the leaf's place INSIDE the unit. Pushing the occurrence
    // matrix alone piled every part of a dragged sub-assembly onto its
    // origin mid-drag — they snapped back only because the resync on release
    // recomputed the composition.
    const glm::mat4 xf = base * m.localXf;
    if (state.meshRenderer)
      state.meshRenderer->SetInstanceTransform(m.meshId, xf);
    if (state.outlineRenderer)
      state.outlineRenderer->SetMeshTransform(m.outlineId, xf);
  }
}

namespace {

// Publishes one axis's handles (arrow and/or arc) per the spec as
// ViewportHandles for the shared hover + click hit test AND the shared drawer
// (docs/ui/viewport-handles.md, ui/ViewportHandleDraw.h). Per-axis ids and
// per-axis colour, so hovering X highlights X rather than the whole gizmo.
// Nothing is drawn here: DrawViewportHandles draws the published record.
void PublishGizmoHandles(AppState& state, const GizmoSpec& spec, float L) {
  const float R = 0.78f * L;
  const auto publish = [&](int axis, bool rotate, float len) {
    ViewportHandle vh;
    vh.id = HandleId(HandleOwner::Gizmo, GizmoSub(axis, rotate));
    vh.kind = rotate ? HandleKind::Rotate : HandleKind::Linear;
    vh.origin = spec.origin;
    vh.dir = spec.axis[axis];
    vh.length = len;
    vh.color = kGizmoColor[axis];
    state.viewportHandles.push_back(vh);
  };
  for (int i = 0; i < 3; i++) {
    if (spec.trans[i])
      publish(i, false, L);
    if (spec.rot[i])
      publish(i, true, R);
  }
}
} // namespace

// ---- The placement gizmo on a JOB tab (docs/cam/job-document.md, M5) ----
//
// The same handles, on the selected job part: X and Y arrows move its
// placement in the stock, the Z arc spins it. Nothing out of plane -- which
// face is up is a fixturing statement and stays a face pick (setup-frame.md).
// A part without a placement gets one on grab, at the origin, which is where
// it was being drawn anyway.
namespace {

uint64_t PlacementGizmoPart(AppState& state) {
  if (!state.job || state.slotPick.Active() || state.measure.active)
    return 0;
  if (ActiveCamSetup(state) == nullptr)
    return 0;
  for (const SelectionItem& item : state.selection.items)
    if (item.jobPartId != 0 && (item.kind == SelectionItem::Kind::Body ||
                                item.kind == SelectionItem::Kind::Face ||
                                item.kind == SelectionItem::Kind::Edge))
      return item.jobPartId;
  return 0;
}

bool BuildPlacementGizmoSpec(AppState& state, uint64_t jobPartId,
                             GizmoSpec& spec) {
  // At the top centre of the part as DRAWN, so the handles sit on the
  // material they move.
  glm::vec3 lo(FLT_MAX), hi(-FLT_MAX);
  bool any = false;
  for (const Body& b : JobPlacedBodies(state)) {
    if (b.jobPartId != jobPartId)
      continue;
    glm::vec3 bmin, bmax;
    b.shape.GetBoundingBox(bmin, bmax);
    lo = glm::min(lo, bmin);
    hi = glm::max(hi, bmax);
    any = true;
  }
  if (!any)
    return false;
  spec.origin = glm::vec3((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, hi.z);
  spec.axis[0] = {1, 0, 0};
  spec.axis[1] = {0, 1, 0};
  spec.axis[2] = {0, 0, 1};
  spec.trans[0] = spec.trans[1] = true;
  spec.trans[2] = false;
  spec.rot[0] = spec.rot[1] = false;
  spec.rot[2] = true;
  return true;
}

cam::Placement* PlacementFor(AppState& state, uint64_t jobPartId) {
  cam::Setup* setup = ActiveCamSetup(state);
  if (setup == nullptr)
    return nullptr;
  for (cam::Placement& p : setup->placements)
    if (p.jobPartId == jobPartId)
      return &p;
  return nullptr;
}

bool HandlePlacementGizmo(AppState& state, const Ray& ray, bool clicked,
                          bool down, bool released) {
  auto& g = state.gizmoDrag;
  if (g.active) {
    cam::Placement* place = PlacementFor(state, g.jobPartId);
    if (released || !down || place == nullptr || !state.job) {
      g.active = false;
      g.jobPartId = 0;
      if (state.job) {
        // Where it landed is what the job cuts: the fit states, the
        // toolpaths and the verdict all follow the drop.
        state.projectMetaDirty = true;
        RefreshCamToolpaths(state);
        RunCamVerification(state);
      }
      return true;
    }
    const glm::vec3 A = g.frameAxis[g.axis];
    // Snapping, unless Ctrl says free: a part's edge to the sheet's edges
    // and to the other parts' -- which is how parts are butted together on
    // a board -- and a spin to the multiples of 45 degrees. The threshold
    // scales with the gizmo, so it is the same on screen at any zoom.
    const bool snap = !ImGui::GetIO().KeyCtrl;
    if (!g.rotate) {
      const float t = RayAxisParam(ray.origin, ray.direction, g.frameOrigin, A);
      double d = static_cast<double>(t - g.startParam);
      if (snap) {
        // On screen, a fraction of the gizmo; on the board, never less than
        // one percent of the sheet's longer side -- zoomed right in, a
        // gizmo-only window shrinks to nothing and the edge is missed.
        double threshold = 0.15 * GizmoScale(state);
        const int ax = g.axis; // 0 = X, 1 = Y
        const double lo = g.startBoxMin[ax] + d, hi = g.startBoxMax[ax] + d;
        std::vector<double> lines;
        const cam::Setup* setup = ActiveCamSetup(state);
        glm::dvec3 slo(0.0), shi(0.0);
        if (setup != nullptr && setup->stock.mode == cam::StockMode::Fixed &&
            cam::StockBounds(*setup, JobBodies(state), slo, shi)) {
          lines.push_back(slo[ax]);
          lines.push_back(shi[ax]);
          lines.push_back((slo[ax] + shi[ax]) * 0.5); // the sheet's centre
          threshold = std::max(threshold,
                               0.01 * std::max(shi.x - slo.x, shi.y - slo.y));
        }
        for (const Body& b : JobPlacedBodies(state)) {
          if (b.jobPartId == g.jobPartId)
            continue;
          glm::vec3 bmin, bmax;
          b.shape.GetBoundingBox(bmin, bmax);
          lines.push_back(bmin[ax]);
          lines.push_back(bmax[ax]);
        }
        double best = threshold, shift = 0.0;
        for (const double line : lines)
          for (const double edge : {lo, hi})
            if (std::abs(line - edge) < best) {
              best = std::abs(line - edge);
              shift = line - edge;
            }
        d += shift;
      }
      place->pos = g.startPlace + glm::dvec2(A.x * d, A.y * d);
    } else {
      float ang;
      if (RotateAngle(ray.origin, ray.direction, g.frameOrigin, A, ang)) {
        double deg = g.startRotZ +
                     glm::degrees(static_cast<double>(UnwrappedTurn(g, ang)));
        if (snap) {
          const double nearest = std::round(deg / 45.0) * 45.0;
          if (std::abs(deg - nearest) < 5.0)
            deg = nearest;
        }
        place->rotZdeg = deg;
      }
    }
    // The placed bodies are rebuilt from the placement on the next frame:
    // JobRenderVersion keys on it.
    state.job->BumpVersion();
    return true;
  }

  const uint64_t part = PlacementGizmoPart(state);
  if (part == 0 || !clicked)
    return false;
  GizmoSpec spec;
  if (!BuildPlacementGizmoSpec(state, part, spec))
    return false;
  int axis = 0;
  bool rotate = false;
  if (!GizmoHitTest(state, spec, ray, axis, rotate))
    return false;
  cam::Setup* setup = ActiveCamSetup(state);
  SnapshotJob(state); // one undo step per drag (R7)
  cam::Placement* place = PlacementFor(state, part);
  if (place == nullptr) {
    cam::Placement fresh;
    fresh.id = state.job->AllocateId();
    fresh.jobPartId = part;
    setup->placements.push_back(fresh);
    place = &setup->placements.back();
  }
  g.active = true;
  g.axis = axis;
  g.rotate = rotate;
  g.frameOrigin = spec.origin;
  for (int i = 0; i < 3; i++)
    g.frameAxis[i] = spec.axis[i];
  g.mateId = 0;
  g.explodeView = 0;
  g.jobPartId = part;
  g.startPlace = place->pos;
  g.startRotZ = place->rotZdeg;
  g.startBoxMin = glm::vec3(FLT_MAX);
  g.startBoxMax = glm::vec3(-FLT_MAX);
  for (const Body& b : JobPlacedBodies(state)) {
    if (b.jobPartId != part)
      continue;
    glm::vec3 bmin, bmax;
    b.shape.GetBoundingBox(bmin, bmax);
    g.startBoxMin = glm::min(g.startBoxMin, bmin);
    g.startBoxMax = glm::max(g.startBoxMax, bmax);
  }
  const glm::vec3 A = spec.axis[axis];
  if (!rotate) {
    g.startParam = RayAxisParam(ray.origin, ray.direction, spec.origin, A);
  } else {
    float ang = 0.0f;
    (void)RotateAngle(ray.origin, ray.direction, spec.origin, A, ang);
    g.startParam = ang;
    g.lastAngle = ang;
    g.turned = 0.0f;
  }
  return true;
}

} // namespace

void PublishGizmoHandles(AppState& state) {
  if (state.job) {
    const uint64_t part = PlacementGizmoPart(state);
    GizmoSpec spec;
    if (part != 0 && BuildPlacementGizmoSpec(state, part, spec))
      PublishGizmoHandles(state, spec, GizmoScale(state));
    return;
  }
  Occurrence* occ = GizmoOccurrence(state);
  if (!occ)
    return;
  GizmoSpec spec;
  Mate* mate = nullptr;
  if (!BuildGizmoSpec(state, occ, spec, mate))
    return;
  PublishGizmoHandles(state, spec, GizmoScale(state));
}

bool HandleGizmoInput(AppState& state, const Ray& ray, bool clicked, bool down,
                      bool released) {
  if (state.job)
    return HandlePlacementGizmo(state, ray, clicked, down, released);
  auto& g = state.gizmoDrag;

  if (g.active) {
    Occurrence* occ =
        state.assembly ? state.assembly->FindById(SelectedOccurrenceId(state))
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
          if (s.occurrenceId == occ->id) {
            step = &s;
            break;
          }
      if (!step) { // view or step vanished mid-drag (undo?) — abort cleanly
        g.active = false;
        return true;
      }
      const float t = RayAxisParam(ray.origin, ray.direction, g.frameOrigin, A);
      const float f =
          std::max(state.explodeFactor, 0.05f); // never divide by ~0
      step->offset = g.startOffset + A * ((t - g.startParam) / f);
      PushOccurrenceLive(state, occ->id);
      return true;
    }
    if (!g.mateId) {
      // Free occurrence: drag its transform directly.
      if (!g.rotate) {
        const float t =
            RayAxisParam(ray.origin, ray.direction, g.frameOrigin, A);
        occ->position = g.startPos + A * (t - g.startParam);
      } else {
        float ang;
        if (RotateAngle(ray.origin, ray.direction, g.frameOrigin, A, ang))
          occ->orientation =
              glm::angleAxis(UnwrappedTurn(g, ang), A) * g.startRot;
      }
    } else {
      // Drag-within-mate: write the mate's DOF, then re-solve so the
      // occurrence follows.
      if (!g.rotate) {
        const float t =
            RayAxisParam(ray.origin, ray.direction, g.frameOrigin, A);
        const float v = g.startDof + (t - g.startParam);
        if (g.axis == 0)
          mate->dx = v;
        else if (g.axis == 1)
          mate->dy = v;
        else
          mate->offset = v;
      } else {
        float ang;
        if (RotateAngle(ray.origin, ray.direction, g.frameOrigin, A, ang)) {
          const float d = UnwrappedTurn(g, ang);
          if (mate->type == MateType::Ball)
            mate->ballRotation = glm::angleAxis(d, A) * g.startBall;
          else
            mate->rotation = g.startDof + d;
        }
      }
      // The dragged mate drives its relations (a gear turns its partner).
      (void)assembly::PropagateRelations(*state.assembly, mate->id);
      SolveActiveAssembly(state);
      return true; // solve already repositioned + resynced
    }
    // A FREE drag moves the occurrence directly and never solves — which is
    // where riders were being left behind, because ApplyAttachments runs at
    // the end of the solve. Reported from use: a bar dragged away from its
    // bolt, with the nut still hanging in the air where the joint was.
    //
    // Re-seated here rather than by solving: a free drag has nothing to
    // solve, and the composition is a few matrix multiplies per rider.
    for (const Occurrence& rider : state.assembly->GetOccurrences())
      if (rider.attachedTo == occ->id) {
        assembly::ApplyAttachments(*state.assembly);
        break;
      }
    PushOccurrenceLive(state, occ->id);
    for (const Occurrence& rider : state.assembly->GetOccurrences())
      if (rider.attachedTo == occ->id)
        PushOccurrenceLive(state, rider.id);
    return true;
  }

  Occurrence* occ = GizmoOccurrence(state);
  if (!occ || !clicked)
    return false;
  GizmoSpec spec;
  Mate* mate = nullptr;
  if (!BuildGizmoSpec(state, occ, spec, mate))
    return false;
  int axis = 0;
  bool rotate = false;
  if (!GizmoHitTest(state, spec, ray, axis, rotate))
    return false;

  SnapshotAssembly(state); // one snapshot per drag
  g.active = true;
  g.axis = axis;
  g.rotate = rotate;
  g.frameOrigin = spec.origin;
  for (int i = 0; i < 3; i++)
    g.frameAxis[i] = spec.axis[i];
  g.mateId = mate ? mate->id : 0;
  g.explodeView = state.activeExplodedView;
  if (g.explodeView) {
    // Find-or-create this occurrence's step in the active view (after the
    // snapshot, so the whole drag is one undo step).
    ExplodedView* view = state.assembly->FindExplodedViewById(g.explodeView);
    ExplodeStep* step = nullptr;
    for (ExplodeStep& s : view->steps)
      if (s.occurrenceId == occ->id) {
        step = &s;
        break;
      }
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
    g.lastAngle = ang;
    g.turned = 0.0f;
  }
  if (mate) { // capture the DOF value at grab
    g.startDof = rotate ? mate->rotation
                        : (axis == 0   ? mate->dx
                           : axis == 1 ? mate->dy
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
  struct OccCenter {
    uint64_t id;
    glm::vec3 center;
  };
  std::vector<OccCenter> centers;
  glm::vec3 lo(FLT_MAX), hi(-FLT_MAX);
  // A5: bound each TOP-LEVEL occurrence's whole subtree. A sub-assembly
  // explodes as one unit — spacing it by the bounds of its first part would
  // send a gangway the distance a single board deserves.
  std::map<uint64_t, std::pair<glm::vec3, glm::vec3>> boxes;
  for (const assembly::FlatInstance& inst : FlattenActiveAssembly(state)) {
    if (!inst.visible)
      continue;
    for (const auto& body : inst.part->GetBodies()) {
      if (body.reference)
        continue; // a ghost does not set the assembly's extent
      glm::vec3 mn, mx;
      body.shape.GetBoundingBox(mn, mx);
      for (int c = 0; c < 8; c++) {
        const glm::vec3 corner((c & 1) ? mx.x : mn.x, (c & 2) ? mx.y : mn.y,
                               (c & 4) ? mx.z : mn.z);
        const glm::vec3 w = glm::vec3(inst.world * glm::vec4(corner, 1.0f));
        auto [it, fresh] =
            boxes.try_emplace(inst.rootOccurrenceId, std::make_pair(w, w));
        if (!fresh) {
          it->second.first = glm::min(it->second.first, w);
          it->second.second = glm::max(it->second.second, w);
        }
        lo = glm::min(lo, w);
        hi = glm::max(hi, w);
      }
    }
  }
  for (const auto& [occId, box] : boxes)
    centers.push_back({occId, (box.first + box.second) * 0.5f});
  if (centers.size() < 2)
    return false; // nothing to spread apart

  glm::vec3 centroid(0.0f);
  for (const OccCenter& c : centers)
    centroid += c.center;
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

} // namespace ui
