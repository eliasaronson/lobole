// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/RequirementsPanel.h"

#include "core/Requirements.h"
#include "ui/AppState.h"
#include "ui/BackgroundJobs.h"
#include "ui/DesignSweepPanel.h"
#include "ui/DockLayout.h" // PanelHeading
#include "ui/PanelTitles.h"
#include "ui/Widgets.h" // DrawJobProgress

#include "imgui.h"
#include "imgui_internal.h" // FindWindowByName, the Model panel's dock

#include <cstdio>
#include <map>
#include <string>

// The Requirements panel (docs/requirements/README.md#the-panel). It reads
// the rows list_requirements answers and starts the job check_requirements
// runs, through the engine the RPC module installs, so the panel and an
// agent see the same states and the same evidence.

namespace ui {

namespace {

RequirementsEngine& Engine() {
  static RequirementsEngine engine;
  return engine;
}

ImVec4 StateColour(const std::string& st) {
  if (st == "pass")
    return ImVec4(0.35f, 0.75f, 0.40f, 1.0f);
  if (st == "fail")
    return ImVec4(0.90f, 0.35f, 0.30f, 1.0f);
  if (st == "stale")
    return ImVec4(0.90f, 0.70f, 0.25f, 1.0f);
  return ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
}

std::string Number(double v) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.4g", v);
  return buf;
}

std::map<uint64_t, RequirementRowDrawn>& Drawn() {
  static std::map<uint64_t, RequirementRowDrawn> drawn;
  return drawn;
}

} // namespace

void SetRequirementsEngine(const RequirementsEngine& engine) {
  Engine() = engine;
}

const RequirementsEngine& GetRequirementsEngine() { return Engine(); }

void RefreshRequirementsInPlace(AppState& state) {
  if (Engine().refresh != nullptr && !state.requirements.empty())
    Engine().refresh(state);
}

const RequirementRowDrawn* RequirementRowDrawnForTest(uint64_t id) {
  const auto it = Drawn().find(id);
  return it == Drawn().end() ? nullptr : &it->second;
}

