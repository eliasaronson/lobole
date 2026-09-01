#include "core/cam/CamJob.h"
#include "core/cam/CamToolpath.h" // FrameIsSquare
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
  case cam::OpType::EdgeRound:
    return "edge_round";
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
  if (s == "edge_round")
    return cam::OpType::EdgeRound;
  return cam::OpType::Profile;
}

const char* ToolTypeName(cam::ToolType t) {
  switch (t) {
  case cam::ToolType::Ball:
    return "ball";
  case cam::ToolType::VBit:
    return "vbit";
  case cam::ToolType::Drill:
    return "drill";
  default:
    return "flat";
  }
}

cam::ToolType ToolTypeFromName(const std::string& s) {
  if (s == "ball")
    return cam::ToolType::Ball;
  if (s == "vbit")
    return cam::ToolType::VBit;
  if (s == "drill")
    return cam::ToolType::Drill;
  return cam::ToolType::Flat;
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

// A machining selection: the element, and which part of the job it is on
// (v73). `part` is omitted for a single-part job, where 0 is unambiguous --
// the one part -- so those files keep the shape they had.
json TargetJson(const cam::Target& t) {
  json j = {{"feature_id", t.ref.featureId}, {"element", t.ref.name}};
  if (t.jobPartId != 0)
    j["part"] = t.jobPartId;
  return j;
}

cam::Target TargetFromJson(const json& j) {
  cam::Target t;
  t.jobPartId = j.value("part", uint64_t{0});
  t.ref.featureId = j.value("feature_id", uint64_t{0});
  t.ref.name = j.value("element", std::string());
  return t;
}

} // namespace

