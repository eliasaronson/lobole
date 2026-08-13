#include "remote/RemoteServer.h"

#include "cad/CADKernel.h"
#include "cad/CADMaterials.h"
#include "cad/FaceSelector.h"
#include "cad/MeshExport.h"
#include "common/Logger.h"
#include "core/Expression.h"
#include "core/MassProps.h"
#include "core/Measure.h"
#include "core/drawing/DrawingExport.h"
#include "core/features/SketchFeature.h"
#include "core/io/Autosaver.h"
#include "core/io/Checkpoints.h"
#include "core/io/GitVault.h"
#include "core/io/Serialization.h"
#include "core/sketch/SketchSolver.h"
#include "remote/RemoteAssembly.h"
#include "remote/RemoteConfigurations.h"
#include "remote/RemoteDrawings.h"
#include "remote/RemoteHelpers.h"
#include "remote/RemoteInspection.h"
#include "remote/RemoteModeling.h"
#include "remote/RemoteProject.h"
#include "remote/RemoteSimulation.h"
#include "remote/RpcParams.h"
#include "render/Camera.h"
#include "render/MeshRenderer.h"
#include "render/RayTracingRenderer.h"
#include "ui/AppActions.h"
#include "ui/PreviewFeature.h"
#include "ui/SelectionPipeline.h" // ResolveFaceItem (U1b)
#include "ui/SketchInteraction.h" // FaceElementRef

#include <algorithm>
#include <fstream>
#include <map>

#include "imgui.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

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
static ImGuiTest* s_UiActionTest = nullptr;

// TestFunc of the reusable "remote action" test: runs whatever UI action is
// currently queued. Runs inside the test engine's coroutine, so it can span
// frames (menu navigation, drags) exactly like a scripted human.
void RemoteServerRunUiAction(ImGuiTestContext* ctx) {
  if (s_Instance && s_Instance->m_uiAction) {
    s_Instance->m_uiAction(ctx);
  }
}

RemoteServer::~RemoteServer() { Stop(); }

