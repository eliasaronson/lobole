// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "remote/RemoteServer.h"

#include "remote/InstanceRegistry.h"

#include <Standard_Failure.hxx>

#include "cad/CADKernel.h"
#include "cad/CADMaterials.h"
#include "cad/FaceSelector.h"
#include "cad/MeshExport.h"
#include "common/Logger.h"
#include "core/Conventions.h"
#include "core/Expression.h"
#include "core/MassProps.h"
#include "core/Measure.h"
#include "core/drawing/DrawingExport.h"
#include "core/features/DerivedFeature.h"
#include "core/features/SketchFeature.h"
#include "core/io/Autosaver.h"
#include "core/io/Checkpoints.h"
#include "core/io/GitVault.h"
#include "core/io/Keymap.h"
#include "core/io/Serialization.h"
#include "core/io/Settings.h"
#include "core/sketch/SketchSolver.h"
#include "remote/RemoteAssembly.h"
#include "remote/RemoteCam.h"
#include "remote/RemoteConfigurations.h"
#include "remote/RemoteDrawings.h"
#include "remote/RemoteHelpers.h"
#include "remote/RemoteInspection.h"
#include "remote/RemoteModeling.h"
#include "remote/RemoteMotion.h"
#include "remote/RemoteProject.h"
#include "remote/RemoteSimulation.h"
#include "remote/RpcParams.h"

#include "render/Camera.h"
#include "render/FrameTimer.h"
#include "render/MeshRenderer.h"
#include "render/RayTracingRenderer.h"
#include "render/RenderSheet.h"
#include "ui/AssemblyActions.h"
#include "ui/BackgroundJobs.h"
#include "ui/CamJobs.h"
#include "ui/Commands.h" // ActiveContext (get_keymap)
#include "ui/DerivedActions.h"
#include "ui/DocumentSlots.h"
#include "ui/PreviewFeature.h"
#include "ui/ProjectActions.h"
#include "ui/RendererSync.h"
#include "ui/SelectionPipeline.h" // ResolveFaceItem (U1b)
#include "ui/SettingsWindow.h"    // ApplySettings / the save debounce
#include "ui/SimulationActions.h"
#include "ui/SketchInteraction.h" // FaceElementRef
#include "ui/VariableActions.h"
#include "ui/ViewActions.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>

#include "imgui.h"
#ifdef HAVE_TEST_ENGINE
#include "imgui_te_context.h"
#include "imgui_te_engine.h"
#endif

#include <cstring>

using nlohmann::json;

#include "remote/RemoteServerInternal.h"

// The connection itself: the listening socket, the reader and client threads,
// and framing a response. Nothing here knows what a method MEANS.
//
// The screenshot handshake is here too, because it belongs to the session
// rather than to any one RPC -- the frame loop asks whether a capture is
// wanted and reports back when it lands.
// Split out of RemoteServer.cpp (docs/architecture.md#the-remote-json-rpc).
// RemoteServer.h is unchanged: these are the same class's members,
// defined across three translation units instead of one.

// --- Sockets: BSD on Linux/mac, Winsock on Windows (loopback JSON-RPC only)
// ---
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
namespace {
// Winsock needs global init; a static object brackets the process lifetime.
struct WinsockInit {
  WinsockInit() {
    WSADATA d;
    WSAStartup(MAKEWORD(2, 2), &d);
  }
  ~WinsockInit() { WSACleanup(); }
} g_winsockInit;
inline int SockClose(int fd) { return ::closesocket(fd); }
inline long SockRead(int fd, void* b, size_t n) {
  return ::recv(fd, static_cast<char*>(b), static_cast<int>(n), 0);
}
inline long SockWrite(int fd, const void* b, size_t n) {
  return ::send(fd, static_cast<const char*>(b), static_cast<int>(n), 0);
}
} // namespace
#ifndef SHUT_RDWR
#define SHUT_RDWR SD_BOTH
#endif
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
namespace {
inline int SockClose(int fd) { return ::close(fd); }
inline long SockRead(int fd, void* b, size_t n) { return ::read(fd, b, n); }
inline long SockWrite(int fd, const void* b, size_t n) {
  return ::write(fd, b, n);
}
} // namespace
#endif

