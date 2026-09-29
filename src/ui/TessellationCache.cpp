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

// One tessellation per SHAPE, and the threads that build them ahead of the
// sync that consumes them.
//
// A part drawn on its own tab and again in an assembly used to be meshed and
// uploaded twice, and a Derived copy of it a third time; keying on the
// shared TShape made switching to a 197-body part cost 2.2 s once instead of
// every time. The prewarm groups work BY TSHAPE so two threads can never
// mesh shapes that share one.
// Split out of AssemblyActions.cpp (docs/architecture.md#the-ui-actions-layer).
// AssemblyActions.h still declares everything it declared.

namespace ui {

// `SharedBody` — one part body tessellated once and shared by every
// occurrence of the part — now lives in AppActionsInternal.h, because it
// outlives this function: AppState::partRenderCache keeps it across resyncs.

namespace {

// One body's tessellation, computed on a worker thread. Pure CPU: no OCC
// document mutation beyond the shape's own triangulation, no AppState, no
// Vulkan.
// Dropping a part's cache entry drops this layer's references to the
// tessellations it drew with. The SHAPE cache owns those (and the renderer
// assets with them), so nothing is released here: another tab, or another
// part deriving the same geometry, may still be drawing them.
} // namespace

void EvictPartRenderCache(AppState& state, uint64_t partSlotId) {
  if (!state.partRenderCache)
    return;
  // every configuration of it
  std::erase_if(state.partRenderCache->parts,
                [&](const auto& kv) { return kv.first.part == partSlotId; });
}

void ReleasePartRenderCache(AppState& state) {
  if (state.partRenderCache) {
    state.partRenderCache->parts.clear();
  }
  ReleaseTessellationCache(state);
}

// --- the per-BODY tessellation cache (AppActionsInternal.h) ---------------

namespace {

void ReleaseTessellation(AppState& state, const BodyTessellation& entry) {
  if (state.meshRenderer && entry.meshAsset != 0)
    state.meshRenderer->ReleaseAsset(entry.meshAsset);
  if (state.outlineRenderer && entry.outlineAsset != 0)
    state.outlineRenderer->ReleaseAsset(entry.outlineAsset);
}

size_t TessellationBytes(const BodyTessellation& entry) {
  return entry.vertices.size() * sizeof(Vertex3D) +
         entry.indices.size() * sizeof(uint32_t);
}
} // namespace

std::shared_ptr<BodyTessellation> TessellationFor(AppState& state,
                                                  const CADShape& shape) {
  return LookupOrBuildTessellation(state, shape, nullptr);
}

uint32_t OutlineAssetFor(AppState& state, BodyTessellation& entry) {
  if (!state.outlineRenderer)
    return 0;
  if (entry.outlineAsset != 0)
    return entry.outlineAsset;
  entry.outlineAsset =
      state.outlineRenderer->CreateAsset(entry.vertices, entry.indices);
  state.outlineRenderer->RetainAsset(entry.outlineAsset);
  if (state.tessellationCache) {
    const size_t extra = TessellationBytes(entry);
    entry.bytes += extra;
    state.tessellationCache->bytes += extra;
  }
  return entry.outlineAsset;
}

void TrimTessellationCache(AppState& state) {
  if (!state.tessellationCache)
    return;
  BodyTessellationCache& cache = *state.tessellationCache;
  cache.serial++;
  if (cache.bytes <= kTessellationCacheBudgetBytes)
    return;
  // Oldest first, and never what this sync drew (serial - 1 after the ++).
  struct Aged {
    const void* key;
    size_t index;
    uint64_t serial;
    size_t bytes;
  };
  std::vector<Aged> aged;
  for (const auto& [key, bucket] : cache.byShape)
    for (size_t i = 0; i < bucket.size(); i++)
      if (bucket[i]->lastSerial + 1 < cache.serial)
        aged.push_back({key, i, bucket[i]->lastSerial, bucket[i]->bytes});
  std::sort(aged.begin(), aged.end(),
            [](const Aged& a, const Aged& b) { return a.serial < b.serial; });
  std::map<const void*, std::vector<size_t>> drop;
  for (const Aged& a : aged) {
    if (cache.bytes <= kTessellationCacheBudgetBytes)
      break;
    cache.bytes -= a.bytes;
    drop[a.key].push_back(a.index);
  }
  for (auto& [key, indices] : drop) {
    auto& bucket = cache.byShape[key];
    std::sort(indices.rbegin(), indices.rend());
    for (size_t i : indices) {
      // Only the CACHE's hold is dropped: a live instance, or a part render
      // cache entry still pointing at this tessellation, keeps its own
      // reference and the buffers with it.
      ReleaseTessellation(state, *bucket[i]);
      bucket.erase(bucket.begin() + static_cast<long>(i));
    }
    if (bucket.empty())
      cache.byShape.erase(key);
  }
}

void ReleaseTessellationCache(AppState& state) {
  if (!state.tessellationCache)
    return;
  for (const auto& [key, bucket] : state.tessellationCache->byShape)
    for (const std::shared_ptr<BodyTessellation>& entry : bucket)
      ReleaseTessellation(state, *entry);
  state.tessellationCache->byShape.clear();
  state.tessellationCache->bytes = 0;
}

// The cache's own interface, at `ui` scope rather than file-static because
// RendererSync.cpp is what consumes it: the sync asks for a prewarm, then
// looks each body up as it walks the assembly. Declared in
// AppActionsInternal.h (stage 6).

// Tessellates the given parts across threads, ahead of the sync loop that
// consumes them.
//
// This is the single biggest cost of first seeing a large assembly: OCC meshes
// 299 parts one after another for ~3.7 s. `BRepMesh_IncrementalMesh` already
// parallelises WITHIN one shape; nothing parallelised across shapes, and the
// parts are independent.
//
// Safety, established by measurement rather than by reading OCC's docs
// (harness in the session notes):
//   - Meshing 299 distinct products serially and across 8 threads produced
//     IDENTICAL output, vertex counts and summed positions alike.
//   - The only shared mutable state a mesher can touch is the triangulation it
//     writes into each TFace, so the hazard is two threads meshing shapes that
//     SHARE a TShape. Part documents load their geometry independently (an
//     .brep sidecar each, or features that build fresh shapes), and a scan of
//     the real 299-part import found zero TFaces shared between parts.
//   - That last point is a property of today's features, not an invariant
//     anything enforces — a future "duplicate part" would copy a CADShape,
//     which SHARES its TShape. So rather than trust it, work is GROUPED BY
//     TSHAPE below: shapes that share one are always meshed by the same
//     thread, which makes the race impossible instead of merely absent.
uint64_t PartRenderStamp(const Document& part) {
  return part.Version() ^
         (part.ConfigurationsRevision() * 0x9E3779B97F4A7C15ull);
}

void PrewarmTessellations(AppState& state,
                          const std::vector<PartRenderKey>& parts,
                          PrewarmedParts& out) {
  // One work item per body, keyed by the TShape whose triangulation it writes.
  struct Item {
    PartRenderKey partId;
    size_t bodyIndex;
    const CADShape* shape;
  };
  std::unordered_map<const void*, std::vector<Item>> byTShape;
  size_t bodies = 0;
  const assembly::PartResolver resolve = PartResolverFor(state);
  for (const PartRenderKey& partId : parts) {
    const Document* part = resolve(partId.part, partId.configuration);
    if (part == nullptr)
      continue;
    const auto& partBodies = part->GetBodies();
    out[partId].resize(partBodies.size());
    for (size_t bi = 0; bi < partBodies.size(); bi++) {
      // A derived ghost is drawn in its own part's tab only: in the
      // assembly the real part is there, at its own occurrence.
      if (!partBodies[bi].shape.IsValid() || partBodies[bi].reference)
        continue;
      byTShape[partBodies[bi].shape.GetShape().TShape().get()].push_back(
          {partId, bi, &partBodies[bi].shape});
      bodies++;
    }
  }
  if (bodies == 0)
    return;

  std::vector<std::vector<Item>*> groups;
  groups.reserve(byTShape.size());
  for (auto& [tshape, items] : byTShape)
    groups.push_back(&items);

  const auto meshOne = [&out](const Item& item) {
    CADShape::Mesh m = item.shape->GenerateMesh();
    if (m.vertices.empty())
      return;
    CpuBody& dst = out[item.partId][item.bodyIndex];
    dst.vertices = CADMeshConverter::ConvertCADMeshToVertices(m);
    dst.indices = std::move(m.indices);
    dst.triangleFaces = std::move(m.triangleFaces);
    dst.edges = item.shape->GenerateEdgePolylines(0.1);
  };

  // `out` is pre-sized above and every item writes its own slot, so the map
  // is never rehashed and no two threads touch the same element.
  //
  // Half the cores, capped: the mesher is internally parallel, so
  // oversubscribing loses — measured 3.2x at 8 threads and only 2.3x at 16 on
  // a 22-thread machine.
  const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
  const size_t workers =
      std::min<size_t>({groups.size(), std::max(1u, hw / 2), 8});
  if (workers <= 1) {
    for (const std::vector<Item>* g : groups)
      for (const Item& item : *g)
        meshOne(item);
    return;
  }

  std::atomic<size_t> next{0};
  std::vector<std::thread> pool;
  pool.reserve(workers);
  for (size_t t = 0; t < workers; t++) {
    pool.emplace_back([&] {
      // OCC turns hardware signals into Standard_Failure through a handler
      // installed PER THREAD (OCC_CONVERT_SIGNALS). Without this a fault in
      // the mesher here is a raw SIGSEGV rather than a catchable failure.
      OSD::SetSignal(Standard_False);
      for (;;) {
        const size_t i = next.fetch_add(1, std::memory_order_relaxed);
        if (i >= groups.size())
          break;
        // Every item in a group shares a TShape, so they run in sequence on
        // this one thread.
        for (const Item& item : *groups[i]) {
          try {
            meshOne(item);
          } catch (const Standard_Failure& e) {
            LOG_WARN("Tessellation failed for part {}: {}", item.partId.part,
                     e.GetMessageString());
          }
        }
      }
    });
  }
  for (std::thread& th : pool)
    th.join();
}

// Looks `shape` up; on a miss adopts `prebuilt` when the caller already
// meshed it (PrewarmTessellations, on worker threads) and meshes here
// otherwise. The GPU upload always happens here, on the main thread.
std::shared_ptr<BodyTessellation>
LookupOrBuildTessellation(AppState& state, const CADShape& shape,
                          CpuBody* prebuilt) {
  if (!state.meshRenderer || !shape.IsValid())
    return nullptr;
  if (!state.tessellationCache)
    state.tessellationCache = std::make_shared<BodyTessellationCache>();
  BodyTessellationCache& cache = *state.tessellationCache;
  auto& bucket = cache.byShape[shape.GeometryId()];
  for (const std::shared_ptr<BodyTessellation>& entry : bucket) {
    if (!entry->shape.SameGeometryAs(shape))
      continue;
    entry->lastSerial = cache.serial;
    return entry;
  }
  auto entry = std::make_shared<BodyTessellation>();
  entry->shape = shape;
  entry->solid = shape.SolidCount() > 0;
  if (prebuilt != nullptr && !prebuilt->vertices.empty()) {
    entry->vertices = std::move(prebuilt->vertices);
    entry->indices = std::move(prebuilt->indices);
    entry->triangleFaces = std::move(prebuilt->triangleFaces);
    entry->edges = std::move(prebuilt->edges);
  } else {
    CADShape::Mesh mesh = shape.GenerateMesh();
    if (mesh.vertices.empty())
      return nullptr;
    entry->vertices = CADMeshConverter::ConvertCADMeshToVertices(mesh);
    entry->indices = std::move(mesh.indices);
    entry->triangleFaces = std::move(mesh.triangleFaces);
    entry->edges = shape.GenerateEdgePolylines(0.1);
  }
  entry->meshAsset =
      state.meshRenderer->CreateAsset(entry->vertices, entry->indices);
  state.meshRenderer->RetainAsset(entry->meshAsset);
  entry->bytes = TessellationBytes(*entry);
  entry->lastSerial = cache.serial;
  cache.bytes += entry->bytes;
  bucket.push_back(entry);
  return entry;
}

} // namespace ui
