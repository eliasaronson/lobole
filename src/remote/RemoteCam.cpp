#include "remote/RemoteCam.h"

#include "core/cam/CamNames.h"

#ifdef HAVE_CAM

#include "core/Document.h"
#include "core/cam/CamFeeds.h"
#include "core/cam/CamMachine.h"
#include "core/cam/CamPost.h"
#include "core/cam/CamSim.h"
#include "core/cam/CamToolpath.h"
#include "core/cam/CamVerify.h"
#include "core/io/Serialization.h"
#include "ui/AppActions.h" // JobUndoGuard
#include "ui/AppState.h"
#include "ui/CamJobs.h"
#include "ui/CamPanel.h" // RefreshCamToolpaths
#include "ui/CamPlayback.h"
#include "ui/JobActions.h"
#include "ui/MachineActions.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>

namespace remote {

using nlohmann::json;

namespace {

constexpr MethodDoc kCamMethods[] = {
    {"list_machines",
     "Your machines, from ~/.config/designer/machines/*.json: each {name, "
     "post, travel?: {min, max}, fit_arcs, has_rotary_axis, rapid_feed, "
     "tools}. A setup names one and EMBEDS a copy of it; set_cam_setup "
     "{machine} puts one on a setup.",
     ""},
    {"save_machine",
     "Write a machine to the store {machine: {name, post?, travel?: {min: "
     "[x,y,z], max: [x,y,z]}, fit_arcs?, has_rotary_axis?, rapid_feed?, "
     "tools?: [...]}}, replacing one of that name. Refused without a name, "
     "with an unknown post (naming the accepted ones), or with a name that "
     "would share a file with a different name. Open jobs are not changed: a "
     "setup keeps the copy it was set up on, and list_cam_setups reports "
     "`machine_match` so a script can see that it now differs.",
     "machine"},
    {"delete_machine", "Remove a machine from the store {name}.", "name"},
    {"machine_part",
     "Make a JOB for a part and switch to it {part?: id or name (default: the "
     "open part), name?}. A job is a document of its own since v73 -- it "
     "REFERENCES parts and says how they are cut -- so this is the one step "
     "from a part you have open to a tab you can add setups on. Returns the "
     "job's tab id and the job part it holds.",
     "part, name"},
    {"add_job_part",
     "Add a part to the open job {part: id or name} -- another thing this job "
     "cuts. Returns the job part's id, which is what an operation target and "
     "a placement name. A job is what makes four brackets out of one sheet "
     "expressible; before v73 a setup could only ever be about one part.",
     "part"},
    {"remove_job_part",
     "Drop a part from the job {id}. Every placement and every operation "
     "target that named it goes with it -- a target on a part the job no "
     "longer holds would resolve against whatever body answered to the same "
     "element name, which is the wrong part, cut for real.",
     "id"},
    {"list_job_parts",
     "The parts this job cuts: id, source document, name, whether the source "
     "has changed since the job last saw it (`stale`), and where each sits in "
     "each setup.",
     ""},
    {"check_layout",
     "How the parts are arranged in the stock {setup?}: per placement, "
     "whether it is `inside` the stock, `partly_outside` (hanging over an "
     "edge) or `fully_outside` (not on it at all), and which other placements "
     "it runs into. Computed when asked, not while you drag: a sheet is laid "
     "out THROUGH states like these. The refusal lands at cam_toolpath and "
     "cam_post, where the mistake becomes G-code.",
     "setup"},
    {"set_job_placement",
     "Where a part sits in the stock {setup, part, pos?: [x,y], rot_z?, "
     "enabled?, lay_face_up?: {feature_id, element}}. Position and spin are "
     "IN-PLANE: anything out of plane is fixturing, and two ways to tilt a "
     "part is a mistake this project already paid for once "
     "(docs/cam/setup-frame.md). `lay_face_up` is the out-of-plane half and "
     "clamps THIS part with that face at the spindle -- so one sheet can hold "
     "a baffle on its face beside a rail on its edge. It says nothing about "
     "the machine: no A word is posted.",
     "setup, part, pos, rot_z, enabled, lay_face_up"},
    {"add_cam_setup",
     "Add a milling setup {name?, post?: mach3|linuxcnc|grbl, safe_z?}; "
     "returns "
     "its id and the safe_z derived for it. A setup arrives with one 6 mm "
     "tool, because a setup with no tools can produce nothing, and with a "
     "rapid height clear of the stock, because the default 5 mm is inside "
     "most of them. `machine` puts one of your machines (list_machines) on "
     "it -- post, arcs, rotary axis, work area, rapid -- and the setup "
     "EMBEDS a copy, so the job file opens anywhere.",
     "machine, name, post, safe_z"},
    {"list_cam_setups",
     "Every setup: id, name, post, safe_z, origin (Z zero), top_face (the "
     "face it was picked from), origin_at (the corner it is measured FROM), "
     "frame (how the part is CLAMPED, as "
     "[x,y,z,w]; identity = as modelled), has_rotary_axis, rotation_a, "
     "fit_arcs, machine (the embedded copy) and machine_match "
     "(unnamed|global|differs|missing -- how that copy stands against your "
     "store, by name), tools, "
     "operations, and the stock box derived from the model — in SETUP space, "
     "so a setup that lays the part flat reports the box the machine sees.",
     ""},
    {"set_cam_setup",
     "Change a setup {id, name?, machine?: name, post?, safe_z?, "
     "lay_face_up?: {feature_id, "
     "element}, flip?, has_rotary_axis?, rotation_a?, fit_arcs?, "
     "origin_at?: stock_min|stock_centre|model_min|stock_max, "
     "reach_offset?: [x,y] | \"auto\", "
     "order?: authored|tool|part, split_output?: bool (one program per "
     "tool, which forces tool order), "
     "stock?: {mode?: relative|fixed, size?: [x,y,z], place?: [x,y,z], "
     "margin?: [x,y,z], bottom_margin?}} or "
     "one of its "
     "tools {id, tool: {number, name?, diameter?, tool_type?: "
     "flat|ball|vbit|drill, tip_angle?, feed_cut?, feed_plunge?, "
     "spindle_rpm?, flutes?}} — the tool is added when the number is new. "
     "tool_type is the shape of the end: it decides what the verifier thinks "
     "was removed, and an edge_round op reads it to know whether it is "
     "cutting a bevel (vbit, tip_angle is the INCLUDED angle) or an arc "
     "(ball). An unknown name is refused, not read as flat. "
     "lay_face_up CLAMPS the part with that face toward the spindle — a "
     "fixturing statement, so it posts no rotary move and any flat face "
     "works. It is what lets a part modelled upright (an imported panel, a "
     "speaker baffle) be cut without a rotated copy of it, and repeating the "
     "call is idempotent. flip turns the fixture over for the second side. "
     "rotation_a is the MACHINE's rotary index and needs has_rotary_axis: "
     "without it no file can carry an A word. "
     "`reach_offset` is WHERE THE BED IS ON THE SHEET, from the stock's "
     "near corner: a sheet longer than the machine is slid through and "
     "clamped where the work is, and without this the reach window is nailed "
     "to the stock's corner — an assumption that decides what gets clipped "
     "and that MOVES when you turn the part round. \"auto\" puts the window "
     "on the cutting. origin_at is WHERE X0 Y0 IS in the posted file — the "
     "corner the "
     "operator touches off on; the post subtracts it from every coordinate, "
     "and Z comes from the picked top face. stock.mode `fixed` makes the "
     "workpiece the sheet you own (size + place, where the part sits in it) "
     "rather than a shrink-wrap of the part; a sheet smaller than the part is "
     "refused, naming the axis and the shortfall.",
     "fit_arcs, flip, has_rotary_axis, id, lay_face_up, machine, name, "
     "order, origin_at, post, reach_offset, rotation_a, safe_z, split_output, "
     "stock, tool"},
    {"remove_cam_setup", "Delete a setup {id} and its operations.", "id"},
    {"add_cam_op",
     "Add an operation {setup, type: "
     "profile|pocket|face|drill|adaptive|edge_round, "
     "tool?, top_z?, bottom_z?, stepdown?, peck? (drill: retract between "
     "pecks of this depth; derived as the tool's diameter once a hole is "
     "deeper than three), corner_relief?: none|dogbone|tbone (profile and "
     "pocket: clear the inside corners a round cutter leaves, by the tool's "
     "radius -- along the bisector, or along the longer wall), lead_arcs?: "
     "bool (profile: leave the wall on a tangent arc of the tool's radius "
     "instead of stopping against it -- no dwell mark; refused inside an "
     "opening with no room for it), finish_tool?: number (rough with `tool` "
     "leaving finish_stock -- derived as a tenth of its diameter -- then "
     "take the wall in one full-depth pass with this tool; two toolpaths "
     "from one operation), finish_stock?, side?: "
     "outside|inside|on, climb?, "
     "stepover?, stock_to_leave?, tabs?: {count, width, height}, targets?: "
     "[{feature_id, element, part?}]}; EVERY target is cut -- five holes "
     "picked into one operation are five bores, in the order given -- and "
     "one that cannot be followed is named in cam_toolpath's `skipped` "
     "without costing the others. `part` is the job part the element is "
     "on (list_elements reports it as `job_part`), and is REQUIRED when the "
     "same element name appears on more than one part of the job -- guessing "
     "there machines the wrong one. Returns its id and every field derived for "
     "it. LEAVE OUT top_z/bottom_z and they are derived from what the "
     "operation acts on — the same derivation the GUI's verbs use, so a "
     "script does not type a depth the panel would not let a person type. "
     "Pocket: pick the floor. Profile: pick a wall (an opening cuts inside "
     "it, the perimeter cuts outside) or nothing at all to cut the part "
     "free. Face: pick nothing. Edge_round: pick the WALL whose top edge to "
     "break — a V-bit chamfers it, a ball rounds it, a flat mill is refused; "
     "edge_size? sets the bevel width / round radius, and left out it is the "
     "biggest break that tool can make within the wall. A refusal says what "
     "to pick instead. A "
     "derived pocket deep enough to want adaptive clearing COMES BACK as "
     "type adaptive; set_cam_op forces either strategy.",
     "bottom_z, setup, side, target, targets, tool, top_z, type"},
    {"set_cam_op",
     "Change an operation {id, …the add_cam_op fields}. `tool` re-points it "
     "at another cutter: a stepdown still at the old tool's derived value "
     "follows the new one (every derived number is a fraction of the "
     "cutter), one you typed is kept, and a tool this setup does not have is "
     "refused.",
     "id"},
    {"move_cam_op",
     "Move an operation to a position {id, to} (0-based) and return the "
     "setup's new order. Operation order IS the cutting sequence — the "
     "generator and the post both walk it — so it decides what is roughed "
     "before what is finished, and where the tool changes and their M0 "
     "pauses fall.",
     "id, to"},
    {"remove_cam_op", "Delete an operation {id}.", "id"},
    {"cam_derive_op",
     "What {setup, type, tool?, target?: {feature_id, element}} would produce "
     "WITHOUT adding it: the derived operation, or why that pick cannot make "
     "one. The dry run behind add_cam_op's derivation.",
     "bottom_z, setup, side, target, targets, tool, top_z, type"},
    {"cam_toolpath",
     "Generate toolpaths for a setup {setup, async?} without writing a file: "
     "per operation, the move count, cut length (mm), lowest Z, and the "
     "error when it produced nothing. `skipped` names the targets of a "
     "multi-target operation that could not be followed while the others "
     "could, and `clipped_mm` how much cutting fell beyond the machine's "
     "travel and is NOT in the file -- both also as `warnings`, which "
     "carries the work-area verdict too. The cheap way to check a job. "
     "Synchronous by default -- an adaptive pocket costs seconds and this "
     "waits for it. `async: true` hands the adaptive passes to a background "
     "job and answers {pending, queued_passes, job: \"cam\"} at once; "
     "watch `background_jobs` until none of kind cam remains, then call "
     "again without `async` for the list.",
     "async, path, post, setup"},
    {"cam_verify",
     "Simulate the POSTED G-code for a setup {setup, resolution?} and compare "
     "the result with the model: {max_gouge_mm, max_leftover_mm, "
     "leftover_area_mm2, leftover_regions, rapids_through_stock, "
     "below_stock_mm, ok, complete, says}. `below_stock_mm` is depth cut past "
     "the WORKPIECE — the spoilboard, which is what a through cut is for; it "
     "is reported, never counted as gouge. "
     "`ok` = nothing scraps the part or breaks the tool (no "
     "gouge, no rapid through material) and is the one to gate on; `complete` "
     "= nothing left standing, which a correct job with tabs or a dropping "
     "slug will NOT satisfy. "
     "Runs on the file, not the toolpath, so it catches post and modal-state "
     "bugs too. `says` is the verdict in the same sentences the panel shows. "
     "Leftover in several tiny regions is the inside corners a round cutter "
     "cannot reach and is CORRECT; one large region is a missing operation. "
     "`leftover_biggest` lists where, largest first — a height map cannot "
     "watch a slug fall out or know a tab was asked for, so those show up "
     "here too and are for you to recognise.",
     "resolution, setup"},
    {"cam_simulate",
     "Play the POSTED file through a block of stock and report what has come "
     "off at a point in it {setup?, at?: 0..1 along the path or \"end\", "
     "play?, close?, resolution?}. Returns {open, at, distance_mm, total_mm, "
     "cutting_minutes, removed_mm3, operation, tool, feed_mm_min, line, "
     "position}. The cursor is PATH DISTANCE, not a clock: every cutting "
     "feed is in the file but no machine states its rapid rate, so a time "
     "axis would be part measurement and part invention — `cutting_minutes` "
     "counts what the file states and nothing else. Generates the toolpaths "
     "if they are not current, and leaves the app SHOWING the simulation "
     "(screenshot it, or `close` to put the model back).",
     "at, close, play, resolution, setup"},
    {"cam_post",
     "Write the G-code for a setup {setup, path, post?}. Reports the line "
     "count, the ops it skipped and why, the targets a multi-target op could "
     "not follow (`skipped`), how much cutting lay beyond the travel and is "
     "missing from the file (`clipped_mm`), and `warnings`. Writes a "
     "file; it does not send it "
     "to a machine. With split_output on the setup, `path` is the base and "
     "one file per tool is written beside it as `<base> - T<n>.<ext>`, each "
     "complete and starting by loading its tool; `files` names every one.",
     "path, post, setup"},
};

cam::Side SideFrom(const std::string& s) {
  cam::Side side{};
  if (cam::TrySideFromName(s, side))
    return side;
  // `side` was the one CAM enum that was silent on BOTH sides -- here and in
  // the loader. add_cam_op(side: "insde") became an OUTSIDE cut: for a pocket
  // that is a pass around the outside of a boundary with nothing to remove,
  // which posts as a program that cuts air.
  throw std::runtime_error("unknown side '" + s + "'; use " +
                           cam::SideNameList());
}

// The operation type's names moved to core/cam/CamNames.h, which is the one
// row per type the loader, the RPC surface and the toolpath generator all
// read. The row ORDER there is this table's old order, so the refusal below
// still names them in the same sequence, character for character.

// The tool's shape and the side both moved to core/cam/CamNames.h, which is
// the one row per value the loader, the RPC surface and the toolpath
// generator read. The row ORDER there is these tables' old order, so the
// refusals still name them in the same sequence, character for character.

cam::ToolType ToolTypeFrom(const std::string& s) {
  cam::ToolType t{};
  if (cam::TryToolTypeFromName(s, t))
    return t;
  throw std::runtime_error("unknown tool type '" + s + "'; use " +
                           cam::ToolTypeNameList());
}

const char* TypeName(cam::OpType t) { return cam::OpTypeName(t); }

cam::OpType TypeFrom(const std::string& s) {
  cam::OpType t{};
  if (cam::TryOpTypeFromName(s, t))
    return t;
  throw std::runtime_error("unknown operation type '" + s + "'; use " +
                           cam::OpTypeNameList());
}

cam::Setup& RequireSetup(cam::JobDocument& job, uint64_t id) {
  cam::Setup* setup = job.FindSetup(id);
  if (setup == nullptr)
    throw std::runtime_error("no CAM setup with id " + std::to_string(id) +
                             "; list_cam_setups shows the ones that exist");
  return *setup;
}

// The setup's own post options, so a file written through the RPC is the
// file the panel would write — arc fitting is a property of the machine
// (C7), not of who asked for the G-code.
cam::PostOptions PostOptsOf(const cam::Setup& setup) {
  cam::PostOptions opts;
  opts.fitArcs = setup.machine.fitArcs;
  return opts;
}

// An operation's name, for a message about it, by id.
std::string OpName(const cam::Setup& setup, uint64_t opId) {
  for (const cam::Operation& op : setup.operations)
    if (op.id == opId)
      return op.name;
  return {};
}

// The setup that owns an operation, plus the operation itself.
std::pair<cam::Setup*, cam::Operation*> FindOp(cam::JobDocument& job,
                                               uint64_t opId) {
  for (cam::Setup& setup : job.GetSetups())
    for (cam::Operation& op : setup.operations)
      if (op.id == opId)
        return {&setup, &op};
  return {nullptr, nullptr};
}

// `keepType` is set when the operation was DERIVED: the `type` in the params
// was then the verb ("pocket"), and the derivation has already turned it into
// the strategy the geometry calls for ("adaptive", C8). Applying the verb over
// the top threw that away and cut a deep pocket in concentric rings.
void ApplyOpFields(ui::AppState& state, const cam::Setup& setup,
                   cam::Operation& op, const RpcParams& p,
                   bool keepType = false) {
  if (p.contains("type") && !keepType)
    op.type = TypeFrom(p.at("type").get<std::string>());
  if (p.contains("name"))
    op.name = p.at("name").get<std::string>();
  if (p.contains("tool")) {
    // Through the same retool the panel's combo uses: a stepdown still at the
    // old cutter's derived value follows the new one, and a tool the setup
    // does not have is REFUSED rather than silently cutting with tool 1.
    std::string why;
    if (!cam::Retool(setup, op, p.at("tool").get<int>(), why))
      throw std::runtime_error(why);
  }
  if (p.contains("top_z"))
    op.topZ = p.at("top_z").get<double>();
  if (p.contains("bottom_z"))
    op.bottomZ = p.at("bottom_z").get<double>();
  if (p.contains("stepdown"))
    op.stepdown = p.at("stepdown").get<double>();
  if (p.contains("peck"))
    op.peck = p.at("peck").get<double>();
  if (p.contains("lead_arcs"))
    op.leadArcs = p.at("lead_arcs").get<bool>();
  if (p.contains("finish_tool")) {
    op.finishTool = p.at("finish_tool").get<int>();
    if (op.finishTool != 0 && setup.FindTool(op.finishTool) == nullptr)
      throw std::runtime_error("finish_tool " + std::to_string(op.finishTool) +
                               " is not in this setup's tool table");
    if (op.finishTool != 0 && op.finishTool == op.toolNumber)
      throw std::runtime_error(
          "finish_tool is the roughing tool; a finishing pass needs another");
    if (op.finishTool != 0 && !p.contains("finish_stock"))
      if (const cam::Tool* rough = setup.FindTool(op.toolNumber))
        op.finishStock = cam::DefaultFinishStock(*rough);
  }
  if (p.contains("finish_stock"))
    op.finishStock = p.at("finish_stock").get<double>();
  if (p.contains("corner_relief")) {
    const std::string r = p.at("corner_relief").get<std::string>();
    if (r == "none")
      op.cornerRelief = cam::CornerRelief::None;
    else if (r == "dogbone")
      op.cornerRelief = cam::CornerRelief::Dogbone;
    else if (r == "tbone")
      op.cornerRelief = cam::CornerRelief::TBone;
    else
      throw std::runtime_error("unknown corner_relief '" + r +
                               "'; accepted: none, dogbone, tbone");
  }
  if (p.contains("stock_to_leave"))
    op.stockToLeave = p.at("stock_to_leave").get<double>();
  if (p.contains("side"))
    op.side = SideFrom(p.at("side").get<std::string>());
  if (p.contains("climb"))
    op.climb = p.at("climb").get<bool>();
  if (p.contains("use_outline"))
    op.useOutline = p.at("use_outline").get<bool>();
  if (p.contains("stepover"))
    op.stepover = p.at("stepover").get<double>();
  if (p.contains("edge_size"))
    op.edgeSize = p.at("edge_size").get<double>();
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
            "each target needs {feature_id, element}, and `part` when the job "
            "holds more than one; list_elements reports both");
      // Which job part the element is on. `part` says it outright --
      // list_elements reports `job_part` beside every body for exactly this
      // -- and without one it is worked out, which is only an answer while
      // the name is unique across the job (v76).
      std::string why;
      const cam::Target t =
          ui::TargetFor(state, ref, tj.value("part", uint64_t{0}), &why);
      if (t.jobPartId == 0)
        throw std::runtime_error(why);
      op.targets.push_back(t);
    }
  }
  (void)state;
}

