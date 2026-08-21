#include "core/cam/CamJob.h"
#include "core/io/SerializationInternal.h"

// CAM setups (v58, docs/cam/plan.md). Small enough to live inline in the
// document rather than file-per-setup as studies do — a setup is a few
// hundred bytes of parameters, and studies earned their own files by
// carrying meshes.
//
// Every key is omitted at its default so a document without CAM re-saves
// byte-identically (the exploded-views v27 discipline).
namespace DocumentIO {

using nlohmann::json;

namespace {

const char* OpTypeName(cam::OpType t) {
  switch (t) {
  case cam::OpType::Pocket:
    return "pocket";
  case cam::OpType::Face:
    return "face";
  case cam::OpType::Drill:
    return "drill";
  case cam::OpType::Adaptive:
    return "adaptive";
  default:
    return "profile";
  }
}

cam::OpType OpTypeFromName(const std::string& s) {
  if (s == "pocket")
    return cam::OpType::Pocket;
  if (s == "face")
    return cam::OpType::Face;
  if (s == "drill")
    return cam::OpType::Drill;
  if (s == "adaptive")
    return cam::OpType::Adaptive;
  return cam::OpType::Profile;
}

const char* SideName(cam::Side s) {
  switch (s) {
  case cam::Side::Inside:
    return "inside";
  case cam::Side::On:
    return "on";
  default:
    return "outside";
  }
}

cam::Side SideFromName(const std::string& s) {
  if (s == "inside")
    return cam::Side::Inside;
  if (s == "on")
    return cam::Side::On;
  return cam::Side::Outside;
}

} // namespace

json CamSetupToJson(const cam::Setup& setup) {
  json j;
  j["id"] = setup.id;
  j["name"] = setup.name;
  j["post"] = setup.post;
  j["safe_z"] = setup.safeZ;
  if (setup.origin != glm::dvec3(0.0))
    j["origin"] = {setup.origin.x, setup.origin.y, setup.origin.z};
  // Written only when set: every 3-axis job would otherwise carry a rotary
  // key that means nothing (and the 4th axis is deferred — plan.md).
  if (setup.rotationA != 0.0)
    j["rotation_a"] = setup.rotationA;
  if (!setup.fitArcs)
    j["fit_arcs"] = false;
  j["stock"] = {
      {"margin",
       {setup.stock.margin.x, setup.stock.margin.y, setup.stock.margin.z}},
      {"bottom_margin", setup.stock.bottomMargin}};

  j["tools"] = json::array();
  for (const cam::Tool& t : setup.tools) {
    json tj = {{"number", t.number},
               {"name", t.name},
               {"diameter", t.diameter},
               {"flutes", t.flutes},
               {"flute_length", t.fluteLength},
               {"feed_cut", t.feedCut},
               {"feed_plunge", t.feedPlunge},
               {"spindle_rpm", t.spindleRpm}};
    j["tools"].push_back(std::move(tj));
  }

  j["operations"] = json::array();
  for (const cam::Operation& op : setup.operations) {
    json oj = {{"id", op.id},
               {"name", op.name},
               {"type", OpTypeName(op.type)},
               {"tool", op.toolNumber},
               {"top_z", op.topZ},
               {"bottom_z", op.bottomZ}};
    if (op.stepdown != 0.0)
      oj["stepdown"] = op.stepdown;
    if (op.stockToLeave != 0.0)
      oj["stock_to_leave"] = op.stockToLeave;
    if (op.type == cam::OpType::Profile)
      oj["side"] = SideName(op.side);
    if (!op.climb)
      oj["climb"] = false;
    if (op.tabCount != 0) {
      oj["tabs"] = {{"count", op.tabCount},
                    {"width", op.tabWidth},
                    {"height", op.tabHeight}};
    }
    if (op.type == cam::OpType::Pocket || op.type == cam::OpType::Face)
      oj["stepover"] = op.stepover;
    if (!op.enabled)
      oj["enabled"] = false;
    if (!op.targets.empty()) {
      oj["targets"] = json::array();
      for (const ElementRef& ref : op.targets)
        oj["targets"].push_back(
            json{{"feature_id", ref.featureId}, {"element", ref.name}});
    }
    j["operations"].push_back(std::move(oj));
  }
  return j;
}

cam::Setup CamSetupFromJson(const json& j) {
  cam::Setup setup;
  setup.id = j.value("id", uint64_t{0});
  setup.name = j.value("name", std::string("Setup"));
  setup.post = j.value("post", std::string("mach3"));
  setup.safeZ = j.value("safe_z", 5.0);
  if (j.contains("origin") && j["origin"].is_array() && j["origin"].size() == 3)
    setup.origin = {j["origin"][0].get<double>(), j["origin"][1].get<double>(),
                    j["origin"][2].get<double>()};
  setup.rotationA = j.value("rotation_a", 0.0);
  setup.fitArcs = j.value("fit_arcs", true);
  if (j.contains("stock")) {
    const json& sj = j["stock"];
    if (sj.contains("margin") && sj["margin"].is_array() &&
        sj["margin"].size() == 3)
      setup.stock.margin = {sj["margin"][0].get<double>(),
                            sj["margin"][1].get<double>(),
                            sj["margin"][2].get<double>()};
    setup.stock.bottomMargin = sj.value("bottom_margin", 1.0);
  }
  for (const json& tj : j.value("tools", json::array())) {
    cam::Tool t;
    t.number = tj.value("number", 1);
    t.name = tj.value("name", std::string());
    t.diameter = tj.value("diameter", 6.0);
    t.flutes = tj.value("flutes", 2);
    t.fluteLength = tj.value("flute_length", 20.0);
    t.feedCut = tj.value("feed_cut", 300.0);
    t.feedPlunge = tj.value("feed_plunge", 100.0);
    t.spindleRpm = tj.value("spindle_rpm", 10000.0);
    setup.tools.push_back(std::move(t));
  }
  for (const json& oj : j.value("operations", json::array())) {
    cam::Operation op;
    op.id = oj.value("id", uint64_t{0});
    op.name = oj.value("name", std::string());
    op.type = OpTypeFromName(oj.value("type", std::string("profile")));
    op.toolNumber = oj.value("tool", 1);
    op.topZ = oj.value("top_z", 0.0);
    op.bottomZ = oj.value("bottom_z", -1.0);
    op.stepdown = oj.value("stepdown", 0.0);
    op.stockToLeave = oj.value("stock_to_leave", 0.0);
    op.side = SideFromName(oj.value("side", std::string("outside")));
    op.climb = oj.value("climb", true);
    if (oj.contains("tabs")) {
      op.tabCount = oj["tabs"].value("count", 0);
      op.tabWidth = oj["tabs"].value("width", 6.0);
      op.tabHeight = oj["tabs"].value("height", 1.5);
    }
    op.stepover = oj.value("stepover", 0.45);
    op.enabled = oj.value("enabled", true);
    for (const json& rj : oj.value("targets", json::array())) {
      ElementRef ref;
      ref.featureId = rj.value("feature_id", uint64_t{0});
      ref.name = rj.value("element", std::string());
      op.targets.push_back(std::move(ref));
    }
    setup.operations.push_back(std::move(op));
  }
  return setup;
}

} // namespace DocumentIO
