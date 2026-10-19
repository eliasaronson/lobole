// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/io/Serialization.h"
#include "core/io/SerializationInternal.h"

#include "cad/CADMaterials.h"
#include "core/InputHash.h"
#include "core/MotionStudy.h"
#include "core/Occurrence.h"

#include <nlohmann/json.hpp>

#include <array>
#include <bit>
#include <cstdint>
#include <map>
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
  // What it rides on (v72). Omitted when free, so every existing assembly
  // serializes byte-identically.
  if (o.attachedTo != 0) {
    j["attached_to"] = o.attachedTo;
    j["attach_offset"] = {o.attachOffset.x, o.attachOffset.y, o.attachOffset.z};
    j["attach_rotation"] = {o.attachRotation.x, o.attachRotation.y,
                            o.attachRotation.z, o.attachRotation.w};
    // v157: the faces it is seated on, by name; the offset is then in the
    // seat's frame (docs/parts/README.md#attached-and-mated). Omitted when
    // it rides the part's own frame, so a v72..v156 rider re-saves
    // unchanged.
    if (o.attachSeat.IsSet()) {
      json seat{{"face",
                 {{"feature_id", o.attachSeat.face.featureId},
                  {"element", o.attachSeat.face.name}}},
                {"axis",
                 {{"feature_id", o.attachSeat.axis.featureId},
                  {"element", o.attachSeat.axis.name}}}};
      if (!o.attachSeat.innerPath.empty())
        seat["inner_path"] = o.attachSeat.innerPath;
      j["attach_seat"] = std::move(seat);
    }
  }
  // v56: which configuration of the reference this placement uses; 0 (and
  // absent, in every older file) = that document's active one.
  j["configuration_id"] = o.configurationId;
  // v135: the material this placement is made of; absent = the part's own.
  if (!o.material.empty())
    j["material"] = o.material;
  // v129: a flexible placement and the poses it keeps for its sub-assembly.
  // Omitted when rigid, so every earlier assembly serializes unchanged.
  if (o.flexible) {
    j["flexible"] = true;
    json poses = json::array();
    for (const InnerPose& p : o.innerPoses)
      poses.push_back({{"occurrence", p.occurrence},
                       {"position", Vec3ToJson(p.position)},
                       {"orientation", QuatToJson(p.orientation)}});
    j["inner_poses"] = std::move(poses);
    json dofs = json::array();
    for (const InnerMateDof& d : o.innerMateDofs) {
      json dj{{"mate", d.mate}, {"rotation", d.rotation}, {"offset", d.offset}};
      if (d.dx != 0.0 || d.dy != 0.0) {
        dj["dx"] = d.dx;
        dj["dy"] = d.dy;
      }
      if (d.ballRotation != glm::dquat(1.0, 0.0, 0.0, 0.0))
        dj["ball_rotation"] = QuatToJson(d.ballRotation);
      dofs.push_back(std::move(dj));
    }
    j["inner_mate_dofs"] = std::move(dofs);
  }
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
  o.material = j.value("material", std::string{});
  CADMaterials::MaterialType unusedType;
  if (!CADMaterials::MaterialLibrary::TryMaterialTypeFromString(o.material,
                                                                unusedType))
    throw std::runtime_error(
        "occurrence '" + o.name + "': unknown material '" + o.material +
        "' (known: " + CADMaterials::MaterialLibrary::SlugList() + ")");
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
  o.attachedTo = j.value("attached_to", uint64_t{0}); // v72
  if (const auto it = j.find("attach_offset");
      it != j.end() && it->is_array() && it->size() == 3) {
    o.attachOffset = glm::dvec3((*it)[0].get<double>(), (*it)[1].get<double>(),
                                (*it)[2].get<double>());
  }
  if (const auto it = j.find("attach_rotation");
      it != j.end() && it->is_array() && it->size() == 4) {
    o.attachRotation =
        glm::dquat((*it)[3].get<double>(), (*it)[0].get<double>(),
                   (*it)[1].get<double>(), (*it)[2].get<double>());
  }
  if (const auto it = j.find("attach_seat"); it != j.end()) { // v157
    // Both faces or nothing: a seat named by its face alone has no axis, and
    // reading it as "no seat" would quietly put the offset in the PART's
    // frame — the rider would load somewhere else and look placed.
    const auto ref = [&](const char* key) {
      const auto f = it->find(key);
      if (f == it->end() || !f->is_object() ||
          f->value("feature_id", uint64_t{0}) == 0 ||
          f->value("element", std::string{}).empty())
        throw std::runtime_error(
            "occurrence '" + o.name + "': attach_seat needs `" + key +
            "` as {feature_id, element}; it has " + it->dump());
      return ElementRef{f->at("feature_id").get<uint64_t>(),
                        f->at("element").get<std::string>()};
    };
    if (o.attachedTo == 0)
      throw std::runtime_error("occurrence '" + o.name +
                               "': attach_seat without attached_to — a seat "
                               "is on the part it rides on");
    o.attachSeat.face = ref("face");
    o.attachSeat.axis = ref("axis");
    if (it->contains("inner_path"))
      o.attachSeat.innerPath =
          it->at("inner_path").get<std::vector<uint64_t>>();
  }
  o.flexible = j.value("flexible", false); // v129
  for (const json& pj : j.value("inner_poses", json::array())) {
    InnerPose p;
    p.occurrence = pj.at("occurrence").get<uint64_t>();
    p.position = Vec3FromJson(pj.at("position"));
    p.orientation = QuatFromJson(pj.at("orientation"));
    o.innerPoses.push_back(p);
  }
  for (const json& dj : j.value("inner_mate_dofs", json::array())) {
    InnerMateDof d;
    d.mate = dj.at("mate").get<uint64_t>();
    d.rotation = dj.value("rotation", 0.0);
    d.offset = dj.value("offset", 0.0);
    d.dx = dj.value("dx", 0.0);
    d.dy = dj.value("dy", 0.0);
    if (dj.contains("ball_rotation"))
      d.ballRotation = QuatFromJson(dj.at("ball_rotation"));
    o.innerMateDofs.push_back(d);
  }
  if (!o.flexible && (!o.innerPoses.empty() || !o.innerMateDofs.empty()))
    throw std::runtime_error("occurrence " + std::to_string(o.id) +
                             " carries inner_poses or inner_mate_dofs but is "
                             "not flexible — only a flexible placement moves "
                             "its sub-assembly");
  return o;
}

