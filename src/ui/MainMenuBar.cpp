// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/MainMenuBar.h"
#include "ui/OpenJob.h"
#include <string>
#include <utility>
#include <vector>

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
#include "ui/Commands.h"
#include "ui/FileDialogs.h"
#include "ui/PreviewFeature.h"
#include "ui/ProjectActions.h"
#include "ui/SettingsWindow.h"
#include "ui/SketchInteraction.h"
#include "ui/TerminalPanel.h"
#include "ui/VariableActions.h"

#include "imgui.h"

#include <algorithm>
#include <filesystem>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

namespace ui {

namespace {

// A menu's shortcut column, read from the keymap rather than typed into the
// literal beside the label. An unbound command prints nothing: a menu is not
// the place to advertise that a key is available.
std::string Hint(const AppState& state, const char* id) {
  return CommandChord(state, id);
}

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
      // Off the frame (ui/OpenJob.h). A path that exists but fails to
      // load stays in the list: the modal says why, and the failure may be
      // this build's, not the file's.
      std::string error;
      if (!StartOpenProject(state, path, &error)) {
        state.openError = "Could not open " + path + ": " + error;
        LOG_ERROR("{}", state.openError);
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

  // The menu ITEM and the chord run the same command — they used to be two
  // bodies, and Ctrl+N did strictly less than File > New.
  if (ImGui::MenuItem("New", Hint(state, "file.new").c_str()))
    RunCommand(state, "file.new");
#ifdef HAVE_NATIVEFILEDIALOG
  if (ImGui::MenuItem("Open Project...", Hint(state, "file.open").c_str()))
    RunCommand(state, "file.open");
  DrawOpenRecentMenu(state);
  if (ImGui::MenuItem("Save Project", Hint(state, "file.save").c_str()))
    RunCommand(state, "file.save");
  if (ImGui::MenuItem("Save Project As...",
                      Hint(state, "file.save_as").c_str()))
    RunCommand(state, "file.save_as");
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
  if (ImGui::MenuItem("Import...", Hint(state, "file.import").c_str()))
    RunCommand(state, "file.import");
  // Greyed out where there is nothing to export, or where no system save
  // dialog exists, rather than letting the click vanish (FileDialogs.h). The
  // gate is the command's own, so the palette and the menu agree.
  if (ImGui::MenuItem("Export...", Hint(state, "file.export").c_str(), false,
                      CommandEnabled(state, "file.export")))
    RunCommand(state, "file.export");
  // A sketch reference image is an import too, and it was reachable only from
  // the sketch toolbar — so a person who had not found that button had no way
  // to discover the feature existed. Gated by the command (Sketching), so it
  // greys out off a sketch instead of failing after the file dialog.
  if (ImGui::MenuItem("Reference Image...###file_reference_image",
                      Hint(state, "sketch.image").c_str(), false,
                      CommandEnabled(state, "sketch.image")))
    RunCommand(state, "sketch.image");
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
  if (ImGui::MenuItem("Undo", Hint(state, "edit.undo").c_str(), false, canUndo))
    RunCommand(state, "edit.undo");
  if (ImGui::MenuItem("Redo", Hint(state, "edit.redo").c_str(), false, canRedo))
    RunCommand(state, "edit.redo");
  ImGui::Separator();
  // A floating window that does not otherwise exist — unlike History, whose
  // menu item only re-selected an always-visible tab and was removed.
  if (ImGui::MenuItem("Preferences...###edit_preferences",
                      Hint(state, "edit.preferences").c_str()))
    RunCommand(state, "edit.preferences");
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
      state.camera->SetUp(conv::kWorldUpF);
    }
  }
  if (ImGui::MenuItem("Fit View", Hint(state, "view.fit").c_str()))
    RunCommand(state, "view.fit");
  if (ImGui::MenuItem("Zoom to Selection",
                      Hint(state, "view.zoom_selection").c_str()))
    RunCommand(state, "view.zoom_selection");
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

  // The standard views, discoverable (U5). Names, order and chords all come
  // from the command table, so rebinding one updates this menu with it.
  if (ImGui::BeginMenu("Standard Views")) {
    const auto view = [&](const char* name, const char* id) {
      if (ImGui::MenuItem(name, Hint(state, id).c_str()))
        RunCommand(state, id);
    };
    view("Front", "view.front");
    view("Back", "view.back");
    view("Left", "view.left");
    view("Right", "view.right");
    view("Top", "view.top");
    view("Bottom", "view.bottom");
    view("Isometric", "view.iso");
    ImGui::EndMenu();
  }

