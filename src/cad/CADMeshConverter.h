// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "cad/CADKernel.h"
#include "render/MeshRenderer.h"
#include <vector>

class CADMeshConverter {
public:
  static std::vector<Vertex3D>
  ConvertCADMeshToVertices(const CADShape::Mesh& cadMesh);
  static void CalculateNormals(std::vector<Vertex3D>& vertices,
                               const std::vector<uint32_t>& indices);
};