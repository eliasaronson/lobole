// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/AssemblyActions.h"

#include "core/cam/CamToolpath.h" // StockBounds (the placement snap)
#include "imgui.h" // KeyCtrl: the placement drag snaps unless Ctrl says free
#include "ui/CamPanel.h" // ActiveCamSetup, RefreshCamToolpaths (the placement gizmo)
#include "ui/CamTool.h" // RunCamVerification
#include "ui/DocumentSlots.h"
#include "ui/FlexibleActions.h" // a moving part of a flexible unit
#include "ui/JobActions.h"      // JobPlacedBodies
#include "ui/MotionActions.h"   // MotionPlaybackLocal
#include "ui/ProjectActions.h"
#include "ui/SelectionPipeline.h"
#include "ui/SequencePlayback.h"
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
// Split out of AssemblyActions.cpp (docs/architecture.md#the-ui-actions-layer).
// AssemblyActions.h still declares everything it declared.

namespace ui {

// ---- Occurrence drag gizmo (A2.5) -----------------------------------------

namespace {
// The screen ray, widened once: the gizmo drags a placement, and a placement
// is double (docs/conventions.md#the-model-is-double).
struct DRay {
  glm::dvec3 origin, direction;
};
DRay Widen(const Ray& r) {
  return {glm::dvec3(r.origin), glm::dvec3(r.direction)};
}

const glm::dvec3 kGizmoAxis[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
const glm::dvec3 kGizmoColor[3] = {
    {0.90, 0.32, 0.32}, {0.35, 0.82, 0.38}, {0.38, 0.55, 0.95}};

// Ray/axis/plane/arc math shared with the preview handles: ui/RayMath.h
// (PerpTo, RayAxisParam, RayPlane, PointRayDist, RotateAngle) and
// ui/DragMath.h (GizmoScale, and SnapAngleDeg/SnapAngleRad + the 45/5
// degree constants that the placement spin and both assembly rotate
// drags share).

// The gizmo-anchored occurrence: selected, non-fixed, mate tool idle. null
// else. `inner` is set when the selection is a MOVING PART of a flexible
// placement (the placement is returned): that part drags within its own mate
// whether or not the unit is fixed, as a top-level part does.
Occurrence* GizmoOccurrence(AppState& state, uint64_t& inner) {
  inner = 0;
  if (!state.assembly || state.mateTool.active)
    return nullptr;
  if (const auto [unit, part] = SelectedInnerOccurrence(state); unit != 0) {
    Occurrence* F = state.assembly->FindById(unit);
    if (F == nullptr || !IsMovingInner(state, *F, part))
      return nullptr;
    inner = part;
    return F;
  }
  const uint64_t occId = SelectedOccurrenceId(state);
  if (occId == 0)
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
  // fastener could be pulled out of its hole (docs/parts/README.md).
  if (o->attachedTo != 0)
    return nullptr;
  return !o->fixed ? o : nullptr;
}

// The gizmo's frame + which handles show. A free occurrence gets world axes at
// its origin with all 6 handles; a mated occurrence gets its mate's connector-A
// world frame with only the mate's free DOF (drag-within-mate, phase 3).
struct GizmoSpec {
  glm::dvec3 origin{0};
  glm::dvec3 axis[3]{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
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

// A moving part of a flexible placement (v129): the sub-assembly's mate that
// places it gives the frame and the handles, at the placement's own pose of
// it. Only what set_mate {path} can pose drags -- a turn and a slide; a
// planar mate's in-plane slide and a ball are the sub-assembly's to edit on
// its own tab.
bool BuildInnerGizmoSpec(AppState& state, Occurrence* occ, uint64_t inner,
                         GizmoSpec& spec, const Mate*& mate) {
  const AssemblyDocument* sub = FlexibleSub(state, *occ);
  const Occurrence* part = sub ? sub->FindById(inner) : nullptr;
  if (part == nullptr)
    return false;
  if (state.activeExplodedView) {
    if (!state.assembly->FindExplodedViewById(state.activeExplodedView))
      return false;
    spec.origin = glm::dvec3(InnerDisplayMatrix(state, *occ, *part)[3]);
    for (int i = 0; i < 3; i++) {
      spec.axis[i] = kGizmoAxis[i];
      spec.trans[i] = true;
    }
    return true;
  }
  mate = InnerMateDriving(state, *occ, inner);
  if (mate == nullptr)
    return false; // rides on another moving part: drag that one
  const auto Wa = MateConnectorWorldAt(state, {occ->id}, mate->a);
  if (!Wa)
    return false;
  for (int i = 0; i < 3; i++)
    spec.axis[i] = glm::normalize(glm::dvec3((*Wa)[i]));
  const double offset =
      assembly::MateDofAt(*state.assembly, {occ->id}, mate->id, MateDof::Offset,
                          AssemblyResolverFor(state))
          .value_or(mate->offset);
  spec.origin = glm::dvec3((*Wa)[3]) + spec.axis[2] * offset;
  switch (mate->type) {
  case MateType::Revolute:
  case MateType::Planar:
    spec.rot[2] = true;
    break;
  case MateType::Slider:
    spec.trans[2] = true;
    break;
  case MateType::Cylindrical:
    spec.rot[2] = spec.trans[2] = true;
    break;
  case MateType::Ball:
  case MateType::Fastened:
    return false;
  }
  return true;
}

// Fills `spec` for the selected occurrence and reports the driving mate (null =
// free 6-DOF). Returns false if no gizmo should show (fastened mate / dangling
// connector).
bool BuildGizmoSpec(AppState& state, Occurrence* occ, uint64_t inner,
                    GizmoSpec& spec, const Mate*& mate) {
  mate = nullptr;
  if (inner != 0)
    return BuildInnerGizmoSpec(state, occ, inner, spec, mate);
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
    spec.axis[i] = glm::normalize(glm::dvec3((*Wa)[i]));
  // Where the DRIVEN part is, not where the mate's A frame is: the axial
  // offset seats B up or down the axis (a gear on a plate sits 6.5 mm above
  // the hole's mid-depth), and a ring drawn at the hole read as the gear
  // floating above it — reported as "the pinion moves away from the gears".
  spec.origin = glm::dvec3((*Wa)[3]) + spec.axis[2] * mate->offset;
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
    // No DOF, so no gizmo. Its authored twist about the connector axis is
    // reached from the mate tool window / inspector, not from here.
    return false;
  }
  return true;
}

// The angle turned since the grab, unwrapped: the per-frame delta is
// wrapped into (-pi, pi] and accumulated, so crossing atan2's seam is a small
// step like any other rather than a full turn (see AppState::GizmoDrag).
double UnwrappedTurn(AppState::GizmoDrag& g, double angleNow) {
  double d = angleNow - g.lastAngle;
  while (d > glm::pi<double>())
    d -= 2.0 * glm::pi<double>();
  while (d <= -glm::pi<double>())
    d += 2.0 * glm::pi<double>();
  g.turned += d;
  g.lastAngle = angleNow;
  return g.turned;
}

// Ray vs the spec's enabled handles → (axis, rotate). Arrows win ties.
bool GizmoHitTest(const AppState& state, const GizmoSpec& spec,
                  const Ray& screenRay, int& axis, bool& rotate) {
  const DRay ray = Widen(screenRay);
  const double scale = GizmoScale(state);
  const double grab = 0.14 * scale, L = scale, R = 0.78 * scale;
  double best = grab;
  bool found = false;
  for (int i = 0; i < 3; i++) {
    const glm::dvec3 A = spec.axis[i];
    if (spec.trans[i]) {
      double t = std::clamp(
          RayAxisParam(ray.origin, ray.direction, spec.origin, A), 0.15 * L, L);
      const double da =
          PointRayDist(spec.origin + A * t, ray.origin, ray.direction);
      if (da < best) {
        best = da;
        axis = i;
        rotate = false;
        found = true;
      }
    }
    if (spec.rot[i]) {
      glm::dvec3 hit;
      if (RayPlane(ray.origin, ray.direction, spec.origin, A, hit)) {
        const double dr = std::abs(glm::length(hit - spec.origin) - R);
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
  const glm::dmat4 base = DisplayMatrix(state, *occ);
  // A sequence being played hides what it has not put in yet; only then,
  // so every other push is what it was.
  const bool sequence = ShownSequence(state) != nullptr;
  const bool hidden = sequence && SequencePlaybackHides(state, occId);
  for (const DocumentRenderMesh& m : state.documentMeshes) {
    if (m.occurrenceId != occId)
      continue;
    // A5: compose the leaf's place INSIDE the unit. Pushing the occurrence
    // matrix alone piled every part of a dragged sub-assembly onto its
    // origin mid-drag — they snapped back only because the resync on release
    // recomputed the composition.
    glm::dmat4 xf = base * glm::dmat4(m.localXf);
    // Inside a flexible placement (v129) playback moves each moving leaf
    // on its own, not just the unit.
    glm::dmat4 played;
    if (!m.innerPath.empty() &&
        MotionPlaybackLocal(state, *occ, m.innerPath, m.localXf, played))
      xf = base * played;
    // ...and an exploded view may displace it on its own (v130).
    if (const glm::dvec3 inner = ExplodeInnerOffset(state, occId, m.innerPath);
        inner != glm::dvec3(0.0))
      xf = glm::translate(glm::dmat4(1.0), inner) * xf;
    if (state.meshRenderer)
      state.meshRenderer->SetInstanceTransform(m.meshId, glm::mat4(xf));
    if (state.outlineRenderer)
      state.outlineRenderer->SetMeshTransform(m.outlineId, glm::mat4(xf));
    if (sequence) {
      if (state.meshRenderer)
        state.meshRenderer->SetInstanceVisible(m.meshId, !hidden);
      if (state.outlineRenderer && m.outlineId != 0)
        state.outlineRenderer->SetMeshVisible(m.outlineId, !hidden);
    }
  }
}

namespace {

// Publishes one axis's handles (arrow and/or arc) per the spec as
// ViewportHandles for the shared hover + click hit test AND the shared drawer
// (docs/ui/viewport-handles.md, ui/ViewportHandleDraw.h). Per-axis ids and
// per-axis colour, so hovering X highlights X rather than the whole gizmo.
// Nothing is drawn here: DrawViewportHandles draws the published record.
void PublishGizmoHandles(AppState& state, const GizmoSpec& spec, double L) {
  const double R = 0.78 * L;
  const auto publish = [&](int axis, bool rotate, double len) {
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

// ---- The placement gizmo on a JOB tab (docs/cam/gui.md#gizmos) ----
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
  glm::dvec3 lo(DBL_MAX), hi(-DBL_MAX);
  bool any = false;
  for (const Body& b : JobPlacedBodies(state)) {
    if (b.jobPartId != jobPartId)
      continue;
    glm::dvec3 bmin, bmax;
    b.shape.GetBoundingBox(bmin, bmax);
    lo = glm::min(lo, bmin);
    hi = glm::max(hi, bmax);
    any = true;
  }
  if (!any)
    return false;
  spec.origin = glm::dvec3((lo.x + hi.x) * 0.5, (lo.y + hi.y) * 0.5, hi.z);
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

bool HandlePlacementGizmo(AppState& state, const Ray& screenRay, bool clicked,
                          bool down, bool released) {
  const DRay ray = Widen(screenRay);
  auto& g = state.gizmoDrag;
  if (g.active) {
    cam::Placement* place = PlacementFor(state, g.jobPartId);
    if (released || !down || place == nullptr || !state.job) {
      g.active = false;
      g.jobPartId = 0;
      if (state.job) {
        // Where it landed is what the job cuts: the fit states, the
        // toolpaths and the verdict all follow the drop.
        MarkJobEdited(state);
        RefreshCamToolpaths(state);
        RunCamVerification(state);
      }
      return true;
    }
    const glm::dvec3 A = g.frameAxis[g.axis];
    // Snapping, unless Ctrl says free: a part's edge to the sheet's edges
    // and to the other parts' -- which is how parts are butted together on
    // a board -- and a spin to the multiples of 45 degrees. The threshold
    // scales with the gizmo, so it is the same on screen at any zoom.
    const bool snap = !ImGui::GetIO().KeyCtrl;
    if (!g.rotate) {
      const double t =
          RayAxisParam(ray.origin, ray.direction, g.frameOrigin, A);
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
          glm::dvec3 bmin, bmax;
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
      double ang;
      if (RotateAngle(ray.origin, ray.direction, g.frameOrigin, A, ang)) {
        const double deg =
            g.startRotZ +
            glm::degrees(static_cast<double>(UnwrappedTurn(g, ang)));
        place->rotZdeg = SnapAngleDeg(deg, snap);
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
  if (!GizmoHitTest(state, spec, screenRay, axis, rotate))
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
  g.startBoxMin = glm::dvec3(DBL_MAX);
  g.startBoxMax = glm::dvec3(-DBL_MAX);
  for (const Body& b : JobPlacedBodies(state)) {
    if (b.jobPartId != part)
      continue;
    glm::dvec3 bmin, bmax;
    b.shape.GetBoundingBox(bmin, bmax);
    g.startBoxMin = glm::min(g.startBoxMin, bmin);
    g.startBoxMax = glm::max(g.startBoxMax, bmax);
  }
  const glm::dvec3 A = spec.axis[axis];
  if (!rotate) {
    g.startParam = RayAxisParam(ray.origin, ray.direction, spec.origin, A);
  } else {
    double ang = 0.0;
    (void)RotateAngle(ray.origin, ray.direction, spec.origin, A, ang);
    g.startParam = ang;
    g.lastAngle = ang;
    g.turned = 0.0;
  }
  return true;
}

// ---- The fixture gizmo on a JOB tab (docs/cam/gui.md#gizmos) ----
//
// The fixture whose editor is open gets the handles a part gets: X, Y and Z
// arrows and a spin about Z (a fixture never tilts). It snaps its OWN flat
// faces to the stock's planes, and a fixture put down with a face on the
// stock's top RESTS there -- the seat is read from where it was put, which is
// the assembly mate idea without a separate tool to make one.

cam::Fixture* EditedFixture(AppState& state) {
  if (!state.job || !state.camToolOpen || state.camEditFixture == 0 ||
      state.slotPick.Active() || state.measure.active)
    return nullptr;
  cam::Setup* setup = ActiveCamSetup(state);
  return setup != nullptr ? setup->FindFixture(state.camEditFixture) : nullptr;
}

// The fixture as drawn, as a box: the model's own box (memoized on its shape)
// with the fixture's transform applied to its corners.
bool FixtureDrawnBox(AppState& state, const cam::Fixture& f, glm::dvec3& lo,
                     glm::dvec3& hi) {
  const glm::dmat4 m(cam::FixtureTransform(f));
  lo = glm::dvec3(DBL_MAX);
  hi = glm::dvec3(-DBL_MAX);
  bool any = false;
  for (const Body& b : FixtureModelBodies(state, f)) {
    glm::dvec3 bmin, bmax;
    b.shape.GetBoundingBox(bmin, bmax);
    for (int c = 0; c < 8; c++) {
      const glm::dvec3 p((c & 1) ? bmax.x : bmin.x, (c & 2) ? bmax.y : bmin.y,
                         (c & 4) ? bmax.z : bmin.z);
      const glm::dvec3 w(m * glm::dvec4(p, 1.0));
      lo = glm::min(lo, w);
      hi = glm::max(hi, w);
    }
    any = true;
  }
  return any;
}

bool BuildFixtureGizmoSpec(AppState& state, const cam::Fixture& f,
                           GizmoSpec& spec) {
  glm::dvec3 lo, hi;
  if (!FixtureDrawnBox(state, f, lo, hi))
    return false;
  spec.origin = glm::dvec3((lo.x + hi.x) * 0.5, (lo.y + hi.y) * 0.5, hi.z);
  spec.axis[0] = {1, 0, 0};
  spec.axis[1] = {0, 1, 0};
  spec.axis[2] = {0, 0, 1};
  spec.trans[0] = spec.trans[1] = spec.trans[2] = true;
  spec.rot[0] = spec.rot[1] = false;
  spec.rot[2] = true;
  return true;
}

// "Pad's underside", "Bar's +X face": what the snap label calls a face.
std::string FaceWords(const Body& b, int axis, int facing) {
  static const char* const kSide[3][2] = {
      {"-X face", "+X face"}, {"-Y face", "+Y face"}, {"underside", "top"}};
  const std::string whose = b.name.empty() ? std::string("its") : b.name + "'s";
  return whose + " " + kSide[axis][facing > 0 ? 1 : 0];
}

bool HandleFixtureGizmo(AppState& state, const Ray& screenRay, bool clicked,
                        bool down, bool released) {
  const DRay ray = Widen(screenRay);
  auto& g = state.gizmoDrag;
  if (g.active && g.fixtureId == 0)
    return false; // a placement drag, not ours
  cam::Setup* setup = ActiveCamSetup(state);
  if (g.active) {
    cam::Fixture* f =
        setup != nullptr ? setup->FindFixture(g.fixtureId) : nullptr;
    if (released || !down || f == nullptr || !state.job) {
      if (f != nullptr && setup != nullptr) {
        // Put down: resting on the stock's top by a face, it is SEATED there
        // and follows the stock; anywhere else it stands on its own.
        glm::dvec3 lo(0.0), hi(0.0);
        const std::vector<Body> model = FixtureModelBodies(state, *f);
        f->seat.reset();
        if (!f->IsBox() && cam::StockBounds(*setup, JobBodies(state), lo, hi)) {
          const ElementRef face =
              cam::FaceOnStockTop(lo, hi, model, f->pos, f->rotZdeg);
          cam::FixtureSeat seat;
          if (face.IsSet() && cam::SeatFromPose(lo, hi, model, face, f->pos,
                                                f->rotZdeg, seat, nullptr))
            f->seat = seat;
        }
        CamFixturesChanged(state, *setup);
      }
      g.active = false;
      g.fixtureId = 0;
      g.snapLabel.clear();
      for (auto& planes : g.facePlanes)
        planes.clear();
      return true;
    }
    const glm::dvec3 A = g.frameAxis[g.axis];
    const bool snap = !ImGui::GetIO().KeyCtrl;
    g.snapLabel.clear();
    if (!g.rotate) {
      const double t =
          RayAxisParam(ray.origin, ray.direction, g.frameOrigin, A);
      double d = static_cast<double>(t - g.startParam);
      glm::dvec3 slo(0.0), shi(0.0);
      if (snap && cam::StockBounds(*setup, JobBodies(state), slo, shi)) {
        // Its faces square to this axis against the stock's planes on it:
        // contact or flush, whichever is nearer -- a clamp's pad onto the
        // top, a stop against a side. Same on screen at any zoom, and never
        // less than a percent of the stock, as the placement snap.
        const int ax = g.axis;
        const double threshold =
            std::max(0.15 * GizmoScale(state),
                     0.01 * std::max(shi.x - slo.x, shi.y - slo.y));
        static const char* const kPlane[3][2] = {
            {"the stock's -X side", "the stock's +X side"},
            {"the stock's -Y side", "the stock's +Y side"},
            {"the bed", "the stock top"}};
        const double planes[2] = {slo[ax], shi[ax]};
        double best = threshold, shift = 0.0;
        std::string label;
        for (const auto& fp : g.facePlanes[ax])
          for (int k = 0; k < 2; k++) {
            const double gap = planes[k] - (fp.at + d);
            if (std::abs(gap) < best) {
              best = std::abs(gap);
              shift = gap;
              label = fp.what + " on " + kPlane[ax][k];
            }
          }
        d += shift;
        g.snapLabel = label;
      }
      f->pos = g.startFixturePos + glm::dvec3(A) * d;
    } else {
      double ang;
      if (RotateAngle(ray.origin, ray.direction, g.frameOrigin, A, ang)) {
        const double deg = SnapAngleDeg(
            g.startRotZ +
                glm::degrees(static_cast<double>(UnwrappedTurn(g, ang))),
            snap);
        // About the handle, not the model's origin: the fixture turns where
        // the arc is drawn, so its origin swings round that point.
        const double delta = glm::radians(deg - g.startRotZ);
        const glm::dvec2 c(g.frameOrigin.x, g.frameOrigin.y);
        const glm::dvec2 r(g.startFixturePos.x - c.x,
                           g.startFixturePos.y - c.y);
        const double cs = std::cos(delta), sn = std::sin(delta);
        f->pos = glm::dvec3(c.x + cs * r.x - sn * r.y,
                            c.y + sn * r.x + cs * r.y, g.startFixturePos.z);
        f->rotZdeg = deg;
      }
    }
    // Drawn from its pose on the next sync: JobRenderVersion keys on it.
    state.job->BumpVersion();
    return true;
  }

  cam::Fixture* f = EditedFixture(state);
  if (f == nullptr || !clicked)
    return false;
  GizmoSpec spec;
  if (!BuildFixtureGizmoSpec(state, *f, spec))
    return false;
  int axis = 0;
  bool rotate = false;
  if (!GizmoHitTest(state, spec, screenRay, axis, rotate))
    return false;
  SnapshotJob(state); // one undo step per drag (R7)
  // Free while it moves: a seat would re-solve it back every sync. Where it is
  // put down decides whether it rests on the stock again.
  f->seat.reset();
  g = {};
  g.active = true;
  g.axis = axis;
  g.rotate = rotate;
  g.frameOrigin = spec.origin;
  for (int i = 0; i < 3; i++)
    g.frameAxis[i] = spec.axis[i];
  g.fixtureId = f->id;
  g.startFixturePos = f->pos;
  g.startRotZ = f->rotZdeg;
  // Its flat faces as placed, once: the snap reads them every frame.
  std::vector<Body> placed = FixtureModelBodies(state, *f);
  const glm::dmat4 m(cam::FixtureTransform(*f));
  for (Body& b : placed)
    b.shape = b.shape.Transformed(m);
  const auto planes = cam::AxisFacePlanes(placed);
  for (int a = 0; a < 3; a++)
    for (const cam::AxisFacePlane& p : planes[a])
      g.facePlanes[a].push_back({p.at, FaceWords(placed[p.body], a, p.facing)});
  const glm::dvec3 A = spec.axis[axis];
  if (!rotate) {
    g.startParam = RayAxisParam(ray.origin, ray.direction, spec.origin, A);
  } else {
    double ang = 0.0;
    (void)RotateAngle(ray.origin, ray.direction, spec.origin, A, ang);
    g.startParam = ang;
    g.lastAngle = ang;
    g.turned = 0.0;
  }
  return true;
}

// A drag of a MOVING PART of a flexible placement (v129), in progress: the
// same two modes as a top-level part -- an exploded view's step, or the
// placement's own pose of the sub-assembly mate that places it, written
// through SetMateDofAt (set_mate {id, path}'s write) and solved holding it,
// so the unit's mechanism and whatever the parent chains to it follow.
bool DragInnerPart(AppState& state, const Ray& screenRay, bool down,
                   bool released) {
  const DRay ray = Widen(screenRay);
  auto& g = state.gizmoDrag;
  Occurrence* F =
      state.assembly ? state.assembly->FindById(g.placement) : nullptr;
  const AssemblyDocument* sub = F ? FlexibleSub(state, *F) : nullptr;
  const Mate* mate = sub && g.mateId ? sub->FindMateById(g.mateId) : nullptr;
  const std::vector<uint64_t> path{g.placement};
  const MateDof dof = g.rotate ? MateDof::Rotation : MateDof::Offset;
  if (released || !down || sub == nullptr || (!g.explodeView && !mate)) {
    const bool explode = g.explodeView != 0;
    g.active = false;
    g.innerOcc = g.placement = 0;
    if (state.assembly) {
      if (explode)
        state.assembly->BumpVersion(); // view edit: resync, nothing to solve
      else if (mate)
        (void)SolveActiveAssemblyReport(state, {mate->id, dof, path});
      else
        SolveActiveAssembly(state);
    }
    return true;
  }
  const glm::dvec3 A = g.frameAxis[g.axis];
  if (g.explodeView) {
    ExplodedView* view = state.assembly->FindExplodedViewById(g.explodeView);
    ExplodeStep* step = nullptr;
    if (view)
      for (ExplodeStep& s : view->steps)
        if (s.occurrenceId == g.innerOcc && s.path == path) {
          step = &s;
          break;
        }
    if (!step) { // view or step vanished mid-drag (undo?) — abort cleanly
      g.active = false;
      g.innerOcc = g.placement = 0;
      return true;
    }
    const double t = RayAxisParam(ray.origin, ray.direction, g.frameOrigin, A);
    const double f = std::max(double(state.explodeFactor), 0.05);
    step->offset = g.startOffset + A * ((t - g.startParam) / f);
    PushOccurrenceLive(state, F->id);
    return true;
  }
  // A DOF the placement's configuration holds is left alone, as a top-level
  // hold is: the drag would pose what that configuration pins.
  if (const Configuration* c = sub->ConfigurationFor(F->configurationId))
    if (const MateOverride* hold = c->OverrideForMate(mate->id);
        hold != nullptr &&
        (g.rotate ? hold->rotation.has_value() : hold->offset.has_value()))
      return true;
  const bool snap = !ImGui::GetIO().KeyCtrl;
  double value = 0.0;
  if (!g.rotate) {
    const double t = RayAxisParam(ray.origin, ray.direction, g.frameOrigin, A);
    value = g.startDof + (t - g.startParam);
  } else {
    double ang;
    if (!RotateAngle(ray.origin, ray.direction, g.frameOrigin, A, ang))
      return true;
    // The absolute value snaps, as a top-level joint's does: the mate's own
    // frame is the zero.
    value = SnapAngleRad(g.startDof + UnwrappedTurn(g, ang), snap);
  }
  assembly::SetMateDofAt(*state.assembly, path, mate->id, dof, value,
                         AssemblyResolverFor(state));
  (void)SolveActiveAssemblyReport(state, {mate->id, dof, path});
  return true; // solve already repositioned + resynced
}

} // namespace

void PublishGizmoHandles(AppState& state) {
  if (state.job) {
    // The fixture being edited takes the handles; otherwise a picked part.
    if (const cam::Fixture* f = EditedFixture(state)) {
      GizmoSpec spec;
      if (BuildFixtureGizmoSpec(state, *f, spec))
        PublishGizmoHandles(state, spec, GizmoScale(state));
      return;
    }
    const uint64_t part = PlacementGizmoPart(state);
    GizmoSpec spec;
    if (part != 0 && BuildPlacementGizmoSpec(state, part, spec))
      PublishGizmoHandles(state, spec, GizmoScale(state));
    return;
  }
  uint64_t inner = 0;
  Occurrence* occ = GizmoOccurrence(state, inner);
  if (!occ)
    return;
  GizmoSpec spec;
  const Mate* mate = nullptr;
  if (!BuildGizmoSpec(state, occ, inner, spec, mate))
    return;
  PublishGizmoHandles(state, spec, GizmoScale(state));
}

bool HandleGizmoInput(AppState& state, const Ray& screenRay, bool clicked,
                      bool down, bool released) {
  const DRay ray = Widen(screenRay);
  if (state.job)
    return HandleFixtureGizmo(state, screenRay, clicked, down, released) ||
           HandlePlacementGizmo(state, screenRay, clicked, down, released);
  auto& g = state.gizmoDrag;

  if (g.active && g.innerOcc != 0)
    return DragInnerPart(state, screenRay, down, released);
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
    const glm::dvec3 A = g.frameAxis[g.axis];
    if (g.explodeView) {
      // Explode authoring: the drag writes the step's offset so that the
      // DISPLAYED position follows the mouse (offset scales by 1/factor).
      ExplodedView* view = state.assembly->FindExplodedViewById(g.explodeView);
      ExplodeStep* step = nullptr;
      if (view)
        for (ExplodeStep& s : view->steps)
          if (s.path.empty() && s.occurrenceId == occ->id) {
            step = &s;
            break;
          }
      if (!step) { // view or step vanished mid-drag (undo?) — abort cleanly
        g.active = false;
        return true;
      }
      const double t =
          RayAxisParam(ray.origin, ray.direction, g.frameOrigin, A);
      const double f =
          std::max(double(state.explodeFactor), 0.05); // never divide by ~0
      step->offset = g.startOffset + A * ((t - g.startParam) / f);
      PushOccurrenceLive(state, occ->id);
      return true;
    }
    // A turn snaps to the common angles unless Ctrl says free — assemblies
    // are right angles far more often than not, and the mouse cannot hit one
    // exactly. Translation is deliberately left free: what a part should butt
    // against is a mate, not a round number.
    const bool snap = !ImGui::GetIO().KeyCtrl;
    if (!g.mateId) {
      // Free occurrence: drag its transform directly.
      if (!g.rotate) {
        const double t =
            RayAxisParam(ray.origin, ray.direction, g.frameOrigin, A);
        occ->position = g.startPos + A * (t - g.startParam);
      } else {
        double ang;
        // The TURN is what snaps here: a free occurrence's starting
        // orientation is arbitrary, so only the increment has a zero to
        // measure from.
        if (RotateAngle(ray.origin, ray.direction, g.frameOrigin, A, ang))
          occ->orientation =
              glm::angleAxis(SnapAngleRad(UnwrappedTurn(g, ang), snap), A) *
              g.startRot;
      }
    } else {
      // Drag-within-mate: write the mate's DOF, then re-solve so the
      // occurrence follows. A DOF the active configuration holds is left
      // alone: the drag would write the AUTHORED value underneath the hold,
      // editing another configuration's mate from this one.
      if (const MateOverride* hold = state.assembly->MateOverrideFor(mate->id);
          hold != nullptr && (g.rotate ? hold->rotation.has_value()
                                       : (g.axis == 2 && hold->offset)))
        return true;
      if (!g.rotate) {
        const double t =
            RayAxisParam(ray.origin, ray.direction, g.frameOrigin, A);
        const double v = g.startDof + (t - g.startParam);
        if (g.axis == 0)
          mate->dx = v;
        else if (g.axis == 1)
          mate->dy = v;
        else
          mate->offset = v;
      } else {
        double ang;
        if (RotateAngle(ray.origin, ray.direction, g.frameOrigin, A, ang)) {
          const double d = UnwrappedTurn(g, ang);
          // Here the ABSOLUTE value snaps: the mate's own frame is the zero,
          // so a joint lands on 90 degrees from its seat rather than 90 from
          // wherever this particular drag was grabbed. Ball has no single
          // angle, so it snaps the turn like a free occurrence.
          if (mate->type == MateType::Ball)
            mate->ballRotation =
                glm::angleAxis(SnapAngleRad(d, snap), A) * g.startBall;
          else
            mate->rotation = SnapAngleRad(g.startDof + d, snap);
        }
      }
      // The dragged mate drives its relations (a gear turns its partner).
      (void)assembly::PropagateRelations(*state.assembly, mate->id,
                                         ui::PartResolverFor(state));
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
        assembly::ApplyAttachments(*state.assembly, PartResolverFor(state),
                                   AssemblyResolverFor(state));
        break;
      }
    PushOccurrenceLive(state, occ->id);
    for (const Occurrence& rider : state.assembly->GetOccurrences())
      if (rider.attachedTo == occ->id)
        PushOccurrenceLive(state, rider.id);
    return true;
  }

  uint64_t inner = 0;
  Occurrence* occ = GizmoOccurrence(state, inner);
  if (!occ || !clicked)
    return false;
  GizmoSpec spec;
  const Mate* mate = nullptr;
  if (!BuildGizmoSpec(state, occ, inner, spec, mate))
    return false;
  int axis = 0;
  bool rotate = false;
  if (!GizmoHitTest(state, spec, screenRay, axis, rotate))
    return false;

  SnapshotAssembly(state); // one snapshot per drag
  g.active = true;
  g.axis = axis;
  g.rotate = rotate;
  g.frameOrigin = spec.origin;
  for (int i = 0; i < 3; i++)
    g.frameAxis[i] = spec.axis[i];
  g.mateId = mate ? mate->id : 0;
  g.innerOcc = inner;
  g.placement = inner != 0 ? occ->id : 0;
  g.explodeView = state.activeExplodedView;
  if (g.explodeView) {
    // Find-or-create this occurrence's step in the active view (after the
    // snapshot, so the whole drag is one undo step). A moving part of a
    // flexible unit has its own, named by path (v130).
    ExplodedView* view = state.assembly->FindExplodedViewById(g.explodeView);
    const std::vector<uint64_t> path =
        inner != 0 ? std::vector<uint64_t>{occ->id} : std::vector<uint64_t>{};
    const uint64_t stepOcc = inner != 0 ? inner : occ->id;
    ExplodeStep* step = nullptr;
    for (ExplodeStep& s : view->steps)
      if (s.occurrenceId == stepOcc && s.path == path) {
        step = &s;
        break;
      }
    if (!step) {
      view->steps.push_back({stepOcc, glm::dvec3(0.0), path});
      step = &view->steps.back();
    }
    g.startOffset = step->offset;
  }
  g.startPos = occ->position;
  g.startRot = occ->orientation;
  const glm::dvec3 A = spec.axis[axis];
  if (!rotate)
    g.startParam = RayAxisParam(ray.origin, ray.direction, spec.origin, A);
  else {
    double ang = 0.0;
    RotateAngle(ray.origin, ray.direction, spec.origin, A, ang);
    g.startParam = ang;
    g.lastAngle = ang;
    g.turned = 0.0;
  }
  if (mate && inner != 0) { // this placement's pose of the inner mate
    g.startDof =
        assembly::MateDofAt(*state.assembly, {occ->id}, mate->id,
                            rotate ? MateDof::Rotation : MateDof::Offset,
                            AssemblyResolverFor(state))
            .value_or(0.0);
  } else if (mate) { // capture the DOF value at grab
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
    glm::dvec3 center;
  };
  std::vector<OccCenter> centers;
  glm::dvec3 lo(DBL_MAX), hi(-DBL_MAX);
  // A5: bound each TOP-LEVEL occurrence's whole subtree. A sub-assembly
  // explodes as one unit — spacing it by the bounds of its first part would
  // send a gangway the distance a single board deserves.
  std::map<uint64_t, std::pair<glm::dvec3, glm::dvec3>> boxes;
  for (const assembly::FlatInstance& inst : FlattenActiveAssembly(state)) {
    if (!inst.visible)
      continue;
    for (const auto& body : inst.part->GetBodies()) {
      if (body.reference)
        continue; // a ghost does not set the assembly's extent
      glm::dvec3 mn, mx;
      body.shape.GetBoundingBox(mn, mx);
      for (int c = 0; c < 8; c++) {
        const glm::dvec3 corner((c & 1) ? mx.x : mn.x, (c & 2) ? mx.y : mn.y,
                                (c & 4) ? mx.z : mn.z);
        const glm::dvec3 w = glm::dvec3(inst.world * glm::dvec4(corner, 1.0));
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
    centers.push_back({occId, (box.first + box.second) * 0.5});
  if (centers.size() < 2)
    return false; // nothing to spread apart

  glm::dvec3 centroid(0.0);
  for (const OccCenter& c : centers)
    centroid += c.center;
  centroid /= static_cast<double>(centers.size());
  const double spread = 0.6 * glm::length(hi - lo);

  SnapshotAssembly(state);
  view->steps.clear();
  for (size_t i = 0; i < centers.size(); i++) {
    glm::dvec3 dir = centers[i].center - centroid;
    if (glm::length(dir) < 1e-6)
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
