#include "ui/CommandPalette.h"

#include "render/Camera.h"
#include "ui/Commands.h"
#include "ui/FileDialogs.h"
#include "ui/PreviewFeature.h"
#include "ui/SketchToolbar.h"
#include "ui/TerminalPanel.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace ui {

namespace {

// A palette row: the command table's label and gating, resolved for this
// frame. The palette used to carry its own hand-written list of 40-odd
// commands with its own copy of each one's shortcut text; both drifted.
struct PaletteCommand {
  std::string label;    // "Create: Box" — section, then the command's name
  std::string shortcut; // live chord, "" when unbound
  bool enabled = true;
  const char* id = nullptr;
};

// Everything reachable from where the app is right now: the active context's
// commands, then the globals. A hidden binding stays out (the perf HUD).
std::vector<PaletteCommand> BuildCommands(AppState& state) {
  const keymap::Context ctx = ActiveContext(state);
  std::vector<PaletteCommand> cmds;
  const auto collect = [&](keymap::Context want) {
    for (const keymap::Binding& b : keymap::Defaults()) {
      if (b.ctx != want || b.hidden)
        continue;
      if (FindCommand(b.id) == nullptr)
        continue; // metadata with no implementation: the gate test catches it
      cmds.push_back({std::string(b.section) + ": " + b.label,
                      CommandChord(state, b.id), CommandEnabled(state, b.id),
                      b.id});
    }
  };
  collect(ctx);
  if (ctx != keymap::Context::Global)
    collect(keymap::Context::Global);
  return cmds;
}

int FuzzyScore(const std::string& text, const std::string& query) {
  if (query.empty())
    return 1;
  std::string t = text, q = query;
  const auto lower = [](std::string& s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
  };
  lower(t);
  lower(q);
  if (t.rfind(q, 0) == 0)
    return 300;
  if (t.find(q) != std::string::npos)
    return 200;
  size_t qi = 0;
  for (size_t ti = 0; ti < t.size() && qi < q.size(); ti++)
    if (t[ti] == q[qi])
      qi++;
  return qi == q.size() ? 100 : 0;
}

} // namespace

void DrawCommandPalette(AppState& state) {
  // The toggle chord is `palette.open` in the command table.
  if (!state.paletteOpen)
    return;

  static char query[128] = "";
  static int selected = 0;

  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f,
                                 vp->WorkPos.y + vp->WorkSize.y * 0.22f),
                          ImGuiCond_Always, ImVec2(0.5f, 0.0f));
  ImGui::SetNextWindowSize(ImVec2(460.0f, 0.0f), ImGuiCond_Always);
  const ImGuiWindowFlags flags =
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
      ImGuiWindowFlags_NoDocking;
  if (!ImGui::Begin("###CommandPalette", nullptr, flags)) {
    ImGui::End();
    return;
  }

  if (ImGui::IsWindowAppearing()) {
    query[0] = '\0';
    selected = 0;
    ImGui::SetKeyboardFocusHere();
  }
  ImGui::SetNextItemWidth(-FLT_MIN);
  const bool entered =
      ImGui::InputText("###palette_query", query, sizeof(query),
                       ImGuiInputTextFlags_EnterReturnsTrue);
  if (ImGui::IsItemEdited())
    selected = 0;

  // Filtered + ranked commands (stable order within equal scores).
  std::vector<PaletteCommand> all = BuildCommands(state);
  struct Ranked {
    int score;
    const PaletteCommand* cmd;
  };
  std::vector<Ranked> matches;
  for (const PaletteCommand& c : all) {
    const int score = FuzzyScore(c.label, query);
    if (score > 0)
      matches.push_back({score, &c});
  }
  std::stable_sort(
      matches.begin(), matches.end(),
      [](const Ranked& a, const Ranked& b) { return a.score > b.score; });
  constexpr int kMaxShown = 10;
  const int shown = std::min<int>(kMaxShown, static_cast<int>(matches.size()));

  if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
    selected = shown > 0 ? (selected + 1) % shown : 0;
  if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
    selected = shown > 0 ? (selected + shown - 1) % shown : 0;
  selected = std::clamp(selected, 0, std::max(0, shown - 1));

  ImGui::Separator();
  for (int i = 0; i < shown; i++) {
    const PaletteCommand& c = *matches[static_cast<size_t>(i)].cmd;
    ImGui::BeginDisabled(!c.enabled);
    if (ImGui::Selectable((c.label + "###pal" + std::to_string(i)).c_str(),
                          i == selected) &&
        c.enabled) {
      state.paletteOpen = false;
      RunCommand(state, c.id);
    }
    ImGui::EndDisabled();
    if (!c.shortcut.empty()) {
      ImGui::SameLine(ImGui::GetWindowWidth() - 90.0f);
      ImGui::TextDisabled("%s", c.shortcut.c_str());
    }
  }
  if (shown == 0)
    ImGui::TextDisabled("No matching command");

  const bool escape = ImGui::IsKeyPressed(ImGuiKey_Escape);
  if (entered && selected < shown) {
    const PaletteCommand& c = *matches[static_cast<size_t>(selected)].cmd;
    if (c.enabled) {
      state.paletteOpen = false;
      RunCommand(state, c.id);
    }
  } else if (escape ||
             (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
              !ImGui::IsWindowAppearing())) {
    state.paletteOpen = false; // Esc or click-away dismisses
  }
  ImGui::End();
}

} // namespace ui