json CamSetupToJson(const cam::Setup& setup) {
  json j;
  j["id"] = setup.id;
  j["name"] = setup.name;
  j["post"] = setup.post;
  j["safe_z"] = setup.safeZ;
  // DERIVED (v68): recomputed from the stock, the fixture and the picked face
  // on load, so it is written for a reader rather than read back. The RULE is
  // saved instead — the corner, not the coordinate — which is what stays right
  // when the part changes under it.
  if (setup.origin != glm::dvec3(0.0))
    j["origin"] = {setup.origin.x, setup.origin.y, setup.origin.z};
  if (setup.originAt != cam::OriginAt::StockMin)
    j["origin_at"] = setup.originAt == cam::OriginAt::StockCentre
                         ? "stock_centre"
                         : "model_min";
  // Which face Z zero came from. Saved because the panel says so — and said
  // "Z zero is not set yet" about a job that had one, every time it was
  // reopened, because the answer lived in the session rather than the job.
  if (setup.topFace.IsSet())
    j["top_face"] = TargetJson(setup.topFace);
  // How the part is clamped (v67). Written only when it is not "as modelled",
  // so a job on a part that already lies flat re-saves byte-identically.
  // Doubles, not the float QuatToJson the assembly schema uses: this frame
  // multiplies into every cut coordinate, and 0.7071067811865476 round-trips
  // where 0.70710678f does not.
  if (!cam::FrameIsSquare(setup))
    j["frame"] = {setup.frame.x, setup.frame.y, setup.frame.z, setup.frame.w};
  // Written only when set: every 3-axis job would otherwise carry a rotary
  // key that means nothing.
  if (setup.hasRotaryAxis)
    j["has_rotary_axis"] = true;
  if (setup.rotationA != 0.0)
    j["rotation_a"] = setup.rotationA;
  if (!setup.fitArcs)
    j["fit_arcs"] = false;
  j["stock"] = {
      {"margin",
       {setup.stock.margin.x, setup.stock.margin.y, setup.stock.margin.z}},
      {"bottom_margin", setup.stock.bottomMargin}};
  // Written only when the workpiece is not the shrink-wrap every job had
  // before v68, so those re-save byte-identically.
  if (setup.stock.mode == cam::StockMode::Fixed) {
    j["stock"]["mode"] = "fixed";
    j["stock"]["size"] = {setup.stock.size.x, setup.stock.size.y,
                          setup.stock.size.z};
    j["stock"]["place"] = {setup.stock.place.x, setup.stock.place.y,
                           setup.stock.place.z};
  }

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
    // Written only when it is not the flat mill everything assumed before
    // v66, so a job of plain end mills re-saves byte-identically.
    if (t.type != cam::ToolType::Flat) {
      tj["tool_type"] = ToolTypeName(t.type);
      tj["tip_angle"] = t.tipAngleDeg;
    }
    j["tools"].push_back(std::move(tj));
  }

  // Where each part sits in the stock (v73). Omitted when nothing has been
  // laid out, so a one-part job re-saves exactly as it did.
  if (!setup.placements.empty()) {
    j["placements"] = json::array();
    for (const cam::Placement& p : setup.placements) {
      json pj = {{"id", p.id}, {"part", p.jobPartId}};
      if (p.pos.x != 0.0 || p.pos.y != 0.0)
        pj["pos"] = {p.pos.x, p.pos.y};
      if (p.rotZdeg != 0.0)
        pj["rot_z"] = p.rotZdeg;
      if (!p.enabled)
        pj["enabled"] = false;
      j["placements"].push_back(std::move(pj));
    }
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
    // EVERY type that reads it, not just profiles. Adaptive clearing is the
    // one strategy that acts on `side`, and it was the one type that never
    // wrote it: a pocket saved and reloaded came back clearing OUTSIDE its
    // own boundary, where there is nothing, and produced no toolpath at all.
    // A job that worked before the save did not work after it
    // (docs/cam/dogfood-round.md).
    if (op.type != cam::OpType::Face && op.type != cam::OpType::Drill)
      oj["side"] = SideName(op.side);
    if (!op.climb)
      oj["climb"] = false;
    // Written only when set, so an operation that goes around its islands —
    // every one before this — re-saves byte-identically.
    if (op.useOutline)
      oj["use_outline"] = true;
    if (op.tabCount != 0) {
      oj["tabs"] = {{"count", op.tabCount},
                    {"width", op.tabWidth},
                    {"height", op.tabHeight}};
    }
    if (op.type == cam::OpType::Pocket || op.type == cam::OpType::Face ||
        op.type == cam::OpType::EdgeRound)
      oj["stepover"] = op.stepover;
    if (op.type == cam::OpType::EdgeRound)
      oj["edge_size"] = op.edgeSize;
    if (!op.enabled)
      oj["enabled"] = false;
    if (!op.targets.empty()) {
      oj["targets"] = json::array();
      for (const cam::Target& t : op.targets)
        oj["targets"].push_back(TargetJson(t));
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
  if (j.contains("top_face"))
    setup.topFace = TargetFromJson(j["top_face"]);
  if (j.contains("origin") && j["origin"].is_array() && j["origin"].size() == 3)
    setup.origin = {j["origin"][0].get<double>(), j["origin"][1].get<double>(),
                    j["origin"][2].get<double>()};
  // An unknown corner is REFUSED, not read as the default: every coordinate
  // in the posted file is measured from it, so a silent substitution moves the
  // whole job.
  if (j.contains("origin_at")) {
    const std::string at = j["origin_at"].get<std::string>();
    if (at == "stock_min")
      setup.originAt = cam::OriginAt::StockMin;
    else if (at == "stock_centre")
      setup.originAt = cam::OriginAt::StockCentre;
    else if (at == "model_min")
      setup.originAt = cam::OriginAt::ModelMin;
    else
      throw std::runtime_error(
          "unknown cam origin_at '" + at +
          "'; this build accepts stock_min, stock_centre, model_min");
  }
  if (j.contains("frame") && j["frame"].is_array() && j["frame"].size() == 4)
    setup.frame =
        glm::dquat(j["frame"][3].get<double>(), j["frame"][0].get<double>(),
                   j["frame"][1].get<double>(), j["frame"][2].get<double>());
  setup.rotationA = j.value("rotation_a", 0.0);
  // Before v67 there was no such flag and `rotation_a` was the only way to
  // turn a part, so a file that carries one was written by a machine that
  // has the axis. Defaulting it to false instead would silently drop the A
  // word from a rotary job that used to post one.
  setup.hasRotaryAxis = j.value("has_rotary_axis", setup.rotationA != 0.0);
  setup.fitArcs = j.value("fit_arcs", true);
  if (j.contains("stock")) {
    const json& sj = j["stock"];
    if (sj.contains("margin") && sj["margin"].is_array() &&
        sj["margin"].size() == 3)
      setup.stock.margin = {sj["margin"][0].get<double>(),
                            sj["margin"][1].get<double>(),
                            sj["margin"][2].get<double>()};
    setup.stock.bottomMargin = sj.value("bottom_margin", 1.0);
    if (sj.contains("mode")) {
      const std::string mode = sj["mode"].get<std::string>();
      if (mode == "fixed")
        setup.stock.mode = cam::StockMode::Fixed;
      else if (mode == "relative")
        setup.stock.mode = cam::StockMode::Relative;
      else
        throw std::runtime_error("unknown cam stock mode '" + mode +
                                 "'; this build accepts relative, fixed");
    }
    if (sj.contains("size") && sj["size"].is_array() && sj["size"].size() == 3)
      setup.stock.size = {sj["size"][0].get<double>(),
                          sj["size"][1].get<double>(),
                          sj["size"][2].get<double>()};
    if (sj.contains("place") && sj["place"].is_array() &&
        sj["place"].size() == 3)
      setup.stock.place = {sj["place"][0].get<double>(),
                           sj["place"][1].get<double>(),
                           sj["place"][2].get<double>()};
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
    t.type = ToolTypeFromName(tj.value("tool_type", std::string("flat")));
    t.tipAngleDeg = tj.value("tip_angle", 90.0);
    setup.tools.push_back(std::move(t));
  }
  for (const json& pj : j.value("placements", json::array())) {
    cam::Placement p;
    p.id = pj.value("id", uint64_t{0});
    p.jobPartId = pj.value("part", uint64_t{0});
    if (pj.contains("pos") && pj["pos"].is_array() && pj["pos"].size() == 2)
      p.pos = {pj["pos"][0].get<double>(), pj["pos"][1].get<double>()};
    p.rotZdeg = pj.value("rot_z", 0.0);
    p.enabled = pj.value("enabled", true);
    setup.placements.push_back(p);
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
    // A clearing operation clears INSIDE its boundary; "outside" is a
    // profile's notion. So the default when the key is missing depends on the
    // type — which is what repairs the documents written before this was
    // saved at all, without asking anyone to re-make their job.
    const bool clears =
        op.type == cam::OpType::Pocket || op.type == cam::OpType::Adaptive;
    op.side = SideFromName(
        oj.value("side", std::string(clears ? "inside" : "outside")));
    op.climb = oj.value("climb", true);
    op.useOutline = oj.value("use_outline", false);
    if (oj.contains("tabs")) {
      op.tabCount = oj["tabs"].value("count", 0);
      op.tabWidth = oj["tabs"].value("width", 6.0);
      op.tabHeight = oj["tabs"].value("height", 1.5);
    }
    op.stepover = oj.value("stepover", 0.45);
    op.edgeSize = oj.value("edge_size", 0.0);
    op.enabled = oj.value("enabled", true);
    for (const json& rj : oj.value("targets", json::array()))
      op.targets.push_back(TargetFromJson(rj));
    setup.operations.push_back(std::move(op));
  }
  return setup;
}

} // namespace DocumentIO
