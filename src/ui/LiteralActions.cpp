#include "ui/LiteralActions.h"

#include "core/Command.h"
#include "core/Document.h"
#include "core/io/Serialization.h"
#include "ui/AppActionsInternal.h" // SlotDoc
#include "ui/AppState.h"
#include "ui/VariableActions.h"

#include <algorithm>
#include <map>
#include <memory>

namespace ui {

namespace {

std::vector<const Document*> PartDocuments(AppState& state,
                                           const Document& also) {
  std::vector<const Document*> out;
  for (size_t i = 0; i < state.parts.size(); i++)
    if (state.parts[i].kind == DocKind::Part)
      if (const Document* d = SlotDoc(state, i))
        out.push_back(d);
  if (std::find(out.begin(), out.end(), &also) == out.end())
    out.push_back(&also);
  return out;
}

// The comment a new variable carries: where its number came from.
std::string Provenance(const literals::Proposal& p) {
  if (p.uses.size() == 1)
    return std::string("extracted from ") + p.uses[0].feature + " " +
           p.uses[0].field.label + " (" + literals::QuantityName(p.quantity) +
           ")";
  return "extracted from " + std::to_string(p.uses.size()) + " fields (" +
         literals::QuantityName(p.quantity) + ")";
}

// The touched features that are broken now and were not before.
std::string NewlyBroken(const Document& doc,
                        const std::map<uint64_t, std::string>& before) {
  for (const auto& [id, error] : before) {
    const Feature* f = doc.FindFeatureById(id);
    if (f != nullptr && error.empty() && !f->GetError().empty())
      return "'" + f->GetName() + "': " + f->GetError();
  }
  return {};
}

} // namespace

std::vector<literals::Variable> LintVariables(AppState& state,
                                              const Document& doc) {
  return literals::VariablesInScope(doc, &state.projectParameters,
                                    PartDocuments(state, doc));
}

std::set<std::string> TakenVariableNames(AppState& state) {
  std::set<std::string> taken;
  for (const Parameter& p : state.projectParameters)
    taken.insert(p.name);
  for (size_t i = 0; i < state.parts.size(); i++)
    if (state.parts[i].kind == DocKind::Part)
      if (const Document* d = SlotDoc(state, i))
        for (const Parameter& p : d->GetParameters())
          taken.insert(p.name);
  return taken;
}

literals::Report LintDocument(AppState& state, const Document& doc) {
  return literals::Lint(doc, LintVariables(state, doc));
}

literals::ExtractPlan PlanExtractVariables(AppState& state,
                                           literals::ExtractOptions options) {
  Document* doc = state.document.get();
  if (doc == nullptr)
    throw std::runtime_error(
        "extract_variables works on a part: switch to a part tab first");
  options.taken = TakenVariableNames(state);
  return literals::PlanExtract(*doc, LintVariables(state, *doc), options);
}

std::string ApplyExtractVariables(AppState& state,
                                  const literals::ExtractPlan& plan,
                                  bool projectScope) {
  Document* doc = state.document.get();
  if (doc == nullptr || state.activePart < 0 ||
      static_cast<size_t>(state.activePart) >= state.parts.size())
    return "extract_variables works on a part: switch to a part tab first";

  // A new name must still be free: the plan may be older than the table.
  const std::set<std::string> taken = TakenVariableNames(state);
  for (const literals::Proposal& p : plan.proposals)
    if (!p.existing && taken.contains(p.name))
      return "a variable named '" + p.name +
             "' exists now; run the dry run again for fresh names";

  // Each touched feature's spec before and after, built on copies so a
  // field that has gone leaves the document untouched.
  struct Edit {
    uint64_t id = 0;
    size_t index = 0;
    nlohmann::json before, after;
  };
  std::map<uint64_t, std::vector<const literals::Use*>> byFeature;
  for (const literals::Proposal& p : plan.proposals)
    for (const literals::Use& u : p.uses)
      byFeature[u.featureId].push_back(&u);
  if (byFeature.empty())
    return {}; // nothing to do is not a failure
  std::vector<Edit> edits;
  std::map<uint64_t, std::string> errorsBefore;
  for (const auto& [id, uses] : byFeature) {
    size_t index = 0;
    const Feature* f = doc->FindFeatureById(id, &index);
    if (f == nullptr)
      return "feature " + std::to_string(id) +
             " is gone; run the dry run again";
    std::unique_ptr<Feature> copy = f->Clone();
    for (const literals::Use* u : uses)
      if (!literals::SetFieldExpr(*copy, u->field.path, u->expression))
        return "'" + f->GetName() + "' has no field " + u->field.path +
               " now; run the dry run again";
    edits.push_back({id, index, DocumentIO::FeatureToJson(*f),
                     DocumentIO::FeatureToJson(*copy)});
    errorsBefore[id] = f->GetError();
  }

  if (projectScope) {
    const uint64_t partId = state.parts[state.activePart].id;
    std::vector<ProjectVarSnapshot::FeatureState> restore;
    for (const Edit& e : edits)
      restore.push_back({partId, e.id, e.before});
    PushProjectVarUndoWithFeatures(state, std::move(restore));
    for (const literals::Proposal& p : plan.proposals)
      if (!p.existing)
        state.projectParameters.push_back(
            {p.name, p.expression, Provenance(p), p.value, {}});
    {
      // One regeneration of this part, after the new variables evaluate.
      Document::BulkEdit bulk(*doc);
      for (const Edit& e : edits)
        doc->ReplaceFeature(e.index, DocumentIO::FeatureFromJson(e.after));
      SyncProjectScope(state);
    }
    if (std::string why = NewlyBroken(*doc, errorsBefore); !why.empty()) {
      UndoProjectVars(state);
      state.projectVarHistory.redo.pop_back();
      return "the extraction would break " + why + "; nothing was changed";
    }
    return {};
  }

  CommandStack& commands = *state.commands;
  const size_t depth = commands.UndoDepth();
  for (const literals::Proposal& p : plan.proposals)
    if (!p.existing &&
        !commands.Execute(std::make_unique<SetVariableCommand>(
                              p.name, p.expression, Provenance(p)),
                          *doc)) {
      commands.RollbackSince(depth, *doc);
      return "the variable '" + p.name +
             "' could not be set; nothing was "
             "changed";
    }
  for (const Edit& e : edits)
    if (!commands.Execute(
            std::make_unique<ModifyFeatureCommand>(e.index, e.before, e.after),
            *doc)) {
      commands.RollbackSince(depth, *doc);
      return "feature " + std::to_string(e.id) +
             " did not take its expressions; nothing was changed";
    }
  if (std::string why = NewlyBroken(*doc, errorsBefore); !why.empty()) {
    commands.RollbackSince(depth, *doc);
    return "the extraction would break " + why + "; nothing was changed";
  }
  commands.CoalesceSince(depth, "Extract variables");
  return {};
}

} // namespace ui
