#include "core/io/Serialization.h"
#include "core/io/SerializationInternal.h"

#include "core/Angles.h"
#include "core/Feature.h"
#include "core/features/BaseFlangeFeature.h"
#include "core/features/BooleanFeature.h"
#include "core/features/CornerReliefFeature.h"
#include "core/features/CurveFeature.h"
#include "core/features/DatumAxisFeature.h"
#include "core/features/DatumPlaneFeature.h"
#include "core/features/DerivedFeature.h"
#include "core/features/DraftFeature.h"
#include "core/features/EdgeFlangeFeature.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/FastenerFeature.h"
#include "core/features/FilletFeature.h"
#include "core/features/FlatPatternFeature.h"
#include "core/features/FrameFeature.h"
#include "core/features/HemFeature.h"
#include "core/features/HoleFeature.h"
#include "core/features/JogFeature.h"
#include "core/features/LoftFeature.h"
#include "core/features/PatternFeature.h"
#include "core/features/PipeFeature.h"
#include "core/features/RefineFeature.h"
#include "core/features/RefoldFeature.h"
#include "core/features/RevolveFeature.h"
#include "core/features/RibFeature.h"
#include "core/features/ShellFeature.h"
#include "core/features/SketchFeature.h"
#include "core/features/SplitFeature.h"
#include "core/features/SweepFeature.h"
#include "core/features/ThreadFeature.h"
#include "core/features/UnfoldFeature.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <sstream>
#include <string>

using nlohmann::json;

namespace {

json EntityToJson(const sketch::Entity& entity) {
  json j = std::visit(
      [](const auto& e) -> json {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, sketch::Point>) {
          return {{"type", "point"}, {"p", Vec2ToJson(e.p)}};
        } else if constexpr (std::is_same_v<T, sketch::Line>) {
          return {
              {"type", "line"}, {"a", Vec2ToJson(e.a)}, {"b", Vec2ToJson(e.b)}};
        } else if constexpr (std::is_same_v<T, sketch::Circle>) {
          return {{"type", "circle"},
                  {"center", Vec2ToJson(e.center)},
                  {"radius", e.radius}};
        } else if constexpr (std::is_same_v<T, sketch::Arc>) {
          return {{"type", "arc"},
                  {"center", Vec2ToJson(e.center)},
                  {"radius", e.radius},
                  {"start_angle", e.startAngle},
                  {"end_angle", e.endAngle}};
        } else if constexpr (std::is_same_v<T, sketch::Ellipse>) {
          return {{"type", "ellipse"},
                  {"center", Vec2ToJson(e.center)},
                  {"r_major", e.rMajor},
                  {"r_minor", e.rMinor},
                  {"rotation", e.rotation}};
        } else if constexpr (std::is_same_v<T, sketch::EllipseArc>) {
          return {{"type", "ellipse_arc"},  {"center", Vec2ToJson(e.center)},
                  {"r_major", e.rMajor},    {"r_minor", e.rMinor},
                  {"rotation", e.rotation}, {"start_param", e.startParam},
                  {"end_param", e.endParam}};
        } else { // BSpline
          json poles = json::array();
          for (const glm::vec2& p : e.poles)
            poles.push_back(Vec2ToJson(p));
          return {{"type", "spline"},
                  {"poles", std::move(poles)},
                  {"degree", e.degree}};
        }
      },
      entity);
  j["id"] = sketch::EntityId(entity);
  if (sketch::IsConstruction(entity))
    j["construction"] = true; // omit when false to keep JSON compact
  return j;
}

// Radians from whichever spelling the caller used, refusing both at once.
//
// Mirrors RemoteAssembly's MateRotationRadians, and is here for the same
// reason it is there: the bug is never the unit, it is a field that NAMES no
// unit in an API whose other angles all do — Revolve.angle_deg,
// Pattern.angle_deg, Frame.rotation_deg, a cam relation's duration_deg. A
// mate's `rotation` read that way pinned a linkage to 60 radians and was
// diagnosed as a loop-solver branch bug twice. A sketch arc read that way
// builds a plausible arc of the wrong size and says nothing at all. These
// looked like the last angles in the API still naming no unit. They were not:
// `pattern`'s per-step angle, `set_reference_image.rotation` and
// `add_constraint.value` followed on 2026-09-10 (docs/units-plan.md), and
// tests/rpc_units_gate.py is what stops the next one.

double AngleRadians(const json& j, const char* radKey, const char* degKey) {
  const bool hasRad = j.contains(radKey);
  const bool hasDeg = j.contains(degKey);
  if (hasRad && hasDeg)
    throw std::runtime_error(std::string("pass ") + radKey + " (radians) or " +
                             degKey + " (degrees), not both");
  if (hasDeg)
    return angles::ToRadians(j.at(degKey).get<double>());
  if (!hasRad)
    throw std::runtime_error(
        std::string("missing ") + radKey + " (RADIANS) or " + degKey +
        " (degrees) — describe sketch_entity_types[] lists every field");
  return j.at(radKey).get<double>();
}

// The same, for an angle that may legitimately be absent (a constraint driven
// by `expr` carries no literal), and which is a PINNED angle rather than a
// sweep — so more than a full turn is not a wrapped arc to be measured, it is
// a number that was meant as degrees. Refusing names the likely cause; the
// alternative is a sketch that solves to a plausible wrong shape.
double AngleRadians(const json& j, const char* radKey, const char* degKey,
                    double absent) {
  if (!j.contains(radKey) && !j.contains(degKey))
    return absent;
  const double rad = AngleRadians(j, radKey, degKey);
  if (j.contains(radKey) && !angles::IsPlausibleSweep(rad))
    throw std::runtime_error(
        std::string(radKey) + " is RADIANS and " + std::to_string(rad) +
        " is more than a full turn — pass " + degKey + " to say degrees");
  return rad;
}

sketch::Entity EntityFromJson(const json& j) {
  const std::string type = j.at("type").get<std::string>();
  // Pre-v4 files have no "id"; leave it 0 so Load can detect and migrate.
  const uint32_t id = j.value("id", uint32_t{0});
  // kSketchEntityTypes is the authority. Rejecting an untabled type BEFORE
  // the chain is what makes a branch-without-a-row unreachable, so an entity
  // cannot load, save and regenerate perfectly while being invisible to
  // `describe` — the failure kFeatureTypes was given the same guard for.
  // "rect" is expanded by FeatureFromJson before it reaches here.
  if (type != "rect") {
    std::string known;
    bool listed = false;
    for (const DocumentIO::SketchEntityInfo& info :
         DocumentIO::SketchEntityTypes()) {
      known += (known.empty() ? "" : ", ") + std::string(info.type);
      listed = listed || type == info.type;
    }
    if (!listed)
      throw std::runtime_error("Unknown sketch entity type: " + type +
                               ". Known types: " + known);
  }
  sketch::Entity e;
  if (type == "point") {
    e = sketch::Point{Vec2FromJson(j.at("p"))};
  } else if (type == "line") {
    e = sketch::Line{Vec2FromJson(j.at("a")), Vec2FromJson(j.at("b"))};
  } else if (type == "circle") {
    e = sketch::Circle{Vec2FromJson(j.at("center")),
                       j.at("radius").get<double>()};
  } else if (type == "arc") {
    const double start = AngleRadians(j, "start_angle", "start_angle_deg");
    const double end = AngleRadians(j, "end_angle", "end_angle_deg");
    // Refused on GEOMETRY, not on a guess about units: a sweep of more than a
    // full turn is a circle, so it is not a drawable arc whatever the caller
    // meant. But the overwhelmingly likely cause is degrees in a radian
    // field, so the message says so rather than making them work it out.
    const double sweep = std::abs(end - start);
    if (sweep > angles::kTwoPi + 1e-9) {
      std::ostringstream why;
      why << "arc sweeps " << sweep << " rad (" << angles::ToDegrees(sweep)
          << " degrees), which is more than one full turn — start_angle and "
             "end_angle are RADIANS. Did you mean start_angle_deg / "
             "end_angle_deg?";
      throw std::runtime_error(why.str());
    }
    e = sketch::Arc{Vec2FromJson(j.at("center")), j.at("radius").get<double>(),
                    start, end};
  } else if (type == "ellipse") {
    e = sketch::Ellipse{Vec2FromJson(j.at("center")),
                        j.at("r_major").get<double>(),
                        j.at("r_minor").get<double>(),
                        AngleRadians(j, "rotation", "rotation_deg")};
  } else if (type == "ellipse_arc") { // v46
    // start_param/end_param are the ellipse PARAMETER, not an angle from the
    // centre, so they get no _deg twin — naming one would be a lie about what
    // the number means. A full-turn sweep is legal here for the same reason.
    e = sketch::EllipseArc{Vec2FromJson(j.at("center")),
                           j.at("r_major").get<double>(),
                           j.at("r_minor").get<double>(),
                           AngleRadians(j, "rotation", "rotation_deg"),
                           j.at("start_param").get<double>(),
                           j.at("end_param").get<double>()};
  } else if (type == "spline") { // v40
    sketch::BSpline bs;
    for (const auto& pj : j.at("poles"))
      bs.poles.push_back(Vec2FromJson(pj));
    bs.degree = j.value("degree", 3);
    e = std::move(bs);
  } else {
    // "rect" is intercepted and expanded in FeatureFromJson (pre-v5 rigid
    // rectangles become four lines); anything else is unknown. Decision-
    // complete, like the feature-type refusal: name the vocabulary so the
    // correction needs no second round-trip.
    // A row with no branch lands here; the guard at the top of this function
    // catches the other direction. test_serialization.cpp pins both.
    throw std::runtime_error("Sketch entity type \"" + type +
                             "\" is documented but has no branch for it");
  }
  sketch::SetEntityId(e, id);
  sketch::SetConstruction(e, j.value("construction", false));
  return e;
}

const char* ConstraintKindToString(sketch::ConstraintKind k) {
  using K = sketch::ConstraintKind;
  switch (k) {
  case K::Coincident:
    return "coincident";
  case K::Horizontal:
    return "horizontal";
  case K::Vertical:
    return "vertical";
  case K::Parallel:
    return "parallel";
  case K::Perpendicular:
    return "perpendicular";
  case K::Equal:
    return "equal";
  case K::Distance:
    return "distance";
  case K::Radius:
    return "radius";
  case K::Angle:
    return "angle";
  case K::Tangent:
    return "tangent";
  case K::PointOnLine:
    return "point_on_line";
  case K::Symmetric:
    return "symmetric";
  case K::Midpoint:
    return "midpoint";
  case K::Fixed:
    return "fixed";
  case K::Concentric:
    return "concentric";
  case K::Collinear:
    return "collinear";
  case K::DistanceX:
    return "distance_x";
  case K::DistanceY:
    return "distance_y";
  case K::Diameter:
    return "diameter";
  case K::DistancePL:
    return "distance_pl";
  }
  return "coincident";
}

sketch::ConstraintKind ConstraintKindFromString(const std::string& s) {
  using K = sketch::ConstraintKind;
  if (s == "coincident")
    return K::Coincident;
  if (s == "horizontal")
    return K::Horizontal;
  if (s == "vertical")
    return K::Vertical;
  if (s == "parallel")
    return K::Parallel;
  if (s == "perpendicular")
    return K::Perpendicular;
  if (s == "equal")
    return K::Equal;
  if (s == "distance")
    return K::Distance;
  if (s == "radius")
    return K::Radius;
  if (s == "angle")
    return K::Angle;
  if (s == "tangent")
    return K::Tangent;
  if (s == "point_on_line")
    return K::PointOnLine;
  if (s == "symmetric")
    return K::Symmetric;
  if (s == "midpoint")
    return K::Midpoint;
  if (s == "fixed")
    return K::Fixed;
  if (s == "concentric")
    return K::Concentric;
  if (s == "collinear")
    return K::Collinear;
  if (s == "distance_x")
    return K::DistanceX;
  if (s == "distance_y")
    return K::DistanceY;
  if (s == "diameter")
    return K::Diameter;
  if (s == "distance_pl")
    return K::DistancePL;
  throw std::runtime_error("Unknown constraint kind: " + s);
}

json ConstraintToJson(const sketch::Constraint& c) {
  json j = {{"kind", ConstraintKindToString(c.kind)},
            {"a", {c.a.entity, c.a.which}},
            {"b", {c.b.entity, c.b.which}},
            {"value", c.value}};
  if (!c.valueExpr.empty()) // literals stay plain numbers (v11)
    j["expr"] = c.valueExpr;
  if (c.kind == sketch::ConstraintKind::Symmetric) // only Symmetric uses c
    j["c"] = {c.c.entity, c.c.which};
  if (sketch::IsDimension(c.kind) &&
      (c.labelOffset.x != 0 || c.labelOffset.y != 0))
    j["label"] = Vec2ToJson(c.labelOffset);
  return j;
}

