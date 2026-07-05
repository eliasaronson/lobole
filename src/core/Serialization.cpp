#include "core/Serialization.h"

#include "Logger.h"
#include "core/Feature.h"

#include <nlohmann/json.hpp>

#include <fstream>

using nlohmann::json;

namespace {

constexpr const char* kFormat = "designer-document";
constexpr int kVersion = 1;

json Vec3ToJson(const glm::vec3& v) { return json::array({v.x, v.y, v.z}); }

glm::vec3 Vec3FromJson(const json& j) {
  return glm::vec3(j.at(0).get<float>(), j.at(1).get<float>(),
                   j.at(2).get<float>());
}

json FeatureToJson(const Feature& feature) {
  json j;
  j["type"] = feature.TypeName();
  j["name"] = feature.GetName();

  if (auto* box = dynamic_cast<const BoxFeature*>(&feature)) {
    j["position"] = Vec3ToJson(box->position);
    j["width"] = box->width;
    j["height"] = box->height;
    j["depth"] = box->depth;
  } else if (auto* sphere = dynamic_cast<const SphereFeature*>(&feature)) {
    j["center"] = Vec3ToJson(sphere->center);
    j["radius"] = sphere->radius;
  } else if (auto* cyl = dynamic_cast<const CylinderFeature*>(&feature)) {
    j["center"] = Vec3ToJson(cyl->center);
    j["direction"] = Vec3ToJson(cyl->direction);
    j["radius"] = cyl->radius;
    j["height"] = cyl->height;
  } else if (auto* import = dynamic_cast<const ImportFeature*>(&feature)) {
    j["path"] = import->path;
  } else {
    throw std::runtime_error("Unserializable feature type: " +
                             feature.TypeName());
  }
  return j;
}

std::unique_ptr<Feature> FeatureFromJson(const json& j) {
  const std::string type = j.at("type").get<std::string>();
  const std::string name = j.at("name").get<std::string>();

  if (type == "Box") {
    return std::make_unique<BoxFeature>(
        name, Vec3FromJson(j.at("position")), j.at("width").get<double>(),
        j.at("height").get<double>(), j.at("depth").get<double>());
  }
  if (type == "Sphere") {
    return std::make_unique<SphereFeature>(name, Vec3FromJson(j.at("center")),
                                           j.at("radius").get<double>());
  }
  if (type == "Cylinder") {
    return std::make_unique<CylinderFeature>(
        name, Vec3FromJson(j.at("center")), Vec3FromJson(j.at("direction")),
        j.at("radius").get<double>(), j.at("height").get<double>());
  }
  if (type == "Import") {
    return std::make_unique<ImportFeature>(name,
                                           j.at("path").get<std::string>());
  }
  throw std::runtime_error("Unknown feature type: " + type);
}

}  // namespace

namespace DocumentIO {

bool Save(const Document& doc, const std::string& path, std::string* error) {
  try {
    json j;
    j["format"] = kFormat;
    j["version"] = kVersion;
    j["features"] = json::array();
    for (const auto& feature : doc.GetFeatures()) {
      j["features"].push_back(FeatureToJson(*feature));
    }

    std::ofstream out(path);
    if (!out) {
      if (error) *error = "Cannot open file for writing: " + path;
      return false;
    }
    out << j.dump(2) << "\n";
    LOG_INFO("Saved document ({} features) to {}", doc.FeatureCount(), path);
    return true;
  } catch (const std::exception& e) {
    if (error) *error = e.what();
    LOG_ERROR("Failed to save document to {}: {}", path, e.what());
    return false;
  }
}

std::unique_ptr<Document> Load(const std::string& path, std::string* error) {
  try {
    std::ifstream in(path);
    if (!in) {
      if (error) *error = "Cannot open file: " + path;
      return nullptr;
    }
    json j = json::parse(in);

    if (j.value("format", "") != kFormat) {
      if (error) *error = "Not a designer document: " + path;
      return nullptr;
    }
    if (j.value("version", 0) > kVersion) {
      if (error)
        *error = "Document version is newer than this build supports";
      return nullptr;
    }

    auto doc = std::make_unique<Document>();
    for (const auto& fj : j.at("features")) {
      doc->AddFeature(FeatureFromJson(fj));
    }
    LOG_INFO("Loaded document ({} features) from {}", doc->FeatureCount(),
             path);
    return doc;
  } catch (const std::exception& e) {
    if (error) *error = e.what();
    LOG_ERROR("Failed to load document from {}: {}", path, e.what());
    return nullptr;
  }
}

}  // namespace DocumentIO
