#include "ui/ConflictsPanel.h"
#include "ui/DockLayout.h" // PanelHeading
#include "ui/PanelTitles.h"

#include "common/Logger.h"
#include "core/io/GitVault.h"
#include "core/io/ProjectMerge.h"
#include "ui/ProjectActions.h"

#include "imgui.h"

#include <string>

namespace ui {

namespace {

// The scan itself. No ImGui: FinishMerge runs it, and FinishMerge is
// reachable over RPC in a headless process where there is no ImGui context
// at all — `ImGui::GetTime()` on that path was a SIGSEGV.
void ScanNow(AppState& state) {
  state.conflictsPath = state.projectPath;
  state.conflictsStale = false;
  state.conflictScans++;
  if (state.projectPath.empty() || !GitVault::Available()) {
    state.conflicts = ProjectMerge::Report{};
    return;
  }
  state.conflicts = ProjectMerge::Scan(state.projectPath);
}

// The draw-path wrapper. No frame time involved any more: a scan happens when
// something could have changed the answer, not on a clock.
void RefreshConflicts(AppState& state, bool force) {
  if (!force && !state.conflictsStale &&
      state.conflictsPath == state.projectPath)
    return;
  ScanNow(state);
}

// What the button on a row should do, in one word. Every class resolves
// differently — this is the whole point of the panel over `git status`.
const char* ActionVerb(const ProjectMerge::Conflict& conflict) {
  switch (conflict.kind) {
  case ProjectMerge::ConflictKind::Order:
    return "Merge both";
  case ProjectMerge::ConflictKind::Derived:
    return "Rebuild";
  case ProjectMerge::ConflictKind::DuplicateId:
    return "Keep both";
  default:
    return "";
  }
}

// A heading per document, so a 48-part project reads as parts rather than as
// one flat list of paths.
std::string GroupLabel(const ProjectMerge::Conflict& conflict) {
  if (conflict.document.empty())
    return "The project";
  if (conflict.documentKind.empty())
    return conflict.document;
  std::string kind = conflict.documentKind;
  kind[0] = static_cast<char>(std::toupper(kind[0]));
  return kind + " " + conflict.document;
}

} // namespace

void InvalidateConflicts(AppState& state) { state.conflictsStale = true; }

bool ProjectHasConflicts(AppState& state) { return !state.conflicts.Empty(); }

bool FinishMerge(AppState& state, std::string* error) {
  state.conflictsBreakage.clear();

  // Sweep the conflicts that have a right answer. They are listed in the
  // panel so nothing is silent, but nobody should have to click "Merge both"
  // once per index: a feature ORDER has a correct union, and baked geometry
  // is rebuilt from the recipe either way. Without this the merge could not
  // be concluded at all — a project with baked geometry conflicts on
  // `geometry/index.json` every time, and CommitMerge rightly refuses while
  // any path is unmerged.
  ScanNow(state);
  for (const ProjectMerge::Conflict& c : state.conflicts.conflicts) {
    if (!c.autoResolvable)
      continue;
    std::string autoError;
    if (!ProjectMerge::AutoResolve(state.projectPath, c, &autoError))
      LOG_WARN("Could not auto-resolve {}: {}", c.path, autoError);
  }
  ScanNow(state);
  if (state.conflicts.NeedsDecision() > 0) {
    if (error)
      *error = "some conflicts still need a decision";
    return false;
  }

  // The bar. A resolution can leave every file valid JSON and still leave an
  // extrude pointing at a sketch that no longer exists, so the question is
  // not "did git accept it" but "does the project still work".
  state.conflictsBreakage = ProjectMerge::Verify(state.projectPath);
  if (!state.conflictsBreakage.empty()) {
    if (error)
      *error = "the merged project does not open yet";
    return false;
  }
  std::string commitError;
  if (GitVault::InMerge(state.projectPath)) {
    if (!GitVault::CommitMerge(state.projectPath, "Merge", &commitError)) {
      if (error)
        *error = commitError;
      return false;
    }
  } else if (!GitVault::CommitAll(state.projectPath, "Resolve duplicate ids",
                                  &commitError)) {
    if (error)
      *error = commitError;
    return false;
  }
  InvalidateConflicts(state);
  state.historyStamp = -1.0;
  // Reopen: the documents in memory are the pre-merge ones, and the next
  // save would write them straight back over the merge.
  return OpenProject(state, state.projectPath, error);
}

bool AbandonMerge(AppState& state, std::string* error) {
  if (!GitVault::AbortMerge(state.projectPath, error))
    return false;
  InvalidateConflicts(state);
  state.historyStamp = -1.0;
  state.conflictsBreakage.clear();
  return OpenProject(state, state.projectPath, error);
}

void DrawConflictsPanel(AppState& state) {
  RefreshConflicts(state, false);
  // The one panel whose absence is the good news: no window at all when the
  // project is in one piece.
  if (state.conflicts.Empty())
    return;

  const bool visible = ImGui::Begin(kPanelConflicts, nullptr,
                                    ImGuiWindowFlags_NoFocusOnAppearing);
  if (!visible) {
    ImGui::End();
    return;
  }
  PanelHeading(kPanelConflicts);

  static std::string actionError;
  const ImGuiStyle& style = ImGui::GetStyle();

  const int decisions = state.conflicts.NeedsDecision();
  if (state.conflicts.inMerge) {
    ImGui::TextWrapped(
        "Two versions of this project were changed separately. Everything "
        "they both left alone is already merged; below is what they "
        "disagree about.");
  } else {
    ImGui::TextWrapped(
        "This project has two things claiming the same identity. It was "
        "merged without complaint, which is exactly why it needs looking "
        "at.");
  }
  ImGui::Spacing();

  // The action runs after the loop: resolving rescans the very list being
  // walked (the History panel's restore does the same). Indices, NOT
  // pointers — "Finish" rescans mid-frame and would reallocate the vector
  // under a pointer taken from a row above it.
  enum class Pending { None, Mine, Theirs, Auto, Field };
  Pending pending = Pending::None;
  int pendingRow = -1;
  std::string fieldName;
  GitVault::MergeSide fieldSide = GitVault::MergeSide::Ours;

  std::string group;
  for (int i = 0; i < static_cast<int>(state.conflicts.conflicts.size()); i++) {
    const ProjectMerge::Conflict& c = state.conflicts.conflicts[i];
    const std::string label = GroupLabel(c);
    if (label != group) {
      group = label;
      ImGui::SeparatorText(group.c_str());
    }
    const std::string rowId = std::to_string(i);

    // Click the row to go and look at the thing being argued about: its
    // document is opened and it is selected, so the feature tree highlights
    // it. Same gesture as the diff view, and the same helper.
    const bool showable = c.id != 0 && !c.oursDeleted;
    if (!showable)
      ImGui::BeginDisabled();
    if (ImGui::Selectable((c.summary + "###cf_row" + rowId).c_str(), false,
                          ImGuiSelectableFlags_AllowDoubleClick)) {
      RevealItem(state, c.document, c.documentKind, c.id,
                 ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left));
    }
    if (!showable)
      ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && showable)
      ImGui::SetTooltip("Click to select it, double-click to frame it.");

