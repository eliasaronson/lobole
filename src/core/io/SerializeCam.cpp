#include "core/cam/CamJob.h"
#include "core/cam/CamMachine.h" // MachineFileName
#include "core/cam/CamNames.h"
#include "core/cam/CamOpFields.h"
#include "core/cam/CamPost.h"     // PostExists, PostIds
#include "core/cam/CamToolpath.h" // FrameIsSquare
#include "core/io/Serialization.h" // WriteTextAtomic
#include "core/io/SerializationInternal.h"

#include <fstream>

#include <filesystem>

#include <algorithm>

// CAM setups (v58, docs/cam/README.md). Small enough to live inline in the
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
// the loader now (docs/conventions.md#refuse-dont-substitute):
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
  // Same rule again (v121): a job whose tools carry no holder dimensions
  // re-saves byte-identically, and the holder check stays silent on it.
  if (t.overallLength > 0.0 || t.collarDiameter > 0.0) {
    tj["overall_length"] = t.overallLength;
    tj["collar_diameter"] = t.collarDiameter;
  }
  // And again (v131): nobody stating a shank is the tool as every check
  // took it before, the cutter's width all the way up.
  if (t.shankDiameter > 0.0)
    tj["shank_diameter"] = t.shankDiameter;
  // A thread mill's head (v145): its flat and its neck, only on one.
  if (t.type == cam::ToolType::ThreadMill) {
    tj["tip_flat"] = t.tipFlat;
    tj["neck_diameter"] = t.neckDiameter;
    tj["neck_length"] = t.neckLength;
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
  t.overallLength = tj.value("overall_length", 0.0);
  t.collarDiameter = tj.value("collar_diameter", 0.0);
  t.shankDiameter = tj.value("shank_diameter", 0.0);
  t.tipFlat = tj.value("tip_flat", 0.0);
  t.neckDiameter = tj.value("neck_diameter", 0.0);
  t.neckLength = tj.value("neck_length", 0.0);
  return t;
}

} // namespace

// A rotary mount is stated whole or not at all: half of one would post a
// guess with the confidence of a statement
// (docs/cam/rotary.md#how-the-rotary-is-installed).
cam::RotaryMount RotaryMountFromJson(const json& j) {
  cam::RotaryMount r;
  const auto str = [&](const char* key) {
    if (!j.contains(key))
      throw std::runtime_error(
          std::string("machine rotary needs `") + key +
          "`: a rotary is stated whole -- along (x|y), chuck_end (-|+), "
          "word (A|B) and top_moves (+x|-x|+y|-y) -- or not at all");
    return j.at(key).get<std::string>();
  };
  const std::string along = str("along");
  if (along != "x" && along != "y")
    throw std::runtime_error("machine rotary along '" + along +
                             "'; it lies along x or y");
  r.along = along[0];
  const std::string end = str("chuck_end");
  if (end != "-" && end != "+")
    throw std::runtime_error("machine rotary chuck_end '" + end +
                             "'; the chuck is at the - or the + end");
  r.chuckEnd = end == "-" ? -1 : 1;
  const std::string word = str("word");
  if (word != "A" && word != "B")
    throw std::runtime_error("machine rotary word '" + word +
                             "'; the controller drives it as A or B");
  r.word = word[0];
  const std::string top = str("top_moves");
  if (top.size() != 2 || (top[0] != '+' && top[0] != '-') ||
      (top[1] != 'x' && top[1] != 'y'))
    throw std::runtime_error("machine rotary top_moves '" + top +
                             "'; it is +x, -x, +y or -y");
  if (top[1] == r.along)
    throw std::runtime_error(
        "machine rotary top_moves '" + top +
        "' is along the rotary itself; "
        "the top of a turning part moves across it, " +
        std::string(r.along == 'x' ? "+y or -y" : "+x or -x"));
  r.topSign = top[0] == '+' ? 1 : -1;
  r.topAxis = top[1];
  return r;
}

