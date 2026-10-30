// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// What importing a triangle mesh will cost, known before it is built
// (docs/interchange/README.md#what-a-mesh-costs). Every triangle of an STL or
// OBJ becomes a face and the faces are sewn, which grows faster than the
// count: a 100k-triangle scan is a minute, a million is hours. The count is
// a scan of the file, no geometry; the cost is a law fitted to imports
// measured on the machine the table in the doc names.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace interchange {

// The wait a person accepts from an import without being asked first. Not a
// property of any mesh: the one number the limit below is solved from.
inline constexpr double kPatientImportSeconds = 60.0;

// .stl, .obj, .ply, .glb or .gltf, any case.
bool IsMeshFile(const std::string& path);

// Triangles in a mesh file: a binary STL's header count (when the file's size
// agrees with it), an ASCII STL's `facet`s, an OBJ's faces with a polygon of
// n corners counted as n - 2, a PLY's header face count, a glTF's placed
// primitives' (mesh::CountPlyFaces, CountGltfTriangles). Nullopt when `path` is
// not a mesh or cannot be read.
std::optional<uint64_t> CountMeshTriangles(const std::string& path);

// Seconds an import of `triangles` may take: read, faces, sewing and the
// feature's placement, as the slowest meshes measured took them.
double ExpectedMeshImportSeconds(uint64_t triangles);

// The largest count whose expected cost is within kPatientImportSeconds,
// solved from the same law.
uint64_t MeshTriangleLimit();

// What recognition costs (docs/interchange/README.md#mesh-bodies). Its
// segmentation is superlinear on an organic mesh -- the slowest fixture
// measured, the figure, took t = 2.0 s (n / 10^4)^2.2 -- and a mechanical
// one is cheaper, so this is what it MAY take.
double ExpectedRecognitionSeconds(uint64_t triangles);
// The count at which that reaches kPatientImportSeconds (about 47,000):
// `auto` recognises below it and keeps a mesh body at or above it.
uint64_t RecognitionTriangleLimit();

// What `auto` reads off a segmentation to call a mesh mechanical (rebuilt)
// or organic (kept a mesh body): the share of its surface that lies in
// LARGE regions, each at least kLargeRegionShare of the whole. Measured on
// the fixtures (the hidden test "[mesh-auto-measure]"): every mechanical
// one 100% at any tessellation, a 12-triangle box included; the organic
// ones 0 to 13% at 10k triangles and up, 52 to 66% at 2k; the clean torus
// 83 to 100%. A count of triangles a region does not separate them: a
// coarse mechanical export holds 2 to 22 a region, as an organic one holds
// 1 to 3. Both shares are fractions, not lengths.
inline constexpr double kLargeRegionShare = 1e-3;
inline constexpr double kMechanicalAreaShare = 0.9;

// The tessellation cache's budget (ui/AppActionsInternal.h asserts it is
// its kTessellationCacheBudgetBytes; core/ cannot include ui/).
inline constexpr size_t kMeshBodyBudgetBytes = 256u * 1024 * 1024;

// The largest mesh body this app holds on screen: what its vertex and index
// buffers, counted twice (the outline asset uploads them again), fit in the
// tessellation budget. A mesh past it is refused, naming the count to
// decimate to.
uint64_t MeshBodyTriangleLimit();

// The refusal of a mesh body past MeshBodyTriangleLimit.
std::string MeshBodyTooLarge(const std::string& path, uint64_t triangles);

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
