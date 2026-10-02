#include "remote/RemoteTemplates.h"

#include "core/Document.h"
#include "core/Template.h"
#include "core/features/TemplateFeature.h"
#include "ui/AppState.h"
#include "ui/TemplateActions.h"

#include <set>
#include <stdexcept>

using nlohmann::json;

namespace remote {

namespace {

constexpr MethodDoc TemplateMethods[] = {
    {"save_template",
     "Save features of the ACTIVE part as a feature template in the "
     "project's library (docs/modeling/templates.md#saving-one): {name, "
     "feature_ids, literals?: [{feature_id, path, param}] (a literal field "
     "-- a check_literals path such as \"distance\" or \"entities/0/r\" -- "
     "made a parameter), strings?: [{feature_id, path, param, choices?}] (a "
     "string field by JSON pointer, \"/end_mode\"), rename?: {variable: "
     "param}, comments?: {param: text}, comment?, replace?, dry_run?}. "
     "Every variable the features read becomes a parameter (of its name, or "
     "rename's); what a sketch stands on outside the selection becomes the "
     "reference parameter `on`, any other face or edge outside `ref`. "
     "Refused, naming what is wrong: a feature outside the selection named "
     "by id, a field that is not a literal, a template that would not give "
     "back the features it was saved from, a name the library has (without "
     "replace, which makes it that template's next revision). dry_run "
     "answers what Save as template offers (suggestions) and the template it "
     "would save, changing nothing. One undo step with the project "
     "variables.",
     "comment, comments, dry_run, feature_ids, literals, name, rename, "
     "replace, strings",
     remote::NeedsPart,
     "template{name,revision,comment?,parameters[]{name,kind,example,"
     "comment?,choices?},features[]{id,type,name}}, instances_marked[]{part,"
     "part_id,feature_id,name}, suggestions?{variables,literals[]{feature_id,"
     "feature,path,label,kind,value,suggested},references}, error?"},
    {"list_templates",
     "The project's feature templates {name?, definition?}: each one's "
     "parameters (name, kind, the example it was saved with), features and "
     "every instance in the project with `template_changed` (true when the "
     "library's template is not the one it expands: update_template_instances "
     "follows it). definition: true adds the saved JSON.",
     "definition, name", remote::NoFlags,
     "templates[]{name,revision,comment?,parameters[]{name,kind,example,"
     "comment?,choices?},features[]{id,type,name},instances[]{part,part_id,"
     "feature_id,name,revision,template_changed},definition?}"},
    {"delete_template",
     "Remove a template from the library {name}. Its instances keep the "
     "definitions they carry and read as no longer in the library. One undo "
     "step with the project variables.",
     "name", remote::NoFlags},
    {"update_template_instances",
     "Move Template instances of the ACTIVE part to the library's revision "
     "{feature_id | index | template (every instance of it) | all: true, "
     "parameters?: {name: value} for parameters the new revision added}. Each "
     "keeps every value the revision still declares, and every feature it "
     "kept keeps its id, so its faces keep their names. One undo step each. "
     "Never automatic: an instance reads template_changed until this runs. "
     "Returns {updated: [ids], regen, digest}.",
     "all, feature_id, index, parameters, template", remote::NeedsPart},
    {"unlink_template",
     "Turn a Template instance of the ACTIVE part into the plain features it "
     "expands {feature_id | index}: the same ids, so every face keeps its "
     "name and every reference to one still resolves, each expression "
     "reading the document's variables the instance read. One undo step. "
     "Returns {features: [ids], regen, digest}.",
     "feature_id, index", remote::NeedsPart},
    {"export_templates",
     "Write the library's templates (all, or `names`) to a file {path, "
     "names?} (\"designer-templates\" JSON) to carry them to another project.",
     "names, path", remote::NotInBatch},
    {"import_templates",
     "Read a templates file into the library {path, replace?}. A name the "
     "library has is refused unless replace, which makes it that template's "
     "next revision (its instances then read template_changed). One undo "
     "step with the project variables.",
     "path, replace", remote::NotInBatch},
};

json Summary(const tmpl::Template& t) {
  json params = json::array();
  for (const tmpl::Param& p : t.params) {
    json pj = {{"name", p.name},
               {"kind", tmpl::ParamKindName(p.kind)},
               {"example", p.example}};
    if (!p.comment.empty())
      pj["comment"] = p.comment;
    if (!p.choices.empty())
      pj["choices"] = p.choices;
    params.push_back(std::move(pj));
  }
  json features = json::array();
  for (const json& f : t.features)
    features.push_back({{"id", f.value("id", uint64_t{0})},
                        {"type", f.value("type", std::string{})},
                        {"name", f.value("name", std::string{})}});
  json out = {{"name", t.name},
              {"revision", t.revision},
              {"parameters", std::move(params)},
              {"features", std::move(features)}};
  if (!t.comment.empty())
    out["comment"] = t.comment;
  return out;
}

json InstanceJson(const ui::TemplateInstanceRef& r) {
  return {{"part", r.part},
          {"part_id", r.partId},
          {"feature_id", r.featureId},
          {"name", r.feature},
          {"revision", r.revision},
          {"template_changed",
           r.changed.has_value() ? json(*r.changed) : json(nullptr)}};
}

tmpl::SaveSpec SpecFrom(const RpcParams& p) {
  tmpl::SaveSpec spec;
  spec.name = p.at("name").get<std::string>();
  spec.comment = p.value("comment", std::string{});
  if (!p.contains("feature_ids") || !p.at("feature_ids").is_array())
    throw std::runtime_error("save_template: feature_ids is the list of the "
                             "features to save (get_state lists their ids)");
  spec.featureIds = p.at("feature_ids").get<std::vector<uint64_t>>();
  for (const json& l : p.value("literals", json::array())) {
    if (!l.is_object() || !l.contains("feature_id") || !l.contains("path") ||
        !l.contains("param"))
      throw std::runtime_error("save_template: a literal is {feature_id, path, "
                               "param}, not " +
                               l.dump());
    spec.literals.push_back({l.at("feature_id").get<uint64_t>(),
                             l.at("path").get<std::string>(),
                             l.at("param").get<std::string>()});
  }
  for (const json& s : p.value("strings", json::array())) {
    if (!s.is_object() || !s.contains("feature_id") || !s.contains("path") ||
        !s.contains("param"))
      throw std::runtime_error("save_template: a string is {feature_id, path, "
                               "param, choices?}, not " +
                               s.dump());
    spec.strings.push_back({s.at("feature_id").get<uint64_t>(),
                            s.at("path").get<std::string>(),
                            s.at("param").get<std::string>(),
                            s.value("choices", std::vector<std::string>{})});
  }
  const json rename = p.value("rename", json::object());
  if (!rename.is_object())
    throw std::runtime_error("save_template: rename is {variable: param}");
  for (auto it = rename.begin(); it != rename.end(); ++it)
    spec.rename[it.key()] = it.value().get<std::string>();
  const json comments = p.value("comments", json::object());
  if (!comments.is_object())
    throw std::runtime_error("save_template: comments is {param: text}");
  for (auto it = comments.begin(); it != comments.end(); ++it)
    spec.comments[it.key()] = it.value().get<std::string>();
  return spec;
}

json SuggestionsJson(const tmpl::Suggestions& s) {
  json lits = json::array();
  for (const tmpl::Candidate& c : s.literals)
    lits.push_back({{"feature_id", c.featureId},
                    {"feature", c.feature},
                    {"path", c.path},
                    {"label", c.label},
                    {"kind", tmpl::ParamKindName(c.kind)},
                    {"value", c.value},
                    {"suggested", c.suggested}});
  return {{"variables", s.variables},
          {"literals", std::move(lits)},
          {"references", s.references}};
}

// The instance a call names in the active part: feature_id or index.
uint64_t InstanceId(ui::AppState& state, const RpcParams& p) {
  Document& doc = *state.document;
  if (p.contains("feature_id"))
    return p.at("feature_id").get<uint64_t>();
  if (p.contains("index")) {
    const long long index = p.at("index").get<long long>();
    if (index < 0 || static_cast<size_t>(index) >= doc.FeatureCount())
      throw std::runtime_error(IndexError("feature", index, doc.FeatureCount(),
                                          "get_state lists features"));
    return doc.GetFeature(static_cast<size_t>(index))->GetId();
  }
  throw std::runtime_error("name the Template instance: {feature_id} or "
                           "{index}");
}

} // namespace

std::span<const MethodDoc> TemplateMethodDocs() { return TemplateMethods; }

json AddTemplateFeature(ui::AppState& state, Document& doc, const json& fj) {
  static const std::set<std::string> kKeys = {"type",       "name", "template",
                                              "parameters", "on",   "material"};
  for (auto it = fj.begin(); it != fj.end(); ++it)
    if (!kKeys.contains(it.key()))
      throw std::runtime_error("unknown field '" + it.key() +
                               "' for Template. It takes: type, name, "
                               "template, parameters, on, material");
  const json on = fj.value("on", json());
  auto tf = ui::MakeTemplateInstance(state, fj.value("template", std::string{}),
                                     fj.value("parameters", json::object()),
                                     fj.contains("on") ? &on : nullptr,
                                     fj.value("name", std::string{}));
  if (fj.contains("material"))
    tf->SetMaterial(fj.at("material").get<std::string>());
  const json preDigest = BodyDigestJson(doc);
  const size_t index = ui::PlaceTemplateInstance(state, std::move(tf));
  const auto* placed =
      dynamic_cast<const TemplateFeature*>(doc.GetFeature(index));
  json features = json::array();
  json ids = json::array();
  for (const auto& f : placed->Expanded()) {
    features.push_back(
        {{"id", f->GetId()}, {"type", f->TypeName()}, {"name", f->GetName()}});
    ids.push_back(f->GetId());
  }
  return json{{"index", index},
              {"id", placed->GetId()},
              {"ids", std::move(ids)},
              {"features", std::move(features)},
              {"regen", RegenSummaryJson(doc)},
              {"digest", DigestJson(preDigest, doc)}};
}

std::optional<json> HandleTemplateRpc(ui::AppState& state, const std::string& m,
                                      const RpcParams& p) {
  if (m == "save_template") {
    if (!state.document)
      throw std::runtime_error("save_template: open a part");
    const tmpl::SaveSpec spec = SpecFrom(p);
    if (p.value("dry_run", false)) {
      json out = {{"suggestions", SuggestionsJson(tmpl::Suggest(
                                      *state.document, spec.featureIds))}};
      try {
        out["template"] = Summary(tmpl::FromFeatures(*state.document, spec));
      } catch (const std::exception& e) {
        out["error"] = e.what();
      }
      return out;
    }
    std::vector<ui::TemplateInstanceRef> marked;
    const tmpl::Template& t =
        ui::SaveTemplate(state, spec, p.value("replace", false), &marked);
    json m2 = json::array();
    for (const ui::TemplateInstanceRef& r : marked)
      m2.push_back(InstanceJson(r));
    return json{{"template", Summary(t)}, {"instances_marked", std::move(m2)}};
  }
  if (m == "list_templates") {
    const std::string name = p.value("name", std::string{});
    if (!name.empty() && tmpl::Find(state.projectTemplates, name) == nullptr)
      throw std::runtime_error(
          "no template named '" + name + "' (the library has: " +
          tmpl::LibraryNames(state.projectTemplates) + ")");
    const bool definition = p.value("definition", false);
    json out = json::array();
    for (const tmpl::Template& t : state.projectTemplates) {
      if (!name.empty() && t.name != name)
        continue;
      json tj = Summary(t);
      json inst = json::array();
      for (const ui::TemplateInstanceRef& r :
           ui::TemplateInstances(state, t.name))
        inst.push_back(InstanceJson(r));
      tj["instances"] = std::move(inst);
      if (definition)
        tj["definition"] = tmpl::ToJson(t);
      out.push_back(std::move(tj));
    }
    return json{{"templates", std::move(out)}};
  }
  if (m == "delete_template") {
    if (const std::string why =
            ui::DeleteTemplate(state, p.at("name").get<std::string>());
        !why.empty())
      throw std::runtime_error(why);
    return json::object();
  }
  if (m == "update_template_instances") {
    if (!state.document)
      throw std::runtime_error("update_template_instances: open a part");
    Document& doc = *state.document;
    const json extra = p.value("parameters", json::object());
    std::vector<uint64_t> ids;
    if (p.value("all", false) || p.contains("template")) {
      const std::string only = p.value("template", std::string{});
      for (const auto& f : doc.GetFeatures())
        if (const auto* tf = dynamic_cast<const TemplateFeature*>(f.get());
            tf != nullptr && (only.empty() || tf->templateName == only))
          ids.push_back(tf->GetId());
      if (ids.empty())
        throw std::runtime_error(
            only.empty() ? "this part has no Template instances"
                         : "this part has no instance of '" + only + "'");
    } else {
      ids.push_back(InstanceId(state, p));
    }
    const json preDigest = BodyDigestJson(doc);
    json updated = json::array();
    // rpc-params: feature_id, index
    for (uint64_t id : ids) {
      if (const std::string why = ui::UpdateTemplateInstance(state, id, extra);
          !why.empty())
        throw std::runtime_error(why);
      updated.push_back(id);
    }
    return json{{"updated", std::move(updated)},
                {"regen", RegenSummaryJson(doc)},
                {"digest", DigestJson(preDigest, doc)}};
  }
  if (m == "unlink_template") {
    if (!state.document)
      throw std::runtime_error("unlink_template: open a part");
    Document& doc = *state.document;
    const uint64_t id = InstanceId(state, p);
    const json preDigest = BodyDigestJson(doc);
    std::vector<uint64_t> ids;
    if (const std::string why = ui::UnlinkTemplate(state, id, &ids);
        !why.empty())
      throw std::runtime_error(why);
    return json{{"features", ids},
                {"regen", RegenSummaryJson(doc)},
                {"digest", DigestJson(preDigest, doc)}};
  }
  if (m == "export_templates") {
    const std::string path = p.at("path").get<std::string>();
    std::vector<std::string> written;
    if (const std::string why = ui::ExportTemplates(
            state, path, p.value("names", std::vector<std::string>{}),
            &written);
        !why.empty())
      throw std::runtime_error(why);
    return json{{"path", path}, {"templates", written}};
  }
  if (m == "import_templates") {
    std::vector<std::string> imported;
    if (const std::string why =
            ui::ImportTemplates(state, p.at("path").get<std::string>(),
                                p.value("replace", false), &imported);
        !why.empty())
      throw std::runtime_error(why);
    return json{{"imported", imported}};
  }
  return std::nullopt;
}

} // namespace remote
