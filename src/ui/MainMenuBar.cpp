#include "ui/MainMenuBar.h"
#include <utility>

#include "cad/FaceSelector.h"
#include "common/BuildInfo.h"
#include "common/DesignerVersion.h"
#include "common/Logger.h"
#include "core/Feature.h"
#include "core/features/SketchFeature.h"
#include "core/io/RecentProjects.h"
#include "core/io/Serialization.h"
#include "render/Camera.h"
#include "render/OutlineRenderer.h"
#include "ui/AppActions.h"
#include "ui/FileDialogs.h"
#include "ui/PreviewFeature.h"
#include "ui/SettingsWindow.h"
#include "ui/SketchInteraction.h"
#include "ui/TerminalPanel.h"

#include "imgui.h"

#include <filesystem>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

namespace ui {

namespace {

// BeginMenu for a TOP-LEVEL menu: records the name for menu_items (the test
// engine cannot enumerate the bar itself), then behaves exactly like
// ImGui::BeginMenu.
bool BeginTopMenu(AppState& state, const char* label) {
  state.menuBarTop.push_back(label);
  return ImGui::BeginMenu(label);
}
// File > Open Recent. The list is the app's own history (RecentProjects),
// kept in the state dir, and it INCLUDES paths that are missing right now —
// an unplugged drive is not a reason to forget a project you had open. Those
// entries are disabled and say why, with a one-click way to drop them.
void DrawOpenRecentMenu(AppState& state) {
  const std::vector<std::string> recent = RecentProjects::List();
  if (!ImGui::BeginMenu("Open Recent", !recent.empty()))
    return;
  std::string forget;
  size_t index = 0;
  for (const std::string& path : recent) {
    const bool exists = std::filesystem::exists(path);
    // The basename is what a person recognises; the full path is the
    // tooltip, because two "bracket.dsgn" in different folders are common.
    std::string label = std::filesystem::path(path).filename().string();
    if (label.empty())
      label = path;
    ImGui::BeginDisabled(!exists);
    // "###" and an INDEX, not the path: "##" would fold the visible name
    // into the id (so a renamed project moves), and a path contains slashes,
    // which a menu ref parses as further submenu levels.
    if (ImGui::MenuItem(
            (label + "###recent" + std::to_string(index)).c_str())) {
      std::string error;
      if (OpenProject(state, path, &error)) {
        MarkProjectOpened(state, path);
      } else {
        LOG_ERROR("Could not open {}: {}", path, error);
        forget = path; // it is in the list but no longer openable
      }
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      ImGui::SetTooltip("%s%s", path.c_str(),
                        exists ? ""
                               : "\n(not found — the file has moved or "
                                 "its drive is not mounted)");
    }
    index++;
    if (!exists && ImGui::BeginPopupContextItem(("###rctx" + path).c_str())) {
      if (ImGui::MenuItem("Remove from list"))
        forget = path;
      ImGui::EndPopup();
    }
  }
  ImGui::Separator();
  if (ImGui::MenuItem("Clear list"))
    RecentProjects::Clear();
  ImGui::EndMenu();
  if (!forget.empty())
    RecentProjects::Forget(forget);
}

void DrawFileMenu(AppState& state) {
  if (!BeginTopMenu(state, "File"))
    return;

  if (ImGui::MenuItem("New", "Ctrl+N")) {
    ReplaceDocument(state, std::make_unique<Document>());
    state.parts.clear();
    state.tabFolders.clear();
    ClearSections(state); // a new project starts uncut
    EnsureParts(state, "Part 1");
    MarkProjectOpened(state, ""); // a fresh document has no home yet
  }
#ifdef HAVE_NATIVEFILEDIALOG
  if (ImGui::MenuItem("Open Project...", "Ctrl+O")) {
    OpenProjectDialog(state);
  }
  DrawOpenRecentMenu(state);
  if (ImGui::MenuItem("Save Project", "Ctrl+S")) {
    SaveProjectAction(state);
  }
  if (ImGui::MenuItem("Save Project As...", "Ctrl+Shift+S")) {
    SaveProjectDialog(state);
  }
#endif
  // No version-history entry here. F6 removed the F4 "Checkpoints" submenu
  // (it snapshotted only the ACTIVE PART, silently leaving out the assembly,
  // the other parts and the drawings) and the History panel replaced it —
  // but the panel is a permanent tab beside Model, exactly like Variables
  // and Configurations, neither of which has a menu entry either. A File
  // item that only selects an already-visible tab earns nothing.
  ImGui::Separator();
  // One import for STEP/STL/BREP: an assembly-structured STEP becomes part
  // tabs + an assembly tab in the CURRENT project (imported and hand-modeled
  // parts mix freely); anything else lands as a feature in the active part.
  if (ImGui::MenuItem("Import...", "Ctrl+I")) {
    ImportFileDialog(state);
  }
  // Enabled for a part with bodies, an assembly (visual export walks its
  // occurrences), or a drawing with views (SVG/PDF sheet export).
  const bool canExport =
      (state.document && !state.document->GetBodies().empty()) ||
      (state.assembly && state.assembly->OccurrenceCount() > 0) ||
      (state.drawing && !state.drawing->GetViews().empty());
  // Grey it out where no system save dialog exists rather than letting the
  // click vanish (FileDialogs.h).
  if (ImGui::MenuItem("Export...", nullptr, false,
                      canExport && FileDialogsAvailable())) {
    ExportDialog(state);
  }
  ImGui::Separator();
  if (ImGui::MenuItem("Exit")) {
    glfwSetWindowShouldClose(state.window, GLFW_TRUE);
  }
  ImGui::EndMenu();
}

void DrawEditMenu(AppState& state) {
  if (!BeginTopMenu(state, "Edit"))
    return;

  const bool pending = HasPendingFeature(state);
  // Global undo (Phase V): the active tab's stack OR the cross-tab project-var
  // stack, whichever was most recently mutated (see GlobalUndo).
  const bool canUndo = !pending && CanGlobalUndo(state);
  const bool canRedo = !pending && CanGlobalRedo(state);
  if (ImGui::MenuItem("Undo", "Ctrl+Z", false, canUndo)) {
    GlobalUndo(state);
  }
  if (ImGui::MenuItem("Redo", "Ctrl+Y", false, canRedo)) {
    GlobalRedo(state);
  }
  ImGui::Separator();
  // A floating window that does not otherwise exist — unlike History, whose
  // menu item only re-selected an always-visible tab and was removed.
  if (ImGui::MenuItem("Preferences...###edit_preferences", "Ctrl+,")) {
    state.settingsWindowOpen = true;
  }
  ImGui::EndMenu();
}

// The Create menu was retired in favor of the model toolbar (feature buttons)
// and the feature tree's "Default geometry" plane entries (start-a-sketch);
// the toolbar's sketch button drives the interactive plane/face pick. See the
// GUI tests, which enter sketches via those paths.

void DrawViewMenu(AppState& state) {
  if (!BeginTopMenu(state, "View"))
    return;

  if (ImGui::MenuItem("Reset Camera")) {
    if (state.camera) {
      state.camera->SetPosition(Camera::DefaultIsometricPosition());
      state.camera->SetTarget(glm::vec3(0.0f, 0.0f, 0.0f));
      // Trackball orbit carries the up vector as state, so it must be reset too
      // — otherwise a tumbled view leaves up near-parallel to the new forward
      // and the reset view is degenerate (right = forward x up collapses).
      state.camera->SetUp(glm::vec3(0.0f, 1.0f, 0.0f));
    }
  }
  if (ImGui::MenuItem("Fit View", "F")) {
    FitCameraToDocument(state);
  }
  if (ImGui::MenuItem("Zoom to Selection", "Shift+F")) {
    FitCameraToSelection(state);
  }
  ImGui::MenuItem("Show Planes", nullptr, &state.showPlanes);
  // Render quality (shadows, SSAO, shadow map) and font size used to live
  // here as session flags that reset every launch. They are USER PREFERENCES
  // now — Edit > Preferences owns them, and duplicating a control is how the
  // two ends of it drift apart.
  ImGui::MenuItem("Show Element Names", nullptr, &state.showElementNames);
  ImGui::MenuItem("Viewport Controls", nullptr, &state.showControlsHelp);
  if (state.topoGhostStudyId != 0 && ImGui::MenuItem("Hide Topology Ghost")) {
    // The translucent optimized-shape overlay a topology study leaves for
    // re-modeling; discoverable off-switch.
    state.topoGhostStudyId = 0;
    state.syncedDocumentVersion = UINT64_MAX;
  }

  // The Shift+1..7 views, discoverable (U5).
  if (ImGui::BeginMenu("Standard Views")) {
    const auto view = [&](const char* name, glm::vec3 n, const char* shortcut) {
      if (ImGui::MenuItem(name, shortcut) && state.camera)
        // The camera's OWN pivot, not the world origin. Every standard
        // view used to slam the target to (0,0,0): fit the view on a part
        // modelled away from the origin, press Front, and you are looking at
        // empty space beside it. A standard view is an orientation, not a
        // place (docs/cam/dogfood-round.md).
        state.camera->AlignToFace(state.camera->GetTarget(), n);
    };
    view("Front", {0, 0, 1}, "Shift+1");
    view("Back", {0, 0, -1}, "Shift+2");
    view("Left", {-1, 0, 0}, "Shift+3");
    view("Right", {1, 0, 0}, "Shift+4");
    view("Top", {0, 1, 0}, "Shift+5");
    view("Bottom", {0, -1, 0}, "Shift+6");
    view("Isometric", glm::normalize(glm::vec3(1, 1, 1)), "Shift+7");
    ImGui::EndMenu();
  }

  if (ImGui::MenuItem("Reset Layout")) {
    // Rebuild the default docking layout next frame — recovers from a panel
    // dragged huge or a stale imgui.ini.
    state.buildDefaultDockLayout = true;
  }

  ImGui::Separator();

  if (state.outlineRenderer && state.outlineRenderer->IsInitialized()) {
    if (ImGui::BeginMenu("Outline Rendering")) {
      OutlineSettings settings = state.outlineRenderer->GetOutlineSettings();
      bool changed = false;

      const char* outlineModes[] = {"None", "Edges", "Wireframe", "Silhouette"};
      int currentMode = std::to_underlying(settings.mode);
      if (ImGui::Combo("Mode", &currentMode, outlineModes, 4)) {
        settings.mode = static_cast<OutlineMode>(currentMode);
        changed = true;
      }

      if (settings.mode != OutlineMode::NONE) {
        changed |= ImGui::ColorEdit3("Outline Color", &settings.outlineColor.x);
        changed |= ImGui::SliderFloat("Outline Width", &settings.outlineWidth,
                                      0.1f, 5.0f);
        if (settings.mode == OutlineMode::WIREFRAME) {
          changed |=
              ImGui::ColorEdit3("Wireframe Color", &settings.wireframeColor.x);
          changed |=
              ImGui::SliderFloat("Line Width", &settings.lineWidth, 0.5f, 3.0f);
        }
        changed |= ImGui::Checkbox("Depth Test", &settings.enableDepthTest);
        changed |= ImGui::Checkbox("Blending", &settings.enableBlending);
      }

      if (changed) {
        state.outlineRenderer->SetOutlineSettings(settings);
      }
      ImGui::EndMenu();
    }
  }

  ImGui::EndMenu();
}

// Every binding the app actually has, grouped the way a user looks for one.
// Ctrl+K was advertised nowhere, and the sketch keymap only in the source.
void DrawShortcutsWindow(bool& open) {
  if (!open)
    return;
  ImGui::SetNextWindowSize(ImVec2(520, 560), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Keyboard Shortcuts###ShortcutsWindow", &open)) {
    ImGui::End();
    return;
  }
  struct Row {
    const char* keys;
    const char* what;
  };
  const auto section = [](const char* title, const Row* rows, size_t count) {
    ImGui::SeparatorText(title);
    if (ImGui::BeginTable(
            title, 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
      ImGui::TableSetupColumn("Keys", ImGuiTableColumnFlags_WidthFixed,
                              ImGui::GetFontSize() * 8.0f);
      ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch);
      for (size_t i = 0; i < count; i++) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(rows[i].keys);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(rows[i].what);
      }
      ImGui::EndTable();
    }
  };