// ---- Mates (A3) -----------------------------------------------------------

namespace {

// The mate types, in one place: the string, and what the enum is. Both the
// parser's refusal and `describe` read this, so a seventh type cannot be
// added and left undocumented.
constexpr std::pair<const char*, MateType> kMateTypes[] = {
    {"fastened", MateType::Fastened}, {"revolute", MateType::Revolute},
    {"slider", MateType::Slider},     {"cylindrical", MateType::Cylindrical},
    {"planar", MateType::Planar},     {"ball", MateType::Ball}};

constexpr std::pair<const char*, RelationKind> kRelationKinds[] = {
    {"gear", RelationKind::Gear},
    {"rack", RelationKind::Rack},
    {"screw", RelationKind::Screw},
    {"linear", RelationKind::Linear},
    {"chain", RelationKind::Chain},
    {"cam", RelationKind::Cam},
    {"freewheel", RelationKind::Freewheel},
    {"click", RelationKind::Click},
    {"worm", RelationKind::Worm},
    {"linear_n", RelationKind::LinearN},
    {"epicyclic", RelationKind::Epicyclic},
    {"planet", RelationKind::Planet},
    {"intermittent", RelationKind::Intermittent}};

template <typename T, size_t N>
std::string NameList(const std::pair<const char*, T> (&table)[N]) {
  std::string out;
  for (size_t i = 0; i < N; ++i)
    out += (i ? ", " : "") + std::string(table[i].first);
  return out;
}

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
  if (s == "fastened")
    return MateType::Fastened;
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
  // Anything else used to fall through to Fastened, which made a typo the
  // most expensive kind of mistake: add_mate(type: "revolut") was ACCEPTED
  // and produced a RIGID joint. The assembly then solved perfectly well with
  // the wrong degrees of freedom, so nothing looked wrong until someone tried
  // to rotate it. Same on load, where a hand-edited or merged file silently
  // demoted every unrecognised mate to fastened.
  throw std::runtime_error("unknown mate type '" + s +
                           "' (expected one of: " + NameList(kMateTypes) +
                           "). To COUPLE two mates' degrees of freedom -- "
                           "gears, a rack and pinion, a screw -- the thing to "
                           "add is a RELATION, not a mate: see add_relation");
}

