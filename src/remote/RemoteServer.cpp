#include "remote/RemoteServer.h"

#include "CADMaterials.h"
#include "Camera.h"
#include "FaceSelector.h"
#include "Logger.h"
#include "RayTracingRenderer.h"
#include "MeshRenderer.h"
#include "core/Autosaver.h"
#include "core/Expression.h"
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
    json out = json::array();
    double totalVolume = 0.0, totalMass = 0.0;
    glm::dvec3 weightedCom(0.0);
    bool massKnown = true;
    for (size_t i = 0; i < bodies.size(); i++) {
      if (only >= 0 && i != static_cast<size_t>(only)) continue;
      CADShape::MassProps mp;
      if (!bodies[i].shape.ComputeMassProps(mp))
        throw std::runtime_error("mass properties failed for body " +
                                 bodies[i].name);
      json inertia = json::array();
      for (int r = 0; r < 3; r++)
        inertia.push_back({mp.inertia[r][0], mp.inertia[r][1],
                           mp.inertia[r][2]});
      json entry = {{"body", i},
                    {"name", bodies[i].name},
                    {"volume", mp.volume},
                    {"surface_area", mp.surfaceArea},
                    {"center_of_mass", Vec3ToJson(mp.centerOfMass)},
                    {"inertia", inertia},
                    {"material", bodies[i].material}};
      const float density = CADMaterials::MaterialLibrary::GetMaterial(
                                CADMaterials::MaterialLibrary::FromSlug(
                                    bodies[i].material))
                                .density;
      if (!bodies[i].material.empty() && density > 0.0f) {
        entry["density"] = density;
        entry["mass_g"] = density * mp.volume * 1e-6;
        totalMass += density * mp.volume * 1e-6;
      } else {
        entry["mass_g"] = nullptr;
        massKnown = false;
      }
      totalVolume += mp.volume;
      weightedCom += glm::dvec3(mp.centerOfMass) * mp.volume;
      out.push_back(entry);
    }
    if (out.empty()) throw std::runtime_error("no bodies");
    const glm::dvec3 com =
        totalVolume > 0.0 ? weightedCom / totalVolume : glm::dvec3(0.0);
    return {{"bodies", out},
            {"total",
             {{"volume", totalVolume},
              {"center_of_mass", {com.x, com.y, com.z}},
              {"mass_g", massKnown ? json(totalMass) : json(nullptr)}}}};
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
    state.commands->Undo(*doc);
    return {{"can_undo", state.commands->CanUndo()}};
  }
  if (m == "redo") {
    state.commands->Redo(*doc);
    return {{"can_redo", state.commands->CanRedo()}};
  }
  if (m == "new_project") {
    ui::ReplaceDocument(state, std::make_unique<Document>());
    ui::MarkProjectOpened(state, "");
    return json::object();
  }
  if (m == "save_project") {
    const std::string path = p.at("path").get<std::string>();
    std::string error;
    if (!DocumentIO::Save(*doc, path, &error))
      throw std::runtime_error(error);
    ui::MarkProjectSaved(state, path); // also clears the autosave sidecar
    return json::object();
  }
  if (m == "open_project") {
    // autosave_available: a sidecar newer than this file exists (crash
    // recovery). Recovering = open_project on Autosaver::SidecarPath(path);
    // callers see the same signal the GUI recovery modal acts on.
    const std::string path = p.at("path").get<std::string>();
    const bool autosaveAvailable = Autosaver::HasNewerAutosave(path);
    std::string error;
    auto loaded = DocumentIO::Load(path, &error);
    if (!loaded) throw std::runtime_error(error);
    ui::ReplaceDocument(state, std::move(loaded));
    ui::FitCameraToDocument(state);
    ui::MarkProjectOpened(state, path);
    return {{"autosave_available", autosaveAvailable},
            {"autosave_path", Autosaver::SidecarPath(path)}};
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
