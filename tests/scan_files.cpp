// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "scan_files.h"

#include <nlohmann/json.hpp>

#include <bit>
#include <cstdio>
#include <cstring>

namespace scanfiles {

namespace {

template <typename T> void Put(std::string& out, T v, bool big) {
  char b[sizeof(T)];
  std::memcpy(b, &v, sizeof(T));
  if (big != (std::endian::native == std::endian::big))
    for (size_t i = 0; i < sizeof(T) / 2; i++)
      std::swap(b[i], b[sizeof(T) - 1 - i]);
  out.append(b, sizeof(T));
}

} // namespace

std::string Ply(const mesh::TriMesh& m, const PlyOptions& o) {
  std::vector<std::vector<uint32_t>> faces = o.faces;
  if (faces.empty() && o.withFaces)
    for (const auto& t : m.triangles)
      faces.push_back({t[0], t[1], t[2]});
  const bool ascii = o.encoding == PlyEncoding::Ascii;
  const bool big = o.encoding == PlyEncoding::BinaryBig;
  std::string out = "ply\nformat ";
  out += ascii ? "ascii" : big ? "binary_big_endian" : "binary_little_endian";
  out += " 1.0\ncomment written by tests/scan_files.cpp\n";
  out += "element vertex " + std::to_string(m.positions.size()) + "\n";
  out += "property float x\nproperty float y\nproperty float z\n";
  if (!o.colours.empty())
    out += "property uchar red\nproperty uchar green\nproperty uchar blue\n";
  if (o.withFaces) {
    out += "element face " + std::to_string(faces.size()) + "\n";
    out += "property list uchar int vertex_indices\n";
  }
  out += "end_header\n";
  char line[160];
  for (size_t i = 0; i < m.positions.size(); i++) {
    const glm::vec3 p(m.positions[i]);
    const std::array<uint8_t, 3> c =
        o.colours.empty() ? std::array<uint8_t, 3>{}
                          : o.colours[o.colours.size() == 1 ? 0 : i];
    if (ascii) {
      std::snprintf(line, sizeof line, "%.9g %.9g %.9g", p.x, p.y, p.z);
      out += line;
      if (!o.colours.empty()) {
        std::snprintf(line, sizeof line, " %d %d %d", c[0], c[1], c[2]);
        out += line;
      }
      out += "\n";
    } else {
      Put(out, p.x, big), Put(out, p.y, big), Put(out, p.z, big);
      if (!o.colours.empty())
        out.append(reinterpret_cast<const char*>(c.data()), 3);
    }
  }
  for (const auto& f : faces) {
    if (ascii) {
      out += std::to_string(f.size());
      for (uint32_t v : f)
        out += " " + std::to_string(v);
      out += "\n";
    } else {
      Put(out, uint8_t(f.size()), big);
      for (uint32_t v : f)
        Put(out, int32_t(v), big);
    }
  }
  return out;
}

namespace {

// The JSON and the buffer of `o`, the buffer named `uri` (or the GLB's own
// when empty).
std::pair<nlohmann::json, std::string> Build(const GltfOptions& o,
                                             const std::string& uri) {
  using nlohmann::json;
  std::string bin;
  json accessors = json::array(), views = json::array(), meshes = json::array();
  for (const mesh::TriMesh& m : o.meshes) {
    const size_t posAt = bin.size();
    glm::vec3 lo(1e30f), hi(-1e30f);
    for (const glm::dvec3& p : m.positions) {
      const glm::vec3 f(p);
      lo = glm::min(lo, f), hi = glm::max(hi, f);
      Put(bin, f.x, false), Put(bin, f.y, false), Put(bin, f.z, false);
    }
    const size_t idxAt = bin.size();
    for (const auto& t : m.triangles)
      for (uint32_t v : t)
        Put(bin, v, false);
    const int pv = int(views.size());
    views.push_back(
        {{"buffer", 0}, {"byteOffset", posAt}, {"byteLength", idxAt - posAt}});
    views.push_back({{"buffer", 0},
                     {"byteOffset", idxAt},
                     {"byteLength", bin.size() - idxAt}});
    const int pa = int(accessors.size());
    accessors.push_back({{"bufferView", pv},
                         {"componentType", 5126},
                         {"count", m.positions.size()},
                         {"type", "VEC3"},
                         {"min", {lo.x, lo.y, lo.z}},
                         {"max", {hi.x, hi.y, hi.z}}});
    accessors.push_back({{"bufferView", pv + 1},
                         {"componentType", 5125},
                         {"count", m.triangles.size() * 3},
                         {"type", "SCALAR"}});
    json prim = {{"attributes", {{"POSITION", pa}}}, {"mode", o.mode}};
    if (o.mode != 0)
      prim["indices"] = pa + 1;
    meshes.push_back({{"primitives", json::array({prim})}});
  }
  json nodes = json::array();
  std::vector<GltfNode> ns = o.nodes;
  if (ns.empty())
    ns.push_back({});
  for (const GltfNode& n : ns) {
    json node = {{"mesh", n.mesh}};
    if (n.matrix != glm::dmat4(1.0)) {
      json mat = json::array();
      for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
          mat.push_back(n.matrix[c][r]);
      node["matrix"] = mat;
    }
    if (!n.children.empty())
      node["children"] = n.children;
    nodes.push_back(node);
  }
  json buffer = {{"byteLength", bin.size()}};
  if (!uri.empty())
    buffer["uri"] = uri;
  json g = {{"asset", {{"version", "2.0"}, {"generator", o.generator}}},
            {"scene", 0},
            {"scenes",
             {{{"nodes",
                o.sceneRoots.empty() ? std::vector<int>{0} : o.sceneRoots}}}},
            {"nodes", nodes},
            {"meshes", meshes},
            {"accessors", accessors},
            {"bufferViews", views},
            {"buffers", {buffer}}};
  if (!o.extensionsRequired.empty()) {
    g["extensionsRequired"] = o.extensionsRequired;
    g["extensionsUsed"] = o.extensionsRequired;
  }
  return {g, bin};
}

} // namespace

std::string Glb(const GltfOptions& o) {
  auto [g, bin] = Build(o, "");
  std::string text = g.dump();
  while (text.size() % 4 != 0)
    text += ' ';
  while (bin.size() % 4 != 0)
    bin += '\0';
  std::string out;
  Put(out, uint32_t(0x46546C67), false); // "glTF"
  Put(out, uint32_t(2), false);
  Put(out, uint32_t(12 + 8 + text.size() + 8 + bin.size()), false);
  Put(out, uint32_t(text.size()), false);
  Put(out, uint32_t(0x4E4F534A), false); // JSON
  out += text;
  Put(out, uint32_t(bin.size()), false);
  Put(out, uint32_t(0x004E4942), false); // BIN
  out += bin;
  return out;
}

std::pair<std::string, std::string> GltfWithBin(const GltfOptions& o,
                                                const std::string& binName) {
  auto [g, bin] = Build(o, binName);
  return {g.dump(2), bin};
}

} // namespace scanfiles
