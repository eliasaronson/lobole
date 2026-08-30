#pragma once

// Internal (non-public) shared helpers for the AppActions* domain translation
// units carved out of AppActions.cpp. Only those TUs include this. The tiny
// LineVertex constructor is used by every overlay/marker builder across the
// split, so it lives here (inline); the heavier tab-slot accessors
// (SlotDoc/SlotAssembly) are declared here and defined in AppActions.cpp.

#include "render/MeshRenderer.h" // LineVertex3D, Vertex3D

#include <glm/glm.hpp>

#include <cstdint>
#include <unordered_map>
#include <vector>

class Document;
class AssemblyDocument;
struct Occurrence;

namespace ui {

struct AppState;

inline LineVertex3D LineVertex(const glm::vec3& pos, const glm::vec3& color) {
  LineVertex3D v;
  v.position = pos;
  v.color = color;
  return v;
}

// Whether the datum planes/axes are drawn at all. Shared because the overlay
// GEOMETRY (AppendDatumPlaneQuads / AppendDatumAxisLines, in AppActions.cpp)
// and the overlay LABELS (DrawDatumLabels, in Overlays.cpp) must agree — a
// label floating beside a hidden plane is the bug this prevents.
bool DatumPlanesVisible(const AppState& state);

// Assembly-domain helpers defined in AppActions.cpp (they stay because the
// core lifecycle/renderer/drawing code also uses them) but called by the
// extracted AssemblyActions.cpp TU. OccurrenceMatrix and PartDocById moved to
// AppActions.h — the RPC layer resolves cross-part queries through them.
glm::vec3 ExplodeOffset(const AppState& state, uint64_t occId);
glm::mat4 DisplayMatrix(const AppState& state, const Occurrence& o);
extern const glm::vec3 kBodyPalette[5];

// One part body's tessellation, its two GPU assets, and the resolved look the
// instances of it draw with. Shared by every occurrence of the part.
struct SharedBody {
  uint32_t assetId = 0;
  // The outline renderer's asset for the same tessellation. Shared for the
  // same reason: uploading it per occurrence cost 3.2 ms EACH, which was
  // most of an assembly resync (394 ms of 670 on a 98-instance dock).
  uint32_t outlineAssetId = 0;
  std::vector<Vertex3D> vertices;
  std::vector<uint32_t> indices;
  std::vector<uint32_t> triangleFaces;
  std::vector<std::vector<glm::vec3>> edges;
  glm::vec3 palette{0.7f};
  bool hasMaterial = false;
  glm::vec3 albedo{0.7f};
  glm::vec4 pbr{0.0f};
  // Procedural wood/plywood grain: pattern, grainScale, grainContrast.
  int procPattern = 0;
  float grainScale = 0.0f;
  float grainContrast = 0.0f;
};

// What is cached for one part, the document version it was built from, and
// what it costs to keep.
struct PartRenderCacheEntry {
  uint64_t docVersion = 0;
  std::vector<SharedBody> bodies;
  // Device-local bytes the two renderer assets hold (vertices + indices,
  // twice). Budgeted against, because this is the scarce resource.
  size_t gpuBytes = 0;
  // The sync serial that last drew this part; the LRU key.
  uint64_t lastUsed = 0;
};

// AppState::partRenderCache's payload: part slot id -> its entry. The assets
// inside are RETAINED in both renderers (MeshRenderer::RetainAsset /
// OutlineRenderer::RetainAsset), so they survive the instance teardown at the
// head of every resync. Anything that drops an entry must release them first
// — ReleasePartRenderCache / EvictPartRenderCache below are the only two
// places that should.
struct PartRenderCacheStore {
  std::unordered_map<uint64_t, PartRenderCacheEntry> parts;
  uint64_t syncSerial = 0; // ++ per sync; stamps entries for the LRU
};

// How much device memory the cache may hold for parts that are NOT currently
// being drawn. Retained geometry beyond this is dropped, least-recently-drawn
// first.
//
// Both simpler rules are wrong, and each was measured on the 299-part speaker
// import (51 MB of assets):
//   - "drop everything not drawn this sync" made two assembly tabs THRASH —
//     alternating between them re-tessellated all 299 parts every switch,
//     4.2 s each, i.e. the cache bought nothing in a multi-assembly project.
//   - "keep everything" grows without bound: every import adds another
//     assembly's worth for the life of the session.
// A budget keeps a handful of large assemblies hot and still has a ceiling.
//
// NOT a cap on total VRAM: what is on screen is always resident, so a single
// assembly larger than this still loads in full (it just keeps nothing else).
inline constexpr size_t kPartRenderCacheBudgetBytes = 256u * 1024 * 1024;

// Drops one part's cached tessellation + assets (a part deleted, or a tab
// closed). Safe when nothing is cached for it.
// (ReleasePartRenderCache — drop everything — is public, in AppActions.h:
// main.cpp calls it during shutdown.)
void EvictPartRenderCache(AppState& state, uint64_t partSlotId);

// Defined in AssemblyActions.cpp; called by SyncDocumentRenderers (AppActions).
void SyncAssemblyRenderers(AppState& state);
void SyncInContextGhosts(AppState& state);

} // namespace ui
