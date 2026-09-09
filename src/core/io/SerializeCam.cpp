#include "core/cam/CamJob.h"
#include "core/cam/CamMachine.h" // MachineFileName
#include "core/cam/CamNames.h"
#include "core/cam/CamPost.h"     // PostExists, PostIds
#include "core/cam/CamToolpath.h" // FrameIsSquare
#include "core/io/Serialization.h" // WriteTextAtomic
#include "core/io/SerializationInternal.h"

#include <fstream>

#include <filesystem>

#include <algorithm>

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

// The type for a saved name. This used to `return cam::OpType::Profile` for
// anything it did not recognise, so a job file with a typo in it -- or a
// merged one, or a hand-edited one, which the directory format invites --
// loaded that operation as a PROFILE: a different strategy against a
// different region, posted and cut without a word. Same rule as the rest of
// the loader now (docs/architecture-review-2026-08-24.md, follow-up E):
// refuse, naming what IS accepted.
cam::OpType OpTypeFromJson(const std::string& s) {
  cam::OpType t{};
  if (cam::TryOpTypeFromName(s, t))
    return t;
  throw std::runtime_error("unknown CAM operation type '" + s +
                           "' (expected one of: " + cam::OpTypeNameList() +
                           ")");
}

cam::ToolType ToolTypeFromName(const std::string& s) {
  cam::ToolType t{};
  if (cam::TryToolTypeFromName(s, t))
    return t;
  // Used to return ToolType::Flat. A ball nose steps over by the cusp it
  // leaves and a V-bit chamfers by its angle; read either as a flat mill and
  // the toolpath is a square-shouldered cut that the verifier -- which models
  // the same wrong tool -- calls clean.
  throw std::runtime_error("unknown CAM tool type '" + s +
                           "' (expected one of: " + cam::ToolTypeNameList() +
                           ")");
}

cam::Side SideFromName(const std::string& s) {
  cam::Side side{};
  if (cam::TrySideFromName(s, side))
    return side;
  // Used to return Side::Outside. This field has already cost one bug in
  // exactly this shape: when `side` was not serialized at all (before v62) a
  // pocket reloaded clearing OUTSIDE its own boundary, where there is
  // nothing, and produced no toolpath. A misspelt `side` did the same thing,
  // and said as little.
  throw std::runtime_error("unknown CAM side '" + s +
                           "' (expected one of: " + cam::SideNameList() + ")");
}

// In what order the operations cut (v75). An unknown string is REFUSED
// naming the three: a job that silently posts in a different order than it
// says is a broken cutter, not a wrong-looking drawing.
const char* OrderName(cam::OrderOutputBy o) {
  switch (o) {
  case cam::OrderOutputBy::Tool:
    return "tool";
  case cam::OrderOutputBy::Part:
    return "part";
  default:
    return "authored";
  }
}

cam::OrderOutputBy OrderFromName(const std::string& s) {
  if (s == "authored")
    return cam::OrderOutputBy::Authored;
  if (s == "tool")
    return cam::OrderOutputBy::Tool;
  if (s == "part")
    return cam::OrderOutputBy::Part;
  throw std::runtime_error("unknown cutting order '" + s +
                           "'; use authored, tool or part");
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

namespace {

std::string PostIdList() {
  std::string out;
  for (const std::string& id : cam::PostIds())
    out += (out.empty() ? "" : ", ") + id;
  return out;
}

json ToolToJson(const cam::Tool& t) {
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
    tj["tool_type"] = cam::ToolTypeName(t.type);
    tj["tip_angle"] = t.tipAngleDeg;
  }
  // Only when somebody stated them, so a job of tools nobody has charted
  // re-saves byte-identically (v92).
  if (t.chiploadMin > 0.0 || t.chiploadMax > 0.0) {
    tj["chipload_min"] = t.chiploadMin;
    tj["chipload_max"] = t.chiploadMax;
  }
  return tj;
}

cam::Tool ToolFromJson(const json& tj) {
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
  t.chiploadMin = tj.value("chipload_min", 0.0);
  t.chiploadMax = tj.value("chipload_max", 0.0);
  return t;
}

} // namespace