sketch::Constraint ConstraintFromJson(const json& j) {
  sketch::Constraint c;
  c.kind = ConstraintKindFromString(j.at("kind").get<std::string>());
  c.a = {j.at("a").at(0).get<uint32_t>(), j.at("a").at(1).get<uint8_t>()};
  c.b = {j.at("b").at(0).get<uint32_t>(), j.at("b").at(1).get<uint8_t>()};
  // An angle takes `value` (RADIANS, what the file stores and the solver
  // works in) or `value_deg`, matching add_constraint and the arc parser
  // above. Input only — ConstraintToJson still writes `value` alone, so no
  // format bump is owed. Every other dimension kind is millimetres, where
  // there is nothing to confuse.
  c.value = c.kind == sketch::ConstraintKind::Angle
                ? AngleRadians(j, "value", "value_deg", 0.0)
                : j.value("value", 0.0);
  c.valueExpr = j.value("expr", std::string{}); // absent (pre-v11) = literal
  if (j.contains("c"))
    c.c = {j.at("c").at(0).get<uint32_t>(), j.at("c").at(1).get<uint8_t>()};
  if (j.contains("label"))
    c.labelOffset = Vec2FromJson(j.at("label"));
  return c;
}

json DerivedOpToJson(const sketch::DerivedOp& op) {
  json insts = json::array();
  for (const auto& in : op.instances)
    insts.push_back({{"source", in.sourceId}, {"k", in.k}, {"gen", in.genId}});
  json j = {{"kind",
             op.kind == sketch::DerivedKind::CircularPattern
                 ? "circular_pattern"
             : op.kind == sketch::DerivedKind::LinkedCopy ? "linked_copy" // v35
             : op.kind == sketch::DerivedKind::Mirror     ? "mirror"      // v51
                                                          : "linear_pattern"},
            {"source_ids", op.sourceIds},
            {"vec", Vec2ToJson(op.vec)},
            {"scalar", op.scalar},
            {"count", op.count},
            {"instances", insts}};
  if (op.count2 > 1) { // v50: 2D linear grid; omitted = 1D (back-compat)
    j["vec2"] = Vec2ToJson(op.vec2);
    j["count2"] = op.count2;
  }
  if (op.axisEntity != 0) // v51: mirror axis
    j["axis_entity"] = op.axisEntity;
  if (op.centerEntity != 0) { // v51: circular centre reference
    j["center_entity"] = op.centerEntity;
    j["center_which"] = static_cast<int>(op.centerWhich);
  }
  if (op.sourceSketchId != 0)
    j["source_sketch_id"] = op.sourceSketchId;
  return j;
}

sketch::DerivedOp DerivedOpFromJson(const json& j) {
  sketch::DerivedOp op;
  const std::string kind = j.at("kind").get<std::string>();
  op.kind = kind == "circular_pattern" ? sketch::DerivedKind::CircularPattern
            : kind == "linked_copy"    ? sketch::DerivedKind::LinkedCopy
            : kind == "mirror"         ? sketch::DerivedKind::Mirror // v51
                                       : sketch::DerivedKind::LinearPattern;
  op.sourceSketchId = j.value("source_sketch_id", uint64_t{0});
  for (const auto& s : j.at("source_ids"))
    op.sourceIds.push_back(s.get<uint32_t>());
  op.vec = Vec2FromJson(j.at("vec"));
  op.scalar = j.at("scalar").get<double>();
  op.count = j.at("count").get<int>();
  if (j.contains("vec2")) // v50: 2D linear grid
    op.vec2 = Vec2FromJson(j.at("vec2"));
  op.count2 = j.value("count2", 1);
  op.axisEntity = j.value("axis_entity", uint32_t{0});     // v51
  op.centerEntity = j.value("center_entity", uint32_t{0}); // v51
  op.centerWhich = static_cast<uint8_t>(j.value("center_which", 0));
  for (const auto& in : j.at("instances"))
    op.instances.push_back({in.at("source").get<uint32_t>(),
                            in.at("k").get<int>(),
                            in.at("gen").get<uint32_t>()});
  return op;
}

const char* ExtrudeModeToString(ExtrudeMode mode) {
  switch (mode) {
  case ExtrudeMode::NewBody:
    return "new_body";
  case ExtrudeMode::Add:
    return "add";
  case ExtrudeMode::Cut:
    return "cut";
  }
  return "new_body";
}

ExtrudeMode ExtrudeModeFromString(const std::string& s) {
  if (s == "new_body")
    return ExtrudeMode::NewBody;
  if (s == "add")
    return ExtrudeMode::Add;
  if (s == "cut")
    return ExtrudeMode::Cut;
  throw std::runtime_error("Unknown extrude mode: " + s);
}

const char* BooleanOpToString(BooleanOp op) {
  switch (op) {
  case BooleanOp::Union:
    return "union";
  case BooleanOp::Subtract:
    return "subtract";
  case BooleanOp::Intersect:
    return "intersect";
  }
  return "union";
}

BooleanOp BooleanOpFromString(const std::string& s) {
  if (s == "union")
    return BooleanOp::Union;
  if (s == "subtract")
    return BooleanOp::Subtract;
  if (s == "intersect")
    return BooleanOp::Intersect;
  throw std::runtime_error("Unknown boolean op: " + s);
}

const char* PatternKindToString(PatternKind kind) {
  switch (kind) {
  case PatternKind::Mirror:
    return "mirror";
  case PatternKind::Linear:
    return "linear";
  case PatternKind::Circular:
    return "circular";
  }
  return "linear";
}

PatternKind PatternKindFromString(const std::string& s) {
  if (s == "mirror")
    return PatternKind::Mirror;
  if (s == "linear")
    return PatternKind::Linear;
  if (s == "circular")
    return PatternKind::Circular;
  throw std::runtime_error("Unknown pattern kind: " + s);
}

const char* DatumPlaneKindToString(DatumPlaneKind kind) {
  switch (kind) {
  case DatumPlaneKind::Offset:
    return "offset";
  case DatumPlaneKind::Angled:
    return "angled";
  case DatumPlaneKind::ThroughPoints:
    return "through_points";
  }
  return "offset";
}

DatumPlaneKind DatumPlaneKindFromString(const std::string& s) {
  if (s == "offset")
    return DatumPlaneKind::Offset;
  if (s == "angled")
    return DatumPlaneKind::Angled;
  if (s == "through_points")
    return DatumPlaneKind::ThroughPoints;
  throw std::runtime_error("Unknown datum plane kind: " + s);
}

const char* FrameKindToString(FrameKind kind) {
  switch (kind) {
  case FrameKind::Explicit:
    return "explicit";
  case FrameKind::FromElement:
    return "from_element";
  case FrameKind::Offset:
    return "offset";
  }
  return "explicit";
}

FrameKind FrameKindFromString(const std::string& s) {
  if (s == "explicit")
    return FrameKind::Explicit;
  if (s == "from_element")
    return FrameKind::FromElement;
  if (s == "offset")
    return FrameKind::Offset;
  throw std::runtime_error("unknown Frame kind '" + s +
                           "' (expected one of: explicit, from_element, "
                           "offset)");
}

const char* DatumAxisKindToString(DatumAxisKind kind) {
  switch (kind) {
  case DatumAxisKind::TwoPoints:
    return "two_points";
  case DatumAxisKind::SketchLine:
    return "sketch_line";
  case DatumAxisKind::GlobalX:
    return "global_x";
  case DatumAxisKind::GlobalY:
    return "global_y";
  case DatumAxisKind::GlobalZ:
    return "global_z";
  }
  return "two_points";
}

DatumAxisKind DatumAxisKindFromString(const std::string& s) {
  if (s == "two_points")
    return DatumAxisKind::TwoPoints;
  if (s == "sketch_line")
    return DatumAxisKind::SketchLine;
  if (s == "global_x")
    return DatumAxisKind::GlobalX;
  if (s == "global_y")
    return DatumAxisKind::GlobalY;
  if (s == "global_z")
    return DatumAxisKind::GlobalZ;
  throw std::runtime_error("Unknown datum axis kind: " + s);
}

const char* Curve3DKindToString(Curve3DKind kind) {
  switch (kind) {
  case Curve3DKind::FitSpline:
    return "fit_spline";
  case Curve3DKind::Helix:
    return "helix";
  case Curve3DKind::ControlPoly:
    return "control_poly";
  }
  return "fit_spline";
}

Curve3DKind Curve3DKindFromString(const std::string& s) {
  if (s == "fit_spline")
    return Curve3DKind::FitSpline;
  if (s == "helix")
    return Curve3DKind::Helix;
  if (s == "control_poly")
    return Curve3DKind::ControlPoly;
  throw std::runtime_error("Unknown curve3d kind: " + s);
}

const char* PlaneTypeToString(PlaneType type) {
  switch (type) {
  case PlaneType::XY:
    return "xy";
  case PlaneType::XZ:
    return "xz";
  case PlaneType::YZ:
    return "yz";
  }
  return "xy";
}

PlaneType PlaneTypeFromString(const std::string& s) {
  if (s == "xy")
    return PlaneType::XY;
  if (s == "xz")
    return PlaneType::XZ;
  if (s == "yz")
    return PlaneType::YZ;
  throw std::runtime_error("Unknown base plane: " + s);
}

json AxisRefToJson(const AxisRef& axis) {
  switch (axis.kind) {
  case AxisRef::Kind::X:
    return {{"kind", "x"}};
  case AxisRef::Kind::Y:
    return {{"kind", "y"}};
  case AxisRef::Kind::Z:
    return {{"kind", "z"}};
  case AxisRef::Kind::SketchLine:
    return {{"kind", "sketch_line"},
            {"sketch_id", axis.sketchId},
            {"entity_id", axis.entityId}};
  case AxisRef::Kind::DatumAxis:
    return {{"kind", "datum_axis"}, {"datum_axis_id", axis.datumAxisId}};
  }
  return {{"kind", "x"}};
}

AxisRef AxisRefFromJson(const json& j) {
  AxisRef axis;
  const std::string kind = j.at("kind").get<std::string>();
  if (kind == "x")
    axis.kind = AxisRef::Kind::X;
  else if (kind == "y")
    axis.kind = AxisRef::Kind::Y;
  else if (kind == "z")
    axis.kind = AxisRef::Kind::Z;
  else if (kind == "sketch_line") {
    axis.kind = AxisRef::Kind::SketchLine;
    axis.sketchId = j.at("sketch_id").get<uint64_t>();
    axis.entityId = j.at("entity_id").get<uint32_t>();
  } else if (kind == "datum_axis") {
    axis.kind = AxisRef::Kind::DatumAxis;
    axis.datumAxisId = j.at("datum_axis_id").get<uint64_t>();
  } else {
    throw std::runtime_error("Unknown axis kind: " + kind);
  }
  return axis;
}

} // namespace