  static const Row kGeneral[] = {
      {"Ctrl+K", "Command palette - every action, by name"},
      {"Ctrl+N / O / S", "New / Open / Save project"},
      {"Ctrl+Shift+S", "Save Project As"},
      {"Ctrl+I", "Import STEP / STL / BREP"},
      {"Ctrl+Z / Ctrl+Y", "Undo / Redo"},
      {"Esc", "Cancel the current tool, pick or preview"},
      {"Enter", "Accept the open tool dialog"},
      {"Delete", "Delete the selection"},
  };
  static const Row kView[] = {
      {"F", "Fit the view to the model"},
      {"Shift+F", "Zoom to the selection"},
      {"N", "Look straight at the selected (or hovered) face"},
      {"Shift+1..6", "Front / Back / Left / Right / Top / Bottom"},
      {"Shift+7", "Isometric"},
      {"Shift+P", "Show or hide the base planes"},
      {"Arrows", "Orbit the camera"},
  };
  static const Row kModel[] = {
      {"S", "New sketch - then click a plane or a flat face"},
      {"Click", "Select; Ctrl+click adds; click again cycles what's behind"},
      {"Shift+click", "Select a face and look straight at it"},
  };
  static const Row kSketch[] = {
      {"L / C / A", "Line / Circle / 3-point arc"},
      {"G / Shift+G", "Rectangle (corner / centre)"},
      {"S / O / M", "Spline / Offset / Trim"},
      {"X", "Project model edges into the sketch"},
      {"D", "Dimension"},
      {"Shift+F", "Fillet a corner"},
      {"Q", "Toggle construction on the selection"},
      {"Space", "Clear the selection"},
      {"H / V / N", "Horizontal / Vertical / Coincident constraint"},
      {"B / T / E", "Parallel / Tangent / Equal constraint"},
      {"Shift+S", "Suspend constraint inferencing while drawing"},
      {"Shift+E / Shift+R", "Extrude / Revolve this sketch"},
      {"Shift+H", "Drill the sketch's points as holes"},
      {"Esc", "Back to Select, then leave the sketch"},
  };
  section("General", kGeneral, IM_ARRAYSIZE(kGeneral));
  section("View", kView, IM_ARRAYSIZE(kView));
  section("Modeling", kModel, IM_ARRAYSIZE(kModel));
  section("Sketching", kSketch, IM_ARRAYSIZE(kSketch));
  ImGui::End();
}

// The third-party components a user is entitled to know about — the LGPL and
// MPL ones require the notice, and the Test Engine's terms decide whether a
// company may use this build at all. Licenses verified against each project's
// own LICENSE file or its nixpkgs metadata, not from memory; when in doubt the
// project's own text governs, which is what the closing line says.
struct Attribution {
  const char* name;
  const char* license;
};
constexpr Attribution kThirdParty[] = {
    {"OpenCASCADE", "LGPL-2.1 with exception"},
    {"Dear ImGui", "MIT"},
    {"Dear ImGui Test Engine", "Dear ImGui Test Engine License (DISCO HELLO)"},
    {"PlaneGCS (FreeCAD)", "LGPL-2.1-or-later"},
    {"Netgen", "LGPL-2.1-or-later"},
    {"Eigen", "MPL-2.0"},
    {"Spectra", "MPL-2.0"},
    {"GLFW", "Zlib"},
    {"nativefiledialog-extended", "Zlib"},
    {"glm", "MIT"},
    {"spdlog / fmt", "MIT"},
    {"nlohmann/json", "MIT"},
    {"FreeType", "FTL or GPL-2.0-or-later"},
    {"Boost", "BSL-1.0"},
    {"Font Awesome 6 Free", "SIL OFL 1.1"},
};

void DrawAboutWindow(AppState& state) {
  if (!state.showAbout)
    return;
  ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Appearing);
  if (!ImGui::Begin("About Designer###AboutDesigner", &state.showAbout,
                    ImGuiWindowFlags_AlwaysAutoResize |
                        ImGuiWindowFlags_NoDocking)) {
    ImGui::End();
    return;
  }

