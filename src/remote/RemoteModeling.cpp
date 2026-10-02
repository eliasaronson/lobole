#include "remote/RemoteModeling.h"
#include "ui/SelectionPipeline.h"

#include "cad/CADMaterials.h"
#include "cad/FaceSelector.h"
#include "core/Angles.h"
#include "core/Command.h"
#include "core/Document.h"
#include "core/Expression.h"
#include "core/Literals.h"
#include "core/Primitives.h"
#include "core/PrintSplit.h"
#include "core/features/DerivedFeature.h"
#include "core/features/SketchFeature.h"
#include "core/features/ThreadFeature.h"
#include "core/features/WormFeature.h"
#include "core/io/Serialization.h"
#include "core/sketch/SketchSolver.h"
#include "remote/RemoteHelpers.h"
#include "remote/RemoteInspection.h" // PrintedThreadJson
#include "remote/RemoteProject.h"    // RequireFreshTabName
#include "ui/AppState.h"
#include "ui/AssemblyActions.h" // PartDocById
#include "ui/DerivedActions.h"
#include "ui/DocumentSlots.h"
#include "ui/LiteralActions.h"
#include "ui/PreviewFeature.h"
#include "ui/RendererSync.h"
#include "ui/ToleranceActions.h" // PrintedThreadFor

#include "thirdparty/stb_image.h" // stbi_info_from_memory (impl in IBL.cpp)

#include <glm/glm.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using nlohmann::json;

namespace remote {

namespace {

// Radians from whichever spelling the caller used, refusing both at once.
//
// The third copy of the shape set_mate settled on (RemoteAssembly.cpp's
// MateRotationRadians) and the arc parser reuses (SerializeFeatures.cpp's
// AngleRadians): bare name = RADIANS, `_deg` twin = degrees, never both. They
// stay separate small readers rather than one shared helper because each
// names what IS accepted in its own message, and the part that must not drift
// — the factor — is core/Angles.h now.
//
// `absent` is what a caller who passed neither gets, so an optional angle and
// a required one share one reader.
//
// Takes the VALUES rather than the params object, exactly as
// MateRotationRadians does and for its reason: rpc_params_gate learns a
// method's vocabulary by reading which keys the handler touches, and a helper
// that reached into `p` itself would make the method look as though it had
// quietly stopped accepting either spelling.
double AngleParam(bool hasRad, double rad, bool hasDeg, double deg,
                  const char* radKey, const char* degKey, double absent) {
  if (hasRad && hasDeg)
    throw std::runtime_error(std::string("pass ") + radKey + " (radians) or " +
                             degKey + " (degrees), not both");
  if (hasDeg)
    return angles::ToRadians(deg);
  if (!hasRad)
    return absent;
  // The one check that rests on GEOMETRY rather than on guessing what the
  // caller meant: more than a full turn is a circle, not a sweep or a pinned
  // angle. A number meant as degrees trips it from 7 upward, which is every
  // realistic case, and the message names the likely cause instead of
  // building a plausible wrong part. Same defence as the arc parser's.
  if (!angles::IsPlausibleSweep(rad))
    throw std::runtime_error(
        std::string(radKey) + " is RADIANS and " + std::to_string(rad) +
        " is more than a full turn — pass " + degKey + " to say degrees");
  return rad;
}

// V2 strict inputs: the feature dialect used to IGNORE unknown fields — a
// Box with mode:"add" was accepted and the mode silently dropped (the dock
// dogfooding round's silent-failure class). Rather than maintain a per-type
// schema table, round-trip the built feature: any top-level request key that
// neither serializes back nor sits in the small allowlist cannot have been
// consumed, so reject it with the accepted vocabulary. File loading stays
// tolerant — only the RPC path is strict.
void RejectUnknownFeatureFields(const json& request, const Feature& built) {
  if (!request.is_object())
    return;
  json out;
  try {
    out = DocumentIO::FeatureToJson(built);
  } catch (...) {
    return; // unserializable type: cannot validate, stay permissive
  }
  // "type" is the FromJson dispatch key; the rest are the get_state envelope
  // (index/error state stamped AROUND the dialect), so a read-modify-write of
  // a get_state feature entry — a legitimate agent pattern — stays valid.
  static const std::set<std::string> kAllow = {
      "type", "index", "error", "warning", "after_earlier_failure", "report"};
  // The round trip alone is not enough: a key whose value is EMPTY
  // serializes back to nothing, so `"external": []` on a sketch (a filter
  // that found no edges — the SO-101 gripper's) was refused as an unknown
  // field naming a vocabulary that did not include it. The type's own
  // documented dialect settles those.
  std::set<std::string> dialect;
  for (const DocumentIO::FeatureTypeInfo& info : DocumentIO::FeatureTypes()) {
    if (built.TypeName() != info.type)
      continue;
    std::string word;
    for (const char* c = info.params; *c != '\0'; ++c) {
      if ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
          (*c >= '0' && *c <= '9') || *c == '_') {
        word += *c;
        continue;
      }
      if (!word.empty())
        dialect.insert(word);
      word.clear();
    }
    if (!word.empty())
      dialect.insert(word);
    break;
  }
  for (auto it = request.begin(); it != request.end(); ++it) {
    if (out.contains(it.key()) || kAllow.count(it.key()) != 0)
      continue;
    // Documented for this type, and it did not serialize back: an empty
    // value, or the DEFAULT, which the dialect omits so older files re-save
    // byte-identically (`keep_origin: false`, an Import's `units: "mm"`).
    // Either way it was consumed; refusing it named a vocabulary that did not
    // include it. An UNdocumented key — a typo, a field of another type — is
    // still refused below.
    if (dialect.count(it.key()) != 0)
      continue;
    // What this feature serialized is what it is SURE of; the type's own
    // row lists the rest, including every field left at its default (a Hem's
    // `kind`, `gap` and `outside` are all accepted, and none of them shows
    // up here). Naming only these as "accepted" sent a caller to delete a
    // field that works — and printed `type` twice, once as the prefix and
    // once out of the round trip.
    std::string accepted = "type";
    for (auto o = out.begin(); o != out.end(); ++o)
      if (o.key() != "type")
        accepted += ", " + o.key();
    throw std::runtime_error(
        "unknown field '" + it.key() + "' for " + built.TypeName() +
        ". This one serialized: " + accepted + " — the rest of what " +
        built.TypeName() +
        " accepts is in its row of describe {method: \"add_feature\"}.");
  }
}

// A Box, Cylinder or Sphere is a one-call convenience that stores what a
// person would have drawn: a dimensioned Sketch on what it names in `on`,
// and an Extrude (or a Revolve) of it, as ONE undo step
// (docs/modeling/README.md#primitives). The stored primitive types are not
// reachable here: an old document may hold one, nothing new makes one.
json AddPrimitive(ui::AppState& state, Document& doc, const json& fj) {
  const primitive::Spec spec = primitive::SpecFromJson(fj);
  const json preDigest = BodyDigestJson(doc);
  const primitive::Added added = primitive::Add(doc, *state.commands, spec);
  size_t index = 0;
  doc.FindFeatureById(added.solidId, &index);
  return json{{"index", index},
              {"id", added.solidId},
              {"ids", json::array({added.solidId, added.sketchId})},
              {"regen", RegenSummaryJson(doc)},
              {"digest", DigestJson(preDigest, doc)}};
}

// modify_feature cannot turn anything into a stored primitive either.
void RefusePrimitiveSpec(const json& fj) {
  if (!fj.is_object())
    return;
  const auto kind = primitive::KindFromName(fj.value("type", std::string{}));
  if (!kind)
    return;
  throw std::runtime_error(
      std::string(primitive::KindName(*kind)) +
      " is not a stored feature: add_feature {type: \"" +
      primitive::KindName(*kind) +
      "\", on, ...} stores a Sketch and an Extrude (a Sphere, a Revolve). "
      "Change one through what it stored: a dimension's value in its Sketch "
      "(modify_feature on the sketch; it re-solves), a variable when the "
      "size was given as an expression (set_variable), or the height "
      "(modify_feature on the Extrude's distance)");
}

// Error-audit round: "no such feature/sketch" names the candidates that DO
// exist, id + name, so the correction is one edit away.
std::string FeatureCandidates(Document& doc, bool sketchesOnly) {
  std::vector<std::string> known;
  for (size_t i = 0; i < doc.FeatureCount(); i++) {
    Feature* f = doc.GetFeature(i);
    if (sketchesOnly && dynamic_cast<SketchFeature*>(f) == nullptr)
      continue;
    known.push_back(std::to_string(f->GetId()) + " (" + f->GetName() + ")");
  }
  return KnownList(known);
}
} // namespace

// Modeling / feature-tree RPCs (select_feature..list_variables) carved out of
// RemoteServer::HandleSemantic. All act on the active part document.
// Sketch constraint kinds accepted by add_constraint(s), also served by the
// describe endpoint. Keep in lockstep with sketch::ConstraintKind.
static const std::map<std::string, sketch::ConstraintKind>&
ConstraintKindTable() {
  static const std::map<std::string, sketch::ConstraintKind> kinds = {
      {"coincident", sketch::ConstraintKind::Coincident},
      {"horizontal", sketch::ConstraintKind::Horizontal},
      {"vertical", sketch::ConstraintKind::Vertical},
      {"parallel", sketch::ConstraintKind::Parallel},
      {"perpendicular", sketch::ConstraintKind::Perpendicular},
      {"equal", sketch::ConstraintKind::Equal},
      {"distance", sketch::ConstraintKind::Distance},
      {"radius", sketch::ConstraintKind::Radius},
      {"angle", sketch::ConstraintKind::Angle},
      {"tangent", sketch::ConstraintKind::Tangent},
      {"point_on_line", sketch::ConstraintKind::PointOnLine},
      {"symmetric", sketch::ConstraintKind::Symmetric},
      {"midpoint", sketch::ConstraintKind::Midpoint},
      {"fixed", sketch::ConstraintKind::Fixed},
      {"concentric", sketch::ConstraintKind::Concentric},
      {"collinear", sketch::ConstraintKind::Collinear},
      {"distance_x", sketch::ConstraintKind::DistanceX},
      {"distance_y", sketch::ConstraintKind::DistanceY},
      {"diameter", sketch::ConstraintKind::Diameter},
      {"distance_pl", sketch::ConstraintKind::DistancePL}};
  return kinds;
}