// True when the executable this process is running has been replaced or
// removed on disk since it started. See the `ping` handler for why.
bool BinaryReplaced() {
#ifdef __linux__
  std::error_code ec;
  const std::string exe =
      std::filesystem::read_symlink("/proc/self/exe", ec).string();
  if (ec)
    return false;
  static const std::string kDeleted = " (deleted)";
  return exe.size() >= kDeleted.size() &&
         exe.compare(exe.size() - kDeleted.size(), kDeleted.size(), kDeleted) ==
             0;
#else
  return false;
#endif
}

// The one live server. RemoteUiActions.cpp needs it: the reusable test the
// engine runs has no argument, so the queued action is reached through here.
RemoteServer* g_RemoteServerInstance = nullptr;

RemoteServer::~RemoteServer() { Stop(); }

const char* ModeName(RemoteMode mode) {
  switch (mode) {
  case RemoteMode::Gui:
    return "gui";
  case RemoteMode::Headless:
    return "headless";
  case RemoteMode::Offscreen:
    return "offscreen";
  }
  return "headless"; // unreachable; the safe answer is the capability floor
}

bool RemoteServer::Start(uint16_t port, RemoteMode mode, bool bindAll) {
  m_mode = mode;
  g_RemoteServerInstance = this;

  // Bind synchronously so a taken/privileged port fails Start (headless CI
  // must exit non-zero, not serve nothing); the thread only accepts.
  m_listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
  int opt = 1;
  // Winsock's optval is const char*; POSIX takes const void* (accepts this
  // too).
  ::setsockopt(m_listenFd, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&opt), sizeof(opt));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  // Loopback unless explicitly told otherwise: this interface can create,
  // read, overwrite and export files with no authentication of any kind.
  addr.sin_addr.s_addr = htonl(bindAll ? INADDR_ANY : INADDR_LOOPBACK);
  // Port 0 asks the KERNEL to choose, and the chosen port is read back below.
  // That closes a race every test script here used to run: pick a free port
  // by binding 0, CLOSE the socket, then hand the number to a child that
  // binds it later — between those two steps the same port can be handed to
  // somebody else. Binding once, in the process that keeps it, has no window.
  addr.sin_port = htons(port);
  if (::bind(m_listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) <
          0 ||
      ::listen(m_listenFd, 1) < 0) {
    LOG_ERROR("Remote control: cannot listen on port {}", port);
    SockClose(m_listenFd);
    m_listenFd = -1;
    return false;
  }
  if (port == 0) {
    sockaddr_in actual{};
    // Winsock spells this `int`, POSIX `socklen_t`; the packaging/windows
    // shim surface is deliberately small, so do not add a typedef for one use.
#ifdef _WIN32
    int len = sizeof(actual);
#else
    socklen_t len = sizeof(actual);
#endif
    if (::getsockname(m_listenFd, reinterpret_cast<sockaddr*>(&actual), &len) <
        0) {
      LOG_ERROR("Remote control: bound a port the OS will not name");
      SockClose(m_listenFd);
      m_listenFd = -1;
      return false;
    }
    port = ntohs(actual.sin_port);
  }

  m_running = true;
  m_port = port;
  m_thread = std::thread([this] { ReaderThread(); });
  // Announce it so a client can FIND this instance rather than be told which
  // port to assume (remote/InstanceRegistry.h). The registry records the
  // MODE, which is what decides whether renders are possible; whether the
  // GUI can be driven is per-build and `ping` reports it.
  remote::AnnounceInstance(port, ModeName(mode));
  if (bindAll) {
    LOG_WARN("Remote control listening on 0.0.0.0:{} — UNAUTHENTICATED, and "
             "it can read and write files. Only do this inside a sandbox you "
             "trust (a container with a published port, not a shared LAN).",
             port);
  } else {
    LOG_INFO("Remote control listening on 127.0.0.1:{}", port);
  }
  return true;
}

