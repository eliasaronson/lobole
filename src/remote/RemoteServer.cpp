#include "remote/RemoteServer.h"

#include "Camera.h"
#include "FaceSelector.h"
#include "Logger.h"
#include "MeshRenderer.h"
#include "core/Serialization.h"
#include "core/SketchFeature.h"
#include "core/SketchSolver.h"
#include "ui/AppActions.h"
#include "ui/PreviewFeature.h"

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

  s_UiActionTest = IM_REGISTER_TEST(engine, "remote", "action");
  s_UiActionTest->TestFunc = [](ImGuiTestContext* ctx) {
    RemoteServerRunUiAction(ctx);
  };

  m_running = true;
  m_thread = std::thread([this, port] { ReaderThread(port); });
  LOG_INFO("Remote control listening on 127.0.0.1:{}", port);
  return true;
}

void RemoteServer::Stop() {
  if (!m_running)
    return;
  m_running = false;
  if (m_clientFd >= 0) {
    ::shutdown(m_clientFd, SHUT_RDWR);
  }
  if (m_listenFd >= 0) {
    ::shutdown(m_listenFd, SHUT_RDWR);
    ::close(m_listenFd);
    m_listenFd = -1;
  }
  if (m_thread.joinable())
    m_thread.join();
}

void RemoteServer::ReaderThread(uint16_t port) {
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
    m_running = false;
    return;
  }

  while (m_running) {
    int client = ::accept(m_listenFd, nullptr, nullptr);
    if (client < 0)
      break;
    m_clientFd = client;
    LOG_INFO("Remote control: client connected");

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
        try {
          json msg = json::parse(line);
          request.id = msg.value("id", json());
          request.method = msg.value("method", "");
          request.params = msg.value("params", json::object());
        } catch (const std::exception& e) {
          Send({{"id", nullptr}, {"error", std::string("parse: ") + e.what()}});
          continue;
        }
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_requests.push_back(std::move(request));
      }
    }
    ::close(client);
    m_clientFd = -1;
    LOG_INFO("Remote control: client disconnected");
  }
}

void RemoteServer::Send(const json& message) {
  int fd = m_clientFd;
  if (fd < 0)
    return;
  std::string line = message.dump() + "\n";
  std::lock_guard<std::mutex> lock(m_sendMutex);
  size_t sent = 0;
  while (sent < line.size()) {
    ssize_t n = ::write(fd, line.data() + sent, line.size() - sent);
    if (n <= 0)
      return;
    sent += static_cast<size_t>(n);
  }
}

void RemoteServer::Respond(const json& id, const json& result) {
  Send({{"id", id}, {"result", result}});
}

