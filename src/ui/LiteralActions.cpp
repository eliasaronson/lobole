// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/LiteralActions.h"

#include "core/AssemblySolve.h"
#include "core/Command.h"
#include "core/Document.h"
#include "core/Occurrence.h"
#include "core/io/Serialization.h"
#include "ui/AppActionsInternal.h" // SlotDoc
#include "ui/AppState.h"
#include "ui/AssemblyActions.h" // SnapshotAssembly
#include "ui/VariableActions.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>

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

// ---- An assembly's literals ------------------------------------------------

namespace {

// One unbound number of an assembly, as the lint and the planner read it.
struct AssemblyField {
  std::string owner;
  literals::Field field;
};

std::vector<AssemblyField> AssemblyFields(AssemblyDocument& a) {
  std::vector<AssemblyField> out;
  a.VisitExprs([&](const std::string& owner, const ExprSlot& slot) {
    literals::Field f;
    f.path = slot.Path();
    f.label = slot.Path();
    f.value = slot.Value();
    f.expr = slot.Expr();
    f.quantity = slot.Quantity();
    out.push_back({owner, std::move(f)});
  });
  return out;
}

// The project's variables, each knowing the kinds the parts use it as: a
// part-less Document sees exactly the project scope.
std::vector<literals::Variable> ProjectVariables(AppState& state) {
  static const Document none;
  return literals::VariablesInScope(none, &state.projectParameters,
                                    PartDocuments(state, none));
}

// "mate 'Hinge'" + "mates/0/limits/rotation/1" -> "hinge_rotation_1": a
// name for the variable a literal becomes.
std::string NameFor(const std::string& owner, const std::string& path) {
  std::string base;
  const size_t q = owner.find('\'');
  std::string word = q == std::string::npos
                         ? owner
                         : owner.substr(q + 1, owner.rfind('\'') - q - 1);
  std::string tail;
  {
    // The path's last two meaningful segments: "rotation/1", "offset".
    std::vector<std::string> segs;
    size_t from = 0;
    while (true) {
      const size_t slash = path.find('/', from);
      segs.push_back(path.substr(from, slash - from));
      if (slash == std::string::npos)
        break;
      from = slash + 1;
    }
    size_t keep =
        segs.size() >= 2 && !segs.back().empty() &&
                std::isdigit(static_cast<unsigned char>(segs.back()[0]))
            ? 2
            : 1;
    for (size_t i = segs.size() - keep; i < segs.size(); i++)
      tail += (tail.empty() ? "" : "_") + segs[i];
  }
  for (char c : word + "_" + tail) {
    const unsigned char u = static_cast<unsigned char>(c);
    base += std::isalnum(u) ? static_cast<char>(std::tolower(u)) : '_';
  }
  if (base.empty() || std::isdigit(static_cast<unsigned char>(base[0])))
    base = "v_" + base;
  return base;
}

} // namespace

std::vector<literals::LiteralFinding>
LintAssembly(AppState& state, AssemblyDocument& a, size_t* checked) {
  const std::vector<literals::Variable> vars = ProjectVariables(state);
  std::vector<literals::LiteralFinding> out;
  size_t n = 0;
  for (AssemblyField& f : AssemblyFields(a)) {
    if (!f.field.expr.empty() || f.field.value == 0.0)
      continue;
    n++;
    literals::LiteralFinding finding;
    finding.feature = f.owner;
    finding.field = f.field;
    for (const literals::Variable& v : vars) {
      if (literals::Matches(v, f.field.quantity, f.field.value))
        finding.variables.push_back(v.name);
      else if (literals::Matches(v, f.field.quantity, -f.field.value))
        finding.variables.push_back("-" + v.name);
    }
    if (!finding.variables.empty())
      out.push_back(std::move(finding));
  }
  if (checked != nullptr)
    *checked = n;
  return out;
}