bool RemoteServer::UiActionsAvailable() const {
#ifdef HAVE_TEST_ENGINE
  return m_engine != nullptr;
#else
  return false;
#endif
}

bool RemoteServer::StartRecording(const std::string& path) {
  m_recording.open(path, std::ios::trunc);
  if (!m_recording.is_open()) {
    LOG_ERROR("Cannot open session recording file {}", path);
    return false;
  }
  LOG_INFO("Recording session to {}", path);
  return true;
}

void RemoteServer::Stop() {
  if (!m_running)
    return;
  m_running = false;
  if (m_port != 0) {
    remote::WithdrawInstance(
        m_port); // a crash leaves it; readers check the pid
    m_port = 0;
  }
  {
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    for (int fd : m_clients)
      ::shutdown(fd, SHUT_RDWR);
  }
  if (m_listenFd >= 0) {
    ::shutdown(m_listenFd, SHUT_RDWR);
    SockClose(m_listenFd);
    m_listenFd = -1;
  }
  if (m_thread.joinable())
    m_thread.join();
  for (std::thread& t : m_clientThreads)
    if (t.joinable())
      t.join();
  m_clientThreads.clear();
}

void RemoteServer::ReaderThread() {
  while (m_running) {
    int client = ::accept(m_listenFd, nullptr, nullptr);
    if (client < 0)
      break;
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    if (m_clients.size() >= 8) { // runaway-connection backstop
      SockClose(client);
      continue;
    }
    m_clients.push_back(client);
    const uint64_t connection = m_nextConnection++;
    m_openConnections.insert(connection);
    m_clientThreads.emplace_back(
        [this, client, connection] { ClientThread(client, connection); });
    LOG_INFO("Remote control: client connected ({} active)", m_clients.size());
  }
}

void RemoteServer::ClientThread(int client, uint64_t connection) {
  std::string buffer;
  char chunk[4096];
  while (m_running) {
    auto n = SockRead(client, chunk, sizeof(chunk));
    if (n <= 0)
      break;
    buffer.append(chunk, static_cast<size_t>(n));

    size_t pos;
    while ((pos = buffer.find('\n')) != std::string::npos) {
      std::string line = buffer.substr(0, pos);
      buffer.erase(0, pos + 1);
      if (line.empty())
        continue;

      Request request;
      request.client = client;
      request.connection = connection;
      try {
        json msg = json::parse(line);
        request.id = msg.value("id", json());
        request.method = msg.value("method", "");
        request.params = msg.value("params", json::object());
      } catch (const std::exception& e) {
        Send(client,
             {{"id", nullptr}, {"error", std::string("parse: ") + e.what()}});
        continue;
      }
      {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_requests.push_back(std::move(request));
      }
      // Outside the lock: wake a throttled frame loop so the request is
      // pumped now, not after the idle timeout
      // (docs/rendering/frame-pacing.md#idle-throttle).
      if (m_wake)
        m_wake();
    }
  }
  SockClose(client);
  {
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    m_clients.erase(std::remove(m_clients.begin(), m_clients.end(), client),
                    m_clients.end());
    // The pump cancels the study jobs this connection started, unless they
    // were asked to outlive it.
    m_openConnections.erase(connection);
    m_closedConnections.push_back(connection);
  }
  if (m_wake)
    m_wake();
  LOG_INFO("Remote control: client disconnected");
}

void RemoteServer::Send(int client, const json& message) {
  if (client < 0)
    return;
  std::string line = message.dump() + "\n";
  std::lock_guard<std::mutex> lock(m_sendMutex);
  size_t sent = 0;
  while (sent < line.size()) {
    auto n = SockWrite(client, line.data() + sent, line.size() - sent);
    if (n <= 0)
      return;
    sent += static_cast<size_t>(n);
  }
}