json MachineToJson(const cam::Machine& m) {
  json j;
  j["name"] = m.name;
  j["post"] = m.post;
  // Written only when stated / not the default, so a job on the job's own
  // default machine carries a small block.
  // Written whenever ANY axis is stated, not only once all three are: the
  // Preferences editor writes the file after every edit, so gating on a
  // complete work area threw away the X a user had just typed while Y and Z
  // were still zero -- the field snapped back to 0.000 on Enter.
  if (m.travelMax != m.travelMin)
    j["travel"] = {{"min", {m.travelMin.x, m.travelMin.y, m.travelMin.z}},
                   {"max", {m.travelMax.x, m.travelMax.y, m.travelMax.z}}};
  if (!m.fitArcs)
    j["fit_arcs"] = false;
  if (m.hasRotaryAxis)
    j["has_rotary_axis"] = true;
  if (m.rapidFeed != 0.0)
    j["rapid_feed"] = m.rapidFeed;
  if (!m.library.empty()) {
    j["tools"] = json::array();
    for (const cam::Tool& t : m.library)
      j["tools"].push_back(ToolToJson(t));
  }
  return j;
}

cam::Machine MachineFromJson(const json& j) {
  cam::Machine m;
  m.name = j.value("name", std::string());
  m.post = j.value("post", std::string("mach3"));
  // An unknown post is REFUSED, naming what is accepted: every line of the
  // file is written in its dialect, and a silent mach3 posts for the wrong
  // controller with nothing to say so.
  if (!cam::PostExists(m.post))
    throw std::runtime_error("unknown cam post '" + m.post +
                             "'; this build accepts " + PostIdList());
  if (j.contains("travel")) {
    const json& t = j.at("travel");
    const auto vec = [&](const char* key) {
      const json& a = t.at(key);
      if (!a.is_array() || a.size() != 3)
        throw std::runtime_error(std::string("machine travel.") + key +
                                 " must be [x, y, z]");
      return glm::dvec3(a[0].get<double>(), a[1].get<double>(),
                        a[2].get<double>());
    };
    m.travelMin = vec("min");
    m.travelMax = vec("max");
  }
  m.fitArcs = j.value("fit_arcs", true);
  m.hasRotaryAxis = j.value("has_rotary_axis", false);
  m.rapidFeed = j.value("rapid_feed", 0.0);
  for (const json& tj : j.value("tools", json::array()))
    m.library.push_back(ToolFromJson(tj));
  return m;
}

std::vector<cam::Machine> LoadMachines(const std::string& dir,
                                       std::string* error) {
  std::vector<cam::Machine> out;
  if (error != nullptr)
    error->clear();
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec))
    return out; // no store yet is not an error: nothing has been saved
  std::string problems;
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".json")
      continue;
    try {
      std::ifstream in(entry.path());
      const json j = json::parse(in);
      out.push_back(MachineFromJson(j));
      if (out.back().name.empty())
        throw std::runtime_error("a machine needs a name");
    } catch (const std::exception& e) {
      // One broken file must not hide the others: say which, keep going.
      problems += (problems.empty() ? "" : "; ") +
                  entry.path().filename().string() + ": " + e.what();
    }
  }
  std::sort(out.begin(), out.end(),
            [](const cam::Machine& a, const cam::Machine& b) {
              return a.name < b.name;
            });
  if (error != nullptr)
    *error = problems;
  return out;
}

bool SaveMachine(const std::string& dir, const cam::Machine& m,
                 std::string* error) {
  if (m.name.empty()) {
    if (error != nullptr)
      *error = "a machine needs a name";
    return false;
  }
  if (!cam::PostExists(m.post)) {
    if (error != nullptr)
      *error = "unknown cam post '" + m.post + "'; this build accepts " +
               PostIdList();
    return false;
  }
  const std::filesystem::path file =
      std::filesystem::path(dir) / cam::MachineFileName(m.name);
  // Two names that fold to one file name ("Shapeoko 4" and "shapeoko-4")
  // would silently overwrite each other; refuse the second.
  std::error_code ec;
  if (std::filesystem::exists(file, ec)) {
    try {
      std::ifstream in(file);
      const json j = json::parse(in);
      const std::string held = j.value("name", std::string());
      if (!held.empty() && held != m.name) {
        if (error != nullptr)
          *error = "'" + m.name + "' would share a file with '" + held +
                   "' — pick a name that differs in more than case or "
                   "punctuation";
        return false;
      }
    } catch (const std::exception&) {
      // Unreadable: it is about to be replaced by something readable.
    }
  }
  std::filesystem::create_directories(dir, ec);
  return WriteTextAtomic(file, MachineToJson(m).dump(2) + "\n", error);
}

