// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// PLY and glTF files written from a TriMesh, for the readers' tests and the
// reader fuzz (docs/interchange/README.md#ply-and-gltf). Written by hand, as
// a scanning app's exporter would, so a test knows every byte it reads.

#include "core/mesh/TriMesh.h"

#include <glm/glm.hpp>

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace scanfiles {

enum class PlyEncoding { Ascii, BinaryLittle, BinaryBig };

struct PlyOptions {
  PlyEncoding encoding = PlyEncoding::Ascii;
  // Per-vertex uchar colours: one for all, or one each (size = vertices).
  std::vector<std::array<uint8_t, 3>> colours;
  // Faces as written; empty = the mesh's triangles.
  std::vector<std::vector<uint32_t>> faces;
  bool withFaces = true; // false: a point cloud
};

std::string Ply(const mesh::TriMesh& m, const PlyOptions& options = {});

// One glTF node: which mesh, and its matrix (column-major, metres).
struct GltfNode {
  int mesh = 0;
  glm::dmat4 matrix{1.0};
  std::vector<int> children;
};

struct GltfOptions {
  // Each a mesh of float32 positions (metres) and uint32 indices.
  std::vector<mesh::TriMesh> meshes;
  std::vector<GltfNode> nodes; // empty = one identity node of mesh 0
  std::vector<int> sceneRoots; // empty = node 0
  std::vector<std::string> extensionsRequired;
  std::string generator = "scanfiles";
  int mode = 4; // TRIANGLES; 0 is POINTS
};

// A binary glTF.
std::string Glb(const GltfOptions& options);
// JSON glTF whose buffer is `binName`, and that buffer's bytes.
std::pair<std::string, std::string> GltfWithBin(const GltfOptions& options,
                                                const std::string& binName);

} // namespace scanfiles
