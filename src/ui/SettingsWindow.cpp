#include "ui/SettingsWindow.h"

#include "common/Logger.h"
#include "core/io/Settings.h"

#include "imgui.h"

#include <cmath>
#include <string>
#include <vector>

namespace ui {

namespace {

// A slider drag is one write, not sixty.
constexpr double kSaveDebounceSeconds = 1.0;

const char* TierNote(settings::Tier tier) {
  switch (tier) {
  case settings::Tier::Live:
    return nullptr; // no note: it already happened
  case settings::Tier::NextTime:
    return "takes effect next time";
  case settings::Tier::Restart:
    return "needs a restart";
  }
  return nullptr;
}

// The muted explanation under a control, plus the tier when it is not Live.
// A settings window that does not explain its settings moves the confusion
// rather than removing it.
void DrawBlurb(const settings::Field& f, const char* extra = nullptr) {
  ImGui::PushStyleColor(ImGuiCol_Text,
                        ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::PushTextWrapPos(0.0f);
  std::string text = f.blurb;
  if (const char* note = TierNote(f.tier))
    text += std::string(" (") + note + ")";
  if (extra)
    text += std::string(" ") + extra;
  ImGui::TextUnformatted(text.c_str());
  ImGui::PopTextWrapPos();
  ImGui::PopStyleColor();
}

// Width reserved for a control: MEASURED, never a pixel literal. The style
// scales with the display's content scale and with the font-size row this
// very window owns, so a hard-coded number is wrong on someone's screen.
float ControlWidth() {
  const ImGuiStyle& style = ImGui::GetStyle();
  return ImGui::CalcTextSize("NVIDIA GeForce RTX 4060 Laptop GPU").x +
         style.FramePadding.x * 2.0f + ImGui::GetFrameHeight();
}

bool DrawField(AppState& state, const settings::Field& f) {
  settings::Settings& s = state.settings;
  const std::string id = std::string("###set_") + f.key;
  bool changed = false;
  ImGui::TextUnformatted(f.label);

  const float width =
      std::min(ControlWidth(), ImGui::GetContentRegionAvail().x);
  ImGui::SetNextItemWidth(width);

  if (auto* m = std::get_if<bool settings::Settings::*>(&f.member)) {
    bool value = s.*(*m);
    if (ImGui::Checkbox(id.c_str(), &value)) {
      s.*(*m) = value;
      changed = true;
    }
  } else if (auto* m =
                 std::get_if<std::string settings::Settings::*>(&f.member)) {
    if (!f.values.empty()) {
      const std::string current = s.*(*m);
      if (ImGui::BeginCombo(id.c_str(), current.c_str())) {
        for (const char* option : f.values) {
          if (ImGui::Selectable(option, current == option)) {
            s.*(*m) = option;
            changed = true;
          }
        }
        ImGui::EndCombo();
      }
    } else {
      // Free-text with a machine-supplied list: the GPU row. "Automatic" is
      // the empty string, spelled out so it does not read as a broken entry.
      const std::string current = s.*(*m);
      const char* label = current.empty() ? "Automatic" : current.c_str();
      if (ImGui::BeginCombo(id.c_str(), label)) {
        if (ImGui::Selectable("Automatic", current.empty())) {
          (s.*(*m)).clear();
          changed = true;
        }
        for (const std::string& device : state.gpuDevices) {
          if (ImGui::Selectable(device.c_str(), current == device)) {
            s.*(*m) = device;
            changed = true;
          }
        }
        ImGui::EndCombo();
      }
    }
  } else if (!f.choices.empty()) {
    // A fixed set of numbers is a combo, not a slider: "1024 or 2048" is a
    // choice, and a slider would imply 1536 means something.
    const double current = [&] {
      if (auto* fm = std::get_if<float settings::Settings::*>(&f.member))
        return static_cast<double>(s.*(*fm));
      if (auto* im = std::get_if<int settings::Settings::*>(&f.member))
        return static_cast<double>(s.*(*im));
      if (auto* dm = std::get_if<double settings::Settings::*>(&f.member))
        return s.*(*dm);
      return 0.0;
    }();
    const auto text = [&f](double v) {
      v *= f.displayScale;
      std::string out = std::to_string(v);
      // Trim the trailing zeros std::to_string insists on.
      if (out.find('.') != std::string::npos) {
        while (!out.empty() && out.back() == '0')
          out.pop_back();
        if (!out.empty() && out.back() == '.')
          out.pop_back();
      }
      return out + f.unit;
    };
    if (ImGui::BeginCombo(id.c_str(), text(current).c_str())) {
      for (float option : f.choices) {
        const bool selected = std::abs(current - option) < 1e-6;
        if (ImGui::Selectable(text(option).c_str(), selected)) {
          if (auto* fm = std::get_if<float settings::Settings::*>(&f.member))
            s.*(*fm) = option;
          else if (auto* im = std::get_if<int settings::Settings::*>(&f.member))
            s.*(*im) = static_cast<int>(option);
          else if (auto* dm =
                       std::get_if<double settings::Settings::*>(&f.member))
            s.*(*dm) = option;
          changed = true;
        }
      }
      ImGui::EndCombo();
    }
  } else if (auto* dm = std::get_if<double settings::Settings::*>(&f.member)) {
    int value = static_cast<int>(s.*(*dm));
    if (ImGui::DragInt(id.c_str(), &value, 1.0f, static_cast<int>(f.min),
                       static_cast<int>(f.max), "%d s")) {
      s.*(*dm) = value;
      changed = true;
    }
  }
  return changed;
}

} // namespace

void ApplySettings(AppState& state) {
  const settings::Settings& s = state.settings;
  state.uiFontScale = s.uiFontScale;
  state.showShadows = s.shadows;
  state.showAmbientOcclusion = s.ssao != "off";
  state.ssaoFullRes = s.ssao == "full";
  state.shadowMapSize = s.shadowMap;
}

void MarkSettingsChanged(AppState& state) {
  ApplySettings(state);
  state.settingsDirtyAt = ImGui::GetCurrentContext() ? ImGui::GetTime() : 0.0;
}

void FlushSettings(AppState& state, double now) {
  if (state.settingsDirtyAt < 0.0 ||
      now - state.settingsDirtyAt < kSaveDebounceSeconds)
    return;
  state.settingsDirtyAt = -1.0;
  std::string error;
  // Against settingsSaved, not against defaults: only the keys THIS instance
  // changed are written, so a second instance's preferences survive.
  if (!settings::Save(state.settings, state.settingsSaved, &error)) {
    LOG_WARN("Could not save settings: {}", error);
    return;
  }
  state.settingsSaved = state.settings;
}

void DrawSettingsWindow(AppState& state) {
  if (!state.settingsWindowOpen)
    return;
  ImGui::SetNextWindowSize(
      ImVec2(ImGui::GetFontSize() * 34.0f, ImGui::GetFontSize() * 30.0f),
      ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Preferences###SettingsWindow",
                    &state.settingsWindowOpen)) {
    ImGui::End();
    return;
  }