  ImGui::Text("Designer %s", BuildInfo::Version().c_str());
  ImGui::TextUnformatted("Open-source parametric 3D CAD");
  ImGui::TextUnformatted("Licensed under AGPL-3.0-or-later");
  ImGui::Separator();

  // The block a bug report needs. Laid out as label/value rows rather than
  // prose so the copy below reads the same as the screen.
  const auto row = [](const char* label, const std::string& value) {
    ImGui::TextDisabled("%-12s", label);
    ImGui::SameLine();
    ImGui::TextUnformatted(value.c_str());
  };
  row("Commit", BuildInfo::Commit() + "  (at configure time)");
  row("Build", BuildInfo::Configuration());
  row("Kernel", "OpenCASCADE " + BuildInfo::KernelVersion());
  row("Renderer",
      state.gpuName.empty()
          ? std::string("(no renderer)")
          : state.gpuName + (state.gpuApiVersion.empty()
                                 ? ""
                                 : "  Vulkan " + state.gpuApiVersion));

  ImGui::Separator();
  if (ImGui::TreeNode("Third-party components")) {
    if (ImGui::BeginTable("###about_thirdparty", 2,
                          ImGuiTableFlags_SizingStretchProp)) {
      for (const Attribution& a : kThirdParty) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(a.name);
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", a.license);
      }
      ImGui::EndTable();
    }
    ImGui::TextWrapped(
        "Each project's own license text governs; see its repository.");
    ImGui::TreePop();
  }

  ImGui::Separator();
  if (ImGui::Button("Copy build info###about_copy")) {
    ImGui::SetClipboardText(
        BuildInfo::Summary(state.gpuName, state.gpuApiVersion).c_str());
  }
  ImGui::SameLine();
  ImGui::TextDisabled("(paste this into a bug report)");
  ImGui::SameLine();
  if (ImGui::Button("Close###about_close"))
    state.showAbout = false;
  ImGui::End();
}

