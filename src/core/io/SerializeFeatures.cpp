#include "core/io/Serialization.h"
#include "core/io/SerializationInternal.h"

#include "core/features/BooleanFeature.h"
#include "core/features/DatumAxisFeature.h"
#include "core/features/DatumPlaneFeature.h"
#include "core/features/DraftFeature.h"
#include "core/features/ExtrudeFeature.h"
#include "core/Feature.h"
#include "core/features/FilletFeature.h"
#include "core/features/HoleFeature.h"
#include "core/features/LoftFeature.h"
#include "core/features/PatternFeature.h"
#include "core/features/RevolveFeature.h"
#include "core/features/ShellFeature.h"
#include "core/features/SketchFeature.h"
#include "core/features/RibFeature.h"
#include "core/features/SweepFeature.h"
#include "core/features/ThreadFeature.h"

#include <nlohmann/json.hpp>

#include <map>
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
          return {{"type", "line"}, {"a", Vec2ToJson(e.a)},
                  {"b", Vec2ToJson(e.b)}};
        } else if constexpr (std::is_same_v<T, sketch::Circle>) {
          return {{"type", "circle"}, {"center", Vec2ToJson(e.center)},
                  {"radius", e.radius}};
        } else if constexpr (std::is_same_v<T, sketch::Arc>) {
          return {{"type", "arc"},
                  {"center", Vec2ToJson(e.center)},
                  {"radius", e.radius},
                  {"start_angle", e.startAngle},
                  {"end_angle", e.endAngle}};
        } else {
          return {{"type", "ellipse"},
                  {"center", Vec2ToJson(e.center)},
                  {"r_major", e.rMajor},
                  {"r_minor", e.rMinor},
                  {"rotation", e.rotation}};
        }
      },
      entity);
  j["id"] = sketch::EntityId(entity);
  if (sketch::IsConstruction(entity))
    j["construction"] = true; // omit when false to keep JSON compact
  return j;
}

sketch::Entity EntityFromJson(const json& j) {
  const std::string type = j.at("type").get<std::string>();
  // Pre-v4 files have no "id"; leave it 0 so Load can detect and migrate.
  const uint32_t id = j.value("id", uint32_t{0});
  sketch::Entity e;
  if (type == "point") {
    e = sketch::Point{Vec2FromJson(j.at("p"))};
  } else if (type == "line") {
    e = sketch::Line{Vec2FromJson(j.at("a")), Vec2FromJson(j.at("b"))};
  } else if (type == "circle") {
    e = sketch::Circle{Vec2FromJson(j.at("center")), j.at("radius").get<double>()};
  } else if (type == "arc") {
    e = sketch::Arc{Vec2FromJson(j.at("center")), j.at("radius").get<double>(),
                    j.at("start_angle").get<double>(),
                    j.at("end_angle").get<double>()};
  } else if (type == "ellipse") {
    e = sketch::Ellipse{Vec2FromJson(j.at("center")),
                        j.at("r_major").get<double>(),
                        j.at("r_minor").get<double>(),
                        j.at("rotation").get<double>()};
  } else {
    // "rect" is intercepted and expanded in FeatureFromJson (pre-v5 rigid
    // rectangles become four lines); anything else is unknown.
    throw std::runtime_error("Unknown sketch entity type: " + type);
  }
  sketch::SetEntityId(e, id);
  sketch::SetConstruction(e, j.value("construction", false));
  return e;
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
  case K::Symmetric: return "symmetric";
  case K::Midpoint: return "midpoint";
  case K::Fixed: return "fixed";
  case K::Concentric: return "concentric";
  case K::Collinear: return "collinear";
  case K::DistanceX: return "distance_x";
  case K::DistanceY: return "distance_y";
  case K::Diameter: return "diameter";
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
  if (s == "symmetric") return K::Symmetric;
  if (s == "midpoint") return K::Midpoint;
  if (s == "fixed") return K::Fixed;
  if (s == "concentric") return K::Concentric;
  if (s == "collinear") return K::Collinear;
  if (s == "distance_x") return K::DistanceX;
  if (s == "distance_y") return K::DistanceY;
  if (s == "diameter") return K::Diameter;
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
  if (sketch::IsDimension(c.kind) && (c.labelOffset.x != 0 || c.labelOffset.y != 0))
    j["label"] = Vec2ToJson(c.labelOffset);
  return j;
}

