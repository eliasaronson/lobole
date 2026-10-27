// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// What importing a triangle mesh will cost, known before it is built
// (docs/interchange/README.md#what-a-mesh-costs). Every triangle of an STL or
// OBJ becomes a face and the faces are sewn, which grows faster than the
// count: a 100k-triangle scan is a minute, a million is hours. The count is
// a scan of the file, no geometry; the cost is a law fitted to imports
// measured on the machine the table in the doc names.

#include <cstdint>
#include <optional>
#include <string>

namespace interchange {

// The wait a person accepts from an import without being asked first. Not a
// property of any mesh: the one number the limit below is solved from.
inline constexpr double kPatientImportSeconds = 60.0;

// .stl or .obj, any case.
bool IsMeshFile(const std::string& path);

// Triangles in a mesh file: a binary STL's header count (when the file's size
// agrees with it), an ASCII STL's `facet`s, an OBJ's faces with a polygon of
// n corners counted as n - 2. Nullopt when `path` is not a mesh or cannot be
// read.
std::optional<uint64_t> CountMeshTriangles(const std::string& path);

// Seconds an import of `triangles` may take: read, faces, sewing and the
// feature's placement, as the slowest meshes measured took them.
double ExpectedMeshImportSeconds(uint64_t triangles);

// The largest count whose expected cost is within kPatientImportSeconds,
// solved from the same law.
uint64_t MeshTriangleLimit();

// "40 s", "about 2 min", "about 3 h".
std::string DescribeSeconds(double seconds);

struct MeshCost {
  uint64_t triangles = 0;
  double expectedSeconds = 0.0;
  bool overLimit = false;
};
// The count and cost of `path`; nullopt when it is not a mesh (or cannot be
// read, which the reader then says in its own words).
std::optional<MeshCost> MeshImportCost(const std::string& path);

// "12,345 triangles" .
std::string DescribeTriangles(uint64_t triangles);

// The refusal of a mesh past the limit: its count, the expected time, the
// limit, and `override` (the caller's spelling of the way past it).
std::string LargeMeshRefusal(const std::string& path, const MeshCost& cost,
                             const std::string& override);

} // namespace interchange
