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
#include "remote/RemoteProject.h"
#include "remote/RemoteSimulation.h"
#include "remote/RpcParams.h"

#include "render/Camera.h"
#include "render/FrameTimer.h"
#include "render/MeshRenderer.h"
#include "render/RayTracingRenderer.h"
#include "render/RenderSheet.h"
#include "ui/AppActions.h"
#include "ui/BackgroundJobs.h"
#include "ui/CamJobs.h"
#include "ui/Commands.h" // ActiveContext (get_keymap)
#include "ui/DerivedActions.h"
#include "ui/PreviewFeature.h"
#include "ui/SelectionPipeline.h" // ResolveFaceItem (U1b)
#include "ui/SettingsWindow.h"    // ApplySettings / the save debounce
#include "ui/SimulationActions.h"
#include "ui/SketchInteraction.h" // FaceElementRef

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

using nlohmann::json;

static RemoteServer* s_Instance = nullptr;
#ifdef HAVE_TEST_ENGINE
static ImGuiTest* s_UiActionTest = nullptr;
#endif

// TestFunc of the reusable "remote action" test: runs whatever UI action is
// currently queued. Runs inside the test engine's coroutine, so it can span
// frames (menu navigation, drags) exactly like a scripted human.
#ifdef HAVE_TEST_ENGINE
void RemoteServerRunUiAction(ImGuiTestContext* ctx) {
  if (s_Instance && s_Instance->m_uiAction) {
    s_Instance->m_uiAction(ctx);
  }
}
#endif

RemoteServer::~RemoteServer() { Stop(); }

namespace {
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
} // namespace

bool RemoteServer::Start(uint16_t port, RemoteMode mode, bool bindAll) {
  m_mode = mode;
  s_Instance = this;

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

void RemoteServer::SetTestEngine(ImGuiTestEngine* engine) {
#ifdef HAVE_TEST_ENGINE
  m_engine = engine;
  if (engine == nullptr)
    return;
  // One reusable test carries every queued UI action (ProcessPending queues
  // it; the engine runs it between frames, where clicks are legal).
  s_UiActionTest = IM_REGISTER_TEST(engine, "remote", "action");
  s_UiActionTest->TestFunc = [](ImGuiTestContext* ctx) {
    RemoteServerRunUiAction(ctx);
  };
#else
  // Built with -DENABLE_TEST_ENGINE=OFF: the UI-parity tier does not exist,
  // and every ui_action refuses with that reason rather than a silent no-op.
  (void)engine;
#endif
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
    m_clientThreads.emplace_back([this, client] { ClientThread(client); });
    LOG_INFO("Remote control: client connected ({} active)", m_clients.size());
  }
}

void RemoteServer::ClientThread(int client) {
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
      // (docs/rendering/idle-throttle.md).
      if (m_wake)
        m_wake();
    }
  }
  SockClose(client);
  {
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    m_clients.erase(std::remove(m_clients.begin(), m_clients.end(), client),
                    m_clients.end());
  }
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

namespace {

json Vec3ToJson(const glm::vec3& v) { return json::array({v.x, v.y, v.z}); }

// Levenshtein distance, for the "did you mean" hint on a rejected param key.
size_t EditDistance(const std::string& a, const std::string& b) {
  std::vector<size_t> prev(b.size() + 1), cur(b.size() + 1);
  for (size_t j = 0; j <= b.size(); j++)
    prev[j] = j;
  for (size_t i = 1; i <= a.size(); i++) {
    cur[0] = i;
    for (size_t j = 1; j <= b.size(); j++) {
      const size_t cost = a[i - 1] == b[j - 1] ? 0 : 1;
      cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
    }
    std::swap(prev, cur);
  }
  return prev[b.size()];
}

// Message for a request whose params carried keys the handler never read —
// a silently-ignored typo under the old path. Names the offending key(s),
// suggests the closest accepted key, and lists the method's vocabulary.
std::string UnknownParamError(const std::string& method,
                              const std::vector<std::string>& unknown,
                              const std::set<std::string>& accepted) {
  std::string msg = "unknown parameter";
  msg += unknown.size() > 1 ? "s " : " ";
  for (size_t i = 0; i < unknown.size(); i++) {
    msg += (i ? ", '" : "'") + unknown[i] + "'";
    // Closest accepted key within a small edit distance = probable typo.
    std::string best;
    size_t bestDist = 3; // only suggest a genuinely near miss
    for (const std::string& k : accepted) {
      const size_t d = EditDistance(unknown[i], k);
      if (d < bestDist) {
        bestDist = d;
        best = k;
      }
    }
    if (!best.empty())
      msg += " (did you mean '" + best + "'?)";
  }
  msg += " for '" + method + "'.";
  if (!accepted.empty()) {
    msg += " Accepted: ";
    bool first = true;
    for (const std::string& k : accepted) {
      msg += (first ? "" : ", ") + k;
      first = false;
    }
    msg += ".";
  }
  return msg;
}

#ifdef HAVE_TEST_ENGINE
ImGuiKeyChord ParseChord(const std::string& text, std::string* error) {
  ImGuiKeyChord chord = 0;
  size_t start = 0;
  while (start <= text.size()) {
    size_t end = text.find('+', start);
    std::string token = text.substr(
        start, end == std::string::npos ? std::string::npos : end - start);
    if (token == "Ctrl")
      chord |= ImGuiMod_Ctrl;
    else if (token == "Shift")
      chord |= ImGuiMod_Shift;
    else if (token == "Alt")
      chord |= ImGuiMod_Alt;
    else if (token.size() == 1 && token[0] >= 'A' && token[0] <= 'Z')
      chord |= ImGuiKey_A + (token[0] - 'A');
    else if (token.size() == 1 && token[0] >= '0' && token[0] <= '9')
      chord |= ImGuiKey_0 + (token[0] - '0');
    else if (token == "Enter")
      chord |= ImGuiKey_Enter;
    else if (token == "Escape")
      chord |= ImGuiKey_Escape;
    else if (token == "Delete")
      chord |= ImGuiKey_Delete;
    else if (token == "Tab")
      chord |= ImGuiKey_Tab;
    else if (token == "Space")
      chord |= ImGuiKey_Space;
    else if (token == "Left")
      chord |= ImGuiKey_LeftArrow;
    else if (token == "Right")
      chord |= ImGuiKey_RightArrow;
    else if (token == "Up")
      chord |= ImGuiKey_UpArrow;
    else if (token == "Down")
      chord |= ImGuiKey_DownArrow;
    else if (token == "PageUp")
      chord |= ImGuiKey_PageUp;
    else if (token == "PageDown")
      chord |= ImGuiKey_PageDown;
    else if (token == "Home")
      chord |= ImGuiKey_Home;
    else if (token == "End")
      chord |= ImGuiKey_End;
    else {
      if (error)
        *error = "unknown key token: " + token;
      return 0;
    }
    if (end == std::string::npos)
      break;
    start = end + 1;
  }
  return chord;
}

// The 3D viewport's screen rect, snapshotted every tick in ProcessPending.
// Zero size = no renderer (--headless) or not drawn yet; callers fall back to
// the whole window.
ImVec2 s_ViewportPos(0.0f, 0.0f);
ImVec2 s_ViewportSize(0.0f, 0.0f);

// (x, y) in 0..1 of the 3D VIEWPORT, which is what `describe` and every
// caller means by "viewport_click" — mapping them over the whole OS window
// aimed every click at the wrong pixel whenever a panel was docked (0.5,0.5
// landed in the feature tree, not the middle of the model).
ImVec2 FractionToPos(const json& params, const char* xKey = "x",
                     const char* yKey = "y", float xDef = 0.5f,
                     float yDef = 0.5f) {
  ImVec2 pos = s_ViewportPos;
  ImVec2 size = s_ViewportSize;
  if (size.x <= 1.0f || size.y <= 1.0f) {
    pos = ImVec2(0.0f, 0.0f);
    size = ImGui::GetMainViewport()->Size;
  }
  return ImVec2(pos.x + params.value(xKey, xDef) * size.x,
                pos.y + params.value(yKey, yDef) * size.y);
}

// A widget addressed either by test-engine path ({ref}) or by the numeric id
// {id} that `ui_items` reports. Ids are the only handle on widgets whose
// label is hidden ("##Type" — every LabeledCombo), which no path can reach.
struct ItemTarget {
  std::string path;
  ImGuiID id = 0;

  // A panel's DISPLAY title is not its address. Titles carry an icon and a
  // "###id" ("<glyph> Model###Model") so they can be reworded without
  // breaking anything, and ImGui hashes a window from the "###" onward — so
  // the addressable form is "//###Model/...". Callers should not have to
  // know that, and every script written before the icons landed says
  // "//Model/...", so accept the friendly form and rewrite it.
  static std::string ResolveWindowAlias(const std::string& ref) {
    if (!ref.starts_with("//") || ref.starts_with("//$") ||
        ref.starts_with("//###") || ref.starts_with("//**"))
      return ref;
    const size_t slash = ref.find('/', 2);
    const std::string window = ref.substr(2, slash - 2);
    if (window.empty() || window.find('#') != std::string::npos)
      return ref;
    // Only when the plain name names no window but the "###" form does —
    // never override a window that really is called that.
    if (ImGui::FindWindowByName(window.c_str()) != nullptr)
      return ref;
    const std::string hashed = "###" + window;
    if (ImGui::FindWindowByID(ImHashStr(hashed.c_str())) == nullptr)
      return ref;
    return "//" + hashed +
           (slash == std::string::npos ? "" : ref.substr(slash));
  }

  ItemTarget(const json& params, const std::string& method) {
    path = ResolveWindowAlias(params.value("ref", std::string()));
    id = params.value("id", ImGuiID(0));
    if (path.empty() && id == 0) {
      throw std::runtime_error(method + " needs {ref} or {id} (ids come from "
                                        "ui_items)");
    }
  }
  // Valid while this object lives — the actions hold it by value.
  ImGuiTestRef Ref() const {
    return path.empty() ? ImGuiTestRef(id) : ImGuiTestRef(path.c_str());
  }

