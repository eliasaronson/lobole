#pragma once

#include "ui/AppState.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <deque>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct ImGuiTestEngine;
struct ImGuiTestContext;

namespace remote {
class RpcParams;
}

// Opt-in JSON-RPC server (newline-delimited JSON over localhost TCP) that
// exposes the application to scripts and AI assistants:
//   - semantic commands (features as JSON, undo/redo, project I/O, camera)
//   - UI-parity actions executed through the embedded ImGui Test Engine
//     (menu clicks, item input, viewport clicks/drags, key chords)
//   - screenshots of the live viewport
// Requests:  {"id": 1, "method": "add_feature", "params": {...}}\n
// Responses: {"id": 1, "result": {...}} or {"id": 1, "error": "..."}\n
class RemoteServer {
public:
  ~RemoteServer();

  // Two capabilities, not one flag. `hasRenderer` = there is a rendered frame
  // to capture and pick against (screenshots, viewport coordinates); false
  // under --headless. Whether the UI-PARITY tier works is a separate
  // question, answered by SetTestEngine — a build configured with
  // -DENABLE_TEST_ENGINE=OFF has a renderer and no way to drive it.
  // `bindAll` opens the port on 0.0.0.0 instead of 127.0.0.1. The RPC
  // surface has no authentication and can read and write files, so loopback
  // is the only safe default on a shared machine; a container with a
  // namespaced network is the case this exists for.
  bool Start(uint16_t port, bool hasRenderer, bool bindAll = false);

  // Enables the UI-parity tier (menu clicks, item input, viewport clicks).
  // A no-op in a build without the ImGui Test Engine, so callers need no
  // #ifdef of their own beyond obtaining the engine.
  void SetTestEngine(ImGuiTestEngine* engine);

  // Can this instance be driven through its GUI? False under --headless and
  // in a build without the test engine — reported by `ping` so a client
  // learns it before trying.
  bool UiActionsAvailable() const;
  // The port this instance is serving (0 = not serving). Also what it
  // announced itself as in the instance registry.
  uint16_t Port() const { return m_port; }
  void Stop();
  bool IsRunning() const { return m_running; }

  // Main-thread pump: executes queued requests against the app state.
  void ProcessPending(ui::AppState& state);

  // Session recording (S4): tee every handled semantic request as one JSON
  // per line — a replayable session (tools/designer_replay.py). UI actions
  // and screenshots are not recorded (not replayable headless).
  bool StartRecording(const std::string& path);

  // Screenshot handshake with the frame loop (capture needs the frame's
  // draw data, so main.cpp performs it and reports back).
  bool WantsScreenshot(std::string& path);
  void ScreenshotDone(bool ok, const std::string& error);

  // V3 render_sheet: a staged multi-view capture job. The handler stages it
  // and the ProcessPending pump captures ONE named view per frame through
  // the ordinary screenshot handshake (temp tile PNGs), then composites
  // tiles + numeric footer into one sheet and responds. Requests queue
  // behind an active job the same way they do behind a screenshot.
  struct SheetJob {
    bool active = false;
    std::vector<std::string> views;
    std::vector<std::string> tilePaths; // temp per-view captures
    std::vector<std::string> footer;    // digest lines, built at request time
    std::string outPath;
    size_t nextView = 0;
    int cropX = 0, cropY = 0, cropW = 0, cropH = 0;
    nlohmann::json id;
    int client = -1;
  };

private:
  struct Request {
    nlohmann::json id;
    std::string method;
    nlohmann::json params;
    int client = -1; // fd the response goes back to
  };

  // Accept loop (listen socket bound in Start) spawning one reader per
  // connection — several clients may be connected at once (e.g. the MCP
  // server plus a hand-driven python shell); requests interleave through the
  // single main-thread queue and responses route back by fd.
  void ReaderThread();
  void ClientThread(int fd);
  void Send(int client, const nlohmann::json& message);
  void Respond(int client, const nlohmann::json& id,
               const nlohmann::json& result);
  void RespondError(int client, const nlohmann::json& id,
                    const std::string& error);

  nlohmann::json HandleSemantic(const Request& request, ui::AppState& state,
                                bool& handled, remote::RpcParams& p);
  bool QueueUiAction(const Request& request); // returns false if unknown
  static bool IsUiAction(const std::string& method);

  ImGuiTestEngine* m_engine = nullptr;
  std::thread m_thread;
  std::atomic<bool> m_running{false};
  bool m_hasRenderer = false;
  uint16_t m_port = 0;
  int m_listenFd = -1;

  std::mutex m_clientsMutex;
  std::vector<int> m_clients;               // open connection fds (for Stop)
  std::vector<std::thread> m_clientThreads; // joined in Stop

  std::mutex m_queueMutex;
  std::deque<Request> m_requests;

  std::ofstream m_recording; // open = recording (main thread only)

  std::mutex m_sendMutex;

  // One in-flight UI action at a time (runs across frames in the engine)
  nlohmann::json m_uiActionId;
  int m_uiActionClient = -1;
  bool m_uiActionInFlight = false;
  std::function<void(ImGuiTestContext*)> m_uiAction;
  // The live AppState, refreshed every ProcessPending tick: UI actions run
  // on the main thread inside the engine's coroutine, so they can read it
  // (menu_items reports the menu bar's own names). Null before the first tick.
  ui::AppState* m_state = nullptr;

  // What the action wants to answer with (ui_items). Written inside the
  // action, on the main thread; sent when the engine's queue drains.
  nlohmann::json m_uiActionResult;

  // Pending screenshot
  nlohmann::json m_screenshotId;
  int m_screenshotClient = -1;
  std::string m_screenshotPath;
  bool m_screenshotPending = false;
  SheetJob m_sheet;

  friend void RemoteServerRunUiAction(ImGuiTestContext* ctx);
};