bool RemoteServer::Start(uint16_t port, ImGuiTestEngine* engine) {
  m_engine = engine;
  s_Instance = this;

  // Headless (--headless) passes no engine: UI actions and screenshots then
  // degrade with specific errors in ProcessPending.
  if (engine) {
    s_UiActionTest = IM_REGISTER_TEST(engine, "remote", "action");
    s_UiActionTest->TestFunc = [](ImGuiTestContext* ctx) {
      RemoteServerRunUiAction(ctx);
    };
  }

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
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // localhost only
  addr.sin_port = htons(port);
  if (::bind(m_listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) <
          0 ||
      ::listen(m_listenFd, 1) < 0) {
    LOG_ERROR("Remote control: cannot listen on port {}", port);
    SockClose(m_listenFd);
    m_listenFd = -1;
    return false;
  }

  m_running = true;
  m_thread = std::thread([this] { ReaderThread(); });
  LOG_INFO("Remote control listening on 127.0.0.1:{}", port);
  return true;
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
      std::lock_guard<std::mutex> lock(m_queueMutex);
      m_requests.push_back(std::move(request));
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

  ItemTarget(const json& params, const std::string& method) {
    path = params.value("ref", std::string());
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

// Snap the camera to a named standard view (optionally fitting first). Shared
// by the `view` RPC and the raw `screenshot` handler so a raw screenshot(view=)
// orients the same way the MCP wrapper does. Returns false for an unknown name.
// Directions follow the default planes (Front = XY, normal +Z, Y up);
// top/bottom pick the up the camera reaches by tilting over from front.
bool ApplyStandardView(ui::AppState& state, const std::string& name, bool fit) {
  static const std::map<std::string, std::pair<glm::vec3, glm::vec3>> kViews = {
      {"front", {{0, 0, 1}, {0, 1, 0}}},
      {"back", {{0, 0, -1}, {0, 1, 0}}},
      {"right", {{1, 0, 0}, {0, 1, 0}}},
      {"left", {{-1, 0, 0}, {0, 1, 0}}},
      {"top", {{0, 1, 0}, {0, 0, -1}}},
      {"bottom", {{0, -1, 0}, {0, 0, 1}}},
      {"iso", {glm::normalize(glm::vec3(1, 1, 1)), {0, 1, 0}}}};
  auto it = kViews.find(name);
  if (it == kViews.end())
    return false;
  // Fit runs first: it re-aims at the document center (SetTarget), so the snap
  // must set the orientation last.
  if (fit)
    ui::FitCameraToDocument(state);
  state.camera->SnapToView(it->second.first, it->second.second);
  return true;
}

} // namespace

// Methods that act on the project container or the active assembly tab, not
// on an active part document. Everything else needs a live part: when an
// assembly tab is active, `doc` is null, so fail with a clear message rather
// than dereferencing null. File scope so the describe endpoint can derive
// each method's requires_part_doc from it.
static const std::set<std::string> kNoPartDoc = {
    "describe", "ping", "list_parts", "add_part", "add_assembly",
    "set_active_part", "delete_part", "rename_part",
    // Configuration RPCs target the active tab: part-doc handling above,
    // assembly handling below. Listed here so they are allowed on an
    // assembly tab (where doc is null).
    "add_configuration", "remove_configuration", "list_configurations",
    "set_configuration", "set_active_configuration", "create_drawing",
    "list_drawings", "add_drawing_view", "remove_drawing_view",
    "set_drawing_view", "set_drawing_sheet", "check_drawing", "update_drawing",
    "list_drawing_views", "export_drawing", "list_drawing_geometry",
    "add_drawing_dim", "remove_drawing_dim", "list_drawing_dims", "new_project",
    "save_project", "open_project", "add_occurrence", "remove_occurrence",
    "list_occurrences", "set_occurrence_transform", "set_occurrence_visible",
    "set_occurrence_fixed", "rename_occurrence", "add_mate", "set_mate",
    "remove_mate", "list_mates", "select_mate", "select_occurrence",
    "solve_assembly", "bom", "export_bom", "edit_in_context",
    "add_exploded_view", "remove_exploded_view", "list_exploded_views",
    "set_explode_step", "auto_explode", "set_explode",
    // export_visual works on a part OR an assembly (occurrence walk).
    "export_visual",
    // interference runs on occurrence pairs on an assembly tab;
    // list_elements/measure work there when given an explicit `part`;
    // mass_properties aggregates occurrence masses in world space.
    "interference", "list_elements", "measure", "mass_properties",
    // set_section only writes the global viewport clip plane (per-frame UBO,
    // world space) — it clips assembly occurrences too. The `section` AREA
    // query stays part-only (computes on one document's bodies).
    "set_section",
    // undo/redo route to the assembly's snapshot history on an assembly tab.
    "undo", "redo",
    // Project-level reads (git vault / checkpoint list act on projectPath,
    // not the active document). checkpoint_create/restore snapshot the
    // ACTIVE PART document and stay part-gated.
    "history_log", "checkpoint_list",
    // Camera / rendering act on the viewport, not the part document, so they
    // work on an assembly tab too. render_still's RT scene sync does the
    // occurrence walk itself.
    "view", "fit_view", "screenshot", "set_render_options", "set_camera",
    "render_still"};

// The methods RemoteServer dispatches itself (viewport, camera, selection,
// project tabs, UI events). describe: one row per `m ==` branch — extend
// together. `screenshot` and the UI actions are dispatched outside
// HandleSemantic but are part of the same surface.
static constexpr remote::MethodDoc kServerMethods[] = {
    {"describe",
     "This endpoint: methods, feature/entity/constraint types, enums, and the "
     "document format version."},
    {"ping", "Liveness check; returns {ok, app}."},
    {"get_state",
     "Full snapshot: features (with errors), bodies (volume, bbox), "
     "selection, camera, undo/redo, rollback, regen summary."},
    {"undo", "Undo one step (assembly snapshot history on an assembly tab)."},
    {"redo", "Redo one step."},
    {"view",
     "Orient the camera to a named view {name: front|back|left|right|top|"
     "bottom|iso}."},
    {"fit_view", "Frame the geometry in the viewport."},
    {"set_camera", "Set camera {position, target, up?} explicitly."},
    {"orient_to_face", "Look straight at the selected face."},
    {"select_face",
     "Select a face by {mesh, face} or stable {element}/{feature_id, "
     "element}; returns normal/center."},
    {"screenshot", "Write the rendered viewport to {path} (needs a renderer "
                   "— not available under --headless)."},
    {"render_still",
     "Path-traced still to {path} (ray tracing; needs GPU support)."},
    {"rt_build", "Build ray-tracing acceleration structures."},
    {"set_render_options", "Toggle shadows/SSAO/quality options."},
    {"reset_layout", "Reset the dock layout."},
    {"add_part", "Add a new empty part tab."},
    {"add_assembly", "Add an assembly tab."},
    {"list_parts", "List project tabs (parts, assemblies, drawings)."},
    {"rename_part", "Rename a part."},
    {"menu_click", "Drive a menu item by path, e.g. \"Edit/Undo\"."},
    {"key_press", "Send a key chord, e.g. \"Ctrl+Z\"."},
    {"item_click",
     "UI test tier: click a widget by {ref} or by the {id} ui_items reports "
     "(the only handle on hidden-label widgets like a combo). Ref example: "
     "\"//##DockHost/**/###tb_box\" (//window, ** = any depth, ### = the "
     "widget's stable id); {button: 1} right-clicks."},
    {"item_double_click", "UI test tier: double-click a widget (same ref "
                          "grammar as item_click)."},
    {"item_input", "UI test tier: type into a widget {ref, value}."},
    {"item_hover", "UI test tier: hover a widget {ref|id} — shows its "
                   "tooltip and hover state without clicking."},
    {"item_combo", "UI test tier: choose {value} in the combo {ref|id} "
                   "(opens and picks in one action — the popup does not "
                   "survive between RPCs)."},
    {"ui_items", "UI test tier: list the widgets under {ref} (default the "
                 "focused window, {depth}): label, id, window, disabled, "
                 "screen centre. The GUI, self-described."},
    {"viewport_click",
     "Click the 3D viewport at (x, y) as 0-1 fractions OF THE VIEWPORT "
     "rect (0.5,0.5 = its centre, whatever is docked around it)."},
    {"viewport_drag", "Drag in the viewport (button 0/1/2) — orbit/pan."},
    {"scroll", "Scroll (zoom) the viewport."},
};

json RemoteServer::HandleSemantic(const Request& request, ui::AppState& state,
                                  bool& handled, remote::RpcParams& p) {
  const std::string& m = request.method;
  Document* doc = state.document.get();
  handled = true;

  // UI actions (item_click, viewport_click, …) fall through to QueueUiAction,
  // not HandleSemantic, so don't guard them here — they drive the assembly tab.
  if (!doc && !kNoPartDoc.count(m) && !IsUiAction(m)) {
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
    json methods = json::array();
    const auto addDocs = [&](std::span<const remote::MethodDoc> docs) {
      for (const auto& d : docs) {
        methods.push_back({{"name", d.name},
                           {"doc", d.doc},
                           {"requires_part_doc",
                            !kNoPartDoc.count(d.name) && !IsUiAction(d.name)}});
      }
    };
    addDocs(kServerMethods);
    addDocs(remote::ModelingMethodDocs());
    addDocs(remote::ProjectMethodDocs());
    addDocs(remote::AssemblyMethodDocs());
    addDocs(remote::InspectionMethodDocs());
    addDocs(remote::DrawingMethodDocs());
    addDocs(remote::ConfigurationMethodDocs());
    addDocs(remote::SimulationMethodDocs());
    json featureTypes = json::array();
    for (const auto& ft : DocumentIO::FeatureTypes())
      featureTypes.push_back(
          {{"type", ft.type}, {"since_version", ft.sinceVersion}});
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
    return {{"ok", true}, {"app", "designer"}};
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
    std::string error;
    bool ok = ui::RenderStillBlocking(state, width, height, samples, path, seed,
                                      ground, &error);
    return {{"ok", ok},         {"path", path},       {"width", width},
            {"height", height}, {"samples", samples}, {"error", error}};
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
  if (auto r = remote::HandleInspectionRpc(state, doc, m, p))
    return *r;
  if (m == "undo") {
    // Global undo (Phase V): the active tab's stack OR the cross-tab
    // project-variable stack, whichever was most recently mutated.
    ui::GlobalUndo(state);
    return {{"can_undo", ui::CanGlobalUndo(state)}};
  }
  if (m == "redo") {
    ui::GlobalRedo(state);
    return {{"can_redo", ui::CanGlobalRedo(state)}};
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
    const std::string name =
        p.value("name", "Part " + std::to_string(state.parts.size() + 1));
    const size_t idx = ui::AddPart(state, name);
    return {{"index", idx}, {"name", name}, {"id", state.parts[idx].id}};
  }
  if (m == "add_assembly") {
    const std::string name =
        p.value("name", "Assembly " + std::to_string(state.parts.size() + 1));
    const size_t idx = ui::AddAssembly(state, name);
    return {{"index", idx}, {"name", name}, {"id", state.parts[idx].id}};
  }
  if (m == "rename_part") {
    // Rename an existing tab (part/assembly/drawing) so the default "Part 1"
    // isn't permanent — it feeds BOM + add_occurrence. Occurrences reference
    // parts by stable id, so a name change never breaks a reference. `part`
    // resolves by name, stable id, or slot index. params: {part, new_name}
    ui::EnsureParts(state);
    const json& sel = p.at("part");
    int slot = -1;
    if (sel.is_number_integer() || sel.is_number_unsigned()) {
      const uint64_t v = sel.get<uint64_t>();
      for (size_t i = 0; i < state.parts.size(); i++)
        if (state.parts[i].id == v) {
          slot = static_cast<int>(i);
          break;
        }
      if (slot < 0 && v < state.parts.size())
        slot = static_cast<int>(v);
    } else if (sel.is_string()) {
      const std::string s = sel.get<std::string>();
      for (size_t i = 0; i < state.parts.size(); i++)
        if (state.parts[i].name == s) {
          slot = static_cast<int>(i);
          break;
        }
      if (slot < 0) {
        try { // numeric string: stable id, then slot index
          const uint64_t v = std::stoull(s);
          for (size_t i = 0; i < state.parts.size(); i++)
            if (state.parts[i].id == v) {
              slot = static_cast<int>(i);
              break;
            }
          if (slot < 0 && v < state.parts.size())
            slot = static_cast<int>(v);
        } catch (...) {
        }
      }
    }
    if (slot < 0)
      throw std::runtime_error("no part '" + sel.dump() +
                               "' — pass a part name, stable id or slot index "
                               "(see list_parts)");
    const std::string newName = p.at("new_name").get<std::string>();
    if (newName.empty())
      throw std::runtime_error("new_name must be non-empty");
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
      throw std::runtime_error("unknown view: " + name);
    return {{"position", Vec3ToJson(state.camera->GetPosition())},
            {"target", Vec3ToJson(state.camera->GetTarget())},
            {"distance", state.camera->GetDistance()}};
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
    if (p.contains("direction")) {
      const glm::vec3 dir = vec3(p["direction"]);
      if (glm::length(dir) < 1e-9f)
        throw std::runtime_error("direction must be non-zero");
      const glm::vec3 up =
          p.contains("up") ? vec3(p["up"]) : glm::vec3(0.0f, 1.0f, 0.0f);
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
      "menu_click", "item_click", "item_double_click", "item_input",
      "item_hover", "ui_items",   "viewport_click",    "viewport_drag",
      "key_press",  "scroll"};
  for (const char* a : kActions)
    if (method == a)
      return true;
  return false;
}

bool RemoteServer::QueueUiAction(const Request& request) {
  const std::string& m = request.method;
  const json p = request.params; // copy: captured by the action lambda

  // Required params are extracted HERE, on the RPC thread, where a missing
  // key throws into ProcessPending's catch and becomes an error response. A
  // p.at() inside an action lambda would throw in the test-engine coroutine
  // instead, escape it, and std::terminate the whole app.
  if (m == "menu_click") {
    const std::string path = p.at("path").get<std::string>();
    m_uiAction = [path](ImGuiTestContext* ctx) {
      ctx->SetRef("//##MainMenuBar");
      ctx->MenuClick(path.c_str());
    };
  } else if (m == "item_click") {
    const ItemTarget target(p, m);
    const int button = p.value("button", 0); // 1 = right-click (context menus)
    m_uiAction = [target, button](ImGuiTestContext* ctx) {
      if (target.RefuseIfDisabled(ctx))
        return;
      ctx->ItemClick(target.Ref(), button);
    };
  } else if (m == "item_double_click") {
    const ItemTarget target(p, m);
    m_uiAction = [target](ImGuiTestContext* ctx) {
      if (target.RefuseIfDisabled(ctx))
        return;
      ctx->ItemDoubleClick(target.Ref());
    };
  } else if (m == "item_input") {
    const ItemTarget target(p, m);
    const json value = p.at("value");
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
    const ItemTarget target(p, m);
    m_uiAction = [target](ImGuiTestContext* ctx) {
      ctx->MouseMove(target.Ref());
    };
  } else if (m == "item_combo") {
    const ItemTarget target(p, m);
    const std::string value = p.at("value").get<std::string>();
    // Open + choose in ONE action: a queued action ends with its own frames,
    // and the combo popup closes in between, so two RPCs cannot do this.
    m_uiAction = [target, value](ImGuiTestContext* ctx) {
      ctx->ItemClick(target.Ref());
      ImGuiWindow* popup = ctx->GetWindowByRef("//$FOCUSED");
      if (popup == nullptr) {
        ctx->LogError("combo did not open a popup");
        return;
      }
      ctx->ItemClick(
          (std::string("//") + popup->Name + "/**/" + value).c_str());
    };
  } else if (m == "ui_items") {
    const std::string ref = p.value("ref", "//$FOCUSED");
    const int depth = p.value("depth", -1);
    m_uiAction = [this, ref, depth](ImGuiTestContext* ctx) {
      ImGuiTestItemList items;
      ctx->GatherItems(&items, ref.c_str(), depth);
      json out = json::array();
      for (const ImGuiTestItemInfo& it : items) {
        json e{{"label", it.DebugLabel},
               {"id", it.ID},
               {"depth", it.Depth},
               {"disabled", (it.ItemFlags & ImGuiItemFlags_Disabled) != 0},
               {"x", it.RectFull.GetCenter().x},
               {"y", it.RectFull.GetCenter().y}};
        if (it.Window != nullptr)
          e["window"] = it.Window->Name;
        out.push_back(std::move(e));
      }
      m_uiActionResult = json{{"items", std::move(out)}};
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
    ImGuiKeyChord chord = ParseChord(p.at("chord").get<std::string>(), &error);
    if (chord == 0) {
      // Returning false here made the dispatcher answer "unknown method:
      // key_press" — the chord is what was unknown, and it said so.
      throw std::runtime_error(error.empty() ? "could not parse the key chord"
                                             : error);
    }
    m_uiAction = [chord](ImGuiTestContext* ctx) { ctx->KeyPress(chord); };
  } else if (m == "scroll") {
    m_uiAction = [p](ImGuiTestContext* ctx) {
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
  ImGuiTestEngine_QueueTest(m_engine, s_UiActionTest);
  return true;
}

void RemoteServer::ProcessPending(ui::AppState& state) {
  if (!m_running)
    return;

  // Fractional viewport coordinates are resolved against the 3D viewport's
  // rect, so snapshot it while we are on the main thread with `state` in hand.
  s_ViewportPos = ImVec2(state.viewportPos.x, state.viewportPos.y);
  s_ViewportSize = ImVec2(state.viewportSize.x, state.viewportSize.y);

  // Store a finished async topology run (TO2) even if the client never polls
  // study_status — this is the app's per-tick main-thread join point.
  if (state.document) {
    ui::PollTopologyRun(state, *state.document);
    ui::PollStudyRun(state, *state.document);
  }

  // Finish an in-flight UI action once the engine is done with it
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

  // Serialize UI actions and screenshots: one at a time
  while (true) {
    Request request;
    {
      std::lock_guard<std::mutex> lock(m_queueMutex);
      if (m_requests.empty())
        return;
      if ((m_uiActionInFlight || m_screenshotPending))
        return; // wait for completion before the next request
      request = std::move(m_requests.front());
      m_requests.pop_front();
    }

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
        if (!m_engine) {
          RespondError(request.client, request.id,
                       "screenshot requires the GUI — start "
                       "designer without --headless");
          continue;
        }
        // Optional `view` reorients + fits before the capture, matching the
        // MCP wrapper so raw callers get the oriented shot too.
        if (request.params.contains("view")) {
          const std::string view = request.params.at("view").get<std::string>();
          if (!ApplyStandardView(state, view,
                                 request.params.value("fit", true))) {
            RespondError(request.client, request.id, "unknown view: " + view);
            continue;
          }
        }
        m_screenshotId = request.id;
        m_screenshotClient = request.client;
        m_screenshotPath = request.params.at("path").get<std::string>();
        m_screenshotPending = true;
        return; // main loop captures after this frame renders
      }
      if (!m_engine && IsUiAction(request.method)) {
        RespondError(request.client, request.id,
                     "UI actions are unavailable in --headless — "
                     "start designer without it");
        continue;
      }
      if (QueueUiAction(request)) {
        return; // response sent when the engine finishes the action
      }
      RespondError(request.client, request.id,
                   "unknown method: " + request.method);
    } catch (const std::exception& e) {
      RespondError(request.client, request.id, e.what());
    }
  }
}

bool RemoteServer::WantsScreenshot(std::string& path) {
  if (!m_screenshotPending)
    return false;
  path = m_screenshotPath;
  return true;
}

void RemoteServer::ScreenshotDone(bool ok, const std::string& error) {
  m_screenshotPending = false;
  if (ok) {
    Respond(m_screenshotClient, m_screenshotId, {{"path", m_screenshotPath}});
  } else {
    RespondError(m_screenshotClient, m_screenshotId, error);
  }
}
