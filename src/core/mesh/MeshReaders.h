// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// Mesh files read straight into a TriMesh, with no B-Rep built
// (docs/interchange/README.md#recognising-a-faceted-mesh).

#include "core/mesh/TriMesh.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mesh {

// Polled while reading; true stops the read, which then fails with
// "cancelled". May be empty.
using Cancelled = std::function<bool()>;

// A binary or ASCII STL: binary when the file's size is exactly what its
// header's triangle count says, ASCII when it starts with `solid` and is not,
// refused otherwise. The triangles are welded (FromSoup) at the precision
// the file was written at: float32 for binary, the digits printed for ASCII.
// False with `error` naming the file and what is wrong.
bool ReadStl(const std::string& path, TriMesh& out, std::string& error,
             const Cancelled& cancelled = {});

// The same from bytes already in memory, `name` used in the error.
bool ReadStlBytes(const std::string& bytes, const std::string& name,
                  TriMesh& out, std::string& error,
                  const Cancelled& cancelled = {});

// A Wavefront OBJ's vertices and faces (`v`, `f`; a polygon fanned into
// triangles, negative indices counted back, texture and normal indices
// ignored), every object in one mesh, welded at the precision of its
// printed digits. Groups, materials and everything else are skipped. False
// with `error` naming the line on a face naming a vertex not there.
bool ReadObj(const std::string& path, TriMesh& out, std::string& error,
             const Cancelled& cancelled = {});
bool ReadObjBytes(const std::string& bytes, const std::string& name,
                  TriMesh& out, std::string& error,
                  const Cancelled& cancelled = {});

// What a PLY or glTF says besides its triangles.
struct ReadExtras {
  // A PLY's vertex colour, 0..1, when every vertex has the same one: the
  // body's display colour. Varied colours are dropped, and `notes` says so.
  std::optional<glm::vec3> color;
  // One sentence each, for the import's warning: what was dropped.
  std::vector<std::string> notes;
  // Labelled as a person would find them in the file ({"asset generator",
  // "Blender 4.2"}), for the up axis (interchange::DecideUpAxis).
  std::vector<std::pair<std::string, std::string>> header;
};

// A PLY (docs/interchange/README.md#ply-and-gltf): ASCII or binary of either
// endianness, read by the vendored miniply. Vertex `x y z` and a face list
// (`vertex_indices` or `vertex_index`), each polygon fanned; a uniform vertex
// colour into `extras`. Welded at half the float32 spacing, as an OBJ is at
// half its digit. Refused, naming the file: a header that promises more
// rows than the file holds, no faces (a point cloud), a face naming a vertex
// not there, data that ends early.
bool ReadPly(const std::string& path, TriMesh& out, std::string& error,
             const Cancelled& cancelled = {}, ReadExtras* extras = nullptr);

// A glTF 2.0, binary (.glb) or JSON (.gltf, its buffers beside it or
// embedded), read by the vendored cgltf. The default scene's node tree is
// flattened: every mesh placed by its node's world transform, all in one
// TriMesh. Metres, as the format defines, are converted to millimetres; the
// file's Y-up is left to the import's up axis. Triangles, strips and fans
// are read; points and lines are skipped and `extras` says so. Refused,
// naming the file: damage cgltf_validate finds, a buffer that cannot be read,
// glTF 1.0, and a required extension that is not read (Draco and meshopt
// compression among them).
bool ReadGltf(const std::string& path, TriMesh& out, std::string& error,
              const Cancelled& cancelled = {}, ReadExtras* extras = nullptr);

// Triangles counted without reading the data: a PLY's face count from its
// header (a polygon counted once), a glTF's from its accessors over the
// placed meshes. Nothing when the file cannot be read as one.
std::optional<uint64_t> CountPlyFaces(const std::string& path);
std::optional<uint64_t> CountGltfTriangles(const std::string& path);

// The files a glTF reads besides itself: each buffer named by a relative
// uri, resolved beside it as cgltf_load_buffers resolves it. An embedded
// (data:) buffer and a .glb's own chunk are not files. Empty when the JSON
// cannot be parsed; the read then refuses it. The import's content hash
// covers these (ImportFeature::ContentHash), so a changed .bin is a changed
// import.
std::vector<std::string> GltfExternalFiles(const std::string& path);

} // namespace mesh
