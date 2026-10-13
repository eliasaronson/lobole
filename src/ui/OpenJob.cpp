// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/OpenJob.h"

#include "common/Logger.h"
#include "ui/AppState.h"
#include "ui/BackgroundJobs.h"
#include "ui/ImportJob.h" // ImportRunning
#include "ui/Widgets.h"   // DrawJobProgress

#include "imgui.h"

#include "cad/CADKernel.h" // ArmOccSignalHandlers

#include <chrono>
#include <filesystem>
#include <memory>
#include <thread>
#include <utility>

namespace ui {

namespace {

constexpr const char* kKind = "open";
constexpr const char* kModal = "Opening project###open_project_modal";

// The job's finish, on the main thread after the join.
bool Finish(AppState& state, const std::shared_ptr<OpenJob>& job,
            const BackgroundJob& bg) {
  state.openJob = nullptr;
  if (bg.cancel.load()) {
    LOG_INFO("Opening '{}' cancelled; the open project is unchanged",
             job->path);
    return true;
  }
  if (!job->result) {
    state.openError =
        "Could not open " + job->path + ": " + job->result.error();
    LOG_ERROR("{}", state.openError);
    return true;
  }
  std::string error;
  if (!AdoptProject(state, std::move(*job->result), &error)) {
    state.openError = "Could not open " + job->path + ": " + error;
    LOG_ERROR("{}", state.openError);
    return true;
  }
  MarkProjectOpened(state, job->path);
  return true;
}

} // namespace

bool OpenRunning(const AppState& state) {
  return BackgroundJobRunning(state, kKind);
}

bool StartOpenProject(AppState& state, const std::string& path,
                      std::string* error, const OpenJobOptions& options) {
  if (OpenRunning(state) || ImportRunning(state)) {
    if (error)
      *error = OpenRunning(state) ? "a project is already being opened"
                                  : "an import is running";
    return false;
  }
  state.openError.clear();
  auto job = std::make_shared<OpenJob>();
  job->path = path;
  job->name = std::filesystem::path(path).filename().string();
  if (job->name.empty())
    job->name = path;
  auto bg = std::make_unique<BackgroundJob>();
  bg->kind = kKind;
  bg->label = "Opening " + job->name;
  bg->cancellable = true;
  BackgroundJob* raw = bg.get();
  job->progress.cancel = &raw->cancel;
  job->progress.delayEachMs = options.delayEachMs;
  bg->thread = std::thread([job, raw] {
    // OCC's signal handler is per thread: without it a fault in a rebuild is
    // a raw SIGSEGV instead of the Standard_Failure the loader catches.
    ArmOccSignalHandlers();
    job->result = ReadProject(job->path, &job->progress);
    raw->done.store(true); // last: the result is visible before this
  });
  bg->finish = [job, raw](AppState& s) { return Finish(s, job, *raw); };
  state.openJob = job;
  StartBackgroundJob(state, std::move(bg));

  // A quick open is done before the modal would appear. A baked open is tens
  // of milliseconds and should look as it always did -- instant, no modal
  // flashing past -- while anything longer is worth a bar. The loop's poll
  // adopts it at the end of this frame (the reap is never called from here:
  // test_conventions.cpp keeps joins out of draw paths).
  const auto until = std::chrono::steady_clock::now() +
                     std::chrono::milliseconds(options.quietWaitMs);
  while (!BackgroundJobDone(*raw) && std::chrono::steady_clock::now() < until)
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  return true;
}

std::string OpenStatusLine(const OpenJob& job) {
  const int total = job.progress.total.load();
  if (total <= 1)
    return "Loading " + job.name;
  return "Loading " + job.name +
         ": parts whose saved geometry cannot be "
         "used are rebuilt";
}

void DrawOpenProjectModal(AppState& state) {
  // A job that has finished but not yet been reaped is adopted by the poll at
  // the end of this frame; opening the modal for it would flash it.
  const BackgroundJob* pending = FindBackgroundJob(state, kKind);
  const bool running = pending != nullptr && !BackgroundJobDone(*pending) &&
                       state.openJob != nullptr;
  if (!running && state.openError.empty()) {
    // Close it, rather than only stop submitting it: an unsubmitted modal
    // stays on ImGui's popup stack, where IsPopupOpen still finds it.
    if (ImGui::IsPopupOpen(kModal) &&
        ImGui::BeginPopupModal(kModal, nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    }
    return;
  }
  // Asked every frame, as the import modal does: IsPopupOpen is the actual
  // state, so a reset around it cannot leave the job running with no modal.
  if (!ImGui::IsPopupOpen(kModal))
    ImGui::OpenPopup(kModal);
  ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 26.0f, 0),
                           ImGuiCond_Appearing);
  if (!ImGui::BeginPopupModal(kModal, nullptr,
                              ImGuiWindowFlags_AlwaysAutoResize))
    return;

  if (!running) {
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
    ImGui::TextUnformatted(state.openError.c_str());
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    if (ImGui::Button("Close###open_error_close") ||
        ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      state.openError.clear();
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
    return;
  }

  OpenJob& job = *state.openJob;
  BackgroundJob* bg = FindBackgroundJob(state, kKind);
  const int done = job.progress.done.load();
  const int total = job.progress.total.load();
  const double fraction =
      total > 1 ? static_cast<double>(done) / static_cast<double>(total) : -1.0;
  if (bg)
    bg->fraction.store(fraction);

  JobProgress bar;
  bar.status = OpenStatusLine(job);
  bar.fraction = fraction;
  if (total > 1)
    bar.overlay =
        std::to_string(done) + " / " + std::to_string(total) + " parts";
  bar.cancelAsked = bg && bg->cancel.load();
  bar.cancelId = "###open_cancel";
  bar.cancelTip = "Stop opening. The project that is open now stays open.";
  if (DrawJobProgress(bar) && bg)
    bg->cancel.store(true);

  // The parts in flight: one wheel can be a third of the wait, and a name
  // says the bar is moving even while it is not.
  std::string building;
  {
    std::lock_guard lock(job.progress.mutex);
    for (const std::string& name : job.progress.building)
      building += (building.empty() ? "" : ", ") + name;
  }
  if (!building.empty())
    ImGui::TextDisabled("%s", building.c_str());
  ImGui::EndPopup();
}

} // namespace ui