namespace DocumentIO {

json ElementMapToJson(const ElementMap& map) {
  json faces = json::object();
  for (const auto& [index, name] : map.names)
    faces[std::to_string(index)] = name;
  json edges = json::object();
  for (const auto& [index, name] : map.edgeNames)
    edges[std::to_string(index)] = name;
  return json{{"faces", std::move(faces)}, {"edges", std::move(edges)}};
}

ElementMap ElementMapFromJson(const json& j) {
  ElementMap map;
  const json faces = j.value("faces", json::object());
  const json edges = j.value("edges", json::object());
  for (const auto& [key, value] : faces.items())
    map.names[std::stoi(key)] = value.get<std::string>();
  for (const auto& [key, value] : edges.items())
    map.edgeNames[std::stoi(key)] = value.get<std::string>();
  return map;
}

json FeatureToJson(const Feature& feature) {
  json j;
  j["type"] = feature.TypeName();
  j["name"] = feature.GetName();
  if (feature.GetId() != 0) {
    j["id"] = feature.GetId();
  }
  if (!feature.GetMaterial().empty()) { // omit default to keep JSON compact
    j["material"] = feature.GetMaterial();
  }
  if (feature.GetAppearance().Any()) { // v54; omit when pure material
    json a = json::object();
    const Appearance& ap = feature.GetAppearance();
    if (ap.color)
      a["color"] = {ap.color->x, ap.color->y, ap.color->z};
    if (ap.roughness)
      a["roughness"] = *ap.roughness;
    if (ap.metallic)
      a["metallic"] = *ap.metallic;
    j["appearance"] = std::move(a);
  }
  if (feature.IsSuppressed()) { // v19; omit when active
    j["suppressed"] = true;
  }

  if (auto* sk = dynamic_cast<const SketchFeature*>(&feature)) {
    j["plane"] = {{"origin", Vec3ToJson(sk->plane.GetOrigin())},
                  {"normal", Vec3ToJson(sk->plane.GetNormal())},
                  {"uAxis", Vec3ToJson(sk->plane.GetUAxis())}};
    if (sk->planeRef != 0) { // v12: tracked datum plane (omit when baked)
      j["plane_ref"] = sk->planeRef;
    }
    if (sk->faceRef.IsSet()) { // v16: attached body face (toponaming)
      j["face_ref"] = {{"feature_id", sk->faceRef.featureId},
                       {"element", sk->faceRef.name}};
    }
    j["entities"] = json::array();
    for (const auto& entity : sk->entities) {
      json ej = EntityToJson(entity);
      // v38: expression-driven coordinate fields ride on the entity as
      // "<field>_expr" keys (omitted when absent, like constraint "expr").
      const auto it = sk->entityExprs.find(sketch::EntityId(entity));
      if (it != sk->entityExprs.end()) {
        for (const auto& [field, expr] : it->second) {
          if (!expr.empty())
            ej[field + "_expr"] = expr;
        }
      }
      j["entities"].push_back(std::move(ej));
    }
    j["constraints"] = json::array();
    for (const auto& c : sk->constraints) {
      j["constraints"].push_back(ConstraintToJson(c));
    }
    if (!sk->derived.empty()) { // omit when empty to keep JSON compact
      j["derived_ops"] = json::array();
      for (const auto& op : sk->derived)
        j["derived_ops"].push_back(DerivedOpToJson(op));
    }
    if (!sk->external.empty()) { // v48: live projected geometry
      j["external"] = json::array();
      for (const auto& e : sk->external) {
        json ej = {
            {"feature_id", e.ref.featureId},
            {"element", e.ref.name},
            {"kind", e.kind == sketch::ExternalKind::Edge ? "edge" : "face"}};
        ej["gen_ids"] = e.genIds;
        j["external"].push_back(std::move(ej));
      }
    }
    if (!sk->images
             .empty()) { // v49: reference underlays (bytes stored out-of-band)
      j["images"] = json::array();
      for (const auto& img : sk->images) {
        json ij = {{"id", img.id},
                   {"asset", img.asset},
                   {"origin", Vec2ToJson(img.origin)},
                   {"rotation", img.rotation},
                   {"units_per_pixel", img.unitsPerPixel},
                   {"opacity", img.opacity},
                   {"width_px", img.widthPx},
                   {"height_px", img.heightPx}};
        if (img.hidden)
          ij["hidden"] = true; // omit when visible to keep JSON compact
        j["images"].push_back(std::move(ij));
      }
      j["next_image_id"] = sk->nextImageId;
    }
  } else if (auto* ext = dynamic_cast<const ExtrudeFeature*>(&feature)) {
    j["sketch_id"] = ext->sketchId;
    j["distance"] = ext->distance;
    if (!ext->distanceExpr.empty()) // literals stay plain numbers (v11)
      j["distance_expr"] = ext->distanceExpr;
    j["reversed"] = ext->reversed;
    j["mode"] = ExtrudeModeToString(ext->mode);
    if (ext->endMode != ExtrudeEnd::Blind) { // v18; omit the default
      j["end_mode"] = ext->endMode == ExtrudeEnd::Symmetric  ? "symmetric"
                      : ext->endMode == ExtrudeEnd::TwoSided ? "two_sided"
                      : ext->endMode == ExtrudeEnd::ThroughAll
                          ? "through_all" // v52
                          : "up_to_face";
      if (ext->endMode == ExtrudeEnd::TwoSided) {
        j["distance2"] = ext->distance2;
        if (!ext->distance2Expr.empty())
          j["distance2_expr"] = ext->distance2Expr;
      }
      if (ext->endMode == ExtrudeEnd::UpToFace) {
        j["end_target"] = {{"feature_id", ext->endTarget.featureId},
                           {"element", ext->endTarget.name}};
      }
    }
    if (!ext->regions.empty()) // v34; omit = whole sketch
      j["regions"] = ext->regions;
    if (ext->thin) { // v18; omit when off
      json thin = {{"thickness", ext->thinThickness},
                   {"side", ext->thinSide == sketch::ThinSide::Out  ? "out"
                            : ext->thinSide == sketch::ThinSide::In ? "in"
                                                                    : "both"}};
      if (!ext->thinThicknessExpr.empty())
        thin["thickness_expr"] = ext->thinThicknessExpr;
      j["thin"] = thin;
    }
  } else if (auto* box = dynamic_cast<const BoxFeature*>(&feature)) {
    j["position"] = Vec3ToJson(box->position);
    j["width"] = box->width;
    j["height"] = box->height;
    j["depth"] = box->depth;
    // v61: the primitives became expression-driven. "<field>_expr" is the
    // format's standing convention and is omitted when the field is a plain
    // literal, so a file that uses none of them is byte-identical to v60.
    if (!box->widthExpr.empty())
      j["width_expr"] = box->widthExpr;
    if (!box->heightExpr.empty())
      j["height_expr"] = box->heightExpr;
    if (!box->depthExpr.empty())
      j["depth_expr"] = box->depthExpr;
  } else if (auto* sphere = dynamic_cast<const SphereFeature*>(&feature)) {
    j["center"] = Vec3ToJson(sphere->center);
    j["radius"] = sphere->radius;
    if (!sphere->radiusExpr.empty())
      j["radius_expr"] = sphere->radiusExpr;
  } else if (auto* cyl = dynamic_cast<const CylinderFeature*>(&feature)) {
    j["center"] = Vec3ToJson(cyl->center);
    j["direction"] = Vec3ToJson(cyl->direction);
    j["radius"] = cyl->radius;
    j["height"] = cyl->height;
    if (!cyl->radiusExpr.empty())
      j["radius_expr"] = cyl->radiusExpr;
    if (!cyl->heightExpr.empty())
      j["height_expr"] = cyl->heightExpr;
  } else if (auto* fastener = dynamic_cast<const FastenerFeature*>(&feature)) {
    j["standard"] = fastener->standard;
    j["size"] = fastener->size;
    if (fastener->length > 0.0)
      j["length"] = fastener->length; // a nut and a washer have none
    if (!fastener->lengthExpr.empty())
      j["length_expr"] = fastener->lengthExpr;
    // Omitted when empty, which means "the drive this standard is made with"
    // — the default, and the only value that stays right if the standard's
    // recess is ever corrected.
    if (!fastener->drive.empty())
      j["drive"] = fastener->drive;
  } else if (auto* import = dynamic_cast<const ImportFeature*>(&feature)) {
    j["path"] = import->path;
    if (import->keepOrigin) // v97; absent = centred, as every older file was
      j["keep_origin"] = true;
    if (import->lengthUnit != units::Length::Millimetre) // v103; absent = mm
      j["units"] = units::ToString(import->lengthUnit);
    if (import->hasColor) // only when set: pre-I2 files stay byte-identical
      j["color"] = Vec3ToJson(import->color);
  } else if (auto* d = dynamic_cast<const DerivedFeature*>(&feature)) {
    // v84 (docs/modeling/derived-plan.md). The snapshot rides here in full
    // but for the B-Reps, which are document assets: the chain hash is a
    // hash of this JSON, so an update (new asset names, new element maps,
    // new source hash) invalidates everything after the feature.
    j["source_part"] = d->sourcePart;
    j["source_name"] = d->sourceName;
    if (!d->bodies.empty())
      j["bodies"] = d->bodies;
    if (!d->bodyNames.empty())
      j["body_names"] = d->bodyNames; // v85
    j["placement"] = {{"position", Vec3ToJson(d->position)},
                      {"orientation", QuatToJson(d->orientation)}};
    j["reference"] = d->reference;
    json bodies = json::array();
    for (const DerivedBody& b : d->snapshot) {
      json bj{{"asset", b.asset},
              {"name", b.name},
              {"material", b.material},
              {"source_created_by", b.sourceCreatedBy},
              {"element_map", ElementMapToJson(b.elementMap)}};
      if (b.hasDisplayColor)
        bj["display_color"] = Vec3ToJson(b.displayColor);
      bodies.push_back(std::move(bj));
    }
    j["snapshot"] = {{"source_chain_hash", d->sourceChainHash},
                     {"bodies", std::move(bodies)}};
  } else if (auto* dp = dynamic_cast<const DatumPlaneFeature*>(&feature)) {
    j["kind"] = DatumPlaneKindToString(dp->kind);
    // Parent (Offset/Angled): earlier datum by id, or a base plane.
    if (dp->kind != DatumPlaneKind::ThroughPoints) {
      if (dp->parentId != 0)
        j["parent_id"] = dp->parentId;
      else
        j["base_plane"] = PlaneTypeToString(dp->basePlane);
    }
    // Emit only the active variant's fields.
    switch (dp->kind) {
    case DatumPlaneKind::Offset:
      j["offset"] = dp->offset;
      if (!dp->offsetExpr.empty())
        j["offset_expr"] = dp->offsetExpr;
      if (dp->flip)
        j["flip"] = true;
      break;
    case DatumPlaneKind::Angled:
      j["axis"] = AxisRefToJson(dp->axis);
      j["angle_deg"] = dp->angleDeg;
      if (!dp->angleExpr.empty())
        j["angle_expr"] = dp->angleExpr;
      break;
    case DatumPlaneKind::ThroughPoints:
      j["points"] = {Vec3ToJson(dp->p0), Vec3ToJson(dp->p1),
                     Vec3ToJson(dp->p2)};
      break;
    }
  } else if (auto* rev = dynamic_cast<const RevolveFeature*>(&feature)) {
    j["sketch_id"] = rev->sketchId;
    j["axis"] = AxisRefToJson(rev->axis);
    j["angle_deg"] = rev->angleDeg;
    if (!rev->angleExpr.empty()) // literals stay plain numbers (v11)
      j["angle_expr"] = rev->angleExpr;
    if (rev->symmetric)
      j["symmetric"] = true;
    j["reversed"] = rev->reversed;
    j["mode"] = ExtrudeModeToString(rev->mode);
    if (!rev->regions.empty()) // v34; omit = whole sketch
      j["regions"] = rev->regions;
  } else if (auto* da = dynamic_cast<const DatumAxisFeature*>(&feature)) {
    j["kind"] = DatumAxisKindToString(da->kind);
    switch (da->kind) {
    case DatumAxisKind::TwoPoints:
      j["points"] = {Vec3ToJson(da->p0), Vec3ToJson(da->p1)};
      break;
    case DatumAxisKind::SketchLine:
      j["sketch_id"] = da->sketchId;
      j["entity_id"] = da->entityId;
      break;
    case DatumAxisKind::GlobalX:
    case DatumAxisKind::GlobalY:
    case DatumAxisKind::GlobalZ:
      break; // kind string is sufficient
    }
  } else if (auto* fr = dynamic_cast<const FrameFeature*>(&feature)) { // v86
    j["kind"] = FrameKindToString(fr->kind);
    switch (fr->kind) {
    case FrameKind::Explicit: {
      j["origin"] = {fr->origin[0], fr->origin[1], fr->origin[2]};
      if (!fr->originExpr[0].empty() || !fr->originExpr[1].empty() ||
          !fr->originExpr[2].empty())
        j["origin_expr"] = {fr->originExpr[0], fr->originExpr[1],
                            fr->originExpr[2]};
      j["z_axis"] = Vec3ToJson(fr->zAxis);
      j["x_axis"] = Vec3ToJson(fr->xAxis);
      break;
    }
    case FrameKind::FromElement: {
      json els = json::array();
      for (const ElementRef& e : fr->elements)
        els.push_back(ElementRefJson(e));
      j["elements"] = std::move(els);
      if (fr->flip)
        j["flip"] = true;
      break;
    }
    case FrameKind::Offset: {
      j["parent_id"] = fr->parentId;
      j["offset"] = {fr->offset[0], fr->offset[1], fr->offset[2]};
      if (!fr->offsetExpr[0].empty() || !fr->offsetExpr[1].empty() ||
          !fr->offsetExpr[2].empty())
        j["offset_expr"] = {fr->offsetExpr[0], fr->offsetExpr[1],
                            fr->offsetExpr[2]};
      j["rotation_deg"] = {fr->rotationDeg[0], fr->rotationDeg[1],
                           fr->rotationDeg[2]};
      if (!fr->rotationExpr[0].empty() || !fr->rotationExpr[1].empty() ||
          !fr->rotationExpr[2].empty())
        j["rotation_expr"] = {fr->rotationExpr[0], fr->rotationExpr[1],
                              fr->rotationExpr[2]};
      break;
    }
    }
  } else if (auto* cv = dynamic_cast<const CurveFeature*>(&feature)) { // v42
    j["kind"] = Curve3DKindToString(cv->kind);
    switch (cv->kind) {
    case Curve3DKind::FitSpline:
    case Curve3DKind::ControlPoly: {
      json pts = json::array();
      for (const glm::vec3& p : cv->points)
        pts.push_back(Vec3ToJson(p));
      j["points"] = std::move(pts);
      if (cv->kind == Curve3DKind::ControlPoly)
        j["degree"] = cv->degree;
      if (cv->closed)
        j["closed"] = true;
      break;
    }
    case Curve3DKind::Helix:
      j["origin"] = Vec3ToJson(cv->axisOrigin);
      j["axis"] = Vec3ToJson(cv->axisDir);
      j["radius"] = cv->radius;
      if (!cv->radiusExpr.empty())
        j["radius_expr"] = cv->radiusExpr;
      j["pitch"] = cv->pitch;
      if (!cv->pitchExpr.empty())
        j["pitch_expr"] = cv->pitchExpr;
      j["turns"] = cv->turns;
      if (!cv->turnsExpr.empty())
        j["turns_expr"] = cv->turnsExpr;
      if (!cv->rightHanded)
        j["left_handed"] = true;
      break;
    }
  } else if (auto* boo = dynamic_cast<const BooleanFeature*>(&feature)) {
    j["target_id"] = boo->targetId;
    j["tool_id"] = boo->toolId;
    j["op"] = BooleanOpToString(boo->op);
  } else if (auto* sp = dynamic_cast<const SplitFeature*>(&feature)) {
    j["body_id"] = sp->bodyId;
    if (sp->knife == SplitKnifeKind::Face) {
      j["knife"] = "face";
      j["face"] = {{"feature_id", sp->face.featureId},
                   {"element", sp->face.name}};
    } else {
      j["knife"] = "plane";
      if (sp->datumId != 0)
        j["datum_id"] = sp->datumId;
      else
        j["base_plane"] = PlaneTypeToString(sp->basePlane);
      if (sp->offset != 0.0)
        j["offset"] = sp->offset;
      if (!sp->offsetExpr.empty())
        j["offset_expr"] = sp->offsetExpr;
    }
  } else if (auto* rf = dynamic_cast<const RefineFeature*>(&feature)) {
    j["body_id"] = rf->bodyId;
  } else if (auto* fil = dynamic_cast<const FilletFeature*>(&feature)) {
    // type is "Fillet"/"Chamfer" (TypeName encodes the op); v17.
    j["radius"] = fil->radius;
    if (!fil->radiusExpr.empty()) // literals stay plain numbers
      j["radius_expr"] = fil->radiusExpr;
    json arr = json::array();
    for (const auto& ref : fil->edges) {
      arr.push_back({{"feature_id", ref.featureId}, {"element", ref.name}});
    }
    j["edges"] = std::move(arr);
  } else if (auto* bf = dynamic_cast<const BaseFlangeFeature*>(&feature)) {
    j["sketch_id"] = bf->sketchId;
    if (bf->reversed)
      j["reversed"] = true;
    if (!bf->regions.empty())
      j["regions"] = bf->regions;
  } else if (auto* ef = dynamic_cast<const EdgeFlangeFeature*>(&feature)) {
    json arr = json::array();
    for (const auto& ref : ef->edges)
      arr.push_back({{"feature_id", ref.featureId}, {"element", ref.name}});
    j["edges"] = std::move(arr);
    j["length"] = ef->length;
    if (!ef->lengthExpr.empty())
      j["length_expr"] = ef->lengthExpr;
    j["angle_deg"] = ef->angleDeg;
    if (!ef->angleDegExpr.empty())
      j["angle_deg_expr"] = ef->angleDegExpr;
    if (ef->radius > 0.0)
      j["radius"] = ef->radius;
    if (!ef->radiusExpr.empty())
      j["radius_expr"] = ef->radiusExpr;
    if (ef->position != FlangePosition::MaterialInside)
      j["position"] = FlangePositionName(ef->position);
    if (ef->lengthMode != FlangeLengthMode::Flat)
      j["length_mode"] = FlangeLengthModeName(ef->lengthMode);
    if (ef->offsetStart != 0.0)
      j["offset_start"] = ef->offsetStart;
    if (!ef->offsetStartExpr.empty())
      j["offset_start_expr"] = ef->offsetStartExpr;
    if (ef->offsetEnd != 0.0)
      j["offset_end"] = ef->offsetEnd;
    if (!ef->offsetEndExpr.empty())
      j["offset_end_expr"] = ef->offsetEndExpr;
    if (ef->reliefStyle)
      j["relief"] = sheet::ReliefStyleName(*ef->reliefStyle);
    if (ef->closedCorners)
      j["corners"] = "closed";
  } else if (auto* fp = dynamic_cast<const FlatPatternFeature*>(&feature)) {
    j["body_id"] = fp->bodyId;
    if (fp->fixedFace.IsSet())
      j["fixed_face"] = {{"feature_id", fp->fixedFace.featureId},
                         {"element", fp->fixedFace.name}};
  } else if (auto* uf = dynamic_cast<const UnfoldFeature*>(&feature)) {
    j["body_id"] = uf->bodyId;
    if (uf->fixedFace.IsSet())
      j["fixed_face"] = {{"feature_id", uf->fixedFace.featureId},
                         {"element", uf->fixedFace.name}};
    if (!uf->bendFeatures.empty())
      j["bends"] = uf->bendFeatures;
  } else if (auto* rf = dynamic_cast<const RefoldFeature*>(&feature)) {
    j["unfold_id"] = rf->unfoldId;
  } else if (auto* hm = dynamic_cast<const HemFeature*>(&feature)) {
    json arr = json::array();
    for (const auto& ref : hm->edges)
      arr.push_back({{"feature_id", ref.featureId}, {"element", ref.name}});
    j["edges"] = std::move(arr);
    j["length"] = hm->length;
    if (!hm->lengthExpr.empty())
      j["length_expr"] = hm->lengthExpr;
    if (hm->kind != HemKind::Closed)
      j["kind"] = HemKindName(hm->kind);
    if (hm->gap > 0.0)
      j["gap"] = hm->gap;
    if (!hm->gapExpr.empty())
      j["gap_expr"] = hm->gapExpr;
    if (hm->outside)
      j["outside"] = true;
  } else if (auto* jg = dynamic_cast<const JogFeature*>(&feature)) {
    j["sketch_id"] = jg->sketchId;
    j["height"] = jg->height;
    if (!jg->heightExpr.empty())
      j["height_expr"] = jg->heightExpr;
    if (jg->angleDeg != 90.0)
      j["angle_deg"] = jg->angleDeg;
    if (!jg->angleDegExpr.empty())
      j["angle_deg_expr"] = jg->angleDegExpr;
    if (jg->flip)
      j["flip"] = true;
  } else if (auto* cr = dynamic_cast<const CornerReliefFeature*>(&feature)) {
    j["body_id"] = cr->bodyId;
    if (cr->style != CornerReliefStyle::Circular)
      j["style"] = CornerReliefStyleName(cr->style);
    if (cr->size > 0.0)
      j["size"] = cr->size;
    if (!cr->sizeExpr.empty())
      j["size_expr"] = cr->sizeExpr;
  } else if (auto* sh = dynamic_cast<const ShellFeature*>(&feature)) {
    j["body_id"] = sh->bodyId;
    j["thickness"] = sh->thickness;
    if (!sh->thicknessExpr.empty())
      j["thickness_expr"] = sh->thicknessExpr;
    json arr = json::array();
    for (const auto& ref : sh->openFaces)
      arr.push_back({{"feature_id", ref.featureId}, {"element", ref.name}});
    j["open_faces"] = std::move(arr);
  } else if (auto* lo = dynamic_cast<const LoftFeature*>(&feature)) {
    j["sketch_ids"] = lo->sketchIds;
    if (lo->pathId != 0) // v45; omit for a classic (guideless) loft
      j["path_id"] = lo->pathId;
    if (lo->guideId != 0)
      j["guide_id"] = lo->guideId;
    j["mode"] = ExtrudeModeToString(lo->mode);
  } else if (auto* sw = dynamic_cast<const SweepFeature*>(&feature)) {
    j["profile_id"] = sw->profileSketchId;
    j["path_id"] = sw->pathSketchId;
    if (sw->guideId != 0) // v44; omit when there's no guide
      j["guide_id"] = sw->guideId;
    j["mode"] = ExtrudeModeToString(sw->mode);
    if (sw->frame != SweepFrame::CorrectedFrenet) // v43; omit the default
      j["frame"] = sw->frame == SweepFrame::Frenet ? "frenet" : "fixed";
  } else if (auto* pi = dynamic_cast<const PipeFeature*>(&feature)) { // v43
    j["path_id"] = pi->pathId;
    j["outer_diameter"] = pi->outerDiameter;
    if (!pi->outerDiameterExpr.empty())
      j["outer_diameter_expr"] = pi->outerDiameterExpr;
    if (pi->wallThickness != 0.0)
      j["wall_thickness"] = pi->wallThickness;
    if (!pi->wallThicknessExpr.empty())
      j["wall_thickness_expr"] = pi->wallThicknessExpr;
    j["mode"] = ExtrudeModeToString(pi->mode);
  } else if (auto* rb = dynamic_cast<const RibFeature*>(&feature)) {
    j["sketch_id"] = rb->sketchId;
    j["thickness"] = rb->thickness;
    if (!rb->thicknessExpr.empty())
      j["thickness_expr"] = rb->thicknessExpr;
    if (rb->flipDirection)
      j["flip"] = true;
    if (rb->bodyId != 0)
      j["body_id"] = rb->bodyId;
  } else if (auto* th = dynamic_cast<const ThreadFeature*>(&feature)) {
    j["face"] = {{"feature_id", th->face.featureId},
                 {"element", th->face.name}};
    j["designation"] = th->designation;
    j["pitch"] = th->pitch;
    j["allow_engagement"] = th->allowEngagement;
    j["minimum_engagement"] = th->minimumEngagement;
    if (!th->threadClass.empty())
      j["thread_class"] = th->threadClass;
    if (th->length > 0.0)
      j["length"] = th->length;
    if (!th->lengthExpr.empty())
      j["length_expr"] = th->lengthExpr;
    if (th->modeled)
      j["modeled"] = true;
  } else if (auto* ho = dynamic_cast<const HoleFeature*>(&feature)) {
    j["sketch_id"] = ho->sketchId;
    j["hole_kind"] = ho->kind == HoleKind::Counterbore   ? "counterbore"
                     : ho->kind == HoleKind::Countersink ? "countersink"
                                                         : "simple";
    // The diameter is written either way — it is what the kernel cut, and a
    // reader that knows nothing about fits still gets a correct hole. When a
    // fit is recorded it is ALSO the thing that derives it on the next
    // regeneration, so the two cannot drift.
    j["diameter"] = ho->diameter;
    if (!ho->diameterExpr.empty())
      j["diameter_expr"] = ho->diameterExpr;
    if (!ho->fitThread.empty()) {
      nlohmann::json fit = {{"thread", ho->fitThread}, {"kind", ho->fitKind}};
      if (ho->fitPitch > 0.0)
        fit["pitch"] = ho->fitPitch; // omitted = the coarse pitch
      j["fit"] = fit;
    }
    j["depth"] = ho->depth;
    if (!ho->depthExpr.empty())
      j["depth_expr"] = ho->depthExpr;
    if (ho->through)
      j["through"] = true;
    if (ho->kind == HoleKind::Counterbore) {
      j["cb_diameter"] = ho->cbDiameter;
      j["cb_depth"] = ho->cbDepth;
    } else if (ho->kind == HoleKind::Countersink) {
      j["cs_diameter"] = ho->csDiameter;
      j["cs_depth"] = ho->csDepth;
    }
  } else if (auto* dr = dynamic_cast<const DraftFeature*>(&feature)) {
    j["body_id"] = dr->bodyId;
    j["angle_deg"] = dr->angleDeg;
    if (!dr->angleExpr.empty())
      j["angle_expr"] = dr->angleExpr;
    j["neutral_plane"] = PlaneTypeToString(dr->neutralPlane);
    json arr = json::array();
    for (const auto& ref : dr->faces)
      arr.push_back({{"feature_id", ref.featureId}, {"element", ref.name}});
    j["faces"] = std::move(arr);
  } else if (auto* pat = dynamic_cast<const PatternFeature*>(&feature)) {
    j["kind"] = PatternKindToString(pat->kind);
    j["source_id"] = pat->sourceId;
    if (pat->kind == PatternKind::Mirror) {
      if (pat->planeId != 0)
        j["parent_id"] = pat->planeId;
      else
        j["base_plane"] = PlaneTypeToString(pat->basePlane);
    } else {
      j["axis"] = AxisRefToJson(pat->axis);
      j["count"] = pat->count;
      if (!pat->countExpr.empty())
        j["count_expr"] = pat->countExpr;
      if (pat->kind == PatternKind::Linear) {
        j["spacing"] = pat->spacing;
        if (!pat->spacingExpr.empty())
          j["spacing_expr"] = pat->spacingExpr;
      } else { // Circular
        j["angle_deg"] = pat->angleDeg;
        if (!pat->angleExpr.empty())
          j["angle_expr"] = pat->angleExpr;
      }
    }
  } else {
    throw std::runtime_error("Unserializable feature type: " +
                             feature.TypeName());
  }
  return j;
}

int DocumentFormatVersion() { return kVersion; }

// Keep in lockstep with the FeatureFromJson chain below (one entry per
// `type ==` branch; Fillet/Chamfer share a branch but are distinct types).
//
// Fields EVERY type accepts, so no row repeats them: `name?`, `id?`,
// `material?` (a slug from describe.material_slugs — an unrecognised one is
// REFUSED naming the accepted set, so it cannot arrive here through
// add_feature or modify_feature the way it used to), `appearance?`.
static constexpr FeatureTypeInfo kFeatureTypes[] = {
    {"Sketch", 1,
     "plane {origin, normal, uAxis} (in-plane V = normal x uAxis!), entities "
     "[{type: line|circle|arc|ellipse|spline|point|rect, ...}], name?, "
     "face_ref? {feature_id, element} (sketch ON a body face), external? "
     "[{feature_id, element, kind: face|edge}] — live projected construction "
     "geometry of another body, the same thing project_geometry adds to an "
     "existing sketch (a projected hole edge is a circle, and Hole takes "
     "circle centres, so a hole pattern needs no coordinates)"},
    {"Extrude", 1,
     "sketch_id, distance (+_expr), mode: new_body|add|cut, end_mode?: "
     "blind|symmetric|two_sided|up_to_face|through_all, distance2? (+_expr), "
     "reversed?, regions?, end_target?; extrudes along +plane normal"},
    {"Box", 1,
     "position [x,y,z] = MIN corner, width/height/depth = X/Y/Z extents mm "
     "(each +_expr, so a variable can drive it)"},
    {"Sphere", 1, "center [x,y,z], radius (+_expr)"},
    {"Cylinder", 1,
     "center [x,y,z] (base), direction [x,y,z], radius (+_expr), "
     "height (+_expr)"},
    {"Fastener", 69,
     "standard: iso4762|iso14579|iso4017|iso10642|iso7380 (screws), iso4032 "
     "(nut), iso7089 (washer); size (a key in that table, e.g. M6); length "
     "(+_expr) mm — under the head, or OVERALL for a countersunk one, and "
     "omitted for a nut or a washer; drive?: leave empty for the one the "
     "standard is made with. Every other dimension is READ from the table — "
     "an unstocked size is refused naming the nearest that are stocked "
     "(docs/parts/plan.md)"},
    {"BaseFlange", 109,
     "sketch_id, reversed?, regions? - the FIRST wall of a sheet part: the "
     "closed profile thickened by the document's sheet-metal thickness, so "
     "it takes no thickness of its own (set_sheet_metal_rules first, or it "
     "REFUSES - a blank is cut from one sheet). The profile is the inside "
     "face; `reversed` grows the material the other way"},
    {"EdgeFlange", 109,
     "edges [{feature_id, element}] (STRAIGHT edges of a flat panel of one "
     "sheet body), length (+_expr) mm, angle_deg (+_expr, default 90 - the "
     "bend's included sweep), radius? (+_expr) mm (omitted = the document's "
     "bend radius), position?: material_inside (default) | material_outside "
     "| bend_outside - which mold line the picked edge marks, length_mode?: "
     "flat (default, the wall past the bend) | outside | inside - which "
     "dimension `length` states, offset_start?/offset_end? (+_expr) mm - "
     "insets that stop the bend line INSIDE the panel and so call for "
     "relief, relief?: none|rectangular|obround|tear (omitted = the "
     "document's), corners?: open (default) | closed - carry two flanges "
     "that meet at a square corner past it and mitre them shut. One bend "
     "per edge, each recorded on the body; edges meeting at a corner whose "
     "flanges would overlap are always mitred"},
    {"FlatPattern", 110,
     "body_id (the sheet body's created_by), fixed_face? {feature_id, "
     "element} - the BLANK: the body laid flat, replacing its shape in "
     "place rather than adding a second body (a folded part and its blank "
     "occupy the same space). Author it SUPPRESSED and put it in a "
     "configuration's `unsuppressed_features` to switch between folded and "
     "flat. Must sit after every feature that reshapes the sheet, since it "
     "flattens from the bends those features recorded"},
    {"Unfold", 111,
     "body_id (the sheet body's created_by), fixed_face? {feature_id, "
     "element}, bends? [the ids of the flange features whose bends to lay "
     "flat; omitted = all] - lays those bends flat IN PLACE so the features "
     "after it are drawn and cut in the flat; a Refold folds them back, "
     "carrying every through-thickness cut across each crease onto the bend"},
    {"Refold", 111,
     "unfold_id - folds back up every bend that Unfold laid flat. A cut made "
     "between the two that crosses a crease is WRAPPED onto the bend; one "
     "that does not go right through the sheet is refused, since the "
     "neutral-axis map cannot carry it"},
    {"Hem", 112,
     "edges [{feature_id, element}] (straight panel edges, as EdgeFlange), "
     "length (+_expr) mm - the return past the fold, kind?: closed "
     "(default; the return lies a tenth of the sheet off the panel) | open, "
     "gap? (+_expr) mm (open only; omitted = one thickness), outside? - "
     "fold starting AT the edge instead of keeping the drawn outline. A hem "
     "is crushed, so it is exempt from the minimum bend radius"},
    {"Jog", 112,
     "sketch_id (a sketch holding ONE line drawn on a panel face), height "
     "(+_expr) mm - face to face, angle_deg? (+_expr, (0, 90], default 90), "
     "flip? - which side of the line is carried up. Two opposing bends the "
     "full width of the panel; a height under what the two bends alone climb, "
     "(2R + t)(1 - cos angle), is refused"},
    {"CornerRelief", 113,
     "body_id, style?: circular (default) | rectangular, size? (+_expr) mm - "
     "radius or half-side, omitted = bend radius + thickness. Cuts a relief "
     "at EVERY corner where two bends of the body cross; three bends at one "
     "corner, or two at other than a right angle, are refused"},
    {"Import", 1,
     "path (STEP/STL/BREP file); units?: the length unit an STL or BREP was "
     "drawn in — mm (default), cm, m, in — since neither format carries one "
     "(a metre mesh read as mm is 1000x too small); refused on STEP, which "
     "declares its own; keep_origin?: the import is CENTRED on its "
     "combined bounding box by default — pass true to keep the file's own "
     "coordinates, which is what a neighbour part, a fixture in a shared "
     "frame, or a re-import of your own export needs. The applied shift is "
     "reported as `recentred_by`"},
    {"Derived", 84,
     "source_part (a part slot id), bodies?: [created_by ids] and/or "
     "body_names?: [names] (both absent = all of the source's bodies; the "
     "names are what get_state shows, and are the only way to pick from the "
     "hundreds one Import creates), placement?: {position [x,y,z], orientation "
     "[x,y,z,w]}, reference?: true (a ghost to draw on — absent from mass, "
     "BOM, export, CAM and booleans) | false (an ordinary body of this "
     "part). A COPY of another part's bodies, taken when the feature is "
     "added and on update_derived; regeneration reads the snapshot, never "
     "the source, and get_state reports `stale` when the source has moved "
     "(docs/modeling/derived-plan.md)"},
    {"DatumPlane", 1,
     "kind: offset|angle, base_plane: xy|xz|yz or parent_id, offset (+_expr), "
     "flip?, axis?, angle_deg? (+_expr)"},
    {"Revolve", 1,
     "sketch_id, axis {kind: x|y|z|sketch_line|datum_axis, ...}, angle_deg "
     "(+_expr), mode: new_body|add|cut, symmetric?, reversed?. Positive "
     "angles sweep RIGHT-HANDED about the axis direction starting from the "
     "sketch position (about +x, a point at +z rotates toward -y) — confirm "
     "partial-sweep orientation from the digest bbox, not a render tile"},
    {"DatumAxis", 1, "kind, points?/sketch_id?/entity_id?"},
    {"Frame", 86,
     "kind: explicit|from_element|offset — a NAMED datum frame (origin + "
     "axes) that mates anchor to by name ({occurrence, frame: <id|name>}) "
     "and `frame` reports. explicit: origin [x,y,z] (+origin_expr "
     "[sx,sy,sz]), z_axis, x_axis. from_element: elements [{feature_id, "
     "element}] — ONE planar face (centroid + normal), cylindrical face or "
     "circular edge (centre + axis), or SEVERAL circular ones = a bolt "
     "circle (centre of the pattern, X toward the first hole); flip? "
     "negates Z. offset: parent_id (a Frame; 0 = the part origin), offset "
     "[x,y,z] (+offset_expr), rotation_deg [rx,ry,rz] (+rotation_expr), "
     "applied as parent · T · Rx · Ry · Rz"},
    {"Curve3D", 42,
     "kind: fit_spline|control_poly|helix; points [[x,y,z]...], degree?, "
     "closed?; helix: origin, axis, radius (+_expr), pitch (+_expr), turns "
     "(+_expr), left_handed?"},
    {"Boolean", 1,
     "target_id, tool_id (body created_by ids), op: "
     "union|subtract|intersect"},
    {"Split", 39,
     "body_id, knife: {face {feature_id, element}} or "
     "{datum_id} or {base_plane, offset (+_expr)}"},
    {"Refine", 47, "body_id (heals/simplifies the B-Rep)"},
    {"Fillet", 17, "edges [{feature_id, element}], radius (+_expr)"},
    {"Chamfer", 17,
     "edges [{feature_id, element}], radius (+_expr) = "
     "setback distance"},
    {"Shell", 20, "open_faces [{feature_id, element}], thickness (+_expr)"},
    {"Loft", 20, "sketch_ids [id...] (section order), mode, guide_id?"},
    {"Sweep", 20,
     "profile_id (sketch), path_id (sketch or curve), mode, "
     "guide_id?, frame?"},
    {"Pipe", 43,
     "path_id (curve), outer_diameter (+_expr), wall_thickness "
     "(+_expr), mode"},
    {"Rib", 36, "sketch_id (open profile), thickness (+_expr), flip?"},
    {"Thread", 36,
     "face {feature_id, element} (cylindrical), designation (e.g. \"M8\"), "
     "pitch?, thread_class?, length (+_expr), modeled?, allow_engagement?, "
     "minimum_engagement?"},
    {"Hole", 21,
     "hole_kind: simple|counterbore|countersink, sketch_id (centres: its "
     "points and its circles' centres, so a projected hole edge places a hole; "
     "drills along -plane normal), diameter (+_expr), depth (+_expr), "
     "through?, cb_diameter?, cb_depth?, cs_diameter?, cs_depth?; "
     "fit?: {thread: \"M6\", kind: close|normal|loose|tapped, pitch?} — what "
     "the hole is FOR, which DERIVES the diameter (6.6 for an M6 normal "
     "clearance, 5.0 tapped) and is what a drawing callout and a fastener "
     "check read (docs/modeling/hole-standards.md)"},
    {"Draft", 22,
     "faces [{feature_id, element}], angle_deg (+_expr), "
     "neutral_plane, body_id?"},
    {"Pattern", 1,
     "kind: linear|circular, source_id (feature to repeat), count (+_expr), "
     "linear: base_plane/axis + spacing (+_expr); circular: axis + angle_deg "
     "(+_expr)"},
};
std::span<const FeatureTypeInfo> FeatureTypes() { return kFeatureTypes; }

// AUTHORITATIVE, like kFeatureTypes: EntityFromJson refuses a type with no
// row here before it reaches the dispatch chain, so an entity cannot arrive
// undocumented. It used to be a bare name list, which meant `describe` named
// every type and described none of their fields.
//
// Angles say their unit. `arc` and `ellipse` take either spelling and refuse
// both at once, the same bargain `set_mate` struck with rotation/rotation_deg
// after 60-read-as-radians was diagnosed as a loop-solver branch bug twice.
static constexpr SketchEntityInfo kSketchEntityTypes[] = {
    {"point", 1, "p [x,y] (+_expr: x, y)"},
    {"line", 1, "a [x,y], b [x,y] (+_expr: ax, ay, bx, by)"},
    {"circle", 1, "center [x,y], radius (+_expr: cx, cy, r)"},
    {"arc", 1,
     "center [x,y], radius, and EITHER start_angle + end_angle in RADIANS OR "
     "start_angle_deg + end_angle_deg in degrees — not both. Measured from "
     "+U toward +V. A sweep of more than one full turn is refused (it is a "
     "circle, not an arc), which is what catches degrees passed as radians. "
     "get_state reports the radian pair (+_expr: cx, cy, r)"},
    {"ellipse", 9,
     "center [x,y], r_major, r_minor, and EITHER rotation in RADIANS OR "
     "rotation_deg in degrees — not both"},
    {"ellipse_arc", 46,
     "center [x,y], r_major, r_minor, rotation (RADIANS) or rotation_deg; "
     "start_param, end_param — the ellipse PARAMETER t in (r_major·cos t, "
     "r_minor·sin t), which is not the angle from the centre unless the "
     "ellipse is a circle, so it has no _deg twin"},
    {"spline", 40, "poles [[x,y], ...], degree? (default 3)"},
};
std::span<const SketchEntityInfo> SketchEntityTypes() {
  return kSketchEntityTypes;
}

std::unique_ptr<Feature> FeatureFromJson(const json& j) {
  const std::string type = j.at("type").get<std::string>();
  const std::string name = j.at("name").get<std::string>();

  // kFeatureTypes is AUTHORITATIVE, not documentation that happens to sit
  // near the dispatch chain. The old arrangement was a comment asking two
  // lists to be kept in lockstep with nothing enforcing it, and a type that
  // gained a `type ==` branch but no table row would load and save perfectly
  // while being invisible to `describe` — the same shape as the CAM and
  // drawings gate lists that shipped with RPCs nobody could discover.
  //
  // Checking the table BEFORE the chain makes both directions of drift a
  // loud failure instead of a silent one: a branch without a row is
  // unreachable (its golden test fails on the next run), and a row without a
  // branch still falls through to the same error below.
  {
    bool known = false;
    for (const FeatureTypeInfo& info : kFeatureTypes) {
      if (type == info.type) {
        known = true;
        break;
      }
    }
    if (!known) {
      std::string accepted;
      for (const FeatureTypeInfo& info : kFeatureTypes)
        accepted += (accepted.empty() ? "" : ", ") + std::string(info.type);
      throw std::runtime_error("Unknown feature type: " + type +
                               ". Known types: " + accepted);
    }
  }

  std::unique_ptr<Feature> out;
  if (type == "Sketch") {
    const json& pl = j.at("plane");
    SketchPlane plane(Vec3FromJson(pl.at("origin")),
                      Vec3FromJson(pl.at("normal")),
                      Vec3FromJson(pl.at("uAxis")));
    auto sk = std::make_unique<SketchFeature>(name, plane);
    // Pre-v5 rigid "rect" entities are expanded into four lines below. During
    // loading they occupy a placeholder slot (preserving order/indexing so the
    // pre-v4 index migration stays correct); their corners are remembered.
    std::vector<std::pair<glm::vec2, glm::vec2>> legacyRects;
    std::vector<size_t> rectSlots; // placeholder positions in sk->entities
    // v38 entity coordinate expressions, collected POSITIONALLY: RPC-sent
    // entities carry no id yet (the pre-v4 migration below stamps them), so
    // the exprs are re-keyed by the final stable id after that migration.
    std::vector<std::pair<size_t, std::map<std::string, std::string>>>
        pendingEntityExprs;
    for (const auto& ej : j.value("entities", json::array())) {
      if (ej.value("type", "") == "rect") {
        glm::vec2 c1 = Vec2FromJson(ej.at("c1"));
        glm::vec2 c2 = Vec2FromJson(ej.at("c2"));
        sketch::Entity ph =
            sketch::Line{c1, c2}; // placeholder, preserves order
        sketch::SetEntityId(ph, ej.value("id", uint32_t{0}));
        rectSlots.push_back(sk->entities.size());
        sk->entities.push_back(ph);
        legacyRects.emplace_back(c1, c2);
      } else {
        sketch::Entity ent = EntityFromJson(ej);
        std::map<std::string, std::string> exprs;
        const std::vector<std::string> fields = sketch::EntityExprFields(ent);
        for (const std::string& field : fields) {
          const std::string expr = ej.value(field + "_expr", std::string{});
          if (!expr.empty())
            exprs[field] = expr;
        }
        // An `_expr` key this entity kind does not have was DROPPED, so a
        // dimension written as a live variable silently became the literal
        // it happened to evaluate to: `generators/gear.py` sent
        // `radius_expr` (the field is `r_expr`) and advertised an adjustable
        // bore that was frozen. Refuse the key instead, naming the ones this
        // entity takes (docs/agent-scale-plan.md, G1b).
        for (auto it = ej.begin(); it != ej.end(); ++it) {
          const std::string& key = it.key();
          if (key.size() <= 5 || key.compare(key.size() - 5, 5, "_expr") != 0)
            continue;
          const std::string field = key.substr(0, key.size() - 5);
          if (std::find(fields.begin(), fields.end(), field) != fields.end())
            continue;
          std::string takes;
          for (const std::string& f : fields)
            takes += (takes.empty() ? "" : ", ") + f + "_expr";
          throw std::runtime_error(
              "sketch entity of type '" + ej.value("type", std::string{}) +
              "': no expression field '" + key + "'. It takes " +
              (takes.empty() ? std::string("none — its geometry is not "
                                           "expression-driven; use a "
                                           "dimensional constraint")
                             : takes));
        }
        if (!exprs.empty())
          pendingEntityExprs.emplace_back(sk->entities.size(),
                                          std::move(exprs));
        sk->entities.push_back(std::move(ent));
      }
    }
    for (const auto& cj : j.value("constraints", json::array())) {
      sk->constraints.push_back(ConstraintFromJson(cj));
    }
    // Migration: pre-v4 sketches carry no per-entity id (all 0) and their
    // constraints reference entity *indices*. Stamp stable ids in list order
    // and shift every index ref to the matching id (index i -> id i+1). Unused
    // refs (default {0,0}) become {1,0} but are never read for their kind.
    const bool legacy =
        !sk->entities.empty() && sketch::EntityId(sk->entities[0]) == 0;
    if (legacy) {
      for (size_t i = 0; i < sk->entities.size(); i++)
        sketch::SetEntityId(sk->entities[i], static_cast<uint32_t>(i + 1));
      for (auto& c : sk->constraints) {
        c.a.entity += 1;
        c.b.entity += 1;
      }
    }
    // No counter to advance any more: ids are minted collision-resistantly
    // (SketchFeature::MintEntityId), so a document written by an older build
    // simply keeps the small ids it has, and anything added later cannot
    // collide with them. `next_entity_id` in such a file is ignored.
    //
    // Two entities sharing an id is refused rather than loaded: a face's
    // stable name is "<feature>/g<entityId>", so a duplicate makes that name
    // — and every fillet, mate or dimension keyed on it — ambiguous, while
    // the document rebuilds perfectly and says nothing.
    {
      std::set<uint32_t> seen;
      for (const auto& e : sk->entities) {
        const uint32_t id = sketch::EntityId(e);
        if (id != 0 && !seen.insert(id).second) {
          throw std::runtime_error(
              "sketch \"" + sk->GetName() + "\" has two entities with id " +
              std::to_string(id) +
              " — a merge kept both; the face names built from it would be "
              "ambiguous, so keep one and reopen");
        }
      }
    }
    // Re-key the collected entity exprs by final stable id (positions are
    // still valid — the rect expansion below is what reorders).
    for (auto& [slot, exprs] : pendingEntityExprs) {
      const uint32_t id = sketch::EntityId(sk->entities[slot]);
      if (id != 0)
        sk->entityExprs[id] = std::move(exprs);
    }
    // Now expand the placeholder rects into real lines (AddEntity mints
    // non-colliding ids). Drop any constraint that
    // referenced a rigid rect — those were solver no-ops.
    if (!legacyRects.empty()) {
      std::vector<uint32_t> rectIds;
      for (size_t slot : rectSlots) // ids are final; migration didn't reorder
        rectIds.push_back(sketch::EntityId(sk->entities[slot]));
      auto isRect = [&](uint32_t id) {
        return std::find(rectIds.begin(), rectIds.end(), id) != rectIds.end();
      };
      sk->entities.erase(std::remove_if(sk->entities.begin(),
                                        sk->entities.end(),
                                        [&](const sketch::Entity& e) {
                                          return isRect(sketch::EntityId(e));
                                        }),
                         sk->entities.end());
      sk->constraints.erase(
          std::remove_if(sk->constraints.begin(), sk->constraints.end(),
                         [&](const sketch::Constraint& c) {
                           return isRect(c.a.entity) || isRect(c.b.entity);
                         }),
          sk->constraints.end());
      // The four lines a legacy rectangle expands into get DETERMINISTIC ids,
      // not the collision-resistant random ones AddEntity mints.
      //
      // A face's stable name is "<feature>/g<entityId>", so random ids here
      // meant the same pre-v5 file loaded twice produced DIFFERENT face names
      // for the same geometry — identical volume and bounding box, and every
      // fillet, mate connector, dimension or CAM target keyed on one of those
      // names pointing somewhere else. Caught by
      // `Determinism: two independent loads agree, body for body`.
      //
      // Random is right where it came from: two people ADDING entities to a
      // shared sketch offline must not mint the same id (C6, 2026-08-22). A
      // migration is not that — it is a pure function of the bytes on disk,
      // and two people opening the same old file must get the same answer.
      // Ids continue past the highest already present, so they cannot collide
      // with the ones the file carries.
      // Past every id the FILE ever used, including the rect placeholders
      // just erased — not merely past the surviving entities. Handing a new
      // line the id the rectangle itself had would make an older reference to
      // "<feature>/g<rectId>" resolve to a real edge instead of failing, which
      // is the silent repoint this whole scheme exists to prevent.
      uint32_t nextMigrated = 0;
      for (const auto& e : sk->entities)
        nextMigrated = std::max(nextMigrated, sketch::EntityId(e));
      for (const uint32_t id : rectIds)
        nextMigrated = std::max(nextMigrated, id);
      for (const auto& [c1, c2] : legacyRects) {
        const std::array<uint32_t, 4> minted = sk->AddRectangle(c1, c2);
        for (const uint32_t from : minted) {
          const uint32_t to = ++nextMigrated;
          for (auto& e : sk->entities)
            if (sketch::EntityId(e) == from)
              sketch::SetEntityId(e, to);
          // AddRectangle writes eight constraints over exactly these ids.
          for (auto& c : sk->constraints) {
            if (c.a.entity == from)
              c.a.entity = to;
            if (c.b.entity == from)
              c.b.entity = to;
            if (c.c.entity == from)
              c.c.entity = to;
          }
        }
      }
    }
    for (const auto& oj : j.value("derived_ops", json::array()))
      sk->derived.push_back(DerivedOpFromJson(oj));
    if (!sk->derived.empty())
      sk->RegenerateDerived(); // re-verify generated geometry (id-stable)
    for (const auto& ej : j.value("external", json::array())) { // v48
      sketch::ExternalGeom eg;
      eg.ref = ElementRef{ej.value("feature_id", uint64_t{0}),
                          ej.value("element", std::string{})};
      eg.kind = ej.value("kind", std::string("face")) == "edge"
                    ? sketch::ExternalKind::Edge
                    : sketch::ExternalKind::Face;
      eg.genIds = ej.value("gen_ids", std::vector<uint32_t>{});
      sk->external.push_back(std::move(eg));
    }
    for (const auto& ij : j.value("images", json::array())) { // v49 underlays
      sketch::ReferenceImage img;
      img.id = ij.value("id", uint32_t{0});
      img.asset = ij.value("asset", std::string{});
      img.origin = Vec2FromJson(ij.at("origin"));
      img.rotation = ij.value("rotation", 0.0);
      img.unitsPerPixel = ij.value("units_per_pixel", 1.0);
      img.opacity = ij.value("opacity", 0.5);
      img.widthPx = ij.value("width_px", 0);
      img.heightPx = ij.value("height_px", 0);
      img.hidden = ij.value("hidden", false);
      // `bytes` are filled in out-of-band by the loader (assets/ file or the
      // single-file base64 blob), keyed by `asset`.
      sk->images.push_back(std::move(img));
    }
    if (!sk->images.empty()) {
      uint32_t maxImg = 0;
      for (const auto& img : sk->images)
        maxImg = std::max(maxImg, img.id);
      sk->nextImageId = std::max<uint32_t>(
          {maxImg + 1, j.value("next_image_id", uint32_t{0})});
    }
    sk->planeRef =
        j.value("plane_ref", uint64_t{0}); // absent (pre-v12) = baked
    if (j.contains("face_ref")) {          // v16: attached body face
      const json& fr = j.at("face_ref");
      sk->faceRef = ElementRef{fr.value("feature_id", uint64_t{0}),
                               fr.value("element", std::string{})};
    }
    out = std::move(sk);
  } else if (type == "Extrude") {
    // `distance` is optional when a `distance_expr` drives it (mirrors
    // Revolve's `angle`/`angle_expr`): the literal is just the last-good value
    // the expr overwrites on regen, so an extrude authored with only
    // `distance_expr` still builds a body instead of erroring on a missing key.
    auto ext = std::make_unique<ExtrudeFeature>(
        name, j.at("sketch_id").get<uint64_t>(), j.value("distance", 1.0),
        ExtrudeModeFromString(j.value("mode", "new_body")),
        j.value("reversed", false));
    ext->distanceExpr =
        j.value("distance_expr", std::string{}); // absent (pre-v11) = literal
    // End condition (v18); absent = blind.
    const std::string end = j.value("end_mode", "blind");
    ext->endMode = end == "symmetric"     ? ExtrudeEnd::Symmetric
                   : end == "two_sided"   ? ExtrudeEnd::TwoSided
                   : end == "up_to_face"  ? ExtrudeEnd::UpToFace
                   : end == "through_all" ? ExtrudeEnd::ThroughAll // v52
                                          : ExtrudeEnd::Blind;
    ext->distance2 = j.value("distance2", 1.0);
    ext->distance2Expr = j.value("distance2_expr", std::string{});
    if (j.contains("regions")) // v34
      ext->regions = j["regions"].get<std::vector<std::vector<uint32_t>>>();
    if (j.contains("end_target")) {
      ext->endTarget =
          ElementRef{j["end_target"].value("feature_id", uint64_t{0}),
                     j["end_target"].value("element", std::string{})};
    }
    if (j.contains("thin")) { // v18
      const json& t = j["thin"];
      ext->thin = true;
      ext->thinThickness = t.value("thickness", 1.0);
      ext->thinThicknessExpr = t.value("thickness_expr", std::string{});
      const std::string side = t.value("side", "out");
      ext->thinSide = side == "in"     ? sketch::ThinSide::In
                      : side == "both" ? sketch::ThinSide::Both
                                       : sketch::ThinSide::Out;
    }
    out = std::move(ext);
  } else if (type == "Box") {
    auto box = std::make_unique<BoxFeature>(
        name, Vec3FromJson(j.at("position")), j.at("width").get<double>(),
        j.at("height").get<double>(), j.at("depth").get<double>());
    // v61 expression fields; absent in an older file, which just means the
    // dimension is a literal.
    box->widthExpr = j.value("width_expr", std::string{});
    box->heightExpr = j.value("height_expr", std::string{});
    box->depthExpr = j.value("depth_expr", std::string{});
    out = std::move(box);
  } else if (type == "Sphere") {
    auto sph = std::make_unique<SphereFeature>(
        name, Vec3FromJson(j.at("center")), j.at("radius").get<double>());
    sph->radiusExpr = j.value("radius_expr", std::string{});
    out = std::move(sph);
  } else if (type == "Cylinder") {
    auto cyl = std::make_unique<CylinderFeature>(
        name, Vec3FromJson(j.at("center")), Vec3FromJson(j.at("direction")),
        j.at("radius").get<double>(), j.at("height").get<double>());
    cyl->radiusExpr = j.value("radius_expr", std::string{});
    cyl->heightExpr = j.value("height_expr", std::string{});
    out = std::move(cyl);
  } else if (type == "Fastener") {
    // `length` is OPTIONAL, because whether it means anything is the
    // standard's business and not the loader's: a nut and a washer have none,
    // and requiring one here made them unreachable through add_feature while
    // the feature itself was perfectly able to build them. A screw without one
    // is still refused — by Execute, which knows which is which.
    auto fastener = std::make_unique<FastenerFeature>(
        name, j.at("standard").get<std::string>(),
        j.at("size").get<std::string>(), j.value("length", 0.0));
    fastener->lengthExpr = j.value("length_expr", std::string{});
    fastener->drive = j.value("drive", std::string{});
    out = std::move(fastener);
  } else if (type == "Import") {
    auto imp =
        std::make_unique<ImportFeature>(name, j.at("path").get<std::string>());
    imp->keepOrigin = j.value("keep_origin", false); // v97
    if (j.contains("units")) { // v103; refused, never read as mm
      const std::string u = j.at("units").get<std::string>();
      if (!units::LengthFromString(u, imp->lengthUnit)) {
        std::string accepted;
        for (const char* n : units::LengthNames())
          accepted += (accepted.empty() ? "" : ", ") + std::string(n);
        throw std::runtime_error("Import: unknown units '" + u +
                                 "'. Accepted: " + accepted);
      }
    }
    if (j.contains("color")) {
      imp->hasColor = true;
      imp->color = Vec3FromJson(j.at("color"));
    }
    out = std::move(imp);
  } else if (type == "Derived") {
    auto d = std::make_unique<DerivedFeature>(
        name, j.at("source_part").get<uint64_t>());
    d->sourceName = j.value("source_name", std::string{});
    if (j.contains("bodies"))
      d->bodies = j.at("bodies").get<std::vector<uint64_t>>();
    if (j.contains("body_names")) // v85
      d->bodyNames = j.at("body_names").get<std::vector<std::string>>();
    if (j.contains("placement")) {
      const json& pl = j.at("placement");
      if (pl.contains("position"))
        d->position = Vec3FromJson(pl.at("position"));
      if (pl.contains("orientation"))
        d->orientation = QuatFromJson(pl.at("orientation"));
    }
    d->reference = j.value("reference", true);
    if (j.contains("snapshot")) {
      const json& snap = j.at("snapshot");
      d->sourceChainHash = snap.value("source_chain_hash", size_t{0});
      for (const json& bj : snap.value("bodies", json::array())) {
        DerivedBody b;
        b.asset = bj.at("asset").get<std::string>();
        b.name = bj.value("name", std::string{});
        b.material = bj.value("material", std::string{});
        b.sourceCreatedBy = bj.value("source_created_by", uint64_t{0});
        b.elementMap =
            ElementMapFromJson(bj.value("element_map", json::object()));
        if (bj.contains("display_color")) {
          b.hasDisplayColor = true;
          b.displayColor = Vec3FromJson(bj.at("display_color"));
        }
        d->snapshot.push_back(std::move(b));
      }
    }
    out = std::move(d);
  } else if (type == "DatumPlane") {
    auto dp = std::make_unique<DatumPlaneFeature>(
        name, DatumPlaneKindFromString(j.at("kind").get<std::string>()));
    dp->parentId = j.value("parent_id", uint64_t{0});
    if (j.contains("base_plane"))
      dp->basePlane =
          PlaneTypeFromString(j.at("base_plane").get<std::string>());
    switch (dp->kind) {
    case DatumPlaneKind::Offset:
      dp->offset = j.value("offset", 0.0);
      dp->offsetExpr = j.value("offset_expr", std::string{});
      dp->flip = j.value("flip", false);
      break;
    case DatumPlaneKind::Angled:
      dp->axis = AxisRefFromJson(j.at("axis"));
      dp->angleDeg = j.value("angle_deg", 0.0);
      dp->angleExpr = j.value("angle_expr", std::string{});
      break;
    case DatumPlaneKind::ThroughPoints: {
      const json& pts = j.at("points");
      dp->p0 = Vec3FromJson(pts.at(0));
      dp->p1 = Vec3FromJson(pts.at(1));
      dp->p2 = Vec3FromJson(pts.at(2));
      break;
    }
    }
    out = std::move(dp);
  } else if (type == "Revolve") {
    auto rev = std::make_unique<RevolveFeature>(
        name, j.at("sketch_id").get<uint64_t>(), j.value("angle_deg", 360.0),
        ExtrudeModeFromString(j.value("mode", "new_body")),
        j.value("reversed", false));
    rev->axis = AxisRefFromJson(j.at("axis"));
    rev->angleExpr = j.value("angle_expr", std::string{});
    rev->symmetric = j.value("symmetric", false);
    if (j.contains("regions")) // v34
      rev->regions = j["regions"].get<std::vector<std::vector<uint32_t>>>();
    out = std::move(rev);
  } else if (type == "DatumAxis") {
    auto da = std::make_unique<DatumAxisFeature>(
        name, DatumAxisKindFromString(j.at("kind").get<std::string>()));
    switch (da->kind) {
    case DatumAxisKind::TwoPoints: {
      const json& pts = j.at("points");
      da->p0 = Vec3FromJson(pts.at(0));
      da->p1 = Vec3FromJson(pts.at(1));
      break;
    }
    case DatumAxisKind::SketchLine:
      da->sketchId = j.at("sketch_id").get<uint64_t>();
      da->entityId = j.at("entity_id").get<uint32_t>();
      break;
    case DatumAxisKind::GlobalX:
    case DatumAxisKind::GlobalY:
    case DatumAxisKind::GlobalZ:
      break;
    }
    out = std::move(da);
  } else if (type == "Frame") { // v86
    auto fr = std::make_unique<FrameFeature>(
        name, FrameKindFromString(j.at("kind").get<std::string>()));
    const auto triple = [&](const char* key, double* out, bool required) {
      if (!j.contains(key)) {
        if (required)
          throw std::runtime_error(std::string("Frame '") + name + "' needs " +
                                   key + " [x, y, z]");
        return;
      }
      const json& a = j.at(key);
      if (!a.is_array() || a.size() != 3)
        throw std::runtime_error(std::string("Frame '") + name + "': " + key +
                                 " is [x, y, z]");
      for (size_t i = 0; i < 3; i++)
        out[i] = a.at(i).get<double>();
    };
    const auto exprTriple = [&](const char* key, std::string* out) {
      if (!j.contains(key))
        return;
      const json& a = j.at(key);
      if (!a.is_array() || a.size() != 3)
        throw std::runtime_error(std::string("Frame '") + name + "': " + key +
                                 " is [expr_x, expr_y, expr_z] (\"\" = "
                                 "literal)");
      for (size_t i = 0; i < 3; i++)
        out[i] = a.at(i).get<std::string>();
    };
    switch (fr->kind) {
    case FrameKind::Explicit:
      triple("origin", fr->origin, /*required=*/false);
      exprTriple("origin_expr", fr->originExpr);
      if (j.contains("z_axis"))
        fr->zAxis = Vec3FromJson(j.at("z_axis"));
      if (j.contains("x_axis"))
        fr->xAxis = Vec3FromJson(j.at("x_axis"));
      break;
    case FrameKind::FromElement:
      if (!j.contains("elements") || !j.at("elements").is_array())
        throw std::runtime_error(
            std::string("Frame '") + name +
            "' from_element needs elements: [{feature_id, element}, ...]");
      for (const json& e : j.at("elements"))
        fr->elements.push_back(ElementRefFromJson(e));
      fr->flip = j.value("flip", false);
      break;
    case FrameKind::Offset:
      fr->parentId = j.value("parent_id", uint64_t{0});
      triple("offset", fr->offset, /*required=*/false);
      exprTriple("offset_expr", fr->offsetExpr);
      triple("rotation_deg", fr->rotationDeg, /*required=*/false);
      exprTriple("rotation_expr", fr->rotationExpr);
      break;
    }
    out = std::move(fr);
  } else if (type == "Curve3D") { // v42
    auto cv = std::make_unique<CurveFeature>(
        name, Curve3DKindFromString(j.at("kind").get<std::string>()));
    switch (cv->kind) {
    case Curve3DKind::FitSpline:
    case Curve3DKind::ControlPoly:
      for (const auto& pj : j.value("points", json::array()))
        cv->points.push_back(Vec3FromJson(pj));
      cv->degree = j.value("degree", 3);
      cv->closed = j.value("closed", false);
      break;
    case Curve3DKind::Helix:
      cv->axisOrigin = Vec3FromJson(j.at("origin"));
      cv->axisDir = Vec3FromJson(j.at("axis"));
      cv->radius = j.value("radius", 1.0);
      cv->radiusExpr = j.value("radius_expr", std::string{});
      cv->pitch = j.value("pitch", 1.0);
      cv->pitchExpr = j.value("pitch_expr", std::string{});
      cv->turns = j.value("turns", 1.0);
      cv->turnsExpr = j.value("turns_expr", std::string{});
      cv->rightHanded = !j.value("left_handed", false);
      break;
    }
    out = std::move(cv);
  } else if (type == "Boolean") {
    out = std::make_unique<BooleanFeature>(
        name, j.at("target_id").get<uint64_t>(),
        j.at("tool_id").get<uint64_t>(),
        BooleanOpFromString(j.value("op", "union")));
  } else if (type == "Split") { // v39
    auto sp =
        std::make_unique<SplitFeature>(name, j.at("body_id").get<uint64_t>());
    if (j.value("knife", "plane") == "face") {
      sp->knife = SplitKnifeKind::Face;
      const json& fj = j.at("face");
      sp->face = ElementRef{fj.at("feature_id").get<uint64_t>(),
                            fj.at("element").get<std::string>()};
    } else {
      sp->knife = SplitKnifeKind::Plane;
      sp->datumId = j.value("datum_id", uint64_t{0});
      if (j.contains("base_plane"))
        sp->basePlane =
            PlaneTypeFromString(j.at("base_plane").get<std::string>());
      sp->offset = j.value("offset", 0.0);
      sp->offsetExpr = j.value("offset_expr", std::string{});
    }
    out = std::move(sp);
  } else if (type == "Refine") { // v47
    out =
        std::make_unique<RefineFeature>(name, j.at("body_id").get<uint64_t>());
  } else if (type == "Fillet" || type == "Chamfer") { // v17
    std::vector<ElementRef> refs;
    for (const auto& ej : j.value("edges", json::array())) {
      refs.push_back(ElementRef{ej.value("feature_id", uint64_t{0}),
                                ej.value("element", std::string{})});
    }
    auto fil = std::make_unique<FilletFeature>(
        name, std::move(refs), j.value("radius", 0.5),
        type == "Chamfer" ? EdgeBlendOp::Chamfer : EdgeBlendOp::Fillet);
    fil->radiusExpr = j.value("radius_expr", std::string{});
    out = std::move(fil);
  } else if (type == "BaseFlange") { // v109
    auto bf = std::make_unique<BaseFlangeFeature>(
        name, j.at("sketch_id").get<uint64_t>(), j.value("reversed", false));
    if (j.contains("regions"))
      bf->regions = j["regions"].get<std::vector<std::vector<uint32_t>>>();
    out = std::move(bf);
  } else if (type == "EdgeFlange") { // v109
    std::vector<ElementRef> picked;
    for (const auto& ej : j.value("edges", json::array()))
      picked.push_back(ElementRef{ej.value("feature_id", uint64_t{0}),
                                  ej.value("element", std::string{})});
    auto ef = std::make_unique<EdgeFlangeFeature>(name, std::move(picked),
                                                  j.value("length", 10.0),
                                                  j.value("angle_deg", 90.0));
    ef->lengthExpr = j.value("length_expr", std::string{});
    ef->angleDegExpr = j.value("angle_deg_expr", std::string{});
    ef->radius = j.value("radius", 0.0);
    ef->radiusExpr = j.value("radius_expr", std::string{});
    if (j.contains("position")) {
      const std::string v = j.at("position").get<std::string>();
      if (!FlangePositionFromString(v, ef->position))
        throw std::runtime_error("unknown flange position '" + v +
                                 "' (expected one of: " + FlangePositionList() +
                                 ")");
    }
    if (j.contains("length_mode")) {
      const std::string v = j.at("length_mode").get<std::string>();
      if (!FlangeLengthModeFromString(v, ef->lengthMode))
        throw std::runtime_error(
            "unknown flange length_mode '" + v +
            "' (expected one of: " + FlangeLengthModeList() + ")");
    }
    ef->offsetStart = j.value("offset_start", 0.0);
    ef->offsetStartExpr = j.value("offset_start_expr", std::string{});
    ef->offsetEnd = j.value("offset_end", 0.0);
    ef->offsetEndExpr = j.value("offset_end_expr", std::string{});
    if (j.contains("relief")) {
      const std::string v = j.at("relief").get<std::string>();
      sheet::ReliefStyle style{};
      if (!sheet::ReliefStyleFromString(v, style))
        throw std::runtime_error(
            "unknown relief style '" + v +
            "' (expected one of: " + sheet::ReliefStyleList() + ")");
      ef->reliefStyle = style;
    }
    if (j.contains("corners")) { // v113
      const std::string v = j.at("corners").get<std::string>();
      if (v == "closed")
        ef->closedCorners = true;
      else if (v != "open")
        throw std::runtime_error("unknown flange corners '" + v +
                                 "' (expected one of: open, closed)");
    }
    out = std::move(ef);
  } else if (type == "FlatPattern") { // v110
    auto fp = std::make_unique<FlatPatternFeature>(
        name, j.at("body_id").get<uint64_t>());
    if (j.contains("fixed_face")) {
      const auto& fj = j.at("fixed_face");
      fp->fixedFace = ElementRef{fj.value("feature_id", uint64_t{0}),
                                 fj.value("element", std::string{})};
    }
    out = std::move(fp);
  } else if (type == "Unfold") { // v111
    auto uf =
        std::make_unique<UnfoldFeature>(name, j.at("body_id").get<uint64_t>());
    if (j.contains("fixed_face")) {
      const auto& fj = j.at("fixed_face");
      uf->fixedFace = ElementRef{fj.value("feature_id", uint64_t{0}),
                                 fj.value("element", std::string{})};
    }
    uf->bendFeatures = j.value("bends", std::vector<uint64_t>{});
    out = std::move(uf);
  } else if (type == "Refold") { // v111
    out = std::make_unique<RefoldFeature>(name,
                                          j.at("unfold_id").get<uint64_t>());
  } else if (type == "Hem") { // v112
    std::vector<ElementRef> picked;
    for (const auto& ej : j.value("edges", json::array()))
      picked.push_back(ElementRef{ej.value("feature_id", uint64_t{0}),
                                  ej.value("element", std::string{})});
    auto hm = std::make_unique<HemFeature>(name, std::move(picked),
                                           j.value("length", 5.0));
    hm->lengthExpr = j.value("length_expr", std::string{});
    if (j.contains("kind")) {
      const std::string v = j.at("kind").get<std::string>();
      if (!HemKindFromString(v, hm->kind))
        throw std::runtime_error("unknown hem kind '" + v +
                                 "' (expected one of: " + HemKindList() + ")");
    }
    hm->gap = j.value("gap", 0.0);
    hm->gapExpr = j.value("gap_expr", std::string{});
    hm->outside = j.value("outside", false);
    out = std::move(hm);
  } else if (type == "Jog") { // v112
    auto jg = std::make_unique<JogFeature>(
        name, j.at("sketch_id").get<uint64_t>(), j.value("height", 5.0));
    jg->heightExpr = j.value("height_expr", std::string{});
    jg->angleDeg = j.value("angle_deg", 90.0);
    jg->angleDegExpr = j.value("angle_deg_expr", std::string{});
    jg->flip = j.value("flip", false);
    out = std::move(jg);
  } else if (type == "CornerRelief") { // v113
    auto cr = std::make_unique<CornerReliefFeature>(
        name, j.at("body_id").get<uint64_t>());
    if (j.contains("style")) {
      const std::string v = j.at("style").get<std::string>();
      if (!CornerReliefStyleFromString(v, cr->style))
        throw std::runtime_error(
            "unknown corner relief style '" + v +
            "' (expected one of: " + CornerReliefStyleList() + ")");
    }
    cr->size = j.value("size", 0.0);
    cr->sizeExpr = j.value("size_expr", std::string{});
    out = std::move(cr);
  } else if (type == "Shell") { // v20
    std::vector<ElementRef> faces;
    for (const auto& ej : j.value("open_faces", json::array()))
      faces.push_back(ElementRef{ej.value("feature_id", uint64_t{0}),
                                 ej.value("element", std::string{})});
    auto sh = std::make_unique<ShellFeature>(
        name, j.at("body_id").get<uint64_t>(), std::move(faces),
        j.value("thickness", 1.0));
    sh->thicknessExpr = j.value("thickness_expr", std::string{});
    out = std::move(sh);
  } else if (type == "Loft") { // v20
    std::vector<uint64_t> ids;
    for (const auto& idj : j.value("sketch_ids", json::array()))
      ids.push_back(idj.get<uint64_t>());
    auto lo = std::make_unique<LoftFeature>(
        name, std::move(ids),
        ExtrudeModeFromString(j.value("mode", "new_body")));
    lo->pathId = j.value("path_id", uint64_t{0});   // v45
    lo->guideId = j.value("guide_id", uint64_t{0}); // v45
    out = std::move(lo);
  } else if (type == "Sweep") { // v20
    auto sw = std::make_unique<SweepFeature>(
        name, j.at("profile_id").get<uint64_t>(),
        j.at("path_id").get<uint64_t>(),
        ExtrudeModeFromString(j.value("mode", "new_body")));
    sw->guideId = j.value("guide_id", uint64_t{0});              // v44
    const std::string fr = j.value("frame", "corrected_frenet"); // v43
    sw->frame = fr == "frenet"  ? SweepFrame::Frenet
                : fr == "fixed" ? SweepFrame::Fixed
                                : SweepFrame::CorrectedFrenet;
    out = std::move(sw);
  } else if (type == "Pipe") { // v43
    auto pi = std::make_unique<PipeFeature>(
        name, j.at("path_id").get<uint64_t>(), j.value("outer_diameter", 2.0),
        j.value("wall_thickness", 0.0),
        ExtrudeModeFromString(j.value("mode", "new_body")));
    pi->outerDiameterExpr = j.value("outer_diameter_expr", std::string{});
    pi->wallThicknessExpr = j.value("wall_thickness_expr", std::string{});
    out = std::move(pi);
  } else if (type == "Rib") { // v36
    auto rb = std::make_unique<RibFeature>(
        name, j.at("sketch_id").get<uint64_t>(), j.value("thickness", 1.0),
        j.value("flip", false), j.value("body_id", uint64_t{0}));
    rb->thicknessExpr = j.value("thickness_expr", std::string{});
    out = std::move(rb);
  } else if (type == "Thread") { // v36
    const json& fj = j.at("face");
    auto th = std::make_unique<ThreadFeature>(
        name,
        ElementRef{fj.at("feature_id").get<uint64_t>(),
                   fj.at("element").get<std::string>()},
        j.value("designation", std::string{}), j.value("pitch", 1.25));
    th->threadClass = j.value("thread_class", std::string{});
    th->length = j.value("length", 0.0);
    th->lengthExpr = j.value("length_expr", std::string{});
    th->modeled = j.value("modeled", false);
    th->allowEngagement = j.value("allow_engagement", false);
    th->minimumEngagement = j.value("minimum_engagement", 0.0);
    out = std::move(th);
  } else if (type == "Hole") { // v21
    const std::string k = j.value("hole_kind", "simple");
    HoleKind kind = k == "counterbore"   ? HoleKind::Counterbore
                    : k == "countersink" ? HoleKind::Countersink
                                         : HoleKind::Simple;
    auto ho = std::make_unique<HoleFeature>(
        name, j.at("sketch_id").get<uint64_t>(), kind, j.value("diameter", 1.0),
        j.value("depth", 1.0));
    ho->diameterExpr = j.value("diameter_expr", std::string{});
    ho->depthExpr = j.value("depth_expr", std::string{});
    ho->through = j.value("through", false);
    ho->cbDiameter = j.value("cb_diameter", 2.0);
    ho->cbDepth = j.value("cb_depth", 0.5);
    ho->csDiameter = j.value("cs_diameter", 2.0);
    ho->csDepth = j.value("cs_depth", 0.5);
    if (j.contains("fit")) { // v71
      const auto& fit = j.at("fit");
      ho->fitThread = fit.value("thread", std::string{});
      ho->fitKind = fit.value("kind", std::string("normal"));
      ho->fitPitch = fit.value("pitch", 0.0);
    }
    out = std::move(ho);
  } else if (type == "Draft") { // v22
    std::vector<ElementRef> refs;
    for (const auto& ej : j.value("faces", json::array()))
      refs.push_back(ElementRef{ej.value("feature_id", uint64_t{0}),
                                ej.value("element", std::string{})});
    auto dr = std::make_unique<DraftFeature>(
        name, j.at("body_id").get<uint64_t>(), std::move(refs),
        j.value("angle_deg", 5.0),
        PlaneTypeFromString(j.value("neutral_plane", "xy")));
    dr->angleExpr = j.value("angle_expr", std::string{});
    out = std::move(dr);
  } else if (type == "Pattern") {
    auto pat = std::make_unique<PatternFeature>(
        name, PatternKindFromString(j.at("kind").get<std::string>()),
        j.at("source_id").get<uint64_t>());
    if (pat->kind == PatternKind::Mirror) {
      pat->planeId = j.value("parent_id", uint64_t{0});
      if (j.contains("base_plane"))
        pat->basePlane =
            PlaneTypeFromString(j.at("base_plane").get<std::string>());
    } else {
      pat->axis = AxisRefFromJson(j.at("axis"));
      pat->count = j.value("count", 3);
      pat->countExpr = j.value("count_expr", std::string{});
      if (pat->kind == PatternKind::Linear) {
        pat->spacing = j.value("spacing", 5.0);
        pat->spacingExpr = j.value("spacing_expr", std::string{});
      } else {
        pat->angleDeg = j.value("angle_deg", 90.0);
        pat->angleExpr = j.value("angle_expr", std::string{});
      }
    }
    out = std::move(pat);
  }

  if (!out) {
    // Reached only when kFeatureTypes lists a type the chain above does not
    // build — the row-without-a-branch half of the drift the gate catches.
    throw std::runtime_error(
        "Feature type '" + type +
        "' is listed in kFeatureTypes but FeatureFromJson has no branch for "
        "it — add the branch, or drop the row.");
  }
  out->SetId(
      j.value("id", uint64_t{0})); // v1 files have no id; Document assigns
  out->SetMaterial(
      j.value("material", std::string{})); // absent (pre-v10) = default
  if (j.contains("appearance")) {          // absent (pre-v54) = pure material
    const json& a = j.at("appearance");
    Appearance ap;
    if (a.contains("color")) {
      const json& c = a.at("color");
      ap.color = glm::vec3(c.at(0).get<float>(), c.at(1).get<float>(),
                           c.at(2).get<float>());
    }
    if (a.contains("roughness"))
      ap.roughness = a.at("roughness").get<float>();
    if (a.contains("metallic"))
      ap.metallic = a.at("metallic").get<float>();
    out->SetAppearance(ap);
  }
  out->SetSuppressed(j.value("suppressed", false)); // absent (pre-v19) = active
  return out;
}

} // namespace DocumentIO
