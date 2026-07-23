#include "app/HeadlessMain.h"

#include "render/Camera.h"
#include "common/Logger.h"
#include "core/io/Autosaver.h"
#include "core/Command.h"
#include "core/Document.h"
#include "remote/RemoteServer.h"
#include "ui/AppActions.h"
#include "ui/AppState.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <thread>

namespace {

// Signal-flag pattern: handlers may only touch lock-free atomics.
std::atomic<bool> g_stopRequested{false};

void HandleStopSignal(int) { g_stopRequested = true; }

} // namespace

int RunHeadless(int remotePort, const std::string& fileToLoad,
                double autosaveSeconds, const std::string& recordPath) {
  LOG_INFO("Headless mode: document engine + remote server, no GUI");

  // Same defaults as the GUI so view/fit_view/get_state.camera behave
  // identically; a GUI session opening the same project sees the same framing.
  Camera camera;
  camera.SetPosition(glm::vec3(8.0f, 6.0f, 8.0f));
  camera.SetTarget(glm::vec3(0.0f, 0.0f, 0.0f));
  camera.SetAspectRatio(1280.0f / 720.0f);

  ui::AppState state;
  state.document = std::make_unique<Document>();
  state.commands = std::make_unique<CommandStack>();
  state.camera = &camera;
  ui::EnsureParts(state); // one part to start (assemblies A1)
  // All renderer/selector pointers stay null: SyncDocumentRenderers no-ops,
  // and RemoteServer degrades the GUI-dependent methods with specific errors.

  if (!fileToLoad.empty()) {
    LOG_INFO("Loading startup file: {}", fileToLoad);
    ui::LoadSTEPFile(state, fileToLoad);
  }

  RemoteServer server;
  if (!server.Start(static_cast<uint16_t>(remotePort), /*engine=*/nullptr)) {
    LOG_ERROR("Headless: cannot listen on port {} — exiting", remotePort);
    return 1; // CI must fail fast, not hang serving nothing
  }
  if (!recordPath.empty() && !server.StartRecording(recordPath)) {
    return 1;
  }

  std::signal(SIGINT, HandleStopSignal);
  std::signal(SIGTERM, HandleStopSignal);

  const auto start = std::chrono::steady_clock::now();
  auto seconds = [&] {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                         start)
        .count();
  };
  Autosaver autosaver(autosaveSeconds);
  autosaver.Prime(seconds());

  while (!g_stopRequested) {
    server.ProcessPending(state);
    if (state.document) {
      autosaver.MaybeSave(*state.document, state.projectPath,
                          state.lastSavedVersion, seconds());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  LOG_INFO("Headless: stop requested, shutting down");
  server.Stop();
  // Clean exit with nothing unsaved: drop the sidecar. Dirty exits keep it.
  if (state.document &&
      state.document->Version() == state.lastSavedVersion) {
    Autosaver::Clear(state.projectPath);
  }
  return 0;
}
