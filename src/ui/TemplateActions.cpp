// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/TemplateActions.h"

#include "core/Command.h"
#include "core/Primitives.h"
#include "core/io/ReadContext.h" // CheckFeatureRoom
#include "core/io/Serialization.h"
#include "ui/AppActionsInternal.h" // SlotDoc
#include "ui/PreviewFeature.h"
#include "ui/VariableActions.h" // PushProjectVarUndoWithFeatures

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace ui {

namespace {

std::string Quoted(const std::string& s) { return "'" + s + "'"; }

// Replaces the instance at `index` with its expanded features (the same
// ids) and back: unlink as ONE undo step.
class UnlinkTemplateCommand : public Command {
public:
  UnlinkTemplateCommand(size_t index, std::vector<nlohmann::json> features)
      : m_index(index), m_features(std::move(features)) {}

  std::string Description() const override { return "Unlink template"; }

  bool Execute(Document& doc) override {
    if (m_index >= doc.FeatureCount())
      return false;
    std::vector<std::unique_ptr<Feature>> plain;
    try {
      for (const nlohmann::json& j : m_features)
        plain.push_back(DocumentIO::FeatureFromJson(j));
    } catch (const std::exception&) {
      return false;
    }
    Document::BulkEdit bulk(doc);
    m_instance = doc.RemoveFeature(m_index);
    for (size_t k = 0; k < plain.size(); k++)
      doc.InsertFeature(std::move(plain[k]), m_index + k);
    return true;
  }

  void Undo(Document& doc) override {
    Document::BulkEdit bulk(doc);
    for (size_t k = 0; k < m_features.size(); k++)
      doc.RemoveFeature(m_index);
    if (m_instance)
      doc.InsertFeature(std::move(m_instance), m_index);
  }

private:
  size_t m_index;
  std::vector<nlohmann::json> m_features;
  std::unique_ptr<Feature> m_instance;
};

const tmpl::Template* Library(const AppState& state, const std::string& name) {
  return tmpl::Find(state.projectTemplates, name);
}

TemplateFeature* ActiveInstance(AppState& state, uint64_t featureId,
                                size_t* index, std::string* why) {
  if (!state.document) {
    *why = "no active part";
    return nullptr;
  }
  Feature* f = state.document->FindFeatureById(featureId, index);
  auto* tf = dynamic_cast<TemplateFeature*>(f);
  if (tf == nullptr)
    *why = f == nullptr ? "no feature with id " + std::to_string(featureId)
                        : Quoted(f->GetName()) + " is a " + f->TypeName() +
                              ", not a Template instance";
  return tf;
}

// The value form a template's example is in, as an instance holds it.
tmpl::Values Examples(const tmpl::Template& t) {
  tmpl::Values v;
  for (const tmpl::Param& p : t.params) {
    tmpl::Value x;
    if (p.kind == tmpl::ParamKind::String)
      x.text = p.example.get<std::string>();
    else if (p.kind == tmpl::ParamKind::Reference)
      x.ref = p.example;
    else
      x.number = p.example.get<double>();
    v[p.name] = x;
  }
  return v;
}

// Checks every reference value against the part as it stands: a sketch
// stands on a planar face, a plane feature or a world plane; anything
// bound at a place in a feature's JSON names a face or an edge that exists.
void CheckReferences(const Document& doc, const tmpl::Template& t,
                     const tmpl::Values& v) {
  for (const tmpl::Param& p : t.params) {
    if (p.kind != tmpl::ParamKind::Reference)
      continue;
    const nlohmann::json& ref = v.at(p.name).ref;
    bool attached = false, placed = false;
    for (const tmpl::Binding& b : t.bindings)
      if (b.param == p.name)
        (b.path == tmpl::kAttach ? attached : placed) = true;
    const primitive::On on = primitive::OnFromJson(ref);
    if (attached) {
      const auto r = primitive::Resolve(doc, on);
      if (!r)
        throw std::runtime_error("template " + Quoted(t.name) + " parameter " +
                                 Quoted(p.name) + ": " + r.error());
    }
    if (placed) {
      if (on.kind != primitive::OnKind::Face)
        throw std::runtime_error(
            "template " + Quoted(t.name) + " parameter " + Quoted(p.name) +
            " lands on a face or an edge: {feature_id, element}");
      bool found = false;
      for (const Body& b : doc.GetBodies())
        if (b.createdBy == on.face.featureId &&
            (b.elementMap.Resolve(on.face.name) ||
             b.elementMap.ResolveEdge(on.face.name)))
          found = true;
      if (!found)
        throw std::runtime_error("template " + Quoted(t.name) + " parameter " +
                                 Quoted(p.name) + ": no face or edge " +
                                 Quoted(on.face.name) + " on feature " +
                                 std::to_string(on.face.featureId) +
                                 "'s body (list_elements names them)");
    }
  }
}

nlohmann::json ReadJsonFile(const std::string& path) {
  std::ifstream in(path);
  if (!in)
    throw std::runtime_error("cannot read " + Quoted(path));
  try {
    return nlohmann::json::parse(in);
  } catch (const std::exception& e) {
    throw std::runtime_error(Quoted(path) + " is not JSON: " + e.what());
  }
}

} // namespace

const tmpl::Template& SaveTemplate(AppState& state, const tmpl::SaveSpec& spec,
                                   bool replace,
                                   std::vector<TemplateInstanceRef>* marked) {
  if (!state.document)
    throw std::runtime_error("save_template works on a part: open one");
  tmpl::Template t = tmpl::FromFeatures(*state.document, spec);
  const tmpl::Template* existing = Library(state, t.name);
  if (existing != nullptr && !replace)
    throw std::runtime_error(
        "the library has a template named " + Quoted(t.name) + " (revision " +
        std::to_string(existing->revision) +
        "): pass replace to make this its revision " +
        std::to_string(existing->revision + 1) + ", or choose another name");
  if (existing != nullptr)
    t.revision = existing->revision + 1;
  PushProjectVarUndoWithFeatures(state, {});
  if (existing != nullptr) {
    for (tmpl::Template& x : state.projectTemplates)
      if (x.name == t.name)
        x = t;
  } else {
    state.projectTemplates.push_back(t);
  }
  if (marked != nullptr)
    for (const TemplateInstanceRef& r : TemplateInstances(state, t.name))
      if (r.changed.value_or(false))
        marked->push_back(r);
  return *Library(state, t.name);
}

std::string DeleteTemplate(AppState& state, const std::string& name) {
  if (Library(state, name) == nullptr)
    return "no template named " + Quoted(name) +
           " (the library has: " + tmpl::LibraryNames(state.projectTemplates) +
           ")";
  PushProjectVarUndoWithFeatures(state, {});
  std::erase_if(state.projectTemplates,
                [&](const tmpl::Template& t) { return t.name == name; });
  return {};
}

std::optional<bool> TemplateChanged(const AppState& state,
                                    const TemplateFeature& instance) {
  const tmpl::Template* t = Library(state, instance.templateName);
  if (t == nullptr)
    return std::nullopt;
  return !(*t == instance.definition);
}

std::vector<TemplateInstanceRef> TemplateInstances(AppState& state,
                                                   const std::string& name) {
  std::vector<TemplateInstanceRef> out;
  for (size_t i = 0; i < state.parts.size(); i++) {
    Document* doc = SlotDoc(state, i);
    if (doc == nullptr)
      continue;
    for (const auto& f : doc->GetFeatures()) {
      const auto* tf = dynamic_cast<const TemplateFeature*>(f.get());
      if (tf == nullptr || (!name.empty() && tf->templateName != name))
        continue;
      out.push_back({state.parts[i].id, state.parts[i].name, tf->GetId(),
                     tf->GetName(), tf->definition.revision,
                     TemplateChanged(state, *tf)});
    }
  }
  return out;
}

std::unique_ptr<TemplateFeature>
MakeTemplateInstance(AppState& state, const std::string& name,
                     const nlohmann::json& params, const nlohmann::json* on,
                     const std::string& featureName) {
  if (!state.document)
    throw std::runtime_error("a template is placed in a part: open one");
  Document& doc = *state.document;
  if (name.empty())
    throw std::runtime_error("Template: name the template (the library has: " +
                             tmpl::LibraryNames(state.projectTemplates) + ")");
  const tmpl::Template* t = Library(state, name);
  if (t == nullptr)
    throw std::runtime_error("no template named " + Quoted(name) +
                             " (the library has: " +
                             tmpl::LibraryNames(state.projectTemplates) + ")");
  auto tf = std::make_unique<TemplateFeature>(
      featureName.empty() ? t->name : featureName);
  tf->templateName = t->name;
  tf->definition = *t;
  tf->values = tmpl::ValuesFromRequest(*t, params, on);
  // The numbers given as expressions, against the part's variables: an
  // unknown one is refused here rather than committed.
  const SymbolTable symbols = primitive::Symbols(doc);
  for (auto& [pname, v] : tf->values) {
    if (v.expr.empty())
      continue;
    const auto r = ExpressionEngine::Default().EvaluateText(v.expr, symbols);
    if (!r)
      throw std::runtime_error("template " + Quoted(t->name) + " parameter " +
                               Quoted(pname) + " '" + v.expr +
                               "': " + r.error().message);
    v.number = *r;
  }
  if (const std::string why = tmpl::CheckEvaluated(*t, tf->values);
      !why.empty())
    throw std::runtime_error("template " + Quoted(t->name) + ": " + why);
  CheckReferences(doc, *t, tf->values);
  tf->AssignIds([&doc] { return doc.AllocateId(); });
  return tf;
}

size_t PlaceTemplateInstance(AppState& state,
                             std::unique_ptr<TemplateFeature> instance) {
  Document& doc = *state.document;
  if (const std::string why = readctx::CheckFeatureRoom(doc, 1); !why.empty())
    throw std::runtime_error(why);
  const size_t depth = state.commands->UndoDepth();
  state.commands->Execute(
      std::make_unique<AddFeatureCommand>(std::move(instance)), doc);
  const size_t index = doc.FeatureCount() - 1;
  const Feature* added = doc.GetFeature(index);
  if (!added->GetError().empty() && !added->ErrorAfterEarlierFailure()) {
    const std::string why = added->GetError();
    // Taken back out and forgotten: no redo brings a refused instance back.
    state.commands->RollbackSince(depth, doc);
    throw std::runtime_error(why);
  }
  return index;
}

std::string UpdateTemplateInstance(AppState& state, uint64_t featureId,
                                   const nlohmann::json& extra) {
  size_t index = 0;
  std::string why;
  TemplateFeature* tf = ActiveInstance(state, featureId, &index, &why);
  if (tf == nullptr)
    return why;
  const tmpl::Template* t = Library(state, tf->templateName);
  if (t == nullptr)
    return "the library has no template " + Quoted(tf->templateName) +
           " to update " + Quoted(tf->GetName()) +
           " from (it has: " + tmpl::LibraryNames(state.projectTemplates) + ")";
  if (*t == tf->definition && (extra.is_null() || extra.empty()))
    return {}; // nothing to do
  // The extra values against the NEW definition: a name it does not
  // declare is refused like any other.
  tmpl::Values more;
  if (extra.is_object())
    for (auto it = extra.begin(); it != extra.end(); ++it) {
      const tmpl::Param* p = t->FindParam(it.key());
      if (p == nullptr)
        return "revision " + std::to_string(t->revision) + " of " +
               Quoted(t->name) + " has no parameter " + Quoted(it.key());
      tmpl::Template one = *t;
      one.params = {*p};
      try {
        nlohmann::json single = {{it.key(), it.value()}};
        more[it.key()] =
            tmpl::ValuesFromRequest(one, single, nullptr).at(it.key());
      } catch (const std::exception& e) {
        return e.what();
      }
    }
  const nlohmann::json before = DocumentIO::FeatureToJson(*tf);
  TemplateFeature next = *tf;
  Document& doc = *state.document;
  if (std::string w =
          next.UpdateTo(*t, more, [&doc] { return doc.AllocateId(); });
      !w.empty())
    return w;
  const nlohmann::json after = DocumentIO::FeatureToJson(next);
  if (!state.commands->Execute(
          std::make_unique<ModifyFeatureCommand>(index, before, after), doc))
    return "the update did not apply";
  return {};
}

std::string UnlinkTemplate(AppState& state, uint64_t featureId,
                           std::vector<uint64_t>* ids) {
  size_t index = 0;
  std::string why;
  TemplateFeature* tf = ActiveInstance(state, featureId, &index, &why);
  if (tf == nullptr)
    return why;
  // A part opened from baked geometry executed nothing, so the instance has
  // no features to hand over yet: build the part once, for real.
  if (tf->Expanded().empty() && tf->GetError().empty()) {
    state.document->ClearRegenCache();
    state.document->Regenerate();
    tf = ActiveInstance(state, featureId, &index, &why);
    if (tf == nullptr)
      return why;
  }
  std::vector<std::unique_ptr<Feature>> plain = tf->Unlinked(&why);
  if (plain.empty())
    return Quoted(tf->GetName()) + ": " + why;
  // The instance's features take its one place in the list, and more.
  if (std::string full =
          readctx::CheckFeatureRoom(*state.document, plain.size() - 1);
      !full.empty())
    return full;
  std::vector<nlohmann::json> features;
  for (const auto& f : plain) {
    features.push_back(DocumentIO::FeatureToJson(*f));
    if (ids)
      ids->push_back(f->GetId());
  }
  if (!state.commands->Execute(
          std::make_unique<UnlinkTemplateCommand>(index, std::move(features)),
          *state.document))
    return "the unlink did not apply";
  return {};
}

std::string ExportTemplates(AppState& state, const std::string& path,
                            const std::vector<std::string>& names,
                            std::vector<std::string>* written) {
  if (state.projectTemplates.empty())
    return "the library has no templates to export";
  std::vector<tmpl::Template> chosen;
  for (const std::string& n : names) {
    const tmpl::Template* t = Library(state, n);
    if (t == nullptr)
      return "no template named " + Quoted(n) + " (the library has: " +
             tmpl::LibraryNames(state.projectTemplates) + ")";
    chosen.push_back(*t);
  }
  if (names.empty())
    chosen = state.projectTemplates;
  const nlohmann::json j = {{"format", "designer-templates"},
                            {"version", 1},
                            {"templates", tmpl::LibraryToJson(chosen)}};
  std::string error;
  if (!DocumentIO::WriteTextAtomic(std::filesystem::path(path), j.dump(2),
                                   &error))
    return error.empty() ? "cannot write " + Quoted(path) : error;
  if (written)
    for (const tmpl::Template& t : chosen)
      written->push_back(t.name);
  return {};
}

std::string ImportTemplates(AppState& state, const std::string& path,
                            bool replace, std::vector<std::string>* imported) {
  std::vector<tmpl::Template> incoming;
  try {
    const nlohmann::json j = ReadJsonFile(path);
    if (!j.is_object() ||
        j.value("format", std::string{}) != "designer-templates")
      return Quoted(path) +
             " is not a template file (format \"designer-templates\")";
    if (j.value("version", 0) != 1)
      return Quoted(path) + " is template file version " +
             std::to_string(j.value("version", 0)) +
             "; this build reads version 1";
    incoming = tmpl::LibraryFromJson(j.value("templates", nlohmann::json()));
  } catch (const std::exception& e) {
    return e.what();
  }
  if (incoming.empty())
    return Quoted(path) + " holds no templates";
  if (!replace) {
    std::string clash;
    for (const tmpl::Template& t : incoming)
      if (Library(state, t.name) != nullptr)
        clash += (clash.empty() ? "" : ", ") + Quoted(t.name);
    if (!clash.empty())
      return "the library already has " + clash +
             ": pass replace to make each the next revision of the one it "
             "has, or rename it first";
  }
  PushProjectVarUndoWithFeatures(state, {});
  for (tmpl::Template& t : incoming) {
    auto it = std::find_if(
        state.projectTemplates.begin(), state.projectTemplates.end(),
        [&](const tmpl::Template& x) { return x.name == t.name; });
    if (it != state.projectTemplates.end()) {
      if (*it == t)
        continue; // the same template: nothing changes
      t.revision = it->revision + 1;
      *it = t;
    } else {
      state.projectTemplates.push_back(t);
    }
    if (imported)
      imported->push_back(t.name);
  }
  return {};
}

void CreateTemplateInstance(AppState& state) {
  if (!state.document)
    return;
  if (HasPendingFeature(state))
    CancelPendingFeature(state);
  state.sketchEdit = SketchEditState{}; // reset FIRST, then arm
  state.sketchEdit.pickingPlane = true;
  state.sketchEdit.purpose = SketchPurpose::Template;
  // With nothing to insert the pick still opens -- its window is where the
  // refusal is read -- and every pick refuses with the same words.
  state.templateTool.refusal =
      state.projectTemplates.empty()
          ? "The project has no templates yet: select the features of one in "
            "the tree and choose Save as template."
          : "";
  state.sketchEdit.actionMsg = state.templateTool.refusal;
}

void BeginTemplatePreview(AppState& state, const nlohmann::json& on) {
  if (!state.document)
    return;
  if (state.projectTemplates.empty()) {
    state.sketchEdit.actionMsg = state.templateTool.refusal =
        "The project has no templates yet: select the features of one in "
        "the tree and choose Save as template.";
    return; // still armed
  }
  const tmpl::Template* t = Library(state, state.templateTool.last);
  if (t == nullptr)
    t = &state.projectTemplates.front();
  // The saved examples, standing on what was picked: the template's own
  // values, not a literal of this code's.
  nlohmann::json params = nlohmann::json::object();
  for (const tmpl::Param& p : t->params)
    if (p.name != "on")
      params[p.name] = p.example;
  std::unique_ptr<TemplateFeature> tf;
  try {
    tf = MakeTemplateInstance(state, t->name, params, &on, t->name);
  } catch (const std::exception& e) {
    state.sketchEdit.actionMsg = e.what(); // still armed
    return;
  }
  state.sketchEdit = SketchEditState{}; // the pick is answered
  state.selection.Clear();
  state.templateTool.last = t->name;
  state.templateTool.on = on;
  BeginPreviewFeature(state, std::move(tf));
}

} // namespace ui
