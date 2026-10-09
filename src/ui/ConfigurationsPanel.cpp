// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/ConfigurationsPanel.h"
#include "ui/AssemblyActions.h" // SnapshotAssembly / SolveActiveAssembly
#include "ui/DockLayout.h"      // PanelHeading
#include "ui/ExprInput.h"       // expression-capable numeric fields
#include "ui/OccurrencePanel.h" // OccurrenceLabel
#include "ui/PanelTitles.h"

#include "core/Angles.h"
#include "core/AssemblySolve.h" // MateHasDof
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

// Assembly configurations (C4, v101): named variants that suppress
// occurrences, mates and relations and hold mate DOFs. The active selector
// switches the shown variant — and re-solves, which is what applies it; the
// active config's editor is a suppression checklist per kind plus a "hold"
// per mate DOF. Structurally the same object a part config is, which is why
// it shares this panel rather than living in the assembly's Model dock
// (docs/ui/panels.md#assembly-tab-the-model-panel).
void DrawAssemblyConfigs(AppState& state) {
  AssemblyDocument& asmDoc = *state.assembly;
  ImGui::SeparatorText("Configurations");
  const uint64_t active = asmDoc.ActiveConfiguration();

  if (ImGui::Selectable("Base (all shown)", active == 0)) {
    SnapshotAssembly(state);
    asmDoc.SetActiveConfiguration(0);
    SolveActiveAssembly(state);
  }
  for (const Configuration& c : asmDoc.GetConfigurations()) {
    ImGui::PushID(1000 + static_cast<int>(c.id));
    if (ImGui::Selectable(c.name.c_str(), active == c.id)) {
      SnapshotAssembly(state);
      asmDoc.SetActiveConfiguration(c.id);
      SolveActiveAssembly(state);
    }
    ImGui::PopID();
  }
  if (ImGui::Button("+ Add configuration")) {
    SnapshotAssembly(state);
    const uint64_t id = asmDoc.AddConfiguration().id;
    asmDoc.SetActiveConfiguration(id);
    SolveActiveAssembly(state);
  }

  Configuration* cfg = asmDoc.FindConfiguration(active);
  if (!cfg)
    return; // base

  char nm[128];
  std::snprintf(nm, sizeof(nm), "%s", cfg->name.c_str());
  // The part arm's row, now that the two sit in one panel and the difference
  // is visible: a LEFT label (an ImGui label draws to the right and is the
  // first thing a dock edge clips — "Config nan"), and an addressable id.
  LeftLabel("Name");
  if (ImGui::InputText("###cfg_asm_name", nm, sizeof(nm),
                       ImGuiInputTextFlags_EnterReturnsTrue)) {
    SnapshotAssembly(state);
    asmDoc.FindConfiguration(active)->name = nm;
    asmDoc.BumpVersion();
  }
  if (ImGui::SmallButton("Delete configuration###cfg_asm_delete")) {
    SnapshotAssembly(state);
    asmDoc.RemoveConfiguration(active);
    SolveActiveAssembly(state);
    return;
  }
  ImGui::TextUnformatted("Suppressed occurrences");
  for (const Occurrence& o : asmDoc.GetOccurrences()) {
    bool sup = cfg->Suppresses(o.id);
    ImGui::PushID(2000 + static_cast<int>(o.id));
    if (ImGui::Checkbox(OccurrenceLabel(state, asmDoc, o.id).c_str(), &sup)) {
      SnapshotAssembly(state);
      auto& v = asmDoc.FindConfiguration(active)->suppressedFeatures;
      v.erase(std::remove(v.begin(), v.end(), o.id), v.end());
      if (sup)
        v.push_back(o.id);
      asmDoc.BumpVersion();
      SolveActiveAssembly(state);
    }
    ImGui::PopID();
  }

  // Mates: out of the solve, or a DOF held at the configuration's own value
  // (radians shown as degrees, offsets in mm). A hold starts at the mate's
  // current value, so ticking it moves nothing until the value is edited.
  bool changed = false;
  if (!asmDoc.GetMates().empty())
    ImGui::TextUnformatted("Mates");
  for (const Mate& mt : asmDoc.GetMates()) {
    const std::string sid = std::to_string(mt.id);
    ImGui::PushID(("cfg_mate_" + sid).c_str());
    Configuration& c = *asmDoc.FindConfiguration(active);
    bool sup = c.SuppressesMate(mt.id);
    const std::string label =
        (mt.name.empty() ? std::string("Mate") : mt.name) + " " + sid +
        "###cfg_mate_sup_" + sid;
    if (ImGui::Checkbox(label.c_str(), &sup)) {
      SnapshotAssembly(state);
      auto& v = asmDoc.FindConfiguration(active)->suppressedMates;
      v.erase(std::remove(v.begin(), v.end(), mt.id), v.end());
      if (sup)
        v.push_back(mt.id);
      changed = true;
    }
    const auto holdRow = [&](bool rotation) {
      Configuration& cc = *asmDoc.FindConfiguration(active);
      auto it =
          std::find_if(cc.mateOverrides.begin(), cc.mateOverrides.end(),
                       [&](const MateOverride& o) { return o.mate == mt.id; });
      std::optional<double>* slot =
          it == cc.mateOverrides.end()
              ? nullptr
              : (rotation ? &it->rotation : &it->offset);
      bool held = slot != nullptr && slot->has_value();
      ImGui::SameLine();
      const std::string hid = std::string(rotation ? "turn" : "slide") +
                              "###cfg_hold_" + (rotation ? "r_" : "o_") + sid;
      if (ImGui::Checkbox(hid.c_str(), &held)) {
        SnapshotAssembly(state);
        if (held) {
          if (it == cc.mateOverrides.end()) {
            cc.mateOverrides.push_back({mt.id, std::nullopt, std::nullopt});
            it = cc.mateOverrides.end() - 1;
          }
          (rotation ? it->rotation : it->offset) =
              rotation ? mt.rotation : mt.offset;
        } else if (it != cc.mateOverrides.end()) {
          (rotation ? it->rotation : it->offset).reset();
          if (!it->rotation && !it->offset)
            cc.mateOverrides.erase(it);
        }
        changed = true;
        return;
      }
      if (held) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.0f);
        // An ExprInput field, not a raw DragFloat: a typed "90/4" must
        // evaluate, not sscanf to 90 (test_conventions.cpp).
        double v = rotation ? angles::ToDegrees(**slot) : **slot;
        const std::string vid =
            std::string("###cfg_hold_v") + (rotation ? "r_" : "o_") + sid;
        if (ExprDragDouble(state, vid.c_str(), &v, rotation ? 1.0f : 0.05f, 0.0,
                           0.0, rotation ? "%.2f deg" : "%.3f mm")) {
          SnapshotAssembly(state);
          *slot = rotation ? angles::ToRadians(v) : v;
          changed = true;
        }
      }
    };
    if (assembly::MateHasDof(mt.type, MateDof::Rotation))
      holdRow(true);
    if (mt.type != MateType::Fastened && mt.type != MateType::Ball)
      holdRow(false);
    ImGui::PopID();
  }
  if (!asmDoc.GetRelations().empty())
    ImGui::TextUnformatted("Relations");
  for (const Relation& rel : asmDoc.GetRelations()) {
    const std::string sid = std::to_string(rel.id);
    bool sup = asmDoc.FindConfiguration(active)->SuppressesRelation(rel.id);
    const std::string label =
        (rel.name.empty() ? std::string("Relation") : rel.name) + " " + sid +
        "###cfg_rel_sup_" + sid;
    if (ImGui::Checkbox(label.c_str(), &sup)) {
      SnapshotAssembly(state);
      auto& v = asmDoc.FindConfiguration(active)->suppressedRelations;
      v.erase(std::remove(v.begin(), v.end(), rel.id), v.end());
      if (sup)
        v.push_back(rel.id);
      changed = true;
    }
  }
  if (changed) {
    asmDoc.BumpVersion();
    SolveActiveAssembly(state);
  }
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
  PanelHeading(kPanelConfigurations);
  // An assembly tab has no `document`, and its configurations are a different
  // object on a different class — but they are the same IDEA, browsed and
  // switched the same way, so they get the same tab instead of a seventh
  // section in the occurrence panel.
  if (state.assembly) {
    DrawAssemblyConfigs(state);
    ImGui::End();
    return;
  }
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
      // that makes a configuration mean anything
      // (docs/ui/testing.md#making-a-widget-addressable).
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
