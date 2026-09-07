#pragma once

#include <glm/glm.hpp>
#include <memory>
#include <vector>

struct Vertex3D;
class Camera;

// A B-Rep face: the selectable unit. Tessellation emits each face as a
// contiguous vertex range, which highlighting relies on.
struct Face {
  uint32_t meshId;
  uint32_t faceIndex; // B-Rep face id within the mesh
  uint32_t firstVertex = 0;
  uint32_t vertexCount = 0;
  glm::vec3 normal; // area-weighted average over the triangulation
  glm::vec3 center;
};

struct Ray {
  glm::vec3 origin;
  glm::vec3 direction;
};

struct RayHit {
  bool hit = false;
  float distance = 0.0f;
  glm::vec3 point;
  glm::vec3 normal;
  uint32_t meshId = 0;
  uint32_t faceIndex = 0;
};

// A picked B-Rep edge (toponaming T5 / fillet M7). `edgeIndex` follows the
// same deduplicated order as CADShape::GenerateEdgePolylines and
// ElementMap::edgeNames.
struct EdgeHit {
  bool hit = false;
  float distance = 0.0f; // along the ray to the closest point on the edge
  glm::vec3 point;       // that closest point (on the edge polyline)
  uint32_t meshId = 0;
  uint32_t edgeIndex = 0;
};

class FaceSelector {
public:
  FaceSelector();
  ~FaceSelector();

  // Mesh management. triangleFaces maps each triangle to its B-Rep face id
  // (from CADShape::Mesh); if empty, every triangle is its own face.
  // edgePolylines (from CADShape::GenerateEdgePolylines) enables PickEdge;
  // its indices are the mesh's edgeIndex space.
  void AddMesh(uint32_t meshId, const std::vector<Vertex3D>& vertices,
               const std::vector<uint32_t>& indices,
               const std::vector<uint32_t>& triangleFaces = {},
               const glm::mat4& transform = glm::mat4(1.0f),
               const std::vector<std::vector<glm::vec3>>& edgePolylines = {});
  void UpdateMeshTransform(uint32_t meshId, const glm::mat4& transform);
  void RemoveMesh(uint32_t meshId);
  void Clear();

  // Selection
  RayHit PickFace(const Ray& ray) const;
  // The edge nearest the ray within a small angular tolerance (~a few
  // pixels at any zoom), rejecting edges hidden behind a surface the same
  // ray hits first. Misses when no mesh carries edge polylines.
  //
  // That rejection needs the face hit, which the hover path has usually just
  // computed: pass it as `occluder` rather than paying for a second sweep.
  // The no-argument form picks one itself.
  EdgeHit PickEdge(const Ray& ray) const;
  EdgeHit PickEdge(const Ray& ray, const RayHit& occluder) const;
  Ray ScreenToWorldRay(float screenX, float screenY, int screenWidth,
                       int screenHeight, const Camera& camera) const;

  // Face access
  const Face* GetSelectedFace() const;
  const Face* GetHoveredFace() const;
  void SetSelectedFace(uint32_t meshId, uint32_t faceIndex);
  void SetHoveredFace(uint32_t meshId, uint32_t faceIndex);
  void ClearSelection();
  void ClearHover();

  // Utility
  glm::vec3 GetFaceNormal(uint32_t meshId, uint32_t faceIndex) const;
  glm::vec3 GetFaceCenter(uint32_t meshId, uint32_t faceIndex) const;
  // Vertex range of a face within its mesh's vertex buffer (for highlight
  // rendering). Returns false if the face doesn't exist.
  bool GetFaceVertexRange(uint32_t meshId, uint32_t faceIndex,
                          uint32_t& firstVertex, uint32_t& vertexCount) const;

private:
  struct MeshData {
    uint32_t meshId;
    std::vector<Vertex3D> vertices;
    std::vector<uint32_t> indices;
    std::vector<uint32_t> triangleFaces; // B-Rep face id per triangle
    std::vector<Face> faces;
    // B-Rep edge polylines in edgeIndex order (untransformed, like
    // vertices); empty when the mesh has no pickable edges.
    std::vector<std::vector<glm::vec3>> edgePolylines;
    glm::mat4 transform = glm::mat4(1.0f);

    // Everything below is DERIVED from the three above and is rebuilt by
    // ExtractFaces, which is already the one place that runs when either the
    // geometry or the placement changes. Nothing else may set them.

    // World-space AABB. A pick tests this first: a ray crosses one or two
    // parts of an assembly and misses the rest, and without it every mesh
    // paid for every triangle whether the ray came near it or not.
    glm::vec3 boundsMin{0.0f}, boundsMax{0.0f};
    bool hasBounds = false;

    // The ray goes into MESH space rather than the vertices into world
    // space: three mat4 x vec4 per triangle was about as expensive as the
    // intersection test it fed. `invTransform` carries the ray in;
    // `normalToWorld` carries the hit normal back out.
    glm::mat4 invTransform = glm::mat4(1.0f);
    // inverse-transpose, times sign(det). cross(Ma, Mb) = det(M) M^-T
    // cross(a, b), so a MIRRORED placement needs that sign or its faces
    // report inward-facing normals — and a normal is what orients a sketch
    // on a face.
    glm::mat3 normalToWorld{1.0f};
  };

  // Does the ray meet this world-space AABB at all (slab test)?
  static bool RayHitsBounds(const Ray& ray, const glm::vec3& boundsMin,
                            const glm::vec3& boundsMax);

  // Ray-triangle intersection
  bool RayTriangleIntersect(const Ray& ray, const glm::vec3& v0,
                            const glm::vec3& v1, const glm::vec3& v2,
                            float& distance, glm::vec3& hitPoint) const;

  // Face extraction
  void ExtractFaces(MeshData& meshData);

  std::vector<std::unique_ptr<MeshData>> m_meshes;

  // Current selection state
  uint32_t m_selectedMeshId = 0;
  uint32_t m_selectedFaceIndex = 0;
  bool m_hasSelection = false;

  uint32_t m_hoveredMeshId = 0;
  uint32_t m_hoveredFaceIndex = 0;
  bool m_hasHover = false;
};