cam::RotaryHolding RotaryHoldingFromJson(const json& h) {
  cam::RotaryHolding out;
  out.chuckDiameter = h.value("chuck_diameter_mm", 0.0);
  out.gripLength = h.value("grip_mm", 0.0);
  out.hasTailstock = h.value("tailstock", false);
  out.centreLength = h.value("centre_length_mm", 0.0);
  out.centreDiameter = h.value("centre_diameter_mm", 0.0);
  out.tailBodyDiameter = h.value("tailstock_body_diameter_mm", 0.0);
  out.clearance = h.value("clearance_mm", 0.0);
  for (const double v :
       {out.chuckDiameter, out.gripLength, out.centreLength, out.centreDiameter,
        out.tailBodyDiameter, out.clearance})
    if (v < 0.0)
      throw std::runtime_error(
          "machine holding sizes are millimetres, 0 for not measured; "
          "none can be negative");
  return out;
}

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
  if (m.rotaryMaxFeed != 0.0) // v123
    j["a_max_deg_min"] = m.rotaryMaxFeed;
  // v132: how the rotary is installed, and what holds a part in it -- each
  // written only when stated, so a machine that never mentioned either
  // re-saves byte-identically.
  if (m.rotary.Stated())
    j["rotary"] = {{"along", std::string(1, m.rotary.along)},
                   {"chuck_end", m.rotary.chuckEnd < 0 ? "-" : "+"},
                   {"word", std::string(1, m.rotary.word)},
                   {"top_moves", std::string(m.rotary.topSign < 0 ? "-" : "+") +
                                     std::string(1, m.rotary.topAxis)}};
  if (m.holding != cam::RotaryHolding{}) {
    json h;
    const auto put = [&](const char* key, double v) {
      if (v != 0.0)
        h[key] = v;
    };
    put("chuck_diameter_mm", m.holding.chuckDiameter);
    put("grip_mm", m.holding.gripLength);
    if (m.holding.hasTailstock)
      h["tailstock"] = true;
    put("centre_length_mm", m.holding.centreLength);
    put("centre_diameter_mm", m.holding.centreDiameter);
    put("tailstock_body_diameter_mm", m.holding.tailBodyDiameter);
    put("clearance_mm", m.holding.clearance);
    j["holding"] = h;
  }
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
  m.rotaryMaxFeed = j.value("a_max_deg_min", 0.0);
  if (j.contains("rotary"))
    m.rotary = RotaryMountFromJson(j.at("rotary"));
  if (j.contains("holding"))
    m.holding = RotaryHoldingFromJson(j.at("holding"));
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
  // The setup's own traverse rate (v139), seeded from the machine's but its
  // own from then on: before it was saved a reload fell back to 0, "not
  // stated", and the time estimates left the rapids out. Absent = 0.
  if (setup.rapidFeed != 0.0)
    j["rapid_feed"] = setup.rapidFeed;
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
  // v132: where the part is held, how much of it the jaws hold, and whether
  // the tailstock is engaged -- each only when stated.
  if (setup.heldIn != cam::Setup::HeldIn::Derived)
    j["held_in"] =
        setup.heldIn == cam::Setup::HeldIn::Rotary ? "rotary" : "bed";
  if (setup.gripLength != 0.0)
    j["grip_mm"] = setup.gripLength;
  if (setup.tailstock != -1)
    j["tailstock"] = setup.tailstock == 1;
  if (setup.chuck != -1)
    j["chuck"] = setup.chuck == 1;
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
  // What holds the stock down (v126, docs/cam/setup.md#workholding-fixtures).
  // Omitted when there are none and each key at its default, so every older job
  // re-saves byte-identically.
  if (!setup.fixtures.empty()) {
    j["fixtures"] = json::array();
    for (const cam::Fixture& f : setup.fixtures) {
      // A part, or a keep-out box: one key or the other, never both.
      json fj = {{"id", f.id}};
      if (f.IsBox())
        fj["box"] = {f.box.x, f.box.y, f.box.z};
      else
        fj["part"] = f.sourceDocId;
      if (!f.name.empty())
        fj["name"] = f.name;
      if (f.pos != glm::dvec3(0.0))
        fj["pos"] = {f.pos.x, f.pos.y, f.pos.z};
      if (f.rotZdeg != 0.0)
        fj["rot_z"] = f.rotZdeg;
      if (!f.enabled)
        fj["enabled"] = false;
      // v127: resting on the stock (docs/cam/setup.md#seats). `pos`
      // and `rot_z` above are then the pose it last solved to.
      if (f.seat) {
        fj["seat"] = {{"face",
                       {{"feature", f.seat->face.featureId},
                        {"name", f.seat->face.name}}},
                      {"edge", cam::StockEdgeName(f.seat->edge)},
                      {"along_mm", f.seat->alongMm},
                      {"over_mm", f.seat->overMm}};
        if (f.seat->spinDeg != 0.0)
          fj["seat"]["spin_deg"] = f.seat->spinDeg;
      }
      j["fixtures"].push_back(std::move(fj));
    }
  }
  j["operations"] = json::array();
  for (const cam::Operation& op : setup.operations) {
    json oj = {{"id", op.id},
               {"name", op.name},
               {"type", cam::OpTypeNameOrThrow(op.type)},
               {"tool", op.toolNumber},
               {"top_z", op.topZ},
               {"bottom_z", op.bottomZ}};
    // Every field in the op-field table, by VALUE and never by type: left
    // out when it equals a default-built Operation's, so a file re-saves
    // byte for byte
    // (docs/cam/operations.md#the-capability-table-and-the-field-table).
    // Writing a field only when its owner's type was on a list is how
    // adaptive's `stepover` and then its `side` were each dropped on save once.
    cam::WriteOpFields(op, oj);
    // The rows with rules of their own. v82: the finishing pass, omitted
    // when there is none -- and its stock only beside it.
    if (op.finishTool != 0) {
      oj["finish_tool"] = op.finishTool;
      oj["finish_stock"] = op.finishStock;
    }
    if (op.tabCount != 0) {
      oj["tabs"] = {{"count", op.tabCount},
                    {"width", op.tabWidth},
                    {"height", op.tabHeight}};
    }
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
  // Refused rather than read as 0: a rate that is not a number, or is
  // negative, is a damaged file, and 0 would quietly drop the rapids from
  // every time estimate.
  if (j.contains("rapid_feed")) {
    const json& r = j.at("rapid_feed");
    if (!r.is_number() || r.get<double>() < 0.0)
      throw std::runtime_error("setup rapid_feed must be a number of mm/min, "
                               "0 or more; got " +
                               r.dump());
    setup.rapidFeed = r.get<double>();
  }
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
  if (j.contains("held_in")) {
    const std::string h = j.at("held_in").get<std::string>();
    if (h != "rotary" && h != "bed")
      throw std::runtime_error("setup held_in '" + h +
                               "'; a part is held in the rotary or on the bed");
    setup.heldIn =
        h == "rotary" ? cam::Setup::HeldIn::Rotary : cam::Setup::HeldIn::Bed;
  }
  setup.gripLength = j.value("grip_mm", 0.0);
  if (j.contains("tailstock"))
    setup.tailstock = j.at("tailstock").get<bool>() ? 1 : 0;
  if (j.contains("chuck"))
    setup.chuck = j.at("chuck").get<bool>() ? 1 : 0;
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
  for (const json& fj : j.value("fixtures", json::array())) {
    cam::Fixture f;
    f.id = fj.value("id", uint64_t{0});
    f.sourceDocId = fj.value("part", uint64_t{0});
    if (fj.contains("box") && fj["box"].is_array() && fj["box"].size() == 3)
      f.box = {fj["box"][0].get<double>(), fj["box"][1].get<double>(),
               fj["box"][2].get<double>()};
    f.name = fj.value("name", std::string{});
    if (fj.contains("pos") && fj["pos"].is_array() && fj["pos"].size() == 3)
      f.pos = {fj["pos"][0].get<double>(), fj["pos"][1].get<double>(),
               fj["pos"][2].get<double>()};
    f.rotZdeg = fj.value("rot_z", 0.0);
    f.enabled = fj.value("enabled", true);
    if (fj.contains("seat") && fj["seat"].is_object()) {
      const json& sj = fj["seat"];
      cam::FixtureSeat seat;
      const json face = sj.value("face", json::object());
      seat.face.featureId = face.value("feature", uint64_t{0});
      seat.face.name = face.value("name", std::string{});
      const std::string edge = sj.value("edge", std::string{});
      // Refused, not defaulted: a clamp read onto the wrong edge loads, looks
      // placed, and is on the wrong side of the part.
      if (!cam::ParseStockEdge(edge, seat.edge))
        throw std::runtime_error("unknown fixture seat edge '" + edge +
                                 "'; use -x, +x, -y or +y");
      seat.alongMm = sj.value("along_mm", 0.0);
      seat.overMm = sj.value("over_mm", 0.0);
      seat.spinDeg = sj.value("spin_deg", 0.0);
      f.seat = seat;
    }
    setup.fixtures.push_back(std::move(f));
  }
  for (const json& oj : j.value("operations", json::array())) {
    cam::Operation op;
    op.id = oj.value("id", uint64_t{0});
    op.name = oj.value("name", std::string());
    op.type = OpTypeFromJson(oj.value("type", std::string("profile")));
    op.toolNumber = oj.value("tool", 1);
    op.topZ = oj.value("top_z", 0.0);
    op.bottomZ = oj.value("bottom_z", -1.0);
    cam::ReadOpFields(oj, op);
    // A clearing operation clears INSIDE its boundary; "outside" is a
    // profile's notion. So the default when the key is missing depends on the
    // type — which is what repairs the documents written before this was
    // saved at all, without asking anyone to re-make their job.
    if (!oj.contains("side") &&
        cam::OpTypeHas(op.type, cam::OpField::ClearsInside))
      op.side = cam::Side::Inside;
    op.finishTool = oj.value("finish_tool", 0);
    op.finishStock = oj.value("finish_stock", 0.0);
    if (oj.contains("tabs")) {
      op.tabCount = oj["tabs"].value("count", 0);
      op.tabWidth = oj["tabs"].value("width", 6.0);
      op.tabHeight = oj["tabs"].value("height", 1.5);
    }
    for (const json& rj : oj.value("targets", json::array()))
      op.targets.push_back(TargetFromJson(rj));
    setup.operations.push_back(std::move(op));
  }
  return setup;
}

} // namespace DocumentIO
