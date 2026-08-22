#include "ui/HistoryPanel.h"

#include "common/Logger.h"
#include "core/io/GitVault.h"
#include "core/io/ProjectStamp.h"
#include "ui/AppActions.h"
#include "ui/ConflictsPanel.h"
#include "ui/FileDialogs.h"
#include "ui/SyncJob.h"

#include "imgui.h"

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

// How often the project directory is re-examined. Slow on purpose: the walk
// is cheap per file but a 48-part project is thousands of them, and the
// question "did somebody else write to this" does not need a fast answer —
// it needs a certain one before the next save.
constexpr double kDiskPollSeconds = 3.0;

} // namespace

// Has the project moved under us? (F8) Two cheap gates before the walk:
// focus coming back (the terminal-and-back workflow), and — for a git
// project — whether HEAD moved, which every checkout/merge/pull does and
// which costs one `git` call instead of a directory walk.
//
// Neither gate is sufficient alone, which is why both are here: HEAD does not
// move for a working-tree edit, and focus does not change when a headless
// agent writes while you keep working.
void PollDiskState(AppState& state) {
  if (state.projectPath.empty() || state.diskChanged)
    return; // already known — the banner is up
  const double now = ImGui::GetTime();
  static bool wasFocused = true;
  const bool focusReturned = state.windowFocused && !wasFocused;
  wasFocused = state.windowFocused;
  if (!focusReturned && state.diskCheckStamp >= 0.0 &&
      now - state.diskCheckStamp < kDiskPollSeconds)
    return;
  state.diskCheckStamp = now;

  if (!state.diskStampHead.empty()) {
    const auto head = GitVault::Log(state.projectPath, 1);
    if (!head.empty() && head[0].hash != state.diskStampHead) {
      if (!state.diskChanged)
        state.conflictsStale = true;
      state.diskChanged = true;
      return;
    }
  }
  const bool wasChanged = state.diskChanged;
  state.diskChanged = ProjectChangedOnDisk(state);
  // Something else wrote the project, which is the only way a conflict can
  // appear without us doing it — so this is where the conflicts scan earns
  // its keep, rather than on a timer.
  if (state.diskChanged && !wasChanged)
    state.conflictsStale = true;
}

namespace {

// Remotes and the ahead/behind counts, cached: each is a `git` process, and
// the panel draws them as labels.
void RefreshRemotes(AppState& state, bool force) {
  const double now = ImGui::GetTime();
  if (!force && state.remotesStamp >= 0.0 &&
      now - state.remotesStamp < kRefreshSeconds)
    return;
  state.remotesStamp = now;
  state.remotes = GitVault::Remotes(state.projectPath);
  state.divergence = GitVault::AheadBehind(state.projectPath);
}

void RefreshBranches(AppState& state, bool force) {
  if (!force && !state.branches.empty() &&
      state.historyPath == state.projectPath)
    return;
  state.branches = GitVault::Branches(state.projectPath);
  state.currentBranch = GitVault::CurrentBranch(state.projectPath);
}

} // namespace

bool SwitchToBranch(AppState& state, const std::string& branch,
                    std::string* error) {
  if (!GitVault::SwitchBranch(state.projectPath, branch, error))
    return false;
  state.historyStamp = -1.0;
  InvalidateConflicts(state);
  // The working tree is a different project now; the documents in memory are
  // the old one, and the next save would write them straight back over it.
  return OpenProject(state, state.projectPath, error);
}