  // A disabled widget swallows clicks silently, so a script "pressing
  // Accept" on a tool that is not ready got a success and no feature. Say so
  // instead — LogError fails the action, which becomes the RPC error.
  bool RefuseIfDisabled(ImGuiTestContext* ctx) const {
    const ImGuiTestItemInfo info = ctx->ItemInfo(Ref());
    if (info.ID != 0 && (info.ItemFlags & ImGuiItemFlags_Disabled) != 0) {
      // IM_ERRORF (not LogError) — only an engine error marks the action
      // failed, which is what turns into the client's error response.
      IM_ERRORF_NOHDR("item is disabled: %s",
                      path.empty() ? info.DebugLabel : path.c_str());
      return true;
    }
    return false;
  }
};

// One widget list as JSON: what a scripted human can see of a window.
json ItemsToJson(const ImGuiTestItemList& items) {
  json out = json::array();
  for (const ImGuiTestItemInfo& it : items) {
    // The test engine keeps a 32-byte DebugLabel, so a longer one arrives
    // CUT — including its "###id" suffix, if the suffix is what falls off.
    // A caller who pastes that back into item_click gets "unable to locate
    // item"; the numeric id below always works. Say which labels are cut
    // rather than let them look whole.
    const bool cut =
        std::strlen(it.DebugLabel) >= IM_ARRAYSIZE(it.DebugLabel) - 1;
    json e{{"label", it.DebugLabel},
           {"label_truncated", cut},
           {"id", it.ID},
           {"depth", it.Depth},
           {"disabled", (it.ItemFlags & ImGuiItemFlags_Disabled) != 0},
           // Openable = a submenu header: clicking it only opens more items,
           // so callers must address the leaf as "Parent/Leaf".
           {"submenu", (it.StatusFlags & ImGuiItemStatusFlags_Openable) != 0},
           {"x", it.RectFull.GetCenter().x},
           {"y", it.RectFull.GetCenter().y}};
    if (it.Window != nullptr)
      e["window"] = it.Window->Name;
    out.push_back(std::move(e));
  }
  return out;
}

// Clicks `target` and returns the popup it opened (a combo list, the tab
// bar's "+" menu, a right-click context menu). Fails the action when nothing
// opened, so callers can just check for null.
ImGuiWindow* OpenPopupOf(ImGuiTestContext* ctx, const ItemTarget& target,
                         int button) {
  ctx->ItemClick(target.Ref(), button);
  ImGuiWindow* popup = ctx->GetWindowByRef("//$FOCUSED");
  if (popup == nullptr)
    IM_ERRORF_NOHDR("%s", "that widget opened no popup");
  return popup;
}

// Why the last UI action failed, from the test engine's own log ("Unable to
// locate item: '###tb_box'"). Without this the client got "see app log" and
// nothing was ever written there.
std::string UiActionFailureReason(ImGuiTest* test) {
  if (test == nullptr)
    return {};
  ImGuiTextBuffer buf;
  test->Output.Log.ExtractLinesForVerboseLevels(
      ImGuiTestVerboseLevel_Error, ImGuiTestVerboseLevel_Warning, &buf);
  // The raw log is the engine's own transcript ("[0007] Error 'Unable to
  // locate item: ...'"). Keep the reasons, drop the frame stamps and the
  // bookkeeping lines, and join them into one JSON-RPC error string.
  std::string out;
  std::string line;
  std::istringstream in(buf.c_str());
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    const size_t close = line.find("] ");
    if (line.rfind("[", 0) == 0 && close != std::string::npos)
      line = line.substr(close + 2); // strip "[0007] "
    if (line.rfind("Test: ", 0) == 0 ||
        line.find("test failed.") != std::string::npos || line.empty())
      continue;
    if (line.rfind("Error ", 0) == 0)
      line = line.substr(6); // the level is already implied
    if (!out.empty())
      out += "; ";
    out += line;
  }
  return out;
}
#endif

// Snap the camera to a named standard view (optionally fitting first). Shared
// by the `view` RPC and the raw `screenshot` handler so a raw screenshot(view=)
// orients the same way the MCP wrapper does. Returns false for an unknown name.
// Directions come from conv::kNamedViews — the same table as the view.*
// commands, the view cube and the drawings' OrientationAxes.
// boxMin/boxMax (both or neither) fit the camera to that WORLD box instead
// of the whole document — the render_sheet focus path.
bool ApplyStandardView(ui::AppState& state, const std::string& name, bool fit,
                       const glm::vec3* boxMin = nullptr,
                       const glm::vec3* boxMax = nullptr) {
  const conv::NamedView* v = conv::FindView(name);
  if (v == nullptr)
    return false;
  // Fit runs first: it re-aims at the target (SetTarget), so the snap must
  // set the orientation last.
  if (fit) {
    if (boxMin != nullptr && boxMax != nullptr)
      ui::FitCameraToWorldBox(state, *boxMin, *boxMax);
    else
      ui::FitCameraToDocument(state);
  }
  state.camera->SnapToView(glm::vec3(v->toCamera), glm::vec3(v->up));
  return true;
}

// V3 render_sheet footer: the kernel digest burned under the view grid, so
// the eyeball pass and the numbers live in the same image. Reuses the
// mass_properties and contact_report handlers rather than re-deriving.
std::vector<std::string> BuildSheetFooter(ui::AppState& state, Document* doc) {
  std::vector<std::string> out;
  char buf[192];
  const nlohmann::json empty = nlohmann::json::object();
  try {
    double volume = 0.0;
    bool massKnown = false;
    double massG = 0.0;
    {
      remote::RpcParams p{empty};
      if (auto r =
              remote::HandleInspectionRpc(state, doc, "mass_properties", p)) {
        const nlohmann::json& total = r->at("total");
        volume = total.value("volume", 0.0);
        if (total.contains("mass_g") && !total.at("mass_g").is_null()) {
          massKnown = true;
          massG = total.at("mass_g").get<double>();
        }
      }
    }
    glm::vec3 mn(0.0f), mx(0.0f);
    bool haveBox = false;
    int count = 0;
    if (doc != nullptr) {
      for (const Body& b : doc->GetBodies()) {
        glm::vec3 bmn(0.0f), bmx(0.0f);
        b.shape.GetBoundingBox(bmn, bmx);
        mn = haveBox ? glm::min(mn, bmn) : bmn;
        mx = haveBox ? glm::max(mx, bmx) : bmx;
        haveBox = true;
        count++;
      }
      snprintf(buf, sizeof buf, "%d BODIES", count);
    } else if (state.assembly) {
      for (const Occurrence& o : state.assembly->GetOccurrences()) {
        if (!o.visible || state.assembly->OccurrenceSuppressed(o.id))
          continue;
        Document* pd = ui::PartDocById(state, o.partId);
        glm::vec3 bmn(0.0f), bmx(0.0f);
        if (pd == nullptr ||
            !Measure::WorldBounds(pd->GetBodies(), ui::OccurrenceMatrix(o), bmn,
                                  bmx))
          continue;
        mn = haveBox ? glm::min(mn, bmn) : bmn;
        mx = haveBox ? glm::max(mx, bmx) : bmx;
        haveBox = true;
        count++;
      }
      snprintf(buf, sizeof buf, "%d OCCURRENCES", count);
    } else {
      return out;
    }
    std::string head = buf;
    if (massKnown)
      snprintf(buf, sizeof buf, "%s   MASS %.2f KG", head.c_str(),
               massG / 1000.0);
    else
      snprintf(buf, sizeof buf, "%s   MASS UNKNOWN", head.c_str());
    out.push_back(buf);
    if (haveBox) {
      snprintf(buf, sizeof buf, "BBOX %.0f X %.0f X %.0f MM   VOL %.0f CM3",
               mx.x - mn.x, mx.y - mn.y, mx.z - mn.z, volume / 1000.0);
      out.push_back(buf);
    }
    if (doc == nullptr && state.assembly) {
      remote::RpcParams p{empty};
      if (auto r =
              remote::HandleInspectionRpc(state, doc, "contact_report", p)) {
        snprintf(buf, sizeof buf,
                 "CONTACT: %zu COMPONENTS, %zu CLASHES, %zu NEAR-MISSES",
                 r->at("components").size(), r->at("clashes").size(),
                 r->at("near_misses").size());
        out.push_back(buf);
      }
    }
  } catch (const std::exception& e) {
    out.push_back(std::string("DIGEST UNAVAILABLE: ") + e.what());
  }
  return out;
}

} // namespace

// Methods that act on the project container or the active assembly tab, not
// on an active part document. Everything else needs a live part: when an
// assembly tab is active, `doc` is null, so fail with a clear message rather
// than dereferencing null. File scope so the describe endpoint can derive
// each method's requires_part_doc from it.
static const std::set<std::string> kNoPartDoc = {
    "describe", "ping", "background_jobs", "cancel_background_job",
    "list_parts", "add_part", "add_assembly", "set_active_part", "delete_part",
    "rename_part",
    // Tab-bar folders group tabs of every kind, so they must work from an
    // assembly or drawing tab as readily as from a part.
    "create_folder", "move_to_folder", "rename_folder", "dissolve_folder",
    // Configuration RPCs target the active tab: part-doc handling above,
    // assembly handling below. Listed here so they are allowed on an
    // assembly tab (where doc is null).
    "add_configuration", "remove_configuration", "list_configurations",
    "set_configuration", "set_active_configuration", "create_drawing",
    "list_drawings", "add_drawing_view", "remove_drawing_view",
    "set_drawing_view", "set_drawing_sheet", "check_drawing", "update_drawing",
    "list_drawing_views", "export_drawing", "list_drawing_geometry",
    "add_drawing_dim", "remove_drawing_dim", "list_drawing_dims",
    "add_drawing_note", "remove_drawing_note", "list_drawing_notes",
    // CAM works on a JOB tab since v73 (docs/cam/job-document.md): a setup
    // stopped being part of the part, so `doc` is null by definition wherever
    // a CAM handler runs. It refuses for itself, naming how to make a job.
    "machine_part", "add_job_part", "remove_job_part", "list_job_parts",
    "list_machines", "save_machine", "delete_machine", "set_job_placement",
    "check_layout", "add_cam_setup", "list_cam_setups", "set_cam_setup",
    "remove_cam_setup", "add_cam_op", "cam_derive_op", "set_cam_op",
    "move_cam_op", "remove_cam_op", "cam_verify", "cam_simulate",
    "cam_toolpath", "cam_post", "new_project", "save_project", "open_project",
    "add_occurrence",
    // A fastener is placed in an assembly, where `doc` is null by definition.
    "add_fastener", "fit_fastener", "check_fasteners", "remove_occurrence",
    "list_occurrences", "set_occurrence_transform", "set_occurrence_visible",
    "set_occurrence_fixed", "rename_occurrence", "add_mate", "set_mate",
    "remove_mate", "list_mates", "add_relation", "set_relation",
    "remove_relation", "list_relations", "select_mate", "select_occurrence",
    "set_occurrence_configuration", "solve_assembly", "bom", "export_bom",
    "edit_in_context", "add_exploded_view", "remove_exploded_view",
    "list_exploded_views", "set_explode_step", "auto_explode", "set_explode",
    // export_visual works on a part OR an assembly (occurrence walk).
    "export_visual",
    // Clearing the compare_geometry overlay must work from any tab — the
    // overlay is part-scoped, but switching tabs is how you leave it behind.
    "clear_geometry_delta",
    // Aims the camera at the selected face. Faces are selectable on an
    // assembly tab too (viewport click), and the handler touches only the
    // selection, faceSelector and camera — none of which are part-scoped.
    // With nothing selected it refuses "no face selected", which is the
    // honest answer the part gate used to mask with "switch to a part tab".
    // Sat in rpc_gate_smoke's GATED_ON_PURPOSE until --offscreen provided a
    // render surface to verify on.
    "orient_to_face",
    // Nothing below is about the open document, and each was reachable only
    // from a part tab until a drawing-tab dogfooding round tripped over it.
    // The gate is a list kept apart from the handlers, so when a handler is
    // widened nothing makes the gate follow: checkpoint_create,
    // checkpoint_restore and history_restore all say in their own comments
    // that they were fixed to cover the WHOLE project, and all three stayed
    // part-gated afterwards.
    "get_settings", "set_settings", // user preferences, not the document
    "get_keymap", "set_keybinding", // and the keyboard bindings inside them
    "reset_layout",                 // the dock layout — and the recovery for
                                    // a wedged one, so gating it is backwards
    "checkpoint_create", "checkpoint_restore", "history_restore",
    "rt_build", // SyncRayTracingScene renders a part OR an assembly
    // interference runs on occurrence pairs on an assembly tab;
    // contact_report IS assembly-only (V1 connectivity gate);
    // list_elements/measure work there when given an explicit `part`;
    // mass_properties aggregates occurrence masses in world space.
    "interference", "contact_report", "compare_geometry", "query_geometry",
    "list_elements", "measure", "frame", "mass_properties", "render_sheet",
    // set_section writes the global viewport clip plane (per-frame UBO,
    // world space); `section` sums the cut area over a part's bodies or,
    // on an assembly tab, over every visible occurrence in place.
    "set_section", "section", "add_section", "list_sections", "update_section",
    "remove_section",
    // undo/redo route to the assembly's snapshot history on an assembly tab.
    "undo", "redo",
    // Project-level reads (git vault / checkpoint list act on projectPath,
    // not the active document). checkpoint_create/restore snapshot the
    // ACTIVE PART document and stay part-gated.
    "history_log", "checkpoint_list",
    // Sharing and conflicts (F7/F9) act on the project's REPOSITORY and the
    // disk check on the project DIRECTORY — none of them read the active
    // document. Gating them on a part meant every one of them failed with
    // "the active document is an assembly, not a part" on an assembly or
    // drawing tab: a 48-part project could not be pushed from its own
    // assembly tab, which is where anyone assembling it is standing.
    "branch_list", "branch_create", "branch_switch", "branch_merge",
    "conflict_list", "conflict_resolve", "merge_finish", "merge_abort",
    "remote_list", "remote_add", "remote_remove", "fetch", "pull", "push",
    "clone_project", "project_changed_on_disk",
    // Autosave acts on the project directory, and gating it on a part is the
    // shape of the bug F10 fixes.
    "autosave_now", "autosave_status",
    // Variables default to PROJECT scope — they are not a property of the tab
    // you happen to be on, and being gated here refused them on an assembly
    // or drawing tab. The handlers already ask for a part only when the
    // scope is "part", and refuse with that reason.
    "set_variable", "list_variables", "rename_variable", "delete_variable",
    // Units belong to the document, and an ASSEMBLY has its own (the BOM's
    // masses are shown there) — so these must not be gated on a part either.
    "set_units", "get_units",
    // Camera / rendering act on the viewport, not the part document, so they
    // work on an assembly tab too. render_still's RT scene sync does the
    // occurrence walk itself.
    "view", "fit_view", "screenshot", "set_render_options", "set_camera",
    "render_still",
    // frame_stats measures the LOOP, which has no opinion about which tab is
    // open — and the frames worth measuring are usually an assembly's.
    "frame_stats"};

