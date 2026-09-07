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

// Pushing the document at the renderers: meshes and outlines for the active
// tab, the geometry-delta overlay, the ray-traced scene, and a still render.
//
// No unit test covers this file. It is verified by rendering a fixed scene
// through the real GPU and diffing the PNG -- which is how the 2026-08-24
// round pinned the same code when it moved the renderer helpers.
// Split out of AppActions.cpp (docs/architecture-review-2026-09-06.md,
// stage 3). AppActions.h still declares everything, so no caller changed.

namespace ui {

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
    CADShape::Mesh mesh = shape.GenerateMesh(0.1);
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
                                  tess->edges);
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

  // Reference geometry is sized from the document's reach
  // (ScenePlaneHalfExtent), and the bounding box behind it is an OCC pass per
  // body. Stamp it HERE, on the resync that already walks every body and
  // tessellates it — a live edit bumps the version every frame, so computing it
  // on demand would put a bbox pass in the frame loop of exactly the drag that
  // can least afford one.
  {
    float reach = 0.0f;
    for (const auto& body : bodies) {
      glm::vec3 mn, mx;
      body.shape.GetBoundingBox(mn, mx);
      for (int i = 0; i < 3; i++)
        reach = std::max({reach, std::abs(mn[i]), std::abs(mx[i])});
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
  SyncSketchViewRollback(state);
  SyncDocumentRenderers(state);
  SyncFieldRenderer(state);
  SyncCamPlayback(state);
  SyncFastenerGhost(state);
  // Last: it reads the meshes the first step just rebuilt. Before them, it
  // would highlight ids that no longer exist.
  SyncSelectionHighlights(state);
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
    // Which cached tessellation this body is, when there is one. partId 0
    // means "no cache entry to look for" (the part-tab branch below): the
    // cache is filled by the assembly sync and keyed on assembly part slots.
    uint64_t partId = 0;
    uint64_t partVersion = 0;
    size_t bodyIndex = 0;
  };
  std::vector<RTItem> rtItems;
  if (state.assembly) {
    // A5: the leaf parts of the whole tree — a nested assembly must appear in
    // the path-traced still as well as the viewport.
    for (const assembly::FlatInstance& inst : FlattenActiveAssembly(state)) {
      if (!inst.visible)
        continue;
      const auto& bodies = inst.part->GetBodies();
      for (size_t bi = 0; bi < bodies.size(); bi++)
        rtItems.push_back(
            {&bodies[bi], inst.world, inst.partId, inst.part->Version(), bi});
    }
  } else if (state.job) {
    // A job tab ray-traces what it draws: the parts laid out.
    for (const Body& body : JobBodies(state))
      rtItems.push_back({&body, glm::mat4(1.0f)});
  } else if (state.document) {
    for (const auto& body : state.document->GetBodies())
      rtItems.push_back({&body, glm::mat4(1.0f)});
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
    const auto entry = state.partRenderCache->parts.find(it.partId);
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
  for (const RTItem& it : rtItems) {
    const Body& body = *it.body;
    const BodyTessellation* cached = cachedBodyFor(it);
    CADShape::Mesh cadMesh;
    if (cached == nullptr) {
      cadMesh = body.shape.GenerateMesh(0.1);
      if (cadMesh.vertices.empty())
        continue;
      meshed++;
    } else {
      reused++;
    }
    const std::vector<uint32_t>& indices =
        cached != nullptr ? cached->indices : cadMesh.indices;

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
        cached != nullptr ? cached->vertices
                          : CADMeshConverter::ConvertCADMeshToVertices(cadMesh);
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

    state.rayTracer->AddGeometry(vertices, indices, matId);
  }

  if (state.rayTracer->GeometryCount() == 0)
    return false;
  LOG_INFO("Ray tracing scene: {} geometries ({} reused from the part render "
           "cache, {} tessellated here)",
           state.rayTracer->GeometryCount(), reused, meshed);

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
                          uint32_t seed, bool studioBackdrop, bool outlines) {
  RTStillDesc desc;
  desc.studioBackdrop = studioBackdrop;
  // Edges, colour and width all come from the renderer the viewport uses, so
  // the still cannot disagree with the screen about what an edge looks like.
  // main.cpp runs it in WIREFRAME mode, which is why this reads wireframeColor
  // and lineWidth rather than the outline pair.
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
  // The still's rig is placed OFF the camera axis. The shared rig
  // (conv::kKeyLightDir) sits 18.7 degrees from the default view direction —
  // a headlamp: it flattens the form, and it throws the shadow directly
  // behind the model where the model hides it, so "Ground shadows" looked
  // like it did nothing. The viewport can live with that (SSAO and its
  // outlines do the shape definition); the still now draws outlines too, but
  // it still has no SSAO and must be LIT for the picture.
  //
  // 40 degrees to the camera's left and 45 up, derived from the camera so it
  // stays off-axis from any viewpoint. Left because the shadow then falls to
  // the right, and the eye reads a form lit from the left as convex.
  if (state.camera) {
    const glm::vec3 toCamera =
        glm::normalize(state.camera->GetPosition() - state.camera->GetTarget());
    const glm::vec3 up = conv::kWorldUpF;
    glm::vec3 right = glm::cross(toCamera, up);
    // Looking straight down the up axis leaves no horizontal to swing about;
    // any perpendicular will do there.
    if (glm::length(right) < 1e-4f)
      right = glm::cross(toCamera, glm::vec3(1.0f, 0.0f, 0.0f));
    right = glm::normalize(right);
    const float swing = glm::radians(40.0f);
    const float lift = glm::radians(45.0f);
    const glm::vec3 flat =
        glm::normalize(toCamera - glm::dot(toCamera, up) * up);
    const glm::vec3 swung =
        std::cos(swing) * flat - std::sin(swing) * right; // toward its left
    desc.keyLightDir =
        glm::normalize(std::cos(lift) * swung + std::sin(lift) * up);
    // Fill opposite the key and barely lifted, to open the shadow side
    // without casting a second one; rim from behind and high, for the bright
    // edge that separates a body from what is behind it. Both carry the
    // shared rig's colour and intensity (cool 0.4, neutral 0.6) — only the
    // placement is derived here, so the three stay a rig from any viewpoint.
    const glm::vec3 fillSwung = std::cos(glm::radians(60.0f)) * flat +
                                std::sin(glm::radians(60.0f)) * right;
    desc.fillLightDir =
        glm::normalize(std::cos(glm::radians(15.0f)) * fillSwung +
                       std::sin(glm::radians(15.0f)) * up);
    desc.rimLightDir = glm::normalize(-flat * std::cos(glm::radians(55.0f)) +
                                      up * std::sin(glm::radians(55.0f)));
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
                         uint32_t seed, bool groundPlane, std::string* error,
                         std::string* warning, bool studioBackdrop,
                         bool outlines) {
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

  RTStillDesc desc =
      MakeStillDesc(state, width, height, seed, studioBackdrop, outlines);
  if (warning && state.rtSceneValid &&
      !StillFramesGeometry(desc, state.rtSceneMin, state.rtSceneMax)) {
    *warning = "the camera framed no geometry — the still is the backdrop "
               "only (fit the view, then render again)";
  }
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
    RTStillDesc desc = MakeStillDesc(state, job.width, job.height, job.seed,
                                     job.studioBackdrop, job.outlines);
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

} // namespace ui