json ToolJson(const cam::Tool& t) {
  return json{
      {"number", t.number},         {"name", t.name},
      {"diameter", t.diameter},     {"tool_type", cam::ToolTypeName(t.type)},
      {"tip_angle", t.tipAngleDeg}, {"flutes", t.flutes},
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
            {"peck", op.peck},
            {"lead_arcs", op.leadArcs},
            {"finish_tool", op.finishTool},
            {"finish_stock", op.finishStock},
            {"corner_relief",
             op.cornerRelief == cam::CornerRelief::Dogbone ? "dogbone"
             : op.cornerRelief == cam::CornerRelief::TBone ? "tbone"
                                                           : "none"},
            // Both settable since they landed and neither reported, so a
            // script could state a boundary and an allowance it could not
            // then read back — and a job built over the RPC could not be
            // checked against the one the panel shows.
            {"stock_to_leave", op.stockToLeave},
            {"use_outline", op.useOutline},
            {"enabled", op.enabled}};
  if (op.type == cam::OpType::EdgeRound) {
    j["edge_size"] = op.edgeSize;
    j["stepover"] = op.stepover;
  }
  // Side is reported for the clearing types too. It was profile-only, so a
  // pocket built over the RPC kept the struct's Outside default INVISIBLY —
  // and adaptive, the one strategy that reads it, then cleared outside a
  // boundary with nothing beyond it and produced silence.
  if (op.type != cam::OpType::Face && op.type != cam::OpType::Drill) {
    j["side"] = cam::SideName(op.side);
    if (op.tabCount > 0)
      j["tabs"] = {{"count", op.tabCount},
                   {"width", op.tabWidth},
                   {"height", op.tabHeight}};
  }
  if (!op.targets.empty()) {
    j["targets"] = json::array();
    for (const cam::Target& t : op.targets) {
      json tj = {{"feature_id", t.ref.featureId}, {"element", t.ref.name}};
      if (t.jobPartId != 0)
        tj["part"] = t.jobPartId;
      j["targets"].push_back(std::move(tj));
    }
  }
  return j;
}

} // namespace

