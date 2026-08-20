#include "core/io/Serialization.h"
#include "core/io/SerializationInternal.h"

#include "core/Occurrence.h"

#include <nlohmann/json.hpp>

#include <string>

using nlohmann::json;

namespace DocumentIO {

json OccurrenceToJson(const Occurrence& o) {
  json j;
  j["type"] = "Occurrence";
  j["id"] = o.id;
  j["name"] = o.name;
  j["part_id"] = o.partId;
  // v55: what part_id points at. Absent in v54 and older ⇒ "part", which is
  // what every existing file means.
  j["ref_kind"] = o.refKind == OccurrenceRef::Assembly ? "assembly" : "part";
  j["part_version"] = o.partVersion ? json(*o.partVersion) : json(nullptr);
  j["transform"] = {{"position", Vec3ToJson(o.position)},
                    {"orientation", QuatToJson(o.orientation)}};
  j["visible"] = o.visible;
  j["fixed"] = o.fixed;
  // v56: which configuration of the reference this placement uses; 0 (and
  // absent, in every older file) = that document's active one.
  j["configuration_id"] = o.configurationId;
  return j;
}

Occurrence OccurrenceFromJson(const json& j) {
  Occurrence o;
  o.id = j.at("id").get<uint64_t>();
  o.name = j.value("name", std::string{});
  o.partId = j.at("part_id").get<uint64_t>();
  o.refKind = j.value("ref_kind", std::string("part")) == "assembly"
                  ? OccurrenceRef::Assembly
                  : OccurrenceRef::Part;
  o.configurationId = j.value("configuration_id", uint64_t{0});
  if (j.contains("part_version") && !j.at("part_version").is_null())
    o.partVersion = j.at("part_version").get<std::string>();
  // Both halves of the transform are optional: a caller placing a part by
  // position alone should not have to spell out an identity quaternion, and
  // omitting it used to surface as a raw "[json.exception.out_of_range.403]
  // key 'orientation' not found" (found by the scale round, writing exactly
  // that call).
  const json& t = j.at("transform");
  if (t.contains("position"))
    o.position = Vec3FromJson(t.at("position"));
  if (t.contains("orientation"))
    o.orientation = QuatFromJson(t.at("orientation"));
  o.visible = j.value("visible", true);
  o.fixed = j.value("fixed", false);
  return o;
}

// ---- Mates (A3) -----------------------------------------------------------

namespace {

const char* MateTypeToString(MateType t) {
  switch (t) {
  case MateType::Fastened:
    return "fastened";
  case MateType::Revolute:
    return "revolute";
  case MateType::Slider:
    return "slider";
  case MateType::Cylindrical:
    return "cylindrical";
  case MateType::Planar:
    return "planar";
  case MateType::Ball:
    return "ball";
  }
  return "fastened";
}

MateType MateTypeFromString(const std::string& s) {
  if (s == "revolute")
    return MateType::Revolute;
  if (s == "slider")
    return MateType::Slider;
  if (s == "cylindrical")
    return MateType::Cylindrical;
  if (s == "planar")
    return MateType::Planar;
  if (s == "ball")
    return MateType::Ball;
  return MateType::Fastened; // default / "fastened"
}

json ConnectorToJson(const Connector& c) {
  json j;
  j["occurrence"] = c.occurrenceId;
  // v57: the chain into a sub-assembly, omitted in the ordinary case so
  // every existing mate serializes byte-identically.
  if (!c.innerPath.empty())
    j["inner_path"] = c.innerPath;
  if (c.anchor.origin) {
    j["anchor"] = {{"origin", true}};
  } else {
    json anchor = {{"face",
                    {{"feature_id", c.anchor.face.featureId},
                     {"element", c.anchor.face.name}}}};
    if (c.anchor.snapEdge.IsSet()) // inference snap (v26); omit when unset
      anchor["snap_edge"] = {{"feature_id", c.anchor.snapEdge.featureId},
                             {"element", c.anchor.snapEdge.name}};
    j["anchor"] = std::move(anchor);
  }
  return j;
}

Connector ConnectorFromJson(const json& j) {
  Connector c;
  c.occurrenceId = j.at("occurrence").get<uint64_t>();
  if (j.contains("inner_path"))
    c.innerPath = j.at("inner_path").get<std::vector<uint64_t>>();
  const json& a = j.at("anchor");
  if (a.value("origin", false)) {
    c.anchor.origin = true;
  } else if (a.contains("face")) {
    const json& f = a.at("face");
    c.anchor.face = ElementRef{f.value("feature_id", uint64_t{0}),
                               f.value("element", std::string{})};
    if (a.contains("snap_edge")) {
      const json& s = a.at("snap_edge");
      c.anchor.snapEdge = ElementRef{s.value("feature_id", uint64_t{0}),
                                     s.value("element", std::string{})};
    }
  }
  return c;
}

} // namespace

json MateToJson(const Mate& m) {
  json j;
  j["type"] = "Mate";
  j["id"] = m.id;
  j["name"] = m.name;
  j["mate_type"] = MateTypeToString(m.type);
  j["a"] = ConnectorToJson(m.a);
  j["b"] = ConnectorToJson(m.b);
  j["flipped"] = m.flipped;
  j["rotation"] = m.rotation;
  j["offset"] = m.offset;
  // Additive DOF for the wider mate set (v25); omit when zero/identity so
  // fastened/revolute/slider files stay byte-identical to v24.
  if (m.dx != 0.0f)
    j["dx"] = m.dx;
  if (m.dy != 0.0f)
    j["dy"] = m.dy;
  if (m.ballRotation != glm::quat(1.0f, 0.0f, 0.0f, 0.0f))
    j["ball_rotation"] = QuatToJson(m.ballRotation);
  return j;
}

Mate MateFromJson(const json& j) {
  Mate m;
  m.id = j.at("id").get<uint64_t>();
  m.name = j.value("name", std::string{});
  m.type = MateTypeFromString(j.value("mate_type", std::string{"fastened"}));
  m.a = ConnectorFromJson(j.at("a"));
  m.b = ConnectorFromJson(j.at("b"));
  m.flipped = j.value("flipped", true);
  m.rotation = j.value("rotation", 0.0f);
  m.offset = j.value("offset", 0.0f);
  m.dx = j.value("dx", 0.0f);
  m.dy = j.value("dy", 0.0f);
  if (j.contains("ball_rotation"))
    m.ballRotation = QuatFromJson(j.at("ball_rotation"));
  return m;
}

// ---- Exploded views (v27, A4) ---------------------------------------------

json ExplodedViewToJson(const ExplodedView& v) {
  json j;
  j["type"] = "ExplodedView";
  j["id"] = v.id;
  j["name"] = v.name;
  j["steps"] = json::array();
  for (const ExplodeStep& s : v.steps) {
    j["steps"].push_back({{"occurrence", s.occurrenceId},
                          {"offset", {s.offset.x, s.offset.y, s.offset.z}}});
  }
  return j;
}

ExplodedView ExplodedViewFromJson(const json& j) {
  ExplodedView v;
  v.id = j.at("id").get<uint64_t>();
  v.name = j.value("name", std::string{});
  for (const json& sj : j.value("steps", json::array())) {
    ExplodeStep s;
    s.occurrenceId = sj.at("occurrence").get<uint64_t>();
    const auto& o = sj.at("offset");
    s.offset = glm::vec3(o.at(0).get<float>(), o.at(1).get<float>(),
                         o.at(2).get<float>());
    v.steps.push_back(std::move(s));
  }
  return v;
}

} // namespace DocumentIO
