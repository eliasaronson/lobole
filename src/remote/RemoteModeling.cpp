#include "remote/RemoteModeling.h"
#include "ui/SelectionPipeline.h"

#include "cad/CADMaterials.h"
#include "cad/FaceSelector.h"
#include "core/Command.h"
#include "core/Document.h"
#include "core/Expression.h"
#include "core/features/DerivedFeature.h"
#include "core/features/SketchFeature.h"
#include "core/io/Serialization.h"
#include "core/sketch/SketchSolver.h"
#include "remote/RemoteHelpers.h"
#include "ui/AppState.h"
#include "ui/DerivedActions.h"
#include "ui/PreviewFeature.h"
#include "ui/RendererSync.h"

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
      "type", "index", "error", "warning", "after_earlier_failure"};
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
    // Documented for this type, just empty here.
    if (dialect.count(it.key()) != 0 &&
        ((it.value().is_array() || it.value().is_object()) &&
         it.value().empty()))
      continue;
    std::string accepted;
    for (auto o = out.begin(); o != out.end(); ++o)
      accepted += (accepted.empty() ? "" : ", ") + o.key();
    throw std::runtime_error("unknown field '" + it.key() + "' for " +
                             built.TypeName() + ". Accepted: type, " +
                             accepted + ".");
  }
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

