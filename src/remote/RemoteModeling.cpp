#include "remote/RemoteModeling.h"

#include "CADMaterials.h"
#include "FaceSelector.h"
#include "core/Command.h"
#include "core/Document.h"
#include "core/Expression.h"
#include "core/Serialization.h"
#include "core/SketchFeature.h"
#include "core/SketchSolver.h"
#include "remote/RemoteHelpers.h"
#include "remote/RemoteSimulation.h"
#include "remote/RemoteInspection.h"
#include "ui/AppActions.h"
#include "ui/AppState.h"
#include "ui/PreviewFeature.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using nlohmann::json;

namespace remote {

// Modeling / feature-tree RPCs (select_feature..list_variables) carved out of
// RemoteServer::HandleSemantic. All act on the active part document.
std::optional<json> HandleModelingRpc(ui::AppState& state, Document& document,
                                      const std::string& m, const json& p) {
  Document* doc = &document;
  if (m == "select_feature") {
    // Select a feature by index (-1 clears). Drives the same selection the
    // feature tree sets, so the Properties panel shows/edits it.
    int index = p.value("index", -1);
    if (index >= 0 && index >= static_cast<int>(doc->FeatureCount()))
      throw std::runtime_error("feature index out of range");
    state.selectedFeature = index;
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
    auto feature = DocumentIO::FeatureFromJson(p.at("feature"));
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
    return resp;
  }
  if (m == "modify_feature") {
    const size_t index = p.at("index").get<size_t>();
    Feature* feature = doc->GetFeature(index);
    if (!feature) throw std::runtime_error("no feature at index");
    json before = DocumentIO::FeatureToJson(*feature);
    if (!state.commands->Execute(std::make_unique<ModifyFeatureCommand>(
                                     index, before, p.at("feature")),
                                 *doc)) {
      throw std::runtime_error("modify failed");
    }
    return json::object();
  }
  if (m == "set_material") {
    // Assign a material to a feature (by id). params: {feature_id, material}
    // where material is a library slug (e.g. "steel") or "" for default.
    // Commits one undoable edit and regenerates.
    size_t index = 0;
    Feature* f = doc->FindFeatureById(p.at("feature_id").get<uint64_t>(), &index);
    if (!f) throw std::runtime_error("no such feature");
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
    return json::object();
  }
  if (m == "set_rollback") {
    // Move the rollback bar: regeneration executes features [0, index) only;
    // -1 (or >= count) = end. Not undoable — it is a view of the history,
    // not an edit of it. params: {index}
    doc->SetRollback(p.at("index").get<int>());
    return json{{"rollback", doc->Rollback()}};
  }
  if (m == "suppress_feature") {
    // Toggle suppression (by feature id) as one undoable edit; a suppressed
    // feature is skipped by regeneration but keeps its parameters. params:
    // {feature_id, suppressed}
    size_t index = 0;
    Feature* f =
        doc->FindFeatureById(p.at("feature_id").get<uint64_t>(), &index);
    if (!f) throw std::runtime_error("no such feature");
    json before = DocumentIO::FeatureToJson(*f);
    f->SetSuppressed(p.at("suppressed").get<bool>());
    doc->Regenerate();
    json after = DocumentIO::FeatureToJson(*f);
    state.commands->Execute(
        std::make_unique<ModifyFeatureCommand>(index, before, after), *doc);
    return json::object();
  }
  if (m == "delete_feature") {
    if (!state.commands->Execute(std::make_unique<RemoveFeatureCommand>(
                                     p.at("index").get<size_t>()),
                                 *doc)) {
      throw std::runtime_error("no feature at index");
    }
    return json::object();
  }
  if (m == "add_constraint") {
    // Append a constraint to a sketch (by id), solve, and commit one edit.
    // params: {sketch_id, kind, refs: [[entity,which],...], value?}
    size_t index = 0;
    Feature* f = doc->FindFeatureById(p.at("sketch_id").get<uint64_t>(), &index);
    auto* sk = dynamic_cast<SketchFeature*>(f);
    if (!sk) throw std::runtime_error("no such sketch");

    sketch::Constraint c;
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
        {"diameter", sketch::ConstraintKind::Diameter}};
    auto it = kinds.find(p.at("kind").get<std::string>());
    if (it == kinds.end()) throw std::runtime_error("unknown constraint kind");
    c.kind = it->second;
    c.value = p.value("value", 0.0);
    // Optional expression driving the value, evaluated against the document's
    // variables on every regeneration (angles in degrees, like the UI).
    c.valueExpr = p.value("expr", std::string{});
    if (!c.valueExpr.empty()) {
      // Evaluate now too: the immediate solve below runs before
      // Document::Regenerate refreshes dimension values (VisitExprs), and
      // solving with the default 0 collapses the geometry irrecoverably.
      ExprError err;
      const auto v = ExpressionEngine::Default().EvaluateText(
          c.valueExpr, doc->Symbols(), &err);
      if (!v) throw std::runtime_error("bad expr: " + err.message);
      c.value = (c.kind == sketch::ConstraintKind::Angle)
                    ? glm::radians(*v)
                    : *v;
    }
    const json& refs = p.at("refs");
    if (refs.size() > 0)
      c.a = {refs[0][0].get<uint32_t>(), refs[0][1].get<uint8_t>()};
    if (refs.size() > 1)
      c.b = {refs[1][0].get<uint32_t>(), refs[1][1].get<uint8_t>()};
    if (refs.size() > 2) // Symmetric's third ref (the symmetry line)
      c.c = {refs[2][0].get<uint32_t>(), refs[2][1].get<uint8_t>()};

    json before = DocumentIO::FeatureToJson(*sk);
    sk->constraints.push_back(c);
    sketch::Solver solver;
    solver.Load(*sk);
    if (solver.Solve().ok)
      solver.WriteBack(sk->entities);
    sk->RegenerateDerived();
    doc->Regenerate();
    json after = DocumentIO::FeatureToJson(*sk);
    state.commands->Execute(
        std::make_unique<ModifyFeatureCommand>(index, before, after), *doc);
    return json::object();
  }
  if (m == "mirror" || m == "fillet" || m == "pattern" || m == "offset") {
    // Generative op on a sketch (by id), solve, and commit one edit.
    // mirror params:  {sketch_id, sources:[id,...], axis:lineId, link?}
    // fillet params:  {sketch_id, line1, line2, radius}
    // pattern params: {sketch_id, sources:[id,...], count, link?,
    //                  circular?, step:[dx,dy] | center:[x,y], angle}
    // offset params:  {sketch_id, chain:[id,...], distance, join_arc?}
    size_t index = 0;
    Feature* f = doc->FindFeatureById(p.at("sketch_id").get<uint64_t>(), &index);
    auto* sk = dynamic_cast<SketchFeature*>(f);
    if (!sk) throw std::runtime_error("no such sketch");

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
      result["arc_id"] =
          sk->FilletCorner(p.at("line1").get<uint32_t>(),
                           p.at("line2").get<uint32_t>(),
                           p.at("radius").get<double>());
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
  if (m == "delete_entity") {
    // Remove a sketch entity by id, cleaning up dependent constraints; commit
    // one undoable edit. params: {sketch_id, entity_id}
    size_t index = 0;
    Feature* f = doc->FindFeatureById(p.at("sketch_id").get<uint64_t>(), &index);
    auto* sk = dynamic_cast<SketchFeature*>(f);
    if (!sk) throw std::runtime_error("no such sketch");
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
  if (m == "set_variable") {
    // Add or redefine a document variable; one undoable edit, regenerates
    // every expression-driven field. params: {name, expression, comment?}.
    // Returns the evaluated value (plus "error" if it didn't evaluate).
    const std::string name = p.at("name").get<std::string>();
    const json& expr = p.at("expression");
    const std::string expression =
        expr.is_string() ? expr.get<std::string>() : expr.dump();
    // Omitting "comment" preserves an existing one (redefinitions usually
    // only change the expression).
    std::string comment;
    if (p.contains("comment")) {
      comment = p.at("comment").get<std::string>();
    } else if (const Parameter* prior = doc->FindParameter(name)) {
      comment = prior->comment;
    }
    if (!state.commands->Execute(
            std::make_unique<SetVariableCommand>(name, expression, comment),
            *doc)) {
      throw std::runtime_error("invalid variable name: " + name);
    }
    const Parameter* param = doc->FindParameter(name);
    json result = {{"value", param->value}};
    if (!param->error.empty()) result["error"] = param->error;
    return result;
  }
  if (m == "delete_variable") {
    // Refused while referenced, reporting what references it. params: {name}
    const std::string name = p.at("name").get<std::string>();
    if (doc->FindParameter(name) == nullptr)
      throw std::runtime_error("no such variable: " + name);
    std::vector<std::string> refs = doc->FindParameterReferences(name);
    if (!refs.empty()) {
      std::string msg = "variable is referenced by:";
      for (const std::string& r : refs) msg += " " + r + ";";
      throw std::runtime_error(msg);
    }
    state.commands->Execute(std::make_unique<DeleteVariableCommand>(name),
                            *doc);
    return json::object();
  }
  if (m == "rename_variable") {
    // Renames + rewrites every referencing expression. params: {name, new_name}
    if (!state.commands->Execute(
            std::make_unique<RenameVariableCommand>(
                p.at("name").get<std::string>(),
                p.at("new_name").get<std::string>()),
            *doc)) {
      throw std::runtime_error("rename failed (unknown, invalid, or taken)");
    }
    return json::object();
  }
  if (m == "list_variables") {
    json variables = json::array();
    for (const Parameter& param : doc->GetParameters()) {
      json entry = {{"name", param.name},
                    {"expression", param.expression},
                    {"value", param.value}};
      if (!param.comment.empty()) entry["comment"] = param.comment;
      if (!param.error.empty()) entry["error"] = param.error;
      variables.push_back(entry);
    }
    return json{{"variables", variables}};
  }

  if (doc) {
    if (auto r = remote::HandleSimulationRpc(state, *doc, m, p)) return *r;
  }

  if (doc) {
    if (auto r = remote::HandleInspectionRpc(state, *doc, m, p)) return *r;
  }
  return std::nullopt;
}

}  // namespace remote