  // Rebuild the default docking layout next frame — recovers from a panel
  // dragged huge or a stale imgui.ini.
  if (ImGui::MenuItem("Reset Layout", Hint(state, "view.reset_layout").c_str()))
    RunCommand(state, "view.reset_layout");

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
void DrawShortcutsWindow(AppState& state) {
  if (!state.showShortcuts)
    return;
  // Sized in FONT UNITS and re-applied on every APPEARING, not
  // FirstUseEver — the same rule the Preferences window documents: this
  // window's contents changed shape (it is generated from the keymap now),
  // and FirstUseEver would leave anyone who had opened the old one stuck
  // with its size forever in imgui.ini.
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  const float em = ImGui::GetFontSize();
  ImGui::SetNextWindowSize(ImVec2(std::min(em * 34.0f, vp->WorkSize.x * 0.9f),
                                  std::min(em * 40.0f, vp->WorkSize.y * 0.85f)),
                           ImGuiCond_Appearing);
  if (!ImGui::Begin("Keyboard Shortcuts###ShortcutsWindow",
                    &state.showShortcuts)) {
    ImGui::End();
    return;
  }

  ImGui::PushStyleColor(ImGuiCol_Text,
                        ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::PushTextWrapPos(0.0f); // wrap, or the line just clips off the edge
  ImGui::TextUnformatted(
      "Edit > Preferences > Keyboard changes any of these. A mode's binding "
      "wins over the same key listed under Everywhere.");
  ImGui::PopTextWrapPos();
  ImGui::PopStyleColor();
  ImGui::Spacing();

  const keymap::Keymap& keys = state.settings.keys;
  const auto row = [](const std::string& chord, const char* what) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    if (chord.empty())
      ImGui::TextDisabled("—");
    else
      ImGui::TextUnformatted(chord.c_str());
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(what);
  };

  for (keymap::Context ctx : keymap::AllContexts()) {
    ImGui::SeparatorText(keymap::ContextLabel(ctx));
    if (!ImGui::BeginTable(keymap::ContextName(ctx), 2,
                           ImGuiTableFlags_SizingFixedFit |
                               ImGuiTableFlags_RowBg))
      continue;
    ImGui::TableSetupColumn("Keys", ImGuiTableColumnFlags_WidthFixed,
                            ImGui::GetFontSize() * 9.0f);
    ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch);
    for (const keymap::Binding& b : keymap::Defaults()) {
      if (b.ctx != ctx || b.hidden)
        continue;
      std::string chord = keys.Text(b.id);
      if (const keymap::Chord alt = keys.Alt(b.id))
        chord += (chord.empty() ? "" : " / ") + keymap::FormatChord(alt);
      row(chord, b.label);
    }
    // The modal verbs, listed where people look for them but marked as what
    // they are: fixed. They resolve against armed tools, previews, plane
    // picks and text fields, so they are not commands and cannot be rebound
    // (docs/ui/keybindings.md).
    if (ctx == keymap::Context::Global) {
      row("Esc", "Cancel the current tool, pick or preview (fixed)");
      row("Enter", "Accept the open tool dialog (fixed)");
      row("Delete", "Delete the selection (fixed)");
      row("Arrows", "Orbit; Ctrl pans, Alt rolls (fixed)");
      row("Click", "Select; Ctrl+click adds; click again cycles behind");
      row("Shift+click", "Select a face and look straight at it");
    }
    ImGui::EndTable();
  }
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
    {"libarea Adaptive (FreeCAD)", "LGPL-2.1-or-later"},
    {"Clipper / Clipper2", "BSL-1.0"},
    {"libvterm", "MIT"},
    {"stb_image", "MIT or public domain"},
    {"Font Awesome 6 Free", "SIL OFL 1.1"},
    {"JetBrains Mono", "SIL OFL 1.1"},
    {"Liberation Sans", "SIL OFL 1.1"},
    {"Hershey fonts (Roman Simplex)", "public domain, attribution required"},
    {"Roboto", "Apache-2.0"},
    {"Fastener tables (FreeCAD Fasteners)", "GPL-2.0-or-later"},
    {"Hole tables (bd_warehouse)", "Apache-2.0"},
};