std::vector<std::string> ConstraintKindNames() {
  std::vector<std::string> names;
  for (const auto& [name, kind] : ConstraintKindTable())
    names.push_back(name);
  return names;
}

std::vector<std::pair<std::string, std::string>> ConstraintValueUnits() {
  std::vector<std::pair<std::string, std::string>> out;
  for (const auto& [name, kind] : ConstraintKindTable()) {
    if (!sketch::IsDimension(kind))
      continue;
    out.emplace_back(name, kind == sketch::ConstraintKind::Angle
                               ? "radians (or value_deg for degrees; `expr` "
                                 "is in DEGREES)"
                               : "mm");
  }
  return out;
}

// describe: one row per `m ==` branch below — extend together.
static constexpr MethodDoc ModelingMethods[] = {
    {"select_feature",
     "Select a feature by index or feature_id (-1/0 clears); "
     "drives the same selection the tree uses.",
     "feature_id, index", remote::NeedsPart},
    {"accept_pending",
     "Commit the pending (preview) feature, like clicking the checkmark.", "",
     remote::NeedsPart},
    {"cancel_pending", "Discard the pending (preview) feature.", "",
     remote::NeedsPart},
    {"add_feature",
     "Append a feature (`describe` lists its JSON dialect in "
     "feature_types[], and the fields every type accepts — including "
     "`material`, whose slug is refused if unrecognised: describe's "
     "material_slugs) and regenerate; returns {index, id, "
     "entity_ids?, regen}. A Box, Cylinder or Sphere names what it is `on` "
     "(a planar face, a DatumPlane, a Sketch, a Frame, or Top/Front/Right) "
     "and is stored as a dimensioned Sketch plus an Extrude (a Sphere, a "
     "Revolve), one undo step; it also returns `ids`, the solid's first "
     "(docs/modeling/README.md#primitives). A Thread on a PRINTED part "
     "(set_print_process) "
     "also returns `print`: what the print wants it modelled at, since the "
     "basic profile it cuts prints tight (docs/tolerances/README.md).",
     "feature", remote::NeedsPart | remote::Deferrable},
    {"split_for_printing",
     "Cut a body into pieces that fit a printer's bed, each piece its own "
     "part {bed_mm: [x, y, z], body?, part?, cuts?: [{axis: x|y|z, at_mm}], "
     "prefix?, dry_run?}. `bed_mm` is the build volume in MILLIMETRES; "
     "`cuts` are axis-aligned planes in the part's coordinates (at_mm, "
     "MILLIMETRES) -- without them the body is cut into the fewest equal "
     "slabs along its longest axis whose pieces all fit. A piece fits when "
     "its box fits the bed in one of the six axis-aligned orientations "
     "(`orientation` says which of its axes lies along the bed's x, y and "
     "z, z up). If one does not fit, NOTHING is created and the refusal "
     "names it and its size. Each piece is a new part '<prefix>_<n>' "
     "(default prefix '<part>_piece'): a Derived copy of the body, a box "
     "and an intersect, so it carries the body's material and, through the "
     "boolean, its face names, and follows the source when the Derived is "
     "updated. `body` names the body when the part has several; `part` "
     "(name or id) splits another part than the active one. `dry_run` "
     "answers the plan without making parts. A body that fits whole answers "
     "fits_whole and makes nothing "
     "(docs/topopt/README.md#split-for-printing).",
     "body, bed_mm, cuts, dry_run, part, prefix",
     remote::NeedsPart | remote::NotInBatch,
     "{part, body, bed_mm, automatic, fits_whole, cuts[]{axis, at_mm}, "
     "pieces[]{index, size_mm, bbox_min, bbox_max, volume_mm3, solids, fits, "
     "material, orientation{bed_x, bed_y, up}, part?, part_id?}}"},
    {"update_derived",
     "Retake a Derived feature's snapshot from its source part (one undo "
     "step): {feature_id} or {index}, or {all: true} for every Derived of "
     "the active part whose source is loaded. Refuses a source that is not "
     "in the project or a broken link. Returns {updated: [ids], regen, "
     "digest}. Never automatic: get_state's `stale` says when to call it "
     "(docs/modeling/derived.md).",
     "all, feature_id, index", remote::NeedsPart},
    {"derive_occurrence",
     "In an in-context edit (edit_in_context): add a Derived copy of a "
     "neighbour occurrence's part, placed where the ghost is "
     "(OccMatrix(edited)^-1 * OccMatrix(neighbour)); {occurrence, name?, "
     "reference?: true, bodies?: [created_by ids], body_names?: [names]}. "
     "Returns {index, id, "
     "regen, digest}.",
     "bodies, body_names, name, occurrence, reference", remote::NeedsPart},
    {"modify_feature",
     "Change a feature to `feature` JSON; one undo step; returns {regen} "
     "(and `print` for a Thread on a printed part, as add_feature). "
     "Name it by `feature_id` (what every other call hands you) or by "
     "`index` (its position in the tree). REPLACES the whole spec rather "
     "than patching it, so `feature` must be complete and carry its `type`; "
     "read it out of get_state, change what you want and send it back. "
     "It keeps its IDENTITY: omit \"id\" INSIDE the feature and it keeps the "
     "one it has, so references to it (sketch_id, mates, drawing dimensions) "
     "keep resolving. Pass a different \"id\" there only to mean 'this is a "
     "different feature now', which deliberately dangles those references.",
     "feature, feature_id, index", remote::NeedsPart | remote::Deferrable},
    {"set_material",
     "Assign a material slug (describe's material_slugs) to a body's creating "
     "feature, by feature_id (its created_by) or by body index; '' restores "
     "default. An unrecognised slug is refused, naming the accepted set. "
     "Drives mass, BOM and FEM as well as appearance.",
     "body, feature_id, material", remote::NeedsPart | remote::Deferrable},
    {"set_appearance",
     "Visual override SEPARATE from the physical material (v54): "
     "{feature_id|body, color?: [r,g,b] LINEAR 0..1 (not sRGB: the renderer "
     "lights it as linear and STEP/GLB are handed it as linear, so an sRGB "
     "grey "
     "of 0.5 comes out near 0.73 — convert with c^2.2, e.g. 0.2 for a mid "
     "grey), roughness?: 0..1, metallic?: 0..1}. Unset fields inherit from the "
     "material; passing NO "
     "optional fields clears the override. Never affects mass/FEM/BOM, and "
     "recoloring rebuilds no geometry.",
     "body, color, feature_id, metallic, roughness", remote::NeedsPart},
    {"set_rollback",
     "Move the rollback bar: only features before it regenerate. Takes "
     "`feature_id` — the bar stops just BEFORE that feature — or `index` "
     "(features [0, index) regenerate; -1 = end).",
     "feature_id, index", remote::NeedsPart},
    {"suppress_feature",
     "Toggle suppression by feature_id; suppressed "
     "features are skipped but keep parameters.",
     "feature_id, suppressed", remote::NeedsPart | remote::Deferrable},
    {"delete_feature",
     "Delete a feature by `feature_id` (`id` is the older spelling) or "
     "`index`; one undo step; returns {regen}.",
     "feature_id, id, index", remote::NeedsPart | remote::Deferrable},
    {"project_geometry",
     "Project a body face/edge (feature_id + element) into "
     "a sketch as live locked construction geometry.",
     "element, feature_id, kind, sketch_id", remote::NeedsPart},
    {"add_reference_image",
     "Attach an image underlay to a sketch from an image file (`path`); "
     "returns image_id.",
     "at, opacity, path, sketch_id, units_per_pixel", remote::NeedsPart},
    {"set_reference_image",
     "Update an underlay's placement, opacity or visibility (`hidden`) by "
     "{sketch_id, image_id}. "
     "origin is [u, v] sketch mm, units_per_pixel is sketch mm per image "
     "pixel, and the tilt is `rotation` (RADIANS, CCW about origin) or "
     "`rotation_deg`, never both.",
     "hidden, image_id, opacity, origin, rotation, rotation_deg, sketch_id, "
     "units_per_pixel",
     remote::NeedsPart},
    {"calibrate_reference_image",
     "Scale an underlay so two picked image points span a given distance, in "
     "sketch MILLIMETRES — which is what sets its units_per_pixel.",
     "align, distance, image_id, p1, p2, sketch_id", remote::NeedsPart},
    {"delete_reference_image", "Remove an underlay by {sketch_id, image_id}.",
     "image_id, sketch_id", remote::NeedsPart},
    {"add_constraint",
     "Append one sketch constraint {sketch_id, kind, refs, value?/expr?}; "
     "kinds in constraint_kinds; solves once. `value` is MILLIMETRES for "
     "distance/radius/diameter and RADIANS for angle (or pass value_deg, "
     "never both — more than a full turn is refused as degrees). `expr` is "
     "an expression in the document's base units, where angles are DEGREES: "
     "\"45\", \"45deg\" and \"0.785rad\" all mean the same 45 degrees. "
     "Refs are [entity_id, which]: an ellipse's 0 = centre, 3/4 = "
     "major-axis ends, 5/6 = minor-axis ends (a radius is a `distance` from "
     "0 to 3 or 5); a spline's k = pole k. A constraint the solver cannot "
     "represent (radius on an ellipse, tangent to a spline away from its "
     "end) is refused naming what its kind takes.",
     "*", remote::NeedsPart},
    {"add_constraints",
     "Append MANY constraints in one solve+regen+undo step: "
     "{sketch_id, constraints: [...]}.",
     "*", remote::NeedsPart},
    {"delete_entity",
     "Delete a sketch entity by {sketch_id, entity_id}, "
     "dropping constraints that reference it.",
     "entity_id, sketch_id", remote::NeedsPart},
    {"fillet",
     "Sketch fillet: round the corner between two lines {sketch_id, "
     "line1, line2, radius (mm)}; errors explain a failed corner.",
     "line1, line2, radius, sketch_id", remote::NeedsPart},
    {"offset",
     "Offset a closed sketch chain {sketch_id, chain: [entity ids], distance "
     "(mm; the sign picks the side), join_arc?}. line1/line2/radius are "
     "`fillet`'s, which shares this branch.",
     "chain, distance, join_arc, line1, line2, radius, sketch_id",
     remote::NeedsPart},
    {"mirror",
     "Mirror sketch entities across a line {sketch_id, "
     "sources: [entity ids], axis: <line entity id>, link?}. line1/line2/"
     "radius are `fillet`'s, which shares this branch.",
     "axis, line1, line2, link, radius, sketch_id, sources", remote::NeedsPart},
    {"pattern",
     "Sketch pattern of entities: linear {step: [du, dv] mm} or circular "
     "{circular: true, center: [u, v], angle (RADIANS) or angle_deg, never "
     "both}. The circular angle is the PER-STEP angle about the centre, not "
     "the total sweep — 3 instances 45 degrees apart is angle_deg: 45. More "
     "than a full turn in `angle` is refused: it was meant as degrees. "
     "line1/line2/radius are `fillet`'s, which shares this branch.",
     "angle, angle_deg, center, circular, count, line1, line2, link, radius, "
     "sketch_id, sources, step",
     remote::NeedsPart},
    {"check_literals",
     "ADVISORY lint for numbers that should be expressions "
     "(docs/variables/README.md#literals-that-should-be-expressions); it "
     "reports and never refuses, and a finding is a suggestion, not a "
     "failure. `literals`: feature fields holding a bare number EQUAL to a "
     "variable in scope (`variables` names each, '-name' for the negation) "
     "-- lengths compared with lengths, angles and counts only with "
     "variables used as such; `value` is mm for a length, degrees for an "
     "angle. `placements`: a sketch (a primitive is one) or an explicit "
     "Frame placed in the WORLD (a base plane, an offset of one, a datum "
     "plane, a fixed frame) whose plane a planar face of the part already "
     "lies on, over that face -- `face` is what to attach it to "
     "(face_ref), so it follows the part instead of floating when it "
     "changes. `part` (name or id) lints another part than the active one. "
     "Run it after building a part from literals; extract_variables "
     "{reuse: true} binds the literal findings.",
     "part", remote::NeedsPart,
     "{part, literals[]{feature_id, feature, field, label, value, quantity, "
     "variables[]}, placements[]{feature_id, feature, on, origin, normal, "
     "face{feature_id, element}, face_made_by, face_feature}, "
     "checked{fields, placements}}"},
    {"extract_variables",
     "Turn the active part's literal dimensions into named variables with "
     "expressions (docs/variables/README.md#extracting-variables). Without "
     "`apply` it is a DRY RUN that changes nothing and answers the plan: "
     "each proposed variable {name, expression, value, quantity, uses[] -- "
     "the fields it would drive}, and every literal `left_alone` with the "
     "reason (no expression twin, a unitless count, zero, a coordinate a "
     "sketch constraint fixes, a hole diameter its fit sets). With `apply: "
     "true` it creates the variables and sets each field's expression as "
     "ONE undo step; the geometry does not move. `feature_ids` limits it "
     "to those features, `fields` to those field paths (as the dry run "
     "and check_literals name them: \"distance\", \"constraints/3\", "
     "\"entities/1/r\"); an unknown one is refused naming what exists. "
     "Names are <feature>_<field>, after `prefix`. Equal literals in "
     "DIFFERENT features share one variable only with `merge_equal: "
     "true` (equal is not the same as related); within one feature they "
     "always do. `reuse: true` binds a literal to the ONE existing "
     "variable it equals (check_literals' finding) instead. `scope` is "
     "'project' (the default) or 'part'. `value` is mm for a length, "
     "degrees for an angle.",
     "apply, feature_ids, fields, merge_equal, prefix, reuse, scope",
     remote::NeedsPart,
     "{applied, scope, variables[]{name, expression, value, quantity, "
     "existing, uses[]{feature_id, feature, field, label, expression}}, "
     "left_alone[]{feature_id, feature, field, label, value, reason}, "
     "regen, digest}"},
    // NOTE: list_variables is documented and handled in RemoteProject.cpp,
    // where the handler is. A second row here made `describe` list the
    // method TWICE — the convention is one row per `m ==` branch, and this
    // module has no such branch.
};
std::span<const MethodDoc> ModelingMethodDocs() { return ModelingMethods; }

