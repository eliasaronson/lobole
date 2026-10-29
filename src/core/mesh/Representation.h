// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// What an imported mesh (STL, OBJ) becomes: the Import feature's
// `representation` and the `mesh_representation` preference
// (docs/interchange/README.md#recognising-a-faceted-mesh).
//
//   facets    -- one planar face per triangle, sewn: how every mesh import
//                was read before, and what a document that names none reads
//   recognize -- the planes, cylinders, cones and spheres it was tessellated
//                from, rebuilt as a solid of those faces
//   mesh      -- the triangles kept as they are, a mesh body
//                (docs/interchange/README.md#mesh-bodies)
//   auto      -- recognize a mechanical mesh, keep an organic one (or one
//                past the recognition limit) a mesh body, decided on each
//                read from what the file holds
//
// One enum for the feature and the preference.

#include <optional>
#include <string>

namespace mesh {

enum class Representation { Facets, Recognize, Mesh, Auto };

inline const char* ToString(Representation r) {
  switch (r) {
  case Representation::Recognize:
    return "recognize";
  case Representation::Mesh:
    return "mesh";
  case Representation::Auto:
    return "auto";
  default:
    return "facets";
  }
}

inline std::optional<Representation>
RepresentationFromString(const std::string& s) {
  if (s == "facets")
    return Representation::Facets;
  if (s == "recognize")
    return Representation::Recognize;
  if (s == "mesh")
    return Representation::Mesh;
  if (s == "auto")
    return Representation::Auto;
  return std::nullopt;
}

// What a refusal names.
inline const char* AcceptedRepresentations() {
  return "facets, recognize, mesh, auto";
}

// What a new import takes when it names none, and the side count a curved
// surface's facets must reach to be one (mesh::SegmentOptions::minSides).
inline constexpr Representation kDefaultRepresentation = Representation::Auto;
inline constexpr int kDefaultMinSides = 12;

} // namespace mesh