// The methods RemoteServer dispatches itself (viewport, camera, selection,
// project tabs, UI events). describe: one row per `m ==` branch — extend
// together. `screenshot` and the UI actions are dispatched outside
// HandleSemantic but are part of the same surface.
static constexpr remote::MethodDoc kServerMethods[] = {
    {"describe",
     "This endpoint: methods (each with the parameter keys it accepts), "
     "feature/entity/constraint types, enums, and the document format "
     "version. {method} narrows to one.",
     "method"},
    {"ping",
     "Liveness check; returns {ok, app, port, mode, ui_actions, project, "
     "dirty, gpu}. `gpu` is the device that answers renders — empty under "
     "--headless, which has no renderer.",
     ""},
    {"background_jobs",
     "Every long operation running off the frame, as {jobs: [{kind, label, "
     "fraction, cancellable, elapsed_seconds}]}. `fraction` is -1 when the "
     "job cannot say. Kinds today: `cam` (an adaptive clearing pass; see "
     "cam_toolpath's `async`). Empty when nothing is running. Not the CAM "
     "job document -- that is list_job_parts.",
     ""},
    {"cancel_background_job",
     "Ask every job of {kind} to stop; returns {asked}. A job with no "
     "interruption points (a `cam` pass) finishes the piece it is on and "
     "starts nothing further; the answer says `cancellable: false` for it. "
     "Refuses a kind that is not running, naming what is.",
     "kind"},
    {"get_state",
     "Full snapshot: features (with errors), bodies (volume, bbox), "
     "selection, camera, undo/redo, rollback, regen summary.",
     ""},
    {"undo", "Undo one step (assembly snapshot history on an assembly tab).",
     ""},
    {"redo", "Redo one step.", ""},
    {"view",
     "Orient the camera to a named view {name: front|back|left|right|top|"
     "bottom|iso}.",
     "fit, name"},
    {"fit_view", "Frame the geometry in the viewport.", ""},
    {"set_camera",
     "Direct camera control {target?, eye?, direction?, up?, distance?}. "
     "NOTE: direction points from target TOWARD the camera (a view-from "
     "vector); passing eye [x,y,z] derives direction+distance instead.",
     "direction, distance, eye, target, up"},
    {"orient_to_face", "Look straight at the selected face.", ""},
    {"select_face",
     "Select a face by {mesh, face} or stable {element}/{feature_id, "
     "element}; returns normal/center.",
     "element, face, mesh"},
    {"screenshot",
     "Write the rendered viewport to {path} (needs a renderer "
     "— not available under --headless).",
     "fit, path, view"},
    {"render_sheet",
     "One PNG: tiled named views (default iso/front/right/top, up to 6 via "
     "{views}) with a numeric footer — bbox, mass, volume, and on an "
     "assembly the contact_report summary. {focus: occurrence name|id} "
     "frames that one occurrence large in every view (close-ups without "
     "camera math). Needs a renderer.",
     "focus, path, views"},
    {"render_still",
     "Path-traced still to {path} (ray tracing; needs GPU support). "
     "`backdrop`: studio (a neutral sweep, the default) or environment (the "
     "HDRI itself).",
     "backdrop, ground, height, path, samples, seed, width"},
    {"rt_build", "Build ray-tracing acceleration structures.", ""},
    {"set_render_options", "Toggle shadows/SSAO/quality options.",
     "shadow_map, shadows, ssao"},
    {"frame_stats",
     "Frame-time statistics over the last {window} frames (default 240): "
     "fps, avg/p50/p95/max ms and the update|ui|wait|record|present split. "
     "{hud: true|false} toggles the overlay (off by default; also "
     "Ctrl+Shift+F). cpu_ms vs display_ms is the read: which of "
     "wait/present holds the vsync idle is driver-dependent. Needs a "
     "renderer — --headless runs no frame loop.",
     "hud, window"},
    {"get_settings", "Read the user preferences (docs/ui/settings.md).", ""},
    {"set_settings", "Change user preferences {key: value, ...}; persists.",
     "*"},
    {"get_keymap",
     "The keyboard bindings (docs/ui/u12-keybindings.md): every command with "
     "its context, label, current chord and default, plus any conflicts. "
     "{context: \"sketch\"} narrows it. A context binding SHADOWS a global "
     "one with the same chord — that is how N is Coincident while sketching "
     "and align-to-face everywhere else.",
     "context"},
    {"set_keybinding",
     "Rebind one command: {command: \"part.fillet\", chord: \"Shift+B\"}. "
     "An empty chord unbinds it; {reset: true} restores the default. "
     "Persists. Escape/Enter/Delete/arrows are fixed and have no command.",
     "chord, command, reset"},
    {"reset_layout", "Reset the dock layout.", ""},
    {"add_part", "Add a new empty part tab.", "name"},
    {"add_assembly", "Add an assembly tab.", "name"},
    {"list_parts", "List project tabs (parts, assemblies, drawings).", ""},
    {"rename_part", "Rename a part.", "new_name, part"},
    {"menu_click", "Drive a menu item by path, e.g. \"Edit/Undo\".", "path"},
    {"menu_items",
     "List the menu bar's contents: no {path} = the top-level "
     "menus, {path: \"File\"} = that menu's items (submenu: "
     "true = walk deeper). The menu closes again.",
     "path"},
    {"key_press", "Send a key chord, e.g. \"Ctrl+Z\".", "chord"},
    {"item_click",
     "UI test tier: click a widget by {ref} or by the {id} ui_items reports "
     "(the only handle on hidden-label widgets like a combo). Ref example: "
     "\"//##DockHost/**/###tb_box\" (//window, ** = any depth, ### = the "
     "widget's stable id); {button: 1} right-clicks.",
     "button, id, ref"},
    {"item_double_click",
     "UI test tier: double-click a widget (same ref "
     "grammar as item_click).",
     "id, ref"},
    {"item_input", "UI test tier: type into a widget {ref, value}.",
     "id, ref, value"},
    {"item_hover",
     "UI test tier: hover a widget {ref|id} — shows its "
     "tooltip and hover state without clicking.",
     "id, ref"},
    {"popup_click",
     "UI test tier: open the popup of {ref|id} ({button}: 1 "
     "= context menu) and click {item} in it — the only way "
     "to reach a menu that is not the main menu bar (the tab "
     "bar's '+', context menus). {item} may be a submenu path, "
     "\"Part 1/Front\".",
     "button, id, item, ref, value"},
    {"popup_items",
     "UI test tier: list what the popup of {ref|id} holds "
     "(submenu: true = address its children as "
     "\"Parent/Child\"), then close it.",
     "button, id, ref"},
    {"item_combo",
     "UI test tier: choose {value} in the combo {ref|id} "
     "(opens and picks in one action — the popup does not "
     "survive between RPCs).",
     "button, id, item, ref, value"},
    {"ui_items",
     "UI test tier: list the widgets under {ref} (default the "
     "focused window, {depth}): label, id, window, disabled, "
     "screen centre. The GUI, self-described. Labels come from a "
     "32-byte buffer, so a long one is CUT — `label_truncated` "
     "says so, and the numeric `id` is the handle item_click "
     "always accepts.",
     "depth, ref"},
    {"scroll_panel",
     "UI test tier: scroll a docked panel {window, to: top|bottom|<y>}; "
     "returns the widget list afterwards. This is for SEEING — a screenshot "
     "shows only what is on screen, so the bottom of a long panel needs "
     "scrolling to appear in one. It is not needed to reach a widget: no "
     "panel here uses a list clipper, so ui_items lists items below the fold "
     "and item_click scrolls them into view by itself. (`scroll` is a "
     "different thing: it zooms the 3D viewport.)",
     "to, window"},
    {"focus_panel",
     "UI test tier: bring a docked panel's TAB forward {name}, e.g. \"CAM\", "
     "\"Model\", \"History\". A panel tabbed behind another is invisible to "
     "item_click, so without this the only panels a script could drive were "
     "whichever the saved layout happened to leave in front.",
     "name"},
    {"viewport_click",
     "Click the 3D viewport at (x, y) as 0-1 fractions OF THE VIEWPORT "
     "rect (0.5,0.5 = its centre, whatever is docked around it).",
     "button, ctrl"},
    {"viewport_drag", "Drag in the viewport (button 0/1/2) — orbit/pan.",
     "button, ctrl, shift"},
    {"scroll", "Scroll (zoom) the viewport.", "amount, dy, window"},
};

// Every method any module documents. The describe tables are the registry
// (a branch without a row is caught by its own test), so this needs no
// second list to drift out of step.
static bool IsKnownMethod(const std::string& m) {
  static const std::set<std::string> known = [] {
    std::set<std::string> s;
    const auto add = [&s](std::span<const remote::MethodDoc> docs) {
      for (const auto& d : docs)
        s.insert(d.name);
    };
    add(kServerMethods);
    add(remote::ModelingMethodDocs());
    add(remote::ProjectMethodDocs());
    add(remote::AssemblyMethodDocs());
    add(remote::InspectionMethodDocs());
    add(remote::CamMethodDocs());
    add(remote::DrawingMethodDocs());
    add(remote::ConfigurationMethodDocs());
    add(remote::SimulationMethodDocs());
    return s;
  }();
  return known.count(m) != 0;
}