namespace {

// FeatureFromJson reads its fields with `at()`, so a MISSING required one
// surfaces as `[json.exception.out_of_range.403] key 'direction' not found` —
// which names neither the feature type nor where the answer is written down.
// The same surface already names UNKNOWN fields well ("unknown parameter 'x'
// for 'add_drawing_view'. Accepted: ..."), and `describe` documents every
// type's params. This makes the two match.
std::unique_ptr<Feature> FeatureFromJsonNamed(const nlohmann::json& j) {
  try {
    return DocumentIO::FeatureFromJson(j);
  } catch (const nlohmann::json::out_of_range& e) {
    const std::string type = j.value("type", "");
    std::string missing = e.what();
    // "...key 'direction' not found" -> direction
    const size_t open = missing.find('\'');
    const size_t close = open == std::string::npos
                             ? std::string::npos
                             : missing.find('\'', open + 1);
    if (open != std::string::npos && close != std::string::npos)
      missing = missing.substr(open + 1, close - open - 1);
    throw std::runtime_error(
        (type.empty() ? std::string("that feature") : type) +
        " is missing the required field \"" + missing +
        "\" — describe feature_types[] lists the fields each type takes");
  }
}

} // namespace

namespace {

json LiteralFieldJson(uint64_t featureId, const std::string& feature,
                      const literals::Field& f) {
  return {{"feature_id", featureId},
          {"feature", feature},
          {"field", f.path},
          {"label", f.label},
          {"value", f.value},
          {"quantity", literals::QuantityName(f.quantity)}};
}

// check_literals
// (docs/variables/README.md#literals-that-should-be-expressions).
json CheckLiterals(ui::AppState& state, Document& active, const RpcParams& p) {
  size_t slot = static_cast<size_t>(state.activePart);
  if (p.contains("part"))
    slot = ResolveSlotIndex(state, p.at("part"));
  if (slot >= state.parts.size() || state.parts[slot].kind != ui::DocKind::Part)
    throw std::runtime_error("check_literals: `part` is not a part");
  const Document* doc = slot == static_cast<size_t>(state.activePart)
                            ? &active
                            : ui::PartDocById(state, state.parts[slot].id);
  if (doc == nullptr)
    throw std::runtime_error("check_literals: part '" + state.parts[slot].name +
                             "' is not loaded");
  const literals::Report report = ui::LintDocument(state, *doc);
  json lits = json::array();
  for (const literals::LiteralFinding& f : report.literals) {
    json j = LiteralFieldJson(f.featureId, f.feature, f.field);
    j["variables"] = f.variables;
    lits.push_back(std::move(j));
  }
  json places = json::array();
  for (const literals::PlacementFinding& f : report.placements)
    places.push_back(
        {{"feature_id", f.featureId},
         {"feature", f.feature},
         {"on", f.on},
         {"origin", {f.origin.x, f.origin.y, f.origin.z}},
         {"normal", {f.normal.x, f.normal.y, f.normal.z}},
         {"face", {{"feature_id", f.face.featureId}, {"element", f.face.name}}},
         {"face_made_by", f.faceMadeBy},
         {"face_feature", f.faceFeature}});
  return {{"part", state.parts[slot].name},
          {"literals", std::move(lits)},
          {"placements", std::move(places)},
          {"checked",
           {{"fields", report.fieldsChecked},
            {"placements", report.placementsChecked}}}};
}

// extract_variables (docs/variables/README.md#extracting-variables).
json ExtractVariables(ui::AppState& state, Document& doc, const RpcParams& p) {
  literals::ExtractOptions options;
  if (p.contains("feature_ids")) {
    const json& ids = p.at("feature_ids");
    if (!ids.is_array())
      throw std::runtime_error("feature_ids is an array of feature ids");
    for (const json& id : ids)
      options.featureIds.push_back(id.get<uint64_t>());
  }
  if (p.contains("fields")) {
    const json& fields = p.at("fields");
    if (!fields.is_array())
      throw std::runtime_error("fields is an array of field paths "
                               "(\"distance\", \"constraints/3\")");
    for (const json& f : fields)
      options.paths.push_back(f.get<std::string>());
  }
  options.prefix = p.value("prefix", std::string{});
  options.mergeEqual = p.value("merge_equal", false);
  options.reuse = p.value("reuse", false);
  const std::string scope = p.value("scope", std::string("project"));
  if (scope != "project" && scope != "part")
    throw std::runtime_error("scope is 'project' (the default) or 'part', "
                             "not '" +
                             scope + "'");
  const bool apply = p.value("apply", false);

  const literals::ExtractPlan plan =
      ui::PlanExtractVariables(state, std::move(options));
  json vars = json::array();
  for (const literals::Proposal& pr : plan.proposals) {
    json uses = json::array();
    for (const literals::Use& u : pr.uses) {
      json j = LiteralFieldJson(u.featureId, u.feature, u.field);
      j.erase("value");
      j.erase("quantity");
      j["expression"] = u.expression;
      uses.push_back(std::move(j));
    }
    vars.push_back({{"name", pr.name},
                    {"expression", pr.expression},
                    {"value", pr.value},
                    {"quantity", literals::QuantityName(pr.quantity)},
                    {"existing", pr.existing},
                    {"uses", std::move(uses)}});
  }
  json left = json::array();
  for (const literals::LeftAlone& l : plan.leftAlone) {
    json j = {{"feature_id", l.featureId},
              {"feature", l.feature},
              {"field", l.path},
              {"label", l.label},
              {"reason", l.reason}};
    if (l.hasValue)
      j["value"] = l.value;
    left.push_back(std::move(j));
  }
  json out = {{"applied", false},
              {"scope", scope},
              {"variables", std::move(vars)},
              {"left_alone", std::move(left)}};
  if (!apply)
    return out;
  const json preDigest = BodyDigestJson(doc);
  if (const std::string why =
          ui::ApplyExtractVariables(state, plan, scope == "project");
      !why.empty())
    throw std::runtime_error("extract_variables: " + why);
  out["applied"] = !plan.proposals.empty();
  out["regen"] = RegenSummaryJson(doc);
  out["digest"] = DigestJson(preDigest, doc);
  return out;
}

// split_for_printing (docs/topopt/README.md#split-for-printing).
json SplitForPrinting(ui::AppState& state, Document& active,
                      const RpcParams& p) {
  // The source part: the active one, or one named.
  size_t srcSlot = state.activePart;
  if (p.contains("part"))
    srcSlot = ResolveSlotIndex(state, p.at("part"));
  if (srcSlot >= state.parts.size() ||
      state.parts[srcSlot].kind != ui::DocKind::Part)
    throw std::runtime_error("split_for_printing: `part` is not a part");
  const uint64_t srcId = state.parts[srcSlot].id;
  const std::string srcName = state.parts[srcSlot].name;
  const Document* src =
      srcSlot == state.activePart ? &active : ui::PartDocById(state, srcId);
  if (src == nullptr)
    throw std::runtime_error("split_for_printing: part '" + srcName +
                             "' is not loaded");

  // The body: by name, or the part's only one.
  std::vector<const Body*> solids;
  for (const Body& b : src->GetBodies())
    if (!b.reference)
      solids.push_back(&b);
  const Body* body = nullptr;
  std::string names;
  for (const Body* b : solids)
    names += (names.empty() ? "'" : ", '") + b->name + "'";
  if (p.contains("body")) {
    const std::string want = p.at("body").get<std::string>();
    for (const Body* b : solids)
      if (b->name == want)
        body = b;
    if (body == nullptr)
      throw std::runtime_error(
          "split_for_printing: part '" + srcName + "' has no body '" + want +
          "' (it has " + (names.empty() ? std::string("none") : names) + ")");
  } else if (solids.size() == 1) {
    body = solids.front();
  } else {
    throw std::runtime_error(
        "split_for_printing: part '" + srcName + "' has " +
        std::to_string(solids.size()) + " bodies; name one with `body` (" +
        (names.empty() ? std::string("none") : names) + ")");
  }

  const json& bj = p.at("bed_mm");
  if (!bj.is_array() || bj.size() != 3)
    throw std::runtime_error("bed_mm is the bed's [x, y, z] in millimetres");
  const glm::dvec3 bed(bj.at(0).get<double>(), bj.at(1).get<double>(),
                       bj.at(2).get<double>());
  std::string err;
  std::optional<printsplit::Plan> plan;
  if (p.contains("cuts")) {
    std::vector<printsplit::Cut> cuts;
    for (const json& cj : p.at("cuts")) {
      if (!cj.is_object() || !cj.contains("axis") || !cj.contains("at_mm"))
        throw std::runtime_error("each cut is {axis: x|y|z, at_mm}");
      const std::string a = cj.at("axis").get<std::string>();
      if (a != "x" && a != "y" && a != "z")
        throw std::runtime_error("a cut's axis is x, y or z, not '" + a +
                                 "' (cuts are axis-aligned)");
      cuts.push_back({a == "x"   ? 0
                      : a == "y" ? 1
                                 : 2,
                      cj.at("at_mm").get<double>()});
    }
    if (cuts.empty())
      throw std::runtime_error("cuts is empty; leave it out to have the cuts "
                               "chosen along the longest axis");
    plan = printsplit::PlanCuts(body->shape.GetShape(), bed, cuts, &err);
  } else {
    plan = printsplit::PlanAutomatic(body->shape.GetShape(), bed, &err);
  }
  if (!plan)
    throw std::runtime_error("split_for_printing: " + err);
  for (size_t i = 0; i < plan->pieces.size(); ++i) {
    const printsplit::Piece& pc = plan->pieces[i];
    if (!pc.fits) {
      const glm::dvec3 s = pc.Size();
      char buf[240];
      std::snprintf(buf, sizeof(buf),
                    "split_for_printing: piece %zu (%.1f x %.1f x %.1f mm) "
                    "does not fit the %.1f x %.1f x %.1f mm bed in any "
                    "orientation; nothing was created -- add a cut through it",
                    i + 1, s.x, s.y, s.z, bed.x, bed.y, bed.z);
      throw std::runtime_error(buf);
    }
  }

  const std::string prefix = p.value("prefix", srcName + "_piece");
  const bool dryRun = p.value("dry_run", false) || plan->fitsWhole;
  const auto vec = [](const glm::dvec3& v) {
    return json::array({v.x, v.y, v.z});
  };
  static const char* kAxis[3] = {"x", "y", "z"};
  json pieces = json::array();
  std::vector<std::string> partNames;
  for (size_t i = 0; i < plan->pieces.size(); ++i) {
    const printsplit::Piece& pc = plan->pieces[i];
    partNames.push_back(prefix + "_" + std::to_string(i + 1));
    pieces.push_back({{"index", i},
                      {"size_mm", vec(pc.Size())},
                      {"bbox_min", vec(pc.min)},
                      {"bbox_max", vec(pc.max)},
                      {"volume_mm3", pc.volume},
                      {"solids", pc.solids},
                      {"fits", pc.fits},
                      {"material", body->material},
                      {"orientation",
                       {{"bed_x", kAxis[pc.orientation[0]]},
                        {"bed_y", kAxis[pc.orientation[1]]},
                        {"up", kAxis[pc.orientation[2]]}}}});
  }
  json cuts = json::array();
  for (const printsplit::Cut& c : plan->cuts)
    cuts.push_back({{"axis", kAxis[c.axis]}, {"at_mm", c.at}});
  json out{{"part", srcName},
           {"body", body->name},
           {"bed_mm", vec(bed)},
           {"automatic", plan->automatic},
           {"fits_whole", plan->fitsWhole},
           {"cuts", std::move(cuts)},
           {"pieces", std::move(pieces)}};
  if (dryRun)
    return out;

  // Every name is checked before anything is made.
  for (const std::string& n : partNames)
    RequireFreshTabName(state, n);
  const std::string bodyName = body->name;
  const std::string material = body->material;
  const std::vector<printsplit::Piece> planned = plan->pieces;
  const uint64_t wasActive = state.parts[state.activePart].id;
  const auto add = [&](json fj) {
    auto feature = FeatureFromJsonNamed(fj);
    if (auto* d = dynamic_cast<DerivedFeature*>(feature.get())) {
      ui::DerivedSource from;
      from.slotId = d->sourcePart;
      if (const std::string why = ui::PointDerivedAt(state, *d, from);
          !why.empty())
        throw std::runtime_error("split_for_printing: Derived: " + why);
    }
    state.commands->Execute(
        std::make_unique<AddFeatureCommand>(std::move(feature)),
        *state.document);
    const Feature* added =
        state.document->GetFeature(state.document->FeatureCount() - 1);
    if (!added->GetError().empty())
      throw std::runtime_error("split_for_printing: " + added->TypeName() +
                               " '" + added->GetName() +
                               "': " + added->GetError());
    return added->GetId();
  };
  for (size_t i = 0; i < planned.size(); ++i) {
    const printsplit::Piece& pc = planned[i];
    const size_t slot = ui::AddPart(state, partNames[i]);
    out["pieces"][i]["part"] = partNames[i];
    out["pieces"][i]["part_id"] = state.parts[slot].id;
    const uint64_t derived = add({{"type", "Derived"},
                                  {"name", bodyName},
                                  {"source_part", srcId},
                                  {"body_names", {bodyName}},
                                  {"reference", false}});
    const glm::dvec3 size = pc.cellMax - pc.cellMin;
    const uint64_t box = add({{"type", "Box"},
                              {"name", "Piece cell"},
                              {"position", vec(pc.cellMin)},
                              {"width", size.x},
                              {"height", size.y},
                              {"depth", size.z}});
    json boolean = {{"type", "Boolean"},
                    {"name", "Piece " + std::to_string(i + 1)},
                    {"op", "intersect"},
                    {"target_id", derived},
                    {"tool_id", box}};
    if (!material.empty())
      boolean["material"] = material;
    add(boolean);
  }
  // Back to the tab that was active.
  for (size_t i = 0; i < state.parts.size(); ++i)
    if (state.parts[i].id == wasActive) {
      ui::SetActivePart(state, i);
      break;
    }
  return out;
}

} // namespace