void RemoteServer::RespondError(const json& id, const std::string& error) {
  Send({{"id", id}, {"error", error}});
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

  if (m == "ping") {
    return {{"ok", true}, {"app", "designer"}};
  }
  if (m == "get_state") {
    json features = json::array();
    for (size_t i = 0; i < doc->FeatureCount(); i++) {
      json f = DocumentIO::FeatureToJson(*doc->GetFeature(i));
      f["index"] = i;
      features.push_back(f);
    }
    json bodies = json::array();
    for (size_t i = 0; i < doc->GetBodies().size(); i++) {
      const auto& body = doc->GetBodies()[i];
      glm::vec3 min, max;
      body.shape.GetBoundingBox(min, max);
      json entry = {{"name", body.name},
                    {"volume", body.shape.Volume()},
                    {"bbox_min", Vec3ToJson(min)},
                    {"bbox_max", Vec3ToJson(max)}};
      // Render/picking mesh id (used by select_face); ids change on
      // regeneration, so always read them fresh from here
      if (i < state.documentMeshes.size()) {
        entry["mesh"] = state.documentMeshes[i].meshId;
      }
      bodies.push_back(entry);
    }
    json selection = nullptr;
    if (const Face* face = state.faceSelector->GetSelectedFace()) {
      selection = {{"mesh", face->meshId},
                   {"face", face->faceIndex},
                   {"normal", Vec3ToJson(face->normal)},
                   {"center", Vec3ToJson(face->center)}};
    }
    return {{"features", features},
            {"bodies", bodies},
            {"selection", selection},
            {"selected_feature", state.selectedFeature},
            {"camera",
             {{"position", Vec3ToJson(state.camera->GetPosition())},
              {"target", Vec3ToJson(state.camera->GetTarget())},
              {"distance", state.camera->GetDistance()}}},
            {"can_undo", state.commands->CanUndo()},
            {"can_redo", state.commands->CanRedo()},
            {"pending_feature_id", state.pendingFeatureId}};
  }
  if (m == "accept_pending") {
    ui::AcceptPendingFeature(state);
    return json::object();
  }
  if (m == "cancel_pending") {
    ui::CancelPendingFeature(state);
    return json::object();
  }
  if (m == "add_feature") {
    auto feature = DocumentIO::FeatureFromJson(p.at("feature"));
    state.commands->Execute(
        std::make_unique<AddFeatureCommand>(std::move(feature)), *doc);
    const size_t index = doc->FeatureCount() - 1;
    // Return the id the document actually assigned, so callers reference it
    // (a downstream extrude names its sketch by id) without guessing.
    json resp = {{"index", index}, {"id", doc->GetFeature(index)->GetId()}};
    // For sketches, also return the stable entity ids assigned on load (clients
    // send entities without ids and reference these in add_constraint).
    if (auto* sk = dynamic_cast<const SketchFeature*>(doc->GetFeature(index))) {
      json ids = json::array();
      for (const auto& e : sk->entities)
        ids.push_back(sketch::EntityId(e));
      resp["entity_ids"] = ids;
    }
    return resp;
  }
  if (m == "modify_feature") {
    const size_t index = p.at("index").get<size_t>();
    Feature* feature = doc->GetFeature(index);
    if (!feature) throw std::runtime_error("no feature at index");
    json before = DocumentIO::FeatureToJson(*feature);
    if (!state.commands->Execute(std::make_unique<ModifyFeatureCommand>(
                                     index, before, p.at("feature")),
                                 *doc)) {
      throw std::runtime_error("modify failed");
    }
    return json::object();
  }
  if (m == "delete_feature") {
    if (!state.commands->Execute(std::make_unique<RemoveFeatureCommand>(
                                     p.at("index").get<size_t>()),
                                 *doc)) {
      throw std::runtime_error("no feature at index");
    }
    return json::object();
  }
  if (m == "add_constraint") {
    // Append a constraint to a sketch (by id), solve, and commit one edit.
    // params: {sketch_id, kind, refs: [[entity,which],...], value?}
    size_t index = 0;
    Feature* f = doc->FindFeatureById(p.at("sketch_id").get<uint64_t>(), &index);
    auto* sk = dynamic_cast<SketchFeature*>(f);
    if (!sk) throw std::runtime_error("no such sketch");

    sketch::Constraint c;
    static const std::map<std::string, sketch::ConstraintKind> kinds = {
        {"coincident", sketch::ConstraintKind::Coincident},
        {"horizontal", sketch::ConstraintKind::Horizontal},
        {"vertical", sketch::ConstraintKind::Vertical},
        {"parallel", sketch::ConstraintKind::Parallel},
        {"perpendicular", sketch::ConstraintKind::Perpendicular},
        {"equal", sketch::ConstraintKind::Equal},
        {"distance", sketch::ConstraintKind::Distance},
        {"radius", sketch::ConstraintKind::Radius},
        {"angle", sketch::ConstraintKind::Angle},
        {"tangent", sketch::ConstraintKind::Tangent},
        {"point_on_line", sketch::ConstraintKind::PointOnLine},
        {"symmetric", sketch::ConstraintKind::Symmetric},
        {"midpoint", sketch::ConstraintKind::Midpoint},
        {"fixed", sketch::ConstraintKind::Fixed},
        {"concentric", sketch::ConstraintKind::Concentric},
        {"collinear", sketch::ConstraintKind::Collinear}};
    auto it = kinds.find(p.at("kind").get<std::string>());
    if (it == kinds.end()) throw std::runtime_error("unknown constraint kind");
    c.kind = it->second;
    c.value = p.value("value", 0.0);
    const json& refs = p.at("refs");
    if (refs.size() > 0)
      c.a = {refs[0][0].get<uint32_t>(), refs[0][1].get<uint8_t>()};
    if (refs.size() > 1)
      c.b = {refs[1][0].get<uint32_t>(), refs[1][1].get<uint8_t>()};
    if (refs.size() > 2) // Symmetric's third ref (the symmetry line)
      c.c = {refs[2][0].get<uint32_t>(), refs[2][1].get<uint8_t>()};

    json before = DocumentIO::FeatureToJson(*sk);
    sk->constraints.push_back(c);
    sketch::Solver solver;
    solver.Load(*sk);
    if (solver.Solve().ok)
      solver.WriteBack(sk->entities);
    doc->Regenerate();
    json after = DocumentIO::FeatureToJson(*sk);
    state.commands->Execute(
        std::make_unique<ModifyFeatureCommand>(index, before, after), *doc);
    return json::object();
  }
  if (m == "delete_entity") {
    // Remove a sketch entity by id, cleaning up dependent constraints; commit
    // one undoable edit. params: {sketch_id, entity_id}
    size_t index = 0;
    Feature* f = doc->FindFeatureById(p.at("sketch_id").get<uint64_t>(), &index);
    auto* sk = dynamic_cast<SketchFeature*>(f);
    if (!sk) throw std::runtime_error("no such sketch");
    json before = DocumentIO::FeatureToJson(*sk);
    const size_t dropped = sk->DeleteEntity(p.at("entity_id").get<uint32_t>());
    sketch::Solver solver;
    solver.Load(*sk);
    if (solver.Solve().ok)
      solver.WriteBack(sk->entities);
    doc->Regenerate();
    json after = DocumentIO::FeatureToJson(*sk);
    state.commands->Execute(
        std::make_unique<ModifyFeatureCommand>(index, before, after), *doc);
    return {{"dropped_constraints", dropped}};
  }
  if (m == "undo") {
    state.commands->Undo(*doc);
    return {{"can_undo", state.commands->CanUndo()}};
  }
  if (m == "redo") {
    state.commands->Redo(*doc);
    return {{"can_redo", state.commands->CanRedo()}};
  }
  if (m == "new_project") {
    ui::ReplaceDocument(state, std::make_unique<Document>());
    return json::object();
  }
  if (m == "save_project") {
    std::string error;
    if (!DocumentIO::Save(*doc, p.at("path").get<std::string>(), &error))
      throw std::runtime_error(error);
    return json::object();
  }
  if (m == "open_project") {
    std::string error;
    auto loaded = DocumentIO::Load(p.at("path").get<std::string>(), &error);
    if (!loaded) throw std::runtime_error(error);
    ui::ReplaceDocument(state, std::move(loaded));
    ui::FitCameraToDocument(state);
    return json::object();
  }
  if (m == "import_step") {
    const size_t before = doc->GetBodies().size();
    ui::LoadSTEPFile(state, p.at("path").get<std::string>());
    if (doc->GetBodies().size() <= before)
      throw std::runtime_error("import produced no bodies");
    return {{"bodies", doc->GetBodies().size() - before}};
  }
  if (m == "fit_view") {
    ui::FitCameraToDocument(state);
    return json::object();
  }
  if (m == "select_face") {
    const uint32_t mesh = p.at("mesh").get<uint32_t>();
    const uint32_t face = p.at("face").get<uint32_t>();
    uint32_t first = 0, count = 0;
    if (!state.faceSelector->GetFaceVertexRange(mesh, face, first, count))
      throw std::runtime_error("no such face");
    state.faceSelector->SetSelectedFace(mesh, face);
    state.meshRenderer->SetFaceHighlight(mesh, first, count);
    return {{"normal", Vec3ToJson(state.faceSelector->GetFaceNormal(mesh, face))},
            {"center", Vec3ToJson(state.faceSelector->GetFaceCenter(mesh, face))}};
  }
  if (m == "orient_to_face") {
    const Face* face = state.faceSelector->GetSelectedFace();
    if (!face) throw std::runtime_error("no face selected");
    state.camera->AlignToFace(
        state.faceSelector->GetFaceCenter(face->meshId, face->faceIndex),
        state.faceSelector->GetFaceNormal(face->meshId, face->faceIndex));
    return json::object();
  }

  handled = false;
  return json();
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
      ctx->ItemClick(p.at("ref").get<std::string>().c_str());
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
  m_uiActionInFlight = true;
  ImGuiTestEngine_QueueTest(m_engine, s_UiActionTest);
  return true;
}

void RemoteServer::ProcessPending(ui::AppState& state) {
  if (!m_running)
    return;

  // Finish an in-flight UI action once the engine is done with it
  if (m_uiActionInFlight && ImGuiTestEngine_IsTestQueueEmpty(m_engine)) {
    m_uiActionInFlight = false;
    m_uiAction = nullptr;
    if (s_UiActionTest->Output.Status == ImGuiTestStatus_Success) {
      Respond(m_uiActionId, json::object());
    } else {
      RespondError(m_uiActionId, "ui action failed (see app log)");
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
        Respond(request.id, result);
        continue;
      }
      if (request.method == "screenshot") {
        m_screenshotId = request.id;
        m_screenshotPath = request.params.at("path").get<std::string>();
        m_screenshotPending = true;
        return; // main loop captures after this frame renders
      }
      if (QueueUiAction(request)) {
        return; // response sent when the engine finishes the action
      }
      RespondError(request.id, "unknown method: " + request.method);
    } catch (const std::exception& e) {
      RespondError(request.id, e.what());
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
    Respond(m_screenshotId, {{"path", m_screenshotPath}});
  } else {
    RespondError(m_screenshotId, error);
  }
}