// describe: one row per `m ==` branch below — extend together.
static constexpr MethodDoc ModelingMethods[] = {
    {"select_feature",
     "Select a feature by index or feature_id (-1/0 clears); "
     "drives the same selection the tree uses.",
     "feature_id, index"},
    {"accept_pending",
     "Commit the pending (preview) feature, like clicking the checkmark.", ""},
    {"cancel_pending", "Discard the pending (preview) feature.", ""},
    {"add_feature",
     "Append a feature (see feature_types for the JSON dialect) "
     "and regenerate; returns {index, id, entity_ids?, regen}.",
     "feature"},
    {"update_derived",
     "Retake a Derived feature's snapshot from its source part (one undo "
     "step): {feature_id} or {index}, or {all: true} for every Derived of "
     "the active part whose source is loaded. Refuses a source that is not "
     "in the project or a broken link. Returns {updated: [ids], regen, "
     "digest}. Never automatic: get_state's `stale` says when to call it "
     "(docs/modeling/derived-plan.md).",
     "all, feature_id, index"},
    {"derive_occurrence",
     "In an in-context edit (edit_in_context): add a Derived copy of a "
     "neighbour occurrence's part, placed where the ghost is "
     "(OccMatrix(edited)^-1 * OccMatrix(neighbour)); {occurrence, name?, "
     "reference?: true, bodies?: [created_by ids], body_names?: [names]}. "
     "Returns {index, id, "
     "regen, digest}.",
     "bodies, body_names, name, occurrence, reference"},
    {"modify_feature",
     "Change a feature to `feature` JSON; one undo step; returns {regen}. "
     "Name it by `feature_id` (what every other call hands you) or by "
     "`index` (its position in the tree). REPLACES the whole spec rather "
     "than patching it, so `feature` must be complete and carry its `type`; "
     "read it out of get_state, change what you want and send it back. "
     "It keeps its IDENTITY: omit \"id\" INSIDE the feature and it keeps the "
     "one it has, so references to it (sketch_id, mates, drawing dimensions) "
     "keep resolving. Pass a different \"id\" there only to mean 'this is a "
     "different feature now', which deliberately dangles those references.",
     "feature, feature_id, index"},
    {"set_material",
     "Assign a material slug (see material_slugs) to a body's creating "
     "feature, by feature_id (its created_by) or by body index; '' restores "
     "default.",
     "body, feature_id, material"},
    {"set_appearance",
     "Visual override SEPARATE from the physical material (v54): "
     "{feature_id|body, color?: [r,g,b] sRGB 0..1, roughness?: 0..1, "
     "metallic?: 0..1}. Unset fields inherit from the material; passing NO "
     "optional fields clears the override. Never affects mass/FEM/BOM, and "
     "recoloring rebuilds no geometry.",
     "body, color, feature_id, metallic, roughness"},
    {"set_rollback",
     "Move the rollback bar: only features [0, index) regenerate; -1 = end.",
     "index"},
    {"suppress_feature",
     "Toggle suppression by feature_id; suppressed "
     "features are skipped but keep parameters.",
     "feature_id, suppressed"},
    {"delete_feature",
     "Delete a feature by `id` or `index`; one undo step; returns {regen}.",
     "id, index"},
    {"project_geometry",
     "Project a body face/edge (feature_id + element) into "
     "a sketch as live locked construction geometry.",
     "element, feature_id, kind, sketch_id"},
    {"add_reference_image",
     "Attach an image underlay to a sketch (path or "
     "base64 bytes); returns image_id.",
     "at, opacity, path, sketch_id, units_per_pixel"},
    {"set_reference_image",
     "Update an underlay's placement/opacity/lock by {sketch_id, image_id}.",
     "hidden, image_id, opacity, origin, rotation, sketch_id, units_per_pixel"},
    {"calibrate_reference_image",
     "Scale an underlay so two picked image points span a given distance.",
     "align, distance, image_id, p1, p2, sketch_id"},
    {"delete_reference_image", "Remove an underlay by {sketch_id, image_id}.",
     "image_id, sketch_id"},
    {"add_constraint",
     "Append one sketch constraint {sketch_id, kind, refs, value?/expr?}; "
     "kinds in constraint_kinds; solves once.",
     "*"},
    {"add_constraints",
     "Append MANY constraints in one solve+regen+undo step: "
     "{sketch_id, constraints: [...]}.",
     "*"},
    {"delete_entity",
     "Delete a sketch entity by {sketch_id, entity_id}, "
     "dropping constraints that reference it.",
     "entity_id, sketch_id"},
    {"fillet",
     "Sketch fillet: round the corner between two lines {sketch_id, "
     "line1, line2, radius}; errors explain a failed corner.",
     "line1, line2, radius, sketch_id"},
    {"offset",
     "Offset a closed sketch chain by a distance {sketch_id, "
     "chain: [entity ids], distance, join_arc?}.",
     "chain, distance, join_arc, line1, line2, radius, sketch_id"},
    {"mirror",
     "Mirror sketch entities across a line {sketch_id, "
     "sources: [entity ids], axis: <line entity id>, link?}.",
     "axis, line1, line2, link, radius, sketch_id, sources"},
    {"pattern",
     "Sketch pattern (linear/circular) of entities; "
     "expression-capable count/spacing.",
     "angle, center, circular, count, line1, line2, link, radius, sketch_id, "
     "sources, step"},
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

std::optional<json> HandleModelingRpc(ui::AppState& state, Document& document,
                                      const std::string& m,
                                      const RpcParams& p) {
  Document* doc = &document;
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
    auto feature = FeatureFromJsonNamed(p.at("feature"));
    RejectUnknownFeatureFields(p.at("feature"), *feature);
    // The GUI refuses a tool with missing inputs; the script API used to
    // accept one and commit a feature that does nothing (a fillet with no
    // edges regenerates happily). Same rule, both surfaces.
    // A Derived arrives without its snapshot (the request names a source;
    // the copy is taken here, through the project): refuses a part that is
    // not loaded, the part itself, and a cycle — in words naming the parts
    // that ARE there (docs/modeling/derived-plan.md).
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
    // than the round trip (docs/dogfood-friction-plan.md, F3).
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
    // Build a throwaway from the request first: validates the JSON AND lets
    // the round-trip unknown-field check run before anything is committed.
    RejectUnknownFeatureFields(p.at("feature"),
                               *DocumentIO::FeatureFromJson(p.at("feature")));
    json before = DocumentIO::FeatureToJson(*feature);
    const json preDigest = BodyDigestJson(*doc);
    if (!state.commands->Execute(std::make_unique<ModifyFeatureCommand>(
                                     index, before, p.at("feature")),
                                 *doc)) {
      throw std::runtime_error(
          "modify failed — the replacement feature did not apply and the "
          "document is unchanged (its regeneration failed where the original "
          "succeeded). Check the JSON against describe feature_types[].params");
    }
    return json{{"regen", RegenSummaryJson(*doc)},
                {"digest", DigestJson(preDigest, *doc)}};
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
    const std::string slug = p.value("material", std::string{});
    // A non-empty slug that resolves to DEFAULT is unknown (DEFAULT is never in
    // the slug table, so valid slugs always map to a specific material).
    if (!slug.empty() && CADMaterials::MaterialLibrary::FromSlug(slug) ==
                             CADMaterials::MaterialType::DEFAULT) {
      throw std::runtime_error("unknown material slug: " + slug);
    }
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
    // from the physical material (docs/rendering/appearance-plan.md). Same
    // addressing as set_material: {feature_id | body}. Optional fields
    // color [r,g,b] (sRGB 0..1), roughness, metallic; passing none clears
    // the override. Appearance is stripped from the regen chain hash, so
    // this never rebuilds geometry — just restamps and resyncs.
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
                                 " — appearance fields are 0..1 (sRGB / "
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
            "color components are sRGB 0..1, not 0..255 — got [" +
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
    // not an edit of it. params: {index}
    doc->SetRollback(p.at("index").get<int>());
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
    size_t index = 0;
    if (p.contains("id")) {
      const uint64_t id = p.at("id").get<uint64_t>();
      if (doc->FindFeatureById(id, &index) == nullptr)
        throw std::runtime_error("no feature with id " + std::to_string(id) +
                                 " (get_state lists features with their ids)");
    } else if (p.contains("index")) {
      index = p.at("index").get<size_t>();
    } else {
      throw std::runtime_error("delete_feature needs `id` or `index` "
                               "(get_state lists features with both)");
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
      if (it == kinds.end())
        throw std::runtime_error("unknown constraint kind");
      c.kind = it->second;
      c.value = spec.value("value", 0.0);
      if (c.kind == sketch::ConstraintKind::Angle)
        c.value = glm::radians(c.value); // wire=degrees, model=radians
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
        result["generated"] = sk->AddCircularPattern(
            sources, {c[0].get<float>(), c[1].get<float>()},
            p.at("angle").get<double>(), count, link);
      } else {
        const auto& s = p.at("step");
        result["generated"] = sk->AddLinearPattern(
            sources, {s[0].get<float>(), s[1].get<float>()}, count, link);
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
    // Sketch reference underlays (v49, docs/sketching/reference-image-plan.md).
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
      glm::vec2 center(0.0f, 0.0f);
      if (p.contains("at")) {
        const auto& a = p.at("at");
        center = {a[0].get<float>(), a[1].get<float>()};
      }
      ri.origin =
          center - 0.5f * glm::vec2(float(ri.widthPx), float(ri.heightPx)) *
                       float(ri.unitsPerPixel);
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
                              {a[0].get<float>(), a[1].get<float>()},
                              {b[0].get<float>(), b[1].get<float>()},
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
      if (p.contains("rotation"))
        ri.rotation = p.at("rotation").get<double>();
      if (p.contains("units_per_pixel"))
        ri.unitsPerPixel = p.at("units_per_pixel").get<double>();
      if (p.contains("origin")) {
        const auto& o = p.at("origin");
        ri.origin = {o[0].get<float>(), o[1].get<float>()};
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
