// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "app/HeadlessMain.h"
#include "app/StopSignal.h"

#include "ui/BackgroundJobs.h"

#include "common/Logger.h"
#include "common/Watchdog.h"
#include "core/Command.h"
#include "core/Document.h"
#include "core/io/Autosaver.h"
#include "core/io/CrashReports.h"
#include "remote/InstanceRegistry.h"
#include "remote/RemoteServer.h"
#include "render/Camera.h"
#include "ui/AppState.h"
#include "ui/CamJobs.h" // OpenCamSurfaceStore
#include "ui/DocumentSlots.h"
#include "ui/ImportActions.h"
#include "ui/ImportJob.h"
#include "ui/MachineActions.h"
#include "ui/PrintProfileActions.h"
#include "ui/ProjectActions.h"
#include "ui/RendererSync.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <thread>

int RunHeadless(int remotePort, const std::string& fileToLoad,
                double autosaveSeconds, const std::string& recordPath,
                bool bindAll, bool allowLargeMesh) {
  LOG_INFO("Headless mode: document engine + remote server, no GUI");

  // Same defaults as the GUI so view/fit_view/get_state.camera behave
  // identically; a GUI session opening the same project sees the same framing.
  Camera camera;
  camera.SetPosition(Camera::DefaultIsometricPosition());
  camera.SetTarget(glm::vec3(0.0f, 0.0f, 0.0f));
  camera.SetAspectRatio(1280.0f / 720.0f);

  ui::AppState state;
  state.document = std::make_unique<Document>();
  state.commands = std::make_unique<CommandStack>();
  state.camera = &camera;
  // No frame to keep free: a drawing's views project where they are asked
  // for, so an RPC that reads them after a tab switch or an open finds them
  // (docs/drawings/README.md#requests-that-wait).
  state.drawingDeferProjection = false;
  ui::EnsureParts(state); // one part to start (assemblies A1)
  // The machine store. main() loads it into the GUI's AppState, and this mode
  // builds its OWN -- so a headless session had no machines at all: an agent
  // could not name one, `list_machines` came back empty over a directory with
  // files in it, and every job reported `machine_match: missing` against a
  // machine plainly on disk. Which meant no travel limits and no rapid-feed
  // ceiling on exactly the surface an agent drives
  // (docs/cam/setup.md#machine-profiles).
  ui::ReloadMachines(state);
  // ...and the print profiles, for the same reason: a printed part names
  // one, and a session without the store reports every one `missing`.
  ui::ReloadPrintProfiles(state);
#ifdef HAVE_CAM
  // ...and the surfacing passes kept between runs, as main() opens them
  // (docs/ui/long-operations.md#passes-kept-on-disk).
  ui::OpenCamSurfaceStore(state);
#endif
  // All renderer/selector pointers stay null: SyncDocumentRenderers no-ops,
  // and RemoteServer degrades the GUI-dependent methods with specific errors.

  // Before the startup file: a stop asked while it opens is the clean
  // shutdown's, bounded (app/StopSignal.h), not the default's.
  stopsignal::Install();
  if (!fileToLoad.empty()) {
    LOG_INFO("Loading startup file: {}", fileToLoad);
    if (ui::IsProjectPath(fileToLoad)) {
      std::string error;
      const std::string root = ui::NormalizeProjectPick(fileToLoad);
      if (ui::OpenProject(state, root, &error))
        ui::MarkProjectOpened(state, root);
      else
        LOG_ERROR("Could not open {}: {}", root, error);
    } else {
      // The import job, as File > Import runs it: the loop below polls it,
      // a client sees it in background_jobs and may cancel it.
      ui::ImportRequest request;
      request.path = fileToLoad;
      request.allowLargeMesh = allowLargeMesh;
      std::string error;
      if (!ui::StartImport(state, request, &error))
        LOG_ERROR("Could not import {}: {}", fileToLoad, error);
    }
  }

  RemoteServer server;
  if (!server.Start(static_cast<uint16_t>(remotePort), RemoteMode::Headless,
                    bindAll)) {
    LOG_ERROR("Headless: cannot listen on port {} — exiting", remotePort);
    return 1; // CI must fail fast, not hang serving nothing
  }
  // With `--remote 0` the kernel chose; say which, so a launcher reading this
  // process's output learns the port without racing for it. The instance
  // registry carries the same number for anyone who missed the line.
  if (remotePort == 0)
    LOG_INFO("Headless: remote port {}", server.Port());
  if (!recordPath.empty() && !server.StartRecording(recordPath)) {
    return 1;
  }

  stopsignal::OnForcedExit(
      [port = server.Port()] { remote::WithdrawInstance(port); });

  const auto start = std::chrono::steady_clock::now();
  auto seconds = [&] {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                         start)
        .count();
  };
  Autosaver autosaver(autosaveSeconds);
  autosaver.Prime(seconds());

  while (!stopsignal::Requested()) {
    watchdog::Beat();
    crash::NoteProject(state.projectPath); // the marker names the project
    server.ProcessPending(state);
    // Every long-running operation, from one place (ui/BackgroundJobs.h). The
    // RPC handlers poll too — a handler must apply a finished result before it
    // answers a question about it — but a job must also be able to finish
    // while no client is asking.
    ui::PollBackgroundJobs(state);
    // The PROJECT, not the open tab (F10) — same fix as the windowed loop.
    ui::AutosaveProject(state, autosaver, seconds());
    // Up to 2 ms for jobs and autosave; a request ends it at once.
    server.WaitForRequest(std::chrono::milliseconds(2));
  }

  LOG_INFO("Headless: stop requested, shutting down");
  server.Stop();
  // Clean exit with nothing unsaved: drop the sidecar. Dirty exits keep it.
  if (state.document && !ui::ProjectDirty(state)) {
    Autosaver::Clear(state.projectPath);
  }
  return 0;
}