  bool changed = false;
  const char* section = nullptr;
  bool sectionOpen = false;
  for (const settings::Field& f : settings::Fields()) {
    if (section == nullptr || std::string(section) != f.section) {
      section = f.section;
      const std::string headerId = std::string(section) + "###sec_" + section;
      sectionOpen = ImGui::CollapsingHeader(headerId.c_str(),
                                            ImGuiTreeNodeFlags_DefaultOpen);
    }
    if (!sectionOpen)
      continue;
    changed |= DrawField(state, f);
    // The GPU row states what is actually in use, so a pending restart-tier
    // change is visible rather than a lie.
    if (std::string(f.key) == "gpu_device" && !state.gpuName.empty()) {
      const std::string inUse =
          "Currently rendering on: " + state.gpuName + ".";
      DrawBlurb(f, inUse.c_str());
    } else {
      DrawBlurb(f);
    }
    ImGui::Spacing();
  }

  ImGui::Separator();
  // A user who has broken their config should not have to find the JSON.
  if (ImGui::Button("Reset all to defaults###set_reset_all")) {
    const auto unknown = state.settings.unknown; // not ours to discard
    state.settings = settings::Settings{};
    state.settings.unknown = unknown;
    changed = true;
  }
  ImGui::SameLine();
  ImGui::TextDisabled("%s", settings::ConfigPath().c_str());

  if (changed)
    MarkSettingsChanged(state);
  ImGui::End();
}

} // namespace ui
