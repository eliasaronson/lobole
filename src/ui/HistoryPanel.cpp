#include "ui/HistoryPanel.h"

#include "common/Logger.h"
#include "core/io/GitVault.h"
#include "ui/AppActions.h"
#include "ui/FileDialogs.h"

#include "imgui.h"
#include "imgui_internal.h" // dock node tab selection; see DrawHistoryPanel

#include <ctime>
#include <string>

namespace ui {

namespace {

// How many versions the panel lists. Long enough that a day's work fits;
// git holds the rest, and "restore something older" is a job for the tool
// that owns the whole history.
constexpr int kVersionsShown = 60;

// Re-reading the log means spawning `git`, so the panel works off a cache
// (AppState::historyLog) and never refreshes faster than this.
constexpr double kRefreshSeconds = 1.0;

// "3 minutes ago". Coarse on purpose: which of two versions is newer is what
// a person reads off this column, not a duration.
std::string Ago(int64_t unixTime, int64_t now) {
  const int64_t s = now - unixTime;
  if (s < 60)
    return "just now";
  if (s < 3600)
    return std::to_string(s / 60) + (s < 120 ? " minute ago" : " minutes ago");
  if (s < 86400)
    return std::to_string(s / 3600) + (s < 7200 ? " hour ago" : " hours ago");
  if (s < 172800)
    return "yesterday";
  if (s < 2592000)
    return std::to_string(s / 86400) + " days ago";
  char buf[32];
  const std::time_t t = static_cast<std::time_t>(unixTime);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
  return buf;
}

void RefreshHistory(AppState& state, bool force) {
  const double now = ImGui::GetTime();
  const bool stale = state.historyStamp < 0.0 ||
                     now - state.historyStamp > kRefreshSeconds ||
                     state.historyPath != state.projectPath;
  if (!force && !stale)
    return;
  state.historyStamp = now;
  state.historyPath = state.projectPath;
  state.historyLog = GitVault::Log(state.projectPath, kVersionsShown);
  state.historyUncommitted = GitVault::Dirty(state.projectPath);
}

} // namespace

bool CanVersionProject(const AppState& state) {
  // `.dsgn` is always the directory format (Serialization: IsDirFormatPath),
  // and only a directory can be a repository.
  return !state.projectPath.empty() && state.projectPath.ends_with(".dsgn") &&
         GitVault::Available();
}

bool ProjectHasHistory(const AppState& state) {
  return CanVersionProject(state) && GitVault::IsRepo(state.projectPath);
}

bool NameCurrentVersion(AppState& state, const std::string& name,
                        std::string* error) {
  if (state.projectPath.empty()) {
    if (error)
      *error = "save the project before naming a version";
    return false;
  }
  if (!CanVersionProject(state)) {
    if (error)
      *error = GitVault::Available()
                   ? "versions need a .dsgn project folder; this project is "
                     "a single file (File > Save Project As)"
                   : "versions need git on PATH";
    return false;
  }
  if (GitVault::TagRef(name).empty()) {
    if (error)
      *error = "a version name needs at least one letter or digit";
    return false;
  }
  std::string saveError;
  if (!SaveProject(state, state.projectPath, &saveError)) {
    if (error)
      *error = saveError.empty() ? "could not save the project" : saveError;
    return false;
  }
  // The name is the commit SUBJECT (the tag is only an address — TagRef).
  // This save commits it when there is something to commit...
  MarkProjectSaved(state, state.projectPath, name);
  // ...and when there is not — naming a state you just saved, which is the
  // common case — the name would otherwise land on a commit called "save".
  // An empty commit carrying the name is what "mark this state" means.
  const auto head = GitVault::Log(state.projectPath, 1);
  if (head.empty() || head[0].subject != name) {
    std::string commitError;
    if (!GitVault::CommitAll(state.projectPath, name, &commitError, true)) {
      if (error)
        *error =
            commitError.empty() ? "could not name this version" : commitError;
      return false;
    }
  }
  if (!GitVault::Tag(state.projectPath, name)) {
    if (error)
      *error = "could not name this version";
    return false;
  }
  state.historyStamp = -1.0; // the list just changed
  return true;
}

bool RestoreVersion(AppState& state, const std::string& commit,
                    const std::string& label, std::string* error) {
  if (!GitVault::RestoreTo(state.projectPath, commit, error))
    return false;
  // Commit the restore rather than leaving the tree ahead of the history:
  // the list only grows, and the state being left is still in it.
  std::string commitError;
  if (!GitVault::CommitAll(state.projectPath, "Restore \"" + label + "\"",
                           &commitError))
    LOG_WARN("Restored, but could not record it: {}", commitError);
  if (!OpenProject(state, state.projectPath, error))
    return false;
  MarkProjectOpened(state, state.projectPath);
  state.historyStamp = -1.0;
  return true;
}

void DrawHistoryPanel(AppState& state) {
  // File > History asks for this tab. Honouring it takes REPEATING the
  // request for a short while, not issuing it once: the menu's own focus is
  // undone when the popup closes, and ImGui's Begin returns true a frame
  // before an about-to-be-selected tab is actually up — so "we are visible,
  // stop asking" drops the request one frame early and the tab never comes
  // forward (measured: 2 frames of asking does nothing, ~1 s works). The
  // frame budget is the safety valve, so a request that can never be
  // satisfied does not fight the user for the tab forever.
  if (state.focusHistoryFrames > 0) {
    state.focusHistoryFrames--;
    ImGui::SetNextWindowFocus();
    // ...and select the TAB explicitly. ImGui::FocusWindow deliberately does
    // not ("For #2304 we avoid applying focus immediately before the tabbar
    // is visible", imgui.cpp), so focus alone can leave the panel behind
    // whichever sibling the saved layout had selected. This is the same
    // request a click on the tab queues.
    ImGuiWindow* w = ImGui::FindWindowByName("History");
    if (w != nullptr && w->DockNode != nullptr &&
        w->DockNode->TabBar != nullptr) {
      if (ImGuiTabItem* tab =
              ImGui::TabBarFindTabByID(w->DockNode->TabBar, w->TabId)) {
        ImGui::TabBarQueueFocus(w->DockNode->TabBar, tab);
      }
    }
  }
  // A secondary tab must not steal the shared dock node when it first
  // appears (see the Variables panel).
  const bool visible =
      ImGui::Begin("History", nullptr, ImGuiWindowFlags_NoFocusOnAppearing);
  if (!visible) {
    // Behind another tab (or collapsed). ImGui would skip the widgets anyway,
    // but RefreshHistory would not: it spawns `git` twice a second for a
    // panel nobody is looking at.
    ImGui::End();
    return;
  }

  // Refusals and failures live here until the next action.
  static std::string actionError;
  static char nameBuf[128] = {};

  if (state.projectPath.empty()) {
    ImGui::TextWrapped(
        "This project has no home yet. Save it, and every save from then on "
        "becomes a version you can go back to.");
    if (ImGui::Button("Save Project...")) {
      SaveProjectAction(state);
      actionError.clear();
    }
    ImGui::End();
    return;
  }
  if (!GitVault::Available()) {
    // The one place the storage engine is named: the user has to act on it.
    ImGui::TextWrapped("Version history needs `git` on your PATH. Saving "
                       "works without it — only the history does not.");
    ImGui::End();
    return;
  }
  if (!ProjectHasHistory(state)) {
    ImGui::TextWrapped(
        "This project is not stored as a .dsgn project folder, so it has no "
        "version history. Save it as one (File > Save Project As) to start "
        "one.");
    ImGui::End();
    return;
  }

  RefreshHistory(state, false);

  // Save state. `ProjectDirty` is the app's own answer (free); the git-level
  // check catches a state that reached disk but not the history — an
  // external edit, or a save while git was unavailable.
  const bool dirty = ProjectDirty(state);
  if (dirty) {
    ImGui::TextUnformatted("Unsaved changes");
    ImGui::SameLine();
    if (ImGui::SmallButton("Save###hist_save")) {
      SaveProjectAction(state);
      state.historyStamp = -1.0;
      actionError.clear();
    }
  } else if (state.historyUncommitted) {
    ImGui::TextUnformatted("Saved, not yet a version");
    ImGui::SameLine();
    if (ImGui::SmallButton("Add to history###hist_commit")) {
      MarkProjectSaved(state, state.projectPath, "save");
      state.historyStamp = -1.0;
    }
  } else {
    ImGui::TextDisabled("Up to date");
  }

  ImGui::Separator();

  // Name the current state. Saves first — a name refers to a state on disk.
  ImGui::SetNextItemWidth(-70.0f);
  const bool entered = ImGui::InputTextWithHint(
      "###hist_name", "Name this version...", nameBuf, sizeof(nameBuf),
      ImGuiInputTextFlags_EnterReturnsTrue);
  ImGui::SameLine();
  ImGui::BeginDisabled(nameBuf[0] == '\0');
  const bool clicked = ImGui::Button("Name###hist_name_go");
  ImGui::EndDisabled();
  if ((entered || clicked) && nameBuf[0] != '\0') {
    std::string error;
    if (NameCurrentVersion(state, nameBuf, &error)) {
      nameBuf[0] = '\0';
      actionError.clear();
    } else {
      actionError = error;
    }
  }

  ImGui::Separator();

  if (state.historyLog.empty()) {
    ImGui::TextDisabled("No versions yet — the next save makes one.");
    ImGui::End();
    return;
  }

  // Restoring while there are unsaved changes would throw them away with no
  // way back, and saving is one keystroke.
  if (dirty) {
    ImGui::TextDisabled("Save to restore an earlier version.");
  }

  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  // The action runs after the loop: restoring reopens the project, which
  // rebuilds the very list being walked.
  std::string restoreHash, restoreLabel;

  if (ImGui::BeginTable("##versions", 3,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                            ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("Version", ImGuiTableColumnFlags_WidthStretch,
                            1.6f);
    ImGui::TableSetupColumn("When", ImGuiTableColumnFlags_WidthStretch, 0.9f);
    ImGui::TableSetupColumn("##restore", ImGuiTableColumnFlags_WidthFixed,
                            58.0f);
    for (int i = 0; i < static_cast<int>(state.historyLog.size()); i++) {
      const GitVault::LogEntry& e = state.historyLog[i];
      ImGui::TableNextRow();
      // Row-indexed "###" ids rather than PushID: the same handle addresses
      // the button from a gui test and keeps it stable as the list grows.
      const std::string rowId = "###hist_restore" + std::to_string(i);

      ImGui::TableSetColumnIndex(0);
      // A named version is the one a person meant to keep; an ordinary save
      // is just a point in time. Colour is the whole distinction.
      if (e.named)
        ImGui::TextUnformatted(e.subject.c_str());
      else
        ImGui::TextDisabled("%s", e.subject.c_str());

      ImGui::TableSetColumnIndex(1);
      ImGui::TextDisabled("%s", Ago(e.unixTime, now).c_str());

      ImGui::TableSetColumnIndex(2);
      // The newest version IS the current state; there is nothing to go back
      // to (and restoring it would add a no-op version).
      const bool isCurrent = i == 0 && !dirty && !state.historyUncommitted;
      ImGui::BeginDisabled(dirty || isCurrent);
      if (ImGui::SmallButton(("Restore" + rowId).c_str())) {
        restoreHash = e.hash;
        restoreLabel = e.subject;
      }
      ImGui::EndDisabled();
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (dirty)
          ImGui::SetTooltip("Save first — restoring would discard your "
                            "unsaved changes.");
        else if (isCurrent)
          ImGui::SetTooltip("This is the current state.");
        else
          ImGui::SetTooltip("Put the project back to this version. It is "
                            "added to the history, so nothing is lost.");
      }
    }
    ImGui::EndTable();
  }

  if (!restoreHash.empty()) {
    std::string error;
    if (RestoreVersion(state, restoreHash, restoreLabel, &error))
      actionError.clear();
    else
      actionError = error.empty() ? "could not restore that version" : error;
  }

  if (!actionError.empty()) {
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.45f, 1.0f), "%s",
                       actionError.c_str());
    ImGui::PopTextWrapPos();
  }

  ImGui::End();
}

} // namespace ui
