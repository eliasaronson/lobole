// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/TemplatePanel.h"

#include "core/Template.h"
#include "ui/FloatingWindow.h"
#include "ui/PanelLayout.h" // LeftLabel
#include "ui/TemplateActions.h"

#include "IconsFontAwesome6.h"
#include "imgui.h"

#include <cfloat>
#include <cstdio>
#include <stdexcept>

namespace ui {

void OpenSaveTemplate(AppState& state, const std::vector<uint64_t>& ids) {
  AppState::SaveTemplateSession& s = state.saveTemplate;
  s = AppState::SaveTemplateSession{};
  if (!state.document || ids.empty())
    return;
  s.open = true;
  s.featureIds = ids;
  // A name nothing in the library has yet, from the first feature's.
  const Feature* first = nullptr;
  size_t firstIndex = SIZE_MAX;
  for (uint64_t id : ids) {
    size_t index = 0;
    if (const Feature* f = state.document->FindFeatureById(id, &index);
        f != nullptr && index < firstIndex) {
      first = f;
      firstIndex = index;
    }
  }
  s.name = first != nullptr ? first->GetName() : std::string("template");
  const tmpl::Suggestions sug = tmpl::Suggest(*state.document, ids);
  s.variables = sug.variables;
  s.references = sug.references;
  for (const tmpl::Candidate& c : sug.literals)
    s.rows.push_back({c, false, c.suggested});
}

void DrawSaveTemplateWindow(AppState& state) {
  AppState::SaveTemplateSession& s = state.saveTemplate;
  if (!s.open)
    return;
  if (!state.document) {
    s.open = false;
    return;
  }
  static FloatingPlacement placement;
  const float wrap = ImGui::GetFontSize() * 26.0f;
  ImGui::SetNextWindowSizeConstraints(ImVec2(wrap * 0.6f, 0.0f),
                                      ImVec2(wrap + 24.0f, FLT_MAX));
  bool open = true;
  if (!BeginFloatingWindow(state, "Save as template###SaveTemplate", &open,
                           placement, FloatAnchor::BelowToolWindow,
                           ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::End();
    s.open = open;
    return;
  }
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
  ImGui::TextDisabled("%zu feature%s", s.featureIds.size(),
                      s.featureIds.size() == 1 ? "" : "s");
  static char nameBuf[128];
  std::snprintf(nameBuf, sizeof(nameBuf), "%s", s.name.c_str());
  LeftLabel("Name");
  if (ImGui::InputText("###tpl_name", nameBuf, sizeof(nameBuf)))
    s.name = nameBuf;

  if (!s.references.empty()) {
    ImGui::SeparatorText("Stands on");
    for (const std::string& r : s.references)
      ImGui::BulletText("%s", r.c_str());
  }
  if (!s.variables.empty()) {
    ImGui::SeparatorText("Variables read (parameters)");
    std::string list;
    for (const std::string& v : s.variables)
      list += (list.empty() ? "" : ", ") + v;
    ImGui::TextUnformatted(list.c_str());
  }
  if (!s.rows.empty()) {
    ImGui::SeparatorText("Literals to make parameters");
    for (size_t i = 0; i < s.rows.size(); i++) {
      AppState::SaveTemplateSession::Row& r = s.rows[i];
      ImGui::PushID(static_cast<int>(i));
      char label[256];
      std::snprintf(label, sizeof(label), "%s %s = %.4g###tpl_lit",
                    r.candidate.feature.c_str(), r.candidate.label.c_str(),
                    r.candidate.value);
      ImGui::Checkbox(label, &r.use);
      if (r.use) {
        static char pbuf[64];
        std::snprintf(pbuf, sizeof(pbuf), "%s", r.param.c_str());
        ImGui::Indent();
        LeftLabel("as");
        if (ImGui::InputText("###tpl_param", pbuf, sizeof(pbuf)))
          r.param = pbuf;
        ImGui::Unindent();
      }
      ImGui::PopID();
    }
  }
  ImGui::Checkbox("Replace a template of this name (its next revision)"
                  "###tpl_replace",
                  &s.replace);
  if (!s.error.empty())
    ImGui::TextColored(ImVec4(0.92f, 0.35f, 0.35f, 1.0f), "%s",
                       s.error.c_str());
  if (!s.message.empty())
    ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f), "%s",
                       s.message.c_str());
  if (ImGui::Button(ICON_FA_FLOPPY_DISK " Save###tpl_save")) {
    tmpl::SaveSpec spec;
    spec.name = s.name;
    spec.featureIds = s.featureIds;
    for (const auto& r : s.rows)
      if (r.use)
        spec.literals.push_back(
            {r.candidate.featureId, r.candidate.path, r.param});
    try {
      std::vector<TemplateInstanceRef> marked;
      const tmpl::Template& t = SaveTemplate(state, spec, s.replace, &marked);
      s.error.clear();
      s.message = "Saved '" + t.name + "' (revision " +
                  std::to_string(t.revision) + ", " +
                  std::to_string(t.params.size()) + " parameters)" +
                  (marked.empty() ? std::string{}
                                  : "; " + std::to_string(marked.size()) +
                                        " instance(s) now read template "
                                        "changed");
    } catch (const std::exception& e) {
      s.error = e.what();
      s.message.clear();
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Close###tpl_close"))
    open = false;
  ImGui::PopTextWrapPos();
  ImGui::End();
  s.open = open;
}

} // namespace ui
