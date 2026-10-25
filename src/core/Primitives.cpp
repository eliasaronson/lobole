// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/Primitives.h"

#include "core/Command.h"
#include "core/Document.h"
#include "core/features/DatumPlaneFeature.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/FrameFeature.h"
#include "core/features/RevolveFeature.h"
#include "core/features/SketchFeature.h"
#include "core/io/ReadContext.h" // CheckFeatureRoom
#include "core/io/Serialization.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <glm/gtc/constants.hpp>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>

using json = nlohmann::json;

namespace primitive {

const char* KindName(Kind kind) {
  switch (kind) {
  case Kind::Box:
    return "Box";
  case Kind::Cylinder:
    return "Cylinder";
  case Kind::Sphere:
    return "Sphere";
  }
  return "Box";
}

std::optional<Kind> KindFromName(const std::string& name) {
  if (name == "Box")
    return Kind::Box;
  if (name == "Cylinder")
    return Kind::Cylinder;
  if (name == "Sphere")
    return Kind::Sphere;
  return std::nullopt;
}

const char* AcceptedOn() {
  return "`on` is what the primitive stands on: \"Top\", \"Front\" or "
         "\"Right\" (a world plane); {feature_id, element} (a planar face); "
         "or {feature_id} of a DatumPlane, a Sketch (its plane) or a Frame "
         "(a mate connector). There is no bare world position: for a point "
         "off those, put it on a DatumPlane or a Frame placed there";
}

const char* AcceptedKeys(Kind kind) {
  switch (kind) {
  case Kind::Box:
    return "type, name, on, width, depth, height (each +_expr), offset [u, v] "
           "(+offset_expr), centered, mode, reversed, material, ids [solid, "
           "sketch]";
  case Kind::Cylinder:
    return "type, name, on, diameter or radius, height (each +_expr), offset "
           "[u, v] (+offset_expr), mode, reversed, material, ids [solid, "
           "sketch]";
  case Kind::Sphere:
    return "type, name, on, diameter or radius (+_expr), offset [u, v] "
           "(+offset_expr), mode, material, ids [solid, sketch]";
  }
  return "";
}

namespace {

std::string Lower(std::string s) {
  for (char& c : s)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// The world plane a person calls `word`, by the same names the tree shows
// (SketchPlane::StandardPlaneName), so "Top" is whichever plane the world
// frame says is up.
std::optional<PlaneType> WorldPlaneNamed(const std::string& word) {
  for (PlaneType t : {PlaneType::XY, PlaneType::XZ, PlaneType::YZ}) {
    std::string name = SketchPlane::StandardPlaneName(t);
    name = name.substr(0, name.find(' '));
    if (Lower(name) == Lower(word))
      return t;
  }
  return std::nullopt;
}

void ReadDim(const json& j, const char* key, Dim& out) {
  const std::string ekey = std::string(key) + "_expr";
  if (j.contains(key)) {
    if (!j.at(key).is_number())
      throw std::runtime_error(std::string(key) +
                               " is a number in mm (an expression goes in " +
                               ekey + ")");
    out.value = j.at(key).get<double>();
    out.set = true;
  }
  if (j.contains(ekey)) {
    if (!j.at(ekey).is_string())
      throw std::runtime_error(ekey + " is an expression string");
    out.expr = j.at(ekey).get<std::string>();
    out.set = out.set || !out.expr.empty();
  }
}

void WriteDim(json& j, const char* key, const Dim& d) {
  if (!d.set)
    return;
  j[key] = d.value;
  if (!d.expr.empty())
    j[std::string(key) + "_expr"] = d.expr;
}

} // namespace

On OnFromJson(const json& on) {
  On out;
  if (on.is_string()) {
    const auto t = WorldPlaneNamed(on.get<std::string>());
    if (!t)
      throw std::runtime_error("on: \"" + on.get<std::string>() +
                               "\" is not a world plane. " + AcceptedOn());
    out.kind = OnKind::WorldPlane;
    out.world = *t;
    return out;
  }
  if (!on.is_object() || !on.contains("feature_id") ||
      !on.at("feature_id").is_number_integer() ||
      on.at("feature_id").get<int64_t>() <= 0)
    throw std::runtime_error("on: " + on.dump() + " names nothing. " +
                             AcceptedOn());
  for (auto it = on.begin(); it != on.end(); ++it)
    if (it.key() != "feature_id" && it.key() != "element")
      throw std::runtime_error("on: unknown key '" + it.key() + "'. " +
                               AcceptedOn());
  const uint64_t id = on.at("feature_id").get<uint64_t>();
  if (on.contains("element")) {
    if (!on.at("element").is_string())
      throw std::runtime_error("on.element is a face name (list_elements)");
    out.kind = OnKind::Face;
    out.face = ElementRef{id, on.at("element").get<std::string>()};
    return out;
  }
  // A DatumPlane, a Sketch or a Frame: which one is the document's to say,
  // so Resolve settles the kind.
  out.kind = OnKind::PlaneFeature;
  out.featureId = id;
  return out;
}

json OnToJson(const On& on) {
  switch (on.kind) {
  case OnKind::WorldPlane: {
    std::string name = SketchPlane::StandardPlaneName(on.world);
    return name.substr(0, name.find(' '));
  }
  case OnKind::Face:
    return {{"feature_id", on.face.featureId}, {"element", on.face.name}};
  default:
    return {{"feature_id", on.featureId}};
  }
}

namespace {

// The size of the part: its largest bounding-box dimension, over the solid
// bodies (a reference ghost is not part of it). 0 when it has none.
double PartSize(const Document& doc) {
  glm::dvec3 lo(std::numeric_limits<double>::max());
  glm::dvec3 hi(std::numeric_limits<double>::lowest());
  bool any = false;
  for (const Body& b : doc.GetBodies()) {
    if (b.reference || !b.shape.IsValid())
      continue;
    glm::dvec3 a, c;
    b.shape.GetBoundingBox(a, c);
    lo = glm::min(lo, a);
    hi = glm::max(hi, c);
    any = true;
  }
  if (!any)
    return 0.0;
  const glm::dvec3 d = hi - lo;
  return std::max({d.x, d.y, d.z});
}

std::string FeatureLabel(const Feature& f) {
  return f.TypeName() + " '" + f.GetName() + "' (" + std::to_string(f.GetId()) +
         ")";
}

using K = sketch::ConstraintKind;

sketch::Constraint DimCon(K kind, sketch::PointRef a, sketch::PointRef b,
                          const Dim& d) {
  sketch::Constraint c{kind, a, b, d.value};
  c.valueExpr = d.expr;
  return c;
}

// Draws the profile and its dimensions into an empty sketch. Returns the id
// of the sphere's axis line (0 for the others).
uint32_t Fill(SketchFeature& sk, const Spec& s) {
  const sketch::PointRef origin{sketch::kOrigin, 0};
  const glm::dvec2 at(s.offsetU.value, s.offsetV.value);
  switch (s.kind) {
  case Kind::Box: {
    const glm::dvec2 size(s.width.value, s.depth.value);
    sketch::PointRef anchor;
    std::array<uint32_t, 4> sides{};
    if (s.centered) {
      const auto ids = sk.AddRectangleCentered(at, at + size * 0.5);
      sides = {ids[0], ids[1], ids[2], ids[3]};
      anchor = {ids[4], 0};
    } else {
      sides = sk.AddRectangle(at, at + size);
      anchor = {sides[0], 0}; // the bottom side's start: the low corner
    }
    sk.constraints.push_back(DimCon(K::DistanceX, origin, anchor, s.offsetU));
    sk.constraints.push_back(DimCon(K::DistanceY, origin, anchor, s.offsetV));
    sk.constraints.push_back(
        DimCon(K::DistanceX, {sides[0], 0}, {sides[0], 1}, s.width));
    sk.constraints.push_back(
        DimCon(K::DistanceY, {sides[1], 0}, {sides[1], 1}, s.depth));
    return 0;
  }
  case Kind::Cylinder: {
    const uint32_t c = sk.AddEntity(sketch::Circle{at, s.diameter.value * 0.5});
    sk.constraints.push_back(DimCon(K::Diameter, {c, 0}, {0, 0}, s.diameter));
    sk.constraints.push_back(DimCon(K::DistanceX, origin, {c, 0}, s.offsetU));
    sk.constraints.push_back(DimCon(K::DistanceY, origin, {c, 0}, s.offsetV));
    return 0;
  }
  case Kind::Sphere: {
    // A half disc over the u axis through the centre, revolved about its
    // own straight side. The centre is held ON that side (point-on-line,
    // one equation) rather than at its midpoint (two, one of them already
    // implied by the ends being on the arc), so the sketch is exactly
    // determined, with nothing redundant.
    const double r = s.diameter.value * 0.5;
    const uint32_t arc = sk.AddEntity(
        sketch::Arc{at, r, 0.0, glm::pi<double>()}); // +u through +v to -u
    const uint32_t axis = sk.AddEntity(
        sketch::Line{at - glm::dvec2(r, 0.0), at + glm::dvec2(r, 0.0)});
    sk.constraints.push_back({K::Coincident, {arc, 1}, {axis, 1}, 0.0});
    sk.constraints.push_back({K::Coincident, {arc, 2}, {axis, 0}, 0.0});
    sk.constraints.push_back({K::Horizontal, {axis, 0}, {0, 0}, 0.0});
    sk.constraints.push_back({K::PointOnLine, {arc, 0}, {axis, 0}, 0.0});
    sk.constraints.push_back(DimCon(K::Diameter, {arc, 0}, {0, 0}, s.diameter));
    sk.constraints.push_back(DimCon(K::DistanceX, origin, {arc, 0}, s.offsetU));
    sk.constraints.push_back(DimCon(K::DistanceY, origin, {arc, 0}, s.offsetV));
    return axis;
  }
  }
  return 0;
}

void SetSolid(Feature& solid, const Spec& s, uint32_t axisLine) {
  if (auto* e = dynamic_cast<ExtrudeFeature*>(&solid)) {
    e->distance = s.height.value;
    e->distanceExpr = s.height.expr;
    e->mode = s.mode;
    e->reversed = s.reversed;
  } else if (auto* r = dynamic_cast<RevolveFeature*>(&solid)) {
    r->axis.kind = AxisRef::Kind::SketchLine;
    r->axis.entityId = axisLine;
    r->mode = s.mode;
  }
}

} // namespace

Spec SpecFromJson(const json& j) {
  if (!j.is_object())
    throw std::runtime_error("a primitive is a JSON object");
  const auto kind = KindFromName(j.value("type", std::string{}));
  if (!kind)
    throw std::runtime_error("not a primitive type: Box, Cylinder or Sphere");
  Spec s;
  s.kind = *kind;
  const std::string type = KindName(s.kind);
  // The old world-coordinate dialect, refused by name: the fields look
  // plausible and used to work, so an agent will send them.
  for (const char* old : DocumentIO::kPrimitiveRetiredKeys)
    if (j.contains(old))
      throw std::runtime_error(
          type + " no longer takes `" + old +
          "`: a primitive is a sketch ON something, extruded or revolved, "
          "and placed by `offset` [u, v] in that thing's frame. " +
          AcceptedOn() + ".");
  std::set<std::string> allowed = {"type",        "name", "on",       "offset",
                                   "offset_expr", "mode", "material", "ids"};
  if (s.kind == Kind::Box)
    allowed.insert({"width", "width_expr", "depth", "depth_expr", "height",
                    "height_expr", "centered", "reversed"});
  else
    allowed.insert({"diameter", "diameter_expr", "radius", "radius_expr"});
  if (s.kind == Kind::Cylinder)
    allowed.insert({"height", "height_expr", "reversed"});
  for (auto it = j.begin(); it != j.end(); ++it)
    if (allowed.count(it.key()) == 0)
      throw std::runtime_error("unknown field '" + it.key() + "' for " + type +
                               ". It accepts: " + AcceptedKeys(s.kind));

  if (!j.contains("on"))
    throw std::runtime_error(type + " needs `on`. " + AcceptedOn() + ".");
  s.on = OnFromJson(j.at("on"));
  s.name = j.value("name", type);

  if (s.kind == Kind::Box) {
    ReadDim(j, "width", s.width);
    ReadDim(j, "depth", s.depth);
    ReadDim(j, "height", s.height);
    if (j.contains("centered")) {
      if (!j.at("centered").is_boolean())
        throw std::runtime_error("centered is true or false");
      s.centered = j.at("centered").get<bool>();
    }
  } else {
    Dim r;
    ReadDim(j, "radius", r);
    ReadDim(j, "diameter", s.diameter);
    if (r.set && s.diameter.set)
      throw std::runtime_error(type + " takes diameter OR radius, not both");
    if (r.set) {
      s.diameter.value = 2.0 * r.value;
      s.diameter.expr = r.expr.empty() ? "" : "(" + r.expr + ") * 2";
      s.diameter.set = true;
    }
    if (s.kind == Kind::Cylinder)
      ReadDim(j, "height", s.height);
  }

  if (j.contains("offset")) {
    const json& o = j.at("offset");
    if (!o.is_array() || o.size() != 2 || !o.at(0).is_number() ||
        !o.at(1).is_number())
      throw std::runtime_error("offset is [u, v] in mm, in the frame of what "
                               "it is on");
    s.offsetU = {o.at(0).get<double>(), "", true};
    s.offsetV = {o.at(1).get<double>(), "", true};
  }
  if (j.contains("offset_expr")) {
    const json& o = j.at("offset_expr");
    if (!o.is_array() || o.size() != 2 || !o.at(0).is_string() ||
        !o.at(1).is_string())
      throw std::runtime_error("offset_expr is [u, v] as expression strings "
                               "(\"\" for a literal)");
    s.offsetU.expr = o.at(0).get<std::string>();
    s.offsetV.expr = o.at(1).get<std::string>();
    s.offsetU.set = true;
    s.offsetV.set = true;
  }

  if (j.contains("mode")) {
    const std::string m = j.at("mode").is_string()
                              ? j.at("mode").get<std::string>()
                              : std::string("<not a string>");
    if (m == "new_body")
      s.mode = ExtrudeMode::NewBody;
    else if (m == "add")
      s.mode = ExtrudeMode::Add;
    else if (m == "cut")
      s.mode = ExtrudeMode::Cut;
    else
      throw std::runtime_error("mode '" + m +
                               "' is not one of new_body, add, cut");
  }
  if (j.contains("reversed")) {
    if (!j.at("reversed").is_boolean())
      throw std::runtime_error("reversed is true or false");
    s.reversed = j.at("reversed").get<bool>();
  }
  if (j.contains("ids")) {
    const json& ids = j.at("ids");
    const auto positive = [](const json& v) {
      return v.is_number_integer() && v.get<int64_t>() > 0;
    };
    if (!ids.is_array() || ids.size() != 2 || !positive(ids.at(0)) ||
        !positive(ids.at(1)) || ids.at(0) == ids.at(1))
      throw std::runtime_error("ids is [solid id, sketch id]: two different "
                               "positive feature ids, or leave it out");
    s.solidId = ids.at(0).get<uint64_t>();
    s.sketchId = ids.at(1).get<uint64_t>();
  }
  if (j.contains("material")) {
    if (!j.at("material").is_string())
      throw std::runtime_error("material is a slug (describe material_slugs)");
    s.material = j.at("material").get<std::string>();
  }
  return s;
}

json SpecToJson(const Spec& s) {
  json j = {
      {"type", KindName(s.kind)}, {"name", s.name}, {"on", OnToJson(s.on)}};
  if (s.kind == Kind::Box) {
    WriteDim(j, "width", s.width);
    WriteDim(j, "depth", s.depth);
    WriteDim(j, "height", s.height);
    if (!s.centered)
      j["centered"] = false;
  } else {
    WriteDim(j, "diameter", s.diameter);
    if (s.kind == Kind::Cylinder)
      WriteDim(j, "height", s.height);
  }
  if (s.offsetU.set || s.offsetV.set) {
    j["offset"] = {s.offsetU.value, s.offsetV.value};
    if (!s.offsetU.expr.empty() || !s.offsetV.expr.empty())
      j["offset_expr"] = {s.offsetU.expr, s.offsetV.expr};
  }
  if (s.mode != ExtrudeMode::NewBody)
    j["mode"] = s.mode == ExtrudeMode::Add ? "add" : "cut";
  if (s.reversed)
    j["reversed"] = true;
  if (!s.material.empty())
    j["material"] = s.material;
  if (s.solidId != 0 && s.sketchId != 0)
    j["ids"] = {s.solidId, s.sketchId};
  return j;
}

std::expected<Placement, std::string> Resolve(const Document& doc,
                                              const On& on) {
  Placement p;
  switch (on.kind) {
  case OnKind::WorldPlane:
    p.plane = SketchPlane(on.world);
    p.size = PartSize(doc);
    return p;
  case OnKind::Face: {
    const Feature* owner = doc.FindFeatureById(on.face.featureId);
    if (owner == nullptr)
      return std::unexpected("on: no feature " +
                             std::to_string(on.face.featureId) +
                             " (get_state lists features with their ids)");
    for (const Body& body : doc.GetBodies()) {
      if (body.createdBy != on.face.featureId)
        continue;
      const auto idx = body.elementMap.Resolve(on.face.name);
      if (!idx)
        continue;
      glm::dvec3 c, n, u;
      if (!body.shape.FacePlane(*idx, c, n, u)) {
        glm::dvec3 o, ax, x;
        const bool round = body.shape.FaceAxis(*idx, o, ax, x);
        return std::unexpected(
            "on: face '" + on.face.name + "' of " + FeatureLabel(*owner) +
            " is " + (round ? "curved" : "not planar") +
            ". A primitive goes on a planar face; for a round one, put it on "
            "a Frame of that face (its Z is the axis, its X the seam)");
      }
      p.plane = SketchPlane(c, n, u);
      if (body.shape.FaceExtent(*idx, c, p.plane.GetUAxis(), p.plane.GetVAxis(),
                                p.lo, p.hi)) {
        p.hasExtent = true;
        p.size = std::min(p.hi.x - p.lo.x, p.hi.y - p.lo.y);
      }
      return p;
    }
    return std::unexpected("on: " + FeatureLabel(*owner) +
                           " has no face named '" + on.face.name +
                           "' (list_elements names its faces)");
  }
  case OnKind::PlaneFeature: {
    const Feature* f = doc.FindFeatureById(on.featureId);
    if (f == nullptr)
      return std::unexpected("on: no feature " + std::to_string(on.featureId) +
                             " (get_state lists features with their ids)");
    if (!f->GetError().empty())
      return std::unexpected("on: " + FeatureLabel(*f) + " is in error (" +
                             f->GetError() +
                             "); a primitive cannot stand on it");
    if (const auto* d = dynamic_cast<const DatumPlaneFeature*>(f))
      p.plane = d->OutputPlane();
    else if (const auto* sk = dynamic_cast<const SketchFeature*>(f))
      p.plane = sk->plane;
    else if (const auto* fr = dynamic_cast<const FrameFeature*>(f))
      p.plane = SketchPlane(fr->Origin(), fr->ZAxis(), fr->XAxis());
    else
      return std::unexpected(
          "on: " + FeatureLabel(*f) +
          " is not a plane. Name one of its faces with `element` "
          "({feature_id, element}), or name a DatumPlane, a Sketch or a "
          "Frame");
    p.size = PartSize(doc);
    return p;
  }
  }
  return std::unexpected(std::string(AcceptedOn()));
}

std::string ApplyDefaults(Spec& s, const Placement& p, double fallbackSize) {
  double size = p.size;
  // A face is what the primitive stands on, so half of it; a plane or a
  // frame has no outline, so a quarter of the part around it.
  double scale = p.hasExtent ? 0.5 : 0.25;
  if (size <= 0.0) {
    size = fallbackSize;
    scale = 1.0;
  }
  const bool missing =
      (s.kind == Kind::Box && !(s.width.set && s.depth.set && s.height.set)) ||
      (s.kind != Kind::Box && !s.diameter.set) ||
      (s.kind == Kind::Cylinder && !s.height.set);
  if (missing && size <= 0.0)
    return std::string("nothing to size a default ") + KindName(s.kind) +
           " from: the part has no bodies and the reference has no outline. "
           "Pass its sizes (" +
           (s.kind == Kind::Box        ? "width, depth, height"
            : s.kind == Kind::Cylinder ? "diameter, height"
                                       : "diameter") +
           ")";
  const double d = size * scale;
  for (Dim* dim : {&s.width, &s.depth, &s.height, &s.diameter})
    if (!dim->set)
      dim->value = d;
  return "";
}

std::string EvaluateDims(Spec& s, const SymbolTable& symbols) {
  struct Named {
    const char* name;
    Dim* dim;
    bool size;
  };
  std::vector<Named> dims = {{"offset u", &s.offsetU, false},
                             {"offset v", &s.offsetV, false}};
  if (s.kind == Kind::Box)
    dims.insert(dims.end(), {{"width", &s.width, true},
                             {"depth", &s.depth, true},
                             {"height", &s.height, true}});
  else
    dims.push_back({"diameter", &s.diameter, true});
  if (s.kind == Kind::Cylinder)
    dims.push_back({"height", &s.height, true});
  for (const Named& n : dims) {
    if (!n.dim->expr.empty()) {
      const auto v =
          ExpressionEngine::Default().EvaluateText(n.dim->expr, symbols);
      if (!v)
        return std::string(n.name) + " '" + n.dim->expr +
               "': " + v.error().message;
      n.dim->value = *v;
    }
    if (n.size && !(n.dim->value > 0.0))
      return std::string(KindName(s.kind)) + " " + n.name +
             " must be positive, not " + std::to_string(n.dim->value);
  }
  return "";
}

Expansion Expand(const Spec& s, const Placement& p) {
  Expansion out;
  out.sketch = std::make_unique<SketchFeature>(s.name + " sketch", p.plane);
  SketchFeature& sk = *out.sketch;
  switch (s.on.kind) {
  case OnKind::Face:
    sk.faceRef = s.on.face;
    break;
  case OnKind::PlaneFeature:
    sk.planeRef = s.on.featureId;
    break;
  case OnKind::WorldPlane:
    break; // a base plane is baked: the world does not move
  }
  const uint32_t axis = Fill(sk, s);
  if (s.kind == Kind::Sphere)
    out.solid =
        std::make_unique<RevolveFeature>(s.name, 0, 360.0, s.mode, false);
  else
    out.solid = std::make_unique<ExtrudeFeature>(s.name, 0, s.height.value,
                                                 s.mode, s.reversed);
  SetSolid(*out.solid, s, axis);
  if (!s.material.empty())
    out.solid->SetMaterial(s.material);
  out.sketch->SetId(s.sketchId);
  out.solid->SetId(s.solidId);
  return out;
}

void BindSketch(Feature& solid, uint64_t sketchId) {
  if (auto* e = dynamic_cast<ExtrudeFeature*>(&solid))
    e->sketchId = sketchId;
  else if (auto* r = dynamic_cast<RevolveFeature*>(&solid)) {
    r->sketchId = sketchId;
    r->axis.sketchId = sketchId;
  }
}

SymbolTable Symbols(const Document& doc) {
  SymbolTable out;
  if (const SymbolTable* project = doc.ProjectSymbols())
    out = *project;
  for (const auto& [name, value] : doc.Symbols())
    out[name] = value;
  return out;
}

Added Add(Document& doc, CommandStack& commands, Spec spec,
          double fallbackSize) {
  const std::string type = KindName(spec.kind);
  // The sketch and the solid.
  if (const std::string why = readctx::CheckFeatureRoom(doc, 2); !why.empty())
    throw std::runtime_error(type + ": " + why);
  for (uint64_t id : {spec.solidId, spec.sketchId})
    if (id != 0 && doc.FindFeatureById(id) != nullptr)
      throw std::runtime_error(type + ": ids: feature " + std::to_string(id) +
                               " already exists");
  const auto placement = Resolve(doc, spec.on);
  if (!placement)
    throw std::runtime_error(type + ": " + placement.error());
  if (const std::string why = ApplyDefaults(spec, *placement, fallbackSize);
      !why.empty())
    throw std::runtime_error(type + ": " + why);
  if (const std::string why = EvaluateDims(spec, Symbols(doc)); !why.empty())
    throw std::runtime_error(type + ": " + why);
  Expansion ex = Expand(spec, *placement);
  const size_t depth = commands.UndoDepth();
  Added out;
  bool ok = commands.Execute(
      std::make_unique<AddFeatureCommand>(std::move(ex.sketch)), doc);
  if (ok) {
    out.sketchId = doc.GetFeature(doc.FeatureCount() - 1)->GetId();
    BindSketch(*ex.solid, out.sketchId);
    ok = commands.Execute(
        std::make_unique<AddFeatureCommand>(std::move(ex.solid)), doc);
  }
  if (!ok) {
    commands.RollbackSince(depth, doc);
    throw std::runtime_error(type + ": the features it expands to could not "
                                    "be added; the document is unchanged");
  }
  out.solidId = doc.GetFeature(doc.FeatureCount() - 1)->GetId();
  commands.CoalesceSince(depth, "Add " + spec.name);
  return out;
}

void Refill(SketchFeature& sketch, Feature& solid, const Spec& s) {
  sketch.entities.clear();
  sketch.constraints.clear();
  sketch.entityExprs.clear();
  const uint32_t axis = Fill(sketch, s);
  SetSolid(solid, s, axis);
}

} // namespace primitive