bool DeleteMachine(const std::string& dir, const std::string& name,
                   std::string* error) {
  const std::filesystem::path file =
      std::filesystem::path(dir) / cam::MachineFileName(name);
  std::error_code ec;
  if (!std::filesystem::exists(file, ec)) {
    if (error != nullptr)
      *error = "no machine called '" + name + "'";
    return false;
  }
  if (!std::filesystem::remove(file, ec)) {
    if (error != nullptr)
      *error = "could not delete " + file.string() + ": " + ec.message();
    return false;
  }
  return true;
}

json CamSetupToJson(const cam::Setup& setup) {
  json j;
  j["id"] = setup.id;
  j["name"] = setup.name;
  // The machine, EMBEDDED (v77): the copy the setup was set up on, named by
  // its `name`. `post`, `has_rotary_axis` and `fit_arcs` used to be keys of
  // the setup; a reader still accepts them from a file that predates the
  // block (the v73-v76 goldens carry them).
  j["machine"] = MachineToJson(setup.machine);
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
                     : setup.originAt == cam::OriginAt::StockMax ? "stock_max"
                                                                 : "model_min";
  // Where the bed sits on the sheet. Zero is the old behaviour, so a job that
  // fits its machine writes nothing and reads back identical.
  if (setup.reachOffset != glm::dvec2(0.0))
    j["reach_offset"] = {setup.reachOffset.x, setup.reachOffset.y};
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
  if (setup.rotationA != 0.0)
    j["rotation_a"] = setup.rotationA;
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
  for (const cam::Tool& t : setup.tools)
    j["tools"].push_back(ToolToJson(t));

  if (setup.order != cam::OrderOutputBy::Authored)
    j["order"] = OrderName(setup.order);
  if (setup.splitOutput)
    j["split_output"] =
        true; // v78; omitted when off, so old jobs re-save byte-identically
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
      // How THIS part is clamped, when it is not clamped the way the setup
      // says (v76). Doubles for the same reason the setup's frame is:
      // 0.7071067811865476 round-trips where 0.70710678f does not.
      if (p.frame)
        pj["frame"] = {p.frame->x, p.frame->y, p.frame->z, p.frame->w};
      j["placements"].push_back(std::move(pj));
    }
  }
  j["operations"] = json::array();
  for (const cam::Operation& op : setup.operations) {
    json oj = {{"id", op.id},
               {"name", op.name},
               {"type", cam::OpTypeName(op.type)},
               {"tool", op.toolNumber},
               {"top_z", op.topZ},
               {"bottom_z", op.bottomZ}};
    if (op.stepdown != 0.0)
      oj["stepdown"] = op.stepdown;
    if (op.peck != 0.0)
      oj["peck"] = op.peck; // v79; omitted when off
    if (op.stockToLeave != 0.0)
      oj["stock_to_leave"] = op.stockToLeave;
    // EVERY type that reads it, not just profiles. Adaptive clearing is the
    // one strategy that acts on `side`, and it was the one type that never
    // wrote it: a pocket saved and reloaded came back clearing OUTSIDE its
    // own boundary, where there is nothing, and produced no toolpath at all.
    // A job that worked before the save did not work after it
    // (docs/cam/dogfood-round.md).
    if (op.type != cam::OpType::Face && op.type != cam::OpType::Drill)
      oj["side"] = cam::SideName(op.side);
    if (!op.climb)
      oj["climb"] = false;
    // Written only when set, so an operation that goes around its islands —
    // every one before this — re-saves byte-identically.
    if (op.useOutline)
      oj["use_outline"] = true;
    if (op.leadArcs)
      oj["lead_arcs"] = true;  // v81; omitted when off
    if (op.drilledBores > 0.0) // v96; omitted when every opening is cut
      oj["drilled_bores"] = op.drilledBores;
    if (op.finishTool != 0) { // v82; omitted when there is no finishing pass
      oj["finish_tool"] = op.finishTool;
      oj["finish_stock"] = op.finishStock;
    }
    if (op.cornerRelief != cam::CornerRelief::None) // v80; omitted when none
      oj["corner_relief"] =
          op.cornerRelief == cam::CornerRelief::Dogbone ? "dogbone" : "tbone";
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
    else if (at == "stock_max")
      setup.originAt = cam::OriginAt::StockMax;
    else
      throw std::runtime_error(
          "unknown cam origin_at '" + at +
          "'; this build accepts stock_min, stock_centre, model_min, "
          "stock_max");
  }
  if (j.contains("reach_offset") && j["reach_offset"].is_array() &&
      j["reach_offset"].size() == 2)
    setup.reachOffset = glm::dvec2(j["reach_offset"][0].get<double>(),
                                   j["reach_offset"][1].get<double>());
  if (j.contains("frame") && j["frame"].is_array() && j["frame"].size() == 4)
    setup.frame =
        glm::dquat(j["frame"][3].get<double>(), j["frame"][0].get<double>(),
                   j["frame"][1].get<double>(), j["frame"][2].get<double>());
  setup.rotationA = j.value("rotation_a", 0.0);
  if (j.contains("machine")) {
    setup.machine = MachineFromJson(j.at("machine"));
  } else {
    // Before v77 the machine's three fields were the setup's own. Read them
    // into the job's own (unnamed) machine, so a v73-v76 file cuts exactly
    // as it did.
    setup.machine.post = j.value("post", std::string("mach3"));
    if (!cam::PostExists(setup.machine.post))
      throw std::runtime_error("unknown cam post '" + setup.machine.post +
                               "'; this build accepts " + PostIdList());
    // Before v67 there was no such flag and `rotation_a` was the only way to
    // turn a part, so a file that carries one was written by a machine that
    // has the axis. Defaulting it to false instead would silently drop the A
    // word from a rotary job that used to post one.
    setup.machine.hasRotaryAxis =
        j.value("has_rotary_axis", setup.rotationA != 0.0);
    setup.machine.fitArcs = j.value("fit_arcs", true);
  }
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
  for (const json& tj : j.value("tools", json::array()))
    setup.tools.push_back(ToolFromJson(tj));
  setup.splitOutput = j.value("split_output", false);
  if (j.contains("order"))
    setup.order = OrderFromName(j.at("order").get<std::string>());
  for (const json& pj : j.value("placements", json::array())) {
    cam::Placement p;
    p.id = pj.value("id", uint64_t{0});
    p.jobPartId = pj.value("part", uint64_t{0});
    if (pj.contains("pos") && pj["pos"].is_array() && pj["pos"].size() == 2)
      p.pos = {pj["pos"][0].get<double>(), pj["pos"][1].get<double>()};
    p.rotZdeg = pj.value("rot_z", 0.0);
    p.enabled = pj.value("enabled", true);
    if (pj.contains("frame") && pj["frame"].is_array() &&
        pj["frame"].size() == 4)
      p.frame = glm::dquat(
          pj["frame"][3].get<double>(), pj["frame"][0].get<double>(),
          pj["frame"][1].get<double>(), pj["frame"][2].get<double>());
    setup.placements.push_back(p);
  }
  for (const json& oj : j.value("operations", json::array())) {
    cam::Operation op;
    op.id = oj.value("id", uint64_t{0});
    op.name = oj.value("name", std::string());
    op.type = OpTypeFromJson(oj.value("type", std::string("profile")));
    op.toolNumber = oj.value("tool", 1);
    op.topZ = oj.value("top_z", 0.0);
    op.bottomZ = oj.value("bottom_z", -1.0);
    op.stepdown = oj.value("stepdown", 0.0);
    op.peck = oj.value("peck", 0.0);
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
    op.leadArcs = oj.value("lead_arcs", false);
    op.drilledBores = oj.value("drilled_bores", 0.0);
    op.finishTool = oj.value("finish_tool", 0);
    op.finishStock = oj.value("finish_stock", 0.0);
    if (oj.contains("corner_relief")) {
      const std::string r = oj.at("corner_relief").get<std::string>();
      if (r == "dogbone")
        op.cornerRelief = cam::CornerRelief::Dogbone;
      else if (r == "tbone")
        op.cornerRelief = cam::CornerRelief::TBone;
      else if (r == "none")
        op.cornerRelief = cam::CornerRelief::None;
      else
        throw std::runtime_error("unknown corner_relief '" + r +
                                 "'; this build accepts none, dogbone, tbone");
    }
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
