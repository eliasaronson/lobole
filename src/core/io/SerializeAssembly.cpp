#include "core/io/Serialization.h"
#include "core/io/SerializationInternal.h"

#include "cad/CADMaterials.h"
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
  // What it rides on (v72). Omitted when free, so every existing assembly
  // serializes byte-identically.
  if (o.attachedTo != 0) {
    j["attached_to"] = o.attachedTo;
    j["attach_offset"] = {o.attachOffset.x, o.attachOffset.y, o.attachOffset.z};
    j["attach_rotation"] = {o.attachRotation.x, o.attachRotation.y,
                            o.attachRotation.z, o.attachRotation.w};
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
      if (d.dx != 0.0f || d.dy != 0.0f) {
        dj["dx"] = d.dx;
        dj["dy"] = d.dy;
      }
      if (d.ballRotation != glm::quat(1.0f, 0.0f, 0.0f, 0.0f))
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
    o.attachOffset = glm::vec3((*it)[0].get<float>(), (*it)[1].get<float>(),
                               (*it)[2].get<float>());
  }
  if (const auto it = j.find("attach_rotation");
      it != j.end() && it->is_array() && it->size() == 4) {
    o.attachRotation = glm::quat((*it)[3].get<float>(), (*it)[0].get<float>(),
                                 (*it)[1].get<float>(), (*it)[2].get<float>());
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
    d.rotation = dj.value("rotation", 0.0f);
    d.offset = dj.value("offset", 0.0f);
    d.dx = dj.value("dx", 0.0f);
    d.dy = dj.value("dy", 0.0f);
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
    {"gear", RelationKind::Gear},           {"rack", RelationKind::Rack},
    {"screw", RelationKind::Screw},         {"linear", RelationKind::Linear},
    {"chain", RelationKind::Chain},         {"cam", RelationKind::Cam},
    {"freewheel", RelationKind::Freewheel}, {"click", RelationKind::Click}};

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
    s.offset = glm::vec3(o.at(0).get<float>(), o.at(1).get<float>(),
                         o.at(2).get<float>());
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
  if (s.configuration)
    j["configuration"] = *s.configuration; // v128; 0 = the base
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
  return s;
}

Relation RelationFromJson(const json& j) {
  Relation r;
  r.id = j.at("id").get<uint64_t>();
  r.name = j.value("name", std::string{});
  r.kind = RelationKindFromString(j.at("kind").get<std::string>());
  r.mateA = j.at("mate_a").get<uint64_t>();
  r.mateB = j.at("mate_b").get<uint64_t>();
  if (j.contains("mate_a_path")) // v129
    r.mateAPath = j.at("mate_a_path").get<std::vector<uint64_t>>();
  if (j.contains("mate_b_path"))
    r.mateBPath = j.at("mate_b_path").get<std::vector<uint64_t>>();
  r.dofA = MateDofFromString(j.at("dof_a").get<std::string>());
  r.dofB = MateDofFromString(j.at("dof_b").get<std::string>());
  r.ratio = j.at("ratio").get<double>();
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

} // namespace DocumentIO
