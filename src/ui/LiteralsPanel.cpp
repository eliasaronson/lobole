// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/LiteralsPanel.h"

#include "core/Document.h"
#include "core/Literals.h"
#include "ui/AppState.h"
#include "ui/LiteralActions.h"
#include "ui/Widgets.h"

#include "imgui.h"

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>

// The Literals section of the Variables panel: the same lint and the same
// extraction the check_literals / extract_variables RPCs run
// (ui/LiteralActions.h), so a person and an agent are told the same things.

namespace ui {

namespace {

struct LiteralsState {
  // The last check, and which document at which edit it describes: a
  // report is only shown against the part it was taken of.
  std::optional<literals::Report> report;
  uint64_t instance = 0, stamp = 0;
  // The pending dry run, until it is applied or dropped.
  std::optional<literals::ExtractPlan> plan;
  uint64_t planInstance = 0; // the document it was planned on
  bool projectScope = true;
  std::string error;
};

LiteralsState& S() {
  static LiteralsState s;
  return s;
}

void Check(AppState& state, const Document& doc) {
  S().report = LintDocument(state, doc);
  S().instance = doc.InstanceId();
  S().stamp = doc.SaveStamp();
}

std::string Number(double v) { return literals::ExactNumber(v); }

} // namespace

void DrawLiteralsSection(AppState& state) {
  LiteralsState& s = S();
  Document* doc = state.document.get();
  const bool current =
      doc != nullptr && s.report && s.instance == doc->InstanceId();
  const int count = current ? static_cast<int>(s.report->literals.size() +
                                               s.report->placements.size())
                            : -1;
  if (!SectionHeader("Literals###literals_header", count))
    return;
  if (doc == nullptr) {
    ImGui::TextDisabled("A part's literals: open a part tab.");
    return;
  }

  if (ImGui::Button("Check###literals_check")) {
    s.error.clear();
    Check(state, *doc);
  }
  ImGui::SameLine();
  ImGui::TextDisabled("numbers that equal a variable, sketches on the world");

  if (current) {
    if (s.stamp != doc->SaveStamp())
      ImGui::TextDisabled("The part has changed since: Check again.");
    const literals::Report& r = *s.report;
    if (r.literals.empty() && r.placements.empty())
      ImGui::TextDisabled("Nothing found in %zu numbers and %zu placements.",
                          r.fieldsChecked, r.placementsChecked);
    for (size_t k = 0; k < r.literals.size(); k++) {
      const literals::LiteralFinding& f = r.literals[k];
      std::string vars;
      for (const std::string& v : f.variables)
        vars += (vars.empty() ? "" : ", ") + v;
      ImGui::TextWrapped("%s: %s = %s, which is %s", f.feature.c_str(),
                         f.field.label.c_str(), Number(f.field.value).c_str(),
                         vars.c_str());
      // Binding is offered when the literal equals exactly ONE variable;
      // two candidates are a choice the person makes in the feature.
      if (f.variables.size() == 1) {
        ImGui::SameLine();
        // Addressed by WHAT it binds, not by its row: "/" separates a
        // test-engine path, so the field's path is spelled with dots.
        std::string field = f.field.path;
        std::replace(field.begin(), field.end(), '/', '.');
        const std::string id =
            "Use###literal_use_" + std::to_string(f.featureId) + "_" + field;
        if (ImGui::SmallButton(id.c_str())) {
          try {
            literals::ExtractOptions o;
            o.featureIds = {f.featureId};
            o.paths = {f.field.path};
            o.reuse = true;
            s.error = ApplyExtractVariables(
                state, PlanExtractVariables(state, o), s.projectScope);
          } catch (const std::exception& e) {
            s.error = e.what();
          }
          Check(state, *doc);
          break; // the report just changed under this loop
        }
      }
    }
    for (const literals::PlacementFinding& p : r.placements)
      ImGui::TextWrapped(
          "%s sits on %s, where face %s of %s lies: attach it to that "
          "face to follow the part.",
          p.feature.c_str(), p.on.c_str(), p.face.name.c_str(),
          p.faceFeature.c_str());
  }

  ImGui::Spacing();
  ImGui::TextUnformatted("New variables in");
  ImGui::SameLine();
  if (ImGui::RadioButton("project###literals_scope_project", s.projectScope))
    s.projectScope = true;
  ImGui::SameLine();
  if (ImGui::RadioButton("part###literals_scope_part", !s.projectScope))
    s.projectScope = false;
  // The selected features, or every feature when none is selected.
  if (ImGui::Button("Extract variables###literals_extract")) {
    literals::ExtractOptions o;
    for (const SelectionItem& it : state.selection.items)
      if (it.kind == SelectionItem::Kind::Feature)
        o.featureIds.push_back(it.id);
    try {
      s.plan = PlanExtractVariables(state, o);
      s.planInstance = doc->InstanceId();
      s.error.clear();
    } catch (const std::exception& e) {
      s.plan.reset();
      s.error = e.what();
    }
  }
  if (s.plan && s.planInstance != doc->InstanceId())
    s.plan.reset(); // planned on another document
  if (s.plan) {
    if (s.plan->proposals.empty())
      ImGui::TextDisabled("No literal to extract (%zu left alone).",
                          s.plan->leftAlone.size());
    for (const literals::Proposal& p : s.plan->proposals) {
      std::string uses;
      for (const literals::Use& u : p.uses)
        uses += (uses.empty() ? "" : ", ") + u.feature + " " + u.field.label;
      ImGui::BulletText("%s = %s  (%s)", p.name.c_str(), p.expression.c_str(),
                        uses.c_str());
    }
    if (!s.plan->proposals.empty() &&
        ImGui::Button("Apply###literals_extract_apply")) {
      s.error = ApplyExtractVariables(state, *s.plan, s.projectScope);
      s.plan.reset();
      if (current)
        Check(state, *doc);
    }
    if (s.plan) {
      ImGui::SameLine();
      if (ImGui::Button("Cancel###literals_extract_cancel"))
        s.plan.reset();
    }
  }
  if (!s.error.empty())
    ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.4f, 1.0f), "%s", s.error.c_str());
}

} // namespace ui
