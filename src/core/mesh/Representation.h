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
//
// One enum for the feature and the preference, so a later representation
// (a mesh body, or `auto` choosing between them) is a value added here.

#include <optional>
#include <string>

namespace mesh {

enum class Representation { Facets, Recognize };

inline const char* ToString(Representation r) {
  return r == Representation::Recognize ? "recognize" : "facets";
}

inline std::optional<Representation>
RepresentationFromString(const std::string& s) {
  if (s == "facets")
    return Representation::Facets;
  if (s == "recognize")
    return Representation::Recognize;
  return std::nullopt;
}

// "facets, recognize": what a refusal names.
inline const char* AcceptedRepresentations() { return "facets, recognize"; }

// What a new import takes when it names none, and the side count a curved
// surface's facets must reach to be one (mesh::SegmentOptions::minSides).
inline constexpr Representation kDefaultRepresentation =
    Representation::Recognize;
inline constexpr int kDefaultMinSides = 12;

} // namespace mesh
