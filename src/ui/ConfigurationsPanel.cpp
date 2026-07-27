#include "ui/ConfigurationsPanel.h"

#include "core/Command.h"
#include "core/Configuration.h"
#include "core/Document.h"
#include "core/io/Serialization.h"

#include "imgui.h"

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
  ImGui::Begin("Configurations");
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
    ImGui::PushID(static_cast<int>(c.id));
    const char* label = c.name.empty() ? "(unnamed)" : c.name.c_str();
    if (ImGui::Selectable(label, active == c.id))
      state.commands->Execute(
          std::make_unique<SetActiveConfigurationCommand>(c.id), doc);
    ImGui::PopID();
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
    if (ImGui::InputText("Name", buf, sizeof(buf),
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
  ImGui::TextUnformatted("Overrides (blank = base value)");
  if (ImGui::BeginTable("cfg_overrides", 3,
                        ImGuiTableFlags_SizingStretchProp |
                            ImGuiTableFlags_BordersInnerH)) {
    ImGui::TableSetupColumn("Variable");
    ImGui::TableSetupColumn("Base");
    ImGui::TableSetupColumn("Override");
    ImGui::TableHeadersRow();
    for (const Parameter& p : doc.GetParameters()) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(p.name.c_str());
      ImGui::TableNextColumn();
      ImGui::TextDisabled("%s", p.expression.c_str());
      ImGui::TableNextColumn();
      ImGui::PushID(p.name.c_str());
      char buf[128];
      std::snprintf(buf, sizeof(buf), "%s", OverrideOf(*cfg, p.name).c_str());
      ImGui::SetNextItemWidth(-FLT_MIN);
      if (ImGui::InputText("##ov", buf, sizeof(buf),
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
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  if (doc.GetParameters().empty())
    ImGui::TextDisabled("No variables to override — add some in Variables.");

  // Feature suppression: a checkbox per feature.
  ImGui::Spacing();
  ImGui::TextUnformatted("Suppressed features");
  for (size_t i = 0; i < doc.GetFeatures().size(); ++i) {
    const Feature* f = doc.GetFeatures()[i].get();
    const uint64_t fid = f->GetId();
    bool suppressed = cfg->Suppresses(fid);
    ImGui::PushID(static_cast<int>(fid));
    if (ImGui::Checkbox(f->GetName().c_str(), &suppressed)) {
      ModifyActiveConfig(state, "Toggle suppression", [&](Configuration& c) {
        auto& v = c.suppressedFeatures;
        v.erase(std::remove(v.begin(), v.end(), fid), v.end());
        if (suppressed)
          v.push_back(fid);
      });
    }
    ImGui::PopID();
  }

  ImGui::End();
}

} // namespace ui
