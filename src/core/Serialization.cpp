#include "core/Serialization.h"

#include "Logger.h"
#include "core/ExtrudeFeature.h"
#include "core/Feature.h"
#include "core/SketchFeature.h"

#include <nlohmann/json.hpp>

#include <fstream>

using nlohmann::json;

namespace {

constexpr const char* kFormat = "designer-document";
constexpr int kVersion = 3; // v3: sketch constraints

json Vec3ToJson(const glm::vec3& v) { return json::array({v.x, v.y, v.z}); }

glm::vec3 Vec3FromJson(const json& j) {
  return glm::vec3(j.at(0).get<float>(), j.at(1).get<float>(),
                   j.at(2).get<float>());
}

json Vec2ToJson(const glm::vec2& v) { return json::array({v.x, v.y}); }

glm::vec2 Vec2FromJson(const json& j) {
  return glm::vec2(j.at(0).get<float>(), j.at(1).get<float>());
}

json EntityToJson(const sketch::Entity& entity) {
  return std::visit(
      [](const auto& e) -> json {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, sketch::Point>) {
          return {{"type", "point"}, {"p", Vec2ToJson(e.p)}};
        } else if constexpr (std::is_same_v<T, sketch::Line>) {
          return {{"type", "line"}, {"a", Vec2ToJson(e.a)},
                  {"b", Vec2ToJson(e.b)}};
        } else if constexpr (std::is_same_v<T, sketch::Circle>) {
          return {{"type", "circle"}, {"center", Vec2ToJson(e.center)},
                  {"radius", e.radius}};
        } else {
          return {{"type", "rect"}, {"c1", Vec2ToJson(e.c1)},
                  {"c2", Vec2ToJson(e.c2)}};
        }
      },
      entity);
}

sketch::Entity EntityFromJson(const json& j) {
  const std::string type = j.at("type").get<std::string>();
  if (type == "point") return sketch::Point{Vec2FromJson(j.at("p"))};
  if (type == "line")
    return sketch::Line{Vec2FromJson(j.at("a")), Vec2FromJson(j.at("b"))};
  if (type == "circle")
    return sketch::Circle{Vec2FromJson(j.at("center")),
                          j.at("radius").get<double>()};
  if (type == "rect")
    return sketch::Rectangle{Vec2FromJson(j.at("c1")), Vec2FromJson(j.at("c2"))};
  throw std::runtime_error("Unknown sketch entity type: " + type);
}

const char* ConstraintKindToString(sketch::ConstraintKind k) {
  using K = sketch::ConstraintKind;
  switch (k) {
  case K::Coincident: return "coincident";
  case K::Horizontal: return "horizontal";
  case K::Vertical: return "vertical";
  case K::Parallel: return "parallel";
  case K::Perpendicular: return "perpendicular";
  case K::Equal: return "equal";
  case K::Distance: return "distance";
  case K::Radius: return "radius";
  case K::Angle: return "angle";
  case K::Tangent: return "tangent";
  case K::PointOnLine: return "point_on_line";
  }
  return "coincident";
}

sketch::ConstraintKind ConstraintKindFromString(const std::string& s) {
  using K = sketch::ConstraintKind;
  if (s == "coincident") return K::Coincident;
  if (s == "horizontal") return K::Horizontal;
  if (s == "vertical") return K::Vertical;
  if (s == "parallel") return K::Parallel;
  if (s == "perpendicular") return K::Perpendicular;
  if (s == "equal") return K::Equal;
  if (s == "distance") return K::Distance;
  if (s == "radius") return K::Radius;
  if (s == "angle") return K::Angle;
  if (s == "tangent") return K::Tangent;
  if (s == "point_on_line") return K::PointOnLine;
  throw std::runtime_error("Unknown constraint kind: " + s);
}

json ConstraintToJson(const sketch::Constraint& c) {
  return {{"kind", ConstraintKindToString(c.kind)},
          {"a", {c.a.entity, c.a.which}},
          {"b", {c.b.entity, c.b.which}},
          {"value", c.value}};
}