std::optional<json> HandleModelingRpc(ui::AppState& state, Document& document,
                                      const std::string& m,
                                      const RpcParams& p) {
  Document* doc = &document;
  if (m == "split_for_printing") {
    return SplitForPrinting(state, document, p);
  }
  if (m == "check_literals") {
    return CheckLiterals(state, document, p);
  }
  if (m == "extract_variables") {
    return ExtractVariables(state, document, p);
  }
  if (m == "select_feature") {
    // Select a feature by index or feature_id (-1 / 0 clears). Drives the
    // same Selection the feature tree sets, so Properties shows/edits it.
    if (p.contains("feature_id")) {
      const uint64_t id = p.at("feature_id").get<uint64_t>();
      if (id == 0) {
        state.selection.Clear();
      } else if (doc->FindFeatureById(id) == nullptr) {
        throw std::runtime_error("no such feature id");
      } else {
        ui::SelectFeature(state, id);
      }
      return json::object();
    }
    int index = p.value("index", -1);
    if (index >= 0 && index >= static_cast<int>(doc->FeatureCount()))
      throw std::runtime_error("feature index out of range");
    if (index < 0)
      state.selection.Clear();
    else
      ui::SelectFeatureByIndex(state, static_cast<size_t>(index));
    return json::object();
  }
  if (m == "accept_pending") {
    ui::AcceptPendingFeature(state);
    return json::object();
  }
  if (m == "cancel_pending") {
    ui::CancelPendingFeature(state);
    return json::object();
  }
  if (m == "add_feature") {
    json fj = p.at("feature");
    if (fj.is_object() &&
        primitive::KindFromName(fj.value("type", std::string{})))
      return AddPrimitive(state, *doc, fj);
    // A Derived's configuration may be NAMED ("pawn"); the feature stores
    // the id, which survives a rename. An unknown name is refused with the
    // ones the source has, never read as "whichever is active".
    if (fj.is_object() && fj.value("type", std::string{}) == "Derived" &&
        fj.contains("configuration") && fj.at("configuration").is_string()) {
      const std::string want = fj.at("configuration").get<std::string>();
      const Document* src =
          ui::PartDocById(state, fj.value("source_part", uint64_t{0}));
      if (src == nullptr)
        throw std::runtime_error("Derived: configuration '" + want +
                                 "' names a configuration of source_part, "
                                 "which is not a part of this project");
      uint64_t id = 0;
      std::string known;
      for (const Configuration& c : src->GetConfigurations()) {
        if (c.name == want)
          id = c.id;
        known += (known.empty() ? "" : ", ") + c.name;
      }
      if (id == 0)
        throw std::runtime_error(
            "Derived: the source part has no configuration '" + want +
            "' (it has: " + (known.empty() ? std::string("none") : known) +
            ")");
      fj["configuration"] = id;
    }
    auto feature = FeatureFromJsonNamed(fj);
    RejectUnknownFeatureFields(fj, *feature);
    // The GUI refuses a tool with missing inputs; the script API used to
    // accept one and commit a feature that does nothing (a fillet with no
    // edges regenerates happily). Same rule, both surfaces.
    // A Derived arrives without its snapshot (the request names a source;
    // the copy is taken here, through the project): refuses a part that is
    // not loaded, the part itself, and a cycle — in words naming the parts
    // that ARE there (docs/modeling/derived.md).
    if (auto* d = dynamic_cast<DerivedFeature*>(feature.get())) {
      ui::DerivedSource src;
      src.slotId = d->sourcePart;
      if (const std::string why = ui::PointDerivedAt(state, *d, src);
          !why.empty())
        throw std::runtime_error("Derived: " + why);
    }
    if (const std::string why = feature->WhyIncomplete(); !why.empty()) {
      throw std::runtime_error(feature->TypeName() + " is incomplete: " + why);
    }
    const json preDigest = BodyDigestJson(*doc);
    state.commands->Execute(
        std::make_unique<AddFeatureCommand>(std::move(feature)), *doc);
    const size_t index = doc->FeatureCount() - 1;
    // Return the id the document actually assigned, so callers reference it
    // (a downstream extrude names its sketch by id) without guessing.
    json resp = {{"index", index}, {"id", doc->GetFeature(index)->GetId()}};
    // For sketches, also return the stable entity ids assigned on load (clients
    // send entities without ids and reference these in add_constraint).
    if (auto* sk = dynamic_cast<const SketchFeature*>(doc->GetFeature(index))) {
      json ids = json::array();
      for (const auto& e : sk->entities)
        ids.push_back(sketch::EntityId(e));
      resp["entity_ids"] = ids;
    }
    // An import is CENTRED unless it was told not to be. Say by how much, in
    // the call that did it: a caller who baked file coordinates on purpose
    // otherwise discovers the move by measuring, and reads the result as an
    // interference rather than a placement.
    if (auto* imp = dynamic_cast<const ImportFeature*>(doc->GetFeature(index));
        imp != nullptr && imp->Shift() != glm::dvec3(0.0))
      resp["recentred_by"] =
          json::array({imp->Shift().x, imp->Shift().y, imp->Shift().z});
    // A worm or a wheel says what it built, in the call that built it.
    if (auto* w = dynamic_cast<const WormFeature*>(doc->GetFeature(index)))
      if (json rep = w->ReportJson(); !rep.is_null())
        resp["report"] = rep;
    if (auto* w = dynamic_cast<const WormWheelFeature*>(doc->GetFeature(index)))
      if (json rep = w->ReportJson(); !rep.is_null())
        resp["report"] = rep;
    // A Thread on a PRINTED part says, in the call that made it, what the
    // print wants it modelled at (docs/tolerances/README.md#printed-parts) —
    // the basic profile it cuts prints tight.
    if (auto* th = dynamic_cast<const ThreadFeature*>(doc->GetFeature(index)))
      if (const auto r = ui::PrintedThreadFor(*doc, *th))
        resp["print"] = PrintedThreadJson(*r);
    resp["regen"] = RegenSummaryJson(*doc);
    resp["digest"] = DigestJson(preDigest, *doc);
    return resp;
  }
  if (m == "update_derived") {
    const json preDigest = BodyDigestJson(*doc);
    json updated = json::array();
    if (p.value("all", false)) {
      std::vector<uint64_t> before;
      for (const auto& f : doc->GetFeatures())
        if (auto* d = dynamic_cast<const DerivedFeature*>(f.get());
            d && d->sourcePart != 0)
          before.push_back(d->GetId());
      std::string why;
      const size_t n = ui::UpdateAllDerived(state, &why);
      if (n == 0 && !why.empty())
        throw std::runtime_error(why);
      for (uint64_t id : before)
        updated.push_back(id);
      json resp = {{"updated", std::move(updated)},
                   {"regen", RegenSummaryJson(*doc)},
                   {"digest", DigestJson(preDigest, *doc)}};
      if (!why.empty())
        resp["skipped"] = why;
      return resp;
    }
    uint64_t id = 0;
    if (p.contains("feature_id")) {
      id = p.at("feature_id").get<uint64_t>();
    } else if (p.contains("index")) {
      const size_t index = p.at("index").get<size_t>();
      Feature* f = doc->GetFeature(index);
      if (!f)
        throw std::runtime_error(IndexError(
            "feature", index, doc->FeatureCount(), "get_state lists features"));
      id = f->GetId();
    } else {
      throw std::runtime_error(
          "pass {feature_id} or {index} of a Derived feature, or {all: true}");
    }
    if (const std::string why = ui::UpdateDerived(state, id); !why.empty())
      throw std::runtime_error(why);
    updated.push_back(id);
    return json{{"updated", std::move(updated)},
                {"regen", RegenSummaryJson(*doc)},
                {"digest", DigestJson(preDigest, *doc)}};
  }
  if (m == "derive_occurrence") {
    if (!state.inContext.assemblySlotId)
      throw std::runtime_error(
          "derive_occurrence works during an in-context edit — open the "
          "assembly and call edit_in_context {occurrence} first; from a plain "
          "part tab use add_feature {type: Derived, source_part}");
    const uint64_t occ = p.at("occurrence").get<uint64_t>();
    const ui::DerivedSource* pick = nullptr;
    std::string neighbours;
    const std::vector<ui::DerivedSource> candidates =
        ui::DerivedSourceCandidates(state);
    for (const ui::DerivedSource& c : candidates) {
      if (c.occurrenceId == 0)
        continue;
      neighbours += (neighbours.empty() ? "" : ", ") + c.name + " (" +
                    std::to_string(c.occurrenceId) + ")";
      if (c.occurrenceId == occ && pick == nullptr)
        pick = &c;
    }
    if (pick == nullptr)
      throw std::runtime_error("occurrence " + std::to_string(occ) +
                               " is not a neighbour of the part being edited" +
                               (neighbours.empty()
                                    ? " (there are none)"
                                    : "; the neighbours are: " + neighbours));
    auto d = std::make_unique<DerivedFeature>(
        p.value("name", pick->name + " (derived)"), 0);
    d->reference = p.value("reference", true);
    if (p.contains("bodies"))
      d->bodies = p.at("bodies").get<std::vector<uint64_t>>();
    if (p.contains("body_names"))
      d->bodyNames = p.at("body_names").get<std::vector<std::string>>();
    if (const std::string why = ui::PointDerivedAt(state, *d, *pick);
        !why.empty())
      throw std::runtime_error("Derived: " + why);
    const json preDigest = BodyDigestJson(*doc);
    state.commands->Execute(std::make_unique<AddFeatureCommand>(std::move(d)),
                            *doc);
    const size_t index = doc->FeatureCount() - 1;
    return json{{"index", index},
                {"id", doc->GetFeature(index)->GetId()},
                {"regen", RegenSummaryJson(*doc)},
                {"digest", DigestJson(preDigest, *doc)}};
  }
  if (m == "modify_feature") {
    // `feature_id` as well as `index`, because an id is what everything else
    // hands you and turning one into a position cost a get_state round trip.
    // NOT `id`: `feature.id` inside the spec already means the feature's own
    // IDENTITY, and two readings of one word beside each other would be worse
    // than the round trip
    // (generators/README.md#single-cylinder-and-air-cooled-engines).
    size_t index = 0;
    if (p.contains("feature_id")) {
      const uint64_t fid = p.at("feature_id").get<uint64_t>();
      bool found = false;
      for (size_t i = 0; i < doc->FeatureCount(); i++)
        if (doc->GetFeature(i)->GetId() == fid) {
          index = i;
          found = true;
          break;
        }
      if (!found)
        throw std::runtime_error(
            "no feature with id " + std::to_string(fid) +
            " — get_state lists every feature with its id and index");
    } else if (p.contains("index")) {
      index = p.at("index").get<size_t>();
    } else {
      throw std::runtime_error(
          "modify_feature needs the feature to change: `feature_id` (what "
          "every other call hands you) or `index` (its position in the tree)");
    }
    Feature* feature = doc->GetFeature(index);
    if (!feature)
      throw std::runtime_error(IndexError("feature", index, doc->FeatureCount(),
                                          "get_state lists features with "
                                          "their indices"));
    // A REPLACEMENT, not a patch, and it has to say so itself. A partial
    // feature — {"depth": 20} — used to reach FeatureFromJson and come back
    // as `[json.exception.out_of_range.403] key 'type' not found`, which is
    // an nlohmann internal rather than a refusal that names what is wanted.
    if (!p.at("feature").is_object() || !p.at("feature").contains("type"))
      throw std::runtime_error(
          "modify_feature REPLACES the whole feature, so `feature` must be a "
          "complete spec including its `type` — not just the fields you want "
          "changed. Read the feature out of get_state, change what you want "
          "and send it back; describe feature_types[] lists the fields each "
          "type takes.");
    RefusePrimitiveSpec(p.at("feature"));
    // Build a throwaway from the request first: validates the JSON AND lets
    // the round-trip unknown-field check run before anything is committed.
    RejectUnknownFeatureFields(p.at("feature"),
                               *DocumentIO::FeatureFromJson(p.at("feature")));
    json before = DocumentIO::FeatureToJson(*feature);
    json after = p.at("feature");
    // A sketch whose dimension VALUE changed is re-solved here, so the
    // geometry follows the number. A sketch with literal dimensions is not
    // re-solved at regeneration (only expression-driven ones are), so a
    // changed `value` used to be stored beside geometry that ignored it --
    // and a primitive's size is exactly such a dimension
    // (docs/modeling/README.md#primitives).
    if (const auto* oldSk = dynamic_cast<const SketchFeature*>(feature)) {
      auto built = DocumentIO::FeatureFromJson(after);
      auto* newSk = dynamic_cast<SketchFeature*>(built.get());
      bool changed = false;
      if (newSk != nullptr &&
          newSk->constraints.size() == oldSk->constraints.size())
        for (size_t c = 0; c < newSk->constraints.size(); c++)
          changed =
              changed ||
              (sketch::IsDimension(newSk->constraints[c].kind) &&
               newSk->constraints[c].value != oldSk->constraints[c].value);
      if (changed) {
        newSk->plane = oldSk->plane; // the resolved plane, for the solve
        sketch::Solver solver;
        solver.Load(*newSk);
        const sketch::SolveResult r = solver.Solve();
        if (!r.ok)
          throw std::runtime_error(
              "the sketch does not solve with those dimension values (the "
              "dimensions conflict or cannot be met); the document is "
              "unchanged");
        solver.WriteBack(newSk->entities);
        const json solved = DocumentIO::FeatureToJson(*newSk);
        after["entities"] = solved.at("entities");
      }
    }
    const json preDigest = BodyDigestJson(*doc);
    if (!state.commands->Execute(
            std::make_unique<ModifyFeatureCommand>(index, before, after),
            *doc)) {
      throw std::runtime_error(
          "modify failed — the replacement feature did not apply and the "
          "document is unchanged (its regeneration failed where the original "
          "succeeded). Check the JSON against describe feature_types[].params");
    }
    json resp{{"regen", RegenSummaryJson(*doc)},
              {"digest", DigestJson(preDigest, *doc)}};
    if (auto* th = dynamic_cast<const ThreadFeature*>(doc->GetFeature(index)))
      if (const auto r = ui::PrintedThreadFor(*doc, *th))
        resp["print"] = PrintedThreadJson(*r);
    return resp;
  }
  if (m == "set_material") {
    // Assign a material to a body-creating feature — by feature_id (the
    // body's created_by) or by body index (get_state order). params:
    // {feature_id? | body?, material} where material is a library slug
    // (e.g. "steel") or "" for default. Commits one undoable edit and
    // regenerates.
    const bool byId = p.contains("feature_id");
    const bool byBody = p.contains("body");
    if (byId == byBody) // neither, or both
      throw std::runtime_error(
          "set_material needs exactly one of feature_id (the body's "
          "created_by from get_state/list_elements) or body (an index into "
          "get_state's bodies)");
    size_t index = 0;
    Feature* f = nullptr;
    if (byId) {
      f = doc->FindFeatureById(p.at("feature_id").get<uint64_t>(), &index);
      if (!f)
        throw std::runtime_error(
            "no feature with id " +
            std::to_string(p.at("feature_id").get<uint64_t>()) +
            " (must be a body's created_by — get_state lists bodies). "
            "Features here: " +
            FeatureCandidates(*doc, false));
    } else {
      const auto& bodies = doc->GetBodies();
      const int body = p.at("body").get<int>();
      if (body < 0 || body >= static_cast<int>(bodies.size()))
        throw std::runtime_error(
            IndexError("body", body, bodies.size(), "get_state lists bodies"));
      // Add/Cut keep the original creator, so this recolors everything
      // fused into that body.
      f = doc->FindFeatureById(bodies[body].createdBy, &index);
      if (!f)
        throw std::runtime_error("body has no creating feature");
    }
    // An unrecognised slug is refused by SetMaterial below, naming what IS
    // accepted — the same funnel add_feature and document load go through.
    // This used to sniff for `== DEFAULT` here, which caught it on this one
    // path and named nothing.
    const std::string slug = p.value("material", std::string{});
    json before = DocumentIO::FeatureToJson(*f);
    f->SetMaterial(slug);
    doc->Regenerate();
    json after = DocumentIO::FeatureToJson(*f);
    state.commands->Execute(
        std::make_unique<ModifyFeatureCommand>(index, before, after), *doc);
    // A material change touches PBR/albedo but not geometry, so the body meshes
    // are unchanged and SyncDocumentRenderers would skip the re-push. Force a
    // resync so the shaded body recolors immediately (no other edit needed).
    state.syncedDocumentVersion = UINT64_MAX;
    return json{{"regen", RegenSummaryJson(*doc)}};
  }
  if (m == "set_appearance") {
    // V-appearance: a visual override on a body's creating feature, separate
    // from the physical material
    // (docs/rendering/materials.md#appearance-override). Same addressing as
    // set_material: {feature_id | body}. Optional fields color [r,g,b] (LINEAR
    // 0..1), roughness, metallic; passing none clears the override. Appearance
    // is stripped from the regen chain hash, so this never rebuilds geometry —
    // just restamps and resyncs.
    const bool byId = p.contains("feature_id");
    const bool byBody = p.contains("body");
    if (byId == byBody)
      throw std::runtime_error(
          "set_appearance needs exactly one of feature_id (the body's "
          "created_by) or body (an index into get_state's bodies)");
    size_t index = 0;
    Feature* f = nullptr;
    if (byId) {
      f = doc->FindFeatureById(p.at("feature_id").get<uint64_t>(), &index);
      if (!f)
        throw std::runtime_error(
            "no feature with id " +
            std::to_string(p.at("feature_id").get<uint64_t>()) +
            ". Features here: " + FeatureCandidates(*doc, false));
    } else {
      const auto& bodies = doc->GetBodies();
      const int body = p.at("body").get<int>();
      if (body < 0 || body >= static_cast<int>(bodies.size()))
        throw std::runtime_error(
            IndexError("body", body, bodies.size(), "get_state lists bodies"));
      f = doc->FindFeatureById(bodies[body].createdBy, &index);
      if (!f)
        throw std::runtime_error("body has no creating feature");
    }
    Appearance ap;
    const auto unit = [&](const char* field) {
      const double v = p.at(field).get<double>();
      if (v < 0.0 || v > 1.0)
        throw std::runtime_error(std::string(field) + " is " +
                                 std::to_string(v) +
                                 " — appearance fields are 0..1 (linear / "
                                 "PBR fractions), not 0..255");
      return static_cast<float>(v);
    };
    if (p.contains("color")) {
      const json& c = p.at("color");
      const glm::vec3 col(c.at(0).get<float>(), c.at(1).get<float>(),
                          c.at(2).get<float>());
      if (col.x < 0 || col.x > 1 || col.y < 0 || col.y > 1 || col.z < 0 ||
          col.z > 1)
        throw std::runtime_error(
            "color components are LINEAR 0..1, not 0..255 — got [" +
            std::to_string(col.x) + ", " + std::to_string(col.y) + ", " +
            std::to_string(col.z) + "]");
      ap.color = col;
    }
    if (p.contains("roughness"))
      ap.roughness = unit("roughness");
    if (p.contains("metallic"))
      ap.metallic = unit("metallic");
    json before = DocumentIO::FeatureToJson(*f);
    f->SetAppearance(ap);
    doc->Regenerate(); // full cache hit (hash-stripped): restamps only
    json after = DocumentIO::FeatureToJson(*f);
    state.commands->Execute(
        std::make_unique<ModifyFeatureCommand>(index, before, after), *doc);
    // Colors changed but no meshes did — force the renderer resync (the
    // same nudge set_material needs).
    state.syncedDocumentVersion = UINT64_MAX;
    json resp = {{"regen", RegenSummaryJson(*doc)}};
    resp["cleared"] = !ap.Any();
    return resp;
  }
  if (m == "set_rollback") {
    // Move the rollback bar: regeneration executes features [0, index) only;
    // -1 (or >= count) = end. Not undoable — it is a view of the history,
    // not an edit of it. params: {feature_id | index}
    //
    // `feature_id` because that is how every other method here names a
    // feature, and because an INDEX is the one thing a caller holding a
    // feature does not have — it has to count the tree to roll back to the
    // thing it just made.
    int stop = -1;
    if (p.contains("feature_id")) {
      size_t index = 0;
      const uint64_t id = p.at("feature_id").get<uint64_t>();
      if (doc->FindFeatureById(id, &index) == nullptr)
        throw std::runtime_error("no feature with id " + std::to_string(id) +
                                 " (get_state lists features with their ids)");
      stop = static_cast<int>(index); // the bar stops just BEFORE it
    } else {
      stop = p.at("index").get<int>();
    }
    doc->SetRollback(stop);
    return json{{"rollback", doc->Rollback()},
                {"regen", RegenSummaryJson(*doc)}};
  }
  if (m == "suppress_feature") {
    // Toggle suppression (by feature id) as one undoable edit; a suppressed
    // feature is skipped by regeneration but keeps its parameters. params:
    // {feature_id, suppressed}
    size_t index = 0;
    Feature* f =
        doc->FindFeatureById(p.at("feature_id").get<uint64_t>(), &index);
    if (!f)
      throw std::runtime_error(
          "no feature with id " +
          std::to_string(p.at("feature_id").get<uint64_t>()) +
          ". Features here: " + FeatureCandidates(*doc, false));
    json before = DocumentIO::FeatureToJson(*f);
    const json preDigest = BodyDigestJson(*doc);
    f->SetSuppressed(p.at("suppressed").get<bool>());
    doc->Regenerate();
    json after = DocumentIO::FeatureToJson(*f);
    state.commands->Execute(
        std::make_unique<ModifyFeatureCommand>(index, before, after), *doc);
    return json{{"regen", RegenSummaryJson(*doc)},
                {"digest", DigestJson(preDigest, *doc)}};
  }
  if (m == "delete_feature") {
    const json preDigest = BodyDigestJson(*doc);
    // `id` as well as `index`: every other verb that names a feature takes
    // an id, and get_state reports both, so requiring the index here was a
    // detour every caller made once.
    // `feature_id` as well, because that is what every OTHER method naming a
    // feature takes — suppress_feature, select_feature, modify_feature,
    // update_derived, set_material, set_appearance, frame. This was the one
    // out of step, and a caller who had just written suppress_feature had to
    // learn it twice.
    size_t index = 0;
    const char* idKey = p.contains("feature_id") ? "feature_id"
                        : p.contains("id")       ? "id"
                                                 : nullptr;
    if (idKey != nullptr) {
      const uint64_t id = p.at(idKey).get<uint64_t>();
      if (doc->FindFeatureById(id, &index) == nullptr)
        throw std::runtime_error("no feature with id " + std::to_string(id) +
                                 " (get_state lists features with their ids)");
    } else if (p.contains("index")) {
      index = p.at("index").get<size_t>();
    } else {
      throw std::runtime_error("delete_feature needs `feature_id` (or `id`) "
                               "or `index` (get_state lists features with "
                               "both)");
    }
    if (!state.commands->Execute(std::make_unique<RemoveFeatureCommand>(index),
                                 *doc)) {
      throw std::runtime_error(IndexError("feature", index, doc->FeatureCount(),
                                          "get_state lists features with "
                                          "their indices"));
    }
    return json{{"regen", RegenSummaryJson(*doc)},
                {"digest", DigestJson(preDigest, *doc)}};
  }
  if (m == "project_geometry") {
    // Add LIVE external (projected) geometry to a sketch: a face or edge of a
    // body earlier in the history, projected onto the sketch plane as locked
    // construction geometry that tracks the source when it moves. params:
    // {sketch_id, feature_id, element, kind?: "face"|"edge"}
    size_t index = 0;
    Feature* f =
        doc->FindFeatureById(p.at("sketch_id").get<uint64_t>(), &index);
    auto* sk = dynamic_cast<SketchFeature*>(f);
    if (!sk)
      throw std::runtime_error(
          "no sketch with id " +
          std::to_string(p.at("sketch_id").get<uint64_t>()) +
          ". Sketches here: " + FeatureCandidates(*doc, true) +
          " (ids come from add_feature results / get_state)");
    json before = DocumentIO::FeatureToJson(*sk);
    const sketch::ExternalKind kind =
        p.value("kind", std::string("face")) == "edge"
            ? sketch::ExternalKind::Edge
            : sketch::ExternalKind::Face;
    const size_t ei =
        sk->AddExternalGeometry(ElementRef{p.at("feature_id").get<uint64_t>(),
                                           p.at("element").get<std::string>()},
                                kind);
    doc->Regenerate();
    if (!sk->GetError().empty()) { // the ref did not resolve — roll back
      const std::string err = sk->GetError();
      sk->RemoveExternalGeometry(ei);
      doc->Regenerate();
      throw std::runtime_error(err);
    }
    json after = DocumentIO::FeatureToJson(*sk);
    // Capture the reply BEFORE the command runs — Execute rebuilds the feature
    // object from JSON, so `sk` dangles afterwards.
    json resp = {{"external_index", ei}, {"gen_ids", sk->external[ei].genIds}};
    const json preDigest = BodyDigestJson(*doc);
    state.commands->Execute(
        std::make_unique<ModifyFeatureCommand>(index, before, after), *doc);
    resp["regen"] = RegenSummaryJson(*doc);
    resp["digest"] = DigestJson(preDigest, *doc);
    return resp;
  }
  if (m == "add_constraint" || m == "add_constraints") {
    // Append one constraint (add_constraint: {sketch_id, kind, refs, value?/
    // expr?}) or MANY (add_constraints: {sketch_id, constraints: [{kind,
    // refs, value?/expr?}, ...]}) to a sketch, then solve + regen + commit
    // ONCE. The plural form is the fast path for importers: dozens of
    // constraints without a full regeneration per constraint.
    size_t index = 0;
    Feature* f =
        doc->FindFeatureById(p.at("sketch_id").get<uint64_t>(), &index);
    auto* sk = dynamic_cast<SketchFeature*>(f);
    if (!sk)
      throw std::runtime_error(
          "no sketch with id " +
          std::to_string(p.at("sketch_id").get<uint64_t>()) +
          ". Sketches here: " + FeatureCandidates(*doc, true) +
          " (ids come from add_feature results / get_state)");

    const auto& kinds = ConstraintKindTable();

    // Build one Constraint from a spec object. Throws on a bad kind/expr.
    const auto build = [&](const json& spec) -> sketch::Constraint {
      sketch::Constraint c;
      auto it = kinds.find(spec.at("kind").get<std::string>());
      if (it == kinds.end()) {
        std::string known;
        for (const auto& [name, kind] : kinds)
          known += (known.empty() ? "" : ", ") + name;
        throw std::runtime_error("unknown constraint kind \"" +
                                 spec.at("kind").get<std::string>() +
                                 "\". Known kinds: " + known);
      }
      c.kind = it->second;
      if (c.kind == sketch::ConstraintKind::Angle) {
        // `value` was DEGREES here and RADIANS in the same field of the same
        // sketch reached through add_feature (ConstraintFromJson), and
        // get_state echoed the radians back: set 45, read 0.785, and both
        // were "right". So the bare name means what it means everywhere else
        // in this file since the arc fix — radians — and `value_deg` says
        // degrees, refusing both at once.
        //
        // `expr` is NOT part of that swap and deliberately stays on the
        // expression language's angle base, which is degrees ("45", "45deg",
        // "0.785rad" all work) exactly as it is for angle_expr on Revolve and
        // Pattern. The two live in different worlds and each says which.
        c.value =
            AngleParam(spec.contains("value"), spec.value("value", 0.0),
                       spec.contains("value_deg"), spec.value("value_deg", 0.0),
                       "value", "value_deg", 0.0);
      } else {
        c.value = spec.value("value", 0.0);
      }
      c.valueExpr = spec.value("expr", std::string{});
      if (!c.valueExpr.empty()) {
        const auto v = ExpressionEngine::Default().EvaluateText(c.valueExpr,
                                                                doc->Symbols());
        if (!v)
          throw std::runtime_error("bad expr: " + v.error().message);
        c.value =
            (c.kind == sketch::ConstraintKind::Angle) ? glm::radians(*v) : *v;
      }
      const json& refs = spec.at("refs");
      if (refs.size() > 0)
        c.a = {refs[0][0].get<uint32_t>(), refs[0][1].get<uint8_t>()};
      if (refs.size() > 1)
        c.b = {refs[1][0].get<uint32_t>(), refs[1][1].get<uint8_t>()};
      if (refs.size() > 2)
        c.c = {refs[2][0].get<uint32_t>(), refs[2][1].get<uint8_t>()};
      return c;
    };

    json before = DocumentIO::FeatureToJson(*sk);
    const size_t had = sk->constraints.size();
    size_t added = 0;
    if (m == "add_constraints") {
      for (const json& spec : p.at("constraints"))
        sk->constraints.push_back(build(spec)), added++;
    } else {
      sk->constraints.push_back(build(p.raw()));
      added = 1;
    }
    sketch::Solver solver;
    solver.Load(*sk);
    // A constraint the solver cannot represent is refused, naming what its
    // kind takes, and the sketch is left as it was -- accepted, it would
    // solve as if absent while the sketch looked constrained.
    for (const auto& bad : solver.UnsupportedConstraints()) {
      if (bad.index < had)
        continue; // already in the sketch: its regen warning says so
      sk->constraints.resize(had);
      throw std::runtime_error(
          (added > 1 ? "constraints[" + std::to_string(bad.index - had) + "]: "
                     : std::string()) +
          bad.reason);
    }
    if (solver.Solve().ok)
      solver.WriteBack(sk->entities);
    sk->RegenerateDerived();
    doc->Regenerate();
    json after = DocumentIO::FeatureToJson(*sk);
    const json preDigest = BodyDigestJson(*doc);
    state.commands->Execute(
        std::make_unique<ModifyFeatureCommand>(index, before, after), *doc);
    return json{{"added", added},
                {"regen", RegenSummaryJson(*doc)},
                {"digest", DigestJson(preDigest, *doc)}};
  }
  if (m == "mirror" || m == "fillet" || m == "pattern" || m == "offset") {
    // Generative op on a sketch (by id), solve, and commit one edit.
    // mirror params:  {sketch_id, sources:[id,...], axis:lineId, link?}
    // fillet params:  {sketch_id, line1, line2, radius}
    // pattern params: {sketch_id, sources:[id,...], count, link?,
    //                  circular?, step:[dx,dy] | center:[x,y], angle}
    // offset params:  {sketch_id, chain:[id,...], distance, join_arc?}
    size_t index = 0;
    Feature* f =
        doc->FindFeatureById(p.at("sketch_id").get<uint64_t>(), &index);
    auto* sk = dynamic_cast<SketchFeature*>(f);
    if (!sk)
      throw std::runtime_error(
          "no sketch with id " +
          std::to_string(p.at("sketch_id").get<uint64_t>()) +
          ". Sketches here: " + FeatureCandidates(*doc, true) +
          " (ids come from add_feature results / get_state)");

    json before = DocumentIO::FeatureToJson(*sk);
    json result = json::object();
    if (m == "mirror") {
      std::vector<uint32_t> sources;
      for (const auto& s : p.at("sources"))
        sources.push_back(s.get<uint32_t>());
      result["generated"] = sk->MirrorEntities(
          sources, p.at("axis").get<uint32_t>(), p.value("link", true));
    } else if (m == "pattern") {
      std::vector<uint32_t> sources;
      for (const auto& s : p.at("sources"))
        sources.push_back(s.get<uint32_t>());
      const int count = p.at("count").get<int>();
      const bool link = p.value("link", true);
      if (p.value("circular", false)) {
        const auto& c = p.at("center");
        // PER-STEP, and radians. Both halves were wrong in the MCP layer,
        // which called it "the total sweep in degrees" and passed it through
        // unconverted: a 3-instance pattern asked for 90 came out as 90
        // radians per step (folded, 335.6 degrees) instead of the 45-degree
        // steps the caller drew in their head. Two errors in one number, and
        // the sketch it builds looks deliberate.
        result["generated"] = sk->AddCircularPattern(
            sources, {c[0].get<double>(), c[1].get<double>()},
            AngleParam(p.contains("angle"), p.value("angle", 0.0),
                       p.contains("angle_deg"), p.value("angle_deg", 0.0),
                       "angle", "angle_deg", 0.0),
            count, link);
      } else {
        const auto& s = p.at("step");
        result["generated"] = sk->AddLinearPattern(
            sources, {s[0].get<double>(), s[1].get<double>()}, count, link);
      }
    } else if (m == "offset") {
      std::vector<uint32_t> chain;
      for (const auto& s : p.at("chain"))
        chain.push_back(s.get<uint32_t>());
      result["generated"] = sk->OffsetChain(
          chain, p.at("distance").get<double>(), p.value("join_arc", true));
    } else {
      std::string whyNot;
      const uint32_t arcId = sk->FilletCorner(
          p.at("line1").get<uint32_t>(), p.at("line2").get<uint32_t>(),
          p.at("radius").get<double>(), &whyNot);
      if (arcId == 0) // loud, not {"arc_id": 0} — a silent no-op to a script
        throw std::runtime_error("fillet failed: " + whyNot);
      result["arc_id"] = arcId;
    }
    sketch::Solver solver;
    solver.Load(*sk);
    if (solver.Solve().ok)
      solver.WriteBack(sk->entities);
    sk->RegenerateDerived();
    doc->Regenerate();
    json after = DocumentIO::FeatureToJson(*sk);
    const json preDigest = BodyDigestJson(*doc);
    state.commands->Execute(
        std::make_unique<ModifyFeatureCommand>(index, before, after), *doc);
    result["regen"] = RegenSummaryJson(*doc);
    result["digest"] = DigestJson(preDigest, *doc);
    return result;
  }
  if (m == "add_reference_image" || m == "calibrate_reference_image" ||
      m == "set_reference_image" || m == "delete_reference_image") {
    // Sketch reference underlays
    // (v49, docs/sketching/README.md#reference-images-underlays).
    // add:       {sketch_id, path, [at:[u,v]], [units_per_pixel], [opacity]}
    // calibrate: {sketch_id, image_id, p1:[u,v], p2:[u,v], distance,
    //             [align:"none"|"horizontal"|"vertical"]}
    // set:       {sketch_id, image_id, [opacity]|[hidden]|[rotation]|
    //             [units_per_pixel]|[origin:[u,v]]}
    // delete:    {sketch_id, image_id}
    size_t index = 0;
    Feature* f =
        doc->FindFeatureById(p.at("sketch_id").get<uint64_t>(), &index);
    auto* sk = dynamic_cast<SketchFeature*>(f);
    if (!sk)
      throw std::runtime_error(
          "no sketch with id " +
          std::to_string(p.at("sketch_id").get<uint64_t>()) +
          ". Sketches here: " + FeatureCandidates(*doc, true) +
          " (ids come from add_feature results / get_state)");
    json before = DocumentIO::FeatureToJson(*sk);
    json result = json::object();

    if (m == "add_reference_image") {
      const std::string path = p.at("path").get<std::string>();
      std::ifstream in(path, std::ios::binary);
      if (!in)
        throw std::runtime_error("cannot open image: " + path);
      std::string data((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
      if (data.empty())
        throw std::runtime_error("empty image: " + path);
      std::vector<unsigned char> bytes(data.begin(), data.end());

      sketch::ReferenceImage img;
      int w = 0, h = 0, comp = 0;
      if (stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()),
                                &w, &h, &comp)) {
        img.widthPx = w;
        img.heightPx = h;
      }
      // Default scale: make the image ~4 plane units wide so it lands visible,
      // unless the caller pins one. (Recalibrated to a real dimension next.)
      img.unitsPerPixel = p.value("units_per_pixel", w > 0 ? 4.0 / w : 0.01);
      img.opacity = p.value("opacity", 0.5);
      const uint32_t id = sk->AddImage(img);
      sketch::ReferenceImage& ri = sk->images[sk->IndexOfImage(id)];
      // Stable asset name carrying the source extension, keyed in the doc
      // store.
      std::string ext = std::filesystem::path(path).extension().string();
      if (ext.empty())
        ext = ".png";
      ri.asset = std::to_string(sk->GetId()) + "-" + std::to_string(id) + ext;
      // Centre the image on the requested point (default plane origin).
      glm::dvec2 center(0.0, 0.0);
      if (p.contains("at")) {
        const auto& a = p.at("at");
        center = {a[0].get<double>(), a[1].get<double>()};
      }
      ri.origin =
          center - 0.5 * glm::dvec2(ri.widthPx, ri.heightPx) * ri.unitsPerPixel;
      doc->SetAsset(ri.asset, std::move(bytes));
      result = {{"image_id", id},
                {"width_px", ri.widthPx},
                {"height_px", ri.heightPx},
                {"asset", ri.asset}};
    } else if (m == "calibrate_reference_image") {
      const auto& a = p.at("p1");
      const auto& b = p.at("p2");
      const std::string al = p.value("align", std::string("none"));
      const int align = al == "horizontal" ? 0 : al == "vertical" ? 1 : -1;
      if (!sk->CalibrateImage(p.at("image_id").get<uint32_t>(),
                              {a[0].get<double>(), a[1].get<double>()},
                              {b[0].get<double>(), b[1].get<double>()},
                              p.at("distance").get<double>(), align))
        throw std::runtime_error(
            "calibration failed (unknown image or coincident points)");
      result["ok"] = true;
    } else if (m == "set_reference_image") {
      const size_t ii = sk->IndexOfImage(p.at("image_id").get<uint32_t>());
      if (ii == std::numeric_limits<size_t>::max())
        throw std::runtime_error("no such image");
      sketch::ReferenceImage& ri = sk->images[ii];
      if (p.contains("opacity"))
        ri.opacity = p.at("opacity").get<double>();
      if (p.contains("hidden"))
        ri.hidden = p.at("hidden").get<bool>();
      if (p.contains("rotation") || p.contains("rotation_deg"))
        ri.rotation =
            AngleParam(p.contains("rotation"), p.value("rotation", 0.0),
                       p.contains("rotation_deg"), p.value("rotation_deg", 0.0),
                       "rotation", "rotation_deg", ri.rotation);
      if (p.contains("units_per_pixel"))
        ri.unitsPerPixel = p.at("units_per_pixel").get<double>();
      if (p.contains("origin")) {
        const auto& o = p.at("origin");
        ri.origin = {o[0].get<double>(), o[1].get<double>()};
      }
      result["ok"] = true;
    } else if (m == "delete_reference_image") {
      // Orphaned pixel bytes stay in the doc store; save-time pruning drops
      // any asset no image references (and keeps redo working meanwhile).
      result["ok"] = sk->DeleteImage(p.at("image_id").get<uint32_t>());
    }
    doc->Regenerate();
    json after = DocumentIO::FeatureToJson(*sk);
    const json preDigest = BodyDigestJson(*doc);
    state.commands->Execute(
        std::make_unique<ModifyFeatureCommand>(index, before, after), *doc);
    result["regen"] = RegenSummaryJson(*doc);
    result["digest"] = DigestJson(preDigest, *doc);
    return result;
  }
  if (m == "delete_entity") {
    // Remove a sketch entity by id, cleaning up dependent constraints; commit
    // one undoable edit. params: {sketch_id, entity_id}
    size_t index = 0;
    Feature* f =
        doc->FindFeatureById(p.at("sketch_id").get<uint64_t>(), &index);
    auto* sk = dynamic_cast<SketchFeature*>(f);
    if (!sk)
      throw std::runtime_error(
          "no sketch with id " +
          std::to_string(p.at("sketch_id").get<uint64_t>()) +
          ". Sketches here: " + FeatureCandidates(*doc, true) +
          " (ids come from add_feature results / get_state)");
    json before = DocumentIO::FeatureToJson(*sk);
    const size_t dropped = sk->DeleteEntity(p.at("entity_id").get<uint32_t>());
    sketch::Solver solver;
    solver.Load(*sk);
    if (solver.Solve().ok)
      solver.WriteBack(sk->entities);
    sk->RegenerateDerived();
    doc->Regenerate();
    json after = DocumentIO::FeatureToJson(*sk);
    const json preDigest = BodyDigestJson(*doc);
    state.commands->Execute(
        std::make_unique<ModifyFeatureCommand>(index, before, after), *doc);
    return json{{"dropped_constraints", dropped},
                {"regen", RegenSummaryJson(*doc)},
                {"digest", DigestJson(preDigest, *doc)}};
  }
  return std::nullopt;
}

} // namespace remote