    // A conflict with a right answer gets one button, not a choice: asking
    // someone to pick a side of a feature ORDER is asking them to lose work.
    if (c.autoResolvable || c.kind == ProjectMerge::ConflictKind::DuplicateId) {
      if (ImGui::SmallButton(
              (std::string(ActionVerb(c)) + "###cf_auto" + rowId).c_str())) {
        pending = Pending::Auto;
        pendingRow = i;
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            c.kind == ProjectMerge::ConflictKind::DuplicateId
                ? "Give one of them a fresh id. Both stay."
                : (c.kind == ProjectMerge::ConflictKind::Order
                       ? "Keep everything both sides added, in order."
                       : "Either side does — it is rebuilt from the recipe."));
      }
    } else {
      // Field-level first: two people who both edited an extrude and disagree
      // only about `distance` should not have to throw away the other's
      // rename.
      for (int f = 0; f < static_cast<int>(c.fields.size()); f++) {
        const ProjectMerge::FieldDiff& diff = c.fields[f];
        const std::string fieldId = rowId + "_" + std::to_string(f);
        ImGui::TextDisabled("    %s", diff.field.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton(
                (diff.ours + "###cf_f_mine" + fieldId).c_str())) {
          pending = Pending::Field;
          pendingRow = i;
          fieldName = diff.field;
          fieldSide = GitVault::MergeSide::Ours;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("or");
        ImGui::SameLine();
        if (ImGui::SmallButton(
                (diff.theirs + "###cf_f_theirs" + fieldId).c_str())) {
          pending = Pending::Field;
          pendingRow = i;
          fieldName = diff.field;
          fieldSide = GitVault::MergeSide::Theirs;
        }
      }
      if (ImGui::SmallButton(("Keep mine###cf_mine" + rowId).c_str())) {
        pending = Pending::Mine;
        pendingRow = i;
      }
      ImGui::SameLine();
      if (ImGui::SmallButton(("Keep theirs###cf_theirs" + rowId).c_str())) {
        pending = Pending::Theirs;
        pendingRow = i;
      }
    }
    ImGui::Spacing();
  }

  ImGui::Separator();

  // Verification failures: the merge is NOT concluded, and this says what
  // broke rather than leaving a project that opens to an error next time.
  if (!state.conflictsBreakage.empty()) {
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.45f, 1.0f),
                       "Not finished — the project does not open yet:");
    for (const ProjectMerge::Breakage& b : state.conflictsBreakage) {
      ImGui::Bullet();
      ImGui::TextWrapped("%s %s: %s", b.documentKind.c_str(),
                         b.document.c_str(), b.reason.c_str());
    }
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
  }

  ImGui::BeginDisabled(decisions > 0);
  if (ImGui::Button("Finish###cf_finish")) {
    std::string error;
    if (FinishMerge(state, &error))
      actionError.clear();
    else
      actionError = error;
  }
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip(decisions > 0
                          ? "Decide the ones above first."
                          : "Check the project still opens, then keep it.");
  }

  if (state.conflicts.inMerge) {
    ImGui::SameLine();
    if (ImGui::Button("Throw the merge away###cf_abort")) {
      std::string error;
      if (AbandonMerge(state, &error))
        actionError.clear();
      else
        actionError = error;
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Put the project back to before the merge. Nothing "
                        "you had saved is lost.");
  }

  if (!actionError.empty()) {
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.45f, 1.0f), "%s",
                       actionError.c_str());
    ImGui::PopTextWrapPos();
  }
  (void)style;

  ImGui::End();

  // Resolutions, after the walk. Re-fetched by index: Finish may have
  // rescanned since the row was clicked.
  if (pending == Pending::None || pendingRow < 0 ||
      pendingRow >= static_cast<int>(state.conflicts.conflicts.size()))
    return;
  const ProjectMerge::Conflict& c = state.conflicts.conflicts[pendingRow];
  std::string error;
  bool ok = false;
  switch (pending) {
  case Pending::Mine:
    ok = ProjectMerge::TakeSide(state.projectPath, c, GitVault::MergeSide::Ours,
                                &error);
    break;
  case Pending::Theirs:
    ok = ProjectMerge::TakeSide(state.projectPath, c,
                                GitVault::MergeSide::Theirs, &error);
    break;
  case Pending::Field:
    ok = ProjectMerge::TakeField(state.projectPath, c, fieldName, fieldSide,
                                 &error);
    break;
  case Pending::Auto:
    ok = c.kind == ProjectMerge::ConflictKind::DuplicateId
             ? ProjectMerge::RenumberDuplicate(state.projectPath, c, &error)
             : ProjectMerge::AutoResolve(state.projectPath, c, &error);
    break;
  case Pending::None:
    break;
  }
  actionError = ok ? "" : (error.empty() ? "could not resolve that" : error);
  InvalidateConflicts(state);
  state.conflictsBreakage.clear();
}

} // namespace ui
