#include "remote/RemoteServer.h"

#include "CADKernel.h"
#include "CADMaterials.h"
#include "Camera.h"
#include "FaceSelector.h"
#include "Logger.h"
#include "MeshExport.h"
#include "RayTracingRenderer.h"
#include "MeshRenderer.h"
#include "core/io/Autosaver.h"
#include "core/io/Checkpoints.h"
#include "core/Expression.h"
#include "core/io/GitVault.h"
#include "core/MassProps.h"
#include "core/Measure.h"
#include "core/drawing/DrawingExport.h"
#include "core/io/Serialization.h"
#include "remote/RemoteConfigurations.h"
#include "remote/RemoteSimulation.h"
#include "remote/RemoteInspection.h"
#include "remote/RemoteModeling.h"
#include "remote/RemoteDrawings.h"
#include "remote/RemoteAssembly.h"
#include "remote/RemoteProject.h"
#include "core/features/SketchFeature.h"
#include "core/sketch/SketchSolver.h"
#include "ui/AppActions.h"
#include "ui/PreviewFeature.h"
#include "ui/SelectionPipeline.h"  // ResolveFaceItem (U1b)
#include "ui/SketchInteraction.h"  // FaceElementRef

#include <algorithm>
#include <fstream>
#include <map>

#include "imgui.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>

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
  ::setsockopt(m_listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // localhost only
  addr.sin_port = htons(port);
  if (::bind(m_listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) <
          0 ||
      ::listen(m_listenFd, 1) < 0) {
    LOG_ERROR("Remote control: cannot listen on port {}", port);
    ::close(m_listenFd);
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
    ::close(m_listenFd);
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
    if (m_clients.size() >= 8) {  // runaway-connection backstop
      ::close(client);
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
    ssize_t n = ::read(client, chunk, sizeof(chunk));
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
  ::close(client);
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
    ssize_t n = ::write(client, line.data() + sent, line.size() - sent);
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

ImGuiKeyChord ParseChord(const std::string& text, std::string* error) {
  ImGuiKeyChord chord = 0;
  size_t start = 0;
  while (start <= text.size()) {
    size_t end = text.find('+', start);
    std::string token = text.substr(
        start, end == std::string::npos ? std::string::npos : end - start);
    if (token == "Ctrl") chord |= ImGuiMod_Ctrl;
    else if (token == "Shift") chord |= ImGuiMod_Shift;
    else if (token == "Alt") chord |= ImGuiMod_Alt;
    else if (token.size() == 1 && token[0] >= 'A' && token[0] <= 'Z')
      chord |= ImGuiKey_A + (token[0] - 'A');
    else if (token.size() == 1 && token[0] >= '0' && token[0] <= '9')
      chord |= ImGuiKey_0 + (token[0] - '0');
    else if (token == "Enter") chord |= ImGuiKey_Enter;
    else if (token == "Escape") chord |= ImGuiKey_Escape;
    else if (token == "Delete") chord |= ImGuiKey_Delete;
    else if (token == "Tab") chord |= ImGuiKey_Tab;
    else if (token == "Space") chord |= ImGuiKey_Space;
    else if (token == "Left") chord |= ImGuiKey_LeftArrow;
    else if (token == "Right") chord |= ImGuiKey_RightArrow;
    else if (token == "Up") chord |= ImGuiKey_UpArrow;
    else if (token == "Down") chord |= ImGuiKey_DownArrow;
    else {
      if (error) *error = "unknown key token: " + token;
      return 0;
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return chord;
}

ImVec2 FractionToPos(const json& params) {
  ImVec2 size = ImGui::GetMainViewport()->Size;
  return ImVec2(params.value("x", 0.5f) * size.x,
                params.value("y", 0.5f) * size.y);
}

} // namespace

json RemoteServer::HandleSemantic(const Request& request, ui::AppState& state,
                                  bool& handled) {
  const std::string& m = request.method;
  const json& p = request.params;
  Document* doc = state.document.get();
  handled = true;

  // Methods that act on the project container or the active assembly tab, not
  // on an active part document. Everything else needs a live part: when an
  // assembly tab is active, `doc` is null, so fail with a clear message rather
  // than dereferencing null.
  static const std::set<std::string> kNoPartDoc = {
      "ping",         "list_parts",       "add_part",
      "add_assembly", "set_active_part",  "delete_part",
      // Configuration RPCs target the active tab: part-doc handling above,
      // assembly handling below. Listed here so they are allowed on an
      // assembly tab (where doc is null).
      "add_configuration", "remove_configuration", "list_configurations",
      "set_configuration", "set_active_configuration",
      "create_drawing", "list_drawings", "add_drawing_view",
      "remove_drawing_view", "update_drawing", "list_drawing_views",
      "export_drawing", "list_drawing_geometry", "add_drawing_dim",
      "remove_drawing_dim", "list_drawing_dims",
      "new_project",  "save_project",     "open_project",
      "add_occurrence", "remove_occurrence", "list_occurrences",
      "set_occurrence_transform", "set_occurrence_visible",
      "set_occurrence_fixed", "rename_occurrence",
      "add_mate", "set_mate", "remove_mate", "list_mates", "select_mate",
      "select_occurrence", "solve_assembly", "bom", "export_bom",
      "edit_in_context", "add_exploded_view", "remove_exploded_view",
      "list_exploded_views", "set_explode_step", "auto_explode", "set_explode",
      // export_visual works on a part OR an assembly (occurrence walk).
      "export_visual",
      // interference runs on occurrence pairs on an assembly tab;
      // list_elements/measure work there when given an explicit `part`.
      "interference", "list_elements", "measure",
      // undo/redo route to the assembly's snapshot history on an assembly tab.
      "undo", "redo",
      // Camera / rendering act on the viewport, not the part document, so they
      // work on an assembly tab too.
      "view",         "fit_view",         "screenshot",
      "set_render_options"};
  // UI actions (item_click, viewport_click, …) fall through to QueueUiAction,
  // not HandleSemantic, so don't guard them here — they drive the assembly tab.
  if (!doc && !kNoPartDoc.count(m) && !IsUiAction(m)) {
    const char* kind = state.assembly ? "an assembly"
                       : state.drawing ? "a drawing"
                                       : "not a part";
    throw std::runtime_error("the active document is " + std::string(kind) +
                             ", not a part; switch to a part tab");
  }

  // Doc-level-object RPCs carved into their own module (decision 7). The
  // config RPCs target whichever tab is active — part or assembly.
  if (doc) {
    if (json cfgOut; remote::HandleConfigurationRpc(state, *doc, m, p, cfgOut))
      return cfgOut;
  } else if (state.assembly) {
    if (json cfgOut;
        remote::HandleAssemblyConfigurationRpc(state, *state.assembly, m, p, cfgOut))
      return cfgOut;
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
            {"normal", Vec3ToJson(state.faceSelector->GetFaceNormal(mesh, face))},
            {"center", Vec3ToJson(state.faceSelector->GetFaceCenter(mesh, face))}};
      }
    }
    // The typed Selection (U1): every item, stable refs only.
    json selectionItems = json::array();
    for (const ui::SelectionItem& it : state.selection.items) {
      const char* kind = it.kind == ui::SelectionItem::Kind::Face  ? "face"
                         : it.kind == ui::SelectionItem::Kind::Edge ? "edge"
                         : it.kind == ui::SelectionItem::Kind::Body ? "body"
                         : it.kind == ui::SelectionItem::Kind::Feature
                             ? "feature"
                         : it.kind == ui::SelectionItem::Kind::Occurrence
                             ? "occurrence"
                         : it.kind == ui::SelectionItem::Kind::Mate
                             ? "mate"
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
            {"can_undo", state.commands->CanUndo()},
            {"can_redo", state.commands->CanRedo()},
            {"rollback", doc->Rollback()},
            {"regen",
             {{"executed", doc->LastRegenStats().executed},
              {"cached", doc->LastRegenStats().cached}}},
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
    return {{"ok", ok},        {"path", path},   {"width", width},
            {"height", height}, {"samples", samples}, {"error", error}};
  }
  // Part-document domain modules (disjoint method sets; order doesn't matter).
  if (doc) {
    if (auto r = remote::HandleModelingRpc(state, *doc, m, p)) return *r;
    if (auto r = remote::HandleSimulationRpc(state, *doc, m, p)) return *r;
  }
  // Inspection takes a nullable doc: interference works on assembly
  // occurrence pairs, list_elements/measure on any part via `part`.
  if (auto r = remote::HandleInspectionRpc(state, doc, m, p)) return *r;
  if (m == "undo") {
    if (state.assembly) { // assembly tab: snapshot-based occurrence undo
      ui::UndoAssembly(state);
      return {{"can_undo", !state.assemblyHistory.undo.empty()}};
    }
    if (state.drawing) { // drawing tab: snapshot-based sheet/view/dim undo
      ui::UndoDrawing(state);
      return {{"can_undo", !state.drawingHistory.undo.empty()}};
    }
    state.commands->Undo(*doc);
    return {{"can_undo", state.commands->CanUndo()}};
  }
  if (m == "redo") {
    if (state.assembly) {
      ui::RedoAssembly(state);
      return {{"can_redo", !state.assemblyHistory.redo.empty()}};
    }
    if (state.drawing) {
      ui::RedoDrawing(state);
      return {{"can_redo", !state.drawingHistory.redo.empty()}};
    }
    state.commands->Redo(*doc);
    return {{"can_redo", state.commands->CanRedo()}};
  }
  if (m == "list_parts") {
    // Multi-document project (assemblies A1/A2): the tabs and which is active.
    ui::EnsureParts(state);
    json arr = json::array();
    for (size_t i = 0; i < state.parts.size(); i++) {
      const char* type = state.parts[i].kind == ui::DocKind::Assembly ? "assembly"
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
    return {{"parts", arr}, {"active", state.activePart}};
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
  if (auto r = remote::HandleDrawingRpc(state, m, p)) return *r;
  if (auto r = remote::HandleAssemblyRpc(state, m, p)) return *r;
  if (auto r = remote::HandleProjectRpc(state, m, p)) return *r;
  if (m == "fit_view") {
    ui::FitCameraToDocument(state);
    return json::object();
  }
  if (m == "view") {
    // Standard view, snapped instantly (no animation) so the very next
    // screenshot captures the final framing. Fit runs first: it re-aims at the
    // document center (SetTarget), so the snap must set the orientation last.
    // params: {name: front|back|left|right|top|bottom|iso, fit?: true}
    // Directions follow the default planes (Front = XY, normal +Z, Y up);
    // top/bottom pick the up the camera would reach by tilting over from front.
    static const std::map<std::string, std::pair<glm::vec3, glm::vec3>> kViews =
        {{"front", {{0, 0, 1}, {0, 1, 0}}},
         {"back", {{0, 0, -1}, {0, 1, 0}}},
         {"right", {{1, 0, 0}, {0, 1, 0}}},
         {"left", {{-1, 0, 0}, {0, 1, 0}}},
         {"top", {{0, 1, 0}, {0, 0, -1}}},
         {"bottom", {{0, -1, 0}, {0, 0, 1}}},
         {"iso", {glm::normalize(glm::vec3(1, 1, 1)), {0, 1, 0}}}};
    const std::string name = p.at("name").get<std::string>();
    auto it = kViews.find(name);
    if (it == kViews.end()) throw std::runtime_error("unknown view: " + name);
    if (p.value("fit", true)) ui::FitCameraToDocument(state);
    state.camera->SnapToView(it->second.first, it->second.second);
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
    if (p.contains("shadows")) state.showShadows = p.at("shadows").get<bool>();
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
      if (!state.document) throw std::runtime_error("no document");
      const std::string el = p.at("element").get<std::string>();
      const auto& bodies = state.document->GetBodies();
      bool found = false;
      for (size_t i = 0; i < bodies.size() && !found; ++i) {
        for (const auto& [idx, name] : bodies[i].elementMap.names) {
          if (name != el) continue;
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
      state.selection.Replace(
          {ui::SelectionItem::Kind::Face, 0, *ref});
    }
    return {{"normal", Vec3ToJson(state.faceSelector->GetFaceNormal(mesh, face))},
            {"center", Vec3ToJson(state.faceSelector->GetFaceCenter(mesh, face))}};
  }
  if (m == "set_camera") {
    // Direct camera control (the view/fit RPCs only snap to standard views):
    // params: {target?: [x,y,z], direction?: [x,y,z] (unit offset from target
    // toward the camera), up?: [x,y,z], distance?: mm}. Applied in that
    // order; each field optional. Works headless (camera state only).
    if (!state.camera) throw std::runtime_error("no camera");
    Camera& cam = *state.camera;
    auto vec3 = [](const json& a) {
      return glm::vec3(a.at(0).get<float>(), a.at(1).get<float>(),
                       a.at(2).get<float>());
    };
    if (p.contains("target")) cam.SetTarget(vec3(p["target"]));
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
      if (d <= 0.0f) throw std::runtime_error("distance must be positive");
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
  static const char* kActions[] = {"menu_click",        "item_click",
                                   "item_double_click", "item_input",
                                   "viewport_click",    "viewport_drag",
                                   "key_press",         "scroll"};
  for (const char* a : kActions)
    if (method == a)
      return true;
  return false;
}

bool RemoteServer::QueueUiAction(const Request& request) {
  const std::string& m = request.method;
  const json p = request.params; // copy: captured by the action lambda

  if (m == "menu_click") {
    m_uiAction = [p](ImGuiTestContext* ctx) {
      ctx->SetRef("//##MainMenuBar");
      ctx->MenuClick(p.at("path").get<std::string>().c_str());
    };
  } else if (m == "item_click") {
    m_uiAction = [p](ImGuiTestContext* ctx) {
      ctx->ItemClick(p.at("ref").get<std::string>().c_str(),
                     p.value("button", 0)); // 1 = right-click (context menus)
    };
  } else if (m == "item_double_click") {
    m_uiAction = [p](ImGuiTestContext* ctx) {
      ctx->ItemDoubleClick(p.at("ref").get<std::string>().c_str());
    };
  } else if (m == "item_input") {
    m_uiAction = [p](ImGuiTestContext* ctx) {
      const std::string ref = p.at("ref").get<std::string>();
      const json& value = p.at("value");
      if (value.is_number()) {
        ctx->ItemInputValue(ref.c_str(), value.get<float>());
      } else {
        ctx->ItemInputValue(ref.c_str(), value.get<std::string>().c_str());
      }
    };
  } else if (m == "viewport_click") {
    m_uiAction = [p](ImGuiTestContext* ctx) {
      const bool ctrl = p.value("ctrl", false);
      ctx->MouseMoveToPos(FractionToPos(p));
      if (ctrl) ctx->KeyDown(ImGuiMod_Ctrl);
      ctx->MouseClick(p.value("button", 0));
      if (ctrl) ctx->KeyUp(ImGuiMod_Ctrl);
    };
  } else if (m == "viewport_drag") {
    m_uiAction = [p](ImGuiTestContext* ctx) {
      ImVec2 size = ImGui::GetMainViewport()->Size;
      ImVec2 from(p.value("x0", 0.5f) * size.x, p.value("y0", 0.5f) * size.y);
      ImVec2 to(p.value("x1", 0.6f) * size.x, p.value("y1", 0.6f) * size.y);
      const int button = p.value("button", ImGuiMouseButton_Middle);
      const bool ctrl = p.value("ctrl", false);
      const bool shift = p.value("shift", false);
      ctx->MouseMoveToPos(from);
      if (ctrl) ctx->KeyDown(ImGuiMod_Ctrl);
      if (shift) ctx->KeyDown(ImGuiMod_Shift);
      ctx->MouseDown(button);
      ctx->MouseMoveToPos(to);
      ctx->MouseUp(button);
      if (shift) ctx->KeyUp(ImGuiMod_Shift);
      if (ctrl) ctx->KeyUp(ImGuiMod_Ctrl);
    };
  } else if (m == "key_press") {
    std::string error;
    ImGuiKeyChord chord = ParseChord(p.at("chord").get<std::string>(), &error);
    if (chord == 0) return false;
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
  m_uiActionInFlight = true;
  ImGuiTestEngine_QueueTest(m_engine, s_UiActionTest);
  return true;
}

void RemoteServer::ProcessPending(ui::AppState& state) {
  if (!m_running)
    return;

  // Store a finished async topology run (TO2) even if the client never polls
  // study_status — this is the app's per-tick main-thread join point.
  if (state.document) ui::PollTopologyRun(state, *state.document);

  // Finish an in-flight UI action once the engine is done with it
  if (m_uiActionInFlight && ImGuiTestEngine_IsTestQueueEmpty(m_engine)) {
    m_uiActionInFlight = false;
    m_uiAction = nullptr;
    if (s_UiActionTest->Output.Status == ImGuiTestStatus_Success) {
      Respond(m_uiActionClient, m_uiActionId, json::object());
    } else {
      RespondError(m_uiActionClient, m_uiActionId,
                   "ui action failed (see app log)");
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
      json result = HandleSemantic(request, state, handled);
      if (handled) {
        // Session recording (S4): successful semantic requests only — a
        // throwing call mutates nothing, so replay equivalence holds without
        // it (UI actions/screenshots are also skipped: not headless-replayable).
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
