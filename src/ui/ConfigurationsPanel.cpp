#include "ui/ConfigurationsPanel.h"
#include "ui/PanelTitles.h"

#include "core/Command.h"
#include "core/Configuration.h"
#include "core/Document.h"
#include "core/io/Serialization.h"

#include "imgui.h"
#include "ui/PanelLayout.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <string>

namespace ui {

namespace {

// Runs one edit of the active configuration as a ModifyConfigurationCommand:
// snapshot before, apply `mutate` to a copy, snapshot after, execute. The
// study/sketch modify idiom.
template <typename Mutate>
void ModifyActiveConfig(AppState& state, const char* desc, Mutate mutate) {
  Document& doc = *state.document;
  const Configuration* cur = doc.FindConfiguration(doc.ActiveConfiguration());
  if (!cur)
    return;
  nlohmann::json before = DocumentIO::ConfigurationToJson(*cur);
  Configuration copy = *cur;
  mutate(copy);
  nlohmann::json after = DocumentIO::ConfigurationToJson(copy);
  state.commands->Execute(std::make_unique<ModifyConfigurationCommand>(
                              desc, std::move(before), std::move(after)),
                          doc);
}

// The override expression a config holds for `name`, or "" if none.
std::string OverrideOf(const Configuration& c, const std::string& name) {
  const std::string* e = c.OverrideFor(name);
  return e ? *e : std::string{};
}

} // namespace

void DrawConfigurationsPanel(AppState& state) {
  // Never steal the shared node from Model on first appearance.
  ImGui::Begin(kPanelConfigurations, nullptr,
               ImGuiWindowFlags_NoFocusOnAppearing);
  if (!state.document) {
    ImGui::End();
    return;
  }
  Document& doc = *state.document;
  const uint64_t active = doc.ActiveConfiguration();

  // Active-configuration selector: Base + each config. Selecting activates
  // (regenerates), so the viewport shows the chosen variant.
  ImGui::SeparatorText("Configurations");
  if (ImGui::Selectable("Base (as authored)", active == 0))
    state.commands->Execute(std::make_unique<SetActiveConfigurationCommand>(0),
                            doc);
  for (const Configuration& c : doc.GetConfigurations()) {
    // Named, not wrapped in a PushID: a scope makes the row reachable only
    // by numeric id, and picking a configuration is the whole panel.
    const std::string label = (c.name.empty() ? "(unnamed)" : c.name) +
                              "###cfg_row_" + std::to_string(c.id);
    if (ImGui::Selectable(label.c_str(), active == c.id))
      state.commands->Execute(
          std::make_unique<SetActiveConfigurationCommand>(c.id), doc);
  }
  if (ImGui::Button("+ Add configuration")) {
    auto cmd = std::make_unique<AddConfigurationCommand>();
    AddConfigurationCommand* raw = cmd.get();
    state.commands->Execute(std::move(cmd), doc);
    // Activate the new one so its editor shows below.
    state.commands->Execute(
        std::make_unique<SetActiveConfigurationCommand>(raw->ConfigurationId()),
        doc);
  }

  // Editor for the active configuration.
  const Configuration* cfg = doc.FindConfiguration(active);
  if (!cfg) {
    ImGui::Spacing();
    ImGui::TextDisabled("The base model, as authored. Add or select a");
    ImGui::TextDisabled("configuration to override variables / suppress");
    ImGui::TextDisabled("features.");
    ImGui::End();
    return;
  }

  ImGui::Spacing();
  ImGui::SeparatorText("Edit configuration");

  // Rename (commit on Enter / focus loss).
  {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s", cfg->name.c_str());
    LeftLabel("Name");
    if (ImGui::InputText("###cfg_name", buf, sizeof(buf),
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
      std::string newName = buf;
      ModifyActiveConfig(state, "Rename configuration",
                         [&](Configuration& c) { c.name = newName; });
    }
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("Delete")) {
    state.commands->Execute(
        std::make_unique<RemoveConfigurationCommand>(active), doc);
    ImGui::End();
    return; // cfg is dangling now
  }

  // Parameter overrides: one row per variable, an editable override cell
  // (empty = inherit the base). Committing sets/clears the override.
  ImGui::Spacing();
  // Wrapped, not clipped: this sentence is the only thing that says what a
  // blank cell means.
  ImGui::TextWrapped("Overrides (blank = base value)");
  if (ImGui::BeginTable("cfg_overrides", 3,
                        ImGuiTableFlags_SizingStretchProp |
                            ImGuiTableFlags_BordersInnerH)) {
    ImGui::TableSetupColumn("Variable");
    ImGui::TableSetupColumn("Base");
    ImGui::TableSetupColumn("Override");
    ImGui::TableHeadersRow();
    // Part-local variables AND project ones. Document::Regenerate has always
    // handled an override that targets a project variable — it injects it as
    // a synthetic local so it shadows the project value in this part alone
    // (the block written for the C-clamp configuration bug). Only this table
    // did not list them, and since the Variables panel creates every variable
    // at PROJECT scope, that made the whole parametric-family flow dead-end
    // at "No variables to override" right after you added one.
    std::vector<Parameter> overridable = doc.GetParameters();
    for (const Parameter& pp : state.projectParameters) {
      const bool shadowed =
          std::any_of(overridable.begin(), overridable.end(),
                      [&](const Parameter& a) { return a.name == pp.name; });
      if (!shadowed) // a part variable of the same name wins, as at regen
        overridable.push_back(pp);
    }
    for (const Parameter& p : overridable) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(p.name.c_str());
      ImGui::TableNextColumn();
      ImGui::TextDisabled("%s", p.expression.c_str());
      ImGui::TableNextColumn();
      // A stable name, not a PushID scope around a "##" label: either one
      // leaves the cell reachable only by numeric id, and this is the cell
      // that makes a configuration mean anything (docs/ui/gui-coverage.md).
      const std::string ovId = "###cfg_override_" + p.name;
      char buf[128];
      std::snprintf(buf, sizeof(buf), "%s", OverrideOf(*cfg, p.name).c_str());
      ImGui::SetNextItemWidth(-FLT_MIN);
      if (ImGui::InputText(ovId.c_str(), buf, sizeof(buf),
                           ImGuiInputTextFlags_EnterReturnsTrue)) {
        std::string expr = buf;
        const std::string name = p.name;
        ModifyActiveConfig(state, "Set override", [&](Configuration& c) {
          // Remove any existing override for this variable.
          for (auto it = c.parameterOverrides.begin();
               it != c.parameterOverrides.end();) {
            if (it->first == name)
              it = c.parameterOverrides.erase(it);
            else
              ++it;
          }
          if (!expr.empty())
            c.parameterOverrides.emplace_back(name, expr);
        });
      }
    }
    ImGui::EndTable();
  }
  if (doc.GetParameters().empty() && state.projectParameters.empty())
    ImGui::TextWrapped("No variables to override — add some in Variables.");

  // Feature suppression: a checkbox per feature.
  ImGui::Spacing();
  ImGui::TextUnformatted("Suppressed features");
  for (size_t i = 0; i < doc.GetFeatures().size(); ++i) {
    const Feature* f = doc.GetFeatures()[i].get();
    const uint64_t fid = f->GetId();
    bool suppressed = cfg->Suppresses(fid);
    const std::string supId =
        f->GetName() + "###cfg_suppress_" + std::to_string(fid);
    if (ImGui::Checkbox(supId.c_str(), &suppressed)) {
      ModifyActiveConfig(state, "Toggle suppression", [&](Configuration& c) {
        auto& v = c.suppressedFeatures;
        v.erase(std::remove(v.begin(), v.end(), fid), v.end());
        if (suppressed)
          v.push_back(fid);
      });
    }
  }

  ImGui::End();
}

} // namespace ui
