// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/RendererSync.h"
#include "ui/DocumentSlots.h"
#include "ui/MotionActions.h"
#include "ui/ProjectActions.h"
#include "ui/SceneOverlays.h"
#include "ui/SequencePlayback.h"
#include "ui/ViewActions.h"

#include "core/LightRig.h"
#include "core/Units.h"
#include "ui/AppActionsInternal.h" // SharedBody / PartRenderCacheStore
#include "ui/CamPanel.h"           // RefreshCamToolpaths (job undo)
#include "ui/CamTool.h"            // RunCamVerification (job undo)
#include "ui/JobActions.h"

#include "imgui.h" // IsAnyItemActive (the undo guard coalesces a drag)         // JobBodies / JobRenderVersion (a job tab)
#include <atomic>
#include <chrono>
#include <map>
#include <thread>
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
#include "render/Screenshot.h"
#include "render/SketchRenderer.h"
#include "ui/BodyAppearance.h"

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
#include "ui/BearingTool.h"   // SyncBearingGhost (SyncRenderersToDocument)
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
#include <unordered_set>

// Pushing the document at the renderers: meshes and outlines for the active
// tab, the geometry-delta overlay, the ray-traced scene, and a still render.
//
// No unit test covers this file. It is verified by rendering a fixed scene
// through the real GPU and diffing the PNG -- which is how the 2026-08-24
// round pinned the same code when it moved the renderer helpers.
// Declared in ui/RendererSync.h; the layer is described in
// docs/architecture.md#the-ui-actions-layer.