sketch::Constraint ConstraintFromJson(const json& j) {
  sketch::Constraint c;
  c.kind = ConstraintKindFromString(j.at("kind").get<std::string>());
  c.a = {j.at("a").at(0).get<uint32_t>(), j.at("a").at(1).get<uint8_t>()};
  c.b = {j.at("b").at(0).get<uint32_t>(), j.at("b").at(1).get<uint8_t>()};
  c.value = j.value("value", 0.0);
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
  json j = {{"kind", op.kind == sketch::DerivedKind::CircularPattern
                         ? "circular_pattern"
                     : op.kind == sketch::DerivedKind::LinkedCopy
                         ? "linked_copy" // v35
                         : "linear_pattern"},
            {"source_ids", op.sourceIds},
            {"vec", Vec2ToJson(op.vec)},
            {"scalar", op.scalar},
            {"count", op.count},
            {"instances", insts}};
  if (op.sourceSketchId != 0)
    j["source_sketch_id"] = op.sourceSketchId;
  return j;
}

sketch::DerivedOp DerivedOpFromJson(const json& j) {
  sketch::DerivedOp op;
  const std::string kind = j.at("kind").get<std::string>();
  op.kind = kind == "circular_pattern" ? sketch::DerivedKind::CircularPattern
            : kind == "linked_copy"    ? sketch::DerivedKind::LinkedCopy
                                       : sketch::DerivedKind::LinearPattern;
  op.sourceSketchId = j.value("source_sketch_id", uint64_t{0});
  for (const auto& s : j.at("source_ids"))
    op.sourceIds.push_back(s.get<uint32_t>());
  op.vec = Vec2FromJson(j.at("vec"));
  op.scalar = j.at("scalar").get<double>();
  op.count = j.at("count").get<int>();
  for (const auto& in : j.at("instances"))
    op.instances.push_back({in.at("source").get<uint32_t>(),
                            in.at("k").get<int>(),
                            in.at("gen").get<uint32_t>()});
  return op;
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

const char* BooleanOpToString(BooleanOp op) {
  switch (op) {
  case BooleanOp::Union: return "union";
  case BooleanOp::Subtract: return "subtract";
  case BooleanOp::Intersect: return "intersect";
  }
  return "union";
}

BooleanOp BooleanOpFromString(const std::string& s) {
  if (s == "union") return BooleanOp::Union;
  if (s == "subtract") return BooleanOp::Subtract;
  if (s == "intersect") return BooleanOp::Intersect;
  throw std::runtime_error("Unknown boolean op: " + s);
}

const char* PatternKindToString(PatternKind kind) {
  switch (kind) {
  case PatternKind::Mirror: return "mirror";
  case PatternKind::Linear: return "linear";
  case PatternKind::Circular: return "circular";
  }
  return "linear";
}

PatternKind PatternKindFromString(const std::string& s) {
  if (s == "mirror") return PatternKind::Mirror;
  if (s == "linear") return PatternKind::Linear;
  if (s == "circular") return PatternKind::Circular;
  throw std::runtime_error("Unknown pattern kind: " + s);
}

const char* DatumPlaneKindToString(DatumPlaneKind kind) {
  switch (kind) {
  case DatumPlaneKind::Offset: return "offset";
  case DatumPlaneKind::Angled: return "angled";
  case DatumPlaneKind::ThroughPoints: return "through_points";
  }
  return "offset";
}

DatumPlaneKind DatumPlaneKindFromString(const std::string& s) {
  if (s == "offset") return DatumPlaneKind::Offset;
  if (s == "angled") return DatumPlaneKind::Angled;
  if (s == "through_points") return DatumPlaneKind::ThroughPoints;
  throw std::runtime_error("Unknown datum plane kind: " + s);
}

const char* DatumAxisKindToString(DatumAxisKind kind) {
  switch (kind) {
  case DatumAxisKind::TwoPoints: return "two_points";
  case DatumAxisKind::SketchLine: return "sketch_line";
  case DatumAxisKind::GlobalX: return "global_x";
  case DatumAxisKind::GlobalY: return "global_y";
  case DatumAxisKind::GlobalZ: return "global_z";
  }
  return "two_points";
}

DatumAxisKind DatumAxisKindFromString(const std::string& s) {
  if (s == "two_points") return DatumAxisKind::TwoPoints;
  if (s == "sketch_line") return DatumAxisKind::SketchLine;
  if (s == "global_x") return DatumAxisKind::GlobalX;
  if (s == "global_y") return DatumAxisKind::GlobalY;
  if (s == "global_z") return DatumAxisKind::GlobalZ;
  throw std::runtime_error("Unknown datum axis kind: " + s);
}

const char* PlaneTypeToString(PlaneType type) {
  switch (type) {
  case PlaneType::XY: return "xy";
  case PlaneType::XZ: return "xz";
  case PlaneType::YZ: return "yz";
  }
  return "xy";
}

PlaneType PlaneTypeFromString(const std::string& s) {
  if (s == "xy") return PlaneType::XY;
  if (s == "xz") return PlaneType::XZ;
  if (s == "yz") return PlaneType::YZ;
  throw std::runtime_error("Unknown base plane: " + s);
}

json AxisRefToJson(const AxisRef& axis) {
  switch (axis.kind) {
  case AxisRef::Kind::X: return {{"kind", "x"}};
  case AxisRef::Kind::Y: return {{"kind", "y"}};
  case AxisRef::Kind::Z: return {{"kind", "z"}};
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
  if (kind == "x") axis.kind = AxisRef::Kind::X;
  else if (kind == "y") axis.kind = AxisRef::Kind::Y;
  else if (kind == "z") axis.kind = AxisRef::Kind::Z;
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

}  // namespace

namespace DocumentIO {

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
    j["next_entity_id"] = sk->nextEntityId;
    j["entities"] = json::array();
    for (const auto& entity : sk->entities) {
      j["entities"].push_back(EntityToJson(entity));
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
  } else if (auto* ext = dynamic_cast<const ExtrudeFeature*>(&feature)) {
    j["sketch_id"] = ext->sketchId;
    j["distance"] = ext->distance;
    if (!ext->distanceExpr.empty()) // literals stay plain numbers (v11)
      j["distance_expr"] = ext->distanceExpr;
    j["reversed"] = ext->reversed;
    j["mode"] = ExtrudeModeToString(ext->mode);
    if (ext->endMode != ExtrudeEnd::Blind) { // v18; omit the default
      j["end_mode"] = ext->endMode == ExtrudeEnd::Symmetric   ? "symmetric"
                      : ext->endMode == ExtrudeEnd::TwoSided ? "two_sided"
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
  } else if (auto* boo = dynamic_cast<const BooleanFeature*>(&feature)) {
    j["target_id"] = boo->targetId;
    j["tool_id"] = boo->toolId;
    j["op"] = BooleanOpToString(boo->op);
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
    j["mode"] = ExtrudeModeToString(lo->mode);
  } else if (auto* sw = dynamic_cast<const SweepFeature*>(&feature)) {
    j["profile_id"] = sw->profileSketchId;
    j["path_id"] = sw->pathSketchId;
    j["mode"] = ExtrudeModeToString(sw->mode);
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
    j["hole_kind"] = ho->kind == HoleKind::Counterbore ? "counterbore"
                     : ho->kind == HoleKind::Countersink ? "countersink"
                                                         : "simple";
    j["diameter"] = ho->diameter;
    if (!ho->diameterExpr.empty()) j["diameter_expr"] = ho->diameterExpr;
    j["depth"] = ho->depth;
    if (!ho->depthExpr.empty()) j["depth_expr"] = ho->depthExpr;
    if (ho->through) j["through"] = true;
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
    if (!dr->angleExpr.empty()) j["angle_expr"] = dr->angleExpr;
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
    // Pre-v5 rigid "rect" entities are expanded into four lines below. During
    // loading they occupy a placeholder slot (preserving order/indexing so the
    // pre-v4 index migration stays correct); their corners are remembered.
    std::vector<std::pair<glm::vec2, glm::vec2>> legacyRects;
    std::vector<size_t> rectSlots; // placeholder positions in sk->entities
    for (const auto& ej : j.value("entities", json::array())) {
      if (ej.value("type", "") == "rect") {
        glm::vec2 c1 = Vec2FromJson(ej.at("c1"));
        glm::vec2 c2 = Vec2FromJson(ej.at("c2"));
        sketch::Entity ph = sketch::Line{c1, c2}; // placeholder, preserves order
        sketch::SetEntityId(ph, ej.value("id", uint32_t{0}));
        rectSlots.push_back(sk->entities.size());
        sk->entities.push_back(ph);
        legacyRects.emplace_back(c1, c2);
      } else {
        sk->entities.push_back(EntityFromJson(ej));
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
      sk->nextEntityId = static_cast<uint32_t>(sk->entities.size() + 1);
    } else {
      uint32_t maxId = 0;
      for (const auto& e : sk->entities)
        maxId = std::max(maxId, sketch::EntityId(e));
      sk->nextEntityId = std::max<uint32_t>(
          {sk->nextEntityId, maxId + 1, j.value("next_entity_id", uint32_t{0})});
    }
    // Now expand the placeholder rects into real lines (nextEntityId is final,
    // so the new lines get fresh non-colliding ids). Drop any constraint that
    // referenced a rigid rect — those were solver no-ops.
    if (!legacyRects.empty()) {
      std::vector<uint32_t> rectIds;
      for (size_t slot : rectSlots) // ids are final; migration didn't reorder
        rectIds.push_back(sketch::EntityId(sk->entities[slot]));
      auto isRect = [&](uint32_t id) {
        return std::find(rectIds.begin(), rectIds.end(), id) != rectIds.end();
      };
      sk->entities.erase(
          std::remove_if(sk->entities.begin(), sk->entities.end(),
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
      for (const auto& [c1, c2] : legacyRects)
        sk->AddRectangle(c1, c2);
    }
    for (const auto& oj : j.value("derived_ops", json::array()))
      sk->derived.push_back(DerivedOpFromJson(oj));
    if (!sk->derived.empty())
      sk->RegenerateDerived(); // re-verify generated geometry (id-stable)
    sk->planeRef = j.value("plane_ref", uint64_t{0}); // absent (pre-v12) = baked
    if (j.contains("face_ref")) { // v16: attached body face
      const json& fr = j.at("face_ref");
      sk->faceRef = ElementRef{fr.value("feature_id", uint64_t{0}),
                               fr.value("element", std::string{})};
    }
    out = std::move(sk);
  } else if (type == "Extrude") {
    auto ext = std::make_unique<ExtrudeFeature>(
        name, j.at("sketch_id").get<uint64_t>(), j.at("distance").get<double>(),
        ExtrudeModeFromString(j.value("mode", "new_body")),
        j.value("reversed", false));
    ext->distanceExpr =
        j.value("distance_expr", std::string{}); // absent (pre-v11) = literal
    // End condition (v18); absent = blind.
    const std::string end = j.value("end_mode", "blind");
    ext->endMode = end == "symmetric"    ? ExtrudeEnd::Symmetric
                   : end == "two_sided"  ? ExtrudeEnd::TwoSided
                   : end == "up_to_face" ? ExtrudeEnd::UpToFace
                                         : ExtrudeEnd::Blind;
    ext->distance2 = j.value("distance2", 1.0);
    ext->distance2Expr = j.value("distance2_expr", std::string{});
    if (j.contains("regions")) // v34
      ext->regions =
          j["regions"].get<std::vector<std::vector<uint32_t>>>();
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
  } else if (type == "DatumPlane") {
    auto dp = std::make_unique<DatumPlaneFeature>(
        name, DatumPlaneKindFromString(j.at("kind").get<std::string>()));
    dp->parentId = j.value("parent_id", uint64_t{0});
    if (j.contains("base_plane"))
      dp->basePlane = PlaneTypeFromString(j.at("base_plane").get<std::string>());
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
      rev->regions =
          j["regions"].get<std::vector<std::vector<uint32_t>>>();
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
  } else if (type == "Boolean") {
    out = std::make_unique<BooleanFeature>(
        name, j.at("target_id").get<uint64_t>(),
        j.at("tool_id").get<uint64_t>(),
        BooleanOpFromString(j.value("op", "union")));
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
    out = std::make_unique<LoftFeature>(
        name, std::move(ids), ExtrudeModeFromString(j.value("mode",
                                                            "new_body")));
  } else if (type == "Sweep") { // v20
    out = std::make_unique<SweepFeature>(
        name, j.at("profile_id").get<uint64_t>(),
        j.at("path_id").get<uint64_t>(),
        ExtrudeModeFromString(j.value("mode", "new_body")));
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
    out = std::move(th);
  } else if (type == "Hole") { // v21
    const std::string k = j.value("hole_kind", "simple");
    HoleKind kind = k == "counterbore"  ? HoleKind::Counterbore
                    : k == "countersink" ? HoleKind::Countersink
                                         : HoleKind::Simple;
    auto ho = std::make_unique<HoleFeature>(
        name, j.at("sketch_id").get<uint64_t>(), kind,
        j.value("diameter", 1.0), j.value("depth", 1.0));
    ho->diameterExpr = j.value("diameter_expr", std::string{});
    ho->depthExpr = j.value("depth_expr", std::string{});
    ho->through = j.value("through", false);
    ho->cbDiameter = j.value("cb_diameter", 2.0);
    ho->cbDepth = j.value("cb_depth", 0.5);
    ho->csDiameter = j.value("cs_diameter", 2.0);
    ho->csDepth = j.value("cs_depth", 0.5);
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
    throw std::runtime_error("Unknown feature type: " + type);
  }
  out->SetId(j.value("id", uint64_t{0})); // v1 files have no id; Document assigns
  out->SetMaterial(j.value("material", std::string{})); // absent (pre-v10) = default
  out->SetSuppressed(j.value("suppressed", false)); // absent (pre-v19) = active
  return out;
}

}  // namespace DocumentIO
