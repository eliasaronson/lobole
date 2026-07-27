#include "cad/CADMeshConverter.h"
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/normal.hpp>
#include <unordered_map>

std::vector<Vertex3D>
CADMeshConverter::ConvertCADMeshToVertices(const CADShape::Mesh& cadMesh) {
  std::vector<Vertex3D> vertices;
  vertices.reserve(cadMesh.vertices.size());

  for (const auto& cadVertex : cadMesh.vertices) {
    Vertex3D vertex;
    vertex.position = cadVertex.position;
    vertex.normal = cadVertex.normal;
    vertices.push_back(vertex);
  }

  return vertices;
}

void CADMeshConverter::CalculateNormals(std::vector<Vertex3D>& vertices,
                                        const std::vector<uint32_t>& indices) {
  // Initialize normals to zero
  for (auto& vertex : vertices) {
    vertex.normal = glm::vec3(0.0f);
  }

  // Calculate face normals and accumulate vertex normals
  for (size_t i = 0; i < indices.size(); i += 3) {
    if (i + 2 >= indices.size())
      break;

    uint32_t i0 = indices[i];
    uint32_t i1 = indices[i + 1];
    uint32_t i2 = indices[i + 2];

    if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size())
      continue;

    glm::vec3 v0 = vertices[i0].position;
    glm::vec3 v1 = vertices[i1].position;
    glm::vec3 v2 = vertices[i2].position;

    glm::vec3 normal = glm::normalize(glm::cross(v1 - v0, v2 - v0));

    vertices[i0].normal += normal;
    vertices[i1].normal += normal;
    vertices[i2].normal += normal;
  }

  // Normalize accumulated normals
  for (auto& vertex : vertices) {
    if (glm::length(vertex.normal) > 0.0f) {
      vertex.normal = glm::normalize(vertex.normal);
    } else {
      vertex.normal = glm::vec3(0.0f, 1.0f, 0.0f); // Default up normal
    }
  }
}