json RemoteServer::HandleSemantic(const Request& request, ui::AppState& state,
                                  bool& handled, remote::RpcParams& p) {
  const std::string& m = request.method;
  Document* doc = state.document.get();
  handled = true;

  // UI actions (item_click, viewport_click, …) fall through to QueueUiAction,
  // not HandleSemantic, so don't guard them here — they drive the assembly tab.
  //
  // An UNKNOWN method falls through too, or the gate answers a typo with
  // "switch to a part tab" and sends the caller hunting a tab problem that
  // does not exist. Whether a method needs a part is only a sensible thing
  // to say about a method that exists.
  if (!doc && !kNoPartDoc.count(m) && !IsUiAction(m) && IsKnownMethod(m)) {
    const char* kind = state.assembly  ? "an assembly"
                       : state.drawing ? "a drawing"
                                       : "not a part";
    throw std::runtime_error("the active document is " + std::string(kind) +
                             ", not a part; switch to a part tab");
  }

  if (m == "describe") {
    // API introspection: every method with a one-line doc, the feature/entity/
    // constraint dialects, and enum values — so clients (agents, the MCP
    // wrapper) discover the surface without grepping the source. Static
    // metadata only; safe with no document.
    // `method` narrows to one entry. Three separate dogfooding agents
    // reached for describe{method: ...} and found describe took no arguments
    // at all, so the whole 195-method surface came back every time — and
    // still without saying what any of them takes.
    const std::string only = p.value("method", std::string{});
    json methods = json::array();
    const auto addDocs = [&](std::span<const remote::MethodDoc> docs) {
      for (const auto& d : docs) {
        if (!only.empty() && only != d.name)
          continue;
        json entry = {{"name", d.name},
                      {"doc", d.doc},
                      {"requires_part_doc",
                       !kNoPartDoc.count(d.name) && !IsUiAction(d.name)}};
        // "" = takes nothing; "*" = a free-form spec read opaquely, where
        // the strict unknown-key check is off and prose is the only guide.
        const std::string ps = d.params ? d.params : "";
        if (ps == "*") {
          entry["params_free_form"] = true;
        } else {
          json keys = json::array();
          for (size_t i = 0, j = 0; i <= ps.size(); i++) {
            if (i == ps.size() || ps[i] == ',') {
              std::string k = ps.substr(j, i - j);
              const size_t b = k.find_first_not_of(" ");
              const size_t e = k.find_last_not_of(" ");
              if (b != std::string::npos)
                keys.push_back(k.substr(b, e - b + 1));
              j = i + 1;
            }
          }
          entry["params"] = std::move(keys);
        }
        methods.push_back(std::move(entry));
      }
    };
    addDocs(kServerMethods);
    addDocs(remote::ModelingMethodDocs());
    addDocs(remote::ProjectMethodDocs());
    addDocs(remote::AssemblyMethodDocs());
    addDocs(remote::InspectionMethodDocs());
    addDocs(remote::CamMethodDocs());
    addDocs(remote::DrawingMethodDocs());
    addDocs(remote::ConfigurationMethodDocs());
    addDocs(remote::SimulationMethodDocs());
    if (!only.empty() && methods.empty())
      throw std::runtime_error(
          "unknown method '" + only +
          "' — call describe with no arguments for the full list");
    json featureTypes = json::array();
    for (const auto& ft : DocumentIO::FeatureTypes())
      featureTypes.push_back({{"type", ft.type},
                              {"since_version", ft.sinceVersion},
                              {"params", ft.params}});
    return json{
        {"app", "designer"},
        {"document_format_version", DocumentIO::DocumentFormatVersion()},
        {"methods", std::move(methods)},
        {"feature_types", std::move(featureTypes)},
        {"sketch_entity_types", DocumentIO::SketchEntityTypes()},
        {"constraint_kinds", remote::ConstraintKindNames()},
        {"extrude_modes", {"new_body", "add", "cut"}},
        {"extrude_end_modes",
         {"blind", "symmetric", "two_sided", "up_to_face", "through_all"}},
        {"drawing_dim_kinds", DrawingDimKindNames()},
        {"axis_kinds", {"x", "y", "z", "sketch_line", "datum_axis"}},
        {"material_slugs", CADMaterials::MaterialLibrary::AllSlugs()}};
  }

  // Doc-level-object RPCs carved into their own module (decision 7). The
  // config RPCs target whichever tab is active — part or assembly.
  if (doc) {
    if (json cfgOut; remote::HandleConfigurationRpc(state, *doc, m, p, cfgOut))
      return cfgOut;
  } else if (state.assembly) {
    if (json cfgOut; remote::HandleAssemblyConfigurationRpc(
            state, *state.assembly, m, p, cfgOut))
      return cfgOut;
  } else {
    // Neither tab kind (a drawing is active): recognize config methods so
    // they fail with guidance instead of falling through to
    // "unknown method".
    for (const auto& d : remote::ConfigurationMethodDocs()) {
      if (m == d.name) {
        throw std::runtime_error(
            m + " needs a part or assembly tab (the active tab is a "
                "drawing) — switch with set_active_part");
      }
    }
  }

  if (m == "ping") {
    // Enough for a client to say WHICH instance it just attached to, live
    // rather than from a file: the port, whether this build can screenshot,
    // the open project and whether it has unsaved changes. A client that
    // silently attaches to the wrong window is the failure this prevents.
    const std::string project =
        state.projectPath.empty() ? std::string{} : state.projectPath;
    return {{"ok", true},
            {"app", "designer"},
            {"port", m_port},
            {"mode", ModeName(m_mode)},
            {"ui_actions", UiActionsAvailable()},
            {"project", project},
            {"dirty", ui::ProjectDirty(state)},
            // Which GPU answers is the first thing worth knowing about a
            // rendering bug, and until now it existed only in the log and in
            // Help > About — both out of reach of the caller writing the
            // report. It is also what lets a pixel baseline be keyed to the
            // device that produced it (tests/render_pixel_smoke.py).
            {"gpu", state.gpuName}};
  }
  if (m == "background_jobs") {
    json jobs = json::array();
    for (const ui::BackgroundJobInfo& j : ui::ListBackgroundJobs(state))
      jobs.push_back({{"kind", j.kind},
                      {"label", j.label},
                      {"fraction", j.fraction},
                      {"cancellable", j.cancellable},
                      {"elapsed_seconds", j.elapsedSeconds}});
    return json{{"jobs", jobs}};
  }
  if (m == "cancel_background_job") {
    const std::string kind = p.at("kind").get<std::string>();
    if (!ui::BackgroundJobRunning(state, kind)) {
      std::string running;
      for (const ui::BackgroundJobInfo& j : ui::ListBackgroundJobs(state))
        running += (running.empty() ? "" : ", ") + j.kind;
      throw std::runtime_error(
          "no background job of kind '" + kind + "' is running" +
          (running.empty() ? std::string(" (nothing is)")
                           : " (running: " + running + ")"));
    }
    // The domain gets to say what cancelling means for what was queued
    // behind the running piece.
    if (kind == "cam")
      ui::CancelAdaptivePasses(state);
    const size_t asked = ui::CancelBackgroundJobs(state, kind);
    bool cancellable = false;
    for (const ui::BackgroundJobInfo& j : ui::ListBackgroundJobs(state))
      if (j.kind == kind)
        cancellable |= j.cancellable;
    return json{{"asked", asked}, {"cancellable", cancellable}};
  }
  if (m == "get_state") {
    json features = json::array();
    for (size_t i = 0; i < doc->FeatureCount(); i++) {
      json f = DocumentIO::FeatureToJson(*doc->GetFeature(i));
      f["index"] = i;
      // Regeneration errors, so remote callers see WHY a feature contributed
      // nothing instead of silently reading stale bodies.
      if (!doc->GetFeature(i)->GetError().empty()) {
        f["error"] = doc->GetFeature(i)->GetError();
        // Failure downstream of another failure — usually collateral; fix
        // the first red feature first.
        if (doc->GetFeature(i)->ErrorAfterEarlierFailure()) {
          f["after_earlier_failure"] = true;
        }
      }
      // Non-fatal advisory (e.g. a cut that intersected nothing): the feature
      // built a valid result, but likely not the intended one.
      if (!doc->GetFeature(i)->GetWarning().empty()) {
        f["warning"] = doc->GetFeature(i)->GetWarning();
      }
      // A Derived copy says whether its source has moved (v84): true/false,
      // or null when the source is not loaded — unknown is not fresh.
      if (auto* d = dynamic_cast<const DerivedFeature*>(doc->GetFeature(i))) {
        const std::optional<bool> stale = ui::DerivedStale(state, *d);
        f["stale"] = stale.has_value() ? json(*stale) : json(nullptr);
      }
      features.push_back(f);
    }
    json bodies = json::array();
    for (size_t i = 0; i < doc->GetBodies().size(); i++) {
      const auto& body = doc->GetBodies()[i];
      glm::vec3 min, max;
      body.shape.GetBoundingBox(min, max);
      json entry = {{"name", body.name},
                    {"volume", body.shape.Volume()},
                    {"material", body.material},
                    {"bbox_min", Vec3ToJson(min)},
                    {"bbox_max", Vec3ToJson(max)}};
      if (body.reference) // a Derived ghost: drawn on, weighed as nothing
        entry["reference"] = true;
      if (body.appearance.Any()) { // v54 visual override, set_appearance
        json a = json::object();
        if (body.appearance.color)
          a["color"] = Vec3ToJson(*body.appearance.color);
        if (body.appearance.roughness)
          a["roughness"] = *body.appearance.roughness;
        if (body.appearance.metallic)
          a["metallic"] = *body.appearance.metallic;
        entry["appearance"] = std::move(a);
      }
      // Render/picking mesh id (used by select_face); ids change on
      // regeneration, so always read them fresh from here
      if (i < state.documentMeshes.size()) {
        entry["mesh"] = state.documentMeshes[i].meshId;
      }
      bodies.push_back(entry);
    }
    // Legacy single-face block, now derived from the Selection (not the
    // FaceSelector mirror, which only updates when a frame runs — RPC
    // batches would read stale state).
    json selection = nullptr;
    if (const ui::SelectionItem* it =
            state.selection.First(ui::SelectionItem::Kind::Face)) {
      uint32_t mesh = 0, face = 0;
      if (state.faceSelector && ui::ResolveFaceItem(state, *it, mesh, face)) {
        selection = {
            {"mesh", mesh},
            {"face", face},
            {"normal",
             Vec3ToJson(state.faceSelector->GetFaceNormal(mesh, face))},
            {"center",
             Vec3ToJson(state.faceSelector->GetFaceCenter(mesh, face))}};
      }
    }
    // The typed Selection (U1): every item, stable refs only.
    json selectionItems = json::array();
    for (const ui::SelectionItem& it : state.selection.items) {
      const char* kind =
          it.kind == ui::SelectionItem::Kind::Face         ? "face"
          : it.kind == ui::SelectionItem::Kind::Edge       ? "edge"
          : it.kind == ui::SelectionItem::Kind::Body       ? "body"
          : it.kind == ui::SelectionItem::Kind::Feature    ? "feature"
          : it.kind == ui::SelectionItem::Kind::Occurrence ? "occurrence"
          : it.kind == ui::SelectionItem::Kind::Mate       ? "mate"
                                                           : "sketch_region";
      json entry = {{"kind", kind}};
      if (it.id != 0)
        entry["id"] = it.id;
      if (!it.element.name.empty())
        entry["element"] = {{"feature_id", it.element.featureId},
                            {"name", it.element.name}};
      selectionItems.push_back(entry);
    }
    return {{"features", features},
            {"bodies", bodies},
            {"selection", selection},
            {"selection_items", selectionItems},
            {"selected_feature", ui::SelectedFeatureIndexOrMinus1(state)},
            {"camera",
             {{"position", Vec3ToJson(state.camera->GetPosition())},
              {"target", Vec3ToJson(state.camera->GetTarget())},
              {"distance", state.camera->GetDistance()}}},
            {"can_undo", ui::CanGlobalUndo(state)},
            {"can_redo", ui::CanGlobalRedo(state)},
            {"rollback", doc->Rollback()},
            {"regen",
             [&] {
               json r = remote::RegenSummaryJson(*doc);
               r["executed"] = doc->LastRegenStats().executed;
               r["cached"] = doc->LastRegenStats().cached;
               return r;
             }()},
            {"pending_feature_id", state.pendingFeatureId}};
  }
  if (m == "rt_build") {
    // Build the ray-tracing acceleration structures from the current document.
    // Verification hook for the offline-render work (R4).
    bool ok = ui::SyncRayTracingScene(state);
    return {{"ok", ok},
            {"geometry_count",
             state.rayTracer ? state.rayTracer->GeometryCount() : 0},
            {"has_tlas", state.rayTracer && state.rayTracer->HasTLAS()}};
  }
  if (m == "render_still") {
    // Assemblies render too: SyncRayTracingScene walks every visible
    // occurrence at its solved placement. What must still fail loudly is a
    // tab with nothing in it, which used to "succeed" with a picture of the
    // environment alone — the dock round's silently-wrong papercut.
    if (state.assembly && state.assembly->OccurrenceCount() == 0)
      throw std::runtime_error(
          "this assembly has no occurrences — there is nothing to render");
    // Offline path-traced still to a PNG. Runs synchronously on the main thread
    // (ProcessPending) using the same idle-queue one-shot submits as the AS
    // build, so it needs no swapchain interaction. Parameters default to a
    // small deterministic render for smoke tests.
    uint32_t width = p.value("width", 640u);
    uint32_t height = p.value("height", 480u);
    uint32_t samples = p.value("samples", 64u);
    uint32_t seed = p.value("seed", 1u);
    std::string path = p.value("path", std::string("render.png"));
    bool ground = p.value("ground", true); // shadow-catcher plane
    // "studio" (default) shows a neutral sweep; "environment" shows the HDRI
    // itself, which is what it was before there was a choice.
    const std::string backdrop = p.value("backdrop", std::string("studio"));
    if (backdrop != "studio" && backdrop != "environment") {
      throw std::runtime_error("unknown backdrop: " + backdrop +
                               "; backdrops: studio, environment");
    }
    std::string error, warning;
    bool ok =
        ui::RenderStillBlocking(state, width, height, samples, path, seed,
                                ground, &error, &warning, backdrop == "studio");
    json result = {{"ok", ok},         {"path", path},       {"width", width},
                   {"height", height}, {"samples", samples}, {"error", error}};
    // The postcondition a screenshot already carries: saying "ok" about a
    // picture of nothing is the confident wrong answer this codebase keeps
    // paying for.
    if (ok && !warning.empty())
      result["warnings"] = json::array({warning});
    return result;
  }
  // Part-document domain modules (disjoint method sets; order doesn't matter).
  if (doc) {
    if (auto r = remote::HandleModelingRpc(state, *doc, m, p))
      return *r;
    if (auto r = remote::HandleSimulationRpc(state, *doc, m, p))
      return *r;
  }
  // Inspection takes a nullable doc: interference works on assembly
  // occurrence pairs, list_elements/measure on any part via `part`.
  if (auto r = remote::HandleCamRpc(state, doc, m, p))
    return *r;
  if (auto r = remote::HandleInspectionRpc(state, doc, m, p))
    return *r;
  if (m == "undo") {
    // Global undo (Phase V): the active tab's stack OR the cross-tab
    // project-variable stack, whichever was most recently mutated.
    //
    // Undo regenerates, so it can leave a feature broken — stepping back past
    // the edit that gave a fillet an edge to bite on, say. It used to report
    // only `can_undo`, so a script learned nothing about the state it had just
    // restored. `state.document` is re-read AFTER the undo: a tab switch is
    // undoable, so the active document may not be the one captured above.
    ui::GlobalUndo(state);
    json out = {{"can_undo", ui::CanGlobalUndo(state)}};
    remote::AttachRegen(out, state.document.get());
    return out;
  }
  if (m == "redo") {
    ui::GlobalRedo(state);
    json out = {{"can_redo", ui::CanGlobalRedo(state)}};
    remote::AttachRegen(out, state.document.get());
    return out;
  }
  if (m == "list_parts") {
    // Multi-document project (assemblies A1/A2): the tabs and which is active.
    ui::EnsureParts(state);
    json arr = json::array();
    for (size_t i = 0; i < state.parts.size(); i++) {
      const char* type =
          state.parts[i].kind == ui::DocKind::Assembly  ? "assembly"
          : state.parts[i].kind == ui::DocKind::Drawing ? "drawing"
                                                        : "part";
      json entry = {{"name", state.parts[i].name},
                    {"index", i},
                    {"id", state.parts[i].id},
                    {"type", type}};
      if (!state.parts[i].folder.empty())
        entry["folder"] = state.parts[i].folder;
      arr.push_back(std::move(entry));
    }
    json out = {{"parts", arr}, {"active", state.activePart}};
    if (!state.tabFolders.empty()) // tab-bar folder registry (may hold empty)
      out["folders"] = state.tabFolders;
    return out;
  }
  if (m == "add_part") {
    // A duplicate name is refused (names address tabs); the DEFAULT skips
    // taken names, since "Part parts.size()+1" collided after a deletion.
    std::string name;
    if (p.contains("name")) {
      name = p.at("name").get<std::string>();
      remote::RequireFreshTabName(state, name);
    } else {
      name = remote::FreshDefaultTabName(state, "Part");
    }
    const size_t idx = ui::AddPart(state, name);
    return {{"index", idx}, {"name", name}, {"id", state.parts[idx].id}};
  }
  if (m == "add_assembly") {
    std::string name;
    if (p.contains("name")) {
      name = p.at("name").get<std::string>();
      remote::RequireFreshTabName(state, name);
    } else {
      name = remote::FreshDefaultTabName(state, "Assembly");
    }
    const size_t idx = ui::AddAssembly(state, name);
    return {{"index", idx}, {"name", name}, {"id", state.parts[idx].id}};
  }
  if (m == "rename_part") {
    // Rename an existing tab (part/assembly/drawing) so the default "Part 1"
    // isn't permanent — it feeds BOM + add_occurrence. Occurrences reference
    // parts by stable id, so a name change never breaks a reference. `part`
    // resolves by name, stable id, or slot index. params: {part, new_name}
    const size_t slot = remote::ResolveSlotIndex(state, p.at("part"));
    const std::string newName = p.at("new_name").get<std::string>();
    if (newName.empty())
      throw std::runtime_error("new_name must be non-empty");
    if (state.parts[slot].name != newName) // renaming to itself is a no-op
      remote::RequireFreshTabName(state, newName);
    state.parts[slot].name = newName;
    state.projectMetaDirty = true; // tab names live in project.json only
    return {{"index", slot}, {"name", newName}, {"id", state.parts[slot].id}};
  }
  if (auto r = remote::HandleDrawingRpc(state, m, p))
    return *r;
  if (auto r = remote::HandleAssemblyRpc(state, m, p))
    return *r;
  if (auto r = remote::HandleProjectRpc(state, m, p))
    return *r;
  if (m == "fit_view") {
    ui::FitCameraToDocument(state);
    return json::object();
  }
  if (m == "view") {
    // Standard view, snapped instantly (no animation) so the very next
    // screenshot captures the final framing.
    // params: {name: front|back|left|right|top|bottom|iso, fit?: true}
    const std::string name = p.at("name").get<std::string>();
    if (!ApplyStandardView(state, name, p.value("fit", true)))
      throw std::runtime_error(
          "unknown view: " + name +
          "; views: front, back, left, right, top, bottom, iso");
    return {{"position", Vec3ToJson(state.camera->GetPosition())},
            {"target", Vec3ToJson(state.camera->GetTarget())},
            {"distance", state.camera->GetDistance()}};
  }
  if (m == "frame_stats") {
    // The door that matters for agents and CI: a HUD nobody is looking at
    // proves nothing (docs/rendering/frame-timing.md).
    if (p.contains("hud"))
      state.perfHudOpen = p.at("hud").get<bool>();
    const size_t window = p.value("window", render::FrameTimer::kHistory);
    const render::FrameTimer::Stats fs = state.frameTimer.Snapshot(window);
    if (fs.frames == 0)
      throw std::runtime_error(
          m_mode == RemoteMode::Gui
              ? "no frames measured yet (the window may be minimised)"
          : m_mode == RemoteMode::Offscreen
              ? "no frame loop in --offscreen (it renders on demand); frame "
                "timing exists only in a windowed instance"
              : "no frame loop under --headless: run the GUI (or "
                "tools/headless-preview.sh --gui) to measure frame time");
    json phases = json::object();
    json worstPhases = json::object();
    for (size_t i = 0; i < render::kPhaseCount; i++) {
      const char* name = render::PhaseName(static_cast<render::FramePhase>(i));
      phases[name] = fs.phaseMs[i];
      worstPhases[name] = fs.worstPhaseMs[i];
    }
    // The conclusion, not just the ingredients: which of wait/present holds
    // the vsync idle is a driver detail, so a caller reading one of them
    // alone would draw the wrong one.
    const double display =
        fs.phaseMs[static_cast<size_t>(render::FramePhase::Wait)] +
        fs.phaseMs[static_cast<size_t>(render::FramePhase::Present)];
    return {{"frames", fs.frames},
            {"total_frames", state.frameTimer.TotalFrames()},
            {"fps", fs.fps},
            {"avg_ms", fs.avgMs},
            {"p50_ms", fs.p50Ms},
            {"p95_ms", fs.p95Ms},
            {"max_ms", fs.maxMs},
            {"phases_ms", phases},
            {"cpu_ms", fs.avgMs - display},
            {"display_ms", display},
            // Iterations the idle throttle paced (docs/rendering/
            // idle-throttle.md): a low fps with a high idle share is the
            // app resting, not the app slow.
            {"idle_frames", fs.idleFrames},
            // Who last held/restored full rate, and how long ago — "idle 0%
            // while hands-off" is undiagnosable without the waker's name.
            {"wake", state.pacerWake},
            {"wake_age_s", state.pacerWakeAgeS},
            // The worst frame's own phase split (its total is max_ms): which
            // phase held the hitch the averages dilute away.
            {"worst_phases_ms", worstPhases},
            {"worst_idle", fs.worstIdle},
            // Which PASS the GPU spends its time on — the number the CPU
            // split cannot see. supported:false names the reason (a queue
            // with no timestamp support) instead of omitting the key.
            {"gpu",
             {{"supported", state.gpuStats.supported},
              {"samples", state.gpuStats.samples},
              {"shadow_ms", state.gpuStats.shadowMs},
              {"ssao_ms", state.gpuStats.ssaoMs},
              {"main_ms", state.gpuStats.mainMs},
              {"total_ms", state.gpuStats.totalMs}}},
            {"hud", state.perfHudOpen}};
  }
  if (m == "get_settings") {
    // The whole struct, not one method per setting: the assistant and the
    // human stay on the same interface, and every setting is testable
    // headlessly.
    json j = settings::ToJson(state.settings);
    j["path"] = settings::ConfigPath();
    j["devices"] = state.gpuDevices; // what gpu_device may be set to
    return j;
  }
  if (m == "set_settings") {
    // Validates first: a rejected value leaves nothing half-applied.
    std::string error;
    if (!settings::ApplyJson(state.settings, p.raw(), &error))
      throw std::runtime_error(error);
    ui::ApplySettings(state);
    // Persist NOW rather than arming the window's debounce: that debounce is
    // pumped by the GUI main loop, which a --headless instance does not run,
    // so a scripted change would apply and then never reach the file.
    if (!settings::Save(state.settings, state.settingsSaved, &error))
      throw std::runtime_error("could not save settings: " + error);
    state.settingsSaved = state.settings;
    state.settingsDirtyAt = -1.0;
    return settings::ToJson(state.settings);
  }
  if (m == "get_keymap") {
    const keymap::Keymap& keys = state.settings.keys;
    const bool filtered = p.contains("context");
    std::string want;
    if (filtered) {
      want = p.at("context").get<std::string>();
      bool known = false;
      for (keymap::Context c : keymap::AllContexts())
        known = known || want == keymap::ContextName(c);
      if (!known)
        throw std::runtime_error("unknown context: " + want);
    }
    json bindings = json::array();
    for (const keymap::Binding& b : keymap::Defaults()) {
      if (filtered && want != keymap::ContextName(b.ctx))
        continue;
      json row{{"id", b.id},
               {"context", keymap::ContextName(b.ctx)},
               {"section", b.section},
               {"label", b.label},
               {"chord", keys.Text(b.id)},
               {"default", b.chord},
               {"custom", !keys.IsDefault(b.id)}};
      if (const keymap::Chord alt = keys.Alt(b.id))
        row["alt"] = keymap::FormatChord(alt);
      if (b.hidden)
        row["hidden"] = true;
      bindings.push_back(row);
    }
    json conflicts = json::array();
    for (const keymap::Conflict& c : keys.Conflicts())
      conflicts.push_back({{"context", keymap::ContextName(c.ctx)},
                           {"chord", keymap::FormatChord(c.chord)},
                           {"commands", json::array({c.a, c.b})}});
    json contexts = json::array();
    for (keymap::Context c : keymap::AllContexts())
      contexts.push_back(keymap::ContextName(c));
    return json{{"contexts", contexts},
                {"bindings", bindings},
                {"active", keymap::ContextName(ui::ActiveContext(state))},
                {"conflicts", conflicts}};
  }
  if (m == "set_keybinding") {
    const std::string command = p.at("command").get<std::string>();
    if (keymap::Find(command) == nullptr)
      throw std::runtime_error(
          "unknown command: " + command +
          " (get_keymap lists them; Escape/Enter/Delete are fixed verbs, not "
          "commands)");
    keymap::Keymap& keys = state.settings.keys;
    if (p.contains("reset") && p.at("reset").get<bool>()) {
      keys.Reset(command);
    } else {
      // An absent `chord` is not the same as an empty one: empty UNBINDS on
      // purpose, absent is a caller who forgot which they meant.
      if (!p.contains("chord"))
        throw std::runtime_error(
            "set_keybinding needs a 'chord' (\"\" unbinds) or {reset: true}");
      std::string error;
      if (!keys.Set(command, p.at("chord").get<std::string>(), &error))
        throw std::runtime_error(error);
    }
    // Persist NOW rather than arming the window's debounce, for the same
    // reason set_settings does: a --headless instance runs no GUI main loop.
    std::string error;
    if (!settings::Save(state.settings, state.settingsSaved, &error))
      throw std::runtime_error("could not save settings: " + error);
    state.settingsSaved = state.settings;
    return json{{"command", command},
                {"chord", keys.Text(command)},
                {"custom", !keys.IsDefault(command)}};
  }
  if (m == "set_render_options") {
    // Viewport render quality (S3): shadows on/off, SSAO off|half|full,
    // shadow-map size. Session state; the main loop applies target
    // recreation. With no params this just reads the current state.
    if (!state.meshRenderer)
      throw std::runtime_error(
          "render options need a renderer — unavailable in --headless");
    if (p.contains("shadows"))
      state.showShadows = p.at("shadows").get<bool>();
    if (p.contains("shadow_map")) {
      const int size = p.at("shadow_map").get<int>();
      if (size != 1024 && size != 2048)
        throw std::runtime_error("shadow_map must be 1024 or 2048");
      state.shadowMapSize = size;
    }
    if (p.contains("ssao")) {
      const std::string q = p.at("ssao").get<std::string>();
      if (q == "off") {
        state.showAmbientOcclusion = false;
      } else if (q == "half" || q == "full") {
        state.showAmbientOcclusion = true;
        state.ssaoFullRes = (q == "full");
      } else {
        throw std::runtime_error("ssao must be off, half or full");
      }
    }
    return {{"shadows", state.showShadows},
            {"ssao", !state.showAmbientOcclusion
                         ? "off"
                         : (state.ssaoFullRes ? "full" : "half")},
            {"shadow_map", state.shadowMapSize}};
  }
  if (m == "select_face") {
    if (!state.faceSelector || !state.meshRenderer)
      throw std::runtime_error(
          "face selection needs render meshes — unavailable in --headless");
    uint32_t mesh = 0, face = 0;
    if (p.contains("element")) {
      // Stable element name (list_elements), e.g. {"element": "2/g4"} — no
      // volatile mesh ids needed. Names are unique per document (prefixed by
      // the creating feature), so the body scan is unambiguous.
      if (!state.document)
        throw std::runtime_error("no document");
      const std::string el = p.at("element").get<std::string>();
      const auto& bodies = state.document->GetBodies();
      bool found = false;
      for (size_t i = 0; i < bodies.size() && !found; ++i) {
        for (const auto& [idx, name] : bodies[i].elementMap.names) {
          if (name != el)
            continue;
          if (i >= state.documentMeshes.size())
            throw std::runtime_error("body has no render mesh yet");
          mesh = state.documentMeshes[i].meshId;
          face = static_cast<uint32_t>(idx);
          found = true;
          break;
        }
      }
      if (!found)
        throw std::runtime_error("no face named '" + el +
                                 "' (see list_elements)");
    } else {
      mesh = p.at("mesh").get<uint32_t>();
      face = p.at("face").get<uint32_t>();
    }
    uint32_t first = 0, count = 0;
    if (!state.faceSelector->GetFaceVertexRange(mesh, face, first, count))
      throw std::runtime_error("no such face");
    // Drive the Selection (U1b) — renderer highlights are its projection.
    // The mirror into FaceSelector happens in SyncSelectionHighlights; a
    // direct write here would be cleared next frame.
    {
      const auto ref = ui::FaceElementRef(state, mesh, face);
      if (!ref)
        throw std::runtime_error("face has no stable name");
      state.selection.Replace({ui::SelectionItem::Kind::Face, 0, *ref});
    }
    return {
        {"normal", Vec3ToJson(state.faceSelector->GetFaceNormal(mesh, face))},
        {"center", Vec3ToJson(state.faceSelector->GetFaceCenter(mesh, face))}};
  }
  if (m == "set_camera") {
    // Direct camera control (the view/fit RPCs only snap to standard views):
    // params: {target?: [x,y,z], direction?: [x,y,z] (unit offset from target
    // toward the camera), up?: [x,y,z], distance?: mm}. Applied in that
    // order; each field optional. Works headless (camera state only).
    if (!state.camera)
      throw std::runtime_error("no camera");
    Camera& cam = *state.camera;
    auto vec3 = [](const json& a) {
      return glm::vec3(a.at(0).get<float>(), a.at(1).get<float>(),
                       a.at(2).get<float>());
    };
    if (p.contains("target"))
      cam.SetTarget(vec3(p["target"]));
    if (p.contains("eye")) {
      // V5: an explicit camera position — the intuitive form. Derives the
      // (target -> camera) direction and the distance, so it composes with
      // an earlier `target` in the same call.
      const glm::vec3 eye = vec3(p["eye"]);
      const glm::vec3 offset = eye - cam.GetTarget();
      if (glm::length(offset) < 1e-6f)
        throw std::runtime_error("eye must not equal the target");
      const glm::vec3 up = p.contains("up") ? vec3(p["up"]) : conv::kWorldUpF;
      cam.SnapToView(glm::normalize(offset), up);
      cam.SetDistance(glm::length(offset));
    }
    if (p.contains("direction")) {
      const glm::vec3 dir = vec3(p["direction"]);
      if (glm::length(dir) < 1e-9f)
        throw std::runtime_error("direction must be non-zero");
      const glm::vec3 up = p.contains("up") ? vec3(p["up"]) : conv::kWorldUpF;
      cam.SnapToView(glm::normalize(dir), up);
    }
    if (p.contains("distance")) {
      const float d = p.at("distance").get<float>();
      if (d <= 0.0f)
        throw std::runtime_error("distance must be positive");
      cam.SetMaxDistance(std::max(20.0f * d, 1000.0f));
      cam.SetDistance(d);
    }
    return {{"position", Vec3ToJson(cam.GetPosition())},
            {"target", Vec3ToJson(cam.GetTarget())},
            {"distance", cam.GetDistance()}};
  }
  if (m == "reset_layout") {
    // Rebuild the default dock layout (panels re-docked to their edges) — the
    // recovery for a wedged imgui.ini state, without restarting the app.
    state.buildDefaultDockLayout = true;
    return json::object();
  }
  if (m == "orient_to_face") {
    if (!state.faceSelector)
      throw std::runtime_error(
          "face selection needs render meshes — unavailable in --headless");
    const ui::SelectionItem* it =
        state.selection.First(ui::SelectionItem::Kind::Face);
    uint32_t mesh = 0, face = 0;
    if (it == nullptr || !ui::ResolveFaceItem(state, *it, mesh, face))
      throw std::runtime_error("no face selected");
    state.camera->AlignToFace(state.faceSelector->GetFaceCenter(mesh, face),
                              state.faceSelector->GetFaceNormal(mesh, face));
    return json::object();
  }

  handled = false;
  return json();
}

