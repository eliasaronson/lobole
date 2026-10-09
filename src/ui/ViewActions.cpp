// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/ViewActions.h"
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

// What the camera looks at, and what is cut away: fitting to a document, a
// selection or a box, and the section views -- their planes, their source
// face picks, and the filled cap meshes a cut leaves behind.
// Declared in ui/ViewActions.h; the layer is described in
// docs/architecture.md#the-ui-actions-layer.

namespace ui {

namespace {
// Aim the camera at a world-space box: the shared tail of the document and
// selection fits.
void FitCameraToBox(AppState& state, const glm::vec3& min, const glm::vec3& max,
                    float forAspect = 0.0f) {
  const glm::vec3 center = (min + max) * 0.5f;

  // The vertical half-FOV is fixed; the horizontal one is tan(fovY/2)*aspect,
  // so in a portrait-ish viewport (aspect < 1 — e.g. the central region of a
  // tall tiled window) the width is what gates the fit. The old radius*2.5
  // constant was exactly the vertical fit for the 45-degree FOV and silently
  // assumed a landscape viewport.
  Camera& cam = *state.camera;
  const float tanHalfV = std::tan(0.5f * glm::radians(cam.GetFOV()));
  // The aspect of the picture being framed, which is NOT always the
  // viewport's: `render_still` projects with the size it was asked for
  // (RendererSync's MakeStillDesc), so a fit solved for the viewport crops
  // the still whenever the two differ — a fifth of a 210 mm panel in frame
  // at 400x1100, correctly projected and framed for a different picture
  // (docs/rendering/still-render.md#framing).
  float aspect = forAspect > 0.0f ? forAspect : cam.GetAspectRatio();
  if (!(aspect > 0.0f))
    aspect = 1.0f;

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
  // (docs/ui/README.md#viewport).
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
    // verbs open, docs/ui/README.md#viewport).
    const glm::vec3 right = cam.GetRight();
    const float halfWidthWorld = distance * tanHalfV * aspect;
    // Off the SOLVED aim, not the box centre — the fit above re-centred the
    // projection, and re-deriving from the centre would throw that away.
    cam.SetPivot(target + right * (halfWidthWorld * coveredFrac));
  }
}
} // namespace

bool VisibleSceneBounds(AppState& state, glm::vec3& outMin, glm::vec3& outMax) {
  glm::dvec3 min(DBL_MAX), max(-DBL_MAX);
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
      const glm::dmat4 xf =
          glm::translate(glm::dmat4(1.0),
                         ExplodeOffset(state, inst.rootOccurrenceId) +
                             ExplodeInnerOffset(state, inst.rootOccurrenceId,
                                                inst.innerIds)) *
          inst.world;
      for (const auto& body : part->GetBodies()) {
        glm::dvec3 bmin, bmax;
        body.shape.GetBoundingBox(bmin, bmax);
        for (int c = 0; c < 8; c++) { // transform all 8 corners
          const glm::dvec3 corner((c & 1) ? bmax.x : bmin.x,
                                  (c & 2) ? bmax.y : bmin.y,
                                  (c & 4) ? bmax.z : bmin.z);
          const glm::dvec3 w = glm::dvec3(xf * glm::dvec4(corner, 1.0));
          min = glm::min(min, w);
          max = glm::max(max, w);
          any = true;
        }
      }
    }
  } else if (state.document) {
    for (const auto& body : state.document->GetBodies()) {
      glm::dvec3 bmin, bmax;
      body.shape.GetBoundingBox(bmin, bmax);
      min = glm::min(min, bmin);
      max = glm::max(max, bmax);
      any = true;
    }
  } else if (state.job) {
    // A job tab draws the parts LAID OUT, so the camera has to frame the
    // layout rather than one part -- otherwise the first look at a sheet is
    // from inside whichever part happens to sit at the origin. Their TIGHT
    // boxes, which the placed bodies already hold (JobPlacedBodies warms
    // them): the conservative one bounds a turned blank's B-splines by their
    // control points, and the chess jig's view was framed round a piece that
    // much bigger than it is, the jig pushed up against the top edge.
    for (const Body& body : JobPlacedBodies(state)) {
      glm::dvec3 bmin, bmax;
      body.shape.GetBoundingBox(bmin, bmax, /*tight=*/true);
      min = glm::min(min, bmin);
      max = glm::max(max, bmax);
      any = true;
    }
    // ...and the stock they are cut from, which is drawn round them and is
    // what a person is setting up.
    glm::dvec3 stockLo(0.0), stockHi(0.0);
    if (CamStockBox(state, stockLo, stockHi)) {
      min = glm::min(min, glm::dvec3(stockLo));
      max = glm::max(max, glm::dvec3(stockHi));
      any = true;
    }
    // ...and what holds them, which is drawn too. The MODEL shape's box
    // (memoized on it) with the fixture's transform applied to its corners,
    // as the assembly branch does: a transformed copy would be a new shape
    // with a cold memo, bounded afresh on every call.
    if (const cam::Setup* setup = ActiveJobSetup(state))
      for (const cam::Fixture& f : setup->fixtures) {
        const glm::dmat4 xf(cam::FixtureTransform(f));
        // A part's bodies by reference (this runs every frame under the
        // section tool, and a Body copy carries its element map); a box is
        // one small cached shape.
        const Document* doc =
            f.IsBox() ? nullptr : JobSourceDocument(state, f.sourceDocId);
        const std::vector<Body> box =
            f.IsBox() ? FixtureModelBodies(state, f) : std::vector<Body>{};
        for (const Body& body : doc != nullptr ? doc->GetBodies() : box) {
          if (body.reference || !body.shape.IsValid())
            continue;
          glm::dvec3 bmin, bmax;
          body.shape.GetBoundingBox(bmin, bmax, /*tight=*/true);
          for (int c = 0; c < 8; c++) {
            const glm::dvec3 corner((c & 1) ? bmax.x : bmin.x,
                                    (c & 2) ? bmax.y : bmin.y,
                                    (c & 4) ? bmax.z : bmin.z);
            const glm::dvec3 w = glm::dvec3(xf * glm::dvec4(corner, 1.0));
            min = glm::min(min, w);
            max = glm::max(max, w);
            any = true;
          }
        }
      }
  }
  // The camera and the section tool take float; the box narrows here.
  outMin = glm::vec3(min);
  outMax = glm::vec3(max);
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
      glm::dvec3 origin(0.0), normal(0.0), uAxis(0.0);
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
      glm::dvec3 o(0.0), n(0.0), u(0.0);
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
  return glm::vec4(n, glm::dot(glm::vec3(sk->plane.GetOrigin()), n));
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