namespace {
// `machine: name` on add_cam_setup / set_cam_setup: the global of that name,
// copied onto the setup. Unknown is refused naming the ones that exist.
[[noreturn]] void RefuseUnknownMachine(const ui::AppState& state,
                                       const std::string& name) {
  std::string known;
  for (const cam::Machine& m : state.machines)
    known += (known.empty() ? "" : ", ") + m.name;
  throw std::runtime_error("no machine called '" + name + "'; " +
                           (known.empty()
                                ? std::string("save_machine defines one")
                                : "list_machines shows " + known));
}
void ApplyNamedMachine(ui::AppState& state, cam::Setup& setup,
                       const std::string& name) {
  const cam::Machine* mc = cam::FindMachine(state.machines, name);
  if (mc == nullptr)
    RefuseUnknownMachine(state, name);
  ui::ApplyMachine(setup, *mc);
}
const char* MatchName(cam::MachineMatch match) {
  switch (match) {
  case cam::MachineMatch::Unnamed:
    return "unnamed";
  case cam::MachineMatch::Global:
    return "global";
  case cam::MachineMatch::Differs:
    return "differs";
  case cam::MachineMatch::Missing:
    return "missing";
  }
  return "unnamed";
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
  (void)doc;

  if (m == "machine_part") {
    // Resolved BEFORE the tab switch: `state.document` is null on a job tab,
    // so "the open part" has to mean the one that was open when asked.
    uint64_t sourceId = 0;
    std::string sourceName;
    if (p.contains("part")) {
      const nlohmann::json& pj = p.at("part");
      for (const ui::PartSlot& slot : state.parts) {
        if (slot.kind != ui::DocKind::Part)
          continue;
        const bool match = pj.is_string() ? slot.name == pj.get<std::string>()
                                          : slot.id == pj.get<uint64_t>();
        if (match) {
          sourceId = slot.id;
          sourceName = slot.name;
        }
      }
      if (sourceId == 0)
        throw std::runtime_error("no part called '" + pj.dump() +
                                 "'; list_documents shows them");
    } else {
      const ui::PartSlot& active = state.parts[state.activePart];
      if (active.kind != ui::DocKind::Part)
        throw std::runtime_error(
            "machine_part needs a part; pass `part` or switch to a part tab");
      sourceId = active.id;
      sourceName = active.name;
    }
    const size_t tab =
        ui::CreateJobForPart(state, sourceId, p.value("name", std::string()));
    cam::JobDocument* made = ui::ActiveJob(state);
    if (made == nullptr || made->GetParts().empty())
      throw std::runtime_error("could not create a job for '" + sourceName +
                               "'");
    return json{{"id", state.parts[tab].id},
                {"name", state.parts[tab].name},
                {"part", json{{"id", made->GetParts()[0].id},
                              {"source", sourceId},
                              {"name", sourceName}}}};
  }

  if (m == "list_machines") {
    json out = json::array();
    for (const cam::Machine& mc : state.machines)
      out.push_back(DocumentIO::MachineToJson(mc));
    return json{{"machines", out}, {"dir", ui::MachineDir()}};
  }
  if (m == "save_machine") {
    const cam::Machine mc = DocumentIO::MachineFromJson(p.at("machine"));
    std::string error;
    if (!ui::SaveMachine(state, mc, &error))
      throw std::runtime_error(error);
    return DocumentIO::MachineToJson(mc);
  }
  if (m == "delete_machine") {
    std::string error;
    if (!ui::DeleteMachine(state, p.at("name").get<std::string>(), &error))
      throw std::runtime_error(error);
    return json{{"deleted", p.at("name").get<std::string>()}};
  }

  cam::JobDocument* job = ui::ActiveJob(state);
  // One undo step per RPC that changes the job (R7); a refused call moves
  // nothing and records nothing.
  std::optional<ui::JobUndoGuard> undoGuard;
  if (job != nullptr)
    undoGuard.emplace(state);
  if (job == nullptr)
    throw std::runtime_error(
        "CAM works on a job, not a part: a setup stopped being part of the "
        "part in v73. `machine_part` makes a job from the part you have open "
        "and switches to it");

  if (m == "add_job_part") {
    if (!p.contains("part"))
      throw std::runtime_error("add_job_part needs {part: id or name}");
    const nlohmann::json& pj = p.at("part");
    uint64_t sourceId = 0;
    std::string sourceName;
    for (const ui::PartSlot& slot : state.parts) {
      if (slot.kind != ui::DocKind::Part)
        continue;
      const bool match = pj.is_string() ? slot.name == pj.get<std::string>()
                                        : slot.id == pj.get<uint64_t>();
      if (match) {
        sourceId = slot.id;
        sourceName = slot.name;
      }
    }
    if (sourceId == 0)
      throw std::runtime_error("no part " + pj.dump() +
                               " in this project; list_parts shows them");
    const uint64_t id = job->AddPart(sourceId, sourceName);
    job->BumpVersion();
    return json{{"id", id}, {"source", sourceId}, {"name", sourceName}};
  }

  if (m == "remove_job_part") {
    const uint64_t id = p.at("id").get<uint64_t>();
    if (!job->RemovePart(id))
      throw std::runtime_error("no job part with id " + std::to_string(id) +
                               "; list_job_parts shows the ones that exist");
    job->BumpVersion();
    ui::RefreshCamToolpaths(state);
    return json{{"removed", id}};
  }

  if (m == "list_job_parts") {
    json out = json::array();
    for (const cam::JobPart& part : job->GetParts()) {
      json pj = {
          {"id", part.id}, {"source", part.sourceDocId}, {"name", part.name}};
      // A source that is gone is REPORTED, never substituted: the job cannot
      // cut what it cannot find, and saying nothing here is how a job posts
      // an empty file and looks fine doing it.
      pj["missing"] = ui::JobSourceDocument(state, part.sourceDocId) == nullptr;
      json places = json::array();
      for (const cam::Setup& setup : job->GetSetups())
        for (const cam::Placement& pl : setup.placements)
          if (pl.jobPartId == part.id) {
            const glm::dquat f = cam::FixtureFrame(setup, part.id);
            places.push_back(json{{"setup", setup.id},
                                  {"id", pl.id},
                                  {"pos", {pl.pos.x, pl.pos.y}},
                                  {"rot_z", pl.rotZdeg},
                                  {"enabled", pl.enabled},
                                  {"frame", {f.x, f.y, f.z, f.w}},
                                  {"clamped_per_part", pl.frame.has_value()}});
          }
      pj["placements"] = std::move(places);
      out.push_back(std::move(pj));
    }
    return json{{"parts", std::move(out)}};
  }

  if (m == "check_layout") {
    json out = json::array();
    for (cam::Setup& setup : job->GetSetups()) {
      if (p.contains("setup") && setup.id != p.at("setup").get<uint64_t>())
        continue;
      const std::vector<Body>& bodies = ui::JobBodies(state);
      json fits = json::array();
      for (const cam::PlacementFit& f : cam::CheckPlacements(setup, bodies)) {
        json fj = {{"placement", f.placementId},
                   {"part", f.jobPartId},
                   {"fit", f.fit == cam::Fit::Inside ? "inside"
                           : f.fit == cam::Fit::PartlyOutside
                               ? "partly_outside"
                               : "fully_outside"}};
        if (!f.overlaps.empty())
          fj["overlaps"] = f.overlaps;
        fits.push_back(std::move(fj));
      }
      json sj = {{"setup", setup.id},
                 {"name", setup.name},
                 {"placements", std::move(fits)}};
      const std::string why = cam::PlacementRefusal(setup, bodies);
      if (!why.empty())
        sj["refusal"] = why;
      out.push_back(std::move(sj));
    }
    return json{{"setups", std::move(out)}};
  }

  if (m == "set_job_placement") {
    cam::Setup& setup = RequireSetup(*job, p.at("setup").get<uint64_t>());
    const uint64_t partId = p.at("part").get<uint64_t>();
    if (job->FindPart(partId) == nullptr)
      throw std::runtime_error("no job part with id " + std::to_string(partId) +
                               "; list_job_parts shows the ones that exist");
    cam::Placement* place = nullptr;
    for (cam::Placement& pl : setup.placements)
      if (pl.jobPartId == partId)
        place = &pl;
    if (place == nullptr) {
      cam::Placement fresh;
      fresh.id = job->AllocateId();
      fresh.jobPartId = partId;
      setup.placements.push_back(fresh);
      place = &setup.placements.back();
    }
    if (p.contains("pos")) {
      const auto v = p.at("pos").get<std::vector<double>>();
      if (v.size() != 2)
        throw std::runtime_error("pos is [x, y] in setup space");
      place->pos = {v[0], v[1]};
    }
    if (p.contains("rot_z"))
      place->rotZdeg = p.at("rot_z").get<double>();
    if (p.contains("enabled"))
      place->enabled = p.at("enabled").get<bool>();
    if (p.contains("lay_face_up")) {
      ElementRef ref;
      ref.featureId = p.at("lay_face_up").value("feature_id", uint64_t{0});
      ref.name = p.at("lay_face_up").value("element", std::string());
      glm::vec3 normal(0.0f);
      if (!cam::FaceNormalOf(ui::JobBodies(state), ref, normal))
        throw std::runtime_error(
            "lay_face_up needs a FLAT face on this part; list_elements shows "
            "the stable names");
      place->frame = cam::SolveFaceUp(glm::dvec3(normal),
                                      cam::FixtureFrame(setup, partId));
    }
    job->BumpVersion();
    (void)cam::RecomputeOrigin(setup, ui::JobBodies(state));
    ui::RefreshCamToolpaths(state);
    json out = {
        {"setup", setup.id},       {"part", partId},
        {"id", place->id},         {"pos", {place->pos.x, place->pos.y}},
        {"rot_z", place->rotZdeg}, {"enabled", place->enabled}};
    // How this part ends up clamped, and whether that is its OWN answer or
    // the setup's. Reported because `lay_face_up` is the one field here whose
    // effect cannot be read back off the numbers you sent.
    const glm::dquat f = cam::FixtureFrame(setup, partId);
    out["frame"] = {f.x, f.y, f.z, f.w};
    out["clamped_per_part"] = place->frame.has_value();
    return out;
  }

  if (m == "add_cam_setup") {
    // The machine is resolved BEFORE the setup exists: a refused name used to
    // leave a half-made setup behind the error, which the panel then showed
    // as "no machine chosen" while the caller believed nothing had happened.
    const cam::Machine* named = nullptr;
    if (p.contains("machine")) {
      const std::string want = p.at("machine").get<std::string>();
      named = cam::FindMachine(state.machines, want);
      if (named == nullptr)
        RefuseUnknownMachine(state, want);
    }
    cam::Setup& setup = job->AddSetup(p.value("name", std::string()));
    (void)cam::RecomputeOrigin(setup, ui::JobBodies(state));
    // Safe Z clear of the stock, exactly as the panel does it. Without this
    // an RPC-built job kept the struct's bare 5 mm, which sits INSIDE any
    // stock taller than that: the first dogfooding round over the RPC
    // produced 68 rapids through material on an 18 mm plate. The GUI got
    // this fix in the G-round; this door did not.
    setup.safeZ = cam::DeriveSafeZ(setup, ui::JobBodies(state));
    if (named != nullptr)
      ui::ApplyMachine(setup, *named);
    if (p.contains("post")) {
      const std::string post = p.at("post").get<std::string>();
      if (!cam::PostExists(post))
        throw std::runtime_error("unknown post '" + post + "'");
      setup.machine.post = post;
    }
    if (p.contains("safe_z"))
      setup.safeZ = p.at("safe_z").get<double>();
    return json{{"id", setup.id},
                {"name", setup.name},
                {"post", setup.machine.post},
                {"machine", DocumentIO::MachineToJson(setup.machine)},
                {"machine_match",
                 MatchName(cam::MatchMachine(state.machines, setup.machine))},
                {"safe_z", setup.safeZ}};
  }

  if (m == "list_cam_setups") {
    json out = json::array();
    for (const cam::Setup& setup : job->GetSetups()) {
      json sj = {{"id", setup.id},
                 {"name", setup.name},
                 {"post", setup.machine.post},
                 {"safe_z", setup.safeZ},
                 {"rotation_a", setup.rotationA},
                 {"has_rotary_axis", setup.machine.hasRotaryAxis},
                 // How the part is CLAMPED, as [x, y, z, w]. Identity = as
                 // modelled. A script that reads a toolpath without this
                 // cannot tell which way up the part was cut.
                 {"frame",
                  {setup.frame.x, setup.frame.y, setup.frame.z, setup.frame.w}},
                 // Z zero, and the face it was taken from. The panel shows
                 // both; a script could not see either.
                 {"origin", {setup.origin.x, setup.origin.y, setup.origin.z}},
                 {"origin_at",
                  setup.originAt == cam::OriginAt::StockCentre ? "stock_centre"
                  : setup.originAt == cam::OriginAt::ModelMin  ? "model_min"
                  : setup.originAt == cam::OriginAt::StockMax  ? "stock_max"
                                                               : "stock_min"},
                 {"reach_offset", {setup.reachOffset.x, setup.reachOffset.y}},
                 {"fit_arcs", setup.machine.fitArcs},
                 {"machine", DocumentIO::MachineToJson(setup.machine)},
                 // How the embedded copy stands against the store, by name:
                 // unnamed | global | differs | missing.
                 {"machine_match",
                  MatchName(cam::MatchMachine(state.machines, setup.machine))},
                 {"split_output", setup.splitOutput},
                 {"order", setup.order == cam::OrderOutputBy::Tool ? "tool"
                           : setup.order == cam::OrderOutputBy::Part
                               ? "part"
                               : "authored"}};
      if (setup.topFace.IsSet())
        sj["top_face"] = {{"feature_id", setup.topFace.ref.featureId},
                          {"element", setup.topFace.ref.name},
                          {"part", setup.topFace.jobPartId}};
      sj["tools"] = json::array();
      for (const cam::Tool& t : setup.tools)
        sj["tools"].push_back(ToolJson(t));
      sj["operations"] = json::array();
      for (const cam::Operation& op : setup.operations)
        sj["operations"].push_back(OpJson(op));
      glm::dvec3 lo(0.0), hi(0.0);
      if (cam::StockBounds(setup, ui::JobBodies(state), lo, hi))
        sj["stock"] = {{"min", {lo.x, lo.y, lo.z}},
                       {"max", {hi.x, hi.y, hi.z}}};
      out.push_back(std::move(sj));
    }
    return json{{"setups", out}};
  }

  if (m == "set_cam_setup") {
    cam::Setup& setup = RequireSetup(*job, p.at("id").get<uint64_t>());
    if (p.contains("name"))
      setup.name = p.at("name").get<std::string>();
    // The machine first, so post/fit_arcs/has_rotary_axis in the same call
    // are edits ON TOP of it -- the job's copy then differs from the named
    // one, and list_cam_setups says so.
    if (p.contains("machine"))
      ApplyNamedMachine(state, setup, p.at("machine").get<std::string>());
    if (p.contains("post")) {
      const std::string post = p.at("post").get<std::string>();
      if (!cam::PostExists(post))
        throw std::runtime_error("unknown post '" + post + "'");
      setup.machine.post = post;
    }
    if (p.contains("safe_z"))
      setup.safeZ = p.at("safe_z").get<double>();
    if (p.contains("rotation_a"))
      setup.rotationA = p.at("rotation_a").get<double>();
    if (p.contains("has_rotary_axis"))
      setup.machine.hasRotaryAxis = p.at("has_rotary_axis").get<bool>();
    if (p.contains("origin_at")) {
      const std::string at = p.at("origin_at").get<std::string>();
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
            "unknown origin_at '" + at +
            "'; accepted: stock_min, stock_centre, model_min, stock_max");
    }
    // Where the bed sits on the sheet. "auto" works it out from the
    // toolpaths, which is the only way anyone would want to supply it.
    if (p.contains("reach_offset")) {
      const json& r = p.at("reach_offset");
      if (r.is_string() && r.get<std::string>() == "auto") {
        const cam::DerivedReach d =
            cam::DeriveReachOffset(setup, ui::JobBodies(state));
        if (!d.ok)
          throw std::runtime_error(d.why);
        setup.reachOffset = d.offset;
      } else if (r.is_array() && r.size() == 2) {
        setup.reachOffset = glm::dvec2(r[0].get<double>(), r[1].get<double>());
      } else {
        throw std::runtime_error(
            "reach_offset is [x, y] in mm from the stock's near corner, or "
            "\"auto\" to put the window where the cutting is");
      }
    }
    if (p.contains("stock")) {
      const json& sj = p.at("stock");
      if (sj.contains("mode")) {
        const std::string mode = sj.at("mode").get<std::string>();
        if (mode == "relative")
          setup.stock.mode = cam::StockMode::Relative;
        else if (mode == "fixed")
          setup.stock.mode = cam::StockMode::Fixed;
        else
          throw std::runtime_error("unknown stock mode '" + mode +
                                   "'; accepted: relative, fixed");
      }
      const auto vec3 = [&](const char* key, glm::dvec3& into) {
        if (sj.contains(key)) {
          const json& v = sj.at(key);
          if (!v.is_array() || v.size() != 3)
            throw std::runtime_error(std::string("stock ") + key +
                                     " must be [x, y, z]");
          into = {v[0].get<double>(), v[1].get<double>(), v[2].get<double>()};
        }
      };
      vec3("size", setup.stock.size);
      vec3("place", setup.stock.place);
      vec3("margin", setup.stock.margin);
      if (sj.contains("bottom_margin"))
        setup.stock.bottomMargin = sj.at("bottom_margin").get<double>();
    }
    // Clamp the part with this face up — the scripted twin of the panel's
    // "Lay a face up". Given as a face, not as a quaternion, because that is
    // the thing a caller actually knows; the frame is derived from it, and
    // from the frame in hand, so repeating the call is idempotent.
    if (p.contains("lay_face_up")) {
      const json& fj = p.at("lay_face_up");
      ElementRef ref;
      ref.featureId = fj.at("feature_id").get<uint64_t>();
      ref.name = fj.at("element").get<std::string>();
      glm::vec3 centre(0.0f), normal(0.0f), uAxis(0.0f);
      bool found = false;
      for (const Body& body : ui::JobBodies(state)) {
        if (body.createdBy != ref.featureId)
          continue;
        const std::optional<int> idx = body.elementMap.Resolve(ref.name);
        found = idx && body.shape.FacePlane(*idx, centre, normal, uAxis);
        break;
      }
      if (!found)
        throw std::runtime_error(
            "no planar face '" + ref.name + "' on feature " +
            std::to_string(ref.featureId) + " — lay_face_up needs a flat face");
      setup.frame = cam::SolveFaceUp(glm::dvec3(normal), setup.frame);
      setup.safeZ = cam::DeriveSafeZ(setup, ui::JobBodies(state));
    }
    // The other side of the part, derived from this frame rather than typed.
    if (p.value("flip", false)) {
      setup.frame = cam::FlipFrame(setup.frame);
      setup.safeZ = cam::DeriveSafeZ(setup, ui::JobBodies(state));
    }
    // Anything above can have moved the stock box, and the work origin is
    // derived from it — as is every coordinate the post writes.
    (void)cam::RecomputeOrigin(setup, ui::JobBodies(state));
    if (p.contains("fit_arcs"))
      setup.machine.fitArcs = p.at("fit_arcs").get<bool>();
    if (p.contains("split_output"))
      setup.splitOutput = p.at("split_output").get<bool>();
    if (p.contains("order")) {
      // Refused, never substituted: a job that posts in a different order
      // than it says is a broken cutter, and every plausible substitute here
      // produces a file that looks right.
      const std::string o = p.at("order").get<std::string>();
      if (o == "authored")
        setup.order = cam::OrderOutputBy::Authored;
      else if (o == "tool")
        setup.order = cam::OrderOutputBy::Tool;
      else if (o == "part")
        setup.order = cam::OrderOutputBy::Part;
      else
        throw std::runtime_error("unknown order '" + o +
                                 "'; use authored, tool or part");
    }
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
      if (tj.contains("tool_type"))
        tool->type = ToolTypeFrom(tj.at("tool_type").get<std::string>());
      tool->tipAngleDeg = tj.value("tip_angle", tool->tipAngleDeg);
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
    if (!job->RemoveSetup(id))
      throw std::runtime_error("no CAM setup with id " + std::to_string(id));
    return json::object();
  }

