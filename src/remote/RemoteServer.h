#pragma once

#include "ui/AppState.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

struct ImGuiTestEngine;
struct ImGuiTestContext;

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

  bool Start(uint16_t port, ImGuiTestEngine* engine);
  void Stop();
  bool IsRunning() const { return m_running; }

  // Main-thread pump: executes queued requests against the app state.
  void ProcessPending(ui::AppState& state);

  // Screenshot handshake with the frame loop (capture needs the frame's
  // draw data, so main.cpp performs it and reports back).
  bool WantsScreenshot(std::string& path);
  void ScreenshotDone(bool ok, const std::string& error);

private:
  struct Request {
    nlohmann::json id;
    std::string method;
    nlohmann::json params;
  };

  void ReaderThread(); // accept loop; the listen socket is bound in Start
  void Send(const nlohmann::json& message);
  void Respond(const nlohmann::json& id, const nlohmann::json& result);
  void RespondError(const nlohmann::json& id, const std::string& error);

  nlohmann::json HandleSemantic(const Request& request, ui::AppState& state,
                                bool& handled);
  bool QueueUiAction(const Request& request); // returns false if unknown
  static bool IsUiAction(const std::string& method);

  ImGuiTestEngine* m_engine = nullptr;
  std::thread m_thread;
  std::atomic<bool> m_running{false};
  int m_listenFd = -1;
  std::atomic<int> m_clientFd{-1};

  std::mutex m_queueMutex;
  std::deque<Request> m_requests;

  std::mutex m_sendMutex;

  // One in-flight UI action at a time (runs across frames in the engine)
  nlohmann::json m_uiActionId;
  bool m_uiActionInFlight = false;
  std::function<void(ImGuiTestContext*)> m_uiAction;

  // Pending screenshot
  nlohmann::json m_screenshotId;
  std::string m_screenshotPath;
  bool m_screenshotPending = false;

  friend void RemoteServerRunUiAction(ImGuiTestContext* ctx);
};
