#pragma once

// Internal (non-public) shared helpers for the Serialize* domain translation
// units carved out of Serialization.cpp. Only the serialize TUs include this;
// consumers use core/Serialization.h. The tiny value/ref converters + the
// format constants live here (inline, global scope so both DocumentIO code and
// the file-local helpers resolve them unqualified); the heavier file/envelope
// helpers stay defined in Serialization.cpp.

#include "core/ElementMap.h" // ElementRef

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include <string>

inline constexpr const char* kFormat = "designer-document";
inline constexpr int kVersion =
    70; // v70: seven fastener standards instead of one (nuts, washers, hex
        //      and countersunk and button heads, a hexalobular drive), the
        //      drive DEFAULTED to the one its standard is made with rather
        //      than to "hex", and the `fastener_callout` drawing dimension.
        //      All three widen what a loader must ACCEPT — a v69 build reads
        //      "iso4017" as an unknown standard — which is what the bump is
        //      for (docs/parts/plan.md, phases 4-5).
        // (v69: the Fastener feature — a standard part built from a vendored
        //      dimension table (docs/parts/plan.md), carrying only what the
        //      table cannot give it: which standard, which size, how long
        // (v68: cam operation `use_outline` (clear the face's outline,
        //      passing over its openings rather than around them);
        //      the cam work origin is DERIVED from a named corner
        // (`origin_at`) and the post subtracts it; fixed-size stock
        // (`stock.mode`/`size`/`place`) — docs/cam/stock-and-origin.md
        // (v67: the cam setup's fixture frame + whether the machine has a
        // rotary axis (docs/cam/setup-frame.md))
        // (v66: drawing notes; cam tool type/tip angle + the edge-break op)
        // (v65: mate connector corner snap (anchor snap_edge2))
        // (v64: display units (length/mass/angle) travel with the document)
        // (v63: the cam setup remembers the face Z zero was picked from)
        // (v62: cam operation `side` written for clearing ops too)
        // (v61: expression-driven Box/Sphere/Cylinder dimensions)
        // (v57: mate connector inner_path)

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
                   j.at(1).get<float>(), j.at(2).get<float>()); // (w,x,y,z)
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
