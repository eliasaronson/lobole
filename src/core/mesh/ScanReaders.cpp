// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// PLY and glTF, the formats phone scanning apps export besides OBJ and STL
// (docs/interchange/README.md#ply-and-gltf), read into a TriMesh with no
// B-Rep built.

#include "core/mesh/MeshReaders.h"

#include "thirdparty/miniply.h"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#define CGLTF_IMPLEMENTATION
#include "thirdparty/cgltf.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <memory>
#include <set>

namespace mesh {

namespace {

constexpr size_t kCancelEvery = 1u << 16; // triangles between cancel polls

std::string Quote(const std::string& path) { return "'" + path + "'"; }

// ---- PLY -------------------------------------------------------------------

size_t PlySize(miniply::PLYPropertyType t) {
  using T = miniply::PLYPropertyType;
  switch (t) {
  case T::Char:
  case T::UChar:
    return 1;
  case T::Short:
  case T::UShort:
    return 2;
  case T::Int:
  case T::UInt:
  case T::Float:
    return 4;
  case T::Double:
    return 8;
  case T::None:
    break;
  }
  return 0;
}

// The fewest bytes the rows `elem` promises can be written in: a binary row
// is its scalars and its list counts, an ASCII value at least a digit and a
// separator. A header promising more than the file holds is refused before
// anything is allocated for it -- miniply sizes its buffers by the header.
uint64_t LeastBytes(const miniply::PLYElement& elem, bool ascii) {
  uint64_t row = 0;
  for (const miniply::PLYProperty& p : elem.properties) {
    if (ascii)
      row += 2;
    else
      row += PlySize(p.countType != miniply::PLYPropertyType::None ? p.countType
                                                                   : p.type);
  }
  return uint64_t{elem.count} * row;
}

std::string Plural(uint64_t n, const char* one, const char* many) {
  return std::to_string(n) + " " + (n == 1 ? one : many);
}

// Opens `path` with miniply and checks its header against its size.
bool OpenPly(const std::string& path, std::unique_ptr<miniply::PLYReader>& out,
             std::string& error) {
  std::error_code ec;
  const uintmax_t size = std::filesystem::file_size(path, ec);
  if (ec) {
    error = Quote(path) + " cannot be read";
    return false;
  }
  out = std::make_unique<miniply::PLYReader>(path.c_str());
  if (!out->valid()) {
    error = Quote(path) +
            " is not a PLY: it does not start with a `ply` line, a `format` "
            "of ascii, binary_little_endian or binary_big_endian, and "
            "`element`s ending in `end_header`";
    return false;
  }
  const bool ascii = out->file_type() == miniply::PLYFileType::ASCII;
  uint64_t least = 0;
  for (uint32_t i = 0; i < out->num_elements(); i++)
    least += LeastBytes(*out->get_element(i), ascii);
  if (least > size) {
    std::string promised;
    for (uint32_t i = 0; i < out->num_elements(); i++) {
      const miniply::PLYElement& e = *out->get_element(i);
      promised += (promised.empty() ? "" : ", ") + std::to_string(e.count) +
                  " " + e.name;
    }
    error = Quote(path) + ": its header promises " + promised +
            ", more than its " + std::to_string(size) + " bytes can hold";
    return false;
  }
  return true;
}

float ColourScale(miniply::PLYPropertyType t) {
  using T = miniply::PLYPropertyType;
  switch (t) {
  case T::UChar:
    return 1.0f / 255.0f;
  case T::Char:
    return 1.0f / 127.0f;
  case T::UShort:
    return 1.0f / 65535.0f;
  case T::Short:
    return 1.0f / 32767.0f;
  default:
    return 1.0f; // float and double are 0..1 already
  }
}

} // namespace

bool ReadPly(const std::string& path, TriMesh& out, std::string& error,
             const Cancelled& cancelled, ReadExtras* extras) {
  out = {};
  ReadExtras local;
  ReadExtras& ex = extras != nullptr ? *extras : local;
  ex = {};
  std::unique_ptr<miniply::PLYReader> reader;
  if (!OpenPly(path, reader, error))
    return false;
  if (reader->find_element(miniply::kPLYVertexElement) ==
      miniply::kInvalidIndex) {
    error = Quote(path) + " has no `vertex` element";
    return false;
  }
  if (reader->find_element(miniply::kPLYFaceElement) ==
      miniply::kInvalidIndex) {
    error = Quote(path) +
            " has no `face` element: a point cloud, not a mesh. Mesh it "
            "first (MeshLab: Surface Reconstruction) and import that";
    return false;
  }
  std::vector<glm::dvec3> verts;
  std::vector<int32_t> corners;    // every face's corners, in order
  std::vector<uint32_t> perFace;   // corners a face
  std::vector<std::string> unread; // elements skipped, with their counts
  bool haveVerts = false, haveFaces = false;
  const auto damaged = [&](const char* what) {
    error = Quote(path) + ": its " + what +
            " data ends early or does not match its header";
    return false;
  };
  for (; reader->has_element(); reader->next_element()) {
    if (cancelled && cancelled()) {
      error = "cancelled";
      return false;
    }
    const miniply::PLYElement& elem = *reader->element();
    if (reader->element_is(miniply::kPLYVertexElement) && !haveVerts) {
      haveVerts = true;
      uint32_t pos[3];
      if (!reader->find_pos(pos)) {
        error = Quote(path) + ": its vertices have no `x y z`";
        return false;
      }
      for (uint32_t k : pos)
        if (elem.properties[k].countType != miniply::PLYPropertyType::None) {
          error = Quote(path) + ": a vertex coordinate is declared a list";
          return false;
        }
      if (!reader->load_element())
        return damaged("vertex");
      std::vector<double> xyz(size_t{reader->num_rows()} * 3);
      if (!reader->extract_properties(pos, 3, miniply::PLYPropertyType::Double,
                                      xyz.data()))
        return damaged("vertex");
      verts.resize(reader->num_rows());
      for (size_t i = 0; i < verts.size(); i++) {
        verts[i] = {xyz[3 * i], xyz[3 * i + 1], xyz[3 * i + 2]};
        if (!std::isfinite(verts[i].x) || !std::isfinite(verts[i].y) ||
            !std::isfinite(verts[i].z)) {
          error = Quote(path) + ": vertex " + std::to_string(i) +
                  " has a coordinate that is not a number";
          return false;
        }
      }
      // A colour: the body's when every vertex has the same one.
      uint32_t col[3];
      bool scalar = reader->find_color(col);
      for (int k = 0; scalar && k < 3; k++)
        scalar =
            elem.properties[col[k]].countType == miniply::PLYPropertyType::None;
      if (scalar && !verts.empty()) {
        std::vector<float> rgb(verts.size() * 3);
        if (!reader->extract_properties(col, 3, miniply::PLYPropertyType::Float,
                                        rgb.data()))
          return damaged("vertex");
        bool uniform = true;
        for (size_t i = 3; i < rgb.size() && uniform; i++)
          uniform = rgb[i] == rgb[i % 3];
        if (uniform) {
          glm::vec3 c;
          for (int k = 0; k < 3; k++)
            c[k] = std::clamp(rgb[size_t(k)] *
                                  ColourScale(elem.properties[col[k]].type),
                              0.0f, 1.0f);
          ex.color = c;
        } else {
          ex.notes.push_back("its vertex colours vary and are not kept: the "
                             "body takes one colour");
        }
      }
    } else if (reader->element_is(miniply::kPLYFaceElement) && !haveFaces) {
      haveFaces = true;
      uint32_t idx[1];
      if (!reader->find_indices(idx) ||
          elem.properties[idx[0]].countType == miniply::PLYPropertyType::None) {
        error = Quote(path) +
                ": its faces have no `vertex_indices` (or `vertex_index`) "
                "list";
        return false;
      }
      using T = miniply::PLYPropertyType;
      const T itype = elem.properties[idx[0]].type;
      if (itype == T::Float || itype == T::Double) {
        error = Quote(path) + ": its face indices are declared real numbers";
        return false;
      }
      uint32_t col[3];
      if (reader->find_color(col))
        ex.notes.push_back("its face colours are not kept: the body takes "
                           "one colour");
      if (!reader->load_element())
        return damaged("face");
      const miniply::PLYProperty& prop = reader->element()->properties[idx[0]];
      if (prop.rowCount.size() != reader->num_rows())
        return damaged("face");
      uint64_t total = 0;
      for (uint32_t n : prop.rowCount)
        total += n;
      if (prop.listData.size() != total * PlySize(itype))
        return damaged("face");
      perFace.assign(prop.rowCount.begin(), prop.rowCount.end());
      corners.resize(total);
      if (total > 0 &&
          !reader->extract_list_property(idx[0], T::Int, corners.data()))
        return damaged("face");
    } else {
      unread.push_back(Plural(elem.count, elem.name.c_str(),
                              (elem.name + " elements").c_str()));
    }
  }
  if (!reader->valid())
    return damaged("element");
  if (!unread.empty()) {
    std::string list;
    for (const std::string& u : unread)
      list += (list.empty() ? "" : ", ") + u;
    ex.notes.push_back("not read: " + list);
  }
  // Each polygon fanned from its first corner, as an OBJ's is.
  std::vector<glm::dvec3> soup;
  size_t at = 0;
  for (size_t f = 0; f < perFace.size(); f++) {
    const uint32_t n = perFace[f];
    if (n < 3) {
      error = Quote(path) + ": face " + std::to_string(f) + " has " +
              std::to_string(n) + " corners, fewer than three";
      return false;
    }
    for (uint32_t k = 0; k < n; k++) {
      const int32_t v = corners[at + k];
      if (v < 0 || size_t(v) >= verts.size()) {
        error = Quote(path) + ": face " + std::to_string(f) + " names vertex " +
                std::to_string(v) + " of " + std::to_string(verts.size());
        return false;
      }
    }
    for (uint32_t k = 1; k + 1 < n; k++) {
      soup.push_back(verts[size_t(corners[at])]);
      soup.push_back(verts[size_t(corners[at + k])]);
      soup.push_back(verts[size_t(corners[at + k + 1])]);
    }
    at += n;
    if ((f + 1) % kCancelEvery == 0 && cancelled && cancelled()) {
      error = "cancelled";
      return false;
    }
  }
  if (soup.empty()) {
    error = Quote(path) + " holds no faces" +
            (verts.empty()
                 ? std::string()
                 : ", only " + Plural(verts.size(), "vertex", "vertices") +
                       ": a point cloud, not a mesh. Mesh it first "
                       "(MeshLab: Surface Reconstruction) and "
                       "import that");
    return false;
  }
  // Shared by index, so the soup welds back exactly; half the float32
  // spacing joins nothing written apart. A PLY's coordinates are float32
  // nearly always (a double one is no coarser), so that spacing is what
  // the fits read.
  const double quantum = Float32Quantum(soup);
  out = FromSoup(soup, 0.5 * quantum);
  out.quantum = quantum;
  if (out.triangles.empty()) {
    error = Quote(path) + " holds only degenerate triangles";
    return false;
  }
  return true;
}

std::optional<uint64_t> CountPlyFaces(const std::string& path) {
  std::unique_ptr<miniply::PLYReader> reader;
  std::string error;
  if (!OpenPly(path, reader, error))
    return std::nullopt;
  const uint32_t face = reader->find_element(miniply::kPLYFaceElement);
  if (face == miniply::kInvalidIndex)
    return std::nullopt;
  return reader->get_element(face)->count;
}

// ---- glTF ------------------------------------------------------------------

namespace {

struct GltfDeleter {
  void operator()(cgltf_data* d) const { cgltf_free(d); }
};
using GltfData = std::unique_ptr<cgltf_data, GltfDeleter>;

// Extensions a file may require that change nothing about where its
// triangles are: materials, textures, lights, and quantized attributes,
// which cgltf_accessor_unpack_floats reads. Any other required one is
// refused by name.
bool HarmlessExtension(const std::string& name) {
  static const char* kPrefixes[] = {"KHR_materials_", "KHR_texture_",
                                    "EXT_texture_"};
  for (const char* p : kPrefixes)
    if (name.rfind(p, 0) == 0)
      return true;
  return name == "KHR_mesh_quantization" || name == "KHR_lights_punctual";
}

std::string ResultText(cgltf_result r) {
  switch (r) {
  case cgltf_result_data_too_short:
    return "it ends early, or a part of it is shorter than it says";
  case cgltf_result_unknown_format:
    return "it is neither a binary glTF (GLB) nor glTF JSON";
  case cgltf_result_invalid_json:
    return "its JSON is damaged";
  case cgltf_result_invalid_gltf:
    return "it is not valid glTF (an index, a type or a reference out of "
           "place)";
  case cgltf_result_legacy_gltf:
    return "it is glTF 1.0, which is not read: re-export it as glTF 2.0";
  case cgltf_result_out_of_memory:
    return "it needs more memory than there is";
  case cgltf_result_file_not_found:
  case cgltf_result_io_error:
    return "it cannot be read";
  default:
    return "cgltf refused it";
  }
}

// Parsed, checked for required extensions; with `buffers`, its buffers
// loaded and the whole file validated.
bool OpenGltf(const std::string& path, bool buffers, GltfData& out,
              std::string& error) {
  cgltf_options options{};
  cgltf_data* raw = nullptr;
  cgltf_result r = cgltf_parse_file(&options, path.c_str(), &raw);
  out.reset(raw);
  if (r != cgltf_result_success) {
    error = Quote(path) + ": " + ResultText(r);
    return false;
  }
  for (cgltf_size i = 0; i < out->extensions_required_count; i++) {
    const std::string name = out->extensions_required[i] != nullptr
                                 ? out->extensions_required[i]
                                 : std::string();
    if (!HarmlessExtension(name)) {
      error = Quote(path) + " requires the extension " + name +
              ", which is not read" +
              (name == "KHR_draco_mesh_compression" ||
                       name == "EXT_meshopt_compression"
                   ? ": its triangles are compressed. Re-export it "
                     "uncompressed (Blender: glTF export, Compression off)"
                   : std::string());
      return false;
    }
  }
  if (!buffers) {
    // The declared sizes, and no cycle in the node tree, which the walk
    // needs; the index bounds wait for the data.
    r = cgltf_validate(out.get());
    if (r != cgltf_result_success) {
      error = Quote(path) + ": " + ResultText(r);
      return false;
    }
    return true;
  }
  r = cgltf_load_buffers(&options, out.get(), path.c_str());
  if (r != cgltf_result_success) {
    std::string which;
    for (cgltf_size i = 0; i < out->buffers_count; i++)
      if (out->buffers[i].data == nullptr && out->buffers[i].uri != nullptr) {
        which = std::string(out->buffers[i].uri).substr(0, 64);
        break;
      }
    error = Quote(path) + ": a buffer it names cannot be read" +
            (which.empty() ? std::string() : " (`" + which + "`)") + ": " +
            ResultText(r);
    return false;
  }
  r = cgltf_validate(out.get());
  if (r != cgltf_result_success) {
    error = Quote(path) + ": " + ResultText(r);
    return false;
  }
  return true;
}

// The placed meshes of the default scene (or the first, or every root node
// when there is none), each with its world transform, in double.
struct PlacedMesh {
  const cgltf_mesh* mesh;
  glm::dmat4 world;
  const cgltf_node* node;
};

std::vector<PlacedMesh> PlacedMeshes(const cgltf_data& data) {
  std::vector<std::pair<const cgltf_node*, glm::dmat4>> stack;
  const cgltf_scene* scene = data.scene != nullptr   ? data.scene
                             : data.scenes_count > 0 ? &data.scenes[0]
                                                     : nullptr;
  if (scene != nullptr) {
    for (cgltf_size i = scene->nodes_count; i-- > 0;)
      if (scene->nodes[i] != nullptr)
        stack.push_back({scene->nodes[i], glm::dmat4(1.0)});
  } else {
    for (cgltf_size i = data.nodes_count; i-- > 0;)
      if (data.nodes[i].parent == nullptr)
        stack.push_back({&data.nodes[i], glm::dmat4(1.0)});
  }
  std::vector<PlacedMesh> out;
  // cgltf refuses a node of two parents and cgltf_validate a cycle, so the
  // walk ends; it is a stack, so a deep chain cannot overflow ours, and it
  // visits no more nodes than there are.
  size_t visits = 0;
  while (!stack.empty() && visits++ < data.nodes_count) {
    const auto [node, parent] = stack.back();
    stack.pop_back();
    float local[16];
    cgltf_node_transform_local(node, local);
    glm::dmat4 m(1.0);
    for (int c = 0; c < 4; c++)
      for (int r = 0; r < 4; r++)
        m[c][r] = double(local[4 * c + r]);
    const glm::dmat4 world = parent * m;
    if (node->mesh != nullptr)
      out.push_back({node->mesh, world, node});
    for (cgltf_size i = node->children_count; i-- > 0;)
      if (node->children[i] != nullptr)
        stack.push_back({node->children[i], world});
  }
  return out;
}

// Triangles a primitive of `n` vertices (or indices) makes, by its mode;
// 0 for points and lines.
uint64_t PrimitiveTriangles(cgltf_primitive_type type, uint64_t n) {
  switch (type) {
  case cgltf_primitive_type_triangles:
    return n / 3;
  case cgltf_primitive_type_triangle_strip:
  case cgltf_primitive_type_triangle_fan:
    return n >= 3 ? n - 2 : 0;
  default:
    return 0;
  }
}

} // namespace

bool ReadGltf(const std::string& path, TriMesh& out, std::string& error,
              const Cancelled& cancelled, ReadExtras* extras) {
  out = {};
  ReadExtras local;
  ReadExtras& ex = extras != nullptr ? *extras : local;
  ex = {};
  GltfData data;
  if (!OpenGltf(path, true, data, error))
    return false;
  if (data->asset.generator != nullptr && data->asset.generator[0] != '\0')
    ex.header.push_back({"asset generator", data->asset.generator});
  std::vector<glm::dvec3> soup; // metres, placed
  size_t skipped = 0, done = 0;
  std::vector<float> xyz;
  std::vector<uint32_t> index;
  for (const PlacedMesh& pm : PlacedMeshes(*data)) {
    const std::string which = "mesh " + std::to_string(pm.mesh - data->meshes);
    // What a viewer draws of these is not the positions as stored: a skin
    // poses them by its joints, and morph weights blend targets in. Neither
    // is read, so the file is refused rather than read in a shape nobody
    // drew.
    if (pm.node->skin != nullptr) {
      error = Quote(path) + ": " + which +
              " is skinned (posed by the joints of an animated "
              "character), which is not read: apply the pose and export "
              "it unskinned";
      return false;
    }
    // A node's weights stand in for its mesh's.
    const bool own = pm.node->weights_count > 0;
    const cgltf_float* weights = own ? pm.node->weights : pm.mesh->weights;
    const cgltf_size nWeights =
        own ? pm.node->weights_count : pm.mesh->weights_count;
    for (cgltf_size w = 0; w < nWeights; w++)
      if (weights[w] != 0.0f) {
        error = Quote(path) + ": " + which +
                " is shown blended with a morph target, which is not "
                "read: apply the shape key and export it";
        return false;
      }
    const bool mirrored = glm::determinant(glm::dmat3(pm.world)) < 0.0;
    for (cgltf_size p = 0; p < pm.mesh->primitives_count; p++) {
      if (cancelled && cancelled()) {
        error = "cancelled";
        return false;
      }
      const cgltf_primitive& prim = pm.mesh->primitives[p];
      if (prim.type != cgltf_primitive_type_triangles &&
          prim.type != cgltf_primitive_type_triangle_strip &&
          prim.type != cgltf_primitive_type_triangle_fan) {
        skipped++;
        continue;
      }
      const cgltf_accessor* pos =
          cgltf_find_accessor(&prim, cgltf_attribute_type_position, 0);
      if (pos == nullptr || pos->type != cgltf_type_vec3) {
        error = Quote(path) + ": a primitive of mesh " +
                std::to_string(pm.mesh - data->meshes) +
                " has no three-component POSITION";
        return false;
      }
      if (pos->buffer_view == nullptr) {
        error = Quote(path) + ": mesh " +
                std::to_string(pm.mesh - data->meshes) +
                "'s positions have no buffer view" +
                (prim.has_draco_mesh_compression
                     ? " (they are compressed with Draco, which is not read: "
                       "re-export it uncompressed)"
                     : std::string());
        return false;
      }
      xyz.assign(pos->count * 3, 0.0f);
      if (cgltf_accessor_unpack_floats(pos, xyz.data(), xyz.size()) !=
          xyz.size()) {
        error = Quote(path) + ": mesh " +
                std::to_string(pm.mesh - data->meshes) +
                "'s positions cannot be read";
        return false;
      }
      if (prim.indices != nullptr && prim.indices->buffer_view == nullptr) {
        error = Quote(path) + ": mesh " +
                std::to_string(pm.mesh - data->meshes) +
                "'s indices have no buffer view";
        return false;
      }
      const size_t n =
          prim.indices != nullptr ? prim.indices->count : size_t(pos->count);
      index.resize(n);
      if (prim.indices != nullptr) {
        if (cgltf_accessor_unpack_indices(prim.indices, index.data(),
                                          sizeof(uint32_t), n) != n) {
          error = Quote(path) + ": mesh " +
                  std::to_string(pm.mesh - data->meshes) +
                  "'s indices cannot be read";
          return false;
        }
        for (uint32_t i : index)
          if (i >= pos->count) {
            error = Quote(path) + ": mesh " +
                    std::to_string(pm.mesh - data->meshes) + " names vertex " +
                    std::to_string(i) + " of " + std::to_string(pos->count);
            return false;
          }
      } else {
        for (size_t i = 0; i < n; i++)
          index[i] = uint32_t(i);
      }
      if (prim.type == cgltf_primitive_type_triangles && n % 3 != 0) {
        error = Quote(path) + ": mesh " +
                std::to_string(pm.mesh - data->meshes) + " has " +
                std::to_string(n) +
                " triangle corners, which is not a multiple of three";
        return false;
      }
      const auto corner = [&](uint32_t i) {
        const glm::dvec4 v(xyz[3 * size_t(i)], xyz[3 * size_t(i) + 1],
                           xyz[3 * size_t(i) + 2], 1.0);
        return glm::dvec3(pm.world * v);
      };
      const auto tri = [&](uint32_t a, uint32_t b, uint32_t c) {
        if (mirrored)
          std::swap(b, c); // a mirror turns the winding inside out
        soup.push_back(corner(a));
        soup.push_back(corner(b));
        soup.push_back(corner(c));
        if (++done % kCancelEvery == 0 && cancelled && cancelled())
          return false;
        return true;
      };
      const uint64_t count = PrimitiveTriangles(prim.type, n);
      for (uint64_t t = 0; t < count; t++) {
        bool going = true;
        if (prim.type == cgltf_primitive_type_triangles)
          going = tri(index[3 * t], index[3 * t + 1], index[3 * t + 2]);
        else if (prim.type == cgltf_primitive_type_triangle_strip)
          going = t % 2 == 0 ? tri(index[t], index[t + 1], index[t + 2])
                             : tri(index[t + 1], index[t], index[t + 2]);
        else
          going = tri(index[0], index[t + 1], index[t + 2]);
        if (!going) {
          error = "cancelled";
          return false;
        }
      }
    }
  }
  if (skipped > 0)
    ex.notes.push_back(Plural(skipped, "primitive", "primitives") +
                       " of points or lines skipped");
  if (soup.empty()) {
    error = Quote(path) + " holds no triangles in its scene" +
            (skipped > 0 ? " (only points or lines)" : "");
    return false;
  }
  for (const glm::dvec3& p : soup)
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {
      error = Quote(path) + ": a vertex, placed, is not a number";
      return false;
    }
  // Metres, by the format; read as millimetres. The precision is float32's
  // in the file's metres.
  const double quantum = 1000.0 * Float32Quantum(soup);
  for (glm::dvec3& p : soup)
    p *= 1000.0;
  out = FromSoup(soup, 0.5 * quantum);
  out.quantum = quantum;
  if (out.triangles.empty()) {
    error = Quote(path) + " holds only degenerate triangles";
    return false;
  }
  return true;
}

std::optional<uint64_t> CountGltfTriangles(const std::string& path) {
  GltfData data;
  std::string error;
  if (!OpenGltf(path, false, data, error))
    return std::nullopt;
  uint64_t triangles = 0;
  for (const PlacedMesh& pm : PlacedMeshes(*data))
    for (cgltf_size p = 0; p < pm.mesh->primitives_count; p++) {
      const cgltf_primitive& prim = pm.mesh->primitives[p];
      const cgltf_accessor* pos =
          cgltf_find_accessor(&prim, cgltf_attribute_type_position, 0);
      const uint64_t n = prim.indices != nullptr ? prim.indices->count
                         : pos != nullptr        ? pos->count
                                                 : 0;
      triangles += PrimitiveTriangles(prim.type, n);
    }
  return triangles;
}

std::vector<std::string> GltfExternalFiles(const std::string& path) {
  cgltf_options options{};
  cgltf_data* raw = nullptr;
  if (cgltf_parse_file(&options, path.c_str(), &raw) != cgltf_result_success) {
    cgltf_free(raw);
    return {};
  }
  GltfData data(raw);
  std::vector<std::string> files;
  for (cgltf_size i = 0; i < data->buffers_count; i++) {
    const char* uri = data->buffers[i].uri;
    // A .glb's own chunk has no uri; an embedded one is data:, and anything
    // with a scheme cgltf_load_buffers refuses. The rest it reads from beside
    // the file, the uri percent-decoded, as resolved here.
    if (uri == nullptr || std::strncmp(uri, "data:", 5) == 0 ||
        std::strstr(uri, "://") != nullptr)
      continue;
    std::string decoded(uri);
    decoded.resize(cgltf_decode_uri(decoded.data()));
    files.push_back(
        (std::filesystem::path(path).parent_path() / decoded).string());
  }
  return files;
}

} // namespace mesh
