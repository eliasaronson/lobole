#include "remote/RemoteServer.h"

#include "CADKernel.h"
#include "CADMaterials.h"
#include "Camera.h"
#include "FaceSelector.h"
#include "Logger.h"
#include "MeshExport.h"
#include "RayTracingRenderer.h"
#include "MeshRenderer.h"
#include "core/Autosaver.h"
#include "core/Checkpoints.h"
#include "core/Expression.h"
#include "core/GitVault.h"
#include "core/MassProps.h"
#include "core/Measure.h"
#include "core/DrawingExport.h"
#include "core/Serialization.h"
#include "remote/RemoteConfigurations.h"
#include "core/SketchFeature.h"
#include "core/SketchSolver.h"
#include "ui/AppActions.h"
#include "ui/PreviewFeature.h"

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

void RemoteServer::ReaderThread() {
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
      // undo/redo route to the assembly's snapshot history on an assembly tab.
      "undo", "redo",
      // Camera / rendering act on the viewport, not the part document, so they
      // work on an assembly tab too.
      "view",         "fit_view",         "screenshot"};
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
    json selection = nullptr;
    if (const Face* face =
            state.faceSelector ? state.faceSelector->GetSelectedFace()
                               : nullptr) {
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
  if (m == "select_feature") {
    // Select a feature by index (-1 clears). Drives the same selection the
    // feature tree sets, so the Properties panel shows/edits it.
    int index = p.value("index", -1);
    if (index >= 0 && index >= static_cast<int>(doc->FeatureCount()))
      throw std::runtime_error("feature index out of range");
    state.selectedFeature = index;
    return json::object();
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
  if (m == "set_material") {
    // Assign a material to a feature (by id). params: {feature_id, material}
    // where material is a library slug (e.g. "steel") or "" for default.
    // Commits one undoable edit and regenerates.
    size_t index = 0;
    Feature* f = doc->FindFeatureById(p.at("feature_id").get<uint64_t>(), &index);
    if (!f) throw std::runtime_error("no such feature");
    const std::string slug = p.value("material", std::string{});
    // A non-empty slug that resolves to DEFAULT is unknown (DEFAULT is never in
    // the slug table, so valid slugs always map to a specific material).
    if (!slug.empty() && CADMaterials::MaterialLibrary::FromSlug(slug) ==
                             CADMaterials::MaterialType::DEFAULT) {
      throw std::runtime_error("unknown material slug: " + slug);
    }
    json before = DocumentIO::FeatureToJson(*f);
    f->SetMaterial(slug);
    doc->Regenerate();
    json after = DocumentIO::FeatureToJson(*f);
    state.commands->Execute(
        std::make_unique<ModifyFeatureCommand>(index, before, after), *doc);
    return json::object();
  }
  if (m == "set_rollback") {
    // Move the rollback bar: regeneration executes features [0, index) only;
    // -1 (or >= count) = end. Not undoable — it is a view of the history,
    // not an edit of it. params: {index}
    doc->SetRollback(p.at("index").get<int>());
    return {{"rollback", doc->Rollback()}};
  }
  if (m == "suppress_feature") {
    // Toggle suppression (by feature id) as one undoable edit; a suppressed
    // feature is skipped by regeneration but keeps its parameters. params:
    // {feature_id, suppressed}
    size_t index = 0;
    Feature* f =
        doc->FindFeatureById(p.at("feature_id").get<uint64_t>(), &index);
    if (!f) throw std::runtime_error("no such feature");
    json before = DocumentIO::FeatureToJson(*f);
    f->SetSuppressed(p.at("suppressed").get<bool>());
    doc->Regenerate();
    json after = DocumentIO::FeatureToJson(*f);
    state.commands->Execute(
        std::make_unique<ModifyFeatureCommand>(index, before, after), *doc);
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
        {"collinear", sketch::ConstraintKind::Collinear},
        {"distance_x", sketch::ConstraintKind::DistanceX},
        {"distance_y", sketch::ConstraintKind::DistanceY},
        {"diameter", sketch::ConstraintKind::Diameter}};
    auto it = kinds.find(p.at("kind").get<std::string>());
    if (it == kinds.end()) throw std::runtime_error("unknown constraint kind");
    c.kind = it->second;
    c.value = p.value("value", 0.0);
    // Optional expression driving the value, evaluated against the document's
    // variables on every regeneration (angles in degrees, like the UI).
    c.valueExpr = p.value("expr", std::string{});
    if (!c.valueExpr.empty()) {
      // Evaluate now too: the immediate solve below runs before
      // Document::Regenerate refreshes dimension values (VisitExprs), and
      // solving with the default 0 collapses the geometry irrecoverably.
      ExprError err;
      const auto v = ExpressionEngine::Default().EvaluateText(
          c.valueExpr, doc->Symbols(), &err);
      if (!v) throw std::runtime_error("bad expr: " + err.message);
      c.value = (c.kind == sketch::ConstraintKind::Angle)
                    ? glm::radians(*v)
                    : *v;
    }
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
    sk->RegenerateDerived();
    doc->Regenerate();
    json after = DocumentIO::FeatureToJson(*sk);
    state.commands->Execute(
        std::make_unique<ModifyFeatureCommand>(index, before, after), *doc);
    return json::object();
  }
  if (m == "mirror" || m == "fillet" || m == "pattern" || m == "offset") {
    // Generative op on a sketch (by id), solve, and commit one edit.
    // mirror params:  {sketch_id, sources:[id,...], axis:lineId, link?}
    // fillet params:  {sketch_id, line1, line2, radius}
    // pattern params: {sketch_id, sources:[id,...], count, link?,
    //                  circular?, step:[dx,dy] | center:[x,y], angle}
    // offset params:  {sketch_id, chain:[id,...], distance, join_arc?}
    size_t index = 0;
    Feature* f = doc->FindFeatureById(p.at("sketch_id").get<uint64_t>(), &index);
    auto* sk = dynamic_cast<SketchFeature*>(f);
    if (!sk) throw std::runtime_error("no such sketch");

    json before = DocumentIO::FeatureToJson(*sk);
    json result = json::object();
    if (m == "mirror") {
      std::vector<uint32_t> sources;
      for (const auto& s : p.at("sources"))
        sources.push_back(s.get<uint32_t>());
      result["generated"] = sk->MirrorEntities(
          sources, p.at("axis").get<uint32_t>(), p.value("link", true));
    } else if (m == "pattern") {
      std::vector<uint32_t> sources;
      for (const auto& s : p.at("sources"))
        sources.push_back(s.get<uint32_t>());
      const int count = p.at("count").get<int>();
      const bool link = p.value("link", true);
      if (p.value("circular", false)) {
        const auto& c = p.at("center");
        result["generated"] = sk->AddCircularPattern(
            sources, {c[0].get<float>(), c[1].get<float>()},
            p.at("angle").get<double>(), count, link);
      } else {
        const auto& s = p.at("step");
        result["generated"] = sk->AddLinearPattern(
            sources, {s[0].get<float>(), s[1].get<float>()}, count, link);
      }
    } else if (m == "offset") {
      std::vector<uint32_t> chain;
      for (const auto& s : p.at("chain"))
        chain.push_back(s.get<uint32_t>());
      result["generated"] = sk->OffsetChain(
          chain, p.at("distance").get<double>(), p.value("join_arc", true));
    } else {
      result["arc_id"] =
          sk->FilletCorner(p.at("line1").get<uint32_t>(),
                           p.at("line2").get<uint32_t>(),
                           p.at("radius").get<double>());
    }
    sketch::Solver solver;
    solver.Load(*sk);
    if (solver.Solve().ok)
      solver.WriteBack(sk->entities);
    sk->RegenerateDerived();
    doc->Regenerate();
    json after = DocumentIO::FeatureToJson(*sk);
    state.commands->Execute(
        std::make_unique<ModifyFeatureCommand>(index, before, after), *doc);
    return result;
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
    sk->RegenerateDerived();
    doc->Regenerate();
    json after = DocumentIO::FeatureToJson(*sk);
    state.commands->Execute(
        std::make_unique<ModifyFeatureCommand>(index, before, after), *doc);
    return {{"dropped_constraints", dropped}};
  }
  if (m == "set_variable") {
    // Add or redefine a document variable; one undoable edit, regenerates
    // every expression-driven field. params: {name, expression, comment?}.
    // Returns the evaluated value (plus "error" if it didn't evaluate).
    const std::string name = p.at("name").get<std::string>();
    const json& expr = p.at("expression");
    const std::string expression =
        expr.is_string() ? expr.get<std::string>() : expr.dump();
    // Omitting "comment" preserves an existing one (redefinitions usually
    // only change the expression).
    std::string comment;
    if (p.contains("comment")) {
      comment = p.at("comment").get<std::string>();
    } else if (const Parameter* prior = doc->FindParameter(name)) {
      comment = prior->comment;
    }
    if (!state.commands->Execute(
            std::make_unique<SetVariableCommand>(name, expression, comment),
            *doc)) {
      throw std::runtime_error("invalid variable name: " + name);
    }
    const Parameter* param = doc->FindParameter(name);
    json result = {{"value", param->value}};
    if (!param->error.empty()) result["error"] = param->error;
    return result;
  }
  if (m == "delete_variable") {
    // Refused while referenced, reporting what references it. params: {name}
    const std::string name = p.at("name").get<std::string>();
    if (doc->FindParameter(name) == nullptr)
      throw std::runtime_error("no such variable: " + name);
    std::vector<std::string> refs = doc->FindParameterReferences(name);
    if (!refs.empty()) {
      std::string msg = "variable is referenced by:";
      for (const std::string& r : refs) msg += " " + r + ";";
      throw std::runtime_error(msg);
    }
    state.commands->Execute(std::make_unique<DeleteVariableCommand>(name),
                            *doc);
    return json::object();
  }
  if (m == "rename_variable") {
    // Renames + rewrites every referencing expression. params: {name, new_name}
    if (!state.commands->Execute(
            std::make_unique<RenameVariableCommand>(
                p.at("name").get<std::string>(),
                p.at("new_name").get<std::string>()),
            *doc)) {
      throw std::runtime_error("rename failed (unknown, invalid, or taken)");
    }
    return json::object();
  }
  if (m == "list_variables") {
    json variables = json::array();
    for (const Parameter& param : doc->GetParameters()) {
      json entry = {{"name", param.name},
                    {"expression", param.expression},
                    {"value", param.value}};
      if (!param.comment.empty()) entry["comment"] = param.comment;
      if (!param.error.empty()) entry["error"] = param.error;
      variables.push_back(entry);
    }
    return {{"variables", variables}};
  }

  // ---- FEM studies (v29) --------------------------------------------------
  // A study is document-level analysis config (loads/refinements on faces).
  // All mutations go through Commands (undoable); every one re-snapshots the
  // whole study, matching ModifyFeatureCommand's idempotent before/after shape.
  if (m == "add_study") {
    auto cmd = std::make_unique<AddStudyCommand>(p.value("name", std::string{}));
    AddStudyCommand* raw = cmd.get();
    state.commands->Execute(std::move(cmd), *doc);
    const fem::Study* s = doc->FindStudy(raw->StudyId());
    return {{"id", s->id}, {"name", s->name}};
  }
  if (m == "remove_study") {
    const uint64_t id = p.at("study").get<uint64_t>();
    if (!doc->FindStudy(id)) throw std::runtime_error("no such study");
    state.commands->Execute(std::make_unique<RemoveStudyCommand>(id), *doc);
    return json::object();
  }
  if (m == "list_studies") {
    json studies = json::array();
    for (const fem::Study& s : doc->GetStudies()) {
      // Report evaluated sizes/magnitudes (against current parameters) without
      // mutating the stored study.
      fem::Study eval = s;
      fem::EvaluateStudyExprs(eval, doc->Symbols(), nullptr);
      json loads = json::array();
      for (const fem::Load& l : eval.loads) {
        json lj = {{"id", l.id},
                   {"type", fem::LoadTypeToString(l.type)},
                   {"face", {{"feature_id", l.face.featureId},
                             {"element", l.face.name}}}};
        if (l.type != fem::LoadType::Fixed) lj["magnitude"] = l.magnitude;
        if (l.type == fem::LoadType::Force)
          lj["direction"] = {l.direction.x, l.direction.y, l.direction.z};
        loads.push_back(std::move(lj));
      }
      studies.push_back({{"id", s.id},
                         {"name", s.name},
                         {"mesh_size", eval.meshSize},
                         {"loads", loads},
                         {"refinements", eval.refinements.size()},
                         {"has_results", doc->StudyResults(s.id) != nullptr},
                         {"stale", doc->StudyResultsStale(s.id)}});
    }
    return {{"studies", studies}};
  }

  // Face-ref decode shared by the study mutation RPCs.
  auto studyFaceOf = [](const json& r) {
    return ElementRef{r.at("feature_id").get<uint64_t>(),
                      r.at("element").get<std::string>()};
  };
  // Runs one study mutation as a ModifyStudyCommand: snapshot before, apply
  // `mutate` to a copy, snapshot after, execute.
  auto modifyStudy = [&](uint64_t id, const std::string& desc,
                         const std::function<void(fem::Study&)>& mutate) {
    const fem::Study* cur = doc->FindStudy(id);
    if (!cur) throw std::runtime_error("no such study");
    json before = DocumentIO::StudyToJson(*cur);
    fem::Study copy = *cur;
    mutate(copy);
    json after = DocumentIO::StudyToJson(copy);
    state.commands->Execute(
        std::make_unique<ModifyStudyCommand>(desc, before, after), *doc);
  };

  if (m == "set_study") {
    const uint64_t id = p.at("study").get<uint64_t>();
    modifyStudy(id, "Edit study", [&](fem::Study& s) {
      if (p.contains("name")) s.name = p.at("name").get<std::string>();
      if (p.contains("mesh_size")) {
        const json& v = p.at("mesh_size");
        if (v.is_string()) {
          s.meshSizeExpr = v.get<std::string>();  // evaluated at mesh time
        } else {
          s.meshSize = v.get<double>();
          s.meshSizeExpr.clear();
        }
      }
      if (p.contains("type") &&
          !fem::AnalysisTypeFromString(p.at("type").get<std::string>(), s.type))
        throw std::runtime_error("analysis type must be static or modal");
      if (p.contains("num_modes")) s.numModes = p.at("num_modes").get<int>();
      if (p.contains("gravity")) {
        const auto& g = p.at("gravity");
        s.gravity = glm::dvec3(g.at(0).get<double>(), g.at(1).get<double>(),
                               g.at(2).get<double>());
      }
    });
    return json::object();
  }
  if (m == "add_refinement") {
    const uint64_t id = p.at("study").get<uint64_t>();
    fem::Refinement r;
    r.face = studyFaceOf(p.at("face"));
    const json& v = p.at("size");
    if (v.is_string()) r.sizeExpr = v.get<std::string>();
    else r.size = v.get<double>();
    modifyStudy(id, "Add refinement",
                [&](fem::Study& s) { s.refinements.push_back(r); });
    return json::object();
  }
  if (m == "remove_refinement") {
    const uint64_t id = p.at("study").get<uint64_t>();
    const ElementRef face = studyFaceOf(p.at("face"));
    modifyStudy(id, "Remove refinement", [&](fem::Study& s) {
      s.refinements.erase(
          std::remove_if(s.refinements.begin(), s.refinements.end(),
                         [&](const fem::Refinement& r) {
                           return r.face.featureId == face.featureId &&
                                  r.face.name == face.name;
                         }),
          s.refinements.end());
    });
    return json::object();
  }
  if (m == "add_load") {
    const uint64_t id = p.at("study").get<uint64_t>();
    fem::Load l;
    l.id = doc->AllocateId();
    if (!fem::LoadTypeFromString(p.at("type").get<std::string>(), l.type))
      throw std::runtime_error("load type must be fixed, force, or pressure");
    l.face = studyFaceOf(p.at("face"));
    if (p.contains("magnitude")) {
      const json& v = p.at("magnitude");
      if (v.is_string()) l.magnitudeExpr = v.get<std::string>();
      else l.magnitude = v.get<double>();
    }
    if (p.contains("direction")) {
      const auto& d = p.at("direction");
      l.direction = glm::dvec3(d.at(0).get<double>(), d.at(1).get<double>(),
                               d.at(2).get<double>());
    }
    modifyStudy(id, "Add load", [&](fem::Study& s) { s.loads.push_back(l); });
    return {{"id", l.id}};
  }
  if (m == "remove_load") {
    const uint64_t id = p.at("study").get<uint64_t>();
    const uint64_t loadId = p.at("load").get<uint64_t>();
    modifyStudy(id, "Remove load", [&](fem::Study& s) {
      s.loads.erase(std::remove_if(s.loads.begin(), s.loads.end(),
                                   [&](const fem::Load& l) { return l.id == loadId; }),
                    s.loads.end());
    });
    return json::object();
  }
  if (m == "mesh_study") {
    // Mesh (no solve) every body the study touches; a headless milestone
    // before the solver exists. params: {study}
    const uint64_t id = p.at("study").get<uint64_t>();
    fem::Study* s = doc->FindStudy(id);
    if (!s) throw std::runtime_error("no such study");
    std::vector<fem::StudyBodyMesh> meshes;
    std::string err;
    if (!fem::MeshStudy(*doc, *s, meshes, &err)) throw std::runtime_error(err);
    json bodies = json::array();
    for (const fem::StudyBodyMesh& bm : meshes) {
      bodies.push_back({{"body", bm.bodyName},
                        {"nodes", bm.mesh.stats.nodeCount},
                        {"elements", bm.mesh.stats.tetCount},
                        {"worst_quality", bm.mesh.stats.worstQuality}});
    }
    return {{"bodies", bodies}};
  }
  if (m == "run_study" || m == "study_results") {
    // run_study: mesh + solve now (explicit act, never automatic) and cache the
    // results. study_results: read the cached results (never solves). Both
    // return the same summary. params: {study}
    const uint64_t id = p.at("study").get<uint64_t>();
    fem::Study* s = doc->FindStudy(id);
    if (!s) throw std::runtime_error("no such study");

    if (m == "run_study") {
      std::string err;
      if (!ui::RunStudyAndCache(*doc, id, &err)) throw std::runtime_error(err);
    }

    const fem::StudyRuntime* rt = doc->StudyResults(id);
    if (!rt) throw std::runtime_error("study has not been run");

    // Modal studies report per-body natural frequencies instead of stress.
    if (s->type == fem::AnalysisType::Modal) {
      std::size_t nodes = 0, elements = 0;
      json bodies = json::array();
      for (const fem::StudyBodyResult& b : rt->bodies) {
        nodes += b.mesh.stats.nodeCount;
        elements += b.mesh.stats.tetCount;
        json freqs = json::array();
        for (const auto& mode : b.results.modes) freqs.push_back(mode.frequencyHz);
        bodies.push_back({{"body", b.bodyName}, {"frequencies_hz", freqs}});
      }
      return {{"analysis", "modal"},
              {"nodes", nodes},
              {"elements", elements},
              {"bodies", bodies},
              {"stale", doc->StudyResultsStale(id)}};
    }

    // Aggregate mesh size and a factor of safety (min over bodies with yield
    // data) from the governing von Mises.
    std::size_t nodes = 0, elements = 0;
    double minFoS = 0.0;
    bool haveFoS = false;
    json perBody = json::array();
    for (const fem::StudyBodyResult& b : rt->bodies) {
      nodes += b.mesh.stats.nodeCount;
      elements += b.mesh.stats.tetCount;
      json bj = {{"body", b.bodyName},
                 {"max_von_mises_mpa", b.results.maxVonMises},
                 {"max_displacement_mm", b.results.maxDisplacement}};
      // Factor of safety against yield, when the body's material defines one.
      if (b.bodyIndex >= 0 && b.bodyIndex < (int)doc->GetBodies().size() &&
          b.results.maxVonMises > 0) {
        const auto mt = CADMaterials::MaterialLibrary::FromSlug(
            doc->GetBodies()[b.bodyIndex].material);
        const float yield = CADMaterials::MaterialLibrary::GetMaterial(mt).yieldMPa;
        if (yield > 0) {
          const double fos = yield / b.results.maxVonMises;
          bj["factor_of_safety"] = fos;
          if (!haveFoS || fos < minFoS) minFoS = fos;
          haveFoS = true;
        }
      }
      perBody.push_back(std::move(bj));
    }

    json result = {
        {"nodes", nodes},
        {"elements", elements},
        {"max_von_mises_mpa",
         {{"value", rt->maxVonMises},
          {"at", {rt->maxVmAt.x, rt->maxVmAt.y, rt->maxVmAt.z}},
          {"body", rt->maxVmBody}}},
        {"max_displacement_mm",
         {{"value", rt->maxDisplacement},
          {"at", {rt->maxDispAt.x, rt->maxDispAt.y, rt->maxDispAt.z}},
          {"body", rt->maxDispBody}}},
        {"bodies", perBody},
        {"stale", doc->StudyResultsStale(id)}};
    if (haveFoS) result["factor_of_safety"] = minFoS;
    return result;
  }
  if (m == "enter_simulation") {
    // Enter the FEM simulation workspace (the GUI mode), optionally selecting a
    // study. The LLM drives the same mode a human enters from the toolbar.
    // params: {study?}
    ui::EnterSimulation(state);
    if (p.contains("study")) {
      const uint64_t id = p.at("study").get<uint64_t>();
      if (!doc->FindStudy(id)) throw std::runtime_error("no such study");
      state.simulation.activeStudyId = id;
    }
    return {{"active", state.simulation.active},
            {"study", state.simulation.activeStudyId}};
  }
  if (m == "exit_simulation") {
    ui::ExitSimulation(state);
    return json::object();
  }
  if (m == "set_result_display") {
    // Show/warp a study's result overlay (display only, like set_explode).
    // params: {study?, field: "von_mises"|"displacement"|"none", deform_scale?}
    ui::EnterSimulation(state);
    if (p.contains("study")) {
      const uint64_t id = p.at("study").get<uint64_t>();
      if (!doc->FindStudy(id)) throw std::runtime_error("no such study");
      state.simulation.activeStudyId = id;
    }
    const std::string f = p.value("field", std::string("von_mises"));
    state.simulation.field =
        f == "displacement" ? ui::SimulationState::Field::Displacement
        : f == "none"       ? ui::SimulationState::Field::None
                            : ui::SimulationState::Field::VonMises;
    if (p.contains("deform_scale"))
      state.simulation.deformScale = static_cast<float>(p.at("deform_scale").get<double>());
    if (p.contains("mode"))  // modal: which mode shape to display (0-based)
      state.simulation.activeMode = p.at("mode").get<int>();
    return {{"field", f},
            {"deform_scale", state.simulation.deformScale},
            {"mode", state.simulation.activeMode}};
  }

  if (m == "set_section") {
    // Toggle/position the viewport section clip plane (I3 raster). params:
    // {enabled, base_plane: "xy"|"xz"|"yz", offset?} or {enabled, origin,
    // normal}. Disabled clears it.
    if (!p.value("enabled", true)) {
      state.sectionPlane = glm::vec4(0.0f);
      return json::object();
    }
    glm::vec3 nrm(0, 0, 1), origin(0.0f);
    if (p.contains("base_plane")) {
      const std::string bp = p.at("base_plane").get<std::string>();
      nrm = bp == "xz" ? glm::vec3(0, 1, 0)
            : bp == "yz" ? glm::vec3(1, 0, 0)
                         : glm::vec3(0, 0, 1);
      origin = nrm * static_cast<float>(p.value("offset", 0.0));
    } else {
      const auto& o = p.at("origin");
      const auto& n = p.at("normal");
      origin = {o[0].get<float>(), o[1].get<float>(), o[2].get<float>()};
      nrm = glm::normalize(glm::vec3(n[0].get<float>(), n[1].get<float>(),
                                     n[2].get<float>()));
    }
    state.sectionPlane = glm::vec4(nrm, glm::dot(origin, nrm));
    return json::object();
  }
  if (m == "section") {
    // Cross-section area where a plane cuts through all bodies (I3). Plane:
    // {base_plane: "xy"|"xz"|"yz", offset?: 0} OR {origin: [x,y,z],
    // normal: [x,y,z]}.
    glm::vec3 origin(0.0f), normal(0.0f, 0.0f, 1.0f);
    if (p.contains("base_plane")) {
      const std::string bp = p.at("base_plane").get<std::string>();
      normal = bp == "xz" ? glm::vec3(0, 1, 0)
               : bp == "yz" ? glm::vec3(1, 0, 0)
                            : glm::vec3(0, 0, 1);
      origin = normal * static_cast<float>(p.value("offset", 0.0));
    } else {
      const auto& o = p.at("origin");
      const auto& nrm = p.at("normal");
      origin = {o[0].get<float>(), o[1].get<float>(), o[2].get<float>()};
      normal = {nrm[0].get<float>(), nrm[1].get<float>(),
                nrm[2].get<float>()};
    }
    const auto area = Measure::CrossSectionArea(doc->GetBodies(), origin,
                                                normal);
    if (!area) throw std::runtime_error("section failed");
    return {{"area", *area}};
  }
  if (m == "interference") {
    // Clash detection (inspection I4): two bodies interfere when their
    // intersection has volume. params: {a: index, b: index}
    const auto& bodies = doc->GetBodies();
    const int ia = p.at("a").get<int>();
    const int ib = p.at("b").get<int>();
    if (ia < 0 || ib < 0 || ia >= static_cast<int>(bodies.size()) ||
        ib >= static_cast<int>(bodies.size()))
      throw std::runtime_error("body index out of range");
    CADShape common = bodies[ia].shape.Intersection(bodies[ib].shape);
    const double v = common.IsValid() ? common.Volume() : 0.0;
    return {{"clash", v > 1e-9}, {"volume", v}};
  }
  if (m == "mass_properties") {
    // Volume, surface area, center of mass, and inertia (about the CoM) per
    // body, plus a document total. Mass assumes model units are millimetres:
    // grams = density[kg/m^3] * volume[mm^3] * 1e-6; bodies without an
    // assigned material report mass null (density unknown), and so does the
    // total if any contributor is unknown. params: {body?: index}
    const auto& bodies = doc->GetBodies();
    const int only = p.value("body", -1);
    if (only >= static_cast<int>(bodies.size()))
      throw std::runtime_error("body index out of range");
    // The math is shared with the BOM (A4): Measure::MassOfBodies. A single
    // body's Body copy is cheap (the shape is a refcounted OCC handle).
    std::vector<Body> subset;
    if (only >= 0) subset.push_back(bodies[only]);
    std::string err;
    const auto mass = Measure::MassOfBodies(only >= 0 ? subset : bodies, &err);
    if (!mass) throw std::runtime_error(err);
    if (mass->bodies.empty()) throw std::runtime_error("no bodies");
    json out = json::array();
    for (size_t i = 0; i < mass->bodies.size(); i++) {
      const Measure::BodyMass& bm = mass->bodies[i];
      json inertia = json::array();
      for (int r = 0; r < 3; r++)
        inertia.push_back({bm.inertia[r][0], bm.inertia[r][1],
                           bm.inertia[r][2]});
      json entry = {{"body", only >= 0 ? static_cast<size_t>(only) : i},
                    {"name", bm.name},
                    {"volume", bm.volume},
                    {"surface_area", bm.surfaceArea},
                    {"center_of_mass",
                     {bm.centerOfMass.x, bm.centerOfMass.y, bm.centerOfMass.z}},
                    {"inertia", inertia},
                    {"material", bm.material}};
      if (bm.massKnown) {
        entry["density"] = bm.density;
        entry["mass_g"] = bm.massG;
      } else {
        entry["mass_g"] = nullptr;
      }
      out.push_back(entry);
    }
    const glm::dvec3& com = mass->centerOfMass;
    return {{"bodies", out},
            {"total",
             {{"volume", mass->volume},
              {"center_of_mass", {com.x, com.y, com.z}},
              {"mass_g", mass->massKnown ? json(mass->massG) : json(nullptr)}}}};
  }
  if (m == "measure") {
    // Geometry queries over stable element names (I1). params:
    // {a: {feature_id, element}, b?: {feature_id, element}}.
    // With `b`: minimum distance between the two elements. Without: the
    // element's own measure (face area / edge length).
    auto refOf = [](const json& r) {
      return ElementRef{r.at("feature_id").get<uint64_t>(),
                        r.at("element").get<std::string>()};
    };
    const ElementRef a = refOf(p.at("a"));
    const auto& bodies = doc->GetBodies();
    if (p.contains("b")) {
      const auto d = Measure::Distance(bodies, a, refOf(p.at("b")));
      if (!d) throw std::runtime_error("measure: an element did not resolve");
      return {{"distance", *d}};
    }
    const auto e = Measure::Describe(bodies, a);
    if (!e) throw std::runtime_error("measure: element did not resolve");
    json out = {{"kind", e->kind}};
    if (e->kind == "face") {
      out["area"] = e->area;
    } else {
      out["length"] = e->length;
      if (e->hasRadius) out["radius"] = e->radius; // circular/arc edges
    }
    return out;
  }
  if (m == "list_elements") {
    // Stable element names (topological naming) for each body's faces and
    // edges — what Fillet/Chamfer edge refs {feature_id, element} point at
    // (feature_id = the body's created_by). Document-only: works headless.
    // params: {body?: index}
    const auto& bodies = doc->GetBodies();
    const int only = p.value("body", -1);
    if (only >= static_cast<int>(bodies.size()))
      throw std::runtime_error("body index out of range");
    json out = json::array();
    for (size_t i = 0; i < bodies.size(); i++) {
      if (only >= 0 && i != static_cast<size_t>(only)) continue;
      json faces = json::array(), edges = json::array();
      for (const auto& [idx, name] : bodies[i].elementMap.names)
        faces.push_back({{"index", idx}, {"name", name}});
      for (const auto& [idx, name] : bodies[i].elementMap.edgeNames)
        edges.push_back({{"index", idx}, {"name", name}});
      out.push_back({{"body", i},
                     {"name", bodies[i].name},
                     {"created_by", bodies[i].createdBy},
                     {"faces", faces},
                     {"edges", edges}});
    }
    return {{"bodies", out}};
  }
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
      arr.push_back({{"name", state.parts[i].name},
                     {"index", i},
                     {"id", state.parts[i].id},
                     {"type", type}});
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
  if (m == "create_drawing") {
    // Adds a drawing tab. Optional `sheet` seeds paper size / orientation /
    // title-block fields so a headless caller can set them in one call.
    const std::string name =
        p.value("name", "Drawing " + std::to_string(state.parts.size() + 1));
    const size_t idx = ui::AddDrawing(state, name);
    if (p.contains("sheet") && state.drawing) {
      const json& sj = p.at("sheet");
      Sheet& sheet = state.drawing->GetSheet();
      SheetSize size;
      if (sj.contains("size") &&
          SheetSizeFromString(sj.at("size").get<std::string>(), size))
        sheet.size = size;
      sheet.landscape = sj.value("landscape", sheet.landscape);
      if (sj.contains("title_block")) {
        const json& tb = sj.at("title_block");
        sheet.titleBlock.project = tb.value("project", sheet.titleBlock.project);
        sheet.titleBlock.document = tb.value("document", sheet.titleBlock.document);
        sheet.titleBlock.author = tb.value("author", sheet.titleBlock.author);
        sheet.titleBlock.date = tb.value("date", sheet.titleBlock.date);
        sheet.titleBlock.notes = tb.value("notes", sheet.titleBlock.notes);
      }
      state.drawing->BumpVersion();
    }
    return {{"index", idx},
            {"name", name},
            {"id", state.parts[idx].id},
            {"size", SheetSizeToString(state.drawing->GetSheet().size)}};
  }
  if (m == "list_drawings") {
    ui::EnsureParts(state);
    json arr = json::array();
    for (size_t i = 0; i < state.parts.size(); i++) {
      if (state.parts[i].kind != ui::DocKind::Drawing) continue;
      // The active drawing lives in AppState; a stashed one in its slot.
      const DrawingDocument* dwg =
          static_cast<int>(i) == state.activePart
              ? state.drawing.get()
              : state.parts[i].drawing.get();
      arr.push_back({{"name", state.parts[i].name},
                     {"index", i},
                     {"id", state.parts[i].id},
                     {"views", dwg ? dwg->ViewCount() : 0},
                     {"dims", dwg ? dwg->DimCount() : 0}});
    }
    return {{"drawings", arr}};
  }
  if (m == "add_drawing_view") {
    if (!state.drawing)
      throw std::runtime_error("the active document is not a drawing");
    // `source` = the source document's NAME (a part or an assembly), resolved
    // to its stable id.
    const std::string sourceName = p.at("source").get<std::string>();
    uint64_t sourceId = 0;
    for (const auto& slot : state.parts) {
      if (slot.name == sourceName && (slot.kind == ui::DocKind::Part ||
                                      slot.kind == ui::DocKind::Assembly)) {
        sourceId = slot.id;
        break;
      }
    }
    if (sourceId == 0)
      throw std::runtime_error("no part or assembly named '" + sourceName + "'");
    DrawingView v;
    v.sourceDocId = sourceId;
    if (p.contains("orientation") &&
        !ViewOrientationFromString(p.at("orientation").get<std::string>(),
                                   v.orientation))
      throw std::runtime_error("unknown orientation '" +
                               p.at("orientation").get<std::string>() + "'");
    // Default the label to the orientation (front/top/iso…) — clearer on the
    // sheet than repeating the source name across every view.
    v.name = p.value("name", std::string(ViewOrientationToString(v.orientation)));
    v.baseViewId = p.value("base_view", uint64_t{0});
    v.scale = p.value("scale", 1.0);
    if (p.contains("position")) {
      const json& pos = p.at("position");
      v.position = {pos.at(0).get<double>(), pos.at(1).get<double>()};
    } else {  // default to the sheet centre
      glm::dvec2 ext = SheetExtentMM(state.drawing->GetSheet().size);
      if (state.drawing->GetSheet().landscape) ext = {ext.y, ext.x};
      v.position = ext * 0.5;
    }
    ui::SnapshotDrawing(state);
    const uint64_t id = state.drawing->AddView(v);
    // Projection is explicit (design §2, the FEM rule): the view is added now;
    // call update_drawing to project it.
    return {{"id", id}, {"source_id", sourceId}, {"needs_update", true}};
  }
  if (m == "remove_drawing_view") {
    if (!state.drawing)
      throw std::runtime_error("the active document is not a drawing");
    ui::SnapshotDrawing(state);
    if (!state.drawing->RemoveView(p.at("id").get<uint64_t>())) {
      state.drawingHistory.undo.pop_back();  // nothing changed; drop the snapshot
      throw std::runtime_error("no such view");
    }
    ui::UpdateDrawingRenders(state);  // drop the removed view's geometry
    return json::object();
  }
  if (m == "update_drawing") {
    if (!state.drawing)
      throw std::runtime_error("the active document is not a drawing");
    ui::UpdateDrawingRenders(state);  // the explicit re-projection
    json views = json::array();
    for (const DrawingView& view : state.drawing->GetViews()) {
      json vj = {{"id", view.id}, {"name", view.name}};
      const auto it = state.drawingRenders.find(view.id);
      if (it != state.drawingRenders.end()) {
        int vis = 0, hid = 0, outline = 0;
        for (const drawing::SheetCurve& c : it->second.curves) {
          if (c.visible) ++vis; else ++hid;
          if (c.kind == drawing::CurveKind::Outline) ++outline;
        }
        vj["curves"] = it->second.curves.size();
        vj["visible"] = vis;
        vj["hidden"] = hid;
        vj["silhouettes"] = outline;
        vj["has_geometry"] = it->second.hasGeometry;
        if (!it->second.error.empty()) vj["error"] = it->second.error;
      }
      views.push_back(vj);
    }
    return {{"views", views}};
  }
  if (m == "list_drawing_views") {
    if (!state.drawing)
      throw std::runtime_error("the active document is not a drawing");
    json arr = json::array();
    for (const DrawingView& view : state.drawing->GetViews()) {
      json vj = {{"id", view.id},
                 {"name", view.name},
                 {"source_id", view.sourceDocId},
                 {"orientation", ViewOrientationToString(view.orientation)},
                 {"scale", view.scale},
                 {"base_view", view.baseViewId},
                 {"position", {view.position.x, view.position.y}},
                 {"show_hidden", view.showHidden}};
      const auto it = state.drawingRenders.find(view.id);
      if (it != state.drawingRenders.end()) {
        vj["curves"] = it->second.curves.size();
        vj["has_geometry"] = it->second.hasGeometry;
        vj["stale"] = ui::DrawingSourceVersion(state, view.sourceDocId) !=
                      it->second.solvedSourceVersion;
        if (!it->second.error.empty()) vj["error"] = it->second.error;
      } else {
        vj["curves"] = 0;
        vj["has_geometry"] = false;
        vj["stale"] = true;  // never projected
      }
      arr.push_back(vj);
    }
    return {{"views", arr}};
  }
  if (m == "export_drawing") {
    if (!state.drawing)
      throw std::runtime_error("the active document is not a drawing");
    const std::string path = p.at("path").get<std::string>();
    std::string err;
    if (!ui::ExportActiveDrawing(state, path, &err))
      throw std::runtime_error(err);
    return {{"path", path}, {"views", state.drawing->ViewCount()}};
  }
  if (m == "list_drawing_geometry") {
    // Discovery for dimensioning: the distinct source edges/silhouettes of a
    // view, each with the ref to pass to add_drawing_dim and a summary of its
    // sheet-mm geometry (endpoints, or circle centre + radius).
    if (!state.drawing)
      throw std::runtime_error("the active document is not a drawing");
    const uint64_t viewId = p.at("view").get<uint64_t>();
    const auto it = state.drawingRenders.find(viewId);
    if (it == state.drawingRenders.end())
      throw std::runtime_error("view not projected; call update_drawing first");
    // Deduplicate curves by their source ref (an edge splits into visible +
    // hidden segments); keep the first geometry seen.
    json edges = json::array();
    std::set<std::string> seen;
    for (const drawing::SheetCurve& c : it->second.curves) {
      const bool isEdge = c.edge.IsSet();
      const ElementRef& ref = isEdge ? c.edge : c.silFace;
      if (!ref.IsSet()) continue;
      const std::string key = std::to_string(c.sourceId) + "|" +
                              (isEdge ? "e|" : "s|") +
                              std::to_string(ref.featureId) + "|" + ref.name;
      if (!seen.insert(key).second) continue;
      json e = {{"kind", isEdge ? "edge" : "silhouette"},
                {"source_id", c.sourceId},
                {"ref", {{"feature_id", ref.featureId}, {"element", ref.name}}}};
      if (c.conicRadius > 0.0) {
        e["type"] = "circle";
        e["center"] = {c.conicCenter.x, c.conicCenter.y};
        e["radius"] = c.conicRadius;
      } else if (c.points.size() >= 2) {
        e["type"] = c.curveType == 0 ? "line" : "curve";
        e["p1"] = {c.points.front().x, c.points.front().y};
        e["p2"] = {c.points.back().x, c.points.back().y};
      }
      edges.push_back(std::move(e));
    }
    return {{"geometry", edges}};
  }
  if (m == "add_drawing_dim") {
    if (!state.drawing)
      throw std::runtime_error("the active document is not a drawing");
    const uint64_t viewId = p.at("view").get<uint64_t>();
    if (!state.drawing->FindView(viewId))
      throw std::runtime_error("no such view");
    DrawingDim dim;
    dim.viewId = viewId;
    if (!DrawingDimKindFromString(p.value("kind", std::string("distance")),
                                  dim.kind))
      throw std::runtime_error("unknown dimension kind '" +
                               p.value("kind", std::string()) + "'");
    // refs: [{edge|sil_face: {feature_id, element}, end?, occurrence?}, ...]
    for (const json& rj : p.at("refs")) {
      ProjRef r;
      r.viewId = viewId;
      r.end = rj.value("end", 0);
      r.occurrenceId = rj.value("occurrence", uint64_t{0});
      if (rj.contains("edge"))
        r.edge = ElementRef{rj.at("edge").value("feature_id", uint64_t{0}),
                            rj.at("edge").value("element", std::string{})};
      if (rj.contains("sil_face"))
        r.silFace = ElementRef{rj.at("sil_face").value("feature_id", uint64_t{0}),
                               rj.at("sil_face").value("element", std::string{})};
      dim.refs.push_back(r);
    }
    if (p.contains("offset")) {
      const json& o = p.at("offset");
      dim.labelOffset = {o.at(0).get<double>(), o.at(1).get<double>()};
    }
    ui::SnapshotDrawing(state);
    const uint64_t id = state.drawing->AddDim(dim);
    // Echo the resolved value now (the view is already projected).
    const drawing::ResolvedDim rd = ui::ResolveDrawingDim(state, *state.drawing->FindDim(id));
    return {{"id", id},
            {"resolved", rd.resolved},
            {"value", rd.value},
            {"text", rd.text}};
  }
  if (m == "remove_drawing_dim") {
    if (!state.drawing)
      throw std::runtime_error("the active document is not a drawing");
    ui::SnapshotDrawing(state);
    if (!state.drawing->RemoveDim(p.at("id").get<uint64_t>())) {
      state.drawingHistory.undo.pop_back();
      throw std::runtime_error("no such dimension");
    }
    return json::object();
  }
  if (m == "list_drawing_dims") {
    if (!state.drawing)
      throw std::runtime_error("the active document is not a drawing");
    json arr = json::array();
    for (const DrawingDim& dim : state.drawing->GetDims()) {
      const drawing::ResolvedDim rd = ui::ResolveDrawingDim(state, dim);
      arr.push_back({{"id", dim.id},
                     {"kind", DrawingDimKindToString(dim.kind)},
                     {"view", dim.viewId},
                     {"resolved", rd.resolved},
                     {"value", rd.value},
                     {"text", rd.text}});
    }
    return {{"dims", arr}};
  }
  if (m == "add_occurrence") {
    // Places a part in the active assembly. `part` is the part's NAME (resolved
    // to its stable id); optional `transform` = {position:[x,y,z],
    // orientation:[x,y,z,w]}.
    if (!state.assembly)
      throw std::runtime_error("the active document is not an assembly");
    const std::string partName = p.at("part").get<std::string>();
    const uint64_t partId = ui::PartIdByName(state, partName);
    if (partId == 0)
      throw std::runtime_error("no part named '" + partName + "'");
    const json identityTransform = {{"position", {0.0, 0.0, 0.0}},
                                    {"orientation", {0.0, 0.0, 0.0, 1.0}}};
    // Build one occurrence-JSON and reuse the loader (id is reassigned on add).
    Occurrence occ = DocumentIO::OccurrenceFromJson(
        {{"id", 0},
         {"name", p.value("name", partName)},
         {"part_id", partId},
         {"part_version", nullptr},
         {"transform", p.value("transform", identityTransform)},
         {"visible", p.value("visible", true)},
         {"fixed", p.value("fixed", false)}});
    ui::SnapshotAssembly(state);
    const uint64_t id = state.assembly->AddOccurrence(occ);
    return {{"id", id}, {"part_id", partId}};
  }
  if (m == "remove_occurrence") {
    if (!state.assembly)
      throw std::runtime_error("the active document is not an assembly");
    ui::SnapshotAssembly(state);
    if (!state.assembly->RemoveOccurrence(p.at("id").get<uint64_t>())) {
      state.assemblyHistory.undo.pop_back(); // nothing changed; drop the snapshot
      throw std::runtime_error("no such occurrence");
    }
    return json::object();
  }
  if (m == "list_occurrences") {
    if (!state.assembly)
      throw std::runtime_error("the active document is not an assembly");
    json arr = json::array();
    for (const Occurrence& o : state.assembly->GetOccurrences())
      arr.push_back(DocumentIO::OccurrenceToJson(o));
    return {{"occurrences", arr}};
  }
  // Occurrence property edits. All snapshot for undo, then bump the assembly
  // version so the renderer resyncs. `id` names the occurrence.
  if (m == "set_occurrence_transform" || m == "set_occurrence_visible" ||
      m == "set_occurrence_fixed" || m == "rename_occurrence") {
    if (!state.assembly)
      throw std::runtime_error("the active document is not an assembly");
    Occurrence* occ = state.assembly->FindById(p.at("id").get<uint64_t>());
    if (!occ)
      throw std::runtime_error("no such occurrence");
    ui::SnapshotAssembly(state);
    if (m == "set_occurrence_transform") {
      const json& t = p.at("transform");
      const json& pos = t.at("position");
      occ->position = glm::vec3(pos.at(0).get<float>(), pos.at(1).get<float>(),
                                pos.at(2).get<float>());
      const json& q = t.at("orientation"); // [x,y,z,w]
      occ->orientation = glm::quat(q.at(3).get<float>(), q.at(0).get<float>(),
                                   q.at(1).get<float>(), q.at(2).get<float>());
    } else if (m == "set_occurrence_visible") {
      occ->visible = p.at("visible").get<bool>();
    } else if (m == "set_occurrence_fixed") {
      occ->fixed = p.at("fixed").get<bool>();
    } else { // rename_occurrence
      occ->name = p.at("name").get<std::string>();
    }
    state.assembly->BumpVersion();
    // Moving/grounding an occurrence can shift what the mates solve to (a fixed
    // anchor moved, or a mated occurrence was nudged and must be re-placed).
    if (m == "set_occurrence_transform" || m == "set_occurrence_fixed")
      ui::SolveActiveAssembly(state);
    return DocumentIO::OccurrenceToJson(*occ);
  }
  // ---- Mates (A3) ---------------------------------------------------------
  // A connector spec is {occurrence, origin:true} or
  // {occurrence, face:{feature_id, element}}; translated to the on-disk anchor
  // shape and parsed by DocumentIO::MateFromJson.
  if (m == "add_mate") {
    if (!state.assembly)
      throw std::runtime_error("the active document is not an assembly");
    auto connToJson = [](const json& c) -> json {
      json anchor;
      if (c.value("origin", false))
        anchor = {{"origin", true}};
      else if (c.contains("face")) {
        anchor = {{"face", {{"feature_id", c.at("face").at("feature_id")},
                            {"element", c.at("face").at("element")}}}};
        if (c.contains("snap_edge")) // inference snap (hole center) — v26
          anchor["snap_edge"] = {{"feature_id", c.at("snap_edge").at("feature_id")},
                                 {"element", c.at("snap_edge").at("element")}};
      } else
        throw std::runtime_error(
            "connector needs 'origin:true' or 'face:{feature_id,element}'");
      return {{"occurrence", c.at("occurrence")}, {"anchor", anchor}};
    };
    json mateJson = {{"id", 0},
                     {"name", p.value("name", std::string{"Mate"})},
                     {"mate_type", p.value("type", std::string{"fastened"})},
                     {"a", connToJson(p.at("a"))},
                     {"b", connToJson(p.at("b"))},
                     {"flipped", p.value("flipped", true)},
                     {"rotation", p.value("rotation", 0.0f)},
                     {"offset", p.value("offset", 0.0f)}};
    if (p.contains("dx")) mateJson["dx"] = p.at("dx");             // planar
    if (p.contains("dy")) mateJson["dy"] = p.at("dy");            // planar
    if (p.contains("ball_rotation")) mateJson["ball_rotation"] = p.at("ball_rotation");
    Mate mate = DocumentIO::MateFromJson(mateJson);
    // Both occurrences must exist before we solve against them.
    if (!state.assembly->FindById(mate.a.occurrenceId) ||
        !state.assembly->FindById(mate.b.occurrenceId))
      throw std::runtime_error("mate references a missing occurrence");
    ui::SnapshotAssembly(state);
    const uint64_t id = state.assembly->AddMate(mate);
    const int errored = ui::SolveActiveAssembly(state);
    json out = DocumentIO::MateToJson(*state.assembly->FindMateById(id));
    out["errored_mates"] = errored;
    if (const Mate* mm = state.assembly->FindMateById(id); mm && !mm->error.empty())
      out["error"] = mm->error;
    return out;
  }
  if (m == "set_mate") {
    if (!state.assembly)
      throw std::runtime_error("the active document is not an assembly");
    Mate* mate = state.assembly->FindMateById(p.at("id").get<uint64_t>());
    if (!mate)
      throw std::runtime_error("no such mate");
    ui::SnapshotAssembly(state);
    if (p.contains("rotation")) mate->rotation = p.at("rotation").get<float>();
    if (p.contains("offset")) mate->offset = p.at("offset").get<float>();
    if (p.contains("flipped")) mate->flipped = p.at("flipped").get<bool>();
    if (p.contains("name")) mate->name = p.at("name").get<std::string>();
    if (p.contains("dx")) mate->dx = p.at("dx").get<float>();       // planar
    if (p.contains("dy")) mate->dy = p.at("dy").get<float>();       // planar
    if (p.contains("ball_rotation")) {                              // ball [x,y,z,w]
      const json& q = p.at("ball_rotation");
      mate->ballRotation = glm::quat(q.at(3).get<float>(), q.at(0).get<float>(),
                                     q.at(1).get<float>(), q.at(2).get<float>());
    }
    ui::SolveActiveAssembly(state);
    json out = DocumentIO::MateToJson(*mate);
    if (!mate->error.empty()) out["error"] = mate->error;
    return out;
  }
  if (m == "remove_mate") {
    if (!state.assembly)
      throw std::runtime_error("the active document is not an assembly");
    ui::SnapshotAssembly(state);
    if (!state.assembly->RemoveMate(p.at("id").get<uint64_t>())) {
      state.assemblyHistory.undo.pop_back(); // nothing changed; drop snapshot
      throw std::runtime_error("no such mate");
    }
    ui::SolveActiveAssembly(state); // freed occurrences keep their last pose
    return json::object();
  }
  if (m == "list_mates") {
    if (!state.assembly)
      throw std::runtime_error("the active document is not an assembly");
    json arr = json::array();
    for (const Mate& mate : state.assembly->GetMates()) {
      json mj = DocumentIO::MateToJson(mate);
      if (!mate.error.empty()) mj["error"] = mate.error;
      arr.push_back(mj);
    }
    return {{"mates", arr}};
  }
  if (m == "select_mate") {
    if (!state.assembly)
      throw std::runtime_error("the active document is not an assembly");
    const uint64_t id = p.value("id", uint64_t{0}); // 0 clears the selection
    if (id && !state.assembly->FindMateById(id))
      throw std::runtime_error("no such mate");
    state.selectedMate = id;
    return {{"selected", id}};
  }
  if (m == "select_occurrence") {
    if (!state.assembly)
      throw std::runtime_error("the active document is not an assembly");
    const uint64_t id = p.value("id", uint64_t{0}); // 0 clears the selection
    if (id && !state.assembly->FindById(id))
      throw std::runtime_error("no such occurrence");
    ui::SelectOccurrence(state, id); // highlights + shows the drag gizmo
    return {{"selected", id}};
  }
  if (m == "solve_assembly") {
    if (!state.assembly)
      throw std::runtime_error("the active document is not an assembly");
    const int errored = ui::SolveActiveAssembly(state);
    json arr = json::array();
    for (const Mate& mate : state.assembly->GetMates()) {
      json mj = {{"id", mate.id}, {"name", mate.name}};
      if (!mate.error.empty()) mj["error"] = mate.error;
      arr.push_back(mj);
    }
    return {{"errored_mates", errored}, {"mates", arr}};
  }
  if (m == "add_exploded_view") {
    // params: {name?} — appends an empty view. Returns its id.
    if (!state.assembly)
      throw std::runtime_error("the active document is not an assembly");
    ui::SnapshotAssembly(state);
    ExplodedView v;
    v.name = p.value("name", std::string{"Exploded"});
    const uint64_t id = state.assembly->AddExplodedView(std::move(v));
    return {{"id", id}};
  }
  if (m == "remove_exploded_view") {
    if (!state.assembly)
      throw std::runtime_error("the active document is not an assembly");
    const uint64_t id = p.at("id").get<uint64_t>();
    ui::SnapshotAssembly(state);
    if (!state.assembly->RemoveExplodedView(id))
      throw std::runtime_error("no such exploded view");
    if (state.activeExplodedView == id) {
      state.activeExplodedView = 0;
      state.syncedDocumentVersion = UINT64_MAX;
    }
    state.assembly->BumpVersion();
    return json::object();
  }
  if (m == "list_exploded_views") {
    if (!state.assembly)
      throw std::runtime_error("the active document is not an assembly");
    json views = json::array();
    for (const ExplodedView& v : state.assembly->GetExplodedViews())
      views.push_back(DocumentIO::ExplodedViewToJson(v));
    return {{"views", views},
            {"active", state.activeExplodedView},
            {"factor", state.explodeFactor}};
  }
  if (m == "set_explode_step") {
    // params: {view, occurrence, offset: [x,y,z]} — find-or-create the step.
    if (!state.assembly)
      throw std::runtime_error("the active document is not an assembly");
    ExplodedView* v =
        state.assembly->FindExplodedViewById(p.at("view").get<uint64_t>());
    if (!v) throw std::runtime_error("no such exploded view");
    const uint64_t occId = p.at("occurrence").get<uint64_t>();
    if (!state.assembly->FindById(occId))
      throw std::runtime_error("no such occurrence");
    const auto& o = p.at("offset");
    const glm::vec3 offset(o.at(0).get<float>(), o.at(1).get<float>(),
                           o.at(2).get<float>());
    ui::SnapshotAssembly(state);
    ExplodeStep* step = nullptr;
    for (ExplodeStep& s : v->steps)
      if (s.occurrenceId == occId) { step = &s; break; }
    if (!step) {
      v->steps.push_back({occId, offset});
    } else {
      step->offset = offset;
    }
    state.assembly->BumpVersion(); // resyncs when this view is active
    return DocumentIO::ExplodedViewToJson(*v);
  }
  if (m == "auto_explode") {
    // params: {view} — radial seed from the assembly centroid (snapshots).
    if (!state.assembly)
      throw std::runtime_error("the active document is not an assembly");
    const uint64_t id = p.at("view").get<uint64_t>();
    if (!ui::AutoExplode(state, id))
      throw std::runtime_error(
          "no such exploded view (or fewer than two placed occurrences)");
    return DocumentIO::ExplodedViewToJson(
        *state.assembly->FindExplodedViewById(id));
  }
  if (m == "set_explode") {
    // Display state (transient, never saved): which view is applied and how
    // far. params: {view?, factor?} — view 0/omitted-with-factor keeps the
    // current view; view 0 explicitly = none.
    if (!state.assembly)
      throw std::runtime_error("the active document is not an assembly");
    if (p.contains("view")) {
      const uint64_t id = p.at("view").get<uint64_t>();
      if (id && !state.assembly->FindExplodedViewById(id))
        throw std::runtime_error("no such exploded view");
      state.activeExplodedView = id;
    }
    if (p.contains("factor"))
      state.explodeFactor =
          std::clamp(p.at("factor").get<float>(), 0.0f, 1.0f);
    state.syncedDocumentVersion = UINT64_MAX; // display change: force resync
    return {{"active", state.activeExplodedView},
            {"factor", state.explodeFactor}};
  }
  if (m == "bom") {
    // Bill of materials (A4): derived, one row per referenced part.
    // Unknown masses (a body without a density) are null and taint the total.
    if (!state.assembly)
      throw std::runtime_error("the active document is not an assembly");
    const ui::BomTable bom = ui::ComputeBom(state);
    json rows = json::array();
    for (const ui::BomRow& r : bom.rows) {
      json row = {{"part_id", r.partId},
                  {"part", r.partName},
                  {"quantity", r.quantity},
                  {"material", r.material},
                  {"unit_volume", r.unitVolume}};
      row["unit_mass_g"] = r.massKnown ? json(r.unitMassG) : json(nullptr);
      row["total_mass_g"] =
          r.massKnown ? json(r.unitMassG * r.quantity) : json(nullptr);
      rows.push_back(row);
    }
    return {{"rows", rows},
            {"total",
             {{"mass_g", bom.massKnown ? json(bom.totalMassG) : json(nullptr)}}}};
  }
  if (m == "export_bom") {
    // params: {path} — writes the BOM as CSV. Same rows as `bom`.
    if (!state.assembly)
      throw std::runtime_error("the active document is not an assembly");
    const std::string path = p.at("path").get<std::string>();
    const ui::BomTable bom = ui::ComputeBom(state);
    std::ofstream out(path);
    if (!out)
      throw std::runtime_error("cannot write " + path);
    out << ui::BomToCsv(bom);
    return {{"path", path}, {"rows", bom.rows.size()}};
  }
  if (m == "set_active_part") {
    ui::SetActivePart(state, p.at("index").get<size_t>());
    return {{"active", state.activePart}};
  }
  if (m == "edit_in_context") {
    // In-context editing (A4): activates the occurrence's part tab with the
    // assembly ghosted around it. Exit with set_active_part. params:
    // {occurrence}
    if (!state.assembly)
      throw std::runtime_error("the active document is not an assembly");
    if (!ui::EnterInContextEdit(state, p.at("occurrence").get<uint64_t>()))
      throw std::runtime_error("no such occurrence (or its part is missing)");
    return {{"active", state.activePart},
            {"assembly_slot", state.inContext.assemblySlotId},
            {"occurrence", state.inContext.occurrenceId}};
  }
  if (m == "delete_part") {
    if (!ui::DeletePart(state, p.at("index").get<size_t>()))
      throw std::runtime_error("cannot delete the last (or a missing) part");
    return {{"active", state.activePart}};
  }
  if (m == "new_project") {
    ui::ReplaceDocument(state, std::make_unique<Document>());
    state.parts.clear(); // reset to a single fresh part (A1)
    ui::EnsureParts(state, "Part 1");
    ui::MarkProjectOpened(state, "");
    return json::object();
  }
  if (m == "save_project") {
    // params: {path, message?} — message becomes the git commit subject for
    // directory (.dsgn) projects (F5); ignored otherwise.
    const std::string path = p.at("path").get<std::string>();
    std::string error;
    if (!ui::SaveProject(state, path, &error)) // multi-part aware (A1)
      throw std::runtime_error(error);
    // Also clears the autosave sidecar and commits git-backed projects.
    ui::MarkProjectSaved(state, path, p.value("message", "save"));
    return json::object();
  }
  if (m == "open_project") {
    // autosave_available: a sidecar newer than this file exists (crash
    // recovery). Recovering = open_project on Autosaver::SidecarPath(path);
    // callers see the same signal the GUI recovery modal acts on.
    const std::string path = p.at("path").get<std::string>();
    const bool autosaveAvailable = Autosaver::HasNewerAutosave(path);
    std::string error;
    if (!ui::OpenProject(state, path, &error)) // multi-part aware (A1)
      throw std::runtime_error(error);
    ui::MarkProjectOpened(state, path);
    return {{"autosave_available", autosaveAvailable},
            {"autosave_path", Autosaver::SidecarPath(path)}};
  }
  if (m == "checkpoint_create") {
    // Snapshot the live document under <project>.checkpoints/<name>.json.
    // Needs a saved project (the checkpoints live beside it). params: {name}
    const std::string name = p.at("name").get<std::string>();
    std::string error;
    if (!Checkpoints::Create(*doc, state.projectPath, name, &error))
      throw std::runtime_error(error);
    // Git-backed projects (F5): a checkpoint is also a commit + tag of the
    // live state.
    if (state.projectPath.ends_with(".dsgn") && GitVault::Available()) {
      if (DocumentIO::Save(*doc, state.projectPath, &error)) {
        ui::MarkProjectSaved(state, state.projectPath, "checkpoint: " + name);
        GitVault::Tag(state.projectPath, "cp/" + name);
      }
    }
    return {{"path", Checkpoints::PathOf(state.projectPath, name)}};
  }
  if (m == "checkpoint_list") {
    json list = json::array();
    for (const auto& info : Checkpoints::List(state.projectPath))
      list.push_back(
          {{"name", info.name}, {"modified_unix", info.modifiedUnix}});
    return {{"checkpoints", list}};
  }
  if (m == "checkpoint_restore") {
    // Load the checkpoint as the current document: same project path,
    // deliberately dirty (the restored state is unsaved until saved).
    const std::string name = p.at("name").get<std::string>();
    const std::string path = Checkpoints::PathOf(state.projectPath, name);
    std::string error;
    auto loaded = path.empty() ? nullptr : DocumentIO::Load(path, &error);
    if (!loaded)
      throw std::runtime_error(error.empty() ? "no such checkpoint: " + name
                                             : error);
    ui::ReplaceDocument(state, std::move(loaded));
    ui::FitCameraToDocument(state);
    ui::MarkProjectRecovered(state, state.projectPath);
    return json::object();
  }
  if (m == "history_log") {
    // Git history of a directory project (F5). params: {limit?: 20}
    json entries = json::array();
    for (const auto& e :
         GitVault::Log(state.projectPath, p.value("limit", 20)))
      entries.push_back({{"hash", e.hash},
                         {"subject", e.subject},
                         {"unix", e.unixTime}});
    return {{"log", entries}};
  }
  if (m == "history_restore") {
    // Check out a commit's tree over the project and reload it; history
    // stays linear (the restored state commits on the next save).
    // params: {commit} (hash, tag like "cp/v1", or any git rev)
    const std::string commit = p.at("commit").get<std::string>();
    std::string error;
    if (!GitVault::RestoreTo(state.projectPath, commit, &error))
      throw std::runtime_error(error);
    auto loaded = DocumentIO::Load(state.projectPath, &error);
    if (!loaded) throw std::runtime_error(error);
    ui::ReplaceDocument(state, std::move(loaded));
    ui::FitCameraToDocument(state);
    ui::MarkProjectRecovered(state, state.projectPath);
    return json::object();
  }
  if (m == "export_step" || m == "export_stl" || m == "export_brep") {
    // Export the whole document (all bodies) or one body (by index) to
    // STEP / binary STL / native BREP. params:
    // {path, body?: index, deflection?: 0.1 (STL only)}
    const std::string path = p.at("path").get<std::string>();
    const auto& bodies = doc->GetBodies();
    if (bodies.empty()) throw std::runtime_error("no bodies to export");
    const int only = p.value("body", -1);
    if (only >= static_cast<int>(bodies.size()))
      throw std::runtime_error("body index out of range");

    auto& kernel = CADKernel::Instance();
    bool ok = false;
    if (m == "export_step") {
      std::vector<CADShape> shapes;
      for (size_t i = 0; i < bodies.size(); i++)
        if (only < 0 || i == static_cast<size_t>(only))
          shapes.push_back(bodies[i].shape);
      ok = kernel.SaveSTEP(path, shapes);
    } else {
      // STL/BREP are single-shape; a whole-document export of several bodies
      // fuses them (Onshape behavior) so one file holds the whole part.
      CADShape shape;
      if (only >= 0) {
        shape = bodies[only].shape;
      } else {
        shape = bodies[0].shape;
        for (size_t i = 1; i < bodies.size(); i++)
          shape = shape.Union(bodies[i].shape);
      }
      ok = m == "export_stl"
               ? kernel.SaveSTL(path, shape, p.value("deflection", 0.1))
               : kernel.SaveBREP(path, shape);
    }
    if (!ok) throw std::runtime_error("export failed (see log)");
    return {{"path", path},
            {"bodies", only < 0 ? bodies.size() : size_t{1}}};
  }
  if (m == "export_visual") {
    // Visual-mesh export (interchange X2): glTF (.glb/.gltf), OBJ, or PLY,
    // chosen by the path extension. Works on a part (bodies at identity) OR
    // the active assembly (occurrences at solved placements). Materials carry
    // through (glTF is PBR). params: {path, deflection?: 0.1}
    const std::string path = p.at("path").get<std::string>();
    const std::vector<MeshExport::Item> items =
        ui::BuildVisualExportItems(state);
    if (items.empty())
      throw std::runtime_error("nothing to export");
    std::string err;
    if (!MeshExport::SaveVisual(path, items, p.value("deflection", 0.1), &err))
      throw std::runtime_error(err);
    return {{"path", path}, {"items", items.size()}};
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
  if (m == "select_face") {
    if (!state.faceSelector || !state.meshRenderer)
      throw std::runtime_error(
          "face selection needs render meshes — unavailable in --headless");
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
    if (!state.faceSelector)
      throw std::runtime_error(
          "face selection needs render meshes — unavailable in --headless");
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

bool RemoteServer::IsUiAction(const std::string& method) {
  static const char* kActions[] = {"menu_click",     "item_click",
                                   "item_input",     "viewport_click",
                                   "viewport_drag",  "key_press",
                                   "scroll"};
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
        Respond(request.id, result);
        continue;
      }
      if (request.method == "screenshot") {
        if (!m_engine) {
          RespondError(request.id, "screenshot requires the GUI — start "
                                   "designer without --headless");
          continue;
        }
        m_screenshotId = request.id;
        m_screenshotPath = request.params.at("path").get<std::string>();
        m_screenshotPending = true;
        return; // main loop captures after this frame renders
      }
      if (!m_engine && IsUiAction(request.method)) {
        RespondError(request.id, "UI actions are unavailable in --headless — "
                                 "start designer without it");
        continue;
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