bool RemoteServer::IsUiAction(const std::string& method) {
  static const char* kActions[] = {
      "menu_click", "item_click",     "item_double_click", "item_input",
      "item_hover", "item_combo",     "popup_click",       "popup_items",
      "ui_items",   "viewport_click", "viewport_drag",     "key_press",
      "menu_items", "scroll",         "focus_panel",       "scroll_panel"};
  for (const char* a : kActions)
    if (method == a)
      return true;
  return false;
}

bool RemoteServer::QueueUiAction(const Request& request) {
#ifndef HAVE_TEST_ENGINE
  // No test engine in this build: nothing here can run, and the
  // caller has already refused every ui_action with that reason.
  (void)request;
  return false;
#else
  const std::string& m = request.method;
  const json p = request.params; // copy: captured by the action lambda

  // Required params are extracted HERE, on the RPC thread, where a missing
  // key throws into ProcessPending's catch and becomes an error response. A
  // p.at() inside an action lambda would throw in the test-engine coroutine
  // instead, escape it, and std::terminate the whole app.
  //
  // `need` because these come off a RAW json (the lambdas capture it by
  // value), which skips RpcParams' caller-grade message: a missing `chord`
  // used to answer "[json.exception.out_of_range.403] key 'chord' not
  // found". `alias` names the key someone plausibly reached for instead,
  // since "missing 'chord'" is not much help to whoever just typed `key`.
  const auto need = [&m](const json& obj, const char* key,
                         const char* alias = nullptr) -> const json& {
    if (obj.contains(key))
      return obj.at(key);
    std::string msg =
        "missing required parameter '" + std::string(key) + "' for " + m;
    if (alias != nullptr && obj.contains(alias))
      msg += " — you passed '" + std::string(alias) + "'";
    throw std::runtime_error(msg);
  };
  if (m == "menu_click") {
    const std::string path = need(p, "path", "menu").get<std::string>();
    m_uiAction = [path](ImGuiTestContext* ctx) {
      ctx->SetRef("//##MainMenuBar");
      ctx->MenuClick(path.c_str());
    };
  } else if (m == "item_click") {
    const ItemTarget target(p, m);           // rpc-params: ref, id
    const int button = p.value("button", 0); // 1 = right-click (context menus)
    m_uiAction = [target, button](ImGuiTestContext* ctx) {
      if (target.RefuseIfDisabled(ctx))
        return;
      ctx->ItemClick(target.Ref(), button);
    };
  } else if (m == "item_double_click") {
    const ItemTarget target(p, m); // rpc-params: ref, id
    m_uiAction = [target](ImGuiTestContext* ctx) {
      if (target.RefuseIfDisabled(ctx))
        return;
      ctx->ItemDoubleClick(target.Ref());
    };
  } else if (m == "item_input") {
    const ItemTarget target(p, m); // rpc-params: ref, id
    const json value = need(p, "value", "text");
    if (!value.is_number() && !value.is_string())
      throw std::runtime_error("item_input value must be a number or string");
    m_uiAction = [target, value](ImGuiTestContext* ctx) {
      if (target.RefuseIfDisabled(ctx))
        return;
      if (value.is_number()) {
        ctx->ItemInputValue(target.Ref(), value.get<float>());
      } else {
        ctx->ItemInputValue(target.Ref(), value.get<std::string>().c_str());
      }
    };
  } else if (m == "item_hover") {
    const ItemTarget target(p, m); // rpc-params: ref, id
    m_uiAction = [target](ImGuiTestContext* ctx) {
      ctx->MouseMove(target.Ref());
    };
  } else if (m == "item_combo" || m == "popup_click") {
    const ItemTarget target(p, m); // rpc-params: ref, id
    // item_combo names the entry `value`; popup_click (menus, context menus)
    // names it `item`. Same motion either way.
    const std::string entry = m == "item_combo"
                                  ? need(p, "value").get<std::string>()
                                  : need(p, "item", "value").get<std::string>();
    const int button = p.value("button", 0); // 1 = a right-click menu
    // Open + choose in ONE action: a queued action ends with its own frames
    // and the popup closes in between, so two RPCs can never do this.
    m_uiAction = [target, entry, button](ImGuiTestContext* ctx) {
      ImGuiWindow* popup = OpenPopupOf(ctx, target, button);
      if (popup == nullptr)
        return;
      const std::string root = std::string("//") + popup->Name + "/";
      if (entry.find('/') != std::string::npos) {
        // A path walks submenus ("Part 1/Front"): MenuClick knows how to
        // hover a submenu open without cutting the corner and closing it.
        ctx->MenuClick((root + entry).c_str());
      } else {
        ctx->ItemClick((root + "**/" + entry).c_str());
      }
    };
  } else if (m == "menu_items") {
    // The menu bar is the one surface ui_items/popup_items cannot describe:
    // its entries only exist while the menu is open, and opening one needs a
    // path you were supposed to know already. {path} omitted lists the top
    // level; "File" lists that menu; "File/Recent" walks into a submenu.
    const std::string path = p.value("path", std::string());
    m_uiAction = [this, path](ImGuiTestContext* ctx) {
      if (path.empty()) {
        // GatherItems cannot see menu-bar buttons (they live in an internal
        // child it does not walk), so the bar reports its own names —
        // recorded by the code that draws them, in AppState::menuBarTop.
        json out = json::array();
        if (m_state == nullptr) {
          IM_ERRORF_NOHDR("%s", "no app state yet");
          return;
        }
        for (const std::string& name : m_state->menuBarTop)
          out.push_back(json{{"label", name}, {"submenu", true}});
        m_uiActionResult = json{{"menu", ""}, {"items", std::move(out)}};
        return;
      }
      ctx->MenuClick(("//##MainMenuBar/" + path).c_str()); // opens it
      ImGuiWindow* popup = ctx->GetWindowByRef("//$FOCUSED");
      if (popup == nullptr) {
        IM_ERRORF_NOHDR("%s", "that menu path opened nothing");
        return;
      }
      ImGuiTestItemList items;
      ctx->GatherItems(&items, (std::string("//") + popup->Name).c_str());
      m_uiActionResult = json{{"menu", path}, {"items", ItemsToJson(items)}};
      ctx->KeyPress(ImGuiKey_Escape); // close it again
    };
  } else if (m == "popup_items") {
    const ItemTarget target(p, m); // rpc-params: ref, id
    const int button = p.value("button", 0);
    m_uiAction = [this, target, button](ImGuiTestContext* ctx) {
      ImGuiWindow* popup = OpenPopupOf(ctx, target, button);
      if (popup == nullptr)
        return;
      ImGuiTestItemList items;
      ctx->GatherItems(&items, (std::string("//") + popup->Name).c_str());
      m_uiActionResult =
          json{{"popup", popup->Name}, {"items", ItemsToJson(items)}};
      // Close it directly. Neither input gesture works: Escape falls through
      // to the app and cancels the tool session the popup belongs to (listing
      // a Loft's sections closed the Loft), and clicking the opener again
      // cannot reach it — a combo popup is drawn OVER its own opener, so the
      // click lands on the popup.
      if (ctx->UiContext->OpenPopupStack.Size > 0)
        ImGui::ClosePopupToLevel(0, true);
    };
  } else if (m == "scroll_panel") {
    // For screenshots, not for reach. Round 2 of the CAM dogfooding reported
    // that a panel's lower half was unreachable; it was not — nothing here
    // uses ImGuiListClipper, so every item is submitted whether or not it is
    // on screen, and the test engine scrolls to an item before clicking it.
    // What a script genuinely cannot do without this is LOOK at the bottom of
    // a panel, because a capture shows the pixels that exist.
    const std::string window = p.at("window").get<std::string>();
    const json to = p.value("to", json("bottom"));
    m_uiAction = [this, window, to](ImGuiTestContext* ctx) {
      const std::string ref =
          window.rfind("//", 0) == 0 ? window : "//" + window;
      if (to.is_number()) {
        ctx->ScrollToY(ref.c_str(), to.get<float>());
      } else {
        const std::string where = to.get<std::string>();
        if (where == "top")
          ctx->ScrollToTop(ref.c_str());
        else
          ctx->ScrollToBottom(ref.c_str());
      }
      ctx->Yield(2);
      ImGuiTestItemList items;
      ctx->GatherItems(&items, ref.c_str());
      m_uiActionResult =
          json{{"window", window}, {"items", ItemsToJson(items)}};
    };
  } else if (m == "focus_panel") {
    // Focusing a docked window takes effect on the NEXT frame, and a tab
    // sharing its node with several others needs the request repeated before
    // it wins (the same thing the History panel needed). So this sets a
    // request the dock host re-applies, and waits for the tab to arrive.
    const std::string name = p.at("name").get<std::string>();
    m_uiAction = [this, name](ImGuiTestContext* ctx) {
      if (m_state != nullptr) {
        m_state->focusPanel = name;
        m_state->focusPanelFrames = 60;
      }
      ctx->Yield(8); // let the request take, so the next RPC can address it
      m_uiActionResult = json{{"name", name}};
    };
  } else if (m == "ui_items") {
    const std::string ref = p.value("ref", "//$FOCUSED");
    const int depth = p.value("depth", -1);
    m_uiAction = [this, ref, depth](ImGuiTestContext* ctx) {
      ImGuiTestItemList items;
      ctx->GatherItems(&items, ref.c_str(), depth);
      m_uiActionResult = json{{"items", ItemsToJson(items)}};
    };
  } else if (m == "viewport_click") {
    m_uiAction = [p](ImGuiTestContext* ctx) {
      const bool ctrl = p.value("ctrl", false);
      ctx->MouseMoveToPos(FractionToPos(p));
      if (ctrl)
        ctx->KeyDown(ImGuiMod_Ctrl);
      ctx->MouseClick(p.value("button", 0));
      if (ctrl)
        ctx->KeyUp(ImGuiMod_Ctrl);
    };
  } else if (m == "viewport_drag") {
    m_uiAction = [p](ImGuiTestContext* ctx) {
      const ImVec2 from = FractionToPos(p, "x0", "y0", 0.5f, 0.5f);
      const ImVec2 to = FractionToPos(p, "x1", "y1", 0.6f, 0.6f);
      const int button = p.value("button", ImGuiMouseButton_Middle);
      const bool ctrl = p.value("ctrl", false);
      const bool shift = p.value("shift", false);
      ctx->MouseMoveToPos(from);
      if (ctrl)
        ctx->KeyDown(ImGuiMod_Ctrl);
      if (shift)
        ctx->KeyDown(ImGuiMod_Shift);
      ctx->MouseDown(button);
      ctx->MouseMoveToPos(to);
      ctx->MouseUp(button);
      if (shift)
        ctx->KeyUp(ImGuiMod_Shift);
      if (ctrl)
        ctx->KeyUp(ImGuiMod_Ctrl);
    };
  } else if (m == "key_press") {
    std::string error;
    ImGuiKeyChord chord =
        ParseChord(need(p, "chord", "key").get<std::string>(), &error);
    if (chord == 0) {
      // Returning false here made the dispatcher answer "unknown method:
      // key_press" — the chord is what was unknown, and it said so.
      throw std::runtime_error(error.empty() ? "could not parse the key chord"
                                             : error);
    }
    m_uiAction = [chord](ImGuiTestContext* ctx) { ctx->KeyPress(chord); };
  } else if (m == "scroll") {
    m_uiAction = [p](ImGuiTestContext* ctx) {
      if (p.contains("window")) {
        // V5: scroll a PANEL by {window, dy} pixels (dy > 0 = down) — the
        // remote surface had no way to reach a panel's lower sections.
        ImGuiWindow* w =
            ImGui::FindWindowByName(p.at("window").get<std::string>().c_str());
        IM_CHECK_SILENT(w != nullptr);
        ImGui::SetScrollY(
            w, ImClamp(w->Scroll.y + static_cast<float>(p.value("dy", 0.0)),
                       0.0f, w->ScrollMax.y));
        return;
      }
      ctx->MouseMoveToPos(FractionToPos(p));
      ctx->MouseWheelY(p.value("amount", 1.0f));
    };
  } else {
    return false;
  }

  m_uiActionId = request.id;
  m_uiActionClient = request.client;
  m_uiActionResult = json::object();
  m_uiActionInFlight = true;
#ifdef HAVE_TEST_ENGINE
  ImGuiTestEngine_QueueTest(m_engine, s_UiActionTest);
#endif
  return true;
#endif
}

