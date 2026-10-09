// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "cad/CADKernel.h"

#include <glm/glm.hpp>

#include <string>
#include <vector>

// A GLB with a glTF ANIMATION (docs/assemblies/README.md#motion-studies): one
// node per moving thing, its mesh once, and a translation + rotation channel
// sampled at the study's keyframes. OpenCASCADE's RWGltf_CafWriter writes a
// static scene and nothing else, so this is a writer of our own — small,
// because glTF 2.0 is a JSON index over one binary buffer and the whole
// animation is three accessors per node (times, translations, rotations) with
// LINEAR interpolation, which for a rigid mechanism is exactly right.
//
// Z-up in, Y-up out: glTF is Y-up by spec, so the scene root carries the
// one rotation that converts and every node below it stays in the world
// coordinates the document uses — the same boundary MeshExport draws for
// the static export.
namespace glb {

struct Mesh {
  std::vector<glm::vec3> positions;
  std::vector<glm::vec3> normals;
  std::vector<uint32_t> indices;
  glm::vec3 color{0.7f};
};

struct Node {
  std::string name;
  Mesh mesh;
  // One placement per keyframe, in world space; size() == times.size().
  std::vector<glm::mat4> keyframes;
};

// Appends `shape`'s triangulation, placed by `local`, to `mesh`.
void AppendShape(Mesh& mesh, const CADShape& shape, const glm::mat4& local);

// Writes the scene. `times` are the keyframe times in seconds, ascending.
// Every node must carry exactly times.size() keyframes. Returns false with
// `error` set on a bad argument or an unwritable path.
bool Write(const std::string& path, const std::vector<Node>& nodes,
           const std::vector<double>& times, std::string* error);

} // namespace glb
