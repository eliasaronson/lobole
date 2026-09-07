#pragma once

// Internal (non-public) shared helpers for the AppActions* domain translation
// units carved out of AppActions.cpp. Only those TUs include this. The tiny
// LineVertex constructor is used by every overlay/marker builder across the
// split, so it lives here (inline); the heavier tab-slot accessors
// (SlotDoc/SlotAssembly) are declared here and defined in AppActions.cpp.

#include "cad/CADKernel.h"       // CADShape (the tessellation cache key)
#include "render/MeshRenderer.h" // LineVertex3D, Vertex3D

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class Document;
class SketchFeature;
class AssemblyDocument;
class DrawingDocument;
struct Occurrence;
struct Body;
namespace cam {
struct JobDocument;
} // namespace cam

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

// ---------------------------------------------------------------------------
// One BODY's tessellation and its GPU assets, keyed by the SHAPE rather than
// by the document it belongs to.
//
// The part tab rebuilt every mesh on every sync, so switching to a part
// holding a derived 197-body camera cost 2.2 s (1.6 s tessellation, 0.5 s
// upload) EVERY time, and the in-context ghost sync re-tessellated every
// neighbour body on every regeneration of the part being edited. Both draw
// shapes that did not change — often the very same shapes, since a Derived
// copy shares its source's TShape.
//
// Keyed on CADShape::GeometryId (the shared TShape) and verified with
// SameGeometryAs, so a placement or an orientation flip is a different
// entry. The entry holds its CADShape: that keeps the TShape alive, so the
// id cannot be recycled under the cache.
struct BodyTessellation {
  CADShape shape; // the exact key, and what keeps GeometryId() valid
  std::vector<Vertex3D> vertices;
  std::vector<uint32_t> indices;
  std::vector<uint32_t> triangleFaces;
  std::vector<std::vector<glm::vec3>> edges;
  uint32_t meshAsset = 0;
  // Created on FIRST use, not with the entry: a reference ghost draws no
  // outline, and the camera's 197 ghost bodies would otherwise pay for 197
  // outline uploads nobody looks at.
  uint32_t outlineAsset = 0;
  size_t bytes = 0;        // device bytes the two assets hold, for the budget
  uint64_t lastSerial = 0; // sync it was last used in (LRU)
};

struct BodyTessellationCache {
  // Bucketed by TShape address; a bucket holds the placements seen of it.
  std::unordered_map<const void*,
                     std::vector<std::shared_ptr<BodyTessellation>>>
      byShape;
  uint64_t serial = 0;
  size_t bytes = 0;
};

// Same budget rule as the part render cache, and for the same reasons (see
// kPartRenderCacheBudgetBytes): what is on screen is always resident, and
// what is not ages out.
inline constexpr size_t kTessellationCacheBudgetBytes = 256u * 1024 * 1024;

// The tessellation for `shape`, computed once and reused. nullptr when the
// shape meshes to nothing. The pointer is valid until the next
// TrimTessellationCache; callers use it within one sync.
std::shared_ptr<BodyTessellation> TessellationFor(AppState& state,
                                                  const CADShape& shape);
// The outline asset for an entry, created on first use. 0 without an
// outline renderer.
uint32_t OutlineAssetFor(AppState& state, BodyTessellation& entry);
// Ages the cache against the budget. Called at the end of a sync, after
// every entry that sync uses has been touched.
void TrimTessellationCache(AppState& state);
// Drops every entry and its assets (shutdown; ReleasePartRenderCache calls it).
void ReleaseTessellationCache(AppState& state);

// One part body's tessellation, its two GPU assets, and the resolved look the
// instances of it draw with. Shared by every occurrence of the part.
struct SharedBody {
  // The geometry: tessellation + both renderers' assets, owned by the
  // app-wide shape cache below (BodyTessellation). It used to be owned
  // here, per part — so a part drawn on its own tab AND in an assembly was
  // tessellated and uploaded twice, and a Derived copy of it a third time.
  // Null for a body that meshes to nothing (the index still lines up with
  // the part's GetBodies()).
  std::shared_ptr<BodyTessellation> tess;
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

// ---------------------------------------------------------------------------
// The tab slots. AppActions.cpp owns the active-tab machinery -- which slot is
// live, what gets stashed when you leave it -- and ProjectActions.cpp opens and
// saves the slots that machinery manages. These are the seam between the two;
// they were file-static while both lived in one 6,400-line file.
Document* SlotDoc(AppState& state, size_t i);
AssemblyDocument* SlotAssembly(AppState& state, size_t i);
DrawingDocument* SlotDrawing(AppState& state, size_t i);
cam::JobDocument* SlotJob(AppState& state, size_t i);
void StashActive(AppState& state);
void ActivateSlot(AppState& state, size_t index);
void ResetActiveTabUiState(AppState& state);

// The section cut's cap meshes, built in AppActions.cpp next to the rest of
// the section machinery and drawn by SyncDocumentRenderers in RendererSync.cpp.
void AppendSectionCaps(AppState& state, const std::vector<Body>& bodies,
                       const std::vector<glm::vec3>& bodyColors);

// The colour a sketch's lines are drawn in, beside the walk that draws them:
// SceneOverlays.cpp uses it for every sketch that is not being edited, and
// SketchOverlay.cpp for the one that is, and two copies would drift.
inline constexpr glm::vec3 kSketchColor{0.40f, 0.72f, 1.00f};

// One sketch's entities as 3D line vertices. Defined in SceneOverlays.cpp,
// which draws the sketches that are NOT being edited; SketchOverlay.cpp wants
// the same walk for the one that is.
void AppendSketchLines(const SketchFeature& sk, const glm::vec3& color,
                       std::vector<LineVertex3D>& out);

// One body's CPU-side mesh, and a batch of them keyed by part slot: what the
// prewarm produces and the sync consumes. Defined in TessellationCache.cpp.
struct CpuBody {
  std::vector<Vertex3D> vertices;
  std::vector<uint32_t> indices;
  std::vector<uint32_t> triangleFaces;
  std::vector<std::vector<glm::vec3>> edges;
};
using PrewarmedParts = std::unordered_map<uint64_t, std::vector<CpuBody>>;

// Tessellates the given parts across threads, ahead of the sync loop that
// consumes them. Work is GROUPED BY TSHAPE, so two threads can never mesh
// shapes that share one -- see the comment on the definition.
void PrewarmTessellations(AppState& state, const std::vector<uint64_t>& parts,
                          PrewarmedParts& out);

// The cached tessellation for a shape, built now if it is not there. Pass the
// prewarmed CPU mesh when the prewarm already produced one; the GPU upload
// always happens on the main thread.
std::shared_ptr<BodyTessellation>
LookupOrBuildTessellation(AppState& state, const CADShape& shape,
                          CpuBody* prebuilt);

// A millimetre figure for a message, trimmed and without a signed zero --
// "the axis meets its face -0 mm away" is not a sentence anyone should read.
// Defined in Bom.cpp; FastenerActions.cpp writes the same kind of refusal.
std::string Mm(double v);

} // namespace ui
