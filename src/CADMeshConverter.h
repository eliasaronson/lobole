#pragma once

#include "CADObject.h"
#include "MeshRenderer.h"
#include <vector>

class CADMeshConverter {
public:
    static std::vector<Vertex3D> ConvertCADMeshToVertices(const CADShape::Mesh& cadMesh);
    static void CalculateNormals(std::vector<Vertex3D>& vertices, const std::vector<uint32_t>& indices);
};