void DrawAboutWindow(AppState& state) {
  if (!state.showAbout)
    return;
  ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Appearing);
  if (!ImGui::Begin("About Loböle###AboutLobole", &state.showAbout,
                    ImGuiWindowFlags_AlwaysAutoResize |
                        ImGuiWindowFlags_NoDocking)) {
    ImGui::End();
    return;
  }

  ImGui::Text("Loböle %s", BuildInfo::Version().c_str());
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
    if (ImGui::MenuItem("Keyboard Shortcuts",
                        Hint(state, "help.shortcuts").c_str()))
      state.showShortcuts = true;
    // Sets a flag; see AppState::showAbout for why this cannot open the
    // popup from in here.
    if (ImGui::MenuItem("About Loböle"))
      state.showAbout = true;
    ImGui::EndMenu();
  }
  // The chord itself belongs to `help.shortcuts` in the one command table.
  DrawShortcutsWindow(state);
  DrawAboutWindow(state);
}

// F8: the project changed on disk, said where it is always visible.
//
// The notices at the right of the menu bar, right-aligned in the order given.
// Each is a DISABLED menu item rather than plain text: it cannot be clicked by
// accident, and unlike a Text widget it has an id, so it is addressable from a
// gui test -- "the notice is visible" is the whole assertion. Disabled items
// draw in TextDisabled, so that is the colour to override.
struct MenuBarNotice {
  const char* label; // "Changed on disk###disk_notice"
  const char* tooltip;
};

void DrawMenuBarNotices(const std::vector<MenuBarNotice>& notices) {
  if (notices.empty())
    return;
  const ImGuiStyle& style = ImGui::GetStyle();
  // Measured, never a pixel literal -- the style scales with the display's
  // content scale and the Font Size setting.
  float width = 0.0f;
  for (const MenuBarNotice& n : notices)
    width += ImGui::CalcTextSize(n.label, nullptr, true).x +
             style.ItemSpacing.x * 2.0f;
  const float x = ImGui::GetWindowWidth() - width;
  if (x <= ImGui::GetCursorPosX())
    return; // a narrow window: the menus themselves come first
  ImGui::SetCursorPosX(x);
  ImGui::PushStyleColor(ImGuiCol_TextDisabled,
                        ImVec4(0.95f, 0.75f, 0.35f, 1.0f));
  for (const MenuBarNotice& n : notices) {
    ImGui::MenuItem(n.label, nullptr, false, false);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ImGui::SetTooltip("%s", n.tooltip);
  }
  ImGui::PopStyleColor();
}

// Text, not buttons. "Changed on disk" needs a reload or a save over it, and
// both throw something away, while the menu bar is the one place in the app
// you can hit by accident: those live where they can be read first, the
// History panel's banner and the modal the next save raises. "Geometry
// rebuilt" is fixed by an ordinary save.
void DrawNotices(const AppState& state) {
  std::vector<MenuBarNotice> notices;
  std::string rebuiltTip;
  if (state.openRebuiltParts > 0) {
    rebuiltTip =
        "Opening rebuilt " + std::to_string(state.openRebuiltParts) +
        (state.openRebuiltParts == 1 ? " part" : " parts") +
        (state.openRebuildReason.empty()
             ? std::string(".")
             : ": " + state.openRebuildReason + ".") +
        "\nSaving stores the geometry again, and the next open is a load "
        "instead of a rebuild.";
    notices.push_back(
        {"Geometry rebuilt###rebuilt_notice", rebuiltTip.c_str()});
  }
  if (state.diskChanged)
    notices.push_back(
        {"Changed on disk###disk_notice",
         "Something else wrote to this project — a command in the "
         "terminal, another editor, or a second window.\nThe History panel can "
         "reload it, and saving will ask before replacing it."});
  DrawMenuBarNotices(notices);
}

} // namespace

void DrawMainMenuBar(AppState& state) {
  // No key handling here any more: every binding is dispatched once per frame
  // from DrawUI, after the panels have drawn (ui::DispatchShortcuts).

  if (!ImGui::BeginMainMenuBar())
    return;
  state.menuBarTop.clear(); // rebuilt by the BeginTopMenu calls below

  DrawFileMenu(state);
  DrawEditMenu(state);
  DrawViewMenu(state);
  DrawHelpMenu(state);
  DrawNotices(state);

  ImGui::EndMainMenuBar();
}

} // namespace ui
