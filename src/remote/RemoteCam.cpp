#include "remote/RemoteCam.h"

#ifdef HAVE_CAM

#include "core/Document.h"
#include "core/cam/CamPost.h"
#include "core/cam/CamToolpath.h"
#include "core/io/Serialization.h"
#include "ui/AppState.h"

#include <fstream>

namespace remote {

using nlohmann::json;

namespace {

constexpr MethodDoc kCamMethods[] = {
    {"add_cam_setup",
     "Add a milling setup {name?, post?: mach3|linuxcnc, safe_z?}; returns "
     "its id. A setup arrives with one 6 mm tool, because a setup with no "
     "tools can produce nothing."},
    {"list_cam_setups",
     "Every setup: id, name, post, safe_z, tools, operations, and the stock "
     "box derived from the model."},
    {"set_cam_setup",
     "Change a setup {id, name?, post?, safe_z?, rotation_a?} or one of its "
     "tools {id, tool: {number, name?, diameter?, feed_cut?, feed_plunge?, "
     "spindle_rpm?, flutes?}} — the tool is added when the number is new."},
    {"remove_cam_setup", "Delete a setup {id} and its operations."},
    {"add_cam_op",
     "Add an operation {setup, type: profile|pocket|face|drill, tool?, "
     "top_z, bottom_z, stepdown?, side?: outside|inside|on, climb?, "
     "stepover?, stock_to_leave?, tabs?: {count, width, height}, targets?: "
     "[{feature_id, element}]}; returns its id."},
    {"set_cam_op", "Change an operation {id, …the add_cam_op fields}."},
    {"remove_cam_op", "Delete an operation {id}."},
    {"cam_toolpath",
     "Generate toolpaths for a setup {setup} without writing a file: per "
     "operation, the move count, cut length (mm), lowest Z, and the error "
     "when it produced nothing. The cheap way to check a job."},
    {"cam_post",
     "Write the G-code for a setup {setup, path, post?}. Reports the line "
     "count, the ops it skipped and why. Writes a file; it does not send it "
     "to a machine."},
};

cam::Side SideFrom(const std::string& s) {
  if (s == "inside")
    return cam::Side::Inside;
  if (s == "on")
    return cam::Side::On;
  return cam::Side::Outside;
}

const char* SideName(cam::Side s) {
  return s == cam::Side::Inside ? "inside"
         : s == cam::Side::On   ? "on"
                                : "outside";
}

const char* TypeName(cam::OpType t) {
  switch (t) {
  case cam::OpType::Pocket:
    return "pocket";
  case cam::OpType::Face:
    return "face";
  case cam::OpType::Drill:
    return "drill";
  default:
    return "profile";
  }
}

cam::OpType TypeFrom(const std::string& s) {
  if (s == "pocket")
    return cam::OpType::Pocket;
  if (s == "face")
    return cam::OpType::Face;
  if (s == "drill")
    return cam::OpType::Drill;
  if (s == "profile")
    return cam::OpType::Profile;
  throw std::runtime_error("unknown operation type '" + s +
                           "'; use profile, pocket, face or drill");
}

cam::Setup& RequireSetup(Document& doc, uint64_t id) {
  cam::Setup* setup = doc.FindCamSetup(id);
  if (setup == nullptr)
    throw std::runtime_error("no CAM setup with id " + std::to_string(id) +
                             "; list_cam_setups shows the ones that exist");
  return *setup;
}

// The setup that owns an operation, plus the operation itself.
std::pair<cam::Setup*, cam::Operation*> FindOp(Document& doc, uint64_t opId) {
  for (cam::Setup& setup : doc.GetCamSetups())
    for (cam::Operation& op : setup.operations)
      if (op.id == opId)
        return {&setup, &op};
  return {nullptr, nullptr};
}

void ApplyOpFields(Document& doc, cam::Operation& op, const RpcParams& p) {
  if (p.contains("type"))
    op.type = TypeFrom(p.at("type").get<std::string>());
  if (p.contains("name"))
    op.name = p.at("name").get<std::string>();
  if (p.contains("tool"))
    op.toolNumber = p.at("tool").get<int>();
  if (p.contains("top_z"))
    op.topZ = p.at("top_z").get<double>();
  if (p.contains("bottom_z"))
    op.bottomZ = p.at("bottom_z").get<double>();
  if (p.contains("stepdown"))
    op.stepdown = p.at("stepdown").get<double>();
  if (p.contains("stock_to_leave"))
    op.stockToLeave = p.at("stock_to_leave").get<double>();
  if (p.contains("side"))
    op.side = SideFrom(p.at("side").get<std::string>());
  if (p.contains("climb"))
    op.climb = p.at("climb").get<bool>();
  if (p.contains("stepover"))
    op.stepover = p.at("stepover").get<double>();
  if (p.contains("enabled"))
    op.enabled = p.at("enabled").get<bool>();
  if (p.contains("tabs")) {
    const json& t = p.at("tabs");
    op.tabCount = t.value("count", 0);
    op.tabWidth = t.value("width", 6.0);
    op.tabHeight = t.value("height", 1.5);
  }
  if (p.contains("targets")) {
    op.targets.clear();
    for (const json& tj : p.at("targets")) {
      ElementRef ref;
      ref.featureId = tj.value("feature_id", uint64_t{0});
      ref.name = tj.value("element", std::string());
      // A target that cannot resolve today would produce an empty toolpath
      // later, with the failure surfacing at the machine. Refuse now.
      if (ref.featureId == 0 || ref.name.empty())
        throw std::runtime_error(
            "each target needs {feature_id, element}; list_elements shows "
            "the stable names");
      op.targets.push_back(std::move(ref));
    }
  }
  (void)doc;
}

json ToolJson(const cam::Tool& t) {
  return json{{"number", t.number},         {"name", t.name},
              {"diameter", t.diameter},     {"flutes", t.flutes},
              {"feed_cut", t.feedCut},      {"feed_plunge", t.feedPlunge},
              {"spindle_rpm", t.spindleRpm}};
}

json OpJson(const cam::Operation& op) {
  json j = {{"id", op.id},
            {"name", op.name},
            {"type", TypeName(op.type)},
            {"tool", op.toolNumber},
            {"top_z", op.topZ},
            {"bottom_z", op.bottomZ},
            {"stepdown", op.stepdown},
            {"enabled", op.enabled}};
  if (op.type == cam::OpType::Profile) {
    j["side"] = SideName(op.side);
    if (op.tabCount > 0)
      j["tabs"] = {{"count", op.tabCount},
                   {"width", op.tabWidth},
                   {"height", op.tabHeight}};
  }
  if (!op.targets.empty()) {
    j["targets"] = json::array();
    for (const ElementRef& r : op.targets)
      j["targets"].push_back(
          json{{"feature_id", r.featureId}, {"element", r.name}});
  }
  return j;
}

} // namespace

std::span<const MethodDoc> CamMethodDocs() { return kCamMethods; }

std::optional<json> HandleCamRpc(ui::AppState& state, Document* doc,
                                 const std::string& m, const RpcParams& p) {
  bool ours = false;
  for (const MethodDoc& d : kCamMethods)
    if (m == d.name)
      ours = true;
  if (!ours)
    return std::nullopt;
  if (doc == nullptr)
    throw std::runtime_error(
        "CAM works on a part; switch to a part tab (a setup cuts one model)");
  (void)state;

  if (m == "add_cam_setup") {
    cam::Setup& setup = doc->AddCamSetup(p.value("name", std::string()));
    if (p.contains("post")) {
      const std::string post = p.at("post").get<std::string>();
      if (!cam::PostExists(post))
        throw std::runtime_error("unknown post '" + post + "'");
      setup.post = post;
    }
    if (p.contains("safe_z"))
      setup.safeZ = p.at("safe_z").get<double>();
    return json{{"id", setup.id}, {"name", setup.name}, {"post", setup.post}};
  }

  if (m == "list_cam_setups") {
    json out = json::array();
    for (const cam::Setup& setup : doc->GetCamSetups()) {
      json sj = {{"id", setup.id},
                 {"name", setup.name},
                 {"post", setup.post},
                 {"safe_z", setup.safeZ},
                 {"rotation_a", setup.rotationA}};
      sj["tools"] = json::array();
      for (const cam::Tool& t : setup.tools)
        sj["tools"].push_back(ToolJson(t));
      sj["operations"] = json::array();
      for (const cam::Operation& op : setup.operations)
        sj["operations"].push_back(OpJson(op));
      glm::dvec3 lo(0.0), hi(0.0);
      if (cam::StockBounds(setup, doc->GetBodies(), lo, hi))
        sj["stock"] = {{"min", {lo.x, lo.y, lo.z}},
                       {"max", {hi.x, hi.y, hi.z}}};
      out.push_back(std::move(sj));
    }
    return json{{"setups", out}};
  }

  if (m == "set_cam_setup") {
    cam::Setup& setup = RequireSetup(*doc, p.at("id").get<uint64_t>());
    if (p.contains("name"))
      setup.name = p.at("name").get<std::string>();
    if (p.contains("post")) {
      const std::string post = p.at("post").get<std::string>();
      if (!cam::PostExists(post))
        throw std::runtime_error("unknown post '" + post + "'");
      setup.post = post;
    }
    if (p.contains("safe_z"))
      setup.safeZ = p.at("safe_z").get<double>();
    if (p.contains("rotation_a"))
      setup.rotationA = p.at("rotation_a").get<double>();
    if (p.contains("tool")) {
      const json& tj = p.at("tool");
      const int number = tj.value("number", 1);
      cam::Tool* tool = nullptr;
      for (cam::Tool& t : setup.tools)
        if (t.number == number)
          tool = &t;
      if (tool == nullptr) {
        cam::Tool fresh;
        fresh.number = number;
        setup.tools.push_back(fresh);
        tool = &setup.tools.back();
      }
      tool->name = tj.value("name", tool->name);
      tool->diameter = tj.value("diameter", tool->diameter);
      tool->flutes = tj.value("flutes", tool->flutes);
      tool->feedCut = tj.value("feed_cut", tool->feedCut);
      tool->feedPlunge = tj.value("feed_plunge", tool->feedPlunge);
      tool->spindleRpm = tj.value("spindle_rpm", tool->spindleRpm);
      if (tool->diameter <= 0.0)
        throw std::runtime_error("tool diameter must be positive");
    }
    return json{{"id", setup.id}};
  }

  if (m == "remove_cam_setup") {
    const uint64_t id = p.at("id").get<uint64_t>();
    if (!doc->RemoveCamSetup(id))
      throw std::runtime_error("no CAM setup with id " + std::to_string(id));
    return json::object();
  }

  if (m == "add_cam_op") {
    cam::Setup& setup = RequireSetup(*doc, p.at("setup").get<uint64_t>());
    cam::Operation op;
    op.id = doc->AllocateId();
    op.toolNumber = setup.tools.empty() ? 1 : setup.tools.front().number;
    ApplyOpFields(*doc, op, p);
    if (op.name.empty())
      op.name = std::string(TypeName(op.type)) + " " +
                std::to_string(setup.operations.size() + 1);
    setup.operations.push_back(op);
    return json{{"id", op.id}, {"name", op.name}};
  }

  if (m == "set_cam_op") {
    auto [setup, op] = FindOp(*doc, p.at("id").get<uint64_t>());
    if (op == nullptr)
      throw std::runtime_error("no CAM operation with that id");
    ApplyOpFields(*doc, *op, p);
    return json{{"id", op->id}};
  }

  if (m == "remove_cam_op") {
    const uint64_t id = p.at("id").get<uint64_t>();
    for (cam::Setup& setup : doc->GetCamSetups()) {
      auto& ops = setup.operations;
      for (auto it = ops.begin(); it != ops.end(); ++it) {
        if (it->id == id) {
          ops.erase(it);
          return json::object();
        }
      }
    }
    throw std::runtime_error("no CAM operation with that id");
  }

  if (m == "cam_toolpath" || m == "cam_post") {
    const cam::Setup& setup = RequireSetup(*doc, p.at("setup").get<uint64_t>());
    const std::vector<cam::Toolpath> paths =
        cam::GenerateAll(setup, doc->GetBodies());

    json ops = json::array();
    double totalCut = 0.0;
    double lowest = 0.0;
    int failed = 0;
    for (const cam::Toolpath& tp : paths) {
      json oj = {{"operation", tp.operationId},
                 {"tool", tp.toolNumber},
                 {"moves", tp.moves.size()},
                 {"cut_length_mm", tp.CutLength()},
                 {"min_z", tp.MinZ()}};
      if (!tp.error.empty()) {
        oj["error"] = tp.error;
        failed++;
      }
      totalCut += tp.CutLength();
      lowest = std::min(lowest, tp.MinZ());
      ops.push_back(std::move(oj));
    }

    if (m == "cam_toolpath")
      return json{{"operations", ops},
                  {"cut_length_mm", totalCut},
                  {"min_z", lowest},
                  {"failed", failed}};

    const std::string post = p.value("post", setup.post);
    std::string gcode, error;
    if (!cam::PostProcess(post, setup, paths, {}, gcode, &error))
      throw std::runtime_error(error);
    const std::string path = p.at("path").get<std::string>();
    if (!DocumentIO::WriteTextAtomic(path, gcode, &error))
      throw std::runtime_error("could not write " + path + ": " + error);
    size_t lines = 0;
    for (const char c : gcode)
      if (c == '\n')
        lines++;
    return json{{"path", path},
                {"post", post},
                {"lines", lines},
                {"operations", ops},
                {"cut_length_mm", totalCut},
                {"min_z", lowest},
                // Surfaced, not buried: a job posted with a failed op is a
                // part that comes off the machine unfinished.
                {"failed", failed}};
  }

  return std::nullopt;
}

} // namespace remote

#else // !HAVE_CAM

// Built without CAM (-DENABLE_CAM=OFF): the methods simply do not exist, and
// `describe` does not list them. The dispatcher needs no #ifdef of its own.
namespace remote {
std::optional<nlohmann::json>
HandleCamRpc(ui::AppState&, Document*, const std::string&, const RpcParams&) {
  return std::nullopt;
}
std::span<const MethodDoc> CamMethodDocs() { return {}; }
} // namespace remote

#endif // HAVE_CAM
