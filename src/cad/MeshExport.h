// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "cad/CADKernel.h"

#include <glm/glm.hpp>

#include <string>
#include <vector>

// Visual-mesh export (interchange X2): glTF (.glb/.gltf), OBJ, and PLY via
// OpenCASCADE's XCAF-based writers. One shape→XCAF bridge (names, placements,
// PBR materials) serves all three formats — glTF is natively
// metallic-roughness, so exported parts keep their real appearance, not just
// a tint. Unlike STEP/BREP this exports the *tessellation*, for viewers
// (Blender, browsers, engines), not for CAD round-trips.
namespace MeshExport {

// One exported node: a body, placed and styled. For a part document the
// transform is identity; for an assembly it is the occurrence's solved
// placement (occurrences of one part share the underlying shape, so the
// writers emit instanced geometry, not copies).
struct Item {
  CADShape shape;
  std::string name;
  glm::mat4 transform{1.0f}; // rigid placement (translate ∘ rotate)
  glm::vec3 baseColor{0.8f}; // linear RGB
  float metallic = 0.0f;
  float roughness = 0.5f;
  // Faces styled apart from the item (a face appearance override, v164).
  // `face` counts TopExp_Explorer order. Written as a style on the face's
  // XCAF sub-shape, so the glTF writer gives those faces their own material
  // (and so their own primitive).
  struct FaceStyle {
    int face = 0;
    glm::vec3 baseColor{0.8f};
    float metallic = 0.0f;
    float roughness = 0.5f;
  };
  std::vector<FaceStyle> faces;
};

// Writes `items` to `path`; the format is chosen by the file extension
// (.glb binary glTF — the default choice, .gltf JSON glTF, .obj, .ply).
// Shapes are tessellated at `deflection` (the app's display default, 0.1).
// Returns false and fills *error on an unknown extension, empty/invalid
// input, or a writer failure.
bool SaveVisual(const std::string& path, const std::vector<Item>& items,
                double deflection = 0.1, std::string* error = nullptr);

} // namespace MeshExport