bool MergeBranch(AppState& state, const std::string& branch,
                 std::string* error) {
  std::string mergeError;
  const GitVault::MergeStatus status =
      GitVault::Merge(state.projectPath, branch, &mergeError);
  state.historyStamp = -1.0;
  InvalidateConflicts(state);
  if (status == GitVault::MergeStatus::Failed) {
    if (error)
      *error = mergeError;
    return false;
  }
  // A conflicted merge is not a failure — it is the state the Conflicts
  // panel exists for, and it appears on its own as soon as the scan runs.
  if (status == GitVault::MergeStatus::Conflicted) {
    LOG_INFO("Merge of {} left conflicts to resolve", branch);
    return true;
  }
  if (status == GitVault::MergeStatus::UpToDate)
    return true;
  return OpenProject(state, state.projectPath, error);
}

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
  // The panel is reached by clicking its tab, like Variables and
  // Configurations — there is no menu entry to focus it from.
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
  // F8: the project moved under us. This sits above the save status because
  // it changes what saving means — and it stays until the project and the
  // disk agree again.
  //
  // When the change left CONFLICTS, F7's panel owns the message: it appears
  // on its own for exactly that state, and two panels shouting about one
  // event is worse than one.
  if (state.diskChanged && state.conflicts.Empty()) {
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1.0f),
                       "This project changed on disk — something else wrote "
                       "to it.");
    ImGui::PopTextWrapPos();
    if (ImGui::SmallButton("Reload###hist_disk_reload")) {
      std::string reloadError;
      if (OpenProject(state, state.projectPath, &reloadError))
        actionError.clear();
      else
        actionError = reloadError;
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Open what is on disk. Unsaved changes here are lost.");
    ImGui::Separator();
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
  const ImGuiStyle& style = ImGui::GetStyle();
  // Status line + its action. The button goes beside the text when there is
  // room for it and on its own line when there is not — a panel dragged
  // narrow (or a large font) must not push the button off the edge.
  const auto statusButton = [&style](const char* label,
                                     const std::string& id) -> bool {
    const float width =
        ImGui::CalcTextSize(label).x + style.FramePadding.x * 2.0f;
    const float used = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x +
                       ImGui::GetScrollX();
    if (ImGui::GetContentRegionMax().x - used - style.ItemSpacing.x >= width)
      ImGui::SameLine();
    return ImGui::SmallButton((std::string(label) + id).c_str());
  };

  const bool dirty = ProjectDirty(state);
  if (dirty) {
    ImGui::TextUnformatted("Unsaved changes");
    if (statusButton("Save", "###hist_save")) {
      SaveProjectAction(state);
      state.historyStamp = -1.0;
      actionError.clear();
    }
  } else if (state.historyUncommitted) {
    ImGui::TextUnformatted("Saved, not yet a version");
    if (statusButton("Add to history", "###hist_commit")) {
      MarkProjectSaved(state, state.projectPath, "save");
      state.historyStamp = -1.0;
    }
  } else {
    ImGui::TextDisabled("Up to date");
  }

  ImGui::Separator();

  // Name the current state. Saves first — a name refers to a state on disk.
  // The room the button needs is MEASURED, never a pixel literal: the style
  // scales with the display's content scale, so a hard-coded 70 reserved
  // half a button on a HiDPI screen and pushed the row past the panel edge.
  const float nameButtonWidth =
      ImGui::CalcTextSize("Name").x + style.FramePadding.x * 2.0f;
  ImGui::SetNextItemWidth(-(nameButtonWidth + style.ItemSpacing.x));
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

  // No ScrollY: that wraps the rows in a scrolling child, and a child whose
  // columns do not fit is what let the panel scroll SIDEWAYS. The window
  // scrolls vertically on its own, and the two flexible columns clip their
  // text instead of widening the content.
  if (ImGui::BeginTable("##versions", 3,
                        ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_SizingStretchProp)) {
    // Measured, not literal — see the Name button above.
    const float restoreWidth = ImGui::CalcTextSize("Restore").x +
                               style.FramePadding.x * 2.0f +
                               style.CellPadding.x * 2.0f;
    ImGui::TableSetupColumn("Version", ImGuiTableColumnFlags_WidthStretch,
                            1.6f);
    ImGui::TableSetupColumn("When", ImGuiTableColumnFlags_WidthStretch, 0.9f);
    ImGui::TableSetupColumn("##restore", ImGuiTableColumnFlags_WidthFixed,
                            restoreWidth);
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

  // Branches (F7). Collapsed by default: the linear history above is what
  // this panel is for, and most projects never need a second line of work.
  // But without a way to make one, the app could not produce a merge at all
  // — which is why the conflict UI F5 and F6 both deferred had no state to
  // serve for four weeks.
  ImGui::Separator();
  if (ImGui::TreeNode("Branches")) {
    RefreshBranches(state, false);
    // Wrapped, not a fixed line: a branch name is user prose and the panel
    // can be dragged narrow.
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("On branch: %s", state.currentBranch.empty()
                                             ? "(none)"
                                             : state.currentBranch.c_str());
    ImGui::PopTextWrapPos();
    static char branchBuf[96] = {};
    const float startWidth =
        ImGui::CalcTextSize("Start").x + style.FramePadding.x * 2.0f;
    ImGui::SetNextItemWidth(-(startWidth + style.ItemSpacing.x));
    const bool named = ImGui::InputTextWithHint(
        "###hist_branch", "New branch...", branchBuf, sizeof(branchBuf),
        ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    ImGui::BeginDisabled(branchBuf[0] == '\0' || dirty);
    const bool start = ImGui::Button("Create###hist_branch_go");
    ImGui::EndDisabled();
    if ((named || start) && branchBuf[0] != '\0') {
      std::string error;
      if (GitVault::CreateBranch(state.projectPath, branchBuf, true, &error)) {
        branchBuf[0] = '\0';
        actionError.clear();
        state.historyStamp = -1.0;
        RefreshBranches(state, true);
      } else {
        actionError = error;
      }
    }

    for (int i = 0; i < static_cast<int>(state.branches.size()); i++) {
      const std::string& branch = state.branches[i];
      if (branch == state.currentBranch)
        continue;
      const std::string rowId = std::to_string(i);
      ImGui::TextUnformatted(branch.c_str());
      ImGui::BeginDisabled(dirty);
      if (statusButton("Checkout", ("###hist_switch" + rowId).c_str())) {
        std::string error;
        if (SwitchToBranch(state, branch, &error)) {
          actionError.clear();
          RefreshBranches(state, true);
        } else {
          actionError = error;
        }
      }
      if (statusButton("Merge", ("###hist_merge" + rowId).c_str())) {
        std::string error;
        if (!MergeBranch(state, branch, &error))
          actionError = error;
        else
          actionError.clear();
      }
      ImGui::EndDisabled();
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && dirty)
        ImGui::SetTooltip("Save first.");
    }
    ImGui::TreePop();
  }

  // Sharing (F9 C2). This section names git's operations — remote, fetch,
  // push — against the rest of the panel's rule of speaking CAD. Decided
  // deliberately (see f9-collaboration.md): a rejected push is a real thing
  // with a git cause, and inventing a word for it would teach the wrong one
  // the moment git's own message appears.
  ImGui::Separator();
  if (ImGui::TreeNode("Sharing")) {
    RefreshRemotes(state, false);
    PollSync(state);

    if (state.remotes.empty()) {
      ImGui::TextWrapped("This project has no remote. Add one to share it "
                         "with other people.");
      static char urlBuf[512] = {};
      const float addWidth =
          ImGui::CalcTextSize("Add").x + style.FramePadding.x * 2.0f;
      ImGui::SetNextItemWidth(-(addWidth + style.ItemSpacing.x));
      const bool entered = ImGui::InputTextWithHint(
          "###hist_remote_url", "Remote URL...", urlBuf, sizeof(urlBuf),
          ImGuiInputTextFlags_EnterReturnsTrue);
      ImGui::SameLine();
      ImGui::BeginDisabled(urlBuf[0] == '\0');
      const bool add = ImGui::Button("Add###hist_remote_add");
      ImGui::EndDisabled();
      if ((entered || add) && urlBuf[0] != '\0') {
        std::string error;
        if (AddProjectRemote(state, "origin", urlBuf, &error)) {
          urlBuf[0] = '\0';
          actionError.clear();
        } else {
          actionError = error;
        }
      }
    } else {
      const GitVault::Remote& remote = state.remotes[0];
      ImGui::TextDisabled("%s", remote.name.c_str());
      ImGui::PushTextWrapPos(0.0f);
      ImGui::TextDisabled("%s", remote.url.c_str());
      ImGui::PopTextWrapPos();

      // What is waiting in each direction. Cached: asking git costs a
      // process, and this is a label.
      const GitVault::Divergence& d = state.divergence;
      if (!d.hasUpstream)
        ImGui::TextDisabled("Not pushed yet");
      else if (d.ahead == 0 && d.behind == 0)
        ImGui::TextDisabled("Up to date");
      else
        ImGui::TextDisabled("%d ahead, %d behind", d.ahead, d.behind);

      const bool busy = SyncRunning(state);
      ImGui::BeginDisabled(busy || dirty);
      if (ImGui::SmallButton("Fetch###hist_fetch"))
        StartFetch(state, remote.name, false);
      ImGui::SameLine();
      if (ImGui::SmallButton("Pull###hist_pull"))
        StartFetch(state, remote.name, true);
      ImGui::SameLine();
      if (ImGui::SmallButton("Push###hist_push"))
        StartPush(state, remote.name);
      ImGui::EndDisabled();
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && dirty)
        ImGui::SetTooltip("Save first.");

      if (busy) {
        ImGui::TextDisabled("Working...");
      } else if (!state.syncMessage.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        if (state.syncFailed) {
          ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.45f, 1.0f), "%s",
                             state.syncMessage.c_str());
          ImGui::TextDisabled(
              "Designer does not ask for passwords. If this is about "
              "credentials, set them up the way you normally would — the "
              "terminal panel opens in this project.");
        } else {
          ImGui::TextDisabled("%s", state.syncMessage.c_str());
        }
        ImGui::PopTextWrapPos();
      }
    }
    ImGui::TreePop();
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
