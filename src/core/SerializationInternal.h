#pragma once

// Internal (non-public) shared helpers for the Serialize* domain translation
// units carved out of Serialization.cpp. Only the serialize TUs include this;
// consumers use core/Serialization.h. The tiny value/ref converters + the
// format constants live here (inline, global scope so both DocumentIO code and
// the file-local helpers resolve them unqualified); the heavier file/envelope
// helpers stay defined in Serialization.cpp.

#include "core/ElementMap.h"  // ElementRef

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include <string>

inline constexpr const char* kFormat = "designer-document";
inline constexpr int kVersion = 34;  // v34: extrude/revolve region selection (v33: topology studies)

inline nlohmann::json Vec3ToJson(const glm::vec3& v) {
  return nlohmann::json::array({v.x, v.y, v.z});
}
inline glm::vec3 Vec3FromJson(const nlohmann::json& j) {
  return glm::vec3(j.at(0).get<float>(), j.at(1).get<float>(),
                   j.at(2).get<float>());
}

// Quaternion as [x, y, z, w] (glm stores (w,x,y,z); JSON order matches the
// a2-design occurrence schema).
inline nlohmann::json QuatToJson(const glm::quat& q) {
  return nlohmann::json::array({q.x, q.y, q.z, q.w});
}
inline glm::quat QuatFromJson(const nlohmann::json& j) {
  return glm::quat(j.at(3).get<float>(), j.at(0).get<float>(),
                   j.at(1).get<float>(), j.at(2).get<float>());  // (w,x,y,z)
}

inline nlohmann::json Vec2ToJson(const glm::vec2& v) {
  return nlohmann::json::array({v.x, v.y});
}
inline glm::vec2 Vec2FromJson(const nlohmann::json& j) {
  return glm::vec2(j.at(0).get<float>(), j.at(1).get<float>());
}

inline nlohmann::json ElementRefJson(const ElementRef& ref) {
  return nlohmann::json{{"feature_id", ref.featureId}, {"element", ref.name}};
}
inline ElementRef ElementRefFromJson(const nlohmann::json& j) {
  return ElementRef{j.value("feature_id", uint64_t{0}),
                    j.value("element", std::string{})};
}
