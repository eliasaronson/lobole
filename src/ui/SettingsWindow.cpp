#include "ui/SettingsWindow.h"

#include "core/io/GitVault.h"

#include "common/Logger.h"
#include "core/io/Keymap.h"
#include "core/io/Settings.h"
#include "ui/Commands.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
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
    if (f.freeText) {
      // A name or an email: no list to choose from. Committed on the way out
      // of the field rather than per keystroke, so half-typed values never
      // reach the settings file.
      char buffer[256] = {};
      const std::string current = s.*(*m);
      std::snprintf(buffer, sizeof(buffer), "%s", current.c_str());
      if (ImGui::InputText(id.c_str(), buffer, sizeof(buffer),
                           ImGuiInputTextFlags_EnterReturnsTrue) ||
          ImGui::IsItemDeactivatedAfterEdit()) {
        s.*(*m) = buffer;
        changed = true;
      }
    } else if (!f.values.empty()) {
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
    // "%g", not to_string + trimming: 0.85f * 100 is 85.000002 in float, and
    // a combo offering "85.000002%" reads as a bug.
    const auto text = [&f](double v) {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%g%s", v * f.displayScale, f.unit);
      return std::string(buf);
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

// Preferences > Keyboard. Not a `Field`: the table above is one scalar per
// row rendered as a slider or a combo, and a chord is neither
// (docs/ui/u12-keybindings.md). One collapsing node per context, because a
// context binding legitimately reuses a key another context owns — `N` is
// Coincident in a sketch and align-to-face everywhere else — so grouping is
// what makes the list readable instead of looking full of duplicates.
bool DrawKeyboardSection(AppState& state) {
  keymap::Keymap& keys = state.settings.keys;
  bool changed = false;

  static char filter[64] = "";
  ImGui::SetNextItemWidth(
      std::min(ControlWidth(), ImGui::GetContentRegionAvail().x));
  ImGui::InputTextWithHint("###key_filter", "Filter by name or key", filter,
                           sizeof(filter));
  ImGui::SameLine();
  if (ImGui::Button("Reset all keys###key_reset_all")) {
    keys.ResetAll();
    changed = true;
  }

  ImGui::PushStyleColor(ImGuiCol_Text,
                        ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::PushTextWrapPos(0.0f);
  ImGui::TextUnformatted(
      "Click a key to rebind it; Backspace clears it, Escape cancels. A "
      "mode's binding wins over the same key listed under Everywhere. "
      "Escape, Enter, Delete and the arrow keys are fixed — they answer to "
      "whatever tool, preview or field is open, so they are not commands.");
  ImGui::PopTextWrapPos();
  ImGui::PopStyleColor();
  ImGui::Spacing();

  // Case-insensitive substring over the label AND the chord, so "shift+" and
  // "fillet" both find the fillet row.
  const auto matches = [](const std::string& haystack, const char* needle) {
    if (*needle == '\0')
      return true;
    const auto lower = [](std::string v) {
      std::transform(v.begin(), v.end(), v.begin(),
                     [](unsigned char c) { return std::tolower(c); });
      return v;
    };
    return lower(haystack).find(lower(needle)) != std::string::npos;
  };

  for (keymap::Context ctx : keymap::AllContexts()) {
    // Which rows survive the filter — counted first, so a context with no
    // match disappears instead of showing an empty node.
    std::vector<const keymap::Binding*> rows;
    for (const keymap::Binding& b : keymap::Defaults()) {
      if (b.ctx != ctx || b.hidden)
        continue;
      if (matches(std::string(b.section) + " " + b.label + " " +
                      keys.Text(b.id),
                  filter))
        rows.push_back(&b);
    }
    if (rows.empty())
      continue;

    const std::string nodeId = std::string(keymap::ContextLabel(ctx)) +
                               "###keyctx_" + keymap::ContextName(ctx);
    if (!ImGui::TreeNodeEx(nodeId.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
      continue;

    if (ImGui::SmallButton(
            (std::string("Reset these###keyreset_") + keymap::ContextName(ctx))
                .c_str())) {
      keys.ResetContext(ctx);
      changed = true;
    }

    if (ImGui::BeginTable(
            (std::string("keytbl_") + keymap::ContextName(ctx)).c_str(), 3,
            ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
      ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed,
                              ImGui::GetFontSize() * 8.0f);
      ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed,
                              ImGui::GetFontSize() * 3.5f);
      for (const keymap::Binding* b : rows) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(b->label);

        ImGui::TableNextColumn();
        const bool capturing = state.keyCaptureId == b->id;
        const std::string chord = keys.Text(b->id);
        const std::string buttonLabel =
            (capturing
                 ? std::string("Press a key\xe2\x80\xa6")
                 : (chord.empty() ? std::string("\xe2\x80\x94") : chord)) +
            "###keybtn_" + b->id;
        if (capturing)
          ImGui::PushStyleColor(
              ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(buttonLabel.c_str(), ImVec2(-FLT_MIN, 0.0f)))
          state.keyCaptureId = capturing ? "" : b->id;
        if (capturing)
          ImGui::PopStyleColor();

        if (capturing) {
          // Escape gets out without binding — it is the one key a person
          // reaches for when they change their mind, so it can never BE the
          // binding. Backspace clears instead, which is the other thing they
          // came here to do.
          if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            state.keyCaptureId.clear();
          } else if (ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) {
            keys.Set(b->id, "");
            state.keyCaptureId.clear();
            changed = true;
          } else if (const keymap::Chord pressed = CaptureChord()) {
            keys.Set(b->id, keymap::FormatChord(pressed));
            state.keyCaptureId.clear();
            changed = true;
          }
        }

        ImGui::TableNextColumn();
        ImGui::BeginDisabled(keys.IsDefault(b->id));
        if (ImGui::SmallButton(
                (std::string("Reset###keyrst_") + b->id).c_str())) {
          keys.Reset(b->id);
          changed = true;
        }
        ImGui::EndDisabled();

        // The collision, named, on the row that causes it. A conflict is
        // ALLOWED — refusing one would strand anyone mid-swap, who has to
        // put a key on B before taking it off A — but it is never silent.
        const std::vector<std::string> clash =
            keys.Bound(ctx, keys.Get(b->id), b->id);
        if (!clash.empty()) {
          std::string others;
          for (const std::string& id : clash) {
            const keymap::Binding* other = keymap::Find(id);
            others += (others.empty() ? "" : ", ");
            others += other ? other->label : id;
          }
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1.0f),
                             "   also bound to %s", others.c_str());
        }
      }
      ImGui::EndTable();
    }
    ImGui::TreePop();
  }
  return changed;
}

} // namespace

