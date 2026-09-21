#ifdef HAVE_CAM
#include "ui/FloatingWindow.h"

#include "ui/CamGenerate.h"

#include "common/Logger.h"
#include "core/cam/CamPost.h"
#include "core/cam/CamToolpath.h"
#include "core/io/Serialization.h"
#include "ui/BackgroundJobs.h"
#include "ui/CamJobs.h"
#include "ui/CamPanel.h"
#include "ui/CamPlayback.h"
#include "ui/CamTool.h"
#include "ui/JobActions.h"
#include "ui/Theme.h" // ElevatedWindowBorder
#include "ui/Widgets.h"

#include "IconsFontAwesome6.h"
#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace ui {

void OpenCamGenerate(AppState& state, bool regenerate) {
  if (!state.job)
    return;
  state.camGenerateOpen = true;
  if (regenerate) {
    // A playback is a picture of a FILE, and this replaces the file: leaving
    // it running would scrub through G-code the job no longer posts.
    EndCamPlayback(state);
    RefreshCamToolpaths(state);
    RunCamVerification(state);
  }
}

void DrawCamGenerateWindow(AppState& state) {
  if (!state.camGenerateOpen)
    return;
  if (!state.job) {
    state.camGenerateOpen = false;
    return;
  }
  cam::Setup* setup = ActiveCamSetup(state);
  if (setup == nullptr) {
    state.camGenerateOpen = false;
    return;
  }
  // Where a result goes: the top right of the viewport, UNDER the tool window
  // when an op is being edited there (it used to open exactly on top of it),
  // and clear of Simulate's transport along the bottom (ui/FloatingWindow.h).
  static FloatingPlacement placement;
  const float wrap = ImGui::GetFontSize() * 26.0f;
  ImGui::SetNextWindowSizeConstraints(ImVec2(wrap * 0.6f, 0.0f),
                                      ImVec2(wrap + 24.0f, FLT_MAX));
  bool open = true;
  if (!BeginFloatingWindow(state, "Generate###CamGenerate", &open, placement,
                           FloatAnchor::BelowToolWindow,
                           ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::End();
    state.camGenerateOpen = open;
    return;
  }
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);

  bool optionsChanged = false;

  // Adaptive passes in flight: the row owns the progress, not a modal — the
  // rest of the job stays editable (docs/ui/long-operations.md, phase 1).
  if (const CamPassProgress progress = CamPassProgressOf(state);
      progress.running) {
    // The same widget the STEP import draws (ui/Widgets.h). It used to be a
    // greyed label and a small Cancel -- no bar at all -- so a surfacing
    // pass that takes minutes looked like nothing happening.
    JobProgress bar;
    bar.status = progress.label;
    bar.fraction = progress.total == 0
                       ? -1.0
                       : static_cast<double>(progress.done) /
                             static_cast<double>(progress.total);
    // Only when there IS a total. "1 / 0" is what this printed while the
    // counters were being reset under a running pass; that is fixed at the
    // source (CamJobs.cpp, LandedAPass), and a count nobody has is still not
    // printed -- the widget's "working" says it honestly.
    if (progress.total > 0) {
      char overlay[48];
      std::snprintf(overlay, sizeof(overlay), "%zu / %zu",
                    std::min(progress.done + 1, progress.total),
                    progress.total);
      bar.overlay = overlay;
    }
    bar.cancelId = "###cam_cancel_passes";
    bar.cancelTip = "Stops after the pass now cutting -- neither the clearing "
                    "library nor the drop cutter can be interrupted mid-pass. "
                    "What was waiting is queued again by the next edit or "
                    "Generate.";
    if (DrawJobProgress(bar))
      CancelCamPasses(state);
  }
  // The verification, when it is on a worker: the same widget again, because
  // it is the same kind of wait. Indeterminate -- the carve reports no
  // fraction -- and not cancellable, since it has no interruption points and
  // a newer request simply overtakes it.
  if (BackgroundJobRunning(state, "cam-verify")) {
    JobProgress bar;
    const BackgroundJob* job = FindBackgroundJob(state, "cam-verify");
    bar.status = job != nullptr ? job->label : "Verifying";
    bar.cancellable = false;
    DrawJobProgress(bar);
  }

  // What the verifier found, in words, without being asked. The numbers
  // behind these lines are in the same Verdict the RPC returns.
  for (const cam::VerdictLine& line : state.camVerdict) {
    const ImVec4 color = line.level == cam::VerdictLine::Level::Bad
                             ? ImVec4(0.95f, 0.45f, 0.40f, 1.0f)
                         : line.level == cam::VerdictLine::Level::Warn
                             ? ImVec4(0.95f, 0.80f, 0.40f, 1.0f)
                             : ImVec4(0.55f, 0.90f, 0.60f, 1.0f);
    // The icon font is merged into the UI atlas, so these are real glyphs;
    // a literal U+2713 would not be (the text font stops at Latin-1).
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextWrapped("%s %s",
                       line.level == cam::VerdictLine::Level::Good
                           ? ICON_FA_CIRCLE_CHECK
                           : ICON_FA_TRIANGLE_EXCLAMATION,
                       line.text.c_str());
    ImGui::PopStyleColor();
  }
  if (state.camVerdict.empty() && !state.camToolpaths.empty() &&
      !BackgroundJobRunning(state, "cam-verify"))
    ImGui::TextDisabled("Not verified since the last change -- Regenerate.");

  int failed = 0;
  double total = 0.0;
  for (const cam::Toolpath& tp : state.camToolpaths) {
    total += tp.CutLength();
    if (!tp.error.empty())
      failed++;
  }
  if (!state.camToolpaths.empty()) {
    ImGui::TextDisabled("%.0f mm of cutting", total);
    if (failed > 0) {
      ImGui::SameLine();
      ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.40f, 1.0f),
                         "· %d operation%s produced nothing", failed,
                         failed == 1 ? "" : "s");
    }
  }

  // --- the output ---------------------------------------------------------
  // In what order the job cuts, and whether it is one file or one per tool
  // (J5, R2). A file per tool is what most hobby controllers need, since
  // they cannot take a tool change mid-file.
  ImGui::SeparatorText("Output");
  {
    const char* orderLabel =
        setup->splitOutput                         ? "by tool (file per tool)"
        : setup->order == cam::OrderOutputBy::Tool ? "by tool"
        : setup->order == cam::OrderOutputBy::Part ? "by part"
                                                   : "as listed";
    ImGui::SetNextItemWidth(160.0f);
    ImGui::BeginDisabled(setup->splitOutput);
    if (ImGui::BeginCombo("Cut###cam_order", orderLabel)) {
      const std::pair<const char*, cam::OrderOutputBy> choices[] = {
          {"as listed", cam::OrderOutputBy::Authored},
          {"by tool", cam::OrderOutputBy::Tool},
          {"by part", cam::OrderOutputBy::Part}};
      for (const auto& [label, value] : choices)
        if (ImGui::Selectable(label, setup->order == value)) {
          setup->order = value;
          optionsChanged = true;
        }
      ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("The order the operations are cut in. By tool "
                          "groups the tool changes; by part finishes each "
                          "part before the next.");
    if (ImGui::Checkbox("One file per tool###cam_split", &setup->splitOutput))
      optionsChanged = true;
    ImGui::SetItemTooltip("Write T1's program, T2's program... each complete "
                          "and starting by loading its tool. Forces tool "
                          "order. For a controller that cannot take a tool "
                          "change mid-file.");
  }
  ImGui::Checkbox("Show path###cam_show", &state.camShowToolpath);
  ImGui::SameLine();
  ImGui::Checkbox("Stock###cam_gen_stock", &state.camShowStock);

  ImGui::Separator();
  if (ImGui::Button(ICON_FA_GEARS " Regenerate###cam_generate"))
    OpenCamGenerate(state, /*regenerate=*/true);
  ImGui::SetItemTooltip("Re-cut every operation and verify the file again");
  if (!state.camToolpaths.empty()) {
    ImGui::SameLine();
    if (ImGui::Button("Save G-code###cam_save_gcode")) {
      // BESIDE the project, which is what this always meant to say and did
      // not: `projectPath` is the .dsgn bundle itself, so appending to it
      // wrote the G-code INSIDE the project. `StampProject` walks every
      // regular file in there, so the job's own output changed the project's
      // disk stamp and the next save refused with "the project changed on
      // disk since you opened it" — the app fighting itself over a file it
      // had just written.
      //
      // With no project there is no "beside", and the fallback was a bare
      // "job.nc" written into whatever directory the app was launched from.
      // Refuse and say where it would go instead of leaving G-code somewhere
      // nobody will look for it (docs/cam/dogfood-round.md).
      if (const std::string why = CamToolpathsPendingWhy(state); !why.empty()) {
        // A file written now would carry the PREVIOUS toolpath where a pass
        // is still cutting.
        state.camLastPost = why;
        LOG_INFO("CAM: refused to post while passes are pending");
      } else if (state.projectPath.empty()) {
        state.camLastPost =
            "save the project first — the G-code goes beside it, named after "
            "the setup";
        LOG_INFO("CAM: refused to post an unsaved project");
      } else {
        const std::filesystem::path project(state.projectPath);
        std::filesystem::path dir = project.parent_path();
        if (dir.empty())
          dir = ".";
        // Named for the project AND the setup: several parts' jobs land in
        // one directory, and "Setup 1.nc" from two of them is one file.
        const std::string stem = project.stem().string() + " - " + setup->name;
        std::string error;
        if (setup->splitOutput) {
          // One file per tool, each named for it, all said.
          std::vector<cam::PostFile> files;
          bool ok = cam::PostProcessSplit(setup->machine.post, *setup,
                                          state.camToolpaths, PostOpts(*setup),
                                          files, &error);
          std::string wrote;
          for (const cam::PostFile& f : files) {
            if (!ok)
              break;
            const std::string path =
                (dir / (stem + "-T" + std::to_string(f.tool) + ".nc")).string();
            ok = DocumentIO::WriteTextAtomic(path, f.gcode, &error);
            if (ok) {
              wrote += (wrote.empty() ? "" : "\n") + path;
              LOG_INFO("CAM: wrote {}", path);
            }
          }
          state.camLastPost = ok ? wrote : "could not write: " + error;
        } else {
          const std::string path = (dir / (stem + ".nc")).string();
          std::string gcode;
          if (cam::PostProcess(setup->machine.post, *setup, state.camToolpaths,
                               PostOpts(*setup), gcode, &error) &&
              DocumentIO::WriteTextAtomic(path, gcode, &error)) {
            state.camLastPost = path;
            LOG_INFO("CAM: wrote {}", path);
          } else {
            state.camLastPost = "could not write: " + error;
          }
        }
      }
    }
  }
  if (!state.camLastPost.empty())
    ImGui::TextDisabled("%s", state.camLastPost.c_str());

  ImGui::PopTextWrapPos();
  ImGui::End();
  if (!open)
    state.camGenerateOpen = false;
  if (optionsChanged) {
    // The order and the split change the FILE, not a toolpath -- but the
    // verdict is about the file, so it is re-run rather than left describing
    // the previous one.
    state.projectMetaDirty = true;
    RefreshCamToolpaths(state);
    state.camVerdict.clear();
    state.camLeftover.clear();
    RunCamVerification(state);
  }
}

} // namespace ui

#else // !HAVE_CAM

#include "ui/CamGenerate.h"

namespace ui {
void OpenCamGenerate(AppState&, bool) {}
void DrawCamGenerateWindow(AppState&) {}
} // namespace ui

#endif // HAVE_CAM