void FitCameraToDocument(AppState& state, float forAspect) {
  if (!state.camera)
    return;
  glm::vec3 min, max;
  if (!VisibleSceneBounds(state, min, max))
    return;
  FitCameraToBox(state, min, max, forAspect);
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
      glm::dvec3 bmin, bmax;
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
        const Document* part = occ ? OccurrencePartDoc(state, *occ) : nullptr;
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
      const Document* part = occ ? OccurrencePartDoc(state, *occ) : nullptr;
      if (!part)
        break;
      const glm::mat4 xf = DisplayMatrix(state, *occ);
      for (const auto& body : part->GetBodies()) {
        glm::dvec3 bmin, bmax;
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
                     const glm::vec3& worldNormal,
                     const std::vector<float>& alphas, uint64_t occurrenceId) {
  const glm::vec3 n = worldNormal;
  const float eps = 1e-3f; // nudge to the kept side; invisible at mm scale
  for (size_t bi = 0; bi < caps.size(); bi++) {
    if (!caps[bi].IsValid())
      continue;
    CADShape::Mesh cm = caps[bi].GenerateMesh();
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
    // A cut glass body's cap is glass too — see-through at the body's own
    // alpha — not an opaque lid over the cut (after the colour, which
    // resets the alpha).
    if (bi < alphas.size() && alphas[bi] < 1.0f)
      state.meshRenderer->SetInstanceGlass(capId, alphas[bi]);
    state.sectionCapMeshes.push_back({capId, occurrenceId, bi});
  }
}

// Hoisted out of the anonymous namespace by the stage-3 split:
// SyncDocumentRenderers draws these caps and now lives in
// RendererSync.cpp, so the declaration is in AppActionsInternal.h.

void AppendSectionCaps(AppState& state, const std::vector<Body>& bodies,
                       const std::vector<glm::vec3>& bodyColors) {
  const ClipPlaneArray planes = SectionPlanes(state);
  std::vector<float> alphas;
  alphas.reserve(bodies.size());
  for (const Body& b : bodies)
    alphas.push_back(CADMaterials::ViewportAlpha(
        CADMaterials::MaterialLibrary::GetMaterialOrDefault(b.material)));
  for (size_t i = 0; i < planes.size(); i++) {
    const glm::vec3 nRaw(planes[i]);
    if (glm::length(nRaw) < 1e-6f)
      continue;
    const glm::vec3 n = glm::normalize(nRaw);
    const glm::vec3 planeOrigin = n * planes[i].w; // dot(origin,n) = w
    // Every OTHER section trims this cap; without that, two cuts leave each
    // other's fill hanging in space past the corner.
    std::vector<glm::dvec4> trim;
    for (size_t j = 0; j < planes.size(); j++)
      if (j != i && glm::length(glm::vec3(planes[j])) > 1e-6f)
        trim.push_back(planes[j]);
    AppendCapMeshes(state,
                    Measure::SectionCaps(bodies, planeOrigin, n, trim,
                                         &state.sectionCapCache),
                    bodyColors, glm::mat4(1.0f), n, alphas);
  }
}

} // namespace ui