void ApplySettings(AppState& state) {
  const settings::Settings& s = state.settings;
  // Who a commit is by (F9 C1). GitVault holds it rather than reading the
  // settings file, which would be a file read per commit.
  GitVault::SetPreferredIdentity(s.gitUserName, s.gitUserEmail);
  state.uiFontScale = s.uiFontScale;
  state.showShadows = s.shadows;
  state.showAmbientOcclusion = s.ssao != "off";
  state.ssaoFullRes = s.ssao == "full";
  state.shadowMapSize = s.shadowMap;
}

void MarkSettingsChanged(AppState& state) {
  // The font atlas is rebuilt BETWEEN frames, not during one, so a scale
  // change has to ask for it — ApplySettings only moves the number. Without
  // this, changing Font size in the window did nothing at all: only the old
  // View > Font Size menu ever set the flag.
  const bool fontChanged = state.uiFontScale != state.settings.uiFontScale;
  ApplySettings(state);
  if (fontChanged)
    state.fontRebuildRequested = true;
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
  // Sized in FONT UNITS, not pixels — this window owns the font-size setting,
  // so a literal is wrong the moment someone uses it — and re-applied on
  // every APPEARING rather than FirstUseEver: the first version shipped too
  // small, and FirstUseEver would leave everyone who had already opened it
  // stuck with that size forever in imgui.ini. Centred for the same reason a
  // remembered position is wrong after a resize: it can be off-screen.
  //
  // NOT AlwaysAutoResize: the blurbs wrap to the content region, so letting
  // content set the width makes each blurb one long line, which then sets the
  // width — the window grew past the screen edge.
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  const float em = ImGui::GetFontSize();
  ImGui::SetNextWindowSize(ImVec2(std::min(em * 30.0f, vp->WorkSize.x * 0.9f),
                                  std::min(em * 28.0f, vp->WorkSize.y * 0.85f)),
                           ImGuiCond_Appearing);
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f,
                                 vp->WorkPos.y + vp->WorkSize.y * 0.5f),
                          ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  if (!ImGui::Begin("Preferences###SettingsWindow",
                    &state.settingsWindowOpen)) {
    // Collapsed or closed: an armed capture would keep eating every key
    // press with no visible row to explain why.
    state.keyCaptureId.clear();
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
    // The GPU row says what is actually in use, and says so LOUDLY when the
    // choice no longer matches it — "nothing happened" is the correct
    // behaviour for a restart-tier setting, but only if the window admits it.
    if (std::string(f.key) == "gpu_device" && !state.gpuName.empty()) {
      const std::string& want = state.settings.gpuDevice;
      const bool pending =
          !want.empty() && state.gpuName.find(want) == std::string::npos;
      const std::string inUse = "Rendering on: " + state.gpuName + ".";
      DrawBlurb(f, inUse.c_str());
      if (!state.gpuPreferenceNote.empty() &&
          state.gpuPreferenceNote.find(want) != std::string::npos) {
        // Startup already tried and could not: promising a restart here
        // would be a lie, and the user would take it.
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.45f, 1.0f), "%s",
                           state.gpuPreferenceNote.c_str());
        ImGui::PopTextWrapPos();
      } else if (pending) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1.0f),
                           "Restart Designer to render on %s.", want.c_str());
        ImGui::PopTextWrapPos();
      }
    } else {
      DrawBlurb(f);
    }
    ImGui::Spacing();
  }

  if (ImGui::CollapsingHeader("Keyboard###sec_Keyboard"))
    changed |= DrawKeyboardSection(state);

  ImGui::Separator();
  // A user who has broken their config should not have to find the JSON.
  if (ImGui::Button("Reset all to defaults###set_reset_all")) {
    const auto unknown = state.settings.unknown; // not ours to discard
    state.settings = settings::Settings{};
    state.settings.unknown = unknown;
    state.keyCaptureId.clear(); // a capture armed against a row that is gone
    changed = true;
  }
  ImGui::SameLine();
  ImGui::TextDisabled("%s", settings::ConfigPath().c_str());

  if (changed)
    MarkSettingsChanged(state);
  if (!state.settingsWindowOpen)
    state.keyCaptureId.clear(); // closed by the title-bar X
  ImGui::End();
}

} // namespace ui