  if (m == "add_cam_op" || m == "cam_derive_op") {
    cam::Setup& setup = RequireSetup(*job, p.at("setup").get<uint64_t>());
    cam::Operation op;
    op.toolNumber = setup.tools.empty() ? 1 : setup.tools.front().number;
    if (p.contains("tool"))
      op.toolNumber = p.at("tool").get<int>();
    // Derive when no depth was typed. This is the door the GUI's verbs go
    // through (cam::DeriveOperation), and until now it was GUI-only: a script
    // had to supply top_z/bottom_z by hand, which is exactly the failure mode
    // the panel stopped allowing. An agent should not have a worse interface
    // than a person.
    const bool derive =
        m == "cam_derive_op" || !p.contains("top_z") || !p.contains("bottom_z");
    if (derive) {
      const cam::OpType type =
          TypeFrom(p.value("type", std::string("profile")));
      ElementRef target;
      const json& tj = p.contains("target") ? p.at("target")
                       : p.contains("targets") && !p.at("targets").empty()
                           ? p.at("targets").at(0)
                           : json::object();
      target.featureId = tj.value("feature_id", uint64_t{0});
      target.name = tj.value("element", std::string());
      // A targetless derive is a real request -- "face the stock", "cut the
      // outline" -- and it is refused further down, in its own words. Only a
      // target that names something has to name a part.
      std::string why;
      const cam::Target picked =
          ui::TargetFor(state, target, tj.value("part", uint64_t{0}), &why);
      if (target.IsSet() && picked.jobPartId == 0)
        throw std::runtime_error(why);
      const cam::DerivedOp derived = cam::DeriveOperation(
          setup, ui::JobBodies(state), type, picked, op.toolNumber);
      if (!derived.ok)
        throw std::runtime_error(derived.why);
      const int tool = op.toolNumber;
      op = derived.op;
      op.toolNumber = tool;
    }
    // Anything stated explicitly still wins — the derivation is a starting
    // point, not a cage. (To force a strategy on a derived operation, set it
    // afterwards with set_cam_op.)
    ApplyOpFields(state, setup, op, p, /*keepType=*/derive);
    // A clearing operation clears INSIDE its boundary; the struct's Outside
    // default is a profile's notion and means "nothing to cut" here.
    if (!p.contains("side") && !derive &&
        (op.type == cam::OpType::Pocket || op.type == cam::OpType::Adaptive))
      op.side = cam::Side::Inside;
    if (op.name.empty())
      op.name = std::string(TypeName(op.type)) + " " +
                std::to_string(setup.operations.size() + 1);
    if (m == "cam_derive_op")
      return json{{"derived", derive}, {"operation", OpJson(op)}};
    op.id = job->AllocateId();
    setup.operations.push_back(op);
    json out = OpJson(op);
    out["derived"] = derive;
    return out;
  }