size_t RemoteServer::ProcessPending(ui::AppState& state) {
  if (!m_running)
    return 0;
  // Remote activity this tick, for the idle pacer: requests executed, plus
  // one while an async job still spans frames (a ui action driven by the
  // test engine, a staged screenshot, a sheet render) — those need the loop
  // at full rate to make progress.
  size_t activity = 0;

  m_state = &state; // UI actions read it from the engine's coroutine

  // Fractional viewport coordinates are resolved against the 3D viewport's
  // rect, so snapshot it while we are on the main thread with `state` in hand.
#ifdef HAVE_TEST_ENGINE
  s_ViewportPos = ImVec2(state.viewportPos.x, state.viewportPos.y);
  s_ViewportSize = ImVec2(state.viewportSize.x, state.viewportSize.y);
#endif

  // Store a finished background job (a solve, a mesh, an adaptive pass…)
  // even if the client never polls for it — this is the RPC pump's own
  // main-thread join point, and it applies before it answers.
  ui::ReapBackgroundJobs(state);

  // Finish an in-flight UI action once the engine is done with it
#ifdef HAVE_TEST_ENGINE
  if (m_uiActionInFlight && ImGuiTestEngine_IsTestQueueEmpty(m_engine)) {
    m_uiActionInFlight = false;
    m_uiAction = nullptr;
    if (s_UiActionTest->Output.Status == ImGuiTestStatus_Success) {
      Respond(m_uiActionClient, m_uiActionId, m_uiActionResult);
    } else {
      const std::string why = UiActionFailureReason(s_UiActionTest);
      LOG_ERROR("Remote UI action failed: {}",
                why.empty() ? "no reason logged" : why);
      RespondError(m_uiActionClient, m_uiActionId,
                   why.empty() ? "ui action failed"
                               : "ui action failed: " + why);
    }
  }
#endif

  // V3 sheet pump: one captured view per frame. Runs before the request
  // loop so the camera write for the next tile lands ahead of this frame's
  // scene update, and requests stay queued while a job is active.
  state.sheetCapture = m_sheet.active; // hide viewport chrome in the tiles
  if (m_sheet.active && !m_screenshotPending) {
    if (m_sheet.nextView < m_sheet.views.size()) {
      if (!ApplyStandardView(state, m_sheet.views[m_sheet.nextView], true,
                             m_sheet.hasFocus ? &m_sheet.focusMin : nullptr,
                             m_sheet.hasFocus ? &m_sheet.focusMax : nullptr)) {
        RespondError(m_sheet.client, m_sheet.id,
                     "unknown view: " + m_sheet.views[m_sheet.nextView] +
                         "; views: front, back, left, right, top, bottom, iso");
        for (const std::string& tile : m_sheet.tilePaths)
          std::filesystem::remove(tile);
        m_sheet = SheetJob{};
      } else {
        m_screenshotPath = m_sheet.tilePaths[m_sheet.nextView];
        m_screenshotPending = true;
        m_screenshotHeldFrames = 0;
        m_sheet.nextView++;
      }
    } else {
      rendersheet::Spec spec;
      spec.tilePaths = m_sheet.tilePaths;
      spec.labels = m_sheet.views;
      spec.footer = m_sheet.footer;
      spec.cropX = m_sheet.cropX;
      spec.cropY = m_sheet.cropY;
      spec.cropW = m_sheet.cropW;
      spec.cropH = m_sheet.cropH;
      std::string err;
      if (rendersheet::Composite(spec, m_sheet.outPath, &err)) {
        json resp = {{"path", m_sheet.outPath},
                     {"views", m_sheet.views},
                     {"footer", m_sheet.footer}};
        if (!m_sheet.warnings.empty())
          resp["warnings"] = m_sheet.warnings;
        Respond(m_sheet.client, m_sheet.id, resp);
      } else {
        RespondError(m_sheet.client, m_sheet.id, err);
      }
      for (const std::string& tile : m_sheet.tilePaths)
        std::filesystem::remove(tile);
      m_sheet = SheetJob{};
    }
  }

  // Serialize UI actions and screenshots: one at a time
  while (true) {
    Request request;
    {
      std::lock_guard<std::mutex> lock(m_queueMutex);
      if ((m_uiActionInFlight || m_screenshotPending || m_sheet.active))
        return activity + 1; // in flight: full rate until it completes
      if (m_requests.empty())
        return activity;
      request = std::move(m_requests.front());
      m_requests.pop_front();
    }
    activity++;

    try {
      bool handled = false;
      remote::RpcParams params{request.params};
      json result = HandleSemantic(request, state, handled, params);
      if (handled) {
        // Strict params: a recognized method that ran without touching a key
        // the caller sent means that key was a typo, silently ignored under
        // the old path (the `named` vs `name` trap). Reject loudly instead.
        // The check is post-execution (we learn a method's vocabulary by which
        // keys it reads), so a mutating call carrying an *extra* junk key still
        // applies before erroring — the caller sees the mistake and the change
        // used only recognized keys; a *misspelled required* key instead throws
        // "key not found" before any mutation.
        if (auto unknown = params.UnknownKeys(); !unknown.empty()) {
          RespondError(
              request.client, request.id,
              UnknownParamError(request.method, unknown, params.SeenKeys()));
          continue;
        }
        // Session recording (S4): successful semantic requests only — a
        // throwing call mutates nothing, so replay equivalence holds without
        // it (UI actions/screenshots are also skipped: not
        // headless-replayable).
        if (m_recording.is_open() && request.method != "ping") {
          m_recording << json{{"method", request.method},
                              {"params", request.params}}
                             .dump()
                      << "\n";
          m_recording.flush();
        }
        Respond(request.client, request.id, result);
        continue;
      }
      if (request.method == "screenshot") {
        if (!CaptureAvailable()) {
          RespondError(request.client, request.id,
                       "screenshot requires a rendered frame — start "
                       "designer without --headless (--offscreen renders "
                       "without a window)");
          continue;
        }
        // Drawing sheets are drawn by the GUI layer, which the offscreen
        // capture deliberately excludes — returning the empty frame here
        // cost a real session an eyeball round on five blank PNGs.
        if (m_mode == RemoteMode::Offscreen && state.drawing) {
          RespondError(request.client, request.id,
                       "a drawing sheet renders in the GUI layer, which "
                       "--offscreen does not draw — export_drawing writes "
                       "the sheet as SVG/PDF, or screenshot from a windowed "
                       "instance");
          continue;
        }
        // Optional `view` reorients + fits before the capture, matching the
        // MCP wrapper so raw callers get the oriented shot too.
        if (request.params.contains("view")) {
          const std::string view = request.params.at("view").get<std::string>();
          if (!ApplyStandardView(state, view,
                                 request.params.value("fit", true))) {
            RespondError(
                request.client, request.id,
                "unknown view: " + view +
                    "; views: front, back, left, right, top, bottom, iso");
            continue;
          }
        }
        m_screenshotId = request.id;
        m_screenshotClient = request.client;
        m_screenshotPath = request.params.at("path").get<std::string>();
        m_screenshotPending = true;
        m_screenshotHeldFrames = 0;
        return activity; // main loop captures after this frame renders
      }
      if (request.method == "render_sheet") {
        // V3: stage a multi-view sheet job; the ProcessPending pump captures
        // one view per frame and composites when all tiles are in.
        if (!CaptureAvailable()) {
          RespondError(request.client, request.id,
                       "render_sheet requires a rendered frame — start "
                       "designer without --headless (--offscreen renders "
                       "without a window)");
          continue;
        }
        const std::string outPath =
            request.params.at("path").get<std::string>();
        std::vector<std::string> views = {"iso", "front", "right", "top"};
        if (request.params.contains("views")) {
          views.clear();
          for (const auto& v : request.params.at("views"))
            views.push_back(v.get<std::string>());
        }
        if (views.empty() || views.size() > 6) {
          RespondError(request.client, request.id,
                       "render_sheet takes 1..6 views");
          continue;
        }
        m_sheet = SheetJob{};
        m_sheet.views = views;
        m_sheet.outPath = outPath;
        m_sheet.footer = BuildSheetFooter(state, state.document.get());
        for (size_t i = 0; i < views.size(); i++)
          m_sheet.tilePaths.push_back(
              (std::filesystem::temp_directory_path() /
               ("designer-sheet-" + std::to_string(m_port) + "-" +
                std::to_string(i) + ".png"))
                  .string());
        // Crop each tile to the 3D viewport (panel chrome stays out of the
        // sheet). Framebuffer scale is 1 on this platform; Composite clamps
        // a stale rect instead of failing.
        m_sheet.cropX = static_cast<int>(state.viewportPos.x);
        m_sheet.cropY = static_cast<int>(state.viewportPos.y);
        m_sheet.cropW = static_cast<int>(state.viewportSize.x);
        m_sheet.cropH = static_cast<int>(state.viewportSize.y);
        if (request.params.contains("focus")) {
          // Round-8 retro: frame ONE occurrence large in every view — the
          // bend-direction misread happened because the interesting part
          // was 40 pixels tall in a whole-dock tile.
          if (state.assembly == nullptr) {
            RespondError(request.client, request.id,
                         "focus frames an occurrence, so it needs an "
                         "assembly tab (see list_parts)");
            continue;
          }
          const nlohmann::json& f = request.params.at("focus");
          const Occurrence* found = nullptr;
          if (f.is_string()) {
            for (const Occurrence& o : state.assembly->GetOccurrences())
              if (o.name == f.get<std::string>())
                found = &o;
          } else {
            found = state.assembly->FindById(f.get<uint64_t>());
          }
          if (found == nullptr) {
            RespondError(request.client, request.id,
                         "no occurrence " + f.dump() +
                             " to focus (see list_occurrences)");
            continue;
          }
          Document* pd = ui::PartDocById(state, found->partId);
          if (pd == nullptr ||
              !Measure::WorldBounds(pd->GetBodies(),
                                    ui::OccurrenceMatrix(*found),
                                    m_sheet.focusMin, m_sheet.focusMax)) {
            RespondError(request.client, request.id,
                         "occurrence '" + found->name +
                             "' has no geometry to frame");
            continue;
          }
          m_sheet.hasFocus = true;
          m_sheet.footer.insert(m_sheet.footer.begin(),
                                "FOCUS: " + found->name);
        }
        m_sheet.id = request.id;
        m_sheet.client = request.client;
        m_sheet.active = true;
        return activity; // the pump kicks tile 0 next frame; requests queue
                         // behind
      }
      if (!UiActionsAvailable() && IsUiAction(request.method)) {
        // Three different reasons, three different remedies: nothing to
        // click (no GUI running), no GUI at all (--offscreen renders but
        // draws no interface), or nothing to click WITH (this build has no
        // test engine). Saying which is the difference between "run it
        // differently" and "build it differently".
        RespondError(
            request.client, request.id,
            m_mode == RemoteMode::Offscreen
                ? "UI actions are unavailable in --offscreen — there is no "
                  "GUI to drive. The semantic RPCs and renders all work."
            : m_mode == RemoteMode::Gui
                ? "UI actions need the ImGui Test Engine, and this build was "
                  "configured with -DENABLE_TEST_ENGINE=OFF. The semantic "
                  "RPCs (add_feature, measure, screenshot, …) all work."
                : "UI actions are unavailable in --headless — start designer "
                  "without it");
        continue;
      }
      if (QueueUiAction(request)) {
        return activity; // response sent when the engine finishes the action
      }
      RespondError(request.client, request.id,
                   "unknown method: " + request.method);
    } catch (const std::exception& e) {
      RespondError(request.client, request.id, e.what());
    } catch (const Standard_Failure& e) {
      // The last line of defence. OCC reports failure by throwing
      // Standard_Failure, which does NOT derive from std::exception, so a
      // kernel throw from any handler that forgot its own guard would escape
      // every catch and terminate the process — one bad measure taking the
      // whole application down. Individual features guard themselves; this
      // makes a miss cost an error response instead of the app.
      RespondError(request.client, request.id,
                   std::string("OpenCASCADE error: ") +
                       (e.GetMessageString() ? e.GetMessageString()
                                             : "unspecified failure"));
    }
  }
}

bool RemoteServer::WantsScreenshot(std::string& path) {
  if (!m_screenshotPending)
    return false;
  // Not while the camera is still moving. A camera move ANIMATES for about a
  // second, so a capture taken straight after one shows a frame part way
  // through the turn — a view nobody asked for, and one that reads as a bug in
  // whatever was being looked at. Two false findings in one dogfooding session
  // came from exactly this (docs/cam/dogfood-round.md). The `view` RPC already
  // dodges it by snapping instead of animating; every other route into a
  // camera move — menu_click, the view cube, AlignToFace — does not, so the
  // wait belongs here, where all of them pass through.
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