void RemoteServer::Respond(int client, const json& id, const json& result) {
  Send(client, {{"id", id}, {"result", result}});
}

void RemoteServer::RespondError(int client, const json& id,
                                const std::string& error) {
  Send(client, {{"id", id}, {"error", error}});
}

// ---------------------------------------------------------------------------
// Main-thread dispatch

bool RemoteServer::WantsScreenshot(std::string& path) {
  if (!m_screenshotPending)
    return false;
  // Not while the camera is still moving. A camera move ANIMATES for about a
  // second, so a capture taken straight after one shows a frame part way
  // through the turn — a view nobody asked for, and one that reads as a bug in
  // whatever was being looked at. Two false findings in one dogfooding session
  // came from exactly this (docs/rendering/README.md#screenshots). The `view`
  // RPC already dodges it by snapping instead of animating; every other route
  // into a camera move — menu_click, the view cube, AlignToFace — does not, so
  // the wait belongs here, where all of them pass through.
  //
  // Capped, because "wait for a condition" and "wait forever" are one bug
  // apart: 300 frames is several times the longest animation, and after it the
  // capture goes ahead rather than leaving a client with no answer.
  constexpr int kMaxHeldFrames = 300;
  if (m_state != nullptr && m_state->camera != nullptr &&
      m_state->camera->IsAnimating() &&
      m_screenshotHeldFrames < kMaxHeldFrames) {
    m_screenshotHeldFrames++;
    return false;
  }
  path = m_screenshotPath;
  return true;
}

void RemoteServer::ScreenshotDone(bool ok, const std::string& error,
                                  float dominant, uint64_t frameHash) {
  m_screenshotPending = false;
  // Frame postconditions: a capture that "succeeded" but shows nothing (or
  // shows the same thing again) is the trap — warn in the same response.
  std::vector<std::string> warnings;
  if (ok) {
    char buf[160];
    if (dominant > 0.95f) {
      snprintf(buf, sizeof buf,
               "%.0f%% of the frame is a single color — is the camera aimed "
               "at the model? (fit_view, or render_sheet for framed views)",
               dominant * 100.0f);
      warnings.push_back(buf);
    }
    if (frameHash != 0 && frameHash == m_lastFrameHash)
      warnings.push_back(
          "frame is pixel-identical to the previous capture — did the scene "
          "or camera actually change?");
    m_lastFrameHash = frameHash;
  }
  if (m_sheet.active) {
    // A sheet tile: progress (and the final response) belong to the pump.
    if (!ok) {
      RespondError(m_sheet.client, m_sheet.id,
                   "render_sheet capture failed: " + error);
      for (const std::string& tile : m_sheet.tilePaths)
        std::filesystem::remove(tile);
      m_sheet = SheetJob{};
      return;
    }
    const size_t tile = m_sheet.nextView - 1; // the tile just captured
    for (const std::string& w : warnings)
      m_sheet.warnings.push_back((tile < m_sheet.views.size()
                                      ? "view '" + m_sheet.views[tile] + "': "
                                      : "") +
                                 w);
    return;
  }
  if (ok) {
    json resp = {{"path", m_screenshotPath}};
    // Where the 3D view sits inside the captured framebuffer. A caller that
    // wants to reason about PIXELS — a render regression test, a diffing
    // script — otherwise has to guess which part of the image is the model
    // and which is panel chrome, and guesses go stale with the layout.
    if (m_state) {
      resp["viewport"] = {{"x", (int)m_state->viewportPos.x},
                          {"y", (int)m_state->viewportPos.y},
                          {"width", (int)m_state->viewportSize.x},
                          {"height", (int)m_state->viewportSize.y}};
    }
    if (!warnings.empty())
      resp["warnings"] = warnings;
    Respond(m_screenshotClient, m_screenshotId, resp);
  } else {
    RespondError(m_screenshotClient, m_screenshotId, error);
  }
}