literals::ExtractPlan PlanExtractAssembly(AppState& state, AssemblyDocument& a,
                                          literals::ExtractOptions options) {
  options.taken = TakenVariableNames(state);
  const std::vector<AssemblyField> fields = AssemblyFields(a);
  for (const std::string& want : options.paths)
    if (std::none_of(fields.begin(), fields.end(), [&](const AssemblyField& f) {
          return f.field.path == want;
        })) {
      std::string known;
      for (const AssemblyField& f : fields)
        known += (known.empty() ? "" : ", ") + f.field.path;
      throw std::runtime_error("extract_variables: this assembly has no field "
                               "'" +
                               want + "'; its fields are " + known);
    }
  const std::vector<literals::Variable> vars = ProjectVariables(state);
  literals::ExtractPlan plan;
  std::set<std::string> taken = options.taken;
  for (const AssemblyField& f : fields) {
    if (!options.paths.empty() &&
        std::find(options.paths.begin(), options.paths.end(), f.field.path) ==
            options.paths.end())
      continue;
    if (!f.field.expr.empty())
      continue;
    const auto leave = [&](const std::string& why) {
      literals::LeftAlone l;
      l.feature = f.owner;
      l.path = f.field.path;
      l.label = f.field.label;
      l.value = f.field.value;
      l.reason = why;
      plan.leftAlone.push_back(std::move(l));
    };
    if (f.field.value == 0.0) {
      leave("zero: nothing to name");
      continue;
    }
    if (f.field.quantity == literals::Quantity::Count) {
      leave("a unitless count");
      continue;
    }
    literals::Use use;
    use.feature = f.owner;
    use.field = f.field;
    if (options.reuse) {
      std::vector<std::string> equal;
      for (const literals::Variable& v : vars)
        if (literals::Matches(v, f.field.quantity, f.field.value))
          equal.push_back(v.name);
      if (equal.size() == 1) {
        use.expression = equal[0];
        auto it = std::find_if(plan.proposals.begin(), plan.proposals.end(),
                               [&](const literals::Proposal& p) {
                                 return p.existing && p.name == equal[0];
                               });
        if (it == plan.proposals.end()) {
          literals::Proposal p;
          p.name = equal[0];
          p.value = f.field.value;
          p.quantity = f.field.quantity;
          p.existing = true;
          plan.proposals.push_back(std::move(p));
          it = plan.proposals.end() - 1;
        }
        it->uses.push_back(std::move(use));
        continue;
      }
    }
    // Equal literals of one owner share one variable; of several, only
    // with mergeEqual (equal is not the same as related).
    auto shared = std::find_if(
        plan.proposals.begin(), plan.proposals.end(),
        [&](const literals::Proposal& p) {
          return !p.existing && p.value == f.field.value &&
                 p.quantity == f.field.quantity &&
                 (options.mergeEqual || p.uses.front().feature == f.owner);
        });
    if (shared != plan.proposals.end()) {
      use.expression = shared->name;
      shared->uses.push_back(std::move(use));
      continue;
    }
    std::string name = options.prefix + NameFor(f.owner, f.field.path);
    for (int k = 2; taken.contains(name); k++)
      name = options.prefix + NameFor(f.owner, f.field.path) + "_" +
             std::to_string(k);
    taken.insert(name);
    literals::Proposal p;
    p.name = name;
    p.expression = literals::ExactNumber(f.field.value);
    p.value = f.field.value;
    p.quantity = f.field.quantity;
    use.expression = name;
    p.uses.push_back(std::move(use));
    plan.proposals.push_back(std::move(p));
  }
  return plan;
}

std::string ApplyExtractAssembly(AppState& state, AssemblyDocument& a,
                                 const literals::ExtractPlan& plan) {
  const std::set<std::string> taken = TakenVariableNames(state);
  for (const literals::Proposal& p : plan.proposals)
    if (!p.existing && taken.contains(p.name))
      return "a variable named '" + p.name +
             "' exists now; run the dry run again for fresh names";
  if (plan.proposals.empty())
    return {};
  // Every use must still be a literal slot of the assembly.
  std::map<std::string, std::string> bind; // path -> expression
  for (const literals::Proposal& p : plan.proposals)
    for (const literals::Use& u : p.uses)
      bind[u.field.path] = u.expression;
  size_t found = 0;
  a.VisitExprs([&](const std::string&, const ExprSlot& slot) {
    if (bind.contains(slot.Path()) && !slot.Bound())
      found++;
  });
  if (found != bind.size())
    return "the assembly changed since the plan; run the dry run again";
  PushProjectVarUndoWithFeatures(state, {});
  for (const literals::Proposal& p : plan.proposals)
    if (!p.existing)
      state.projectParameters.push_back(
          {p.name, p.expression, Provenance(p), p.value, {}});
  if (&a == state.assembly.get())
    SnapshotAssembly(state);
  a.VisitExprs([&](const std::string&, const ExprSlot& slot) {
    if (const auto it = bind.find(slot.Path()); it != bind.end())
      slot.Expr() = it->second;
  });
  a.MarkEdited();
  // Evaluated over the new table: the values are the literals they were,
  // so nothing moves.
  SyncProjectScope(state);
  return {};
}

} // namespace ui
