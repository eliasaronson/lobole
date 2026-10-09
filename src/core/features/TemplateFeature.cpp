// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/features/TemplateFeature.h"

#include "core/Primitives.h"

#include <algorithm>
#include <set>

namespace {

std::string Quoted(const std::string& s) { return "'" + s + "'"; }

// Why a reference value no longer names anything, or empty.
std::string LostReference(const RegenContext& ctx, const std::string& param,
                          const nlohmann::json& ref) {
  if (!ref.is_object() || !ref.contains("feature_id"))
    return {}; // a world plane does not go away
  const uint64_t id = ref.at("feature_id").get<uint64_t>();
  if (ref.contains("element")) {
    const std::string name = ref.at("element").get<std::string>();
    for (const Body& b : ctx.bodies) {
      if (b.createdBy != id)
        continue;
      if (b.elementMap.Resolve(name) || b.elementMap.ResolveEdge(name))
        return {};
    }
    return "the face or edge it stands on (" + param + ": " + Quoted(name) +
           " of feature " + std::to_string(id) +
           ") is gone; pick what it stands on again (modify_feature)";
  }
  if (ctx.FindById(id) == nullptr)
    return "the plane it stands on (" + param + ": feature " +
           std::to_string(id) + ") is gone or suppressed";
  return {};
}

} // namespace

void TemplateFeature::VisitExprs(const ExprVisitor& visit) {
  for (const tmpl::Param& p : definition.params) {
    if (p.kind == tmpl::ParamKind::String ||
        p.kind == tmpl::ParamKind::Reference)
      continue;
    const auto it = values.find(p.name);
    if (it != values.end())
      visit(it->second.expr, it->second.number);
  }
}

void TemplateFeature::VisitFeatureRefs(const RefVisitor& visit) const {
  for (const auto& [name, v] : values)
    if (v.ref.is_object() && v.ref.contains("feature_id"))
      visit(v.ref.at("feature_id").get<uint64_t>());
}

void TemplateFeature::VisitOwnedIds(const RefVisitor& visit) const {
  for (const auto& [local, id] : ids)
    visit(id);
}

std::string TemplateFeature::WhyIncomplete() const {
  if (definition.features.empty())
    return "it names no template (add_feature {type: \"Template\", "
           "template: <name>}; list_templates names them)";
  std::string missing;
  for (const tmpl::Param& p : definition.params)
    if (!values.contains(p.name))
      missing += (missing.empty() ? "" : ", ") + p.name;
  if (!missing.empty())
    return "parameters without a value: " + missing;
  for (uint64_t local : tmpl::LocalIds(definition))
    if (IdOf(local) == 0)
      return "template feature " + std::to_string(local) +
             " has no id of its own (update the instance)";
  return {};
}

uint64_t TemplateFeature::IdOf(uint64_t local) const {
  for (const auto& [l, id] : ids)
    if (l == local)
      return id;
  return 0;
}

void TemplateFeature::AssignIds(const std::function<uint64_t()>& allocate) {
  const std::vector<uint64_t> locals = tmpl::LocalIds(definition);
  std::vector<std::pair<uint64_t, uint64_t>> next;
  for (uint64_t local : locals) {
    const uint64_t have = IdOf(local);
    next.emplace_back(local, have != 0 ? have : allocate());
  }
  ids = std::move(next);
}

std::string
TemplateFeature::UpdateTo(const tmpl::Template& next, const tmpl::Values& extra,
                          const std::function<uint64_t()>& allocate) {
  tmpl::Values kept;
  std::string missing;
  for (const tmpl::Param& p : next.params) {
    if (const auto e = extra.find(p.name); e != extra.end()) {
      kept[p.name] = e->second;
      continue;
    }
    const tmpl::Param* was = definition.FindParam(p.name);
    const auto have = values.find(p.name);
    if (was != nullptr && was->kind == p.kind && have != values.end()) {
      kept[p.name] = have->second;
      continue;
    }
    missing += (missing.empty() ? "" : ", ") + p.name + " (" +
               tmpl::ParamKindName(p.kind) + ")";
  }
  if (!missing.empty())
    return "revision " + std::to_string(next.revision) + " of template " +
           Quoted(next.name) +
           " has parameters this instance gives no "
           "value: " +
           missing;
  definition = next;
  values = std::move(kept);
  AssignIds(allocate);
  return {};
}

bool TemplateFeature::Execute(RegenContext& ctx) {
  m_expanded.clear();
  if (const std::string why = WhyIncomplete(); !why.empty()) {
    SetError(why);
    return false;
  }
  if (const std::string why = tmpl::CheckEvaluated(definition, values);
      !why.empty()) {
    SetError("template " + Quoted(templateName) + ": " + why);
    return false;
  }
  // A lost reference is the instance's error, in its own words, never a
  // fall-back to anywhere else.
  for (const tmpl::Param& p : definition.params) {
    if (p.kind != tmpl::ParamKind::Reference)
      continue;
    if (const std::string why =
            LostReference(ctx, p.name, values.at(p.name).ref);
        !why.empty()) {
      SetError(why);
      return false;
    }
  }

  const std::vector<Body> before = ctx.bodies;
  RegenContext inner = ctx;
  inner.inner = &m_expanded;
  std::string warnings;
  const auto fail = [&](const std::string& why) {
    ctx.bodies = before;
    m_expanded.clear();
    SetError("template " + Quoted(templateName) + ": " + why);
    return false;
  };
  for (size_t k = 0; k < definition.features.size(); k++) {
    auto built = tmpl::BuildFeature(
        definition, k, values, [this](uint64_t l) { return IdOf(l); },
        tmpl::HostIn(ctx.bodies));
    if (!built)
      return fail(built.error());
    m_expanded.push_back(std::move(*built));
    Feature& f = *m_expanded.back();
    inner.innerIndex = k; // its own features before it, and the document's
    f.ClearError();
    f.ClearWarning();
    if (!f.Execute(inner))
      return fail(Quoted(f.GetName()) + " (" + f.TypeName() + "): " +
                  (f.GetError().empty() ? std::string("regeneration failed")
                                        : f.GetError()));
    if (!f.GetWarning().empty())
      warnings += (warnings.empty() ? "" : "; ") + Quoted(f.GetName()) + ": " +
                  f.GetWarning();
    FollowModifiedFaces(ctx.bodies);
  }
  if (!warnings.empty())
    SetWarning(warnings);
  return true;
}

nlohmann::json TemplateFeature::ExpandedRows() const {
  nlohmann::json out = nlohmann::json::array();
  for (const nlohmann::json& f : definition.features) {
    const uint64_t local = f.value("id", uint64_t{0});
    out.push_back({{"id", IdOf(local)},
                   {"type", f.value("type", std::string{})},
                   {"name", f.value("name", std::string{})}});
  }
  return out;
}

std::vector<std::unique_ptr<Feature>>
TemplateFeature::Unlinked(std::string* why) const {
  std::vector<std::unique_ptr<Feature>> out;
  if (m_expanded.size() != definition.features.size()) {
    if (why)
      *why = "the instance has not built (" +
             (GetError().empty() ? std::string("not regenerated yet")
                                 : GetError()) +
             "): fix it first, so the features it hands over are the ones "
             "it builds";
    return {};
  }
  for (const auto& f : m_expanded) {
    std::unique_ptr<Feature> copy = f->Clone();
    copy->ClearError();
    copy->ClearWarning();
    copy->VisitExprs([&](std::string& text, double&) {
      text = tmpl::SubstituteParams(definition, values, text);
    });
    out.push_back(std::move(copy));
  }
  return out;
}