json ConnectorToJson(const Connector& c) {
  json j;
  j["occurrence"] = c.occurrenceId;
  // v57: the chain into a sub-assembly, omitted in the ordinary case so
  // every existing mate serializes byte-identically.
  if (!c.innerPath.empty())
    j["inner_path"] = c.innerPath;
  if (c.anchor.frameId != 0) { // v86: a named Frame of the part
    j["anchor"] = {{"frame", c.anchor.frameId}};
  } else if (c.anchor.origin) {
    j["anchor"] = {{"origin", true}};
  } else {
    json anchor = {{"face",
                    {{"feature_id", c.anchor.face.featureId},
                     {"element", c.anchor.face.name}}}};
    if (c.anchor.snapEdge.IsSet()) // inference snap (v26); omit when unset
      anchor["snap_edge"] = {{"feature_id", c.anchor.snapEdge.featureId},
                             {"element", c.anchor.snapEdge.name}};
    // The second edge of a CORNER snap (v65). Only ever set alongside the
    // first, and omitted otherwise so nothing else re-saves differently.
    if (c.anchor.snapEdge2.IsSet())
      anchor["snap_edge2"] = {{"feature_id", c.anchor.snapEdge2.featureId},
                              {"element", c.anchor.snapEdge2.name}};
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
  if (a.contains("frame")) { // v86
    c.anchor.frameId = a.at("frame").get<uint64_t>();
  } else if (a.value("origin", false)) {
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
    if (a.contains("snap_edge2")) {
      const json& s2 = a.at("snap_edge2");
      c.anchor.snapEdge2 = ElementRef{s2.value("feature_id", uint64_t{0}),
                                      s2.value("element", std::string{})};
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
  if (m.dx != 0.0)
    j["dx"] = m.dx;
  if (m.dy != 0.0)
    j["dy"] = m.dy;
  if (m.ballRotation != glm::dquat(1.0, 0.0, 0.0, 0.0))
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
  m.rotation = j.value("rotation", 0.0);
  m.offset = j.value("offset", 0.0);
  m.dx = j.value("dx", 0.0);
  m.dy = j.value("dy", 0.0);
  if (j.contains("ball_rotation"))
    m.ballRotation = QuatFromJson(j.at("ball_rotation"));
  return m;
}

// ---- The assembly sequence (v187; groups and poses vNEXT) ----------------

json AssemblySequenceToJson(const AssemblySequence& seq) {
  json j;
  if (seq.allowanceMm != 0.0)
    j["allowance_mm"] = seq.allowanceMm;
  j["steps"] = json::array();
  for (const SequenceStep& s : seq.steps) {
    json sj = json::object();
    if (s.IsPose()) {
      json pose = json::array();
      for (const SequencePose& p : s.pose)
        pose.push_back(
            {{"mate", p.mateId},
             {p.dof == MateDof::Rotation ? "rotation_deg" : "offset_mm",
              p.value}});
      sj["pose"] = std::move(pose);
    } else if (s.occurrences.size() == 1) {
      sj["occurrence"] = s.occurrences[0];
    } else {
      sj["occurrences"] = s.occurrences;
    }
    if (s.direction)
      sj["direction"] = {s.direction->x, s.direction->y, s.direction->z};
    if (s.origin)
      sj["origin_mm"] = {s.origin->x, s.origin->y, s.origin->z};
    if (s.mateId != 0)
      sj["mate"] = s.mateId;
    if (s.distanceMm >= 0.0)
      sj["distance_mm"] = s.distanceMm;
    if (s.turnDeg != 0.0)
      sj["turn_deg"] = s.turnDeg;
    if (s.thread)
      sj["thread"] = true;
    if (!s.note.empty())
      sj["note"] = s.note;
    j["steps"].push_back(std::move(sj));
  }
  return j;
}

AssemblySequence AssemblySequenceFromJson(const json& j) {
  AssemblySequence seq;
  if (!j.is_object())
    throw std::runtime_error("an assembly sequence is an object {steps, "
                             "allowance_mm?}");
  for (auto it = j.begin(); it != j.end(); ++it)
    if (it.key() != "steps" && it.key() != "allowance_mm")
      throw std::runtime_error("unknown assembly sequence key '" + it.key() +
                               "' (accepted: steps, allowance_mm)");
  seq.allowanceMm = j.value("allowance_mm", 0.0);
  const auto vec = [](const json& v, const char* key) {
    if (!v.is_array() || v.size() != 3 ||
        !std::all_of(v.begin(), v.end(),
                     [](const json& e) { return e.is_number(); }))
      throw std::runtime_error(std::string("a sequence step's ") + key +
                               " is three numbers");
    return glm::dvec3(v[0].get<double>(), v[1].get<double>(),
                      v[2].get<double>());
  };
  for (const json& sj : j.value("steps", json::array())) {
    for (auto it = sj.begin(); it != sj.end(); ++it) {
      static const char* kKeys[] = {
          "occurrence", "occurrences", "pose",     "direction", "origin_mm",
          "mate",       "distance_mm", "turn_deg", "thread",    "note"};
      if (std::none_of(std::begin(kKeys), std::end(kKeys),
                       [&](const char* k) { return it.key() == k; }))
        throw std::runtime_error(
            "unknown sequence step key '" + it.key() +
            "' (accepted: occurrence, occurrences, pose, direction, "
            "origin_mm, mate, distance_mm, turn_deg, thread, note)");
    }
    SequenceStep s;
    const int what = int(sj.contains("occurrence")) +
                     int(sj.contains("occurrences")) + int(sj.contains("pose"));
    if (what != 1)
      throw std::runtime_error("a sequence step states one of occurrence, "
                               "occurrences (a group) or pose");
    if (sj.contains("occurrence"))
      s.occurrences.push_back(sj.at("occurrence").get<uint64_t>());
    if (sj.contains("occurrences"))
      s.occurrences = sj.at("occurrences").get<std::vector<uint64_t>>();
    if (sj.contains("pose")) {
      for (const json& pj : sj.at("pose")) {
        SequencePose p;
        p.mateId = pj.at("mate").get<uint64_t>();
        const bool rot = pj.contains("rotation_deg"),
                   off = pj.contains("offset_mm");
        if (rot == off || pj.size() != 2)
          throw std::runtime_error("a pose value is {mate, rotation_deg} or "
                                   "{mate, offset_mm}");
        p.dof = rot ? MateDof::Rotation : MateDof::Offset;
        p.value = pj.at(rot ? "rotation_deg" : "offset_mm").get<double>();
        s.pose.push_back(p);
      }
      if (s.pose.empty())
        throw std::runtime_error("a pose step sets at least one mate value");
    }
    if (s.occurrences.empty() && !sj.contains("pose"))
      throw std::runtime_error("a group step names its occurrences");
    if (sj.contains("direction"))
      s.direction = vec(sj.at("direction"), "direction");
    if (sj.contains("origin_mm"))
      s.origin = vec(sj.at("origin_mm"), "origin_mm");
    s.mateId = sj.value("mate", uint64_t{0});
    s.distanceMm = sj.value("distance_mm", -1.0);
    s.turnDeg = sj.value("turn_deg", 0.0);
    s.thread = sj.value("thread", false);
    s.note = sj.value("note", std::string{});
    seq.steps.push_back(std::move(s));
  }
  return seq;
}

// ---- Exploded views (v27, A4) ---------------------------------------------

json ExplodedViewToJson(const ExplodedView& v) {
  json j;
  j["type"] = "ExplodedView";
  j["id"] = v.id;
  j["name"] = v.name;
  j["steps"] = json::array();
  for (const ExplodeStep& s : v.steps) {
    json step{{"occurrence", s.occurrenceId},
              {"offset", {s.offset.x, s.offset.y, s.offset.z}}};
    if (!s.path.empty()) // v130: a moving part inside a flexible placement
      step["path"] = s.path;
    j["steps"].push_back(std::move(step));
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
    s.offset = glm::dvec3(o.at(0).get<double>(), o.at(1).get<double>(),
                          o.at(2).get<double>());
    if (sj.contains("path")) {
      s.path = sj.at("path").get<std::vector<uint64_t>>();
      if (s.path.size() > 1) // only one level moves; never half-applied
        throw std::runtime_error(
            "an explode step's path is one flexible placement, [occurrence]; "
            "this one goes " +
            std::to_string(s.path.size()) + " levels down");
    }
    v.steps.push_back(std::move(s));
  }
  return v;
}

std::vector<std::string> MateTypeNames() {
  std::vector<std::string> out;
  for (const auto& [name, type] : kMateTypes)
    out.emplace_back(name);
  return out;
}

std::vector<std::string> RelationKindNames() {
  std::vector<std::string> out;
  for (const auto& [name, kind] : kRelationKinds)
    out.emplace_back(name);
  return out;
}

// ---- Relations (v83) ------------------------------------------------------

const char* RelationKindToString(RelationKind kind) {
  switch (kind) {
  case RelationKind::Gear:
    return "gear";
  case RelationKind::Rack:
    return "rack";
  case RelationKind::Screw:
    return "screw";
  case RelationKind::Linear:
    return "linear";
  case RelationKind::Chain:
    return "chain";
  case RelationKind::Cam:
    return "cam";
  case RelationKind::Freewheel:
    return "freewheel";
  case RelationKind::Click:
    return "click";
  case RelationKind::Worm:
    return "worm";
  case RelationKind::LinearN:
    return "linear_n";
  case RelationKind::Epicyclic:
    return "epicyclic";
  case RelationKind::Planet:
    return "planet";
  case RelationKind::Intermittent:
    return "intermittent";
  }
  return "gear";
}

RelationKind RelationKindFromString(const std::string& s) {
  if (s == "gear")
    return RelationKind::Gear;
  if (s == "rack")
    return RelationKind::Rack;
  if (s == "screw")
    return RelationKind::Screw;
  if (s == "linear")
    return RelationKind::Linear;
  if (s == "chain")
    return RelationKind::Chain;
  if (s == "cam")
    return RelationKind::Cam;
  if (s == "freewheel")
    return RelationKind::Freewheel;
  if (s == "click")
    return RelationKind::Click;
  if (s == "worm")
    return RelationKind::Worm;
  if (s == "linear_n")
    return RelationKind::LinearN;
  if (s == "epicyclic")
    return RelationKind::Epicyclic;
  if (s == "planet")
    return RelationKind::Planet;
  if (s == "intermittent")
    return RelationKind::Intermittent;
  throw std::runtime_error("unknown relation type '" + s +
                           "' (expected one of: " + NameList(kRelationKinds) +
                           ")");
}

const char* MateDofToString(MateDof dof) {
  return dof == MateDof::Rotation ? "rotation" : "offset";
}

const char* FreewheelDrivesToString(int drives) {
  return drives < 0 ? "negative" : "positive";
}

int FreewheelDrivesFromString(const std::string& s) {
  if (s == "positive")
    return 1;
  if (s == "negative")
    return -1;
  throw std::runtime_error("unknown freewheel direction '" + s +
                           "' (expected positive or negative — the "
                           "direction b is driven in)");
}

int IntermittentSlotsFor(const std::string& motion, int slots) {
  if (motion == "uniform" || motion == "table") {
    if (slots != 0)
      throw std::runtime_error("slots is a Geneva's; a " + motion +
                               " step has none");
    return 0;
  }
  if (motion == "geneva") {
    if (slots < 3)
      throw std::runtime_error("a Geneva has at least 3 slots");
    return slots;
  }
  throw std::runtime_error("unknown intermittent motion '" + motion +
                           "' (expected uniform, geneva or table)");
}

std::vector<std::pair<double, double>>
IntermittentTableFromJson(const json& j) {
  std::vector<std::pair<double, double>> out;
  if (!j.is_array())
    throw std::runtime_error("an intermittent table is an array of "
                             "[driver fraction, step fraction] points");
  for (const json& pt : j) {
    if (!pt.is_array() || pt.size() != 2 || !pt[0].is_number() ||
        !pt[1].is_number())
      throw std::runtime_error("an intermittent table's point is "
                               "[driver fraction, step fraction]: " +
                               pt.dump());
    out.emplace_back(pt[0].get<double>(), pt[1].get<double>());
  }
  if (const std::string why = IntermittentTableError(out); !why.empty())
    throw std::runtime_error(why);
  return out;
}

namespace {
int IntermittentMotionFromJson(const json& j) {
  return IntermittentSlotsFor(j.at("motion").get<std::string>(),
                              j.value("slots", 0));
}
// A table motion's points (v184); a table under any other motion is
// refused rather than dropped.
std::vector<std::pair<double, double>> IntermittentTableOf(const json& j) {
  const bool table = j.at("motion").get<std::string>() == "table";
  if (table != j.contains("table"))
    throw std::runtime_error(table ? "a table motion needs its `table`"
                                   : "`table` is a table motion's");
  return table ? IntermittentTableFromJson(j.at("table"))
               : std::vector<std::pair<double, double>>{};
}
} // namespace

MateDof MateDofFromString(const std::string& s) {
  if (s == "rotation")
    return MateDof::Rotation;
  if (s == "offset")
    return MateDof::Offset;
  throw std::runtime_error("unknown mate degree of freedom '" + s +
                           "' (expected rotation or offset)");
}

json RelationToJson(const Relation& r) {
  json j;
  j["type"] = "Relation";
  j["id"] = r.id;
  j["name"] = r.name;
  j["kind"] = RelationKindToString(r.kind);
  if (r.HasTerms()) {
    // v171: a relation over several mates is its terms and a constant,
    // sum(coefficient * dof) = constant; an epicyclic's and a planet's
    // members carry the tooth counts their coefficients came from.
    json terms = json::array();
    for (const RelationTerm& t : r.terms) {
      json tj{{"mate", t.mate},
              {"dof", MateDofToString(t.dof)},
              {"coefficient", t.coefficient}};
      if (!t.path.empty())
        tj["path"] = t.path;
      if (t.teeth > 0)
        tj["teeth"] = t.teeth;
      terms.push_back(std::move(tj));
    }
    j["terms"] = std::move(terms);
    j["constant"] = r.phase;
    return j;
  }
  j["mate_a"] = r.mateA;
  j["mate_b"] = r.mateB;
  // v129: a side inside a flexible placement names its path; absent = a
  // mate of this document.
  if (!r.mateAPath.empty())
    j["mate_a_path"] = r.mateAPath;
  if (!r.mateBPath.empty())
    j["mate_b_path"] = r.mateBPath;
  j["dof_a"] = MateDofToString(r.dofA);
  j["dof_b"] = MateDofToString(r.dofB);
  j["ratio"] = r.ratio;
  if (r.teethA > 0 && r.teethB > 0) { // v171: what the ratio came from
    j["teeth_a"] = r.teethA;
    j["teeth_b"] = r.teethB;
  }
  if (r.internal)
    j["internal"] = true; // v171
  if (r.threadPart || r.threadFeature) {
    j["thread_part"] = r.threadPart;
    j["thread_feature"] = r.threadFeature;
  }
  j["phase"] = r.phase;
  if (!r.lift.empty()) { // v88, cam only; absent for every other kind
    json lift = json::array();
    for (const auto& [angle, mm] : r.lift)
      lift.push_back({angle, mm});
    j["lift"] = std::move(lift);
  }
  if (r.kind == RelationKind::Intermittent) {
    // v174: the step in the follower's unit (degrees for a turn, as a cam's
    // lift), a's angles in degrees, and the law over the window.
    const Relation::Intermittent& im = r.intermittent;
    j["step"] = im.step;
    j["period_deg"] = im.periodDeg;
    j["start_deg"] = im.startDeg;
    j["duration_deg"] = im.durationDeg;
    j["motion"] = im.slots > 0        ? "geneva"
                  : !im.table.empty() ? "table"
                                      : "uniform";
    if (im.slots > 0)
      j["slots"] = im.slots;
    if (!im.table.empty()) { // v184
      json t = json::array();
      for (const auto& [f, st] : im.table)
        t.push_back({f, st});
      j["table"] = std::move(t);
    }
  }
  if (r.kind == RelationKind::Freewheel || r.kind == RelationKind::Click) {
    j["drives"] = FreewheelDrivesToString(r.drives); // v104, click v105
    j["lead"] = r.lead;
  }
  if (r.teeth > 0) { // v105, freewheel only
    j["teeth"] = r.teeth;
    if (r.rideMate != 0) {
      j["ride"] = {{"amount", r.rideAmount},
                   {"mate", r.rideMate},
                   {"dof", MateDofToString(r.rideDof)}};
    }
  }
  return j;
}

json ClearanceAssertionToJson(const ClearanceAssertion& a) {
  json j;
  j["type"] = "ClearanceAssertion";
  j["id"] = a.id;
  j["name"] = a.name;
  j["a"] = a.a;
  j["b"] = a.b;
  j["min_mm"] = a.minMm;
  j["mode"] = a.mode;
  if (!a.why.empty())
    j["why"] = a.why;
  // The last answer travels WITH the assertion: a document that has been
  // verified and a document that never has are different documents, and the
  // difference is exactly what a reader needs to know.
  if (!a.verdict.empty()) {
    j["verdict"] = a.verdict;
    j["measured_mm"] = a.measuredMm;
    j["checked_key"] = a.checkedKey;
    if (!a.note.empty())
      j["note"] = a.note;
  }
  return j;
}

ClearanceAssertion ClearanceAssertionFromJson(const json& j) {
  ClearanceAssertion a;
  a.id = j.at("id").get<uint64_t>();
  a.name = j.value("name", std::string{});
  a.a = j.at("a").get<uint64_t>();
  a.b = j.at("b").get<uint64_t>();
  a.minMm = j.at("min_mm").get<double>();
  a.mode = j.value("mode", std::string("mesh"));
  if (a.mode != "mesh" && a.mode != "exact")
    throw std::runtime_error("unknown clearance assertion mode '" + a.mode +
                             "' (expected mesh or exact)");
  a.why = j.value("why", std::string{});
  a.verdict = j.value("verdict", std::string{});
  if (!a.verdict.empty() && a.verdict != "holds" && a.verdict != "violated" &&
      a.verdict != "unverifiable")
    throw std::runtime_error("unknown clearance assertion verdict '" +
                             a.verdict +
                             "' (expected holds, violated or unverifiable)");
  a.measuredMm = j.value("measured_mm", 0.0);
  a.checkedKey = j.value("checked_key", uint64_t{0});
  a.note = j.value("note", std::string{});
  return a;
}

json MotionStudyToJson(const MotionStudy& s) {
  json j;
  j["type"] = "MotionStudy";
  j["id"] = s.id;
  j["name"] = s.name;
  j["driver_mate"] = s.driverMate;
  j["driver_dof"] = MateDofToString(s.driverDof);
  if (!s.driverPath.empty())
    j["driver_path"] = s.driverPath; // v129
  j["from"] = s.from;
  j["to"] = s.to;
  j["samples"] = s.samples;
  j["check_clash"] = s.checkClash;
  j["clash_mode"] = s.clashMode;
  if (s.clashDeflectionMm > 0.0)
    j["clash_deflection_mm"] = s.clashDeflectionMm; // v167; absent = derived
  if (s.configuration)
    j["configuration"] = *s.configuration; // v128; 0 = the base
  if (s.maxRefine != MotionStudy::kDefaultMaxRefine)
    j["max_refine"] = s.maxRefine; // v179; absent = the default
  return j;
}

MotionStudy MotionStudyFromJson(const json& j) {
  MotionStudy s;
  s.id = j.at("id").get<uint64_t>();
  s.name = j.value("name", std::string{});
  s.driverMate = j.at("driver_mate").get<uint64_t>();
  s.driverDof = MateDofFromString(j.at("driver_dof").get<std::string>());
  if (j.contains("driver_path")) // v129
    s.driverPath = j.at("driver_path").get<std::vector<uint64_t>>();
  s.from = j.at("from").get<double>();
  s.to = j.at("to").get<double>();
  s.samples = j.value("samples", 48);
  if (s.samples < 2)
    throw std::runtime_error("a motion study needs at least 2 samples");
  s.checkClash = j.value("check_clash", true);
  s.clashMode = j.value("clash_mode", std::string("mesh"));
  if (s.clashMode != "mesh" && s.clashMode != "exact")
    throw std::runtime_error("unknown motion study clash_mode '" + s.clashMode +
                             "' (expected mesh or exact)");
  if (j.contains("configuration") && !j.at("configuration").is_null())
    s.configuration = j.at("configuration").get<uint64_t>();
  if (j.contains("max_refine")) { // v179
    const json& v = j.at("max_refine");
    if (!v.is_number_integer() || v.get<int>() < 0 ||
        v.get<int>() > MotionStudy::kMaxMaxRefine)
      throw std::runtime_error("motion study '" + s.name + "': max_refine " +
                               v.dump() +
                               " is not a number of halvings from 0 to " +
                               std::to_string(MotionStudy::kMaxMaxRefine));
    s.maxRefine = v.get<int>();
  }
  if (j.contains("clash_deflection_mm")) { // v167
    s.clashDeflectionMm = j.at("clash_deflection_mm").get<double>();
    if (const std::string why = MotionStudyDeflectionProblem(s); !why.empty())
      throw std::runtime_error("motion study '" + s.name + "': " + why);
  }
  return s;
}

Relation RelationFromJson(const json& j) {
  Relation r;
  r.id = j.at("id").get<uint64_t>();
  r.name = j.value("name", std::string{});
  r.kind = RelationKindFromString(j.at("kind").get<std::string>());
  if (r.HasTerms()) { // v171
    if (!j.contains("terms") || !j.at("terms").is_array())
      throw std::runtime_error(std::string("a ") +
                               RelationKindToString(r.kind) +
                               " relation needs its `terms`");
    for (const json& tj : j.at("terms")) {
      RelationTerm t;
      t.mate = tj.at("mate").get<uint64_t>();
      if (tj.contains("path"))
        t.path = tj.at("path").get<std::vector<uint64_t>>();
      t.dof = MateDofFromString(tj.at("dof").get<std::string>());
      t.coefficient = tj.at("coefficient").get<double>();
      t.teeth = tj.value("teeth", 0);
      r.terms.push_back(std::move(t));
    }
    r.phase = j.value("constant", 0.0);
    r.ratio = 1.0;
    return r;
  }
  r.mateA = j.at("mate_a").get<uint64_t>();
  r.mateB = j.at("mate_b").get<uint64_t>();
  if (j.contains("mate_a_path")) // v129
    r.mateAPath = j.at("mate_a_path").get<std::vector<uint64_t>>();
  if (j.contains("mate_b_path"))
    r.mateBPath = j.at("mate_b_path").get<std::vector<uint64_t>>();
  r.dofA = MateDofFromString(j.at("dof_a").get<std::string>());
  r.dofB = MateDofFromString(j.at("dof_b").get<std::string>());
  r.ratio = j.at("ratio").get<double>();
  r.teethA = j.value("teeth_a", 0); // v171
  r.teethB = j.value("teeth_b", 0);
  r.internal = j.value("internal", false);
  if (r.internal && r.kind != RelationKind::Gear)
    throw std::runtime_error("`internal` is a gear relation's; this one is a " +
                             std::string(RelationKindToString(r.kind)));
  r.threadPart = j.value("thread_part", uint64_t{0});
  r.threadFeature = j.value("thread_feature", uint64_t{0});
  r.phase = j.value("phase", 0.0);
  for (const json& pt : j.value("lift", json::array())) { // v88
    if (!pt.is_array() || pt.size() != 2)
      throw std::runtime_error(
          "a cam's lift table is [[angle_deg, lift_mm], ...]");
    r.lift.emplace_back(pt.at(0).get<double>(), pt.at(1).get<double>());
  }
  if (r.teeth > 0 || j.contains("teeth")) { // v105
    r.teeth = j.value("teeth", 0);
    if (j.contains("ride")) {
      const json& ride = j.at("ride");
      r.rideAmount = ride.at("amount").get<double>();
      r.rideMate = ride.at("mate").get<uint64_t>();
      r.rideDof = MateDofFromString(ride.at("dof").get<std::string>());
    }
  }
  if (r.kind == RelationKind::Intermittent) { // v174
    for (const char* k : {"step", "start_deg", "duration_deg", "motion"})
      if (!j.contains(k))
        throw std::runtime_error(
            std::string("an intermittent relation needs `") + k + "`");
    Relation::Intermittent& im = r.intermittent;
    im.step = j.at("step").get<double>();
    im.periodDeg = j.value("period_deg", 360.0);
    im.startDeg = j.at("start_deg").get<double>();
    im.durationDeg = j.at("duration_deg").get<double>();
    im.slots = IntermittentMotionFromJson(j);
    im.table = IntermittentTableOf(j);
  }
  if (r.kind == RelationKind::Freewheel ||
      r.kind == RelationKind::Click) { // v104: the direction is required
    if (!j.contains("drives"))
      throw std::runtime_error(
          "a freewheel relation needs `drives` (positive or negative)");
    r.drives = FreewheelDrivesFromString(j.at("drives").get<std::string>());
    r.lead = j.value("lead", 0.0);
  }
  return r;
}

// ---- Motion result (v153, binary v154) -------------------------------------
// Saved beside its study (motion/<id>-<slug>.result.json and .result.bin)
// with the input hash that produced it: a reopened project's run of the same
// inputs is answered from it (docs/simulation/README.md#studies-as-jobs).
// Everything the table, the scrub, the export and the RPCs read; each
// placement a matrix of 16 f64 in the .bin, column by column (a v153 file
// held them inline).

namespace {

glm::dmat4 Mat4FromJson(const json& j) {
  if (!j.is_array() || j.size() != 16)
    throw std::runtime_error("motion result: a placement is not 16 numbers");
  glm::dmat4 m(1.0);
  for (int c = 0; c < 4; ++c)
    for (int r = 0; r < 4; ++r)
      m[c][r] = j.at(c * 4 + r).get<double>();
  return m;
}

json ClashToJson(const motion::Clash& c) {
  json j = {{"a", c.a},
            {"b", c.b},
            {"a_name", c.aName},
            {"b_name", c.bName},
            {"within_min", Vec3ToJson(c.withinMin)},
            {"within_max", Vec3ToJson(c.withinMax)}};
  if (c.aInner != 0)
    j["a_inner"] = c.aInner;
  if (c.bInner != 0)
    j["b_inner"] = c.bInner;
  if (c.rigid)
    j["rigid"] = true;
  if (c.known)
    j["overlap"] = {{"volume", c.volume},
                    {"centroid", Vec3ToJson(c.centroid)},
                    {"bbox_min", Vec3ToJson(c.bboxMin)},
                    {"bbox_max", Vec3ToJson(c.bboxMax)}};
  return j;
}

// v159: a press fit the run relieved (docs/tolerances/README.md#press-fits).
json PressFitToJson(const motion::PressFit& f) {
  json j = {{"a", f.a},
            {"b", f.b},
            {"a_name", f.aName},
            {"b_name", f.bName},
            {"fit", f.fit},
            {"interference_mm", f.interference},
            {"max_interference_mm", f.maxInterference},
            {"volume_mm3", f.volume}};
  if (f.aInner != 0)
    j["a_inner"] = f.aInner;
  if (f.bInner != 0)
    j["b_inner"] = f.bInner;
  return j;
}

motion::PressFit PressFitFromJson(const json& j) {
  motion::PressFit f;
  f.a = j.at("a").get<uint64_t>();
  f.b = j.at("b").get<uint64_t>();
  f.aInner = j.value("a_inner", uint64_t{0});
  f.bInner = j.value("b_inner", uint64_t{0});
  f.aName = j.value("a_name", std::string{});
  f.bName = j.value("b_name", std::string{});
  f.fit = j.at("fit").get<std::string>();
  f.interference = j.at("interference_mm").get<double>();
  f.maxInterference = j.at("max_interference_mm").get<double>();
  f.volume = j.at("volume_mm3").get<double>();
  return f;
}

motion::Clash ClashFromJson(const json& j) {
  motion::Clash c;
  c.a = j.at("a").get<uint64_t>();
  c.b = j.at("b").get<uint64_t>();
  c.aInner = j.value("a_inner", uint64_t{0});
  c.bInner = j.value("b_inner", uint64_t{0});
  c.aName = j.value("a_name", std::string{});
  c.bName = j.value("b_name", std::string{});
  c.rigid = j.value("rigid", false);
  if (j.contains("within_min"))
    c.withinMin = Vec3FromJson(j.at("within_min"));
  if (j.contains("within_max"))
    c.withinMax = Vec3FromJson(j.at("within_max"));
  if (j.contains("overlap")) {
    const json& o = j.at("overlap");
    c.known = true;
    c.volume = o.value("volume", 0.0);
    c.centroid = Vec3FromJson(o.at("centroid"));
    c.bboxMin = Vec3FromJson(o.at("bbox_min"));
    c.bboxMax = Vec3FromJson(o.at("bbox_max"));
  }
  return c;
}

} // namespace

json MotionResultToJson(const motion::Result& r) {
  // The placements go to the .result.bin (v154): each distinct matrix once
  // in `placements`, 16 f64 column by column -- a pose is the model's, which
  // is double (docs/conventions.md#the-model-is-double) -- and each sample's
  // poses as indices into it. Most occurrences hold still through a run,
  // and one that does is one matrix, not one a sample. Distinct by bits, so
  // the table gives back exactly what was written.
  ResultArrayWriter w;
  std::map<std::array<std::uint64_t, 16>, int> seen;
  std::vector<double> table;
  const auto placements = [&](const std::vector<const glm::dmat4*>& ms) {
    std::vector<int> indices;
    indices.reserve(ms.size());
    for (const glm::dmat4* m : ms) {
      std::array<std::uint64_t, 16> bits;
      for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row)
          bits[c * 4 + row] = std::bit_cast<std::uint64_t>((*m)[c][row]);
      const auto [it, added] =
          seen.emplace(bits, static_cast<int>(table.size() / 16));
      if (added)
        for (int c = 0; c < 4; ++c)
          for (int row = 0; row < 4; ++row)
            table.push_back((*m)[c][row]);
      indices.push_back(it->second);
    }
    return w.Index(indices);
  };
  json samples = json::array();
  for (const motion::Sample& s : r.samples) {
    json occurrences = json::array();
    std::vector<const glm::dmat4*> worlds;
    for (const auto& [id, m] : s.worlds) {
      occurrences.push_back(id);
      worlds.push_back(&m);
    }
    json sj = {{"t", s.t},
               {"value", s.value},
               {"solved", s.solved},
               {"worlds",
                {{"occurrences", std::move(occurrences)},
                 {"world", placements(worlds)}}}};
    if (s.erroredMates != 0)
      sj["errored_mates"] = s.erroredMates;
    if (s.residualMm != 0.0)
      sj["residual_mm"] = s.residualMm;
    if (!s.leaves.empty()) {
      json leaves = json::array();
      std::vector<const glm::dmat4*> leafWorlds;
      for (const motion::Sample::LeafWorld& l : s.leaves) {
        leaves.push_back({{"root", l.root}, {"occurrence", l.occurrence}});
        leafWorlds.push_back(&l.world);
      }
      sj["leaves"] = std::move(leaves);
      sj["leaf_world"] = placements(leafWorlds);
    }
    if (!s.clashes.empty()) {
      json clashes = json::array();
      for (const motion::Clash& c : s.clashes)
        clashes.push_back(ClashToJson(c));
      sj["clashes"] = std::move(clashes);
    }
    if (!s.touching.empty()) { // v180: a mesh check's seated pairs
      json touching = json::array();
      for (const motion::Touch& t : s.touching) {
        json tj = {
            {"a", t.a}, {"b", t.b}, {"a_name", t.aName}, {"b_name", t.bName}};
        if (t.aInner != 0)
          tj["a_inner"] = t.aInner;
        if (t.bInner != 0)
          tj["b_inner"] = t.bInner;
        touching.push_back(std::move(tj));
      }
      sj["touching"] = std::move(touching);
    }
    if (!s.unknownContacts.empty()) {
      json unknown = json::array();
      for (const motion::UnknownContact& u : s.unknownContacts) {
        json uj = {{"a", u.a},
                   {"b", u.b},
                   {"a_name", u.aName},
                   {"b_name", u.bName},
                   {"reason", u.reason}};
        if (u.aInner != 0)
          uj["a_inner"] = u.aInner;
        if (u.bInner != 0)
          uj["b_inner"] = u.bInner;
        unknown.push_back(std::move(uj));
      }
      sj["unknown_contacts"] = std::move(unknown);
    }
    if (s.refined)
      sj["refined"] = true; // v179
    if (const auto& b = s.branchJump) {
      json bj = {{"mate", b->mate},         {"name", b->name},
                 {"rotation", b->rotation}, {"jump", b->jump},
                 {"expected", b->expected}, {"at", b->atValue},
                 {"step", b->stepValue}};
      if (!b->path.empty())
        bj["path"] = b->path;
      sj["branch_jump"] = std::move(bj); // v179
    }
    if (!s.mates.empty()) {
      json mates = json::array();
      for (const motion::Sample::MateValue& m : s.mates) {
        json mj = {{"mate", m.mate},
                   {"rotation", m.rotation},
                   {"offset", m.offset},
                   {"turns", m.turns},
                   {"slides", m.slides}};
        if (!m.path.empty())
          mj["path"] = m.path;
        if (!m.name.empty())
          mj["name"] = m.name;
        mates.push_back(std::move(mj));
      }
      sj["mates"] = std::move(mates);
    }
    samples.push_back(std::move(sj));
  }
  json out = {{"samples", std::move(samples)},
              {"pairs_rigid", r.pairsRigid},
              {"pairs_rigid_checked", r.pairsRigidChecked},
              {"rigid_checked_at", r.rigidCheckedAt},
              {"clean", r.clean},
              {"worst_sample", r.worstSample},
              {"deflection_mm", r.deflectionMm},
              {"pairs_cached", r.pairsCached},
              {"pairs_repeated", r.pairsRepeated},
              {"pairs_unknown", r.pairsUnknown},
              {"checked_clashes", r.checkedClashes},
              {"pairs_checked", r.pairsChecked},
              {"seconds", r.seconds},
              {"cancelled", r.cancelled}};
  if (r.refinedSamples != 0)
    out["refined_samples"] = r.refinedSamples; // v179
  if (r.branchJumps != 0)
    out["branch_jumps"] = r.branchJumps; // v179
  if (!r.error.empty())
    out["error"] = r.error;
  if (!r.pressFits.empty()) {
    json fits = json::array();
    for (const motion::PressFit& f : r.pressFits)
      fits.push_back(PressFitToJson(f));
    out["press_fits"] = std::move(fits);
  }
  if (r.inputHash != 0)
    out["input_hash"] = inputhash::Hex(r.inputHash);
  out["placements"] = w.F64(table);
  out[kResultData] = w.Take();
  out["bin_bytes"] = out[kResultData].get_binary().size();
  return out;
}

motion::Result MotionResultFromJson(const json& j) {
  motion::Result r;
  const ResultArrayReader bin(j, "motion result");
  // The distinct placements, and `count` of them named by indices (v154).
  std::vector<double> table;
  if (j.contains("placements")) {
    table = bin.Reals(j.at("placements"), "placements");
    if (table.size() % 16 != 0)
      throw std::runtime_error(
          "motion result: placements is not a whole number of matrices");
  }
  const auto placements = [&](const json& ref, size_t count, const char* name) {
    const std::vector<int> indices = bin.Indices(ref, name);
    if (indices.size() != count)
      throw std::runtime_error("motion result: " + std::string(name) +
                               " names " + std::to_string(indices.size()) +
                               " placements for " + std::to_string(count));
    std::vector<glm::dmat4> out(count, glm::dmat4(1.0));
    for (size_t i = 0; i < count; ++i) {
      const int k = indices[i];
      if (k < 0 || static_cast<size_t>(k) >= table.size() / 16)
        throw std::runtime_error("motion result: " + std::string(name) +
                                 " names placement " + std::to_string(k) +
                                 " of " + std::to_string(table.size() / 16));
      for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row)
          out[i][c][row] = table[k * 16 + c * 4 + row];
    }
    return out;
  };
  for (const json& sj : j.at("samples")) {
    motion::Sample s;
    s.t = sj.at("t").get<double>();
    s.value = sj.at("value").get<double>();
    s.solved = sj.value("solved", true);
    s.erroredMates = sj.value("errored_mates", 0);
    s.residualMm = sj.value("residual_mm", 0.0);
    const json worlds = sj.value("worlds", json::array());
    if (worlds.is_object()) {
      const auto ids = worlds.at("occurrences").get<std::vector<uint64_t>>();
      const auto ms = placements(worlds.at("world"), ids.size(), "worlds");
      for (size_t i = 0; i < ids.size(); ++i)
        s.worlds.emplace_back(ids[i], ms[i]);
    } else {
      for (const json& wj : worlds) // v153: inline
        s.worlds.emplace_back(wj.at("occurrence").get<uint64_t>(),
                              Mat4FromJson(wj.at("world")));
    }
    const json leaves = sj.value("leaves", json::array());
    std::vector<glm::dmat4> leafWorlds;
    if (sj.contains("leaf_world"))
      leafWorlds = placements(sj.at("leaf_world"), leaves.size(), "leaves");
    for (size_t i = 0; i < leaves.size(); ++i) {
      const json& lj = leaves[i];
      s.leaves.push_back(
          {lj.at("root").get<uint64_t>(), lj.at("occurrence").get<uint64_t>(),
           leafWorlds.empty() ? Mat4FromJson(lj.at("world")) : leafWorlds[i]});
    }
    for (const json& cj : sj.value("clashes", json::array()))
      s.clashes.push_back(ClashFromJson(cj));
    for (const json& tj : sj.value("touching", json::array())) {
      motion::Touch t;
      t.a = tj.at("a").get<uint64_t>();
      t.b = tj.at("b").get<uint64_t>();
      t.aName = tj.value("a_name", std::string{});
      t.bName = tj.value("b_name", std::string{});
      t.aInner = tj.value("a_inner", uint64_t{0});
      t.bInner = tj.value("b_inner", uint64_t{0});
      s.touching.push_back(std::move(t));
    }
    for (const json& uj : sj.value("unknown_contacts", json::array())) {
      motion::UnknownContact u;
      u.a = uj.at("a").get<uint64_t>();
      u.b = uj.at("b").get<uint64_t>();
      u.aName = uj.value("a_name", std::string{});
      u.bName = uj.value("b_name", std::string{});
      u.reason = uj.value("reason", u.reason);
      u.aInner = uj.value("a_inner", uint64_t{0});
      u.bInner = uj.value("b_inner", uint64_t{0});
      s.unknownContacts.push_back(std::move(u));
    }
    s.refined = sj.value("refined", false);
    if (sj.contains("branch_jump")) {
      const json& bj = sj.at("branch_jump");
      motion::Sample::BranchJump b;
      b.mate = bj.at("mate").get<uint64_t>();
      b.name = bj.value("name", std::string{});
      b.rotation = bj.value("rotation", true);
      b.jump = bj.value("jump", 0.0);
      b.expected = bj.value("expected", 0.0);
      b.atValue = bj.value("at", 0.0);
      b.stepValue = bj.value("step", 0.0);
      b.path = bj.value("path", std::vector<uint64_t>{});
      s.branchJump = std::move(b);
    }
    for (const json& mj : sj.value("mates", json::array())) {
      motion::Sample::MateValue m;
      m.mate = mj.at("mate").get<uint64_t>();
      m.rotation = mj.value("rotation", 0.0);
      m.offset = mj.value("offset", 0.0);
      m.turns = mj.value("turns", false);
      m.slides = mj.value("slides", false);
      m.path = mj.value("path", std::vector<uint64_t>{});
      m.name = mj.value("name", std::string{});
      s.mates.push_back(std::move(m));
    }
    r.samples.push_back(std::move(s));
  }
  r.pairsRigid = j.value("pairs_rigid", size_t{0});
  r.pairsRigidChecked = j.value("pairs_rigid_checked", size_t{0});
  r.rigidCheckedAt = j.value("rigid_checked_at", -1);
  r.clean = j.value("clean", true);
  r.worstSample = j.value("worst_sample", size_t{0});
  r.deflectionMm = j.value("deflection_mm", 0.0);
  r.pairsCached = j.value("pairs_cached", size_t{0});
  r.pairsRepeated = j.value("pairs_repeated", size_t{0});
  r.pairsUnknown = j.value("pairs_unknown", size_t{0});
  r.checkedClashes = j.value("checked_clashes", false);
  r.pairsChecked = j.value("pairs_checked", size_t{0});
  r.seconds = j.value("seconds", 0.0);
  r.cancelled = j.value("cancelled", false);
  r.refinedSamples = j.value("refined_samples", size_t{0});
  r.branchJumps = j.value("branch_jumps", size_t{0});
  r.error = j.value("error", std::string{});
  for (const json& fj : j.value("press_fits", json::array()))
    r.pressFits.push_back(PressFitFromJson(fj));
  if (j.contains("input_hash") &&
      !inputhash::FromHex(j.at("input_hash").get<std::string>(), r.inputHash))
    throw std::runtime_error("motion result: input_hash " +
                             j.at("input_hash").dump() +
                             " is not 0x and 16 hex digits");
  if (r.worstSample >= std::max<size_t>(r.samples.size(), 1))
    throw std::runtime_error("motion result: worst_sample " +
                             std::to_string(r.worstSample) + " of " +
                             std::to_string(r.samples.size()) + " samples");
  return r;
}

} // namespace DocumentIO
