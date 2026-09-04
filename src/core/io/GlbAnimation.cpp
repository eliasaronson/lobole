#include "core/io/GlbAnimation.h"

#include <glm/gtc/quaternion.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstring>
#include <fstream>

namespace glb {

using json = nlohmann::json;

void AppendShape(Mesh& mesh, const CADShape& shape, const glm::mat4& local) {
  const CADShape::Mesh tess = shape.GenerateMesh();
  const uint32_t base = static_cast<uint32_t>(mesh.positions.size());
  const glm::mat3 normalXf = glm::mat3(glm::transpose(glm::inverse(local)));
  for (const CADShape::Vertex& v : tess.vertices) {
    mesh.positions.push_back(glm::vec3(local * glm::vec4(v.position, 1.0f)));
    mesh.normals.push_back(glm::normalize(normalXf * v.normal));
  }
  for (uint32_t i : tess.indices)
    mesh.indices.push_back(base + i);
}

namespace {

struct Bin {
  std::vector<uint8_t> bytes;
  json views = json::array();
  json accessors = json::array();

  // Appends raw data as one bufferView (4-byte aligned) and returns its index.
  size_t View(const void* data, size_t size, int target = 0) {
    while (bytes.size() % 4 != 0)
      bytes.push_back(0);
    const size_t offset = bytes.size();
    const auto* p = static_cast<const uint8_t*>(data);
    bytes.insert(bytes.end(), p, p + size);
    json v{{"buffer", 0}, {"byteOffset", offset}, {"byteLength", size}};
    if (target)
      v["target"] = target;
    views.push_back(v);
    return views.size() - 1;
  }

  size_t Vec3(const std::vector<glm::vec3>& data, bool minmax, int target) {
    const size_t view =
        View(data.data(), data.size() * sizeof(glm::vec3), target);
    json a{{"bufferView", view},
           {"componentType", 5126},
           {"count", data.size()},
           {"type", "VEC3"}};
    if (minmax && !data.empty()) {
      glm::vec3 lo = data[0], hi = data[0];
      for (const glm::vec3& v : data) {
        lo = glm::min(lo, v);
        hi = glm::max(hi, v);
      }
      a["min"] = {lo.x, lo.y, lo.z};
      a["max"] = {hi.x, hi.y, hi.z};
    }
    accessors.push_back(a);
    return accessors.size() - 1;
  }

  size_t Vec4(const std::vector<glm::vec4>& data) {
    const size_t view = View(data.data(), data.size() * sizeof(glm::vec4));
    accessors.push_back({{"bufferView", view},
                         {"componentType", 5126},
                         {"count", data.size()},
                         {"type", "VEC4"}});
    return accessors.size() - 1;
  }

  size_t Scalars(const std::vector<float>& data, bool minmax) {
    const size_t view = View(data.data(), data.size() * sizeof(float));
    json a{{"bufferView", view},
           {"componentType", 5126},
           {"count", data.size()},
           {"type", "SCALAR"}};
    if (minmax && !data.empty()) {
      float lo = data[0], hi = data[0];
      for (float v : data) {
        lo = std::min(lo, v);
        hi = std::max(hi, v);
      }
      a["min"] = {lo};
      a["max"] = {hi};
    }
    accessors.push_back(a);
    return accessors.size() - 1;
  }