sketch::Constraint ConstraintFromJson(const json& j) {
  sketch::Constraint c;
  c.kind = ConstraintKindFromString(j.at("kind").get<std::string>());
  c.a = {j.at("a").at(0).get<uint32_t>(), j.at("a").at(1).get<uint8_t>()};
  c.b = {j.at("b").at(0).get<uint32_t>(), j.at("b").at(1).get<uint8_t>()};
  c.value = j.value("value", 0.0);
  return c;
}

const char* ExtrudeModeToString(ExtrudeMode mode) {
  switch (mode) {
  case ExtrudeMode::NewBody: return "new_body";
  case ExtrudeMode::Add: return "add";
  case ExtrudeMode::Cut: return "cut";
  }
  return "new_body";
}

ExtrudeMode ExtrudeModeFromString(const std::string& s) {
  if (s == "new_body") return ExtrudeMode::NewBody;
  if (s == "add") return ExtrudeMode::Add;
  if (s == "cut") return ExtrudeMode::Cut;
  throw std::runtime_error("Unknown extrude mode: " + s);
}

}  // namespace

namespace DocumentIO {

json FeatureToJson(const Feature& feature) {
  json j;
  j["type"] = feature.TypeName();
  j["name"] = feature.GetName();
  if (feature.GetId() != 0) {
    j["id"] = feature.GetId();
  }

  if (auto* sk = dynamic_cast<const SketchFeature*>(&feature)) {
    j["plane"] = {{"origin", Vec3ToJson(sk->plane.GetOrigin())},
                  {"normal", Vec3ToJson(sk->plane.GetNormal())},
                  {"uAxis", Vec3ToJson(sk->plane.GetUAxis())}};
    j["entities"] = json::array();
    for (const auto& entity : sk->entities) {
      j["entities"].push_back(EntityToJson(entity));
    }
    j["constraints"] = json::array();
    for (const auto& c : sk->constraints) {
      j["constraints"].push_back(ConstraintToJson(c));
    }
  } else if (auto* ext = dynamic_cast<const ExtrudeFeature*>(&feature)) {
    j["sketch_id"] = ext->sketchId;
    j["distance"] = ext->distance;
    j["reversed"] = ext->reversed;
    j["mode"] = ExtrudeModeToString(ext->mode);
  } else if (auto* box = dynamic_cast<const BoxFeature*>(&feature)) {
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

  std::unique_ptr<Feature> out;
  if (type == "Sketch") {
    const json& pl = j.at("plane");
    SketchPlane plane(Vec3FromJson(pl.at("origin")),
                      Vec3FromJson(pl.at("normal")),
                      Vec3FromJson(pl.at("uAxis")));
    auto sk = std::make_unique<SketchFeature>(name, plane);
    for (const auto& ej : j.value("entities", json::array())) {
      sk->entities.push_back(EntityFromJson(ej));
    }
    for (const auto& cj : j.value("constraints", json::array())) {
      sk->constraints.push_back(ConstraintFromJson(cj));
    }
    out = std::move(sk);
  } else if (type == "Extrude") {
    out = std::make_unique<ExtrudeFeature>(
        name, j.at("sketch_id").get<uint64_t>(), j.at("distance").get<double>(),
        ExtrudeModeFromString(j.value("mode", "new_body")),
        j.value("reversed", false));
  } else if (type == "Box") {
    out = std::make_unique<BoxFeature>(
        name, Vec3FromJson(j.at("position")), j.at("width").get<double>(),
        j.at("height").get<double>(), j.at("depth").get<double>());
  } else if (type == "Sphere") {
    out = std::make_unique<SphereFeature>(name, Vec3FromJson(j.at("center")),
                                          j.at("radius").get<double>());
  } else if (type == "Cylinder") {
    out = std::make_unique<CylinderFeature>(
        name, Vec3FromJson(j.at("center")), Vec3FromJson(j.at("direction")),
        j.at("radius").get<double>(), j.at("height").get<double>());
  } else if (type == "Import") {
    out = std::make_unique<ImportFeature>(name,
                                          j.at("path").get<std::string>());
  }

  if (!out) {
    throw std::runtime_error("Unknown feature type: " + type);
  }
  out->SetId(j.value("id", uint64_t{0})); // v1 files have no id; Document assigns
  return out;
}

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