void DrawHelpMenu(AppState& state) {
  if (BeginTopMenu(state, "Help")) {
    if (ImGui::MenuItem("Keyboard Shortcuts", "Ctrl+/"))
      state.showShortcuts = true;
    // Sets a flag; see AppState::showAbout for why this cannot open the
    // popup from in here.
    if (ImGui::MenuItem("About Designer"))
      state.showAbout = true;
    ImGui::EndMenu();
  }
  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Slash))
    state.showShortcuts = !state.showShortcuts;
  DrawShortcutsWindow(state.showShortcuts);
  DrawAboutWindow(state);
}

// Global keyboard shortcuts. Registered every frame; ImGui's routing lets a
// focused text field claim chords like Ctrl+Z for itself first.
void HandleShortcuts(AppState& state) {
  // The terminal owns the keyboard while the pointer is over it
  // (docs/ui/terminal-panel.md). Without this gate Ctrl+Z would mean two
  // things at once — undo a feature AND suspend the job — and which one you
  // got would depend on invisible focus state.
  if (TerminalCapturesKeys(state))
    return;

  const ImGuiInputFlags route = ImGuiInputFlags_RouteGlobal;

  // Preferences: the platform-conventional chord, and it works mid-preview
  // (opening a window changes nothing about the pending feature).
  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Comma, route))
    state.settingsWindowOpen = true;

  // While a feature preview is pending, freeze the undo stack: Ctrl+Z / Escape
  // cancel the preview instead of touching history.
  if (HasPendingFeature(state)) {
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, route))
      CancelPendingFeature(state);
    return;
  }

  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, route)) {
    GlobalUndo(state);
  }
  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y, route) ||
      ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, route)) {
    GlobalRedo(state);
  }
  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_N, route)) {
    ReplaceDocument(state, std::make_unique<Document>());
  }
