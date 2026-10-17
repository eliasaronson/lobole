// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/RequirementsPanel.h"

#include "ui/AppState.h"
#include "ui/BackgroundJobs.h"
#include "ui/DockLayout.h" // PanelHeading
#include "ui/PanelTitles.h"
#include "ui/Widgets.h" // DrawJobProgress

#include "imgui.h"
#include "imgui_internal.h" // FindWindowByName, the Model panel's dock

#include <cstdio>
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

std::string BoundText(const nlohmann::json& b, const std::string& unit,
                      const nlohmann::json& row) {
  const std::string kind = b.value("kind", std::string());
  const auto side = [&](const char* key, const char* evaluated) {
    if (row.contains(evaluated))
      return Number(row.at(evaluated).get<double>());
    const nlohmann::json& v = b.at(key);
    return v.is_number() ? Number(v.get<double>()) : v.get<std::string>();
  };
  const std::string u = unit.empty() ? "" : " " + unit;
  if (kind == "at_most")
    return "<= " + side("value", "limit") + u;
  if (kind == "at_least")
    return ">= " + side("value", "limit") + u;
  if (kind == "within")
    return side("min", "limit_min") + " .. " + side("max", "limit_max") + u;
  return "passes";
}

} // namespace

void SetRequirementsEngine(const RequirementsEngine& engine) {
  Engine() = engine;
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
  if (ImGui::BeginTable("###req_table", 4, flags)) {
    ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Requirement");
    ImGui::TableSetupColumn("Value");
    ImGui::TableSetupColumn("Bound");
    ImGui::TableHeadersRow();
    for (const nlohmann::json& row : rows) {
      const std::string st = row.value("state", std::string());
      const std::string unit = row.value("unit", std::string());
      const std::string id = std::to_string(row.value("id", uint64_t{0}));
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextColored(StateColour(st), "%s", st.c_str());
      ImGui::TableNextColumn();
      ImGui::Selectable(
          (row.value("name", std::string()) + "###req_row" + id).c_str(), false,
          ImGuiSelectableFlags_SpanAllColumns);
      if (ImGui::IsItemHovered()) {
        std::string tip = row.at("measure").value("kind", std::string());
        if (row.contains("source"))
          tip += "\n" + row.at("source").get<std::string>();
        if (row.contains("note"))
          tip += "\n" + row.at("note").get<std::string>();
        // A failed verdict's findings: "findings" in the Value column says
        // there are some, and this is where they are read.
        if (row.contains("detail") && row.at("detail").contains("findings"))
          for (const nlohmann::json& f : row.at("detail").at("findings"))
            if (f.is_string())
              tip += "\n- " + f.get<std::string>();
        if (row.contains("checked_at"))
          tip += "\nchecked " + row.at("checked_at").get<std::string>() +
                 " (revision " +
                 std::to_string(row.value("evidence_revision", 0)) + ")";
        ImGui::SetTooltip("%s", tip.c_str());
      }
      ImGui::TableNextColumn();
      if (row.contains("value"))
        ImGui::Text("%s%s", Number(row.at("value").get<double>()).c_str(),
                    unit.empty() ? "" : (" " + unit).c_str());
      else if (st == "pass" || st == "fail")
        ImGui::TextUnformatted(st == "pass" ? "clean" : "findings");
      else
        ImGui::TextDisabled("-");
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(BoundText(row.at("bound"), unit, row).c_str());
    }
    ImGui::EndTable();
  }
  ImGui::End();
}

} // namespace ui