void DrawRequirementsPanel(AppState& state) {
  // The panel whose absence says the project states none.
  if (state.requirements.empty() || Engine().rows == nullptr)
    return;
  // A window that first appears after the dock homes were enforced would
  // float over the viewport: it goes where the Model panel is.
  if (ImGuiWindow* model = ImGui::FindWindowByName(kPanelModel);
      model != nullptr && model->DockId != 0)
    ImGui::SetNextWindowDockID(model->DockId, ImGuiCond_FirstUseEver);
  const bool visible = ImGui::Begin(kPanelRequirements, nullptr,
                                    ImGuiWindowFlags_NoFocusOnAppearing);
  if (!visible) {
    ImGui::End();
    return;
  }
  PanelHeading(kPanelRequirements);

  const nlohmann::json out = Engine().rows(state);
  const nlohmann::json& rows = out.at("requirements");
  const nlohmann::json& sum = out.at("summary");

  BackgroundJob* job = FindBackgroundJob(state, "requirements");
  if (job != nullptr) {
    // The bar every long operation draws (docs/ui/long-operations.md), and
    // with it the way to stop the check.
    JobProgress bar;
    bar.status = job->label;
    bar.fraction = job->fraction.load();
    bar.cancellable = job->cancellable;
    bar.cancelAsked = job->cancel.load();
    bar.cancelId = "###req_cancel";
    bar.cancelTip = "Stop checking: what was measured stays";
    if (DrawJobProgress(bar))
      CancelBackgroundJobs(state, "requirements");
  } else {
    if (ImGui::Button("Check###req_check"))
      Engine().check(state, ImGui::GetIO().KeyShift);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip(
          "Measure every requirement that is not current. A study whose "
          "inputs are unchanged answers from its stored result.\nShift: "
          "measure and run everything again.");
    // The ranges the requirements state, swept: offered when one bounds a
    // bare variable (docs/requirements/README.md#design-sweep).
    bool ranges = false;
    for (const req::Requirement& r : state.requirements)
      ranges = ranges || (r.Kind() == "expression" && r.bound.kind != "pass" &&
                          ExpressionEngine::Default().IsValidParameterName(
                              r.measure.value("expression", std::string())));
    if (ranges) {
      ImGui::SameLine();
      if (ImGui::Button("Sweep the ranges###req_sweep"))
        OpenDesignSweepFromRequirements(state);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Regenerate copies of the model over the ranges "
                          "the requirements state, and find where it stops "
                          "building or meeting them. Changes nothing.");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%d pass, %d fail, %d stale, %d unresolved, %d "
                        "unchecked",
                        sum.value("pass", 0), sum.value("fail", 0),
                        sum.value("stale", 0), sum.value("unresolved", 0),
                        sum.value("unchecked", 0));
  }
  ImGui::Spacing();

  const ImGuiTableFlags flags = ImGuiTableFlags_RowBg |
                                ImGuiTableFlags_BordersInnerH |
                                ImGuiTableFlags_SizingStretchProp;
  // The name takes what is left: a value or a bound clipped to "findi"
  // says nothing, and the name's tooltip and the finding line under it
  // give it back whole.
  Drawn().clear();
  if (ImGui::BeginTable("###req_table", 4, flags)) {
    // The state's width is set rather than fitted: the finding line below
    // a row starts in this column and runs across the table, and a fitted
    // column would grow to it.
    ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed,
                            ImGui::CalcTextSize("unresolved").x);
    ImGui::TableSetupColumn("Requirement", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Bound", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableHeadersRow();
    for (const nlohmann::json& row : rows) {
      const std::string st = row.value("state", std::string());
      const std::string unit = row.value("unit", std::string());
      const uint64_t rid = row.value("id", uint64_t{0});
      const std::string id = std::to_string(rid);
      RequirementRowDrawn& drawn = Drawn()[rid];
      // A failing or unresolved row's whole finding, one a line.
      std::string finding;
      for (const std::string& line : req::FindingLines(row))
        finding += (finding.empty() ? "" : "\n") + line;
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextColored(StateColour(st), "%s", st.c_str());
      ImGui::TableNextColumn();
      ImGui::Selectable(
          (row.value("name", std::string()) + "###req_row" + id).c_str(), false,
          ImGuiSelectableFlags_SpanAllColumns);
      if (ImGui::IsItemHovered()) {
        std::string tip = row.at("measure").value("kind", std::string());
        // An expression says what it reads and where.
        if (tip == "expression") {
          const nlohmann::json& m = row.at("measure");
          tip += ": " + m.value("expression", std::string()) + "\nin " +
                 (m.contains("document")
                      ? "part " + m.at("document").dump() +
                            (m.contains("configuration")
                                 ? ", configuration " +
                                       m.at("configuration").dump()
                                 : std::string(" (its active configuration)"))
                      : std::string("the project's variables"));
        }
        if (row.contains("source"))
          tip += "\n" + row.at("source").get<std::string>();
        if (row.contains("note") && st != "unresolved")
          tip += "\n" + row.at("note").get<std::string>();
        if (!finding.empty())
          tip += "\n" + finding;
        if (row.contains("checked_at"))
          tip += "\nchecked " + row.at("checked_at").get<std::string>() +
                 " (revision " +
                 std::to_string(row.value("evidence_revision", 0)) + ")";
        ImGui::SetTooltip("%s", tip.c_str());
      }
      ImGui::TableNextColumn();
      if (row.contains("value"))
        drawn.value = Number(row.at("value").get<double>()) +
                      (unit.empty() ? "" : " " + unit);
      else if (st == "pass" || st == "fail")
        drawn.value = st == "pass" ? "clean" : "findings";
      drawn.valueCellWidth = ImGui::GetContentRegionAvail().x;
      drawn.valueTextWidth = ImGui::CalcTextSize(drawn.value.c_str()).x;
      if (drawn.value.empty())
        ImGui::TextDisabled("-");
      else
        ImGui::TextUnformatted(drawn.value.c_str());
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(req::BoundText(row).c_str());
      if (!finding.empty()) {
        // Under the row, across the whole table and wrapped to it: nothing
        // to hover for, and nothing cut off however narrow the panel is.
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        const ImGuiTable* table = ImGui::GetCurrentTable();
        const float avail =
            table->WorkRect.Max.x - ImGui::GetCursorScreenPos().x;
        ImGui::PushClipRect(table->InnerClipRect.Min, table->InnerClipRect.Max,
                            false);
        ImGui::PushStyleColor(ImGuiCol_Text, StateColour(st));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + avail);
        ImGui::TextUnformatted(finding.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::PopClipRect();
        drawn.finding = finding;
        drawn.findingWidth = ImGui::GetItemRectSize().x;
        drawn.findingCellWidth = avail;
      }
    }
    ImGui::EndTable();
  }
  ImGui::End();
}

} // namespace ui
