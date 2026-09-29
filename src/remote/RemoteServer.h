#pragma once

#include "ui/AppState.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <deque>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct ImGuiTestEngine;
struct ImGuiTestContext;

namespace remote {
class RpcParams;
class CamParking;
}

// Opt-in JSON-RPC server (newline-delimited JSON over localhost TCP) that
// exposes the application to scripts and AI assistants:
//   - semantic commands (features as JSON, undo/redo, project I/O, camera)
//   - UI-parity actions executed through the embedded ImGui Test Engine
//     (menu clicks, item input, viewport clicks/drags, key chords)
//   - screenshots of the live viewport
// Requests:  {"id": 1, "method": "add_feature", "params": {...}}\n
// Responses: {"id": 1, "result": {...}} or {"id": 1, "error": "..."}\n
// What kind of process is serving: decides which render/GUI capabilities the
// RPC surface can honestly offer, and is reported by `ping` and the instance
// registry so a client learns it before trying.
//   Gui       — windowed frame loop; renders, screenshots, and (per build)
//               the ImGui test engine.
//   Headless  — document engine only; no renderer of any kind.
//   Offscreen — real renderer with no window or display server; renders
//               work, there is no GUI to drive.
enum class RemoteMode { Gui, Headless, Offscreen };

class RemoteServer {
public:
  ~RemoteServer();

  // Mode and capability are separate questions. `mode` says whether a
  // rendered frame exists to capture and pick against (screenshots, viewport
  // coordinates) — Headless has none. Whether the UI-PARITY tier works is
  // answered by SetTestEngine — a build configured with
  // -DENABLE_TEST_ENGINE=OFF has a renderer and no way to drive it.
  // `bindAll` opens the port on 0.0.0.0 instead of 127.0.0.1. The RPC
  // surface has no authentication and can read and write files, so loopback
  // is the only safe default on a shared machine; a container with a
  // namespaced network is the case this exists for.
  bool Start(uint16_t port, RemoteMode mode, bool bindAll = false);

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
  // Returns the tick's remote activity: requests executed, plus one while an
  // async job (ui action / screenshot / sheet render) is still in flight —
  // the idle pacer treats nonzero as "stay at full rate"
  // (docs/rendering/frame-pacing.md#idle-throttle).
  size_t ProcessPending(ui::AppState& state);

  // Called from the socket reader threads whenever a request lands in the
  // queue, so a throttled frame loop wakes immediately instead of eating the
  // idle timeout on every RPC. Set BEFORE Start() and never after: the
  // reader threads read it unsynchronized.
  void SetWakeCallback(std::function<void()> wake) { m_wake = std::move(wake); }

  // Session recording (S4): tee every handled semantic request as one JSON
  // per line — a replayable session (tools/designer_replay.py). UI actions
  // and screenshots are not recorded (not replayable headless).
  bool StartRecording(const std::string& path);

  // Screenshot handshake with the frame loop (capture needs the frame's
  // draw data, so main.cpp performs it and reports back). dominant/frameHash
  // are the capture's FrameStats: the response WARNS when a frame is almost
  // one flat color or pixel-identical to the previous capture — a confident
  // picture of nothing is the failure mode being caught.
  bool WantsScreenshot(std::string& path);
  void ScreenshotDone(bool ok, const std::string& error, float dominant = 0.0f,
                      uint64_t frameHash = 0);

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
    std::vector<std::string> warnings;  // per-tile frame postconditions
    std::string outPath;
    size_t nextView = 0;
    int cropX = 0, cropY = 0, cropW = 0, cropH = 0;
    // Focus (round-8 retro): frame ONE occurrence large in every view
    // instead of the whole document — close-ups become automatic.
    bool hasFocus = false;
    glm::vec3 focusMin{0.0f}, focusMax{0.0f};
    nlohmann::json id;
    int client = -1;
  };

private:
  struct Request {
    nlohmann::json id;
    std::string method;
    nlohmann::json params;
    int client = -1; // fd the response goes back to
    // Came back from parking (below): dispatch it, do not park it again.
    bool resumed = false;
  };

  // A request answered LATER, while the frame and other clients carry on:
  // a CAM request waiting for its passes (remote/RemoteCam.h,
  // docs/architecture.md#requests-that-wait). Unlike a screenshot it does
  // not hold the queue -- only its own client's later requests wait behind
  // it, so each connection still gets its answers in the order it asked.
  // Defined in RemoteUiActions.cpp; shared_ptr because it is incomplete here.
  struct Parked;
  std::vector<std::shared_ptr<Parked>> m_parked;
  // Advance every parked request one tick: answer it, refuse it, or put it
  // back at the head of the queue to be dispatched.
  void PollParked(ui::AppState& state);
  bool ClientConnected(int client);

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
  RemoteMode m_mode = RemoteMode::Headless;
  // Something outside this class fulfils the screenshot/sheet handshake
  // (WantsScreenshot/ScreenshotDone): the windowed frame loop in Gui, the
  // offscreen tick in Offscreen, nothing in Headless. Gating on this is what
  // keeps a capture request from wedging the queue in a mode where nothing
  // ever answers it: accepting one sets m_screenshotPending and
  // ProcessPending then queues every later request behind a capture that
  // would never come.
  bool CaptureAvailable() const { return m_mode != RemoteMode::Headless; }
  uint16_t m_port = 0;
  int m_listenFd = -1;

  std::mutex m_clientsMutex;
  std::vector<int> m_clients;               // open connection fds (for Stop)
  std::vector<std::thread> m_clientThreads; // joined in Stop

  std::mutex m_queueMutex;
  std::deque<Request> m_requests;
  std::function<void()> m_wake; // enqueue notification; see SetWakeCallback

  std::ofstream m_recording; // open = recording (main thread only)

  std::mutex m_sendMutex;

  // One in-flight UI action at a time (runs across frames in the engine)
  nlohmann::json m_uiActionId;
  int m_uiActionClient = -1;
  // True while a `batch` is running its calls, so a nested one is refused
  // rather than opening a second transaction inside the first.
  bool m_inBatch = false;
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
  // Frames a staged capture has waited for the camera to stop moving. A
  // capture taken mid-animation shows a view nobody asked for; the cap is so
  // an animation that never ends cannot hold a client forever.
  int m_screenshotHeldFrames = 0;
  uint64_t m_lastFrameHash = 0; // unchanged-frame postcondition
  SheetJob m_sheet;

  friend void RemoteServerRunUiAction(ImGuiTestContext* ctx);
};
