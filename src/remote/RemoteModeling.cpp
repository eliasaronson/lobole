#include "remote/RemoteModeling.h"

#include "cad/CADMaterials.h"
#include "cad/FaceSelector.h"
#include "core/Command.h"
#include "core/Document.h"
#include "core/Expression.h"
#include "core/features/SketchFeature.h"
#include "core/io/Serialization.h"
#include "core/sketch/SketchSolver.h"
#include "remote/RemoteHelpers.h"
#include "ui/AppActions.h"
#include "ui/AppState.h"
#include "ui/PreviewFeature.h"

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
  for (auto it = request.begin(); it != request.end(); ++it) {
    if (out.contains(it.key()) || kAllow.count(it.key()) != 0)
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
    {"select_feature", "Select a feature by index or feature_id (-1/0 clears); "
                       "drives the same selection the tree uses."},
    {"accept_pending",
     "Commit the pending (preview) feature, like clicking the checkmark."},
    {"cancel_pending", "Discard the pending (preview) feature."},
    {"add_feature", "Append a feature (see feature_types for the JSON dialect) "
                    "and regenerate; returns {index, id, entity_ids?, regen}."},
    {"modify_feature",
     "Change the feature at `index` to `feature` JSON; one undo step; returns "
     "{regen}. It keeps its IDENTITY: omit \"id\" and the feature keeps the "
     "one it has, so references to it (sketch_id, mates, drawing dimensions) "
     "keep resolving. Pass a different \"id\" only to mean 'this is a "
     "different feature now', which deliberately dangles those references."},
    {"set_material",
     "Assign a material slug (see material_slugs) to a body's creating "
     "feature, by feature_id (its created_by) or by body index; '' restores "
     "default."},
    {"set_appearance",
     "Visual override SEPARATE from the physical material (v54): "
     "{feature_id|body, color?: [r,g,b] sRGB 0..1, roughness?: 0..1, "
     "metallic?: 0..1}. Unset fields inherit from the material; passing NO "
     "optional fields clears the override. Never affects mass/FEM/BOM, and "
     "recoloring rebuilds no geometry."},
    {"set_rollback",
     "Move the rollback bar: only features [0, index) regenerate; -1 = end."},
    {"suppress_feature", "Toggle suppression by feature_id; suppressed "
                         "features are skipped but keep parameters."},
    {"delete_feature",
     "Delete the feature at `index`; one undo step; returns {regen}."},
    {"project_geometry", "Project a body face/edge (feature_id + element) into "
                         "a sketch as live locked construction geometry."},
    {"add_reference_image", "Attach an image underlay to a sketch (path or "
                            "base64 bytes); returns image_id."},
    {"set_reference_image",
     "Update an underlay's placement/opacity/lock by {sketch_id, image_id}."},
    {"calibrate_reference_image",
     "Scale an underlay so two picked image points span a given distance."},
    {"delete_reference_image", "Remove an underlay by {sketch_id, image_id}."},
    {"add_constraint",
     "Append one sketch constraint {sketch_id, kind, refs, value?/expr?}; "
     "kinds in constraint_kinds; solves once."},
    {"add_constraints", "Append MANY constraints in one solve+regen+undo step: "
                        "{sketch_id, constraints: [...]}."},
    {"delete_entity", "Delete a sketch entity by {sketch_id, entity_id}, "
                      "dropping constraints that reference it."},
    {"fillet", "Sketch fillet: round the corner between two lines {sketch_id, "
               "line1, line2, radius}; errors explain a failed corner."},
    {"offset", "Offset a closed sketch chain by a distance {sketch_id, "
               "chain: [entity ids], distance, join_arc?}."},
    {"mirror", "Mirror sketch entities across a line {sketch_id, "
               "sources: [entity ids], axis: <line entity id>, link?}."},
    {"pattern", "Sketch pattern (linear/circular) of entities; "
                "expression-capable count/spacing."},
    {"list_variables",
     "List variables with expressions, values, comments, and scope."},
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
  if (m == "modify_feature") {
    const size_t index = p.at("index").get<size_t>();
    Feature* feature = doc->GetFeature(index);
    if (!feature)
      throw std::runtime_error(IndexError("feature", index, doc->FeatureCount(),
                                          "get_state lists features with "
                                          "their indices"));
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
    if (!state.commands->Execute(
            std::make_unique<RemoveFeatureCommand>(p.at("index").get<size_t>()),
            *doc)) {
      throw std::runtime_error(IndexError(
          "feature", p.at("index").get<size_t>(), doc->FeatureCount(),
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
    state.commands->Execute(
        std::make_unique<ModifyFeatureCommand>(index, before, after), *doc);
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
    state.commands->Execute(
        std::make_unique<ModifyFeatureCommand>(index, before, after), *doc);
    return json{{"added", added}};
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
    state.commands->Execute(
        std::make_unique<ModifyFeatureCommand>(index, before, after), *doc);
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
    } else { // delete_reference_image
      // Orphaned pixel bytes stay in the doc store; save-time pruning drops
      // any asset no image references (and keeps redo working meanwhile).
      result["ok"] = sk->DeleteImage(p.at("image_id").get<uint32_t>());
    }
    doc->Regenerate();
    json after = DocumentIO::FeatureToJson(*sk);
    state.commands->Execute(
        std::make_unique<ModifyFeatureCommand>(index, before, after), *doc);
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
    state.commands->Execute(
        std::make_unique<ModifyFeatureCommand>(index, before, after), *doc);
    return json{{"dropped_constraints", dropped}};
  }
  return std::nullopt;
}

} // namespace remote