#ifdef HAVE_NATIVEFILEDIALOG
  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_O, route)) {
    OpenProjectDialog(state);
  }
  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, route)) {
    SaveProjectAction(state);
  }
  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S, route)) {
    SaveProjectDialog(state);
  }
#endif
  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_I, route)) {
    ImportFileDialog(state);
  }
}

// F8: the project changed on disk, said where it is always visible.
//
// Text, not a button. The actions this state needs — reload, or save over it
// — both throw something away, and the menu bar is the one place in the app
// you can hit by accident. They live where they can be read first: the
// History panel's banner, and the modal the next save raises.
void DrawDiskChangeNotice(const AppState& state) {
  if (!state.diskChanged)
    return;
  const char* label = "Changed on disk";
  const ImGuiStyle& style = ImGui::GetStyle();
  // Measured, never a pixel literal — the style scales with the display's
  // content scale and the Font Size setting.
  const float width = ImGui::CalcTextSize(label).x + style.ItemSpacing.x * 2.0f;
  const float x = ImGui::GetWindowWidth() - width;
  if (x <= ImGui::GetCursorPosX())
    return; // a narrow window: the menus themselves come first
  ImGui::SetCursorPosX(x);
  // A DISABLED menu item rather than plain text: it cannot be clicked by
  // accident, and unlike a Text widget it has an id, so it is addressable
  // from a gui test — "the notice is visible" is the whole assertion.
  // Disabled items draw in TextDisabled, so that is the colour to override.
  ImGui::PushStyleColor(ImGuiCol_TextDisabled,
                        ImVec4(0.95f, 0.75f, 0.35f, 1.0f));
  ImGui::MenuItem("Changed on disk###disk_notice", nullptr, false, false);
  ImGui::PopStyleColor();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("Something else wrote to this project — a command in "
                      "the terminal, another editor, or a second window.\n"
                      "The History panel can reload it, and saving will ask "
                      "before replacing it.");
  }
}

} // namespace

void DrawMainMenuBar(AppState& state) {
  HandleShortcuts(state);

  if (!ImGui::BeginMainMenuBar())
    return;
  state.menuBarTop.clear(); // rebuilt by the BeginTopMenu calls below

  DrawFileMenu(state);
  DrawEditMenu(state);
  DrawViewMenu(state);
  DrawHelpMenu(state);
  DrawDiskChangeNotice(state);

  ImGui::EndMainMenuBar();
}

} // namespace ui
