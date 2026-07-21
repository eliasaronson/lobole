#include "ui/CommandPalette.h"

#include "Camera.h"
#include "ui/AppActions.h"
#include "ui/FileDialogs.h"
#include "ui/PreviewFeature.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace ui {

namespace {

struct PaletteCommand {
  std::string label;
  const char* shortcut = nullptr;
  bool enabled = true;
  std::function<void(AppState&)> run;
};

// The command surface, mirrored from the menus (same gating, same actions).
std::vector<PaletteCommand> BuildCommands(AppState& state) {
  const bool part = state.document != nullptr;
  const bool idle = part && !HasPendingFeature(state);
  const size_t producers =
      part ? BodyProducers(*state.document, state.document->FeatureCount())
                 .size()
           : 0;
  const bool hasBody = part && !state.document->GetBodies().empty();

  std::vector<PaletteCommand> cmds;
  const auto add = [&](std::string label, const char* shortcut, bool enabled,
                       std::function<void(AppState&)> run) {
    cmds.push_back({std::move(label), shortcut, enabled, std::move(run)});
  };

  add("Create: Sketch", "S", idle, [](AppState& s) { StartNewSketch(s); });
  add("Create: Box", nullptr, idle, [](AppState& s) { CreateBox(s); });
  add("Create: Sphere", nullptr, idle, [](AppState& s) { CreateSphere(s); });
  add("Create: Cylinder", nullptr, idle,
      [](AppState& s) { CreateCylinder(s); });
  add("Create: Datum Plane", nullptr, idle,
      [](AppState& s) { CreateDatumPlane(s); });
  add("Create: Datum Axis", nullptr, idle,
      [](AppState& s) { CreateDatumAxis(s); });
  add("Create: Boolean", nullptr, idle && producers >= 2,
      [](AppState& s) { CreateBoolean(s); });
  add("Create: Mirror", nullptr, idle && producers >= 1,
      [](AppState& s) { CreateMirror(s); });
  add("Create: Pattern", nullptr, idle && producers >= 1,
      [](AppState& s) { CreatePattern(s); });
  add("Create: Fillet", nullptr, idle && hasBody,
      [](AppState& s) { CreateFillet(s); });
  add("Create: Chamfer", nullptr, idle && hasBody,
      [](AppState& s) { CreateChamfer(s); });
  add("Create: Rib", nullptr, idle && hasBody,
      [](AppState& s) { CreateRib(s); });
  add("Create: Thread", nullptr, idle && hasBody,
      [](AppState& s) { CreateThread(s); });
  const size_t sketches =
      state.document ? SketchFeatureCount(*state.document) : 0;
  const bool hasPathLike =
      state.document && HasCurveOrSketch(*state.document);
  add("Create: 3D Curve", nullptr, idle,
      [](AppState& s) { CreateCurve3D(s); });
  add("Create: Pipe", nullptr, idle && hasPathLike,
      [](AppState& s) { CreatePipe(s); });
  add("Create: Sweep", nullptr, idle && sketches >= 1,
      [](AppState& s) { CreateSweep(s); });
  add("Create: Loft", nullptr, idle && sketches >= 2,
      [](AppState& s) { CreateLoft(s); });

  add("Tool: Measure", nullptr, part || state.assembly != nullptr,
      [](AppState& s) { s.measure = MeasureState{}; s.measure.active = true; });
  add("Tool: Simulation", nullptr, hasBody,
      [](AppState& s) { EnterSimulation(s); });

  add("Edit: Undo", "Ctrl+Z", true, [](AppState& s) { GlobalUndo(s); });
  add("Edit: Redo", "Ctrl+Y", true, [](AppState& s) { GlobalRedo(s); });

  add("View: Fit", "F", true, [](AppState& s) { FitCameraToDocument(s); });
  add("View: Zoom to Selection", "Shift+F", true,
      [](AppState& s) { FitCameraToSelection(s); });
  add("View: Reset Layout", nullptr, true,
      [](AppState& s) { s.buildDefaultDockLayout = true; });
  add("View: Toggle Planes", "Shift+P", part,
      [](AppState& s) { s.showPlanes = !s.showPlanes; });
  add("View: Viewport Controls", nullptr, true,
      [](AppState& s) { s.showControlsHelp = !s.showControlsHelp; });
  const auto stdView = [&](const char* name, glm::vec3 n,
                           const char* shortcut) {
    add(std::string("View: ") + name, shortcut, true,
        [n](AppState& s) {
          if (s.camera)
            s.camera->AlignToFace(glm::vec3(0.0f), n);
        });
  };
  stdView("Front", {0, 0, 1}, "Shift+1");
  stdView("Back", {0, 0, -1}, "Shift+2");
  stdView("Left", {-1, 0, 0}, "Shift+3");
  stdView("Right", {1, 0, 0}, "Shift+4");
  stdView("Top", {0, 1, 0}, "Shift+5");
  stdView("Bottom", {0, -1, 0}, "Shift+6");
  stdView("Isometric", glm::normalize(glm::vec3(1, 1, 1)), "Shift+7");

  add("File: New", "Ctrl+N", true, [](AppState& s) {
    ReplaceDocument(s, std::make_unique<Document>());
  });
#ifdef HAVE_NATIVEFILEDIALOG
  add("File: Open Project...", "Ctrl+O", true,
      [](AppState& s) { OpenProjectDialog(s); });
  add("File: Save Project As...", "Ctrl+S", true,
      [](AppState& s) { SaveProjectDialog(s); });
#endif
  add("File: Import STEP...", "Ctrl+I", true,
      [](AppState& s) { OpenStepFileDialog(s); });
  add("File: Export...", nullptr, hasBody,
      [](AppState& s) { ExportDialog(s); });
  return cmds;
}

// Case-insensitive fuzzy score: 0 = no match; higher = better (prefix beats
// substring beats subsequence).
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
  // Ctrl+K toggles from anywhere (a focused text field keeps its Ctrl+K).
  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_K, ImGuiInputFlags_RouteGlobal))
    state.paletteOpen = !state.paletteOpen;
  if (!state.paletteOpen)
    return;

  static char query[128] = "";
  static int selected = 0;

  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(
      ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f,
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
  std::stable_sort(matches.begin(), matches.end(),
                   [](const Ranked& a, const Ranked& b) {
                     return a.score > b.score;
                   });
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
      c.run(state);
    }
    ImGui::EndDisabled();
    if (c.shortcut != nullptr) {
      ImGui::SameLine(ImGui::GetWindowWidth() - 90.0f);
      ImGui::TextDisabled("%s", c.shortcut);
    }
  }
  if (shown == 0)
    ImGui::TextDisabled("No matching command");

  const bool escape = ImGui::IsKeyPressed(ImGuiKey_Escape);
  if (entered && selected < shown) {
    const PaletteCommand& c = *matches[static_cast<size_t>(selected)].cmd;
    if (c.enabled) {
      state.paletteOpen = false;
      c.run(state);
    }
  } else if (escape || (!ImGui::IsWindowFocused(
                            ImGuiFocusedFlags_RootAndChildWindows) &&
                        !ImGui::IsWindowAppearing())) {
    state.paletteOpen = false; // Esc or click-away dismisses
  }
  ImGui::End();
}

} // namespace ui
