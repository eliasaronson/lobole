#pragma once

#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <vector>

// View-independent edge adjacency + per-view silhouette classification for
// the wireframe overlay. Pure geometry (glm only, no Vulkan) so it compiles
// into designer_core and designer_tests can cover it; OutlineRenderer owns
// the GPU side.
//
// The wireframe a view sees is [static prefix | silhouette suffix]:
//  - static: boundary edges (one adjacent triangle — includes all B-Rep face
//    borders, since tessellation doesn't share vertices across faces) plus
//    feature edges (dihedral sharper than the threshold). View-independent,
//    computed once per mesh here.
//  - silhouette: smooth internal edges whose two triangles face the camera
//    differently. Reclassified per view by AppendSilhouetteIndices as one
//    flat pass over precomputed per-triangle data — no maps, no allocations.
namespace outline {

struct EdgeAdjacency {
  // A smooth internal edge (exactly two adjacent triangles, dihedral below
  // the feature threshold) — the only silhouette candidates.
  struct SmoothEdge {
    uint32_t v0, v1; // vertex indices (line endpoints)
    uint32_t triA, triB;
  };

  std::vector<glm::vec3> triNormal;   // unit, mesh-local; zero if degenerate
  std::vector<glm::vec3> triCentroid; // mesh-local
  std::vector<SmoothEdge> smoothEdges;

  // Static wireframe as line-list indices: feature + boundary edges, drawn
  // in every view (the selection the old ExtractEdges made).
  std::vector<uint32_t> staticIndices;

  // Worst case any view's full index list can reach — sizes the GPU buffer.
  size_t MaxIndexCount() const {
    return staticIndices.size() + 2 * smoothEdges.size();
  }
};

// Builds adjacency from a triangle mesh. `positions` walks `positionCount`
// vertices `positionStride` bytes apart (pass sizeof(Vertex3D) to read
// positions straight out of a renderer vertex array). Internal edges whose
// face-normal dot is below `featureCos` are feature edges (0.5 = 60°); if
// that leaves almost nothing, a lenient 0.8 pass is retried (the old
// extractor's fallback so simple smooth meshes still show structure). Edges
// with >2 adjacent triangles (non-manifold) are ignored, as before.
EdgeAdjacency BuildEdgeAdjacency(const glm::vec3* positions,
                                 size_t positionCount, size_t positionStride,
                                 const uint32_t* indices, size_t indexCount,
                                 float featureCos = 0.5f);

// Reused across calls so steady-state classification allocates nothing.
struct SilhouetteScratch {
  std::vector<uint8_t> facing;
};

// Appends line-list indices for the silhouette edges seen from
// `cameraLocal`, the camera position transformed into mesh-local space
// (inverse(model) * cameraWorld) — equivalent to the world-space facing
// test without touching any vertex.
void AppendSilhouetteIndices(const EdgeAdjacency& adj,
                             const glm::vec3& cameraLocal,
                             SilhouetteScratch& scratch,
                             std::vector<uint32_t>& out);

} // namespace outline