  size_t Indices(const std::vector<uint32_t>& data) {
    const size_t view =
        View(data.data(), data.size() * sizeof(uint32_t), 34963);
    accessors.push_back({{"bufferView", view},
                         {"componentType", 5125},
                         {"count", data.size()},
                         {"type", "SCALAR"}});
    return accessors.size() - 1;
  }
};

} // namespace

bool Write(const std::string& path, const std::vector<Node>& nodes,
           const std::vector<double>& times, std::string* error) {
  const auto fail = [&](const std::string& m) {
    if (error)
      *error = m;
    return false;
  };
  if (nodes.empty())
    return fail("nothing to export: no nodes");
  if (times.size() < 2)
    return fail("an animation needs at least two keyframes");
  for (const Node& n : nodes)
    if (n.keyframes.size() != times.size())
      return fail("node '" + n.name + "' has " +
                  std::to_string(n.keyframes.size()) + " keyframes for " +
                  std::to_string(times.size()) + " times");

  Bin bin;
  json meshes = json::array(), materials = json::array();
  json gltfNodes = json::array();
  json channels = json::array(), samplers = json::array();

  // The keyframe times, shared by every channel.
  std::vector<float> ftimes;
  for (double t : times)
    ftimes.push_back(static_cast<float>(t));
  const size_t timesAcc = bin.Scalars(ftimes, /*minmax=*/true);

  // Node 0 is the Z-up -> Y-up root; everything hangs under it.
  json children = json::array();
  for (size_t i = 0; i < nodes.size(); i++) {
    const Node& n = nodes[i];
    if (n.mesh.positions.empty() || n.mesh.indices.empty())
      continue;
    const size_t pos = bin.Vec3(n.mesh.positions, true, 34962);
    const size_t nrm = bin.Vec3(n.mesh.normals, false, 34962);
    const size_t idx = bin.Indices(n.mesh.indices);
    materials.push_back(
        {{"name", n.name},
         {"pbrMetallicRoughness",
          {{"baseColorFactor",
            {n.mesh.color.r, n.mesh.color.g, n.mesh.color.b, 1.0}},
           {"metallicFactor", 0.2},
           {"roughnessFactor", 0.6}}}});
    meshes.push_back({{"name", n.name},
                      {"primitives",
                       {{{"attributes", {{"POSITION", pos}, {"NORMAL", nrm}}},
                         {"indices", idx},
                         {"material", materials.size() - 1}}}}});

    // TRS per keyframe: rigid placements, so scale is 1 and a quaternion is
    // exact. Decomposed rather than written as matrices because a glTF
    // animation can only target translation, rotation, scale.
    std::vector<glm::vec3> tr;
    std::vector<glm::vec4> rot;
    for (const glm::mat4& m : n.keyframes) {
      tr.push_back(glm::vec3(m[3]));
      const glm::quat q = glm::normalize(glm::quat_cast(glm::mat3(m)));
      rot.push_back(glm::vec4(q.x, q.y, q.z, q.w));
    }
    const size_t nodeIndex = gltfNodes.size() + 1; // after the root
    gltfNodes.push_back(
        {{"name", n.name},
         {"mesh", meshes.size() - 1},
         {"translation", {tr[0].x, tr[0].y, tr[0].z}},
         {"rotation", {rot[0].x, rot[0].y, rot[0].z, rot[0].w}}});
    children.push_back(nodeIndex);

    const size_t trAcc = bin.Vec3(tr, false, 0);
    const size_t rotAcc = bin.Vec4(rot);
    samplers.push_back(
        {{"input", timesAcc}, {"output", trAcc}, {"interpolation", "LINEAR"}});
    channels.push_back(
        {{"sampler", samplers.size() - 1},
         {"target", {{"node", nodeIndex}, {"path", "translation"}}}});
    samplers.push_back(
        {{"input", timesAcc}, {"output", rotAcc}, {"interpolation", "LINEAR"}});
    channels.push_back(
        {{"sampler", samplers.size() - 1},
         {"target", {{"node", nodeIndex}, {"path", "rotation"}}}});
  }
  if (children.empty())
    return fail("nothing to export: every node is empty");

  // The root: -90 degrees about X takes Z-up to Y-up.
  const float s = 0.70710678f;
  json root{{"name", "Z-up scene"},
            {"rotation", {-s, 0.0, 0.0, s}},
            {"children", children}};
  gltfNodes.insert(gltfNodes.begin(), root);

  json doc{
      {"asset", {{"version", "2.0"}, {"generator", "Designer"}}},
      {"scene", 0},
      {"scenes", {{{"nodes", {0}}}}},
      {"nodes", gltfNodes},
      {"meshes", meshes},
      {"materials", materials},
      {"buffers", {{{"byteLength", bin.bytes.size()}}}},
      {"bufferViews", bin.views},
      {"accessors", bin.accessors},
      {"animations",
       {{{"name", "motion"}, {"channels", channels}, {"samplers", samplers}}}}};

  std::string jsonText = doc.dump();
  while (jsonText.size() % 4 != 0)
    jsonText += ' ';
  while (bin.bytes.size() % 4 != 0)
    bin.bytes.push_back(0);

  std::ofstream out(path, std::ios::binary);
  if (!out)
    return fail("cannot write " + path);
  const auto u32 = [&](uint32_t v) {
    out.write(reinterpret_cast<const char*>(&v), 4);
  };
  const uint32_t total = 12 + 8 + static_cast<uint32_t>(jsonText.size()) + 8 +
                         static_cast<uint32_t>(bin.bytes.size());
  u32(0x46546C67); // "glTF"
  u32(2);
  u32(total);
  u32(static_cast<uint32_t>(jsonText.size()));
  u32(0x4E4F534A); // "JSON"
  out.write(jsonText.data(), static_cast<std::streamsize>(jsonText.size()));
  u32(static_cast<uint32_t>(bin.bytes.size()));
  u32(0x004E4942); // "BIN\0"
  out.write(reinterpret_cast<const char*>(bin.bytes.data()),
            static_cast<std::streamsize>(bin.bytes.size()));
  return out.good();
}

} // namespace glb
