#pragma once

#include "ui/ProjectActions.h" // LoadedProject, OpenProgress

#include <expected>
#include <string>

namespace ui {

struct AppState;

// Opening a project off the frame (docs/ui/project-open-progress-plan.md).
//
// A project whose baked geometry cannot be used -- written by an older build,
// git-ignored, or out of date with its recipe -- is rebuilt part by part, and
// that froze the window with nothing on screen: 20 s on the bike. The read
// (ReadProject) runs on the registry's "open" job while a modal shows the
// shared progress bar; the adopt runs in the job's finish, on the main thread.
// Cancel drops the half-read project, and the one that was open stays open,
// because nothing in AppState is touched until the adopt.
//
// The opens a PERSON starts come here: File > Open and Open Recent. Every
// caller that needs the answer before it returns (the RPC, restore, merge,
// sync, tests) keeps the synchronous OpenProject.
struct OpenJob {
  std::string path;
  std::string name; // the project's folder name, for the modal's title
  OpenProgress progress;
  // Written by the worker before `done`, read by the finish after the join.
  std::expected<LoadedProject, std::string> result =
      std::unexpected(std::string("not started"));
};

struct OpenJobOptions {
  int quietWaitMs = 300; // how long to wait before putting the modal up
  int delayEachMs = 0;   // OpenProgress::delayEachMs -- tests only
};

// Starts opening `path`. A project that loads quickly (a baked open is tens of
// milliseconds) has been read before this returns and is adopted by the
// loop's poll at the end of the frame, so no modal flashes; a slow one
// returns with the job running and the modal up. Refused, with the reason
// in `error`, while another open or an import is running.
bool StartOpenProject(AppState& state, const std::string& path,
                      std::string* error = nullptr,
                      const OpenJobOptions& options = {});

// Is an open in flight?
[[nodiscard]] bool OpenRunning(const AppState& state);

// The modal: the progress bar while an open runs, and the error of one that
// failed until it is closed. Drawn every frame at the host-window level.
void DrawOpenProjectModal(AppState& state);

// The status line the modal and the background_jobs listing show.
[[nodiscard]] std::string OpenStatusLine(const OpenJob& job);

} // namespace ui