  if (m == "set_cam_op") {
    auto [setup, op] = FindOp(*job, p.at("id").get<uint64_t>());
    if (op == nullptr)
      throw std::runtime_error("no CAM operation with that id");
    ApplyOpFields(state, *setup, *op, p);
    return json{{"id", op->id}};
  }

  if (m == "move_cam_op") {
    const uint64_t id = p.at("id").get<uint64_t>();
    const size_t to = p.at("to").get<size_t>();
    for (cam::Setup& setup : job->GetSetups())
      for (const cam::Operation& op : setup.operations)
        if (op.id == id) {
          if (!cam::MoveOperation(setup, id, to))
            throw std::runtime_error("index " + std::to_string(to) +
                                     " is past the end — this " + "setup has " +
                                     std::to_string(setup.operations.size()) +
                                     " operations");
          json order = json::array();
          for (const cam::Operation& o : setup.operations)
            order.push_back(o.id);
          return json{{"order", order}};
        }
    throw std::runtime_error("no CAM operation with that id");
  }

  if (m == "remove_cam_op") {
    const uint64_t id = p.at("id").get<uint64_t>();
    for (cam::Setup& setup : job->GetSetups()) {
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

  if (m == "cam_verify") {
    const cam::Setup& setup = RequireSetup(*job, p.at("setup").get<uint64_t>());
    // Synchronous, like every reader here: the pass in flight is joined and
    // the rest is computed now, through the same cache the panel fills
    // (docs/ui/long-operations.md, "RPC semantics").
    const std::vector<cam::Toolpath> paths =
        ui::GenerateCamToolpathsNow(state, setup);
    std::vector<cam::Toolpath::Relief> reliefs; // corner relief: not gouge
    for (const cam::Toolpath& tp : paths)
      reliefs.insert(reliefs.end(), tp.reliefs.begin(), tp.reliefs.end());
    std::string gcode, error;
    if (!cam::PostProcess(setup.machine.post, setup, paths, PostOptsOf(setup),
                          gcode, &error))
      throw std::runtime_error(error);
    const double res = p.value("resolution", 0.25);
    const cam::Verdict v =
        cam::VerifyGcode(gcode, setup, ui::JobBodies(state), res, reliefs);
    const double tol = res * 1.5; // one cell of quantisation, plus slack
    // The same sentences the panel shows. A verdict that reads as a row of
    // numbers over the RPC and as English in the GUI is two verdicts, and the
    // agent gets the worse one.
    bool breaksAnEdge = false;
    for (const cam::Operation& op : setup.operations)
      breaksAnEdge |= op.enabled && op.type == cam::OpType::EdgeRound;
    json says = json::array();
    for (const cam::VerdictLine& line : cam::Explain(v, tol, 0.0, breaksAnEdge))
      says.push_back(line.text);
    // The machine's lines too -- the parts' reach beyond the travel, the
    // sheet's overhang -- the same helper the panel uses.
    for (const cam::VerdictLine& line :
         cam::MachineLines(setup, ui::JobBodies(state), &paths))
      says.push_back(line.text);
    json biggest = json::array();
    for (const cam::Verdict::Region& r : v.biggest)
      biggest.push_back(json{{"at", {r.at.x, r.at.y}},
                             {"area_mm2", r.area},
                             {"depth_mm", r.depth}});
    return json{{"max_gouge_mm", v.maxGouge},
                {"max_leftover_mm", v.maxLeftover},
                // Past the bottom of the stock: the spoilboard, not the part.
                // Separate from the gouge because every through cut does it
                // on purpose, and conflating them called correct G-code scrap.
                {"below_stock_mm", v.belowStock},
                {"below_stock_at", {v.belowStockAt.x, v.belowStockAt.y}},
                {"gouge_at", {v.gougeAt.x, v.gougeAt.y}},
                {"leftover_at", {v.leftoverAt.x, v.leftoverAt.y}},
                // How MUCH is left over, and in how many places: corner radii
                // a cutter cannot reach are several tiny regions, a forgotten
                // operation is one big one, and the deepest-point number
                // alone reads the same for both.
                {"leftover_area_mm2", v.leftoverArea},
                {"leftover_regions", v.leftoverRegions},
                {"leftover_biggest", biggest},
                {"rapids_through_stock", v.rapidsThroughStock},
                {"first_bad_line", v.firstBadLine},
                {"resolution_mm", res},
                {"cells", v.cells},
                {"says", says},
                // Two questions. `ok` is the one that is never in doubt:
                // nothing here scraps the part or breaks the tool. `complete`
                // is softer — a height map cannot watch a slug fall out of a
                // through cut or know that tabs were asked for, so a correct
                // job routinely reports material left over.
                {"ok", v.Safe(tol)},
                {"complete", v.Complete(tol)}};
  }
  if (m == "cam_simulate") {
    if (p.value("close", false)) {
      ui::EndCamPlayback(state);
      return json{{"open", false}};
    }
    const cam::Setup& setup =
        RequireSetup(*job, p.contains("setup") ? p.at("setup").get<uint64_t>()
                                               : state.camActiveSetup);
    if (p.contains("resolution"))
      state.camSimRes = std::clamp(p.at("resolution").get<double>(), 0.05, 5.0);
    // A setup change, a finer grid, or a job that was never generated all
    // mean the same thing: whatever was playing is not this. Rearm rather
    // than scrub a file that no longer exists.
    if (state.camActiveSetup != setup.id || p.contains("resolution")) {
      ui::EndCamPlayback(state);
      state.camActiveSetup = setup.id;
    }
    if (!state.camPlaybackOpen) {
      // Complete toolpaths or none: a pass still cutting would make the
      // playback a picture of the previous file.
      ui::SettleCamToolpaths(state);
      if (!ui::BeginCamPlayback(state))
        throw std::runtime_error(state.camPlaybackRefusal);
    }

    const cam::Timeline& tl = state.camTimeline;
    if (p.contains("at")) {
      const json& at = p.at("at");
      if (at.is_string()) {
        const std::string where = at.get<std::string>();
        if (where == "end")
          ui::SeekCamPlayback(state, tl.End());
        else if (where == "start")
          ui::SeekCamPlayback(state, {0, 0.0});
        else
          throw std::runtime_error("at must be a fraction 0..1, \"start\" or "
                                   "\"end\"; got '" +
                                   where + "'");
      } else {
        const double f = std::clamp(at.get<double>(), 0.0, 1.0);
        ui::SeekCamPlayback(state, tl.CursorAtDistance(f * tl.totalDistance));
      }
    }
    if (p.contains("play"))
      state.camPlaying = p.at("play").get<bool>();

    const cam::GMove* move = tl.MoveAt(state.camCursor);
    const glm::dvec3 pos = tl.PositionAt(state.camCursor);
    json out{{"open", true},
             {"playing", state.camPlaying},
             {"at", tl.totalDistance > 1e-9
                        ? tl.DistanceAt(state.camCursor) / tl.totalDistance
                        : 0.0},
             {"distance_mm", tl.DistanceAt(state.camCursor)},
             {"total_mm", tl.totalDistance},
             {"cutting_minutes", tl.MinutesAt(state.camCursor)},
             {"removed_mm3", state.camStockSim.RemovedVolume()},
             {"resolution_mm", state.camSimRes},
             {"segments", tl.Segments()},
             {"position", {pos.x, pos.y, pos.z}}};
    if (move != nullptr) {
      out["operation"] = move->op;
      out["tool"] = move->tool;
      out["line"] = move->line;
      out["rapid"] = move->rapid;
      if (move->feed > 1e-9)
        out["feed_mm_min"] = move->feed;
      for (const cam::Operation& op : setup.operations)
        if (op.id == move->op)
          out["operation_name"] = op.name;
    }
    return out;
  }

  if (m == "cam_toolpath") {
    // The asynchronous form, in its own branch so `async` is cam_toolpath's
    // key and not cam_post's (rpc_params_gate). Without it, fall through to
    // the shared synchronous handler below.
    if (p.value("async", false)) {
      // Defer the adaptive passes to the worker and answer with what is
      // owed; `background_jobs` shows them landing. A script that reads
      // `pending: 0` here has the same list a synchronous call returns.
      const cam::Setup& setup =
          RequireSetup(*job, p.at("setup").get<uint64_t>());
      if (state.camActiveSetup != setup.id)
        state.camActiveSetup = setup.id;
      ui::RefreshCamToolpaths(state);
      size_t pending = 0;
      for (const cam::Toolpath& tp : state.camToolpaths)
        pending += tp.pending ? 1 : 0;
      return json{{"pending", pending},
                  {"queued_passes", state.camAdaptiveQueue.size() +
                                        (state.camAdaptiveInFlight ? 1 : 0)},
                  {"job", "cam"}};
    }
  }

  if (m == "cam_toolpath" || m == "cam_post") {
    const cam::Setup& setup = RequireSetup(*job, p.at("setup").get<uint64_t>());
    // Synchronous, the default: a script that calls cam_toolpath then
    // cam_post gets the same file it always did. The pass in flight (if the
    // panel started one) is joined rather than recomputed.
    const std::vector<cam::Toolpath> paths =
        ui::GenerateCamToolpathsNow(state, setup);

    // An explicit top_z below the stock top means every entry RAPID passes
    // through material — the verifier catches it later as "rapid through
    // material at line N", which names neither the op nor the height. Say
    // it here, where the fix is one field on one op. (The derivation uses
    // the stock top; only typed depths can get this wrong.)
    json warnings = json::array();
    glm::dvec3 stockLo(0.0), stockHi(0.0);
    if (cam::StockBounds(setup, ui::JobBodies(state), stockLo, stockHi)) {
      const auto zfmt = [](double v) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.4g", v);
        return std::string(buf);
      };
      for (const cam::Operation& op : setup.operations) {
        if (op.enabled && op.topZ < stockHi.z - 1e-6)
          warnings.push_back(
              "op " + std::to_string(op.id) + " '" + op.name + "': top_z " +
              zfmt(op.topZ) + " is below the stock top (Z" + zfmt(stockHi.z) +
              ") — entry rapids will pass through stock; raise top_z or "
              "leave it out to derive");
      }
    }

    json ops = json::array();
    double totalCut = 0.0;
    double totalClipped = 0.0;
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
      // Cut by a different strategy than the type names — adaptive falling
      // back to concentric clearing. Reported, because feeds are chosen for a
      // strategy.
      if (!tp.strategy.empty())
        oj["strategy"] = tp.strategy;
      // What this operation lost, and why. Both used to be visible only in
      // the panel's verdict or in cam_verify — which a caller has to know to
      // poll for — while the mutating call that made the file said nothing.
      if (!tp.skipped.empty()) {
        oj["skipped"] = tp.skipped;
        for (const std::string& why : tp.skipped)
          warnings.push_back("op " + std::to_string(tp.operationId) + " '" +
                             OpName(setup, tp.operationId) + "': " + why);
      }
      if (tp.clippedMm > 0.0) {
        oj["clipped_mm"] = tp.clippedMm;
        totalClipped += tp.clippedMm;
      }
      totalCut += tp.CutLength();
      lowest = std::min(lowest, tp.MinZ());
      ops.push_back(std::move(oj));
    }
    // The machine's own lines of the verdict, through the SAME helper the
    // panel uses (cam::MachineLines) so the two cannot disagree: what the
    // spindle cannot reach, and how much cutting the clip left out.
    for (const cam::VerdictLine& line :
         cam::MachineLines(setup, ui::JobBodies(state), &paths))
      warnings.push_back(line.text);
    // Does this cutter, at these numbers, actually cut? The app has the
    // diameter, the flutes, the feed, the spindle and the body's material,
    // and used to say nothing while a job posted at a third of the chip load
    // it needed.
    for (const std::string& w : cam::FeedWarnings(setup, ui::JobBodies(state)))
      warnings.push_back(w);

    // (Braceless on purpose: rpc_gate_smoke's handler extractor keys on
    // `if (m == "...") {`, and a braced inner dispatch would shadow this
    // handler's body with just the return block.)
    json toolpathOut{{"operations", ops},
                     {"cut_length_mm", totalCut},
                     {"min_z", lowest},
                     {"failed", failed}};
    if (totalClipped > 0.0)
      toolpathOut["clipped_mm"] = totalClipped;
    if (!warnings.empty())
      toolpathOut["warnings"] = warnings;
    if (m == "cam_toolpath")
      return toolpathOut;

    const std::string post = p.value("post", setup.machine.post);
    std::string gcode, error;
    const std::string path = p.at("path").get<std::string>();
    json files = json::array();
    if (setup.splitOutput) {
      // One file per tool (R2): `path` is the base, and every file written
      // is named in the answer -- a script that reads only `path` would
      // otherwise send the machine one tool's worth of the job.
      std::vector<cam::PostFile> split;
      if (!cam::PostProcessSplit(post, setup, paths, PostOptsOf(setup), split,
                                 &error))
        throw std::runtime_error(error);
      const std::filesystem::path base(path);
      for (const cam::PostFile& f : split) {
        const std::string one =
            (base.parent_path() /
             (base.stem().string() + " - T" + std::to_string(f.tool) +
              base.extension().string()))
                .string();
        if (!DocumentIO::WriteTextAtomic(one, f.gcode, &error))
          throw std::runtime_error("could not write " + one + ": " + error);
        files.push_back({{"tool", f.tool}, {"path", one}});
        gcode += f.gcode;
      }
    } else {
      if (!cam::PostProcess(post, setup, paths, PostOptsOf(setup), gcode,
                            &error))
        throw std::runtime_error(error);
      if (!DocumentIO::WriteTextAtomic(path, gcode, &error))
        throw std::runtime_error("could not write " + path + ": " + error);
    }
    size_t lines = 0;
    for (const char c : gcode)
      if (c == '\n')
        lines++;
    json postOut{{"path", path},
                 {"files", files},
                 {"post", post},
                 {"lines", lines},
                 {"operations", ops},
                 {"cut_length_mm", totalCut},
                 {"min_z", lowest},
                 // Surfaced, not buried: a job posted with a failed op is a
                 // part that comes off the machine unfinished.
                 {"failed", failed}};
    if (totalClipped > 0.0)
      postOut["clipped_mm"] = totalClipped;
    // cam_post never carried these at all, so the one call that WRITES the
    // file was the one that said least about it.
    if (!warnings.empty())
      postOut["warnings"] = warnings;
    return postOut;
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