namespace ui {

namespace {
// A body's faces with looks of their own (v164), as the mesh renderer's
// runs over the tessellation's index buffer: one per run of triangles that
// share a look (FaceLookRuns), the overridden ones carrying the face's
// resolved look, the rest the instance's. Empty for a body with none.
std::vector<MeshLookRun>
MeshLookRunsFor(const Body& body, const BodyLook& look,
                const std::vector<uint32_t>& triangleFaces) {
  std::vector<MeshLookRun> out;
  for (const FaceLookRun& r : FaceLookRuns(body, triangleFaces)) {
    MeshLookRun m;
    m.firstIndex = r.firstTriangle * 3;
    m.indexCount = r.triangleCount * 3;
    if (r.face != nullptr) {
      const CADMaterials::MaterialProperties& mat =
          ResolveFaceLook(look, r.face->look).mat;
      m.own = true;
      m.baseColor = mat.baseColor;
      m.materialParams =
          glm::vec4(mat.metallic, mat.roughness, mat.specular, mat.reflectance);
      m.procParams =
          glm::vec4(static_cast<float>(static_cast<int>(mat.pattern)),
                    mat.patternScale, mat.patternContrast, 0.0f);
    }
    out.push_back(m);
  }
  return out;
}
} // namespace

void SetGeometryDelta(AppState& state, const CADShape& missing,
                      const CADShape& extra, const std::string& summary) {
  state.deltaMissingShape = missing;
  state.deltaExtraShape = extra;
  state.deltaSummary = summary;
  state.deltaOwner = state.document.get();
  state.deltaVersion = state.document ? state.document->Version() : 0;
  state.syncedDocumentVersion = UINT64_MAX; // force a renderer resync
}

void ClearGeometryDelta(AppState& state) {
  if (!state.HasGeometryDelta() && state.deltaOwner == nullptr)
    return;
  state.deltaMissingShape = CADShape();
  state.deltaExtraShape = CADShape();
  state.deltaSummary.clear();
  state.deltaOwner = nullptr;
  state.deltaVersion = 0;
  state.syncedDocumentVersion = UINT64_MAX;
}

void SyncGeometryDelta(AppState& state) {
  if (!state.meshRenderer)
    return; // headless: the delta is still recorded, just not drawn
  // Remove the old instances FIRST, unconditionally — the same rule the
  // topology ghost learned: an overlay tracked apart from documentMeshes
  // survives a tab switch and haunts a part it was never measured on.
  for (uint32_t* inst :
       {&state.deltaMissingInstance, &state.deltaExtraInstance}) {
    if (*inst != 0) {
      state.meshRenderer->RemoveInstance(*inst);
      *inst = 0;
    }
  }
  if (!state.HasGeometryDelta())
    return;
  // A diff is only true of the shape it was measured on. Different document,
  // or the same one edited since, and the honest thing is to stop drawing it.
  if (state.deltaOwner != state.document.get() ||
      (state.document && state.document->Version() != state.deltaVersion)) {
    ClearGeometryDelta(state);
    return;
  }

  const auto add = [&](const CADShape& shape, const glm::vec3& color) {
    if (!shape.IsValid())
      return uint32_t{0};
    CADShape::Mesh mesh = shape.GenerateMesh();
    if (mesh.vertices.empty())
      return uint32_t{0};
    const uint32_t id = state.meshRenderer->AddMesh(
        CADMeshConverter::ConvertCADMeshToVertices(mesh), mesh.indices);
    state.meshRenderer->SetMeshColor(id, color);
    // Translucent so the part stays readable underneath: the delta is an
    // annotation ON the model, not a replacement for it.
    state.meshRenderer->SetInstanceGhost(id, 0.72f);
    return id;
  };
  // Pre-darkened for the bright lighting rig, the same lesson the wood
  // materials learned: a nominal 0.9 red renders as pale pink here, and a
  // delta you have to squint at is not an answer.
  state.deltaMissingInstance =
      add(state.deltaMissingShape, glm::vec3(0.45f, 0.02f, 0.02f));
  state.deltaExtraInstance =
      add(state.deltaExtraShape, glm::vec3(0.02f, 0.35f, 0.06f));
}

// Which active tab drives the sync, and its change counter. Empty = no tab
// whose geometry the renderers mirror. NOT a sentinel value: a fresh document
// and a fresh drawing are both at version 0, and reading that as "nothing to
// sync" would skip the resync that CLEARS the previous tab's meshes.
static std::optional<uint64_t> ActiveRenderVersion(AppState& state) {
  if (state.assembly)
    return state.assembly->Version();
  if (state.drawing)
    return state.drawing->Version();
  if (state.document)
    return state.document->Version();
  if (state.job)
    // A job's own version, mixed with every source part's: the job draws
    // OTHER documents' bodies, so a model edit in one of them has to reach
    // the viewport (ui::JobBodies keeps the same key).
    return JobRenderVersion(state);
  return std::nullopt;
}

bool RenderMeshesCurrent(AppState& state) {
  if (!state.meshRenderer)
    return false;
  const std::optional<uint64_t> active = ActiveRenderVersion(state);
  return active && *active == state.syncedDocumentVersion;
}

void SyncDocumentRenderers(AppState& state) {
  // The section cuts reach the PICK from here, every frame and ahead of both
  // gates below: the cut is a fragment discard, so without them a pick lands
  // on the material it removed — you section an assembly to reach a part
  // inside and every hover answers with the housing you just cut open
  // (docs/ui/selection.md#picking-through-a-section). Ahead of the gates
  // because the planes move live while the cap GEOMETRY deliberately settles on
  // release, and a picture and a pick that disagree is the bug itself.
  if (state.faceSelector)
    state.faceSelector->SetClipPlanes(SectionPlanes(state));
  if (!state.meshRenderer)
    return;
  const std::optional<uint64_t> active = ActiveRenderVersion(state);
  if (!active)
    return;
  const uint64_t activeVersion = *active;
  // Section caps (C4): a change to the clip plane forces a rebuild so the
  // filled cut faces follow it. Gated on nothing being INTERACTED with, so
  // caps settle on release rather than every drag frame — the shader clip
  // previews live regardless (main.cpp pushes SectionPlanes every frame), so
  // the only thing deferred is the filled cut face.
  //
  // `anyWidgetActive` is ImGui::IsAnyItemActive() sampled by the frame loop
  // at exactly this point (main.cpp, just before this call) — the indirection
  // keeps this file free of ImGui without changing what is read or when. It
  // covers the tool window's offset slider, which is what
  // this gate was written for. It does NOT cover the viewport ARROW: that is
  // not an ImGui item, so an arrow drag rebuilt every frame — and a rebuild
  // removes and re-adds every mesh and recomputes SectionCaps (an OCC
  // half-space boolean) for every body. One body on a part tab: unnoticeable.
  // Every part of an assembly, sixty times a second: the drag crawled, on any
  // GPU.
  const bool interacting = state.anyWidgetActive || state.handleDrag.active;
  if (SectionPlanes(state) != state.syncedSectionPlanes && !interacting) {
    state.syncedSectionPlanes = SectionPlanes(state);
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
  for (const AppState::SectionCapMesh& cap : state.sectionCapMeshes)
    state.meshRenderer->RemoveMesh(cap.meshId);
  state.sectionCapMeshes.clear();
  state.sectionCapLeaves.clear();
  // So are a job's fixtures.
  for (const uint32_t id : state.fixtureInstances)
    state.meshRenderer->RemoveInstance(id);
  state.fixtureInstances.clear();
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

  // A JOB tab draws the parts LAID OUT: each one where its placement puts
  // it, in the setup's own frame. That is the picture the whole document
  // kind is for -- parts arranged on a piece of stock -- and it is the same
  // list the toolpath is generated from, so what you see is what is cut.
  size_t palette = 0;
  const std::vector<Body>& bodies = ViewportBodies(state);
  // One submit+wait for every asset this sync uploads, as the assembly sync
  // does: without it each new body drains the queue on its own.
  if (state.meshRenderer)
    state.meshRenderer->BeginAssetBatch();
  if (state.outlineRenderer)
    state.outlineRenderer->BeginAssetBatch();
  std::vector<glm::vec3> bodyColors(bodies.size(), glm::vec3(0.7f));
  // The same phase breakdown the assembly sync logs, for the same reason:
  // this loop is the app's stall on a part with many bodies (a derived
  // 197-body camera), and a guess about which phase costs what has been
  // wrong every time it has been made.
  using SyncClock = std::chrono::steady_clock;
  const auto syncStart = SyncClock::now();
  const auto msSince = [](SyncClock::time_point t) {
    return std::chrono::duration<double, std::milli>(SyncClock::now() - t)
        .count();
  };
  double tessMs = 0, uploadMs = 0, pickMs = 0;
  size_t drawnBodies = 0;
  for (size_t bi = 0; bi < bodies.size(); bi++) {
    const auto& body = bodies[bi];
    if (state.hiddenBodies.count(body.createdBy) != 0)
      continue; // hidden (body context menu): no mesh/outline/picking
    const auto tessStart = SyncClock::now();
    // Cached by shape identity: a body whose shape did not change keeps its
    // tessellation AND its uploaded asset across syncs and across tabs.
    const std::shared_ptr<BodyTessellation> tess =
        TessellationFor(state, body.shape);
    tessMs += msSince(tessStart);
    if (!tess)
      continue;
    drawnBodies++;
    const auto uploadStart = SyncClock::now();
    uint32_t meshId = state.meshRenderer->AddInstance(tess->meshAsset);
    uploadMs += msSince(uploadStart);
    // Albedo is per-instance state (not baked into vertices): palette color
    // by default; a body with an assigned material overrides it with the
    // material's base color and real PBR params (ui/BodyAppearance.h).
    const BodyLook look = ResolveBodyLook(body, palette);
    glm::vec3 albedo = look.palette;
    state.meshRenderer->SetMeshColor(meshId, albedo);
    if (look.hasMaterial) {
      const CADMaterials::MaterialProperties& mat = look.mat;
      state.meshRenderer->SetMeshMaterial(meshId, mat.baseColor,
                                          glm::vec4(mat.metallic, mat.roughness,
                                                    mat.specular,
                                                    mat.reflectance));
      // Procedural surface pattern (Flat, so unpatterned materials pass
      // through unchanged). An overridden color KEEPS the pattern — that is
      // stained wood, and a painted carbon plate, the desired reading.
      state.meshRenderer->SetMeshProcedural(
          meshId, mat.pattern, mat.patternScale, mat.patternContrast);
      // Glass is drawn see-through, in the ghost pass (after the material:
      // setting one resets the alpha).
      if (mat.transmission > 0.0f)
        state.meshRenderer->SetInstanceGlass(meshId,
                                             CADMaterials::ViewportAlpha(mat));
      albedo = mat.baseColor;
    }
    bodyColors[bi] = albedo; // section caps reuse the body's color
    // Faces with looks of their own draw as runs. Not on a reference ghost:
    // it is one translucent tint, whatever its faces wear.
    if (!body.reference && !body.faceAppearances.empty())
      state.meshRenderer->SetInstanceLookRuns(
          meshId, MeshLookRunsFor(body, look, tess->triangleFaces));
    // A Derived reference body is drawn the way an in-context neighbour is:
    // the ghost tint, translucent, no outline — so what is another part's
    // geometry never reads as this part's. It stays pickable (that is what
    // it is for), through the face selector below.
    if (body.reference) {
      state.meshRenderer->SetMeshColor(meshId, glm::vec3(0.55f, 0.60f, 0.68f));
      state.meshRenderer->SetInstanceGhost(meshId, 0.35f);
    }
    uint32_t outlineId = 0;
    const auto outlineStart = SyncClock::now();
    if (state.outlineRenderer && !body.reference)
      outlineId =
          state.outlineRenderer->AddInstance(OutlineAssetFor(state, *tess));
    uploadMs += msSince(outlineStart);
    const auto pickStart = SyncClock::now();
    if (state.faceSelector)
      state.faceSelector->AddMesh(meshId, tess->vertices, tess->indices,
                                  tess->triangleFaces, glm::mat4(1.0f),
                                  tess->edges, tess->solid);
    pickMs += msSince(pickStart);
    state.documentMeshes.push_back({meshId,
                                    outlineId,
                                    bi,
                                    /*occurrenceId=*/0,
                                    glm::mat4(1.0f),
                                    /*leafPartId=*/0,
                                    {},
                                    tess->edges});
  }

  // A job's fixtures: what holds the stock, drawn ghosted where they sit on
  // the bed. Instanced from their part's own tessellation at the fixture's
  // transform, so moving a clamp re-meshes nothing. A disabled one (drawn,
  // not checked) is fainter.
  if (state.job)
    if (cam::Setup* setup = ActiveCamSetup(state)) {
      // A seat is solved against the stock as it is now, and the pose kept
      // on the fixture for the per-frame readers (docs/cam/setup.md#seats).
      // This sync is keyed on the stock's definition, so it runs
      // whenever the stock can have moved.
      RefreshFixtureSeats(state, *setup);
      for (const cam::Fixture& f : setup->fixtures) {
        const glm::mat4 xf(cam::FixtureTransform(f));
        for (const Body& b : FixtureModelBodies(state, f)) {
          const std::shared_ptr<BodyTessellation> tess =
              TessellationFor(state, b.shape);
          if (!tess)
            continue;
          const uint32_t id =
              state.meshRenderer->AddInstance(tess->meshAsset, xf);
          state.meshRenderer->SetMeshColor(id, glm::vec3(0.62f, 0.62f, 0.66f));
          state.meshRenderer->SetInstanceGhost(id, f.enabled ? 0.45f : 0.2f);
          state.fixtureInstances.push_back(id);
        }
      }
    }

  // Reference geometry is sized from the document's reach
  // (ScenePlaneHalfExtent), and the bounding box behind it is an OCC pass per
  // body. Stamp it HERE, on the resync that already walks every body and
  // tessellates it — a live edit bumps the version every frame, so computing it
  // on demand would put a bbox pass in the frame loop of exactly the drag that
  // can least afford one.
  {
    float reach = 0.0f;
    for (const auto& body : bodies) {
      glm::dvec3 mn, mx;
      body.shape.GetBoundingBox(mn, mx);
      for (int i = 0; i < 3; i++)
        reach =
            std::max({reach, float(std::abs(mn[i])), float(std::abs(mx[i]))});
    }
    state.sceneReachDoc = state.document.get();
    state.sceneReachVersion = activeVersion;
    state.sceneReach = reach;
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
  // A job tab has no document of its own, and a topology study belongs to a
  // part -- nothing to ghost here.
  if (state.topoGhostStudyId && !state.simulation.active && state.document) {
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

  SyncGeometryDelta(state);

  // In-context editing (A4): ghost the context assembly around the part.
  // Appended AFTER the part bodies so documentMeshes[i] still aligns with
  // GetBodies()[i] for the part-tab consumers (get_state, face picking).
  SyncInContextGhosts(state);

  // Sketch line geometry (static inactive sketches + the active sketch's
  // expanded primitives) is rebuilt per-frame in UI.cpp, since it must react to
  // edit-mode and selection/hover changes that don't bump the document version.

  if (state.meshRenderer)
    state.meshRenderer->EndAssetBatch();
  if (state.outlineRenderer)
    state.outlineRenderer->EndAssetBatch();
  TrimTessellationCache(state);
  const double totalMs = msSince(syncStart);
  if (state.job)
    LOG_INFO("Job synced: {} parts, {} bodies in {:.0f} ms [tessellate {:.0f}, "
             "instance {:.0f}, picking {:.0f}]",
             state.job->PartCount(), bodies.size(), totalMs, tessMs, uploadMs,
             pickMs);
  else
    LOG_INFO("Document synced: {} features, {} bodies ({} drawn) in {:.0f} ms "
             "[tessellate {:.0f}, instance {:.0f}, picking {:.0f}]",
             state.document->FeatureCount(), state.document->GetBodies().size(),
             drawnBodies, totalMs, tessMs, uploadMs, pickMs);
}

void SyncRenderersToDocument(AppState& state) {
  // A lost device takes no more uploads
  // (docs/rendering/README.md#a-lost-device): the document is CPU state and
  // stays whole; the projection waits for a restart that will never use this
  // device again.
  if (state.renderer.lost)
    return;
  SyncSketchViewRollback(state);
  SyncDocumentRenderers(state);
  SyncFieldRenderer(state);
  SyncCamPlayback(state);
  SyncFastenerGhost(state);
  SyncBearingGhost(state);
  // Last: it reads the meshes the first step just rebuilt. Before them, it
  // would highlight ids that no longer exist.
  SyncSelectionHighlights(state);
}

bool SyncRayTracingScene(AppState& state, bool groundPlane) {
  if (!state.rayTracer || !state.rayTracer->IsInitialized() ||
      state.renderer.lost)
    return false;
  if (!state.document && !state.assembly)
    return false;

  // What to render: a part's bodies at identity, or — on an assembly tab —
  // every visible occurrence's bodies at its SOLVED placement, moved by the
  // active explode as the viewport moves them: a still of an exploded view
  // was the assembled head, small in a frame `fit` had sized for the
  // exploded one (docs/rendering/still-render.md#framing).
  struct RTItem {
    const Body* body;
    glm::mat4 xf;
    // Which cached tessellation this body is, when there is one. partId 0
    // means "no cache entry to look for" (the part-tab branch below): the
    // cache is filled by the assembly sync and keyed on assembly part slots.
    uint64_t partId = 0;
    uint64_t configuration = 0;
    std::string material; // the placement's, overriding the body's
    uint64_t partVersion = 0;
    size_t bodyIndex = 0;
  };
  std::vector<RTItem> rtItems;
  // Section caps are computed per PART (Measure::SectionCaps takes a body
  // list and returns one cap each), so the cap pass below needs the parts as
  // parts -- rtItems has already been flattened to individual bodies.
  struct CapSource {
    const std::vector<Body>* bodies;
    glm::mat4 xf;
  };
  std::vector<CapSource> capSources;
  if (state.assembly) {
    // A5: the leaf parts of the whole tree — a nested assembly must appear in
    // the path-traced still as well as the viewport.
    for (const assembly::FlatInstance& inst : FlattenActiveAssembly(state)) {
      if (!inst.visible)
        continue;
      const auto& bodies = inst.part->GetBodies();
      // The explode, as the raster sync composes it: the top-level
      // occurrence's step, then a flexible unit's moving part's own.
      const glm::mat4 world(
          glm::translate(
              glm::dmat4(1.0),
              ExplodeInnerOffset(state, inst.rootOccurrenceId, inst.innerIds)) *
          glm::translate(glm::dmat4(1.0),
                         ExplodeOffset(state, inst.rootOccurrenceId)) *
          inst.world);
      for (size_t bi = 0; bi < bodies.size(); bi++)
        rtItems.push_back({&bodies[bi], world, inst.partId,
                           inst.configurationId, inst.material,
                           PartStamp(*inst.part), bi});
      capSources.push_back({&bodies, world});
    }
  } else if (state.job) {
    // A job tab ray-traces what it draws: the parts laid out.
    for (const Body& body : JobBodies(state))
      rtItems.push_back({&body, glm::mat4(1.0f)});
  } else if (state.document) {
    for (const auto& body : state.document->GetBodies())
      rtItems.push_back({&body, glm::mat4(1.0f)});
    capSources.push_back({&state.document->GetBodies(), glm::mat4(1.0f)});
  }

  state.rayTracer->ClearGeometry();
  state.rayTracer->ClearMaterials();

  // The raster sync already tessellated every one of these parts and the
  // result is still in AppState::partRenderCache. Reading it instead of
  // re-meshing turns a Render of a 299-part import from ~4 s of pure
  // duplicate OCC work into a copy — and it is a copy of exactly the geometry
  // on screen, so the still cannot disagree with the viewport.
  const auto cachedBodyFor =
      [&state](const RTItem& it) -> const BodyTessellation* {
    if (it.partId == 0 || !state.partRenderCache)
      return nullptr;
    const auto entry = state.partRenderCache->parts.find(
        PartRenderKey{it.partId, it.configuration, it.material});
    if (entry == state.partRenderCache->parts.end())
      return nullptr;
    // Version-checked, not merely present: a stale tessellation in a render
    // is the "correct-looking wrong answer" this codebase keeps paying for.
    if (entry->second.docVersion != it.partVersion ||
        it.bodyIndex >= entry->second.bodies.size())
      return nullptr;
    const SharedBody& sb = entry->second.bodies[it.bodyIndex];
    return sb.tess && !sb.tess->vertices.empty() ? sb.tess.get() : nullptr;
  };

  glm::vec3 bbMin(FLT_MAX), bbMax(-FLT_MAX);
  size_t reused = 0, meshed = 0;
  // Only an override with no colour on an unassigned body reads the palette
  // here; the count is the trace's own.
  size_t rtPalette = 0;
  for (const RTItem& it : rtItems) {
    const Body& body = *it.body;
    const BodyTessellation* cached = cachedBodyFor(it);
    CADShape::Mesh cadMesh;
    if (cached == nullptr) {
      cadMesh = body.shape.GenerateMesh();
      if (cadMesh.vertices.empty())
        continue;
      meshed++;
    } else {
      reused++;
    }
    const std::vector<uint32_t>& indices =
        cached != nullptr ? cached->indices : cadMesh.indices;

    // The look every raster path and export resolves (ui/BodyAppearance.h),
    // the placement's material wearing over the part's. A body with neither
    // a material nor an override traces as the DEFAULT material, as it
    // always has, not as its viewport palette colour.
    const BodyLook look = ResolveBodyLook(body, rtPalette, it.material);
    const CADMaterials::MaterialProperties mat =
        look.hasMaterial ? look.mat
                         : CADMaterials::MaterialLibrary::GetMaterialOrDefault(
                               std::string{});
    const std::vector<uint32_t>& triangleFaces =
        cached != nullptr ? cached->triangleFaces : cadMesh.triangleFaces;

    std::vector<Vertex3D> vertices =
        cached != nullptr ? cached->vertices
                          : CADMeshConverter::ConvertCADMeshToVertices(cadMesh);

    // The PART-LOCAL extent, measured BEFORE the occurrence transform is baked
    // in below — that is the frame PatternAxis wants, and the frame the raster
    // path measures in (an asset bbox, with the model matrix still in a push
    // constant). Measuring it after the bake gave a rotated occurrence a
    // different axis in a still than in the viewport.
    glm::vec3 localLo(FLT_MAX), localHi(-FLT_MAX);
    for (const Vertex3D& v : vertices) {
      localLo = glm::min(localLo, v.position);
      localHi = glm::max(localHi, v.position);
    }

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

    // Per-body WORLD bbox: feeds the scene bbox (the ground plane), so it is
    // taken after the bake above. The pattern axis uses localLo/localHi
    // instead — see there.
    glm::vec3 lo(FLT_MAX), hi(-FLT_MAX);
    for (const Vertex3D& v : vertices) {
      lo = glm::min(lo, v.position);
      hi = glm::max(hi, v.position);
    }
    bbMin = glm::min(bbMin, lo);
    bbMax = glm::max(bbMax, hi);

    const auto rtMaterialOf = [&](const CADMaterials::MaterialProperties& m) {
      RTMaterial rtm{};
      rtm.baseColor = m.baseColor;
      rtm.metallic = m.metallic;
      rtm.roughness = m.roughness;
      rtm.emissive = glm::vec3(0.0f);
      rtm.specular = glm::vec3(m.specular);
      // Glass: closesthit.rchit refracts through it, and section.rahit lets
      // a shadow ray through it tinted (docs/rendering/materials.md#glass).
      rtm.transmission = m.transmission;
      rtm.optics = glm::vec4(m.ior, 0.0f, 0.0f, 0.0f);
      if (m.pattern != CADMaterials::SurfacePattern::Flat) {
        // One rule, shared with the raster path (MeshRenderer's draw loop).
        // Each body gets its own material here, so the axis rides in it.
        rtm.procParams =
            glm::vec4(static_cast<float>(static_cast<int>(m.pattern)),
                      m.patternScale, m.patternContrast,
                      static_cast<float>(CADMaterials::PatternAxis(
                          m.pattern, localHi - localLo)));
      }
      return rtm;
    };
    uint32_t matId = state.rayTracer->AddMaterial(rtMaterialOf(mat));

    // Faces with looks of their own (v164): a material per look and an index
    // per triangle, so the body stays ONE geometry. Split into one geometry
    // per look, the still's outline pass would draw a body boundary along
    // every colour change, smooth or not.
    const std::vector<FaceLookRun> runs = FaceLookRuns(body, triangleFaces);
    if (runs.empty() || triangleFaces.size() * 3 != indices.size()) {
      state.rayTracer->AddGeometry(vertices, indices, matId);
    } else {
      std::map<const FaceAppearance*, uint32_t> faceMats;
      std::vector<uint32_t> triMats(triangleFaces.size(), matId);
      for (const FaceLookRun& r : runs) {
        if (r.face == nullptr)
          continue;
        auto [slot, fresh] = faceMats.try_emplace(r.face, 0u);
        if (fresh) {
          const BodyLook faceLook = ResolveFaceLook(look, r.face->look);
          slot->second = state.rayTracer->AddMaterial(
              rtMaterialOf(faceLook.hasMaterial ? faceLook.mat : mat));
        }
        std::fill_n(triMats.begin() + r.firstTriangle, r.triangleCount,
                    slot->second);
      }
      state.rayTracer->AddGeometry(vertices, indices, matId, 0, &triMats);
    }
  }

  if (state.rayTracer->GeometryCount() == 0)
    return false;
  LOG_INFO("Ray tracing scene: {} geometries ({} reused from the part render "
           "cache, {} tessellated here)",
           state.rayTracer->GeometryCount(), reused, meshed);

  // ---- Section cut caps -------------------------------------------------
  // section.rahit removes the material past a plane, which leaves a solid
  // looking like a shell: you see straight through the wall into the inside
  // of the far surface. The raster viewport fills that with a cap per body
  // per plane, and this is the same Measure::SectionCaps call it makes, so
  // both renderers cut the same shape and fill it the same way.
  //
  // The cap carries the body's BASE colour and no procedural grain, matching
  // AppendCapMeshes. End grain on a cut wooden face would be more truthful,
  // but the viewport does not draw it and disagreeing with the screen is the
  // thing this whole change exists to stop.
  {
    const ClipPlaneArray worldPlanes = SectionPlanes(state);
    size_t capCount = 0;
    for (const CapSource& src : capSources) {
      for (size_t i = 0; i < worldPlanes.size(); i++) {
        glm::vec3 localOrigin, localNormal;
        if (!SectionPlaneInPartSpace(worldPlanes[i], src.xf, localOrigin,
                                     localNormal))
          continue;
        // Every OTHER plane trims this cap, or two cuts leave each other's
        // fill hanging in space past the corner.
        std::vector<glm::dvec4> trim;
        for (size_t j = 0; j < worldPlanes.size(); j++) {
          glm::vec3 to, tn;
          if (j != i && SectionPlaneInPartSpace(worldPlanes[j], src.xf, to, tn))
            trim.push_back(glm::vec4(tn, glm::dot(to, tn)));
        }
        // Cached on (bodies, plane): an assembly asks for the same cap of the
        // same part about the same plane every render.
        const std::vector<CADShape> shapes =
            Measure::SectionCaps(*src.bodies, localOrigin, localNormal, trim,
                                 &state.sectionCapCache);
        const glm::vec3 worldNormal = glm::normalize(glm::vec3(worldPlanes[i]));
        for (size_t bi = 0; bi < shapes.size() && bi < src.bodies->size();
             bi++) {
          if (!shapes[bi].IsValid())
            continue;
          CADShape::Mesh cm = shapes[bi].GenerateMesh();
          if (cm.vertices.empty())
            continue;
          std::vector<Vertex3D> cv =
              CADMeshConverter::ConvertCADMeshToVertices(cm);
          const glm::mat3 rot(src.xf);
          for (Vertex3D& v : cv) {
            v.position = glm::vec3(src.xf * glm::vec4(v.position, 1.0f));
            v.normal = glm::normalize(rot * v.normal);
            // The cap lies exactly ON the plane, where the any-hit test
            // dot(n, p) > w is a coin toss in floating point -- half the cap
            // would be cut away by the very plane that created it. Nudge it
            // to the kept side; 1e-3 of a millimetre is invisible.
            v.position -= worldNormal * 1e-3f;
          }
          const Body& b = (*src.bodies)[bi];
          CADMaterials::MaterialProperties cmat =
              CADMaterials::MaterialLibrary::GetMaterialOrDefault(b.material);
          if (b.appearance.color)
            cmat.baseColor = *b.appearance.color;
          RTMaterial crm{};
          crm.baseColor = cmat.baseColor;
          crm.metallic = cmat.metallic;
          crm.roughness = cmat.roughness;
          crm.emissive = glm::vec3(0.0f);
          crm.specular = glm::vec3(cmat.specular);
          // A cut glass body's cap is glass too, not an opaque lid over it.
          crm.transmission = cmat.transmission; // procParams 0: no cap figure
          crm.optics = glm::vec4(cmat.ior, 0.0f, 0.0f, 0.0f);
          state.rayTracer->AddGeometry(cv, cm.indices,
                                       state.rayTracer->AddMaterial(crm));
          capCount++;
        }
      }
    }
    if (capCount > 0)
      LOG_INFO("Ray tracing scene: {} section cut caps", capCount);
  }

  // What the ray tracer was given, for the still to size its ray limits from
  // (MakeStillDesc). Recorded before the ground plane, which is derived from
  // these bounds in turn.
  state.rtSceneValid = bbMin.x <= bbMax.x;
  state.rtSceneMin = state.rtSceneValid ? bbMin : glm::vec3(0.0f);
  state.rtSceneMax = state.rtSceneValid ? bbMax : glm::vec3(0.0f);

  // Shadow-catcher ground plane: a large quad just under the scene so soft
  // shadows and AO have a surface to land on. The closest-hit shader renders it
  // as the environment darkened by occlusion, so the backdrop stays the HDRI.
  if (groundPlane) {
    const glm::vec3 center = 0.5f * (bbMin + bbMax);
    const float diag = glm::length(bbMax - bbMin);
    const float half = 10.0f * diag; // covers every camera angle
    // The floor is perpendicular to world up, just below the scene's lowest
    // point (no coplanar fizz).
    const glm::vec3 n = conv::kWorldUpF;
    const float floorH = glm::dot(bbMin, n) - 0.001f * diag;
    // A floor the camera is UNDER is not a floor, it is a lid. The plane is
    // opaque from both sides, so from below it hid the model and left only
    // its shadow lying on the underside — a render of a dock that is just a
    // dock-shaped smudge (2026-09-01). Nothing sits on a floor you are
    // looking up at, so there is nothing for it to catch.
    if (state.camera && glm::dot(state.camera->GetPosition(), n) <= floorH) {
      LOG_INFO("Ray Tracing: no shadow catcher — the camera is below the "
               "scene's floor");
    } else {
      const glm::vec3 base = center - glm::dot(center, n) * n + floorH * n;
      const glm::vec3 a = half * conv::kWorldRightF;
      const glm::vec3 b = half * conv::kWorldFrontF;
      std::vector<Vertex3D> quad = {
          {base - a - b, n},
          {base + a - b, n},
          {base + a + b, n},
          {base - a + b, n},
      };
      std::vector<uint32_t> quadIdx = {0, 1, 2, 0, 2, 3};
      state.rayTracer->AddGeometry(quad, quadIdx, 0, RT_GEOM_SHADOW_CATCHER);
    }
  }

  return state.rayTracer->BuildAccelerationStructures();
}

// Whether any of the scene's bounding box lands inside the still's frustum.
// The cheap box-vs-frustum rejection: if all eight corners sit outside the
// SAME clip plane, nothing can be in frame. It is what tells a render of the
// backdrop from a render of the part — flatness cannot, because the backdrop
// is an HDRI photograph and photographs are not flat.
bool StillFramesGeometry(const RTStillDesc& desc, const glm::vec3& min,
                         const glm::vec3& max) {
  const glm::mat4 vp = desc.proj * desc.view;
  int outLeft = 0, outRight = 0, outBottom = 0, outTop = 0, outNear = 0,
      outFar = 0;
  for (int c = 0; c < 8; c++) {
    const glm::vec4 p =
        vp * glm::vec4((c & 1) ? max.x : min.x, (c & 2) ? max.y : min.y,
                       (c & 4) ? max.z : min.z, 1.0f);
    outLeft += p.x < -p.w;
    outRight += p.x > p.w;
    outBottom += p.y < -p.w;
    outTop += p.y > p.w;
    outNear += p.z < 0.0f;
    outFar += p.z > p.w;
  }
  return outLeft < 8 && outRight < 8 && outBottom < 8 && outTop < 8 &&
         outNear < 8 && outFar < 8;
}

RTStillDesc MakeStillDesc(AppState& state, uint32_t width, uint32_t height,
                          uint32_t seed, StillBackground& background,
                          bool outlines) {
  RTStillDesc desc;
  desc.backdrop = background.backdrop;
  // Edges, colour and width all come from the renderer the viewport uses, so
  // the still cannot disagree with the screen about what an edge looks like.
  // main.cpp runs it in WIREFRAME mode, which is why this reads wireframeColor
  // and lineWidth rather than the outline pair.
  // The live section cuts, from the same call main.cpp hands every raster
  // renderer -- so a still and the viewport cut on the same planes or neither
  // does. Applied by section.rahit, since closest-hit is too late to discard.
  desc.clipPlanes = SectionPlanes(state);
  desc.outlines = outlines;
  if (state.outlineRenderer) {
    const OutlineSettings& os = state.outlineRenderer->GetOutlineSettings();
    desc.outlines = outlines && os.mode != OutlineMode::NONE;
    desc.outlineColor = glm::vec3(os.wireframeColor);
    desc.outlineWidth = os.lineWidth;
  }
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
  // How far a ray may travel, and how far AO reaches — from the SCENE, not
  // from a literal in the shader. Both were lengths, and the length of what
  // was never asked: at 10000 the ray tracer could not see past 10 m, so a
  // 6 m dock (framed from ~11 m) rendered as pure environment, and AO at
  // 2.0 reached two millimetres of it. Call this AFTER
  // SyncRayTracingScene — it is the bounds of what that handed over.
  if (state.rtSceneValid) {
    const glm::vec3 centre = 0.5f * (state.rtSceneMin + state.rtSceneMax);
    const float diag = glm::length(state.rtSceneMax - state.rtSceneMin);
    // Reach the far side of the scene from wherever the camera stands, plus
    // the shadow-catcher, which is itself 10 diagonals wide (so its far
    // corner is ~14 away); double it and nothing can fall off the end.
    desc.rayFar =
        2.0f * (glm::length(desc.cameraPos - centre) + 15.0f * diag) + 1.0f;
    // The 2.0 literal was tuned on part-scale work — about 0.012 of a
    // 170 mm part's diagonal, which is what this reproduces there.
    desc.aoRadius = std::max(0.012f * diag, 0.01f);
    // Logged because it is the number that was wrong: a reach shorter than
    // the camera's own standoff renders the backdrop and nothing else, and
    // from the picture alone that looks like a rendering bug of any kind.
    LOG_INFO("Ray Tracing: still reach {:.0f}, AO {:.2f} (scene {:.0f} across,"
             " camera {:.0f} away)",
             desc.rayFar, desc.aoRadius, diag,
             glm::length(desc.cameraPos - centre));
  }
  // The viewport's rig for this camera (core/LightRig.h), so the still is
  // lit the way the screen it came from is — including the lock to the model.
  // That rig was the still's own once: the viewport's old world-fixed key sat
  // 18.7 degrees from the home view direction, a headlamp that flattened the
  // form and hid the ground shadow behind the model, so the still swung its
  // own off the camera. The viewport now uses the same placement.
  if (state.camera)
    desc.rig = conv::StudioRig(state.camera->GetViewMatrix(),
                               state.lightsLockedToModel);
  IBLTextures* ibl = state.ibl;
  if (background.backdrop == StillBackdrop::Sky) {
    // The sky (docs/rendering/still-render.md#the-sky). The sun stands where
    // the studio key stands for this camera unless the caller placed it —
    // over the viewer's left shoulder, so the side being looked at is lit,
    // which is the reason the studio key stands there.
    if (!background.sunAzimuthDeg) {
      glm::vec3 k = desc.rig.key;
      k.z = 0.0f; // the key's direction on the ground
      const float len = glm::length(k);
      background.sunAzimuthDeg =
          len > 1e-4f
              ? std::fmod(glm::degrees(std::atan2(k.y, k.x)) + 360.0, 360.0)
              : 0.0;
    }
    if (!background.sunElevationDeg)
      background.sunElevationDeg = kDefaultSunElevationDeg;
    const double az = glm::radians(*background.sunAzimuthDeg);
    const double el = glm::radians(*background.sunElevationDeg);
    const glm::vec3 sun(static_cast<float>(std::cos(el) * std::cos(az)),
                        static_cast<float>(std::cos(el) * std::sin(az)),
                        static_cast<float>(std::sin(el)));
    // The sky is the world's, not the studio's: it does not turn with the
    // camera, and the key light IS the sun, so the shadow falls away from
    // the disc the backdrop shows.
    desc.rig.key = sun;
    desc.rig.envRotation = glm::mat3(1.0f);
    // Outdoors the sun is most of the light. The sky gives about a third of
    // the studio HDRI's light, so at the studio key's strength the whole
    // still sat a stop under the studio one; at 2.5 times it a light grey
    // face square to the sun blew out to white. 1.5 keeps a sunlit face
    // bright with its form, and the shade side a sky-lit blue.
    desc.keyScale = 1.5f;
    ibl = state.skyEnvironment
              ? state.skyEnvironment(*background.sunAzimuthDeg,
                                     *background.sunElevationDeg)
              : nullptr;
  }
  if (ibl) {
    desc.environment = ibl->EnvironmentView();
    desc.irradiance = ibl->IrradianceView();
    desc.prefilter = ibl->PrefilterView();
    desc.brdfLut = ibl->BrdfLutView();
    desc.envSampler = ibl->EnvSampler();
    desc.lutSampler = ibl->LutSampler();
    desc.prefilterMips = ibl->PrefilterMips();
  }
  return desc;
}

std::string StillTabRefusal(const AppState& state) {
  if (state.document || state.assembly)
    return {};
  if (state.activePart >= state.parts.size())
    return "no tab is open: a still renders a part or an assembly tab";
  const PartSlot& slot = state.parts[state.activePart];
  const char* kind = slot.kind == DocKind::Drawing ? "a drawing"
                     : slot.kind == DocKind::Job   ? "a CAM job"
                                                   : "not a model";
  return "the active tab '" + slot.name + "' is " + kind +
         ": render_still renders a part or an assembly tab -- switch to "
         "one (set_active_part) first";
}

std::string PathTracerRefusal(const AppState& state) {
  if (state.renderer.lost)
    return "device_lost: the graphics device was lost (" +
           state.renderer.lostReason +
           "); nothing renders until Loböle restarts";
  if (state.rayTracer && state.rayTracer->IsInitialized())
    return {};
  if (state.renderer.device.empty() && state.gpuName.empty())
    return "ray tracing is not available: this instance has no renderer "
           "(--headless)";
  const std::string device =
      state.renderer.device.empty() ? state.gpuName : state.renderer.device;
  if (state.safeMode.active)
    return "ray tracing is not available in safe mode; mode: quick renders "
           "what the viewport draws";
  if (state.renderer.software)
    return "ray tracing is not available on " + device +
           ", a software device: the path-traced still needs a GPU with "
           "hardware ray tracing; mode: quick renders what the viewport draws";
  return "ray tracing is not available on " + device +
         "; mode: quick renders what the viewport draws";
}

namespace {
// What a still that could not build its scene says: the tab when it is not
// a model, else that the model is empty.
std::string NoSceneReason(const AppState& state) {
  const std::string tab = StillTabRefusal(state);
  return tab.empty() ? std::string("no geometry to render") : tab;
}
} // namespace

bool RenderStillBlocking(AppState& state, uint32_t width, uint32_t height,
                         uint32_t samples, const std::string& path,
                         uint32_t seed, bool groundPlane, std::string* error,
                         std::string* warning, StillBackground* background,
                         bool outlines) {
  auto fail = [&](const std::string& m) {
    if (error)
      *error = m;
    return false;
  };
  if (const std::string why = PathTracerRefusal(state); !why.empty())
    return fail(why);
  if (!SyncRayTracingScene(state, groundPlane))
    return fail(NoSceneReason(state));
  if (samples == 0)
    samples = 1;

  StillBackground studio;
  RTStillDesc desc = MakeStillDesc(state, width, height, seed,
                                   background ? *background : studio, outlines);
  if (warning && state.rtSceneValid &&
      !StillFramesGeometry(desc, state.rtSceneMin, state.rtSceneMax)) {
    *warning = "the camera framed no geometry — the still is the backdrop "
               "only (fit the view, then render again)";
  }
  if (!desc.irradiance)
    return fail(desc.backdrop == StillBackdrop::Sky
                    ? "the sky's lighting maps could not be built"
                    : "IBL maps unavailable");
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

// A quick render's second half, on a worker: average the supersampled frame
// down and write the PNG (docs/rendering/still-render.md#the-dialog). The
// capture before it needs the renderers and runs on the frame; this needs
// nothing but the pixels, so the app keeps drawing, the dialog shows the
// progress, and Cancel stops it before the file is written.
struct QuickEncode {
  RasterFrame frame;
  std::string path;
  std::atomic<float> progress{0.0f};
  std::atomic<bool> cancel{false};
  std::atomic<bool> done{false};
  bool ok = false; // read once `done`
  std::string error;
  std::jthread thread; // last: joined before the fields it uses are freed
};

static void PumpQuickRenderJob(AppState& state) {
  RenderJob& job = state.renderJob;
  auto finish = [&](bool ok, const std::string& err) {
    job.active = false;
    job.started = false;
    job.finished = true;
    job.success = ok;
    job.error = ok ? std::string() : err;
    job.resultPath = ok ? job.outPath : std::string();
    job.encode.reset();
  };

  if (!job.started) {
    // The frame after the click, so the dialog is already showing the bar.
    if (job.cancel) {
      finish(false, "cancelled");
      return;
    }
    if (!state.captureRaster) {
      finish(false, "a quick render needs a renderer");
      return;
    }
    SyncRenderersToDocument(state);
    AppState::RasterStill req;
    req.width = job.width;
    req.height = job.height;
    req.supersample = job.supersample;
    req.outlines = job.outlines;
    req.lineWeight = job.lineWeight;
    req.path = job.outPath;
    auto encode = std::make_shared<QuickEncode>();
    job.warning.clear();
    std::string err;
    if (!state.captureRaster(req, &encode->frame, &err, &job.warning)) {
      finish(false, err.empty() ? "the capture failed" : err);
      return;
    }
    encode->path = job.outPath;
    QuickEncode* e = encode.get();
    e->thread = std::jthread([e] {
      e->ok = DownsampleToPng(e->frame.rgb, e->frame.width, e->frame.height,
                              e->frame.factor, e->path, &e->error, &e->progress,
                              &e->cancel);
      e->frame.rgb = {};
      e->done.store(true);
    });
    job.encode = std::move(encode);
    job.progress = 0.0f;
    job.started = true;
    return;
  }

  QuickEncode& e = *job.encode;
  if (job.cancel)
    e.cancel.store(true);
  job.progress = e.progress.load();
  if (!e.done.load())
    return;
  e.thread.join();
  finish(e.ok, e.error.empty() ? "the PNG could not be written" : e.error);
}

void PumpRenderJob(AppState& state) {
  RenderJob& job = state.renderJob;
  if (!job.active)
    return;
  if (job.quick) {
    PumpQuickRenderJob(state);
    return;
  }

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

  if (const std::string why = PathTracerRefusal(state); !why.empty()) {
    finish(false, why);
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
      finish(false, NoSceneReason(state));
      return;
    }
    StillBackground background;
    background.backdrop =
        job.studioBackdrop ? StillBackdrop::Studio : StillBackdrop::Environment;
    RTStillDesc desc = MakeStillDesc(state, job.width, job.height, job.seed,
                                     background, job.outlines);
    // Saved is not the same as good: a still whose scene is entirely outside
    // the frustum is a valid PNG of the backdrop, and the modal used to
    // report that as a plain success.
    job.warning.clear();
    if (state.rtSceneValid &&
        !StillFramesGeometry(desc, state.rtSceneMin, state.rtSceneMax)) {
      job.warning = "the camera framed no geometry — this is the backdrop "
                    "only (Fit, then render again)";
    }
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

// ---------------------------------------------------------------------------
// The ASSEMBLY tab's renderer sync, from AssemblyActions.cpp
// (docs/architecture.md#the-ui-actions-layer). It is the same job as
// SyncDocumentRenderers above -- push what the tab shows at the renderers --
// for a different kind of tab, and the two were in different files only
// because the monolith they came from was split by document kind first.

// Renders an assembly: one asset per referenced part body, one instance per
// occurrence. Assets are shared across occurrences of the same part.
// The section plane in ONE occurrence's part space. A world plane
// {n, d} and a rigid placement M = [R|t] give n' = Rᵀn and d' = d - n·t:
// caps are computed on the part's own bodies (which is where its geometry
// lives) and placed by M afterwards. Declared in AppActions.h — the still
// needs the same trip inward, and two copies of this would drift.
bool SectionPlaneInPartSpace(const glm::vec4& world, const glm::mat4& xf,
                             glm::vec3& localOrigin, glm::vec3& localNormal) {
  const glm::vec3 n(world);
  if (glm::length(n) < 1e-6f)
    return false;
  const glm::mat3 basis(xf);
  const glm::vec3 t(xf[3]);
  localNormal = glm::normalize(glm::transpose(basis) * n);
  localOrigin = localNormal * (world.w - glm::dot(n, t));
  return true;
}

namespace {

// Where a leaf of `occ` is DRAWN: its document placement `docWorld` (the
// flattened tree's), as the playbacks show it -- a motion run's pose or a
// sequence's, a flexible unit's moving leaf on its own -- and offset by
// the exploded view's steps. The raster sync and the caps that follow a
// playback (FollowPlaybackCaps) both place by it.
glm::dmat4 DrawnLeafWorld(AppState& state, const Occurrence& occ,
                          const glm::dmat4& docWorld,
                          const std::vector<uint64_t>& innerIds) {
  // Motion playback swaps the unit's solved placement for the pose the
  // run recorded — display-only. Only when playing, so every other frame
  // is computed exactly as before (the pixel gate is byte-exact).
  glm::dmat4 world =
      glm::translate(glm::dmat4(1.0), ExplodeOffset(state, occ.id)) * docWorld;
  glm::dmat4 played;
  if (MotionPlaybackWorld(state, occ.id, played)) {
    glm::dmat4 local = glm::inverse(OccurrenceMatrix(occ)) * docWorld;
    glm::dmat4 leaf; // a moving leaf of a flexible placement (v129)
    if (MotionPlaybackLocal(state, occ, innerIds, local, leaf))
      local = leaf;
    world = glm::translate(glm::dmat4(1.0), ExplodeOffset(state, occ.id)) *
            played * local;
  }
  // A moving part of a flexible unit may explode on its own (v130), on top
  // of the unit's step. Only when it has one: the pixel gate is byte-exact.
  if (const glm::dvec3 inner = ExplodeInnerOffset(state, occ.id, innerIds);
      inner != glm::dvec3(0.0))
    world = glm::translate(glm::dmat4(1.0), inner) * world;
  return world;
}

// Section caps (I3) of one leaf placed at `xf`, recorded against leaf
// `leaf` (a sectionCapLeaves index). Without these an assembly section
// shows the inside of hollow shells — the geometry is clipped by the
// shader but the cut has no surface, which reads as "section view is
// broken" rather than as a section. One call per LEAF instance: the plane
// lands somewhere different in each part's own space, and a part nested
// two levels down gets there through its composed world transform.
void AddLeafCaps(AppState& state, const Document& part,
                 const std::vector<SharedBody>& shared, const glm::mat4& xf,
                 uint64_t occurrenceId, size_t leaf) {
  const ClipPlaneArray planes = SectionPlanes(state);
  std::vector<glm::vec3> capColors;
  std::vector<float> capAlphas;
  const size_t first = state.sectionCapMeshes.size();
  for (size_t i = 0; i < planes.size(); i++) {
    glm::vec3 localOrigin, localNormal;
    if (!SectionPlaneInPartSpace(planes[i], xf, localOrigin, localNormal))
      continue;
    if (capColors.empty()) {
      capColors.reserve(shared.size());
      for (const SharedBody& sb : shared) {
        capColors.push_back(sb.hasMaterial ? sb.albedo : sb.palette);
        capAlphas.push_back(sb.alpha); // glass caps stay see-through
      }
    }
    // The other sections, also in this part's space, trim this cap.
    std::vector<glm::dvec4> trim;
    for (size_t j = 0; j < planes.size(); j++) {
      glm::vec3 to, tn;
      if (j != i && SectionPlaneInPartSpace(planes[j], xf, to, tn))
        trim.push_back(glm::vec4(tn, glm::dot(to, tn)));
    }
    // CACHED, and this is the path that pays: the plane is put into the
    // PART's space, so a part that has not moved between two poses of a
    // mechanism asks for the same cap of the same bodies about the same
    // plane. Scrubbing the air-cooled engine with one section on cost
    // 913 ms/step against 9.5 without — an OCC half-space boolean per
    // body, every step, for 27 parts of which four move.
    AppendCapMeshes(state,
                    Measure::SectionCaps(part.GetBodies(), localOrigin,
                                         localNormal, trim,
                                         &state.sectionCapCache),
                    capColors, xf, glm::normalize(glm::vec3(planes[i])),
                    capAlphas, occurrenceId);
  }
  for (size_t c = first; c < state.sectionCapMeshes.size(); c++)
    state.sectionCapMeshes[c].leaf = leaf;
}

// Does the rigid move `d` take every section plane onto itself? Then a cap
// cut before it is the cap after it, moved by `d`: the plane in the part's
// space is unchanged, so the cut is the same.
bool KeepsPlanes(const glm::dmat4& d, const ClipPlaneArray& planes) {
  for (const glm::vec4& p : planes) {
    const glm::dvec3 n(p);
    if (glm::length(n) < 1e-9)
      continue;
    const glm::dvec3 un = glm::normalize(n);
    if (glm::length(glm::dmat3(d) * un - un) > 1e-7 ||
        std::abs(glm::dot(un, glm::dvec3(d[3]))) > 1e-5)
      return false;
  }
  return true;
}

} // namespace

void FollowPlaybackCaps(AppState& state) {
  if (!state.assembly || !state.meshRenderer || state.sectionCapLeaves.empty())
    return;
  const ClipPlaneArray planes = SectionPlanes(state);
  if (planes != state.syncedSectionPlanes)
    return; // the next resync cuts them all again
  // Every leaf: its caps moved by how far it has moved since they were cut,
  // or, where that move turns or slides it across a plane, cut again.
  std::vector<size_t> recut;
  std::vector<glm::dmat4> nowAt(state.sectionCapLeaves.size());
  for (size_t i = 0; i < state.sectionCapLeaves.size(); i++) {
    const AppState::SectionCapLeaf& leaf = state.sectionCapLeaves[i];
    const Occurrence* occ = state.assembly->FindById(leaf.occurrenceId);
    if (occ == nullptr)
      continue;
    nowAt[i] = DrawnLeafWorld(
        state, *occ, OccurrenceMatrix(*occ) * leaf.docLocal, leaf.innerPath);
    const glm::dmat4 d = nowAt[i] * glm::inverse(leaf.world);
    if (!KeepsPlanes(d, planes)) {
      recut.push_back(i);
      continue;
    }
    for (const AppState::SectionCapMesh& cap : state.sectionCapMeshes)
      if (cap.leaf == i)
        state.meshRenderer->SetInstanceTransform(cap.meshId, glm::mat4(d));
  }
  if (!recut.empty() && state.partRenderCache) {
    const PartRenderCacheStore& cache = *state.partRenderCache;
    const std::vector<assembly::FlatInstance> flat =
        FlattenActiveAssembly(state);
    for (const size_t i : recut) {
      std::erase_if(state.sectionCapMeshes,
                    [&](const AppState::SectionCapMesh& cap) {
                      if (cap.leaf != i)
                        return false;
                      state.meshRenderer->RemoveMesh(cap.meshId);
                      return true;
                    });
      AppState::SectionCapLeaf& leaf = state.sectionCapLeaves[i];
      for (const assembly::FlatInstance& inst : flat) {
        if (inst.rootOccurrenceId != leaf.occurrenceId ||
            inst.innerIds != leaf.innerPath || !inst.visible ||
            inst.part == nullptr)
          continue;
        const auto it = cache.parts.find(
            PartRenderKey{inst.partId, inst.configurationId, inst.material});
        if (it == cache.parts.end())
          break;
        AddLeafCaps(state, *inst.part, it->second.bodies, glm::mat4(nowAt[i]),
                    leaf.occurrenceId, i);
        leaf.world = nowAt[i];
        break;
      }
    }
  }
  // What a sequence has not put in yet has no cut face either.
  for (const AppState::SectionCapMesh& cap : state.sectionCapMeshes)
    state.meshRenderer->SetInstanceVisible(
        cap.meshId, !SequencePlaybackHides(state, cap.occurrenceId));
}

void SyncAssemblyRenderers(AppState& state) {
  if (!state.partRenderCache)
    state.partRenderCache = std::make_shared<PartRenderCacheStore>();
  PartRenderCacheStore& cache = *state.partRenderCache;
  cache.syncSerial++;
  // Which parts this sync actually drew. They are never evicted below — you
  // cannot drop what is on screen — and everything else ages against the
  // budget.
  std::unordered_set<PartRenderKey, PartRenderKeyHash> touched;

  // One submit+wait for every asset this sync uploads, instead of one per
  // buffer. A cold 299-part build creates 1196 buffers; unbatched that is
  // 1196 queue drains. Closed at the end of the function (and defensively by
  // the renderers' Render()).
  if (state.meshRenderer)
    state.meshRenderer->BeginAssetBatch();
  if (state.outlineRenderer)
    state.outlineRenderer->BeginAssetBatch();

  // This function has been the app's worst stall twice (per-occurrence
  // outline uploads, then the un-cached rebuild), and both times the guess
  // about which half was slow was wrong. The breakdown is logged, so the next
  // regression is read rather than re-profiled.
  using SyncClock = std::chrono::steady_clock;
  const auto syncStart = SyncClock::now();
  // Device ALLOCATIONS this sync makes, not bytes — a work-volume signal the
  // phase timings above cannot give. Each buffer and image takes its own
  // vkAllocateMemory (no sub-allocator), and a cold 299-part build makes 1196
  // of them; a sync that suddenly makes ten times what it used to has stopped
  // reusing something, which shows here before it shows in the clock.
  // (No device we target imposes a limit — see VulkanCommon.h.)
  const uint64_t allocsAtStart = vkutil::TotalDeviceAllocations();
  double buildMs = 0, instanceMs = 0, pickMs = 0, prewarmMs = 0;
  size_t builtParts = 0, instanceCount = 0;
  const auto elapsedMsSince = [](SyncClock::time_point t) {
    return std::chrono::duration<double, std::milli>(SyncClock::now() - t)
        .count();
  };

  // Which parts this sync will have to tessellate, gathered BEFORE the loop
  // so they can be meshed in parallel (PrewarmTessellations). On a warm cache
  // this list is empty and the whole phase costs one walk of the tree.
  const std::vector<assembly::FlatInstance> flat = FlattenActiveAssembly(state);
  PrewarmedParts prewarmed;
  {
    std::vector<PartRenderKey> missing;
    std::unordered_set<PartRenderKey, PartRenderKeyHash> seen;
    for (const assembly::FlatInstance& inst : flat) {
      const PartRenderKey key{inst.partId, inst.configurationId, inst.material};
      if (!inst.visible || !seen.insert(key).second)
        continue;
      const Document* part = inst.part;
      if (part == nullptr)
        continue;
      const auto entry = cache.parts.find(key);
      if (entry == cache.parts.end() ||
          entry->second.docVersion != PartStamp(*part))
        missing.push_back(key);
    }
    if (!missing.empty()) {
      const auto warmStart = SyncClock::now();
      PrewarmTessellations(state, missing, prewarmed);
      prewarmMs = elapsedMsSince(warmStart);
    }
  }

  // A5: the LEAF parts of the tree, placed. A top-level part occurrence is a
  // one-element subtree, so this loop is unchanged for a flat assembly — and
  // instancing survives nesting, because a FlatInstance still carries its
  // partId and the mesh-asset cache is keyed on that (six barrels in two
  // nested assemblies remain one asset).
  for (const assembly::FlatInstance& inst : flat) {
    if (!inst.visible)
      continue; // hidden here, or inside a hidden sub-assembly
    const Document* part = inst.part;
    if (!part)
      continue; // dangling reference (part deleted) — skip
    const PartRenderKey key{inst.partId, inst.configurationId, inst.material};
    // The occurrence this leaf belongs to at the TOP level: what selection,
    // explode and picking address. A leaf inside a sub-assembly reports its
    // sub-assembly's occurrence, which is what makes it one unit.
    const Occurrence* rootOcc = state.assembly->FindById(inst.rootOccurrenceId);
    if (rootOcc == nullptr)
      continue;
    const Occurrence& occ = *rootOcc;

    touched.insert(key);
    auto it = cache.parts.find(key);
    // A hit is only a hit at the SAME document version: a regenerated part
    // must re-tessellate. Materials and appearances bump the version too, and
    // they are baked into the entry, so one key covers both.
    if (it != cache.parts.end() && it->second.docVersion != PartStamp(*part)) {
      cache.parts.erase(it);
      it = cache.parts.end();
    }
    if (it == cache.parts.end()) {
      const auto buildStart = SyncClock::now();
      builtParts++;
      // First time we see this part at this version: tessellate its bodies +
      // create assets. Everything below is what the cache exists to skip.
      PartRenderCacheEntry entry;
      entry.docVersion = PartStamp(*part);
      std::vector<SharedBody>& shared = entry.bodies;
      const auto& bodies = part->GetBodies();
      // The fallback palette is seeded from the PART, not from the order
      // parts happen to be encountered in. It used to be a counter running
      // across the whole assembly, so hiding one occurrence could renumber
      // every part after it and silently recolour unrelated geometry — and a
      // cached colour would then disagree with a freshly built one.
      size_t palette = static_cast<size_t>(inst.partId);
      const auto warm = prewarmed.find(key);
      for (size_t bodyIdx = 0; bodyIdx < bodies.size(); bodyIdx++) {
        const auto& body = bodies[bodyIdx];
        SharedBody sb;
        // A Derived ghost is drawn on its OWN part's tab and nowhere else:
        // in the assembly the thing it copies is already here, at its own
        // occurrence. Drawing both put a second servo and a second horn
        // inside the real ones — visibly a different colour, offset by the
        // copy's placement, and impossible to select apart. PrewarmTessella-
        // tions already skipped them; this loop did not, so they were meshed
        // on the fallback path and instanced anyway. The empty slot keeps
        // bodyIdx aligned with the part's GetBodies() (picking reads it).
        if (body.reference) {
          shared.push_back(std::move(sb));
          continue;
        }
        // The geometry comes from the app-wide shape cache, so a body drawn
        // on its own part tab, here, and as a Derived copy is tessellated
        // and uploaded ONCE. Normally the mesh is already computed
        // (PrewarmTessellations, above); meshing inside the lookup is the
        // fallback for a part that was not in the missing list.
        CpuBody* pre = nullptr;
        if (warm != prewarmed.end() && bodyIdx < warm->second.size() &&
            !warm->second[bodyIdx].vertices.empty())
          pre = &warm->second[bodyIdx];
        sb.tess = LookupOrBuildTessellation(state, body.shape, pre);
        if (!sb.tess) {
          shared.push_back(std::move(sb)); // keep index alignment
          continue;
        }
        // Resolve order: appearance > material > displayColor > palette
        // (ui/BodyAppearance.h — the part-tab loop and the exports share it).
        // A placement made of another material than its part wears it.
        const BodyLook look = ResolveBodyLook(body, palette, inst.material);
        sb.palette = look.palette;
        sb.runs = MeshLookRunsFor(body, look, sb.tess->triangleFaces);
        if (state.outlineRenderer)
          OutlineAssetFor(state, *sb.tess);
        if (look.hasMaterial) {
          const CADMaterials::MaterialProperties& mat = look.mat;
          sb.hasMaterial = true;
          sb.albedo = mat.baseColor;
          sb.pbr = glm::vec4(mat.metallic, mat.roughness, mat.specular,
                             mat.reflectance);
          sb.pattern = mat.pattern;
          sb.patternScale = mat.patternScale;
          sb.patternContrast = mat.patternContrast;
          sb.alpha = CADMaterials::ViewportAlpha(mat);
        }
        shared.push_back(std::move(sb));
      }
      for (const SharedBody& sb : entry.bodies) {
        if (!sb.tess)
          continue;
        // What this entry keeps alive through its shared_ptr. The shape
        // cache has its own budget over the same bytes; this one bounds how
        // many PARTS the assembly layer pins, which is what its LRU is for.
        entry.gpuBytes += sb.tess->bytes;
      }
      it = cache.parts.emplace(key, std::move(entry)).first;
      buildMs += elapsedMsSince(buildStart);
    }
    it->second.lastUsed = cache.syncSerial;

    // The flattened world transform, offset by the TOP-LEVEL occurrence's
    // explode vector — exploding a sub-assembly moves the whole unit, not
    // each of its parts to its own corner — and posed by a playback.
    const glm::dmat4 world =
        DrawnLeafWorld(state, occ, inst.world, inst.innerIds);
    // The model composes in double; the renderers take float, here.
    const glm::mat4 xf(world);
    for (size_t bi = 0; bi < it->second.bodies.size(); bi++) {
      const SharedBody& sb = it->second.bodies[bi];
      if (!sb.tess)
        continue; // empty body
      const auto instanceStart = SyncClock::now();
      uint32_t meshId = state.meshRenderer->AddInstance(sb.tess->meshAsset, xf);
      if (sb.hasMaterial) {
        state.meshRenderer->SetInstanceMaterial(meshId, sb.albedo, sb.pbr);
        state.meshRenderer->SetInstanceProcedural(
            meshId, sb.pattern, sb.patternScale, sb.patternContrast);
        if (sb.alpha < 1.0f) // glass: see-through, after the material
          state.meshRenderer->SetInstanceGlass(meshId, sb.alpha);
      } else {
        state.meshRenderer->SetInstanceColor(meshId, sb.palette);
      }
      if (!sb.runs.empty())
        state.meshRenderer->SetInstanceLookRuns(meshId, sb.runs);
      uint32_t outlineId = 0;
      if (state.outlineRenderer && sb.tess->outlineAsset != 0) {
        // Untransformed vertices + a per-instance model matrix (A2.5): the
        // outline draws at the occurrence transform via a push constant, so a
        // live drag is a SetMeshTransform matrix update, not a re-upload — and
        // since R? the geometry itself is shared, so placing the sixth copy of
        // a bracket uploads nothing at all.
        outlineId =
            state.outlineRenderer->AddInstance(sb.tess->outlineAsset, xf);
      }
      instanceMs += elapsedMsSince(instanceStart);
      instanceCount++;
      const auto pickStart = SyncClock::now();
      if (state.faceSelector)
        state.faceSelector->AddMesh(meshId, sb.tess->vertices, sb.tess->indices,
                                    sb.tess->triangleFaces, xf, sb.tess->edges,
                                    sb.tess->solid);
      pickMs += elapsedMsSince(pickStart);
      // localXf: the leaf's place inside the unit. xf = DisplayMatrix(occ) *
      // localXf by construction, which is what lets the drag path recompose
      // it per frame without a resync.
      // A part an assembly sequence being played has not put in yet.
      if (SequencePlaybackHides(state, occ.id)) {
        state.meshRenderer->SetInstanceVisible(meshId, false);
        if (state.outlineRenderer && outlineId != 0)
          state.outlineRenderer->SetMeshVisible(outlineId, false);
      }
      state.documentMeshes.push_back(
          {meshId, outlineId, bi, occ.id,
           glm::inverse(OccurrenceMatrix(occ)) * inst.world, inst.partId,
           inst.innerIds, sb.tess->edges, inst.configurationId});
    }

    // Section caps (I3), recorded with where the leaf stood, so a playback
    // can move them with it (FollowPlaybackCaps).
    if (!SectionPlanes(state).empty()) {
      const size_t leaf = state.sectionCapLeaves.size();
      state.sectionCapLeaves.push_back(
          {occ.id, inst.innerIds,
           glm::inverse(OccurrenceMatrix(occ)) * inst.world, world});
      const size_t firstCap = state.sectionCapMeshes.size();
      AddLeafCaps(state, *part, it->second.bodies, xf, occ.id, leaf);
      // A part a sequence has not put in yet has no cut face either.
      if (SequencePlaybackHides(state, occ.id))
        for (size_t c = firstCap; c < state.sectionCapMeshes.size(); c++)
          state.meshRenderer->SetInstanceVisible(
              state.sectionCapMeshes[c].meshId, false);
    }
  }
  // Land every upload this sync recorded before anything reads the assets.
  if (state.meshRenderer)
    state.meshRenderer->EndAssetBatch();
  if (state.outlineRenderer)
    state.outlineRenderer->EndAssetBatch();

  // Age out what is not on screen, down to kPartRenderCacheBudgetBytes.
  //
  // This was once a strict "release anything not drawn this sync", which
  // looked tidy and made two assembly tabs THRASH: alternating between two
  // 299-part assemblies re-tessellated all 299 every switch (4.2 s each), so
  // the cache bought exactly nothing in a multi-assembly project. Keeping
  // everything instead grows without bound across imports. Hence a budget,
  // evicting least-recently-drawn first — and never a part drawn this sync,
  // whose assets are in use.
  size_t idleBytes = 0;
  std::vector<std::pair<uint64_t, PartRenderKey>> idle; // (lastUsed, key)
  for (const auto& [partId, entry] : cache.parts) {
    if (touched.count(partId) != 0)
      continue;
    idleBytes += entry.gpuBytes;
    idle.emplace_back(entry.lastUsed, partId);
  }
  if (idleBytes > kPartRenderCacheBudgetBytes) {
    std::sort(idle.begin(), idle.end(), [](const auto& a, const auto& b) {
      return a.first < b.first;
    }); // oldest first
    size_t dropped = 0;
    for (const auto& [lastUsed, partId] : idle) {
      if (idleBytes <= kPartRenderCacheBudgetBytes)
        break;
      auto it = cache.parts.find(partId);
      if (it == cache.parts.end())
        continue;
      idleBytes -= it->second.gpuBytes;
      dropped += it->second.gpuBytes;
      cache.parts.erase(it);
    }
    LOG_INFO("Part render cache over budget: dropped {} MB of idle geometry",
             dropped / (1024 * 1024));
  }

  // (Highlights re-apply themselves: they are a per-frame projection of the
  // Selection now — see SyncSelectionHighlights.)
  LOG_INFO("Assembly synced: {} occurrences, {} instances, {} parts cached "
           "({} tessellated) in {:.0f} ms [build {:.0f}, instances {:.0f}, "
           "picking {:.0f}, of which parallel tessellation {:.0f}], "
           "{} device allocations ({} total)",
           state.assembly->OccurrenceCount(), instanceCount, cache.parts.size(),
           builtParts, elapsedMsSince(syncStart), buildMs, instanceMs, pickMs,
           prewarmMs, vkutil::TotalDeviceAllocations() - allocsAtStart,
           vkutil::TotalDeviceAllocations());
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
  const assembly::PartResolver resolve = PartResolverFor(state);

  // Stale check against the stashed slot's stamps (the active-tab machinery
  // in SolveActiveAssembly/AssemblyPartsStale can't see a stashed assembly).
  bool stale = false;
  for (const Occurrence& occ : asmDoc.GetOccurrences()) {
    // the live part: its stamp covers every configuration of it
    const Document* part = PartDocById(state, occ.partId);
    if (!part)
      continue;
    const auto it = asmSlot->solvedPartVersions.find(occ.partId);
    if (it == asmSlot->solvedPartVersions.end() ||
        it->second != PartStamp(*part)) {
      stale = true;
      break;
    }
  }
  if (stale) {
    assembly::Solve(asmDoc, resolve, AssemblyResolverFor(state));
    asmDoc.BumpVersion();
    asmSlot->solvedPartVersions.clear();
    for (const Occurrence& occ : asmDoc.GetOccurrences())
      // the live part: its stamp covers every configuration of it
      if (const Document* part = PartDocById(state, occ.partId))
        asmSlot->solvedPartVersions[occ.partId] = PartStamp(*part);
  }

  const Occurrence* edited = asmDoc.FindById(state.inContext.occurrenceId);
  if (!edited)
    return;
  const glm::dmat4 inv = glm::inverse(OccurrenceMatrix(*edited));

  // One shared asset per context part body (as in SyncAssemblyRenderers),
  // minus everything a ghost doesn't need: outlines, picking, materials.
  const glm::vec3 kGhostTint(0.55f, 0.60f, 0.68f);
  // partId -> one cache entry per body (nullptr for a body that meshes to
  // nothing, so the index still lines up with GetBodies()).
  std::unordered_map<PartRenderKey,
                     std::vector<std::shared_ptr<BodyTessellation>>,
                     PartRenderKeyHash>
      byPart;
  // A5: ghost the whole tree. Editing INTO a sub-assembly is still a non-goal
  // (open its own tab), but the CONTEXT around the part you are editing has
  // to look like the assembly does — a gangway that vanished while you worked
  // would be worse than no context at all.
  auto flat = assembly::Flatten(asmDoc, PartResolverFor(state),
                                AssemblyResolverFor(state));
  if (!flat)
    return; // a cycle: the resync that draws the real thing already said so
  for (const assembly::FlatInstance& inst : *flat) {
    if (!inst.visible || inst.rootOccurrenceId == edited->id)
      continue;
    const Document* part = inst.part;
    if (!part)
      continue;
    const PartRenderKey key{inst.partId, inst.configurationId, inst.material};
    auto it = byPart.find(key);
    if (it == byPart.end()) {
      // Cached by shape identity (AppActionsInternal.h): the neighbour was
      // tessellated once, by whichever tab drew it first, and a regeneration
      // of the part being edited re-instances it rather than rebuilding it.
      std::vector<std::shared_ptr<BodyTessellation>> tess;
      for (const auto& body : part->GetBodies())
        tess.push_back(TessellationFor(state, body.shape));
      it = byPart.emplace(key, std::move(tess)).first;
    }
    const glm::mat4 xf(inv * inst.world); // to the renderer, in float
    for (size_t bi = 0; bi < it->second.size(); bi++) {
      if (it->second[bi] == nullptr)
        continue; // empty body
      const uint32_t meshId =
          state.meshRenderer->AddInstance(it->second[bi]->meshAsset, xf);
      state.meshRenderer->SetInstanceColor(meshId, kGhostTint);
      state.meshRenderer->SetInstanceGhost(meshId); // after color: sets alpha
      // Ghosts are never dragged, so their localXf is unused; keep it
      // honest anyway rather than leaving a lie in the record.
      state.documentMeshes.push_back(
          {meshId, /*outlineId=*/0, bi, inst.rootOccurrenceId, inst.world, {}});
    }
  }
}

} // namespace ui
