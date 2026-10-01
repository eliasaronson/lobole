#include "remote/RemoteCam.h"

#include "core/cam/CamEngrave.h"
#include "core/cam/CamNames.h"
#include "core/cam/CamOpFields.h"
#include "core/cam/CamOtherSetups.h"
#include "ui/ViewActions.h"

#ifdef HAVE_CAM

#include <Standard_Failure.hxx>

#include "core/Configuration.h"
#include "core/Document.h"
#include "core/cam/CamFeeds.h"
#include "core/cam/CamMachine.h"
#include "core/cam/CamMount.h"
#include "core/cam/CamPost.h"
#include "core/cam/CamSim.h"
#include "core/cam/CamToolpath.h"
#include "core/cam/CamVerify.h"
#include "core/io/Serialization.h"
#include "ui/AppState.h"
#include "ui/AssemblyActions.h" // PartDocById
#include "ui/BackgroundJob.h"
#include "ui/BackgroundJobs.h"
#include "ui/CamJobs.h"
#include "ui/CamPanel.h" // RefreshCamToolpaths
#include "ui/CamPlayback.h"
#include "ui/CamTool.h"        // CamFixturesChanged
#include "ui/DocumentSlots.h"  // JobUndoGuard
#include "ui/DrawingActions.h" // ConfiguredPartDocument
#include "ui/JobActions.h"
#include "ui/MachineActions.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <optional>
#include <thread>

namespace remote {

using nlohmann::json;

namespace {

// A configuration of `sourceId`, a job part's source, by name or id; null,
// 0 or "" is its active one. Refused naming the ones it has, or saying
// which feature fails in it.
uint64_t SourceConfiguration(ui::AppState& state, uint64_t sourceId,
                             const std::string& label,
                             const nlohmann::json& c) {
  if (c.is_null())
    return 0;
  const Document* part = ui::PartDocById(state, sourceId);
  uint64_t id = 0;
  std::string known;
  if (part != nullptr)
    for (const Configuration& cfg : part->GetConfigurations()) {
      known += (known.empty() ? "" : ", ") + cfg.name;
      if ((c.is_number() && cfg.id == c.get<uint64_t>()) ||
          (c.is_string() && cfg.name == c.get<std::string>()))
        id = cfg.id;
    }
  if (id == 0) {
    if ((c.is_number() && c.get<uint64_t>() == 0) ||
        (c.is_string() && c.get<std::string>().empty()))
      return 0;
    throw std::runtime_error("no configuration " + c.dump() + " in '" + label +
                             "'. Known: " + (known.empty() ? "(none)" : known));
  }
  std::string bad;
  if (ui::ConfiguredPartDocument(state, sourceId, id, &bad) == nullptr)
    throw std::runtime_error("'" + label + "': " + bad);
  return id;
}

constexpr MethodDoc kCamMethods[] = {
    {"list_machines",
     "Your machines, from ~/.config/designer/machines/*.json: each {name, "
     "post, travel?: {min, max}, fit_arcs, has_rotary_axis, rapid_feed, "
     "a_max_deg_min (the A axis's top speed; 0/absent = not stated), "
     "rotary?, holding?, tools}. A setup names one and EMBEDS a copy of it; "
     "set_cam_setup "
     "{machine} puts one on a setup.",
     "", remote::NoFlags,
     "machines[]{name,post,travel?,fit_arcs?,has_rotary_axis?,rapid_feed?,"
     "a_max_deg_min?,"
     "tools?}, "
     "dir"},
    {"save_machine",
     "Write a machine to the store {machine: {name, post?, travel?: {min: "
     "[x,y,z], max: [x,y,z]}, fit_arcs?, has_rotary_axis?, rapid_feed?, "
     "a_max_deg_min?, rotary?: {along: x|y, chuck_end: -|+, word: A|B, "
     "top_moves: +x|-x|+y|-y} (how the rotary is INSTALLED: the machine "
     "axis it lies along, which end the chuck is at, its letter, and which "
     "way the top of the part moves on a positive jog -- stated whole or "
     "not at all; without it a setup held in the rotary does not post), "
     "holding?: {chuck_diameter_mm, grip_mm, tailstock: bool, "
     "centre_length_mm, centre_diameter_mm, tailstock_body_diameter_mm, "
     "clearance_mm} (the chuck and tailstock every rotary toolpath is kept "
     "out of; a size left out is a conservative default the verdict names), "
     "tools?: [...]}}, replacing one of that name. Refused without a name, "
     "with an unknown post (naming the accepted ones), or with a name that "
     "would share a file with a different name. Open jobs are not changed: a "
     "setup keeps the copy it was set up on, and list_cam_setups reports "
     "`machine_match` so a script can see that it now differs.",
     "machine"},
    {"delete_machine", "Remove a machine from the store {name}.", "name"},
    {"machine_part",
     "Make a JOB for a part and switch to it {part?: id or name (default: the "
     "open part), name?, configuration?: which of the part's configurations "
     "it cuts (v135; default its active one)}. A job is a document of its own "
     "since v73 -- it "
     "REFERENCES parts and says how they are cut -- so this is the one step "
     "from a part you have open to a tab you can add setups on. Returns the "
     "job's tab id and the job part it holds. Every verb below then takes "
     "`job` (name or id) to say WHICH job it acts on; without one they act on "
     "whichever job tab is in front, which is fine interactively and is a "
     "trap in a script that switches tabs between calls.",
     "configuration, name, part"},
    {"add_job_part",
     "Add a part to the open job {part: id or name, configuration?: name or "
     "id} -- another thing this job cuts, in that configuration of it (v135; "
     "default its active one). Returns the job part's id, which is what an "
     "operation target and a placement name. A job is what makes four "
     "brackets out of one sheet expressible; before v73 a setup could only "
     "ever be about one part.",
     "configuration, job, part"},
    {"set_job_part",
     "Change which configuration of its source a job part is cut in {id, "
     "configuration: name or id; null, 0 or \"\" = the source's active one}. "
     "Refused for a configuration the part does not have (naming the ones it "
     "has) or one it does not build in. One undo step; every setup's origin, "
     "safe Z and targeted operations' depths follow the new geometry, as a "
     "re-clamp does, and `moved` says how many operations' depths did -- the "
     "same action as the CAM panel's Parts row > Configuration.",
     "configuration, id, job", remote::NoFlags,
     "id, configuration, configuration_name?, moved"},
    {"remove_job_part",
     "Drop a part from the job {id}. Every placement and every operation "
     "target that named it goes with it -- a target on a part the job no "
     "longer holds would resolve against whatever body answered to the same "
     "element name, which is the wrong part, cut for real.",
     "id, job"},
    {"list_job_parts",
     "The parts this job cuts {job?}: per part `id`, `source` (the document "
     "id), `name`, `configuration` (+ `configuration_name`), `missing` (its "
     "source is gone or does not build in that configuration -- reported "
     "with `why`, never substituted) and `placements`, each {setup, id, pos, "
     "rot_z, enabled, frame, clamped_per_part}. The id is what an operation "
     "target and a placement name.",
     "job", remote::NoFlags,
     "parts[]{id,source,name,configuration,configuration_name?,missing,why?,"
     "placements}"},
    {"check_layout",
     "How the parts are arranged in the stock {job?, setup?}. Per placement: "
     "`fit` -- `inside`, `partly_outside` (hanging over an edge) or "
     "`fully_outside` (not on it at all) -- `overlaps` (the placements it runs "
     "into) and `box` (min/max in SETUP space, which is where the material "
     "is once the part is laid the way it is clamped: what a nesting caller "
     "wants). Per setup: `refusal` when the arrangement cannot be cut at all, "
     "and `warnings` for what is merely close -- two parts nearer than the "
     "widest cutter is a gouge waiting to happen and is not a refusal, "
     "because only the tools that go between them care. Computed when asked, "
     "not while you drag: a sheet is laid out THROUGH states like these. The "
     "refusal lands again at cam_toolpath and cam_post, where the mistake "
     "becomes G-code.",
     "job, setup", remote::NoFlags,
     "setups[]{setup,name,placements,refusal?,warnings?}, "
     "receipt{schema,standing,summary,claims}"},
    {"set_job_placement",
     "Where a part sits in the stock {setup, part, pos?: [x,y], rot_z?, "
     "enabled?, lay_face_up?: {feature_id, element}}. Position and spin are "
     "IN-PLANE: anything out of plane is fixturing, and two ways to tilt a "
     "part is a mistake this project already paid for once "
     "(docs/cam/setup.md#parts-and-placements). `lay_face_up` is the "
     "out-of-plane half and "
     "clamps THIS part with that face at the spindle -- so one sheet can hold "
     "a baffle on its face beside a rail on its edge. It says nothing about "
     "the machine: no A word is posted.",
     "enabled, job, lay_face_up, part, pos, rot_z, setup"},
    {"add_job_fixture",
     "Put a FIXTURE in a setup {setup, part: id or name | box: [x,y,z] mm, "
     "side?: -x|+x|-y|+y|centre|top, pos?: [x,y,z] mm in setup space, "
     "rot_z?: degrees, name?} -- what holds the stock: any part (a vise, a "
     "shaped clamp), read in its own coordinates as bed coordinates (Z up), "
     "or a keep-out BOX with no part at all (a clamp, this big, here; its "
     "corner at its origin). Exactly one of part and box. It is not cut; it "
     "is where the tool must never go, and cam_verify reports every move "
     "into it. Without pos it is placed from the stock and its own size: a "
     "side puts it on the bed against that face, `centre` centres it on the "
     "stock (a vise goes round it), `top` lays it on the stock straddling "
     "the -X edge (a toe clamp's arm). With neither, it is centred when it "
     "is wider than the stock and otherwise takes the next free side. Safe "
     "Z is re-derived to clear it. A part the job CUTS is refused.",
     "box, job, name, part, pos, rot_z, setup, side", remote::NoFlags,
     "id, part|box, name, pos, rot_z, enabled, setup, safe_z"},
    {"set_job_fixture",
     "Move or change a fixture {setup, fixture: id, pos?: [x,y,z] mm in "
     "setup space, side?: -x|+x|-y|+y|centre|top (re-derive against the "
     "stock), rot_z?: degrees, box?: [x,y,z] mm (a keep-out box's size), "
     "enabled?: false = drawn, not checked, name?, seat?: {feature_id, "
     "element, edge: -x|+x|-y|+y, along_mm, over_mm, spin_deg?} | null}. pos "
     "and side both place it, so passing both is refused. `seat` RESTS it on "
     "the stock instead: that face of the fixture (it must face down) on the "
     "stock's top, reaching over_mm in past `edge`, its middle along_mm from "
     "that edge's low (-X or -Y) end, turned spin_deg from pointing straight "
     "in -- all in mm and degrees, measured from the stock, and re-solved "
     "whenever the stock changes. null stands it back on its own at the pose "
     "it has. A seat with pos, side or rot_z is refused. Safe Z is "
     "re-derived.",
     "box, enabled, fixture, job, name, pos, rot_z, seat, setup, side",
     remote::NoFlags, "id, part|box, name, pos, rot_z, enabled, setup, safe_z"},
    {"remove_job_fixture",
     "Take a fixture off a setup {setup, fixture: id}. Safe Z is re-derived.",
     "fixture, job, setup"},
    {"add_cam_setup",
     "Add a milling setup {name?, post?: mach3|linuxcnc|grbl, safe_z?}; "
     "returns "
     "its id and the safe_z derived for it. A setup arrives with one 6 mm "
     "tool, because a setup with no tools can produce nothing, and with a "
     "rapid height clear of the stock, because the default 5 mm is inside "
     "most of them. `machine` puts one of your machines (list_machines) on "
     "it -- post, arcs, rotary axis, work area, rapid -- and the setup "
     "EMBEDS a copy, so the job file opens anywhere.",
     "job, machine, name, post, safe_z"},
    {"list_cam_setups",
     "Every setup: id, name, post, safe_z, origin (Z zero), top_face (the "
     "face it was picked from), origin_at (the corner it is measured FROM), "
     "frame (how the part is CLAMPED, as "
     "[x,y,z,w]; identity = as modelled), has_rotary_axis, rotation_a, "
     "fit_arcs, machine (the embedded copy) and machine_match "
     "(unnamed|global|differs|missing -- how that copy stands against your "
     "store, by name), tools, "
     "operations, fixtures (what holds the stock: {id, part or box, name, "
     "pos, rot_z, enabled}), and the stock box derived from the model — in "
     "SETUP space, "
     "so a setup that lays the part flat reports the box the machine sees.",
     "job", remote::NoFlags,
     "setups[]{id,name,post,safe_z,rotation_a,has_rotary_axis,frame,origin,"
     "origin_at,reach_offset,fit_arcs,machine,machine_match,split_output,order,"
     "tools,operations,fixtures,top_face?,stock?}"},
    {"set_cam_setup",
     "Change a setup {id, name?, machine?: name, post?, safe_z?, "
     "lay_face_up?: {feature_id, "
     "element, part? (the job part, when several share the feature id)}, "
     "flip?, has_rotary_axis?, rotation_a?, held_in?: "
     "rotary|bed|derived (whether the part is held in the rotary; derived "
     "from a rotary op, an op with index_a_deg or a non-zero rotation_a, so "
     "an indexed setup at A=0 states it), grip_mm? (mm of the stock the "
     "chuck's jaws hold; 0 = the "
     "machine's), tailstock?: bool, chuck?: bool (false: held some other "
     "way), rotary?/holding? (this setup's copy of the machine's -- see "
     "save_machine), fit_arcs?, "
     "origin_at?: stock_min|stock_centre|model_min|stock_max, "
     "reach_offset?: [x,y] | \"auto\", "
     "order?: authored|tool|part, split_output?: bool (one program per "
     "tool, which forces tool order), "
     "stock?: {mode?: relative|fixed, size?: [x,y,z], place?: [x,y,z], "
     "margin?: [x,y,z], bottom_margin?}} or "
     "one of its "
     "tools {id, tool: {number, name?, diameter?, tool_type?: "
     "flat|ball|vbit|drill|thread_mill, tip_angle?, feed_cut?, feed_plunge?, "
     "spindle_rpm?, flutes?, flute_length?, chipload_min?, chipload_max?, "
     "overall_length?, collar_diameter?, shank_diameter?, tip_flat?, "
     "neck_diameter?, neck_length?}} "
     "(a thread_mill is a single-profile head: diameter is its cutting "
     "diameter at the flat, tip_angle its V (60 when not given), tip_flat "
     "the flat's width in mm, neck_diameter and neck_length the neck under "
     "it in mm; each left 0 is derived from the diameter -- a flat of 1/60, "
     "a neck of 0.6 of it, three diameters long -- and its flute_length is "
     "the head's own width, derived and refused if typed) "
     "— `id` and `tool` are SIBLING parameters, not a nested object, and "
     "`number` is required because it says WHICH tool. The tool is "
     "added when the number is new. chipload_min/max are mm per tooth THIS "
     "cutter wants in the material this setup cuts (a vendor's chart), given "
     "as a pair or not at all; without them the chip-load check falls back "
     "to a rule off the diameter, and every finding cites which it used. "
     "surface_parallel and surface_waterline are surface passes, whose Z "
     "varies within a pass: they take no side, no tabs and no climb, and "
     "setting one is refused. A surface_parallel with a stepdown (mm) ROUGHS: "
     "layers from the stock top down to the surface plus stock_to_leave, over "
     "the part's outline and a ring one cutter wide; without one it "
     "finishes. rest_from_tool on a surface_parallel or surface_rotary names "
     "the LARGER cutter that finished before, and the pass then cuts only "
     "where that one could not reach (rest machining). surface_parallel "
     "rasters at "
     "cut_angle_deg (degrees anticlockwise from +X), stepping ACROSS by "
     "stepover (a fraction of the cutter) and reading the surface ALONG by "
     "sample_step_mm; surface_waterline laps at constant Z, stepping DOWN by "
     "z_step_mm, and closer where the surface is shallow. Those are three "
     "different quantities and none of them is "
     "the others. A zero sample_step_mm or z_step_mm derives from the cutter. "
     "surface_rotary is 4-AXIS finishing, by rotary_pattern: parallel "
     "(default) runs passes along the rotary axis (setup-space X) stepping "
     "round it, the part turning BETWEEN passes, and stepover is a fraction "
     "of the cutter converted to millimetres of ARC at the part's widest "
     "point; spiral is CONTINUOUS 4-axis, one helix end to end with A "
     "turning while the tool cuts, and stepover is the spacing between "
     "turns along the surface; helical is continuous too, passes that "
     "FOLLOW a twist -- helices at lead_mm per turn (signed for the hand; "
     "0 derives the part's own twist), stepped round by stepover measured "
     "across them -- and a part with no twist is cut as parallel passes, "
     "said in the toolpath's strategy. A stepdown (mm, radial) cuts it in "
     "layers from the blank inward, "
     "the last one the finish. A turning cut's F is scaled in the post for "
     "how the controller reads a mixed mm/degree move, held to the "
     "machine's axis speeds (rapid_feed for X/Y/Z, a_max_deg_min for A), and "
     "the file says where it was held; between operations whose angles are "
     "half a turn or more apart the post writes G92 A, renaming the angle so "
     "the next op does not unwind it. It needs a machine with "
     "has_rotary_axis. With no target it cuts the whole length; a target "
     "face bounds it ALONG the axis to that face's extent (it still turns "
     "all the way round). "
     "index_a_deg (a list of degrees about the rotary axis) cuts any op but "
     "surface_rotary at EACH of those indices -- four crenels are one op at "
     "[0, 90, 180, 270] -- generated in each angle's frame, joined by a "
     "retract, every move carrying its A; empty cuts at the setup's "
     "rotation_a. It needs a machine with has_rotary_axis. "
     "flute_length is the cutter's USABLE DEPTH in mm (the fluted part, not "
     "the whole tool): an operation cutting deeper than that is cutting with "
     "the shank and driving the holder into the work, and the reach finding "
     "says so. overall_length is mm from the TIP to the underside of the "
     "collet nut and collar_diameter is that nut's width in mm; given as a "
     "pair, they turn on the holder check in cam_verify, which finds the "
     "collar driven into material the cutter never went near -- a clamp, a "
     "boss beside a pocket, a tall neighbour on the same sheet. Without them "
     "no holder is checked, because a limit nobody typed is not a limit. "
     "shank_diameter is how wide the tool is ABOVE its flutes, in mm (a 3 mm "
     "cutter on a 6 mm shank, a V-bit's body); left out, the tool is the "
     "cutter's width all the way up. Wider than the cutter, an operation "
     "deeper than flute_length drives that shank into the wall, so the edit "
     "that would make one -- this tool edit, or an add_cam_op/set_cam_op -- "
     "is REFUSED naming the op; narrower is a necked tool, made to reach "
     "past its flutes. A reach the tool can make with little to spare, or "
     "only by rubbing its shank, is allowed and comes back in `warnings`. "
     "diameter, tip_angle and the feeds are mm, degrees and mm/min. "
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
     "an index without it is REFUSED, because the part would be machined as "
     "if turned while the file never turns it. Without it no file can carry "
     "an A word. "
     "`reach_offset` is WHERE THE BED IS ON THE SHEET, in mm from the "
     "default window: centred on the stock along an axis where the stock "
     "fits the travel, at its near corner along one where it is longer. A "
     "sheet longer than the machine is slid through and clamped where the "
     "work is, and without this the window sits at its corner — an "
     "assumption that decides what gets clipped and that MOVES when you turn "
     "the part round. \"auto\" puts the window on the cutting. origin_at is "
     "WHERE X0 Y0 IS in the posted file — the "
     "corner the "
     "operator touches off on; the post subtracts it from every coordinate, "
     "and Z comes from the picked top face. stock.mode `fixed` makes the "
     "workpiece the sheet you own (size + place, where the part sits in it) "
     "rather than a shrink-wrap of the part; a sheet smaller than the part is "
     "refused, naming the axis and the shortfall.",
     "chuck, fit_arcs, flip, grip_mm, has_rotary_axis, held_in, holding, id, "
     "lay_face_up, machine, name, order, origin_at, post, reach_offset, "
     "rotary, rotation_a, safe_z, split_output, job, stock, tailstock, "
     "tool"},
    {"remove_cam_setup", "Delete a setup {id} and its operations.", "id, job"},
    {"add_cam_op",
     "Add an operation. Every length here is MILLIMETRES — top_z, bottom_z, "
     "stepdown, peck, finish_stock, stock_to_leave and edge_size — "
     "stepover is the ONE exception and is NOT a length: it is a FRACTION of "
     "the tool diameter (0.05-0.9), so 0.45 steps over by 45% of the cutter. "
     "and feeds are mm/min on the TOOL, not here. {setup, type: "
     "profile|pocket|face|drill|adaptive|edge_round|surface_parallel|"
     "surface_waterline|surface_rotary|engrave|thread_mill, "
     "tool?, target? (one pick, as cam_derive_op takes it -- the same as a "
     "one-entry targets; not both), top_z?, bottom_z?, stepdown?, peck? "
     "(drill: retract between "
     "pecks of this depth; derived as the tool's diameter once a hole is "
     "deeper than three), corner_relief?: none|dogbone|tbone (profile and "
     "pocket: clear the inside corners a round cutter leaves, by the tool's "
     "radius -- along the bisector, or along the longer wall), lead_arcs?: "
     "bool (profile: leave the wall on a tangent arc of the tool's radius "
     "instead of stopping against it -- no dwell mark; every depth pass "
     "leaves at the same point, so each arc cuts one stepdown of the waste; "
     "refused inside an opening with no room for it), finish_tool?: number "
     "(rough with `tool` "
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
     "it, and `warnings` when its tool reaches the depth only just or by "
     "rubbing its shank; a depth its tool cannot reach at all -- deeper than "
     "the flutes on a shank wider than the cutter -- is refused (see "
     "set_cam_setup's shank_diameter). LEAVE OUT top_z/bottom_z and they are "
     "derived from what the "
     "operation acts on — the same derivation the GUI's verbs use, so a "
     "script does not type a depth the panel would not let a person type. "
     "Pocket / adaptive: pick the floor -- or the part's OUTSIDE wall to "
     "clear the stock ROUND the part, from the stock's edge to that wall "
     "(down the wall, through the stock's bottom when it reaches the "
     "part's; a vise-held part stops above the jaws with bottom_z). "
     "Profile: pick a wall (an opening cuts inside "
     "it, the perimeter cuts outside), a level FACE with side `inside` to cut "
     "EVERY opening in that face in one operation (`drilled_bores` leaves the "
     "round ones up to that diameter alone, because the drill takes them; a "
     "boss is not an opening; "
     "one narrow slot among five is skipped by name and the rest are cut; and "
     "one operation has one depth, so openings of two depths want a wall pick "
     "each), or nothing at all to cut the part free. Face: pick nothing. "
     "Edge_round: pick the WALL whose top edge to "
     "break — a V-bit chamfers it, a ball rounds it, a flat mill is refused; "
     "edge_size? sets the bevel width / round radius; left out it is the "
     "break the MODEL has on that edge (a chamfer or fillet, from the part's "
     "top -- a V-bit of another angle is refused), or on a sharp edge the "
     "biggest break that tool can make within the wall. A refusal says what "
     "to pick instead. Derivation notes (a drill's point in a flat-bottomed "
     "blind hole) come back in `warnings`. A "
     "derived pocket deep enough to want adaptive clearing COMES BACK as "
     "type adaptive; set_cam_op forces either strategy. Engrave: target a "
     "SKETCH entity, {feature_id: <the sketch>, element: \"g<entity id>\"} "
     "(a line, an arc, a circle, an ellipse, a spline -- open or closed -- "
     "or a text label, every stroke of every glyph), and the tool's "
     "axis follows its curves ON them, from the surface the sketch lies on; "
     "the sketch must lie on the part's face and that face must face the "
     "spindle (refused otherwise, naming the angle; with index_a_deg each "
     "target is cut at the index that turns its plane up). width_mm? is the "
     "groove's width at the surface -- a V-bit's depth IS its width, cut to "
     "where its cone is that wide; a width past the cone is refused -- or "
     "depth_mm? its depth; state one (setting one clears the other). A flat "
     "or ball takes depth_mm only. Left out, a V-bit's width is a quarter of "
     "the cutter (a text label's: its cap height / 8), a flat's or ball's "
     "depth a quarter of the cutter. Strokes are cut nearest first, reversed "
     "where that is shorter, the tool staying down where one stroke ends on "
     "the next. Thread_mill: target the cylindrical face a Thread (or a Hole "
     "with a tapped fit) is on, with a thread_mill tool (the setup's first, "
     "when `tool` is left out). Everything comes from the thread -- size, "
     "pitch, hand, inside or outside, and the class, whose pitch-diameter "
     "band's MIDDLE the last pass aims at (the series' general-purpose class, "
     "said in `warnings`, when the Thread states none). It cuts a helix of "
     "half-turn G2/G3 arcs, Z advancing half a pitch each, a partial last "
     "arc, in `passes` radial passes of equal area (0 derives them: the "
     "first takes at most half the cutter's profile depth), climb by "
     "default, entering and leaving on semicircles (lead_arcs); a through "
     "thread runs a pitch past its last full turn, a blind one stops the "
     "head's tip on the floor. top_z/bottom_z are the thread's and are "
     "refused. Refused: a face with no thread, a cutter that with twice the "
     "thread's depth does not fit the tap-drill bore, a flat wider than the "
     "thread's root (P/8: too fine a thread), a profile shallower than the "
     "thread (the neck would rub), and a machine that posts no arcs.",
     "bottom_z, climb, corner_relief, cut_angle_deg, depth_mm, edge_size, "
     "enabled, finish_stock, finish_tool, index_a_deg, lead_arcs, lead_mm, "
     "name, "
     "peck, rest_from_tool, rotary_pattern, "
     "sample_step_mm, setup, side, stepdown, stepover, job, drilled_bores, "
     "stock_to_leave, tabs, target, targets, tool, top_z, type, use_outline, "
     "passes, width_mm, z_step_mm"},
    {"set_cam_op",
     "Change an operation {id, …the add_cam_op fields, whose lengths — "
     "stepdown, edge_size and the rest — are MILLIMETRES, while stepover is a "
     "FRACTION of the tool diameter}. `tool` "
     "re-points it "
     "at another cutter: a stepdown still at the old tool's derived value "
     "follows the new one (every derived number is a fraction of the "
     "cutter), one you typed is kept, and a tool this setup does not have is "
     "refused. The tool's reach is checked as add_cam_op checks it: refused "
     "when the shank would be driven into a wall, `warnings` otherwise.",
     "bottom_z, climb, corner_relief, cut_angle_deg, depth_mm, edge_size, "
     "enabled, finish_stock, finish_tool, id, index_a_deg, lead_arcs, "
     "lead_mm, name, "
     "peck, rest_from_tool, rotary_pattern, "
     "sample_step_mm, side, stepdown, stepover, job, drilled_bores, "
     "stock_to_leave, tabs, target, targets, tool, top_z, type, use_outline, "
     "passes, width_mm, z_step_mm"},
    {"move_cam_op",
     "Move an operation to a position {id, to} (0-based) and return the "
     "setup's new order. Operation order IS the cutting sequence — the "
     "generator and the post both walk it — so it decides what is roughed "
     "before what is finished, and where the tool changes and their M0 "
     "pauses fall.",
     "id, job, to"},
    {"remove_cam_op", "Delete an operation {id}.", "id, job"},
    {"cam_derive_op",
     "What {setup, type, tool?, target?: {feature_id, element}} would produce "
     "WITHOUT adding it: the derived operation, or why that pick cannot make "
     "one -- including a depth its tool cannot reach (a shank wider than the "
     "cutter below the flutes), with `warnings` when it reaches only just or "
     "by rubbing. The dry run behind add_cam_op's derivation, and its lengths "
     "are "
     "the same MILLIMETRES — stepdown, edge_size and the rest — and stepover "
     "is the same FRACTION of the tool diameter.",
     "bottom_z, climb, corner_relief, cut_angle_deg, depth_mm, edge_size, "
     "enabled, finish_stock, finish_tool, index_a_deg, lead_arcs, lead_mm, "
     "name, "
     "peck, rest_from_tool, rotary_pattern, "
     "sample_step_mm, setup, side, stepdown, stepover, job, drilled_bores, "
     "stock_to_leave, tabs, target, targets, tool, top_z, type, use_outline, "
     "passes, width_mm, z_step_mm"},
    {"cam_toolpath",
     "Generate toolpaths for a setup {setup, async?} without writing a file: "
     "per operation, the move count, cut length (mm, the tool TIP's path: "
     "a move that turns A counts the arc it turns under the tool), lowest "
     "Z, and the "
     "error when it produced nothing. `skipped` names the targets of a "
     "multi-target operation that could not be followed while the others "
     "could, and `clipped_mm` how much cutting fell beyond the machine's "
     "travel and is NOT in the file -- both also as `warnings`, which "
     "carries the work-area verdict too. `holding_clipped_mm` is the cutting "
     "left out beside a rotary setup's chuck and tailstock. "
     "The cheap way to check a job. "
     "Synchronous by default -- an adaptive pocket costs seconds and this "
     "waits for it, but OFF the frame: the passes cut on the `cam` "
     "background job while the app keeps drawing and other connections are "
     "answered, and a later request on the SAME connection waits behind "
     "this one. Cancelling the passes (cancel_background_job cam) refuses "
     "the waiting call. `async: true` hands the slow passes -- adaptive "
     "clearing, surfacing, and the sections a profile follows -- to a "
     "background "
     "job and answers {pending, queued_passes, job: \"cam\"} at once; "
     "watch `background_jobs` until none of kind cam remains, then call "
     "again without `async` for the list.",
     "async, job, path, post, setup"},
    {"cam_verify",
     "Simulate the POSTED G-code for a setup {setup, resolution?: the height "
     "map's cell size in mm, echoed as resolution_mm} and compare "
     "the result with the model: {max_gouge_mm, max_leftover_mm, "
     "leftover_area_mm2, leftover_regions, rapids_through_stock "
     "(with worst_rapid_depth_mm/_at/_line: a graze and a crash are the "
     "same COUNT and different numbers), "
     "rotary (the file's A MOVES, so it was checked against a cylindrical "
     "blank r(x, theta) instead of a height map -- and the second number of "
     "every _at pair is then an ANGLE IN DEGREES, not a Y; a rotary answer "
     "also carries leftover_p95_mm, the 95th-percentile leftover -- the "
     "typical scallop, which the end faces no rotary pass can reach do not "
     "move the way they set max_leftover_mm -- and leftover_surface_max_mm "
     "/ _at / leftover_surface_p99_mm, the same measured OFF the faces "
     "turned toward the axis's ends (steeper than 70 degrees along it), which "
     "is what tells two rotary patterns apart; each leftover_biggest region "
     "then says end_face, span_deg (how far round the axis it reaches; its "
     "angle is the middle of that arc) and ring (all the way round -- a "
     "corner no pass reaches, whose angle is then 0 and means nothing; a "
     "patch at one angle is a pass missing there), and "
     "leftover_end_face_area_mm2 sums the end faces' leftover by cell), "
     "file_errors (with first_file_error: lines the PARSER could not read -- "
     "a NaN coordinate, a feed of zero; anything else in the answer was "
     "measured on a different program, so this counts against ok), "
     "file_warnings (with first_file_warning: what a CONTROLLER may refuse "
     "though the parser survived it, such as a nested comment Mach3 will not "
     "load), "
     "holder_collisions (with worst_holder_depth_mm/_at/_line: the collet "
     "nut driven into material the cutter never touched -- silent unless the "
     "tool states overall_length and collar_diameter), "
     "fixture_collisions (with worst_fixture_depth_mm/_at/_line, "
     "worst_fixture (its name) and worst_fixture_by: cutter|holder -- moves "
     "into a FIXTURE of the setup, see add_job_fixture; fixtures_checked "
     "counts them, and not_checked names any that were not: a fixture whose "
     "part is gone, or every fixture on a rotary setup), "
     "holding_collisions (with worst_holding_depth_mm/_x/_line, "
     "worst_holding: chuck|tailstock, and holding_defaults naming the sizes "
     "the machine did not state -- moves into what holds a rotary setup), "
     "below_stock_mm, tabs, reliefs, grooves, groove_shallow_mm, "
     "narrow_grooves, threads, ok, complete, cuts_nothing, says}. `threads` "
     "judges every milled thread on its own terms, since a height map cannot "
     "see a flank that faces sideways: per thread {op, designation, class, "
     "judged (false, with why, when the file has no helix for it -- which "
     "counts against complete), pitch_mm and pitch_measured_mm (what the "
     "last pass's helix advances a turn), hand and hand_measured, "
     "pitch_diameter_band_mm [min, max] of its class and pitch_diameter_mm "
     "the last pass's tip reached, in_band, passes, span_mm and "
     "full_thread_mm (Z of the thread and of its full-depth part), "
     "short_mm, floor_limited, bore_standing_mm (stock standing where the "
     "head goes when the op starts: the tap drill not cut first -- not ok), "
     "neck_clearance_mm (negative: the neck rubs the crests -- not ok), ok, "
     "good}; the carve inside a thread down to its head's lowest tip is not "
     "gouge. `cuts_nothing`: the "
     "program never touches the stock -- safe, and not a job, so `complete` "
     "is false with it. `grooves` counts the engraved "
     "strokes checked against the groove each declares: a cut inside one is "
     "engraving, a cut below or beside it is gouge; groove_shallow_mm is how "
     "far short of its groove an engraving stopped (it counts against "
     "`complete`), and narrow_grooves how many are under three cells of the "
     "check wide. `below_stock_mm` is "
     "depth cut past "
     "the WORKPIECE — the spoilboard, which is what a through cut is for; it "
     "is reported, never counted as gouge. "
     "failed_ops [{op, error}] names the operations that produced no "
     "toolpath and are not in the file (first in `says` too). "
     "leftover_other_setups [{setup, name, area_mm2}]: material this setup "
     "leaves that another setup of the job removes -- a flip's other side, "
     "an indexed flat -- said per setup and NOT counted in max_leftover_mm, "
     "the regions or complete; other_setups_not_checked says which setups "
     "could not be consulted and why (their cut then reads as leftover). "
     "To know that, it generates and posts every other setup of the job "
     "too, so it costs more on a job of several setups. The check starts "
     "from the stock the job's EARLIER setups left, so an indexed setup "
     "after a turning one is checked against the turned part. It waits OFF the "
     "frame: the passes cut on the `cam` background job and the simulation "
     "runs as a `cam-verify` one, while other connections are answered; "
     "cancelling either refuses the call. "
     "`ok` = nothing scraps the part or breaks the tool (no "
     "gouge, no rapid through material, no holder, shank, fixture, chuck or "
     "tailstock collision, no bite deeper than the op's passes, and a file "
     "that read straight) and is the one to gate on; over_bites / "
     "worst_bite_mm (_allowed_mm, _at, _line, _op) / bite_by_op [{op, "
     "max_mm, allowed_mm}] say how deep a stretch of cutting actually took "
     "as a full-width equivalent, against the op's stepdown or tab height; "
     "shank_collisions / worst_shank_depth_mm (_at, _line) find a shank "
     "wider than its cutter driven into a wall; `complete` "
     "= nothing left standing. A TAB is not left standing by accident -- the "
     "toolpath says where it put each one and they are excused, up to their "
     "own height, so a tabbed job can now be complete. A dropping "
     "slug will NOT satisfy. "
     "Runs on the file, not the toolpath, so it catches post and modal-state "
     "bugs too. `says` is the verdict in the same sentences the panel shows. "
     "Leftover in several tiny regions is the inside corners a round cutter "
     "cannot reach and is CORRECT; one large region is a missing operation. "
     "`leftover_biggest` lists where, largest first — a height map cannot "
     "watch a slug fall out or know a tab was asked for, so those show up "
     "here too and are for you to recognise.",
     "job, resolution, setup"},
    {"cam_simulate",
     "Play the POSTED file through a block of stock and report what has come "
     "off at a point in it {setup?, at?: 0..1 along the path or \"end\", "
     "play?, close?, resolution?: the height map's cell size in mm, echoed as "
     "resolution_mm}. Returns {open, at, distance_mm, total_mm, "
     "cutting_minutes, removed_mm3, operation, tool, feed_mm_min, line, "
     "position, starts_from, earlier_setups_not_read?}. The stock starts "
     "from what the job's EARLIER setups left (`starts_from` names them; "
     "empty = the fresh stock), so an indexed setup after a turning one "
     "plays against the turned part. The cursor is PATH DISTANCE, not a "
     "clock: every cutting "
     "feed is in the file but no machine states its rapid rate, so a time "
     "axis would be part measurement and part invention — `cutting_minutes` "
     "counts what the file states and nothing else. Generates the toolpaths "
     "if they are not current -- waiting for their passes off the frame, as "
     "cam_toolpath does -- and leaves the app SHOWING the simulation "
     "(screenshot it, or `close` to put the model back).",
     "at, close, job, play, resolution, setup"},
    {"cam_post",
     "Write the G-code for a setup {job?, setup, path, post?}. Pass `job` "
     "unless you are sure which tab is in front: this writes a FILE, and the "
     "wrong job's program under the right name is scrap at the machine. "
     "Reports the line "
     "count, the ops it skipped and why, the targets a multi-target op could "
     "not follow (`skipped`), how much cutting lay beyond the travel and is "
     "missing from the file (`clipped_mm`), the cycle time "
     "(`cutting_minutes`, rapids only when the setup states a traverse rate), "
     "and `warnings`. Writes a "
     "file; it does not send it "
     "to a machine. With split_output on the setup, `path` is the base and "
     "one file per tool is written beside it as `<base>-T<n>.<ext>` (no "
     "spaces: these go on an SD card and through a shell), each "
     "complete and starting by loading its tool; `files` names every one. "
     "Waits for its passes off the frame, as cam_toolpath does; a cancelled "
     "pass refuses the call and writes nothing.",
     "job, path, post, setup"},
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

const char* TypeName(cam::OpType t) { return cam::OpTypeNameOrThrow(t); }

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

  // A field this op type does not have is REFUSED, not applied and ignored.
  // Every one of these used to be accepted for every type: tabs on a drill,
  // `peck` on a face, `edge_size` on a profile all landed in the struct,
  // were never read, and came back out of `get_state` looking like settings
  // that meant something. Checked after the type is settled above, because
  // the same call can change it.
  // The owned list is the op-field table (CamOpFields.h), so a field added
  // there is refused, applied and reported here without a line of its own.
  for (const cam::OpFieldRow& row : cam::OpFieldRows()) {
    if (!p.contains(row.key) || cam::OpTypeHasRow(op.type, row))
      continue;
    std::string takes;
    for (const cam::OpFieldRow& other : cam::OpFieldRows())
      if (other.field != cam::OpField::None &&
          cam::OpTypeHas(op.type, other.field))
        takes += (takes.empty() ? "" : ", ") + std::string(other.key);
    throw std::runtime_error(
        std::string("a ") + cam::OpTypeNameOrThrow(op.type) +
        " operation has no " + row.key + " — it takes " + takes +
        ". Setting it would change a number nothing reads.");
  }

  if (p.contains("name"))
    op.name = p.at("name").get<std::string>();
  if (p.contains("tool")) {
    // Through the same retool the panel's combo uses: a stepdown still at the
    // old cutter's derived value follows the new one, and a tool the setup
    // does not have is REFUSED rather than silently cutting with tool 1.
    // BEFORE the table's fields, so a stepdown stated in the same call wins.
    std::string why;
    if (!cam::Retool(setup, op, p.at("tool").get<int>(), why))
      throw std::runtime_error(why);
  }
  // A thread mill's depths are its thread's: the helix runs over the span the
  // Thread says, past its open ends, and stops on a floor
  // (docs/cam/operations.md#thread-mill). A typed one would steer nothing.
  if (op.type == cam::OpType::ThreadMill &&
      (p.contains("top_z") || p.contains("bottom_z")))
    throw std::runtime_error(
        "a thread_mill's depths are its thread's -- the span of the Thread "
        "on its face, past the ends that open onto air -- so top_z and "
        "bottom_z are not set on one");
  if (p.contains("top_z"))
    op.topZ = p.at("top_z").get<double>();
  if (p.contains("bottom_z"))
    op.bottomZ = p.at("bottom_z").get<double>();
  // Every table field, read THROUGH `p` so the strict unknown-key check sees
  // the key was read.
  for (const cam::OpFieldRow& row : cam::OpFieldRows())
    if (!(row.flags & cam::kOpFieldByHand) && p.contains(row.key))
      cam::SetOpField(row, p.at(row.key), op);
  // The rows with rules of their own.
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
  // An engraving's groove is ONE statement, its width or its depth
  // (docs/cam/operations.md#engrave): stating one clears the other, and the
  // bottom follows it. A groove the tool cannot cut -- a width past a V-bit's
  // cone, a width for a flat -- is refused here, where it is said.
  if (op.type == cam::OpType::Engrave) {
    if (p.contains("width_mm") && !p.contains("depth_mm") &&
        op.engraveWidth > 0.0)
      op.engraveDepth = 0.0;
    if (p.contains("depth_mm") && !p.contains("width_mm") &&
        op.engraveDepth > 0.0)
      op.engraveWidth = 0.0;
    if (const cam::Tool* tool = setup.FindTool(op.toolNumber)) {
      const cam::EngraveGroove g = cam::EngraveGrooveFor(op, *tool);
      if (!g.ok)
        throw std::runtime_error(g.why);
      if (!p.contains("bottom_z"))
        op.bottomZ = op.topZ - g.depth;
    }
  }
  if (p.contains("tabs")) {
    const json& t = p.at("tabs");
    op.tabCount = t.value("count", 0);
    op.tabWidth = t.value("width", 6.0);
    op.tabHeight = t.value("height", 1.5);
  }
  // `target` is one pick, the shape cam_derive_op takes. The derivation
  // read it, but only when it ran -- with top_z and bottom_z both given it
  // did not, the op kept NO target, and a profile meant for one slit cut
  // round the whole part (the chess bishop's mitre gouged the blank it was
  // on, and the call answered "unknown parameter 'target'" after making it).
  if (p.contains("target") && p.contains("targets"))
    throw std::runtime_error(
        "give `targets` (a list) or `target` (one pick), not both");
  const json oneTarget =
      p.contains("target") ? json::array({p.at("target")}) : json::array();
  if (p.contains("targets") || p.contains("target")) {
    op.targets.clear();
    for (const json& tj : p.contains("targets") ? p.at("targets") : oneTarget) {
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
  // Everything set_setup accepts comes back out. `flute_length` and the
  // chipload pair used to go in and vanish, so a caller could not read back
  // what it had just written -- and flute_length in particular decides a
  // safety finding, which makes it worth seeing.
  json j = json{{"number", t.number},
                {"name", t.name},
                {"diameter", t.diameter},
                {"tool_type", cam::ToolTypeName(t.type)},
                {"tip_angle", t.tipAngleDeg},
                {"flutes", t.flutes},
                {"flute_length", t.fluteLength},
                {"overall_length", t.overallLength},
                {"collar_diameter", t.collarDiameter},
                {"shank_diameter", t.shankDiameter},
                {"feed_cut", t.feedCut},
                {"feed_plunge", t.feedPlunge},
                {"spindle_rpm", t.spindleRpm},
                {"chipload_min", t.chiploadMin},
                {"chipload_max", t.chiploadMax}};
  // A thread mill's head (v145): only on one, as in the file.
  if (t.type == cam::ToolType::ThreadMill) {
    j["tip_flat"] = t.tipFlat;
    j["neck_diameter"] = t.neckDiameter;
    j["neck_length"] = t.neckLength;
  }
  return j;
}

// Whether an op's tools reach its depth (cam::ReachCheck,
// docs/cam/setup.md#reach): THROWS when a shank wider than the cutter would be
// driven into a wall, and returns the rest of what it has to say for the
// edit's own `warnings`. Asked by every edit that can change the answer --
// the op's own, and its tool's -- so the problem is said where it is made,
// not first when somebody posts the file. A disabled op cuts nothing and is
// asked again when it is enabled.
json ReachWarnings(const cam::Setup& setup, const cam::Operation& op) {
  json out = json::array();
  for (const cam::Reach& r : cam::ReachChecks(setup, op)) {
    if (r.level == cam::Reach::Level::Collides)
      throw std::runtime_error(r.text);
    out.push_back(r.text);
  }
  return out;
}

json OpJson(const cam::Operation& op) {
  json j = {
      {"id", op.id},           {"name", op.name},  {"type", TypeName(op.type)},
      {"tool", op.toolNumber}, {"top_z", op.topZ}, {"bottom_z", op.bottomZ}};
  // Everything else is what this TYPE uses, from the op-field table
  // (CamOpFields.h), so what comes back out is exactly what set_cam_op will
  // take in. It used to be hand-kept lists that disagreed with the panel's
  // and the serializer's: `stepover` was reported for edge_round ALONE, so a
  // pocket's stepover was settable and unreadable, while `peck` and
  // `lead_arcs` were reported for every type including the ones that never
  // read them. `side` is reported for the clearing types too: a pocket built
  // over the RPC once kept the struct's Outside default INVISIBLY.
  for (const cam::OpFieldRow& row : cam::OpFieldRows())
    if (!(row.flags & cam::kOpFieldByHand) && cam::OpTypeHasRow(op.type, row))
      j[row.key] = cam::OpFieldJson(row, op);
  if (cam::OpTypeHas(op.type, cam::OpField::Finish)) {
    j["finish_tool"] = op.finishTool;
    j["finish_stock"] = op.finishStock;
  }
  if (cam::OpTypeHas(op.type, cam::OpField::Tabs) && op.tabCount > 0)
    j["tabs"] = {{"count", op.tabCount},
                 {"width", op.tabWidth},
                 {"height", op.tabHeight}};
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
json FixtureJson(const cam::Fixture& f) {
  json j = {{"id", f.id},
            {"name", f.name},
            {"pos", {f.pos.x, f.pos.y, f.pos.z}},
            {"rot_z", f.rotZdeg},
            {"enabled", f.enabled}};
  if (f.IsBox())
    j["box"] = {f.box.x, f.box.y, f.box.z};
  else
    j["part"] = f.sourceDocId;
  if (f.seat)
    j["seat"] = {{"feature_id", f.seat->face.featureId},
                 {"element", f.seat->face.name},
                 {"edge", cam::StockEdgeName(f.seat->edge)},
                 {"along_mm", f.seat->alongMm},
                 {"over_mm", f.seat->overMm},
                 {"spin_deg", f.seat->spinDeg}};
  return j;
}

glm::dvec3 RequireBoxSize(const json& v) {
  const auto size = v.get<std::vector<double>>();
  if (size.size() != 3 || size[0] <= 0.0 || size[1] <= 0.0 || size[2] <= 0.0)
    throw std::runtime_error("box is [x, y, z] in mm, each above zero");
  return {size[0], size[1], size[2]};
}

cam::Fixture& RequireFixture(cam::Setup& setup, uint64_t id) {
  if (cam::Fixture* f = setup.FindFixture(id))
    return *f;
  throw std::runtime_error("no fixture " + std::to_string(id) + " in setup " +
                           std::to_string(setup.id) +
                           "; list_cam_setups lists each setup's fixtures");
}

cam::FixtureSide RequireSide(const std::string& text) {
  cam::FixtureSide side{};
  if (!cam::ParseFixtureSide(text, side))
    throw std::runtime_error("unknown side \"" + text +
                             "\" — the sides are -x, +x, -y, +y, centre and "
                             "top");
  return side;
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
// `job` (name or id) switches to that job tab first. Every verb that acts on
// a job takes it: without one they act on whichever job happens to be in
// front, which is how `check_drawing` once judged the wrong sheet and
// reported "ok" (RemoteDrawings.cpp, SelectDrawing). Here it is worse -- a
// `cam_post` after an incidental tab switch writes ANOTHER job's G-code to
// the path you named.
void SelectJob(ui::AppState& state, const RpcParams& p) {
  if (!p.contains("job"))
    return;
  const nlohmann::json& which = p.at("job");
  for (size_t i = 0; i < state.parts.size(); i++) {
    const auto& slot = state.parts[i];
    if (slot.kind != ui::DocKind::Job)
      continue;
    if (which.is_number() ? slot.id == which.get<uint64_t>()
                          : slot.name == which.get<std::string>()) {
      ui::SetActivePart(state, i);
      return;
    }
  }
  std::string have;
  for (const auto& slot : state.parts)
    if (slot.kind == ui::DocKind::Job)
      have += (have.empty() ? "" : ", ") + slot.name;
  throw std::runtime_error("no job named " + which.dump() +
                           "; jobs: " + (have.empty() ? "(none)" : have));
}

// cam_verify in three steps (docs/cam/README.md#requests-that-wait): what
// it reads, COPIED on the frame; the simulation, which touches nothing but
// this struct and so runs inline or on a worker alike; and the answer, on
// the frame again. The synchronous handler runs all three in a row; a
// parked request runs the middle one on a "cam-verify" background job.
struct VerifyRun {
  cam::Setup setup;
  std::vector<cam::Toolpath> paths;
  std::vector<cam::Toolpath::Relief> reliefs; // corner relief: not gouge
  std::vector<cam::Toolpath::Groove> grooves; // engraving: not gouge
  std::vector<cam::Toolpath::Tab> tabs;       // tabs: not leftover
  std::vector<cam::Toolpath::Thread> threads; // judged on their own terms
  std::string gcode;
  double res = 0.25;
  // The job's other setups and their files, so what one of them removes is
  // said as its cut rather than as leftover here
  // (docs/cam/simulation.md#leftover-another-setup-removes).
  std::vector<cam::Setup> others;
  std::vector<std::string> otherFiles;
  std::vector<bool> othersEarlier; // cut before this one, in job order
  std::vector<std::string> unposted;
  std::vector<Body> bodies;
  std::vector<cam::FixtureSolid> fixtures;
  std::vector<std::string> missingFixtures;
  // out
  cam::Verdict verdict;
  std::string error;
  // Main thread only: set by the background job's finish.
  bool landed = false;
  bool cancelled = false;
};

// Step one, on the frame. Throws the handler's refusals. Every pass it reads
// is generated through the caches; a parked request only gets here once
// they are all cut, so nothing here takes long.
std::shared_ptr<VerifyRun>
PrepareVerify(ui::AppState& state, cam::JobDocument& job, const RpcParams& p) {
  auto w = std::make_shared<VerifyRun>();
  const cam::Setup& setup = RequireSetup(job, p.at("setup").get<uint64_t>());
  w->setup = setup;
  w->paths = ui::GenerateCamToolpathsNow(state, setup);
  for (const cam::Toolpath& tp : w->paths) {
    w->reliefs.insert(w->reliefs.end(), tp.reliefs.begin(), tp.reliefs.end());
    w->tabs.insert(w->tabs.end(), tp.tabs.begin(), tp.tabs.end());
    w->grooves.insert(w->grooves.end(), tp.grooves.begin(), tp.grooves.end());
    w->threads.insert(w->threads.end(), tp.threads.begin(), tp.threads.end());
  }
  std::string error;
  if (!cam::PostProcess(setup.machine.post, setup, w->paths, PostOptsOf(setup),
                        w->gcode, &error))
    throw std::runtime_error(error);
  w->res = p.value("resolution", 0.25);
  bool before = true; // the job's order is the order they are cut in
  for (const cam::Setup& other : job.GetSetups()) {
    if (other.id == setup.id)
      before = false;
    if (other.id == setup.id || other.operations.empty())
      continue;
    std::string file, why;
    if (!cam::PostProcess(other.machine.post, other,
                          ui::GenerateCamToolpathsNow(state, other),
                          PostOptsOf(other), file, &why)) {
      w->unposted.push_back("setup '" + other.name +
                            "', because it does not post: " + why);
      continue;
    }
    w->others.push_back(other);
    w->otherFiles.push_back(std::move(file));
    w->othersEarlier.push_back(before);
  }
  w->bodies = ui::JobBodies(state);
  w->fixtures = ui::JobFixtureSolids(state, setup, true, &w->missingFixtures);
  return w;
}

// Step two: the simulations. Reads nothing but `w`.
void ComputeVerify(VerifyRun& w) {
  std::vector<cam::SetupProgram> programs;
  for (size_t k = 0; k < w.others.size(); k++)
    programs.push_back({&w.others[k], w.otherFiles[k], w.othersEarlier[k]});
  cam::OtherSetups others =
      cam::OtherSetupsOf(w.setup, programs, w.bodies, w.res);
  others.notChecked.insert(others.notChecked.end(), w.unposted.begin(),
                           w.unposted.end());
  w.verdict =
      cam::VerifyGcode(w.gcode, w.setup, w.bodies, w.res, w.reliefs, w.tabs,
                       w.fixtures, &others, w.grooves, w.threads);
}

// Step three: the answer, in the same sentences the panel shows.
json VerifyAnswer(ui::AppState& state, const VerifyRun& w) {
  const cam::Verdict& v = w.verdict;
  const double res = w.res;
  const double tol = res * 1.5; // one cell of quantisation, plus slack
  // The same sentences the panel shows. A verdict that reads as a row of
  // numbers over the RPC and as English in the GUI is two verdicts, and the
  // agent gets the worse one.
  bool breaksAnEdge = false;
  for (const cam::Operation& op : w.setup.operations)
    breaksAnEdge |=
        op.enabled && cam::OpTypeHas(op.type, cam::OpField::EdgeSize);
  json says = json::array();
  // What was not cut because its operation produced nothing, FIRST: a
  // setup whose only operation failed verified "ok" -- true, nothing in an
  // empty file is dangerous -- with "cuts nothing" the only hint why.
  json failedOps = json::array();
  for (const cam::Toolpath& tp : w.paths)
    if (!tp.error.empty()) {
      std::string name;
      for (const cam::Operation& op : w.setup.operations)
        if (op.id == tp.operationId)
          name = op.name;
      failedOps.push_back({{"op", tp.operationId}, {"error", tp.error}});
      says.push_back("Operation " + std::to_string(tp.operationId) + " '" +
                     name +
                     "' produced no toolpath and is not in the "
                     "file: " +
                     tp.error);
    }
  for (const cam::VerdictLine& line : cam::Explain(v, tol, 0.0, breaksAnEdge))
    says.push_back(line.text);
  // The machine's lines too -- the parts' reach beyond the travel, the
  // sheet's overhang -- the same helper the panel uses.
  for (const cam::VerdictLine& line :
       cam::MachineLines(w.setup, w.bodies, &w.paths))
    says.push_back(line.text);
  // A fixture that pushed safe Z past the machine's Z, named.
  if (const auto line = ui::FixtureTravelLine(state, w.setup, v))
    says.push_back(line->text);
  // A fixture whose part is gone was not checked, and says so: the same
  // words the panel shows.
  json notChecked = json::array();
  for (const std::string& name : w.missingFixtures) {
    const std::string line = "fixture \"" + name + "\" -- its part is gone";
    notChecked.push_back(line);
    says.push_back("Not checked: " + line);
  }
  if (v.fixturesSkippedRotary)
    notChecked.push_back("fixtures, on a rotary setup");
  json biggest = json::array();
  for (const cam::Verdict::Region& r : v.biggest) {
    json rj{
        {"at", {r.at.x, r.at.y}}, {"area_mm2", r.area}, {"depth_mm", r.depth}};
    if (v.rotary) {
      rj["end_face"] = r.endFace;
      rj["ring"] = r.ring;
      rj["span_deg"] = r.spanDeg;
    }
    biggest.push_back(std::move(rj));
  }
  json otherCuts = json::array();
  for (const cam::Verdict::OtherCut& c : v.leftoverOtherSetups)
    otherCuts.push_back(
        {{"setup", c.setup}, {"name", c.name}, {"area_mm2", c.area}});
  json out = json{
      {"leftover_other_setups", otherCuts},
      {"other_setups_not_checked", v.otherSetupsNotChecked},
      {"max_gouge_mm", v.maxGouge},
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
      {"worst_rapid_depth_mm", v.worstRapidDepth},
      {"worst_rapid_at", {v.worstRapidAt.x, v.worstRapidAt.y}},
      {"worst_rapid_line", v.worstRapidLine},
      // Which STOCK MODEL answered. True means the file's A moved, so
      // it was checked against a rotary blank r(x, theta) and the
      // second number in every `*_at` pair is an ANGLE IN DEGREES,
      // not a Y. A pair of doubles cannot say that and a caller
      // reading an angle as a coordinate would be quietly wrong.
      {"rotary", v.rotary},
      {"file_errors", v.fileErrors},
      {"first_file_error", v.firstFileError},
      {"file_warnings", v.fileWarnings},
      {"first_file_warning", v.firstFileWarning},
      // Cutting that bit deeper than its op's passes take
      // (docs/cam/simulation.md#bite), as a full-width equivalent.
      {"over_bites", v.overBites},
      {"worst_bite_mm", v.worstBite},
      {"worst_bite_allowed_mm", v.worstBiteAllowed},
      {"worst_bite_at", {v.worstBiteAt.x, v.worstBiteAt.y}},
      {"worst_bite_line", v.worstBiteLine},
      {"worst_bite_op", v.worstBiteOp},
      {"bite_by_op",
       [&] {
         json a = json::array();
         for (const auto& [op, b] : v.biteByOp)
           a.push_back(
               {{"op", op}, {"max_mm", b.first}, {"allowed_mm", b.second}});
         return a;
       }()},
      // The chuck and the tailstock of a setup held in the rotary:
      // docs/cam/rotary.md#chuck-and-tailstock.
      {"holding_collisions", v.holdingCollisions},
      {"worst_holding_depth_mm", v.worstHoldingDepth},
      {"worst_holding_x", v.worstHoldingX},
      {"worst_holding_line", v.worstHoldingLine},
      {"worst_holding", v.worstHoldingWhat},
      {"holding_defaults", v.holdingDefaults},
      // A shank wider than the cutter driven into a wall:
      // docs/cam/simulation.md#shank-collisions.
      {"shank_collisions", v.shankCollisions},
      {"worst_shank_depth_mm", v.worstShankDepth},
      {"worst_shank_at", {v.worstShankAt.x, v.worstShankAt.y}},
      {"worst_shank_line", v.worstShankLine},
      {"holder_collisions", v.holderCollisions},
      {"worst_holder_depth_mm", v.worstHolderDepth},
      {"worst_holder_at", {v.worstHolderAt.x, v.worstHolderAt.y}},
      {"worst_holder_line", v.worstHolderLine},
      // The cutter or the holder into a fixture (a clamp, a vise):
      // docs/cam/simulation.md#fixture-collisions.
      {"fixture_collisions", v.fixtureCollisions},
      {"fixtures_checked", v.fixturesChecked},
      {"worst_fixture_depth_mm", v.worstFixtureDepth},
      {"worst_fixture_at", {v.worstFixtureAt.x, v.worstFixtureAt.y}},
      {"worst_fixture_line", v.worstFixtureLine},
      {"worst_fixture", v.worstFixtureName},
      {"worst_fixture_by", v.worstFixtureByHolder ? "holder" : "cutter"},
      {"not_checked", notChecked},
      // Moves the machine cannot reach. A verdict field nothing
      // reports is the same silence this check exists to end.
      {"outside_travel", v.outsideTravel},
      {"outside_travel_axis", std::string(1, v.outsideAxis)},
      {"outside_travel_overshoot_mm", v.worstOvershoot},
      {"first_outside_line", v.firstOutsideLine},
      {"z_span_over_mm", v.zSpanOver},
      {"z_deepest_line", v.zDeepestLine},
      {"z_highest_line", v.zHighestLine},
      {"first_bad_line", v.firstBadLine},
      {"resolution_mm", res},
      {"cells", v.cells},
      {"failed_ops", failedOps},
      {"says", says},
      // Tabs the program left ON PURPOSE. Reported rather than
      // silently subtracted: four is a job to sign off, none where
      // four were asked for is a job to look at.
      {"tabs", v.tabs},
      {"reliefs", v.reliefs},
      {"grooves", v.grooves},
      {"groove_shallow_mm", v.grooveShallow},
      {"narrow_grooves", v.narrowGrooves},
      // Two questions. `ok` is the one that is never in doubt:
      // nothing here scraps the part or breaks the tool. `complete`
      // is softer — a height map cannot watch a slug fall out of a
      // through cut, so a correct job can still report material
      // left over. Tabs no longer count against it.
      {"ok", v.Safe(tol)},
      {"complete", v.Complete(tol)},
      {"cuts_nothing", v.CutsNothing()}};
  // Every milled thread, judged on its own terms
  // (docs/cam/simulation.md#threads): a height map cannot see a flank that
  // faces sideways, so these are read from the file's helix.
  if (!v.threads.empty()) {
    json threads = json::array();
    for (const cam::ThreadVerdict& t : v.threads) {
      json tj = {{"op", t.op},
                 {"designation", t.designation},
                 {"class", t.threadClass},
                 {"class_stated", t.classStated},
                 {"internal", t.internal},
                 {"judged", t.judged}};
      if (!t.judged) {
        tj["why"] = t.why;
      } else {
        tj["pitch_mm"] = t.pitch;
        tj["pitch_measured_mm"] = t.pitchMeasured;
        tj["hand"] = t.rightHand ? "right" : "left";
        tj["hand_measured"] = t.handMeasured ? "right" : "left";
        tj["pitch_diameter_band_mm"] = {t.pdMin, t.pdMax};
        tj["pitch_diameter_mm"] = t.pdReached;
        tj["in_band"] = t.InBand();
        tj["passes"] = t.passes;
        tj["span_mm"] = {t.zLo, t.zHi};
        tj["full_thread_mm"] = {std::max(t.fullFrom, t.zLo),
                                std::min(t.fullTo, t.zHi)};
        tj["short_mm"] = t.shortMm;
        tj["floor_limited"] = t.floorLimited;
        tj["bore_standing_mm"] = t.boreStanding;
        tj["neck_clearance_mm"] = t.neckClearance;
        tj["ok"] = t.Safe(tol);
        tj["good"] = t.Good(tol);
      }
      threads.push_back(std::move(tj));
    }
    out["threads"] = std::move(threads);
  }
  // The typical scallop, where the worst cell is an end face no rotary
  // pass can reach. Rotary only: the height map does not compute it, and
  // a zero here would read as a measurement.
  if (v.rotary) {
    out["leftover_p95_mm"] = v.leftoverP95;
    out["leftover_surface_max_mm"] = v.leftoverSurfaceMax;
    out["leftover_surface_at"] = {v.leftoverSurfaceAt.x, v.leftoverSurfaceAt.y};
    out["leftover_surface_p99_mm"] = v.leftoverSurfaceP99;
    out["leftover_end_face_area_mm2"] = v.leftoverEndFaceArea;
  }
  {
    // A simulated cut on a height map at `resolution_mm`, not the B-Rep —
    // so PREDICTED (core/Receipt.h). Anything narrower than a cell is
    // invisible to it, and `complete` is soft by construction because a
    // height map cannot watch a slug fall out of a through cut. The receipt
    // can therefore never read better than provisional, which is the honest
    // standing for a number that comes from a discretisation.
    receipt::Receipt receipt;
    const receipt::Oracle oracle{"designer/cam-stocksim", "1"};
    const std::string note = "height-map simulation at " + std::to_string(res) +
                             " mm resolution over " + std::to_string(v.cells) +
                             " cells; anything narrower than a cell is "
                             "below what this can see";
    receipt::Claim safe(
        "the program does not cut into the part or the "
        "machine, and asks no more of the tool than its "
        "passes and its reach allow",
        oracle, v.Safe(tol) ? receipt::Verdict::Pass : receipt::Verdict::Fail);
    safe.basis = receipt::Basis::Predicted;
    safe.value = v.maxGouge;
    safe.unit = "mm";
    safe.note = note;
    receipt.Add(std::move(safe));
    receipt::Claim done("the program cuts everything it was asked to", oracle,
                        v.Complete(tol) ? receipt::Verdict::Pass
                                        : receipt::Verdict::Fail);
    done.basis = receipt::Basis::Predicted;
    done.value = v.maxLeftover;
    done.unit = "mm";
    done.note = note;
    receipt.Add(std::move(done));
    AttachReceipt(out, receipt);
  }
  return out;
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
    const uint64_t cfg =
        SourceConfiguration(state, sourceId, sourceName,
                            p.value("configuration", nlohmann::json(nullptr)));
    const size_t tab =
        ui::CreateJobForPart(state, sourceId, p.value("name", std::string()));
    cam::JobDocument* made = ui::ActiveJob(state);
    if (made == nullptr || made->GetParts().empty())
      throw std::runtime_error("could not create a job for '" + sourceName +
                               "'");
    if (cfg != 0) {
      made->GetParts()[0].configuration = cfg;
      made->BumpVersion();
    }
    return json{{"id", state.parts[tab].id},
                {"name", state.parts[tab].name},
                {"part", json{{"id", made->GetParts()[0].id},
                              {"source", sourceId},
                              {"name", sourceName},
                              {"configuration", cfg}}}};
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

  // Which job, before anything asks what the active one holds.
  SelectJob(state, p);
  cam::JobDocument* job = ui::ActiveJob(state);
  // One undo step per RPC that changes the job (R7); a refused call moves
  // nothing and records nothing.
  std::optional<ui::JobUndoGuard> undoGuard;
  if (job != nullptr)
    undoGuard.emplace(state, /*coalesce=*/false, /*compareContent=*/true);
  if (job == nullptr)
    throw std::runtime_error(
        "CAM works on a job, not a part: a setup stopped being part of the "
        "part in v73. `machine_part` makes a job from the part you have open "
        "and switches to it");

  if (m == "add_job_part") {
    // `job` is read in the preamble, before the active job is looked up --
    // one place for every branch, and invisible to the scanner from here.
    // rpc-params: job
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
    const uint64_t cfg =
        SourceConfiguration(state, sourceId, sourceName,
                            p.value("configuration", nlohmann::json(nullptr)));
    const uint64_t id = job->AddPart(sourceId, sourceName);
    job->FindPart(id)->configuration = cfg;
    job->BumpVersion();
    return json{{"id", id},
                {"source", sourceId},
                {"name", sourceName},
                {"configuration", cfg}};
  }

  if (m == "set_job_part") {
    // `job` is read in the preamble, before the active job is looked up --
    // one place for every branch, and invisible to the scanner from here.
    // rpc-params: job
    const uint64_t id = p.at("id").get<uint64_t>();
    const cam::JobPart* part = job->FindPart(id);
    if (part == nullptr)
      throw std::runtime_error("no job part with id " + std::to_string(id) +
                               "; list_job_parts shows the ones that exist");
    if (!p.contains("configuration"))
      throw std::runtime_error(
          "set_job_part needs {id, configuration: name or id, or null for "
          "the part's active one}");
    const uint64_t cfg = SourceConfiguration(state, part->sourceDocId,
                                             part->name, p.at("configuration"));
    std::string why;
    int moved = 0;
    if (!ui::SetJobPartConfiguration(state, id, cfg, &why, &moved))
      throw std::runtime_error(why);
    json out{{"id", id}, {"configuration", cfg}, {"moved", moved}};
    if (cfg != 0)
      if (const Document* src = ui::JobSourceDocument(state, part->sourceDocId))
        if (const Configuration* c = src->FindConfiguration(cfg))
          out["configuration_name"] = c->name;
    return out;
  }

  if (m == "remove_job_part") {
    // `job` is read in the preamble, before the active job is looked up --
    // one place for every branch, and invisible to the scanner from here.
    // rpc-params: job
    const uint64_t id = p.at("id").get<uint64_t>();
    if (!job->RemovePart(id))
      throw std::runtime_error("no job part with id " + std::to_string(id) +
                               "; list_job_parts shows the ones that exist");
    job->BumpVersion();
    ui::RefreshCamToolpaths(state);
    return json{{"removed", id}};
  }

  if (m == "list_job_parts") {
    // `job` is read in the preamble, before the active job is looked up --
    // one place for every branch, and invisible to the scanner from here.
    // rpc-params: job
    json out = json::array();
    for (const cam::JobPart& part : job->GetParts()) {
      json pj = {{"id", part.id},
                 {"source", part.sourceDocId},
                 {"name", part.name},
                 {"configuration", part.configuration}};
      if (part.configuration != 0)
        if (const Document* src =
                ui::JobSourceDocument(state, part.sourceDocId))
          if (const Configuration* c =
                  src->FindConfiguration(part.configuration))
            pj["configuration_name"] = c->name;
      // A source that is gone -- or no longer builds in the configuration
      // the job cuts -- is REPORTED, never substituted: the job cannot cut
      // what it cannot find, and saying nothing here is how a job posts an
      // empty file and looks fine doing it.
      std::string why;
      pj["missing"] = ui::JobPartDocument(state, part, &why) == nullptr;
      if (pj["missing"])
        pj["why"] = why;
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
    // `job` is read in the preamble, before the active job is looked up --
    // one place for every branch, and invisible to the scanner from here.
    // rpc-params: job
    json out = json::array();
    size_t placements = 0, outside = 0, refusals = 0;
    for (cam::Setup& setup : job->GetSetups()) {
      if (p.contains("setup") && setup.id != p.at("setup").get<uint64_t>())
        continue;
      const std::vector<Body>& bodies = ui::JobBodies(state);
      json fits = json::array();
      const std::vector<cam::PlacementFit> checked =
          cam::CheckPlacements(setup, bodies);
      placements += checked.size();
      for (const cam::PlacementFit& f : checked) {
        if (f.fit != cam::Fit::Inside)
          outside++;
        json fj = {{"placement", f.placementId},
                   {"part", f.jobPartId},
                   {"fit", f.fit == cam::Fit::Inside ? "inside"
                           : f.fit == cam::Fit::PartlyOutside
                               ? "partly_outside"
                               : "fully_outside"}};
        if (!f.overlaps.empty())
          fj["overlaps"] = f.overlaps;
        // Where the material is, in setup space -- the box this check had to
        // measure to answer `fit` at all. A caller laying a sheet out wants
        // exactly this and had to reconstruct it one part at a time.
        fj["box"] = {{"min", {f.lo.x, f.lo.y, f.lo.z}},
                     {"max", {f.hi.x, f.hi.y, f.hi.z}}};
        fits.push_back(std::move(fj));
      }
      json sj = {{"setup", setup.id},
                 {"name", setup.name},
                 {"placements", std::move(fits)}};
      const std::string why = cam::PlacementRefusal(setup, bodies);
      if (!why.empty())
        sj["refusal"] = why;
      const std::vector<std::string> warn = cam::LayoutWarnings(setup, bodies);
      if (!warn.empty())
        sj["warnings"] = warn;
      out.push_back(std::move(sj));
      if (!why.empty())
        refusals++;
    }
    json result{{"setups", std::move(out)}};
    {
      // A job with no setups, or a setup with nothing placed in it, has
      // nothing to lay out badly. Reporting that as a clean layout is the
      // vacuous pass this receipt exists to name.
      receipt::Receipt receipt;
      const receipt::Oracle oracle{"designer/cam-layout", "1"};
      const std::string claim = "every part is inside its stock and clear of "
                                "the others";
      if (placements == 0) {
        receipt.Unverifiable(claim, oracle,
                             "nothing is placed in any setup checked here, so "
                             "there is no layout to be right or wrong");
      } else {
        receipt::Claim c(claim, oracle,
                         (outside == 0 && refusals == 0)
                             ? receipt::Verdict::Pass
                             : receipt::Verdict::Fail);
        c.basis = receipt::Basis::Verified;
        c.value = static_cast<double>(placements);
        c.unit = "placements";
        c.note = std::to_string(outside) + " outside the stock, " +
                 std::to_string(refusals) + " setup(s) refused";
        receipt.Add(std::move(c));
      }
      AttachReceipt(result, receipt);
    }
    return result;
  }

  if (m == "set_job_placement") {
    // `job` is read in the preamble, before the active job is looked up --
    // one place for every branch, and invisible to the scanner from here.
    // rpc-params: job
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
      if (!cam::FaceNormalOf(ui::JobBodies(state), ref, normal, partId))
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

  if (m == "add_job_fixture") {
    // rpc-params: job
    cam::Setup& setup = RequireSetup(*job, p.at("setup").get<uint64_t>());
    if (p.contains("part") == p.contains("box"))
      throw std::runtime_error(
          "a fixture is a part {part: id or name} or a keep-out box {box: "
          "[x, y, z] mm}; pass exactly one");
    cam::Fixture f;
    if (p.contains("box")) {
      f.box = RequireBoxSize(p.at("box"));
      f.name =
          p.value("name", "Clamp " + std::to_string(setup.fixtures.size() + 1));
    } else {
      const nlohmann::json& pj = p.at("part");
      const ui::PartSlot* slot = nullptr;
      for (const ui::PartSlot& s : state.parts)
        if (s.kind == ui::DocKind::Part &&
            (pj.is_string() ? s.name == pj.get<std::string>()
                            : s.id == pj.get<uint64_t>()))
          slot = &s;
      if (slot == nullptr)
        throw std::runtime_error("no part " + pj.dump() +
                                 " in this project; list_parts shows them");
      for (const cam::JobPart& jp : job->GetParts())
        if (jp.sourceDocId == slot->id)
          throw std::runtime_error(
              "\"" + slot->name +
              "\" is a part this job CUTS; a fixture is what holds it, and "
              "one part cannot be both");
      f.sourceDocId = slot->id;
      f.name = p.value("name", slot->name);
    }
    const std::vector<Body> model = ui::FixtureModelBodies(state, f);
    if (model.empty())
      throw std::runtime_error("\"" + f.name +
                               "\" has no solid to hold anything with");
    f.id = job->AllocateId();
    f.rotZdeg = p.value("rot_z", 0.0);
    if (p.contains("pos")) {
      const auto v = p.at("pos").get<std::vector<double>>();
      if (v.size() != 3)
        throw std::runtime_error("pos is [x, y, z] in setup space, mm");
      f.pos = {v[0], v[1], v[2]};
    } else {
      // Derived from the stock and the fixture's own size, never typed.
      const cam::FixtureSide side =
          p.contains("side")
              ? RequireSide(p.at("side"))
              : cam::DefaultFixtureSide(setup, ui::JobBodies(state), model,
                                        f.rotZdeg);
      if (!cam::PlaceFixture(setup, ui::JobBodies(state), model, f.rotZdeg,
                             side, f.pos))
        throw std::runtime_error(
            "there is no stock to place a fixture against yet; add a part "
            "to the job first");
    }
    setup.fixtures.push_back(f);
    ui::CamFixturesChanged(state, setup);
    json out = FixtureJson(f);
    out["setup"] = setup.id;
    out["safe_z"] = setup.safeZ;
    return out;
  }

  if (m == "set_job_fixture") {
    // rpc-params: job
    cam::Setup& setup = RequireSetup(*job, p.at("setup").get<uint64_t>());
    cam::Fixture& f = RequireFixture(setup, p.at("fixture").get<uint64_t>());
    // Validate everything before changing anything: a refused call leaves
    // the fixture as it was.
    std::optional<cam::FixtureSide> side;
    if (p.contains("side"))
      side = RequireSide(p.at("side"));
    std::optional<glm::dvec3> pos;
    if (p.contains("pos")) {
      const auto v = p.at("pos").get<std::vector<double>>();
      if (v.size() != 3)
        throw std::runtime_error("pos is [x, y, z] in setup space, mm");
      pos = glm::dvec3(v[0], v[1], v[2]);
    }
    if (pos && side)
      throw std::runtime_error(
          "pos and side both say where the fixture goes; pass one");
    // A seat (docs/cam/setup.md#seats): validated whole -- the face, the
    // edge, and that it SOLVES against the stock -- before anything changes.
    std::optional<std::optional<cam::FixtureSeat>> seat; // outer: given
    const bool placing = pos || side || p.contains("rot_z");
    const bool unseating = p.contains("seat") && p.at("seat").is_null();
    // A seated fixture's pose is SOLVED: a pos written into it would be
    // replaced by the next solve, in silence. Say so instead.
    if (f.seat && placing && !unseating)
      throw std::runtime_error(
          "\"" + f.name +
          "\" rests on the stock, so its place is solved from its seat; pass "
          "seat: null with pos, side or rot_z to stand it on its own");
    if (p.contains("seat")) {
      const json& sj = p.at("seat");
      if (!sj.is_null() && placing)
        throw std::runtime_error(
            "a seat says where the fixture goes and how it is turned; pass it "
            "without pos, side or rot_z");
      if (sj.is_null()) {
        seat = std::optional<cam::FixtureSeat>{};
      } else {
        if (f.IsBox())
          throw std::runtime_error("\"" + f.name +
                                   "\" is a keep-out box, which has no faces "
                                   "to rest on; give it a pos");
        cam::FixtureSeat s;
        s.face.featureId = sj.at("feature_id").get<uint64_t>();
        s.face.name = sj.at("element").get<std::string>();
        const std::string edge = sj.at("edge").get<std::string>();
        if (!cam::ParseStockEdge(edge, s.edge))
          throw std::runtime_error("unknown seat edge '" + edge +
                                   "'; use -x, +x, -y or +y");
        s.alongMm = sj.at("along_mm").get<double>();
        s.overMm = sj.at("over_mm").get<double>();
        s.spinDeg = sj.value("spin_deg", 0.0);
        glm::dvec3 lo(0.0), hi(0.0), at(0.0);
        double rot = 0.0;
        std::string why = "there is no stock to rest it on";
        if (!cam::StockBounds(setup, ui::JobBodies(state), lo, hi) ||
            !cam::SolveFixtureSeat(lo, hi, ui::FixtureModelBodies(state, f), s,
                                   at, rot, &why))
          throw std::runtime_error("cannot rest \"" + f.name +
                                   "\" there: " + why);
        seat = s;
      }
    }
    if (p.contains("name"))
      f.name = p.at("name").get<std::string>();
    if (p.contains("enabled"))
      f.enabled = p.at("enabled").get<bool>();
    if (p.contains("box") && !f.IsBox())
      throw std::runtime_error("\"" + f.name +
                               "\" is a part, not a box: its size is the "
                               "part's");
    std::optional<glm::dvec3> box;
    if (p.contains("box"))
      box = RequireBoxSize(p.at("box"));
    if (p.contains("rot_z"))
      f.rotZdeg = p.at("rot_z").get<double>();
    if (box)
      f.box = *box;
    if (pos)
      f.pos = *pos;
    if (seat)
      f.seat = *seat; // CamFixturesChanged below solves it into pos/rot_z
    if (side && !cam::PlaceFixture(setup, ui::JobBodies(state),
                                   ui::FixtureModelBodies(state, f), f.rotZdeg,
                                   *side, f.pos))
      throw std::runtime_error("no stock, or no fixture solid, to place by");
    ui::CamFixturesChanged(state, setup);
    json out = FixtureJson(f);
    out["setup"] = setup.id;
    out["safe_z"] = setup.safeZ;
    return out;
  }

  if (m == "remove_job_fixture") {
    // rpc-params: job
    cam::Setup& setup = RequireSetup(*job, p.at("setup").get<uint64_t>());
    const uint64_t id = p.at("fixture").get<uint64_t>();
    (void)RequireFixture(setup, id);
    std::erase_if(setup.fixtures,
                  [id](const cam::Fixture& f) { return f.id == id; });
    ui::CamFixturesChanged(state, setup);
    return json{{"removed", id}, {"setup", setup.id}, {"safe_z", setup.safeZ}};
  }

  if (m == "add_cam_setup") {
    // `job` is read in the preamble, before the active job is looked up --
    // one place for every branch, and invisible to the scanner from here.
    // rpc-params: job
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
    setup.safeZ = ui::DeriveJobSafeZ(state, setup);
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
    // `job` is read in the preamble, before the active job is looked up --
    // one place for every branch, and invisible to the scanner from here.
    // rpc-params: job
    json out = json::array();
    for (const cam::Setup& setup : job->GetSetups()) {
      json sj = {{"id", setup.id},
                 {"name", setup.name},
                 {"post", setup.machine.post},
                 {"safe_z", setup.safeZ},
                 {"rotation_a", setup.rotationA},
                 {"has_rotary_axis", setup.machine.hasRotaryAxis},
                 {"held_in", cam::HeldInRotary(setup) ? "rotary" : "bed"},
                 {"grip_mm", setup.gripLength},
                 {"tailstock", setup.tailstock},
                 {"chuck", setup.chuck},
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
      // The stock: what was ASKED for, and the box it resolves to.
      //
      // The BOX was already reported. What was not, and what a caller cannot
      // otherwise discover, is the request behind it -- the mode, the typed
      // size, the placement and the margins. So `set_cam_setup {stock: ...}`
      // could be called and never read back, and a fixed size that did not
      // take looked exactly like one that did. Same asymmetry Phase 0 closed
      // for op fields, on the field that decides how much material you buy
      // and where the work origin sits.
      //
      // One place, not two: this used to be assembled here and then
      // overwritten by a narrower copy forty lines further down.
      json stock = {
          {"mode",
           setup.stock.mode == cam::StockMode::Fixed ? "fixed" : "relative"},
          {"margin",
           {setup.stock.margin.x, setup.stock.margin.y, setup.stock.margin.z}},
          {"bottom_margin", setup.stock.bottomMargin},
          {"size",
           {setup.stock.size.x, setup.stock.size.y, setup.stock.size.z}},
          {"place",
           {setup.stock.place.x, setup.stock.place.y, setup.stock.place.z}}};
      glm::dvec3 slo(0.0), shi(0.0);
      if (cam::StockBounds(setup, ui::JobBodies(state), slo, shi))
        stock["box"] = {{"min", {slo.x, slo.y, slo.z}},
                        {"max", {shi.x, shi.y, shi.z}}};
      if (const std::string why =
              cam::StockRefusal(setup, ui::JobBodies(state));
          !why.empty())
        stock["refusal"] = why;
      sj["stock"] = std::move(stock);

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
      sj["fixtures"] = json::array();
      for (const cam::Fixture& f : setup.fixtures)
        sj["fixtures"].push_back(FixtureJson(f));
      out.push_back(std::move(sj));
    }
    return json{{"setups", out}};
  }

  if (m == "set_cam_setup") {
    // `job` is read in the preamble, before the active job is looked up --
    // one place for every branch, and invisible to the scanner from here.
    // rpc-params: job
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
    // The axis before the angle, so a call that states both is judged on
    // what it asked for rather than on what the setup used to be.
    if (p.contains("has_rotary_axis"))
      setup.machine.hasRotaryAxis = p.at("has_rotary_axis").get<bool>();
    // The setup's copy of the machine's rotary mount and holding, as the
    // store's profile would state them -- refused whole, naming the fields.
    if (p.contains("rotary"))
      setup.machine.rotary =
          p.at("rotary").is_null()
              ? cam::RotaryMount{}
              : DocumentIO::RotaryMountFromJson(p.at("rotary"));
    if (p.contains("holding"))
      setup.machine.holding =
          DocumentIO::RotaryHoldingFromJson(p.at("holding"));
    if (p.contains("rotation_a")) {
      const double a = p.at("rotation_a").get<double>();
      // REFUSED, not quietly honoured. `SetupToModel` applies rotationA
      // whatever the machine says, while `EmitsRotary` needs the flag before
      // it will post an A word -- so an index on a machine with no 4th axis
      // generated a toolpath for a part turned 90 degrees and a FILE that
      // never turns it. The part is cut on a face the machine cannot present:
      // plausible G-code, wrong part, no message. The GUI cannot reach this
      // (the A drag lives inside the flag) and neither can a loaded file
      // (`has_rotary_axis` defaults to `rotation_a != 0` on pre-v77 jobs);
      // this door was the one left open, and the row above has been claiming
      // the invariant it did not enforce.
      if (std::abs(a) > 1e-9 && !setup.machine.hasRotaryAxis)
        throw std::runtime_error(
            "rotation_a needs a machine with a 4th axis: set has_rotary_axis "
            "as well, or name a machine that has one. Without it the part "
            "would be MACHINED as if turned and the file would never turn it");
      setup.rotationA = a;
    }
    // Where the part is held, and by what
    // (docs/cam/rotary.md#chuck-and-tailstock).
    if (p.contains("held_in")) {
      const std::string h = p.at("held_in").get<std::string>();
      if (h == "rotary")
        setup.heldIn = cam::Setup::HeldIn::Rotary;
      else if (h == "bed")
        setup.heldIn = cam::Setup::HeldIn::Bed;
      else if (h == "derived")
        setup.heldIn = cam::Setup::HeldIn::Derived;
      else
        throw std::runtime_error("unknown held_in '" + h +
                                 "'; a part is held in the rotary, on the "
                                 "bed, or `derived` from its operations");
    }
    if (p.contains("grip_mm")) {
      const double g = p.at("grip_mm").get<double>();
      if (g < 0.0)
        throw std::runtime_error(
            "grip_mm is how much of the stock the jaws hold, in mm; 0 for "
            "the machine's, and never negative (`chuck: false` for a part "
            "held some other way)");
      setup.gripLength = g;
    }
    if (p.contains("tailstock"))
      setup.tailstock = p.at("tailstock").get<bool>() ? 1 : 0;
    if (p.contains("chuck"))
      setup.chuck = p.at("chuck").get<bool>() ? 1 : 0;
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
        const cam::DerivedReach d = cam::DeriveReachOffset(
            setup, ui::JobBodies(state), ui::JobSketchCurves(state, setup));
        if (!d.ok)
          throw std::runtime_error(d.why);
        setup.reachOffset = d.offset;
      } else if (r.is_array() && r.size() == 2) {
        setup.reachOffset = glm::dvec2(r[0].get<double>(), r[1].get<double>());
      } else {
        throw std::runtime_error(
            "reach_offset is [x, y] in mm from the default window (centred on "
            "a stock that fits, at the near corner of a longer one), or "
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
      const uint64_t partId = fj.value("part", uint64_t{0});
      if (partId != 0 && job->FindPart(partId) == nullptr)
        throw std::runtime_error("lay_face_up: no job part with id " +
                                 std::to_string(partId) +
                                 "; list_job_parts shows the ones that exist");
      // Every Derived part's feature is id 1, so in a job of several parts
      // a feature id alone names a face on each of them: which one is the
      // caller's to say, not the first body's.
      if (partId == 0) {
        std::vector<uint64_t> owners;
        for (const Body& body : ui::JobBodies(state))
          if (body.createdBy == ref.featureId && body.jobPartId != 0 &&
              body.elementMap.Resolve(ref.name) &&
              std::ranges::find(owners, body.jobPartId) == owners.end())
            owners.push_back(body.jobPartId);
        if (owners.size() > 1) {
          std::string ids;
          for (uint64_t o : owners)
            ids += (ids.empty() ? "" : ", ") + std::to_string(o);
          throw std::runtime_error(
              "lay_face_up: face '" + ref.name + "' of feature " +
              std::to_string(ref.featureId) + " is on job parts " + ids +
              " — say which with lay_face_up.part");
        }
      }
      glm::vec3 normal(0.0f);
      if (!cam::FaceNormalOf(ui::JobBodies(state), ref, normal, partId))
        throw std::runtime_error(
            "no planar face '" + ref.name + "' on feature " +
            std::to_string(ref.featureId) + " — lay_face_up needs a flat face");
      setup.frame = cam::SolveFaceUp(glm::dvec3(normal), setup.frame);
      setup.safeZ = ui::DeriveJobSafeZ(state, setup);
    }
    // The other side of the part, derived from this frame rather than typed.
    if (p.value("flip", false)) {
      setup.frame = cam::FlipFrame(setup.frame);
      setup.safeZ = ui::DeriveJobSafeZ(state, setup);
    }
    // Anything above can have moved the stock box, and the work origin is
    // derived from it — as is every coordinate the post writes.
    (void)cam::RecomputeOrigin(setup, ui::JobBodies(state));
    // ...and the safe height is derived from it too, so an edit that grows
    // what the cutter must clear -- a bigger stock, the part put in the
    // rotary, where a square bar sweeps its corners -- raises it. Never
    // lowers it: a height someone typed stays theirs. Without this a setup
    // added and then put in the rotary kept the bed's safe Z below its own
    // blank's corners.
    if (!p.contains("safe_z") &&
        (p.contains("stock") || p.contains("held_in") ||
         p.contains("rotation_a") || p.contains("has_rotary_axis") ||
         p.contains("machine")))
      setup.safeZ = std::max(setup.safeZ, ui::DeriveJobSafeZ(state, setup));
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
      // REFUSED, not defaulted to 1. A tool object with no number used to
      // edit tool 1 -- rename it, resize it, change its type -- and report
      // success, which is how a 6 mm ball asked for as tool 2 became a
      // rename of the 6 mm flat with nothing said. The row's wording invites
      // exactly that call, so the substitution had to go
      // (docs/cam/README.md#invariants).
      if (!tj.contains("number"))
        throw std::runtime_error(
            "a tool needs a `number`: it is which tool of the setup to add or "
            "edit, and without it this would silently edit tool 1. Note that "
            "`id` (the setup) and `tool` are SIBLING parameters: "
            "set_cam_setup {id: <setup>, tool: {number: 2, ...}}");
      const int number = tj.at("number").get<int>();
      cam::Tool* existing = nullptr;
      for (cam::Tool& t : setup.tools)
        if (t.number == number)
          existing = &t;
      // Edited as a copy and committed only once every check below has
      // passed: a refused edit leaves the tool, and the setup, as they were.
      cam::Tool next;
      next.number = number;
      if (existing != nullptr)
        next = *existing;
      cam::Tool* tool = &next;
      tool->name = tj.value("name", tool->name);
      tool->diameter = tj.value("diameter", tool->diameter);
      const bool wasThreadMill = tool->type == cam::ToolType::ThreadMill;
      if (tj.contains("tool_type"))
        tool->type = ToolTypeFrom(tj.at("tool_type").get<std::string>());
      tool->tipAngleDeg = tj.value("tip_angle", tool->tipAngleDeg);
      tool->flutes = tj.value("flutes", tool->flutes);
      tool->fluteLength = tj.value("flute_length", tool->fluteLength);
      tool->feedCut = tj.value("feed_cut", tool->feedCut);
      tool->feedPlunge = tj.value("feed_plunge", tool->feedPlunge);
      tool->spindleRpm = tj.value("spindle_rpm", tool->spindleRpm);
      tool->chiploadMin = tj.value("chipload_min", tool->chiploadMin);
      tool->chiploadMax = tj.value("chipload_max", tool->chiploadMax);
      tool->overallLength = tj.value("overall_length", tool->overallLength);
      tool->collarDiameter = tj.value("collar_diameter", tool->collarDiameter);
      tool->shankDiameter = tj.value("shank_diameter", tool->shankDiameter);
      tool->tipFlat = tj.value("tip_flat", tool->tipFlat);
      tool->neckDiameter = tj.value("neck_diameter", tool->neckDiameter);
      tool->neckLength = tj.value("neck_length", tool->neckLength);
      if (tool->diameter <= 0.0)
        throw std::runtime_error("tool diameter must be positive");
      if (tool->type == cam::ToolType::ThreadMill) {
        // A thread mill's head (docs/cam/operations.md#the-thread-mill-cutter):
        // what nobody measured is derived from the diameter, and its flute
        // is the head's own width -- derived, so a typed one is refused
        // rather than silently replaced.
        if (tj.contains("flute_length"))
          throw std::runtime_error(
              "a thread mill's flute_length is its head's axial width, "
              "derived from diameter, neck_diameter, tip_flat and tip_angle; "
              "neck_length is how far it reaches above the head");
        if (tool->tipFlat < 0.0 || tool->neckDiameter < 0.0 ||
            tool->neckLength < 0.0)
          throw std::runtime_error(
              "tip_flat, neck_diameter and neck_length are sizes, and cannot "
              "be negative; 0 derives each from the diameter");
        cam::FillThreadMillShape(*tool,
                                 wasThreadMill || tj.contains("tip_angle"));
        if (tool->neckDiameter >= tool->diameter)
          throw std::runtime_error(
              "neck_diameter must be narrower than the head (diameter): the "
              "head stands out from its neck by the depth it can cut");
      } else if (tj.contains("tip_flat") || tj.contains("neck_diameter") ||
                 tj.contains("neck_length")) {
        throw std::runtime_error(
            "tip_flat, neck_diameter and neck_length describe a thread "
            "mill's head; this tool is a " +
            std::string(cam::ToolTypeName(tool->type)));
      }
      if (tool->fluteLength <= 0.0)
        throw std::runtime_error(
            "flute_length must be positive: it is how deep this cutter can "
            "cut, and the reach check compares every operation's depth "
            "against it");
      // Half a band is not a band, and a backwards one is a typo that would
      // pass every feed silently. Refused rather than repaired.
      if ((tool->chiploadMin > 0.0) != (tool->chiploadMax > 0.0))
        throw std::runtime_error(
            "chipload_min and chipload_max come as a pair; give both, or "
            "neither and the check falls back to a rule off the diameter");
      if (tool->chiploadMin > 0.0 && tool->chiploadMax <= tool->chiploadMin)
        throw std::runtime_error("chipload_max must be above chipload_min");
      // The holder is a pair too, and for a sharper reason than the chip
      // load: one of the two alone silently disables the check, so a person
      // who typed a number would believe their holder was being looked at
      // while nothing was looking. Refuse, and name the other half.
      if ((tool->overallLength > 0.0) != (tool->collarDiameter > 0.0))
        throw std::runtime_error(
            "overall_length and collar_diameter come as a pair: the holder "
            "check needs how far the tool sticks out AND how wide the collet "
            "nut is. Give both, or neither and no holder is checked");
      if (tool->overallLength > 0.0 && tool->overallLength < tool->fluteLength)
        throw std::runtime_error(
            "overall_length is measured from the TIP and includes the "
            "flutes, so it cannot be shorter than flute_length");
      if (tool->shankDiameter < 0.0)
        throw std::runtime_error(
            "shank_diameter cannot be negative; 0 (or leaving it out) means "
            "the tool is the cutter's width all the way up");
      if (tool->shankDiameter > 0.0 && tool->collarDiameter > 0.0 &&
          tool->shankDiameter > tool->collarDiameter)
        throw std::runtime_error(
            "shank_diameter is wider than collar_diameter, and the shank "
            "sits INSIDE the collet nut -- one of the two is a typo");
      // Every op this tool cuts, against the tool as it is about to be: a
      // shorter flute or a wider shank can make an existing op uncuttable,
      // and that is refused here, naming the op, rather than found at post.
      json warnings = json::array();
      for (const cam::Operation& op : setup.operations) {
        if (op.toolNumber != number && op.finishTool != number)
          continue;
        cam::Setup trial;
        trial.tools = {next};
        for (const json& w : ReachWarnings(trial, op))
          warnings.push_back(w);
      }
      if (existing != nullptr)
        *existing = next;
      else
        setup.tools.push_back(next);
      json out{{"id", setup.id}};
      if (!warnings.empty())
        out["warnings"] = warnings;
      return out;
    }
    return json{{"id", setup.id}};
  }

  if (m == "remove_cam_setup") {
    // `job` is read in the preamble, before the active job is looked up --
    // one place for every branch, and invisible to the scanner from here.
    // rpc-params: job
    const uint64_t id = p.at("id").get<uint64_t>();
    if (!job->RemoveSetup(id))
      throw std::runtime_error("no CAM setup with id " + std::to_string(id));
    return json::object();
  }

  if (m == "add_cam_op" || m == "cam_derive_op") {
    // `job` is read in the preamble, before the active job is looked up --
    // one place for every branch, and invisible to the scanner from here.
    // rpc-params: job
    // Every field of an operation that is not depth or pick is read inside
    // ApplyOpFields, which tools/rpc_params_scan.py FOLLOWS: its own reads
    // plus every key of the op-field table. No annotation to go stale.
    cam::Setup& setup = RequireSetup(*job, p.at("setup").get<uint64_t>());
    cam::Operation op;
    op.toolNumber = setup.tools.empty() ? 1 : setup.tools.front().number;
    // A thread is milled with a thread mill: without a `tool`, the setup's
    // first one rather than its first tool of any kind.
    if (!p.contains("tool") &&
        p.value("type", std::string()) ==
            cam::OpTypeNameOrThrow(cam::OpType::ThreadMill))
      for (const cam::Tool& t : setup.tools)
        if (t.type == cam::ToolType::ThreadMill) {
          op.toolNumber = t.number;
          break;
        }
    if (p.contains("tool"))
      op.toolNumber = p.at("tool").get<int>();
    // Derive when no depth was typed. This is the door the GUI's verbs go
    // through (cam::DeriveOperation), and until now it was GUI-only: a script
    // had to supply top_z/bottom_z by hand, which is exactly the failure mode
    // the panel stopped allowing. An agent should not have a worse interface
    // than a person.
    const bool derive =
        m == "cam_derive_op" || !p.contains("top_z") || !p.contains("bottom_z");
    json notes = json::array(); // what the derivation had to say
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
      // An op cut at several indices is derived where its first target is
      // cut (cam::IndexFrameFor), not in the setup's own frame.
      // An engraving's pick is a sketch's curves, resolved from the part
      // documents (docs/cam/operations.md#engrave).
      const std::vector<cam::SketchCurves> curves =
          ui::JobSketchCurves(state, setup, {picked});
      const cam::Setup frame = cam::IndexFrameFor(
          setup, ui::JobBodies(state), type, picked,
          p.contains("index_a_deg")
              ? p.at("index_a_deg").get<std::vector<double>>()
              : std::vector<double>{},
          curves);
      const cam::DerivedOp derived = cam::DeriveOperation(
          frame, ui::JobBodies(state), type, picked, op.toolNumber,
          cam::EarlierSetups(job->GetSetups(), setup.id), curves);
      if (!derived.ok)
        throw std::runtime_error(derived.why);
      for (const std::string& n : derived.notes)
        notes.push_back(n);
      const int tool = op.toolNumber;
      op = derived.op;
      op.toolNumber = tool;
    }
    // Anything stated explicitly still wins — the derivation is a starting
    // point, not a cage. (To force a strategy on a derived operation, set it
    // afterwards with set_cam_op.)
    ApplyOpFields(state, setup, op, p, /*keepType=*/derive);
    // The derivation read the first target in the setup's own frame; a
    // derived top is the top of what the earlier setups left over ALL of
    // them, and for an op cut at several indices, in each angle's frame.
    if (derive && !p.contains("top_z") &&
        (op.targets.size() > 1 || cam::CutAtIndices(op)) &&
        cam::TopFromStock(op.type)) {
      glm::dvec3 lo(0.0), hi(0.0);
      if (cam::StockBounds(setup, ui::JobBodies(state), lo, hi)) {
        const double top = cam::StockTopOver(
            setup, cam::EarlierSetups(job->GetSetups(), setup.id),
            ui::JobBodies(state), op, hi.z);
        if (top > op.bottomZ)
          op.topZ = top;
      }
    }
    // A clearing operation clears INSIDE its boundary; the struct's Outside
    // default is a profile's notion and means "nothing to cut" here.
    if (!p.contains("side") && !derive &&
        cam::OpTypeHas(op.type, cam::OpField::ClearsInside))
      op.side = cam::Side::Inside;
    if (op.name.empty())
      op.name = std::string(TypeName(op.type)) + " " +
                std::to_string(setup.operations.size() + 1);
    json warnings = ReachWarnings(setup, op);
    for (const json& n : notes)
      warnings.push_back(n);
    if (m == "cam_derive_op") {
      json out{{"derived", derive}, {"operation", OpJson(op)}};
      if (!warnings.empty())
        out["warnings"] = warnings;
      return out;
    }
    op.id = job->AllocateId();
    setup.operations.push_back(op);
    json out = OpJson(op);
    out["derived"] = derive;
    if (!warnings.empty())
      out["warnings"] = warnings;
    return out;
  }

  if (m == "set_cam_op") {
    // `job` is read in the preamble, before the active job is looked up --
    // one place for every branch, and invisible to the scanner from here.
    // rpc-params: job
    // The add_cam_op fields, all of them through ApplyOpFields (above), which
    // the params scanner follows.
    auto [setup, op] = FindOp(*job, p.at("id").get<uint64_t>());
    if (op == nullptr)
      throw std::runtime_error("no CAM operation with that id");
    // On a copy, so a refusal leaves the op as it was.
    cam::Operation next = *op;
    ApplyOpFields(state, *setup, next, p);
    const json warnings = ReachWarnings(*setup, next);
    *op = next;
    json out{{"id", op->id}};
    if (!warnings.empty())
      out["warnings"] = warnings;
    return out;
  }

  if (m == "move_cam_op") {
    // `job` is read in the preamble, before the active job is looked up --
    // one place for every branch, and invisible to the scanner from here.
    // rpc-params: job
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
    // `job` is read in the preamble, before the active job is looked up --
    // one place for every branch, and invisible to the scanner from here.
    // rpc-params: job
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
    // `job` is read in the preamble, before the active job is looked up --
    // one place for every branch, and invisible to the scanner from here.
    // rpc-params: job, resolution, setup
    // Synchronous here: the pass in flight is joined and the rest computed
    // now, through the same cache the panel fills
    // (docs/ui/long-operations.md#cam-toolpath-passes). A client's request
    // is PARKED first (ParkCamRequest) and runs these steps itself, the
    // simulation on a worker; it reaches this handler only when parking
    // gave up (docs/cam/README.md#requests-that-wait).
    const std::shared_ptr<VerifyRun> w = PrepareVerify(state, *job, p);
    ComputeVerify(*w);
    return VerifyAnswer(state, *w);
  }
  if (m == "cam_simulate") {
    // `job` is read in the preamble, before the active job is looked up --
    // one place for every branch, and invisible to the scanner from here.
    // rpc-params: job
    if (p.value("close", false)) {
      // `setup` is read even here, and ignored: naming the setup you are
      // closing is the natural thing to write, and refusing it with
      // "unknown parameter 'setup'; accepted: close" reads as though the
      // method has no such parameter at all.
      (void)p.value("setup", uint64_t{0});
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
      // `settle`: the earlier setups this one starts from are generated
      // now, too -- a reader waits for the answer, it does not get a
      // refusal to come back later.
      if (!ui::BeginCamPlayback(state, /*settle=*/true))
        throw std::runtime_error(state.camPlaybackRefusal);
      // Framed ONCE, as the simulation opens: an instance driven entirely
      // over the RPC has a camera nobody ever pointed, and its first
      // screenshot of a simulation was a close-up of the stock. The camera's
      // direction is kept, and a later seek leaves the view alone, so a
      // caller who set one keeps it.
      ui::FitCameraToDocument(state);
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
             // From whichever stock is armed: a rotary program is carved
             // into a radius field, and reading the (empty) height map would
             // report nothing removed while the part turned.
             {"removed_mm3", state.camTimeline.rotary
                                 ? state.camRotarySim.RemovedVolume()
                                 : state.camStockSim.RemovedVolume()},
             {"rotary", state.camTimeline.rotary},
             {"a_deg", state.camTimeline.AngleAt(state.camCursor)},
             {"resolution_mm", state.camSimRes},
             // What the stock started from: the earlier setups whose cut it
             // was seeded with (empty = the fresh stock), and any that could
             // not be read.
             {"starts_from", state.camPlaybackSeededFrom},
             {"segments", tl.Segments()},
             {"position", {pos.x, pos.y, pos.z}}};
    if (!state.camPlaybackNotSeeded.empty())
      out["earlier_setups_not_read"] = state.camPlaybackNotSeeded;
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
      // Defer the slow passes to the worker and answer with what is
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
      // Every queue the worker drains, not only the adaptive one: a
      // surfacing op or a profile's sections is a pass owed the same way.
      const size_t queued =
          state.camOutlineQueue.size() + (state.camOutlineInFlight ? 1 : 0) +
          state.camAdaptiveQueue.size() + (state.camAdaptiveInFlight ? 1 : 0) +
          state.camSurfaceQueue.size() + (state.camSurfaceInFlight ? 1 : 0);
      return json{
          {"pending", pending}, {"queued_passes", queued}, {"job", "cam"}};
    }
  }

  if (m == "cam_toolpath" || m == "cam_post") {
    // `job` is read in the preamble, before the active job is looked up --
    // one place for every branch, and invisible to the scanner from here.
    // rpc-params: job
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
      // A PHYSICAL tolerance. This was 1e-6 mm -- a nanometre -- against a
      // bound derived from float32 geometry, whose error on a 25 mm part is
      // about 2 nm, so a top_z typed to exactly the stock top tripped it and
      // the message then read "top_z 25 is below the stock top (Z25)" because
      // %.4g prints the difference it is complaining about as nothing. A
      // micron is still a thousand times finer than any router cares about
      // (docs/cam/README.md#invariants).
      constexpr double kTopSlack = 1e-3;
      // The stock top AS IT STANDS when each op runs: a face op before it in
      // the cutting order has already taken the top down to its own bottom.
      // Against the untouched top, every op after a skim that starts at the
      // part's top -- an edge break, most -- was warned about rapids through
      // stock that was no longer there.
      double top = stockHi.z;
      // And as the job's EARLIER setups leave it: over an indexed op after a
      // turning, the top is the turned part's, not the bar's.
      const std::vector<const cam::Setup*> earlier =
          cam::EarlierSetups(job->GetSetups(), setup.id);
      for (const size_t i : cam::CuttingOrder(setup)) {
        const cam::Operation& op = setup.operations[i];
        if (!op.enabled)
          continue;
        const double here =
            earlier.empty() ? top
                            : cam::StockTopOver(setup, earlier,
                                                ui::JobBodies(state), op, top);
        // An engraving's top is the surface its sketch lies on, not a
        // choice: typing it higher changes nothing, so the remedy is the
        // stock's (docs/cam/operations.md#engrave).
        if (op.type == cam::OpType::Engrave && op.topZ < here - kTopSlack)
          warnings.push_back(
              "op " + std::to_string(op.id) + " '" + op.name +
              "': the surface it engraves is Z" + zfmt(op.topZ) + ", " +
              zfmt(here - op.topZ) + " mm under the stock top (Z" + zfmt(here) +
              ") -- the hops between its strokes pass through that stock; "
              "face it down to the surface in an operation before this "
              "one");
        // A thread mill enters down its bore at the elevator, a rapid only
        // to the stock top (docs/cam/operations.md#thread-mill): its top_z is
        // the helix's, and whether the bore is open is the verifier's
        // bore_standing_mm.
        else if (op.type != cam::OpType::ThreadMill &&
                 op.topZ < here - kTopSlack)
          warnings.push_back(
              "op " + std::to_string(op.id) + " '" + op.name + "': top_z " +
              zfmt(op.topZ) + " is " + zfmt(here - op.topZ) +
              " mm below the stock top (Z" + zfmt(here) +
              (here < stockHi.z - kTopSlack
                   ? (here < top - kTopSlack ? ", as the earlier setups "
                                               "leave it over this op"
                                             : ", as the face op before it "
                                               "leaves it")
                   : "") +
              ") — entry rapids will pass through stock; raise top_z or "
              "leave it out to derive");
        if (op.type == cam::OpType::Face)
          top = std::min(top, op.bottomZ);
      }
    }

    json ops = json::array();
    json toolpathExtra = json::object();
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
      if (tp.holdingClippedMm > 0.0)
        oj["holding_clipped_mm"] = tp.holdingClippedMm;
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
    // ...and the sheet's own: parts too close together for the cutter that
    // goes between them. check_layout says the same thing, and this is where
    // it is unavoidable.
    for (const std::string& line :
         cam::LayoutWarnings(setup, ui::JobBodies(state)))
      warnings.push_back(line);
    // Does this cutter, at these numbers, actually cut? The app has the
    // diameter, the flutes, the feed, the spindle and the body's material,
    // and used to say nothing while a job posted at a third of the chip load
    // it needed.
    // `unchecked` is the other half: an empty answer used to mean both
    // "fine" and "nobody looked", which are not the same answer.
    const cam::FeedReport feeds = cam::FeedsFor(setup, ui::JobBodies(state));
    json feedsOut = json::object();
    if (!feeds.findings.empty()) {
      json list = json::array();
      for (const cam::Finding& f : feeds.findings) {
        warnings.push_back(f.text);
        json fj = {{"tool", f.tool}, {"source", f.source}, {"says", f.text}};
        // The arithmetic beside the sentence, so a caller can compare
        // numbers rather than parse prose -- and argue with the limit.
        if (f.measured > 0.0) {
          fj["chipload_mm_per_tooth"] = f.measured;
          fj["band_min"] = f.bandMin;
          fj["band_max"] = f.bandMax;
        }
        list.push_back(std::move(fj));
      }
      feedsOut["findings"] = std::move(list);
    }
    // `unchecked` gets its OWN channel and stays OUT of `warnings`. Both
    // halves have to be visible -- an empty answer must not read as "fine" --
    // but they are different claims: a warning says something is wrong with
    // this job, and this says nobody looked. Conflating them makes every job
    // on an unlabelled body warn, and a verdict that cries wolf is one people
    // learn to scroll past (which is the lesson the chip-load wording already
    // cost us). The panel shows it as its own line; `feeds.unchecked` is
    // where a script reads it.
    if (!feeds.unchecked.empty())
      feedsOut["unchecked"] = feeds.unchecked;
    if (!feedsOut.empty())
      toolpathExtra["feeds"] = std::move(feedsOut);

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
    for (auto& [k, v] : toolpathExtra.items())
      toolpathOut[k] = v;
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
             (base.stem().string() + "-T" + std::to_string(f.tool) +
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
                 // The panel's cycle time, which the RPC never had: a turning
                 // cut is timed the way the controller reads its scaled F,
                 // capped moves included (CamGcode.h).
                 {"cutting_minutes", cam::CuttingMinutes(setup, paths)},
                 {"min_z", lowest},
                 // Surfaced, not buried: a job posted with a failed op is a
                 // part that comes off the machine unfinished.
                 {"failed", failed}};
    if (totalClipped > 0.0)
      postOut["clipped_mm"] = totalClipped;
    for (auto& [k, v] : toolpathExtra.items())
      postOut[k] = v;
    // cam_post never carried these at all, so the one call that WRITES the
    // file was the one that said least about it.
    if (!warnings.empty())
      postOut["warnings"] = warnings;
    return postOut;
  }

  return std::nullopt;
}

// --- Requests that wait (docs/cam/README.md#requests-that-wait) -----------

namespace {

// Rounds of "the worker went idle and passes were STILL owed" before a
// parked request stops waiting and runs synchronously. Owed again after
// idle means the job moved on under it (another client edited it) or the
// cache let a pass go; either way it converges, and the bound only keeps a
// cache too small for the whole job from cycling for ever. The fallback is
// what every one of these requests did before parking existed.
constexpr int kMaxPassRounds = 4;

// Queue every pass `m` reads; true while any is owed. Throws the handler's
// refusals, which ParkCamRequest turns into "dispatch it and let the
// handler say it".
bool PassesOwed(ui::AppState& state, const std::string& m, const RpcParams& p) {
  SelectJob(state, p);
  cam::JobDocument* job = ui::ActiveJob(state);
  if (job == nullptr)
    throw std::runtime_error("no job");
  std::vector<const cam::Setup*> read;
  if (m == "cam_simulate") {
    const uint64_t id = p.contains("setup") ? p.at("setup").get<uint64_t>()
                                            : state.camActiveSetup;
    const cam::Setup& setup = RequireSetup(*job, id);
    // Only a simulation that (re)opens settles the toolpaths; a seek on the
    // one already playing reads nothing new.
    if (state.camPlaybackOpen && state.camActiveSetup == setup.id &&
        !p.contains("resolution"))
      return false;
    read.push_back(&setup);
    // ...and the setups before it, whose files its stock starts from
    // (docs/cam/simulation.md#leftover-another-setup-removes).
    for (const cam::Setup& other : job->GetSetups()) {
      if (other.id == setup.id)
        break;
      if (!other.operations.empty())
        read.push_back(&other);
    }
  } else {
    const cam::Setup& setup = RequireSetup(*job, p.at("setup").get<uint64_t>());
    read.push_back(&setup);
    // cam_verify posts every other setup of the job too
    // (docs/cam/simulation.md#leftover-another-setup-removes).
    if (m == "cam_verify")
      for (const cam::Setup& other : job->GetSetups())
        if (other.id != setup.id && !other.operations.empty())
          read.push_back(&other);
  }
  bool owed = false;
  for (const cam::Setup* setup : read)
    owed |= ui::QueueCamPassesFor(state, *setup);
  return owed;
}

// cam_simulate, once its passes are cached: whether the stock it starts
// from -- what the earlier setups left, the rotary carve of a turning's
// whole file -- is still being read on a worker. Starts that read.
bool SimulateStartReading(ui::AppState& state, const RpcParams& p) {
  SelectJob(state, p);
  const uint64_t id = p.contains("setup") ? p.at("setup").get<uint64_t>()
                                          : state.camActiveSetup;
  if (state.camPlaybackOpen && state.camActiveSetup == id &&
      !p.contains("resolution"))
    return false; // a seek on the one playing
  // The grid the handler will arm at, clamped as it clamps it.
  const double res =
      p.contains("resolution")
          ? std::clamp(p.at("resolution").get<double>(), 0.05, 5.0)
          : state.camSimRes;
  return ui::CamPlaybackStartReading(state, id, res);
}

class CamWait final : public CamParking {
public:
  explicit CamWait(std::string method)
      : m_method(std::move(method)),
        m_cancellations(ui::CamPassCancellations()) {}

  // cam_verify's simulation, on a "cam-verify" worker. Throws a refusal
  // from PrepareVerify (a setup that does not post, say).
  void StartVerify(ui::AppState& state, const RpcParams& p) {
    SelectJob(state, p);
    cam::JobDocument* job = ui::ActiveJob(state);
    if (job == nullptr)
      throw std::runtime_error("no job");
    std::shared_ptr<VerifyRun> w = PrepareVerify(state, *job, p);
    auto bg = std::make_unique<ui::BackgroundJob>();
    bg->kind = "cam-verify";
    bg->label = "Verifying setup '" + w->setup.name + "' for cam_verify";
    bg->cancellable = false; // the carve has no interruption points
    bg->fraction.store(-1.0);
    ui::BackgroundJob* raw = bg.get();
    bg->thread = std::thread([w, raw] {
      try {
        ComputeVerify(*w);
      } catch (const std::exception& e) {
        w->error = std::string("cam_verify failed: ") + e.what();
      } catch (const Standard_Failure& e) {
        w->error = std::string("cam_verify failed: OpenCASCADE error: ") +
                   (e.GetMessageString() ? e.GetMessageString()
                                         : "unspecified failure");
      }
      raw->done.store(true); // last
    });
    bg->finish = [w, raw](ui::AppState&) -> bool {
      w->landed = true;
      w->cancelled = raw->cancel.load();
      return true;
    };
    ui::StartBackgroundJob(state, std::move(bg));
    m_verify = std::move(w);
  }

  // cam_simulate, parked for its start's read alone.
  void Reading() { m_reading = true; }

  Next Poll(ui::AppState& state, const RpcParams& p, json& answer) override {
    if (m_verify) {
      if (!m_verify->landed)
        return Next::Wait;
      if (m_verify->cancelled)
        throw std::runtime_error(
            "cam_verify's simulation was cancelled (cancel_background_job "
            "cam-verify) before it answered -- call cam_verify again");
      if (!m_verify->error.empty())
        throw std::runtime_error(m_verify->error);
      answer = VerifyAnswer(state, *m_verify);
      return Next::Answer;
    }
    if (m_reading) {
      // Reading what the earlier setups left: nothing else to wait for.
      try {
        return SimulateStartReading(state, p) ? Next::Wait : Next::Dispatch;
      } catch (...) {
        return Next::Dispatch;
      }
    }
    // A cancel drops the queue; the passes this request waits on will not
    // land, and waiting on would only queue them again behind the caller's
    // back.
    if (ui::CamPassCancellations() != m_cancellations)
      throw std::runtime_error(
          m_method +
          " was waiting for toolpath passes and they were cancelled "
          "(cancel_background_job cam) -- call it again to cut them");
    if (ui::CamPassesBusy(state))
      return Next::Wait;
    bool owed = false;
    try {
      owed = PassesOwed(state, m_method, p);
    } catch (...) {
      return Next::Dispatch; // the handler refuses it, in its own words
    }
    if (owed && ++m_rounds < kMaxPassRounds)
      return Next::Wait;
    // The stock a simulation starts from is read off the frame too, even
    // after the passes stopped converging: that read is the whole carve of
    // a turning's file.
    if (m_method == "cam_simulate") {
      try {
        if (SimulateStartReading(state, p)) {
          m_reading = true;
          return Next::Wait;
        }
      } catch (...) {
        return Next::Dispatch;
      }
    }
    if (owed || m_method != "cam_verify")
      return Next::Dispatch;
    try {
      StartVerify(state, p);
    } catch (...) {
      return Next::Dispatch;
    }
    return Next::Wait;
  }

private:
  std::string m_method;
  uint64_t m_cancellations = 0;
  int m_rounds = 0;
  bool m_reading = false; // cam_simulate: its start is being read
  std::shared_ptr<VerifyRun> m_verify;
};

} // namespace

std::unique_ptr<CamParking>
ParkCamRequest(ui::AppState& state, const std::string& m, const RpcParams& p) {
  if (m != "cam_toolpath" && m != "cam_post" && m != "cam_verify" &&
      m != "cam_simulate")
    return nullptr;
  try {
    // The poller's form answers at once, and closing a simulation reads
    // nothing: neither waits.
    if (m == "cam_toolpath" && p.value("async", false))
      return nullptr;
    if (m == "cam_simulate" && p.value("close", false))
      return nullptr;
    auto wait = std::make_unique<CamWait>(m);
    // Ready means every pass cached AND the worker idle: the synchronous
    // handler joins every "cam" job, so a pass cutting for somebody else
    // would stall it just the same.
    if (!PassesOwed(state, m, p) && !ui::CamPassesBusy(state)) {
      if (m == "cam_simulate") {
        if (!SimulateStartReading(state, p))
          return nullptr;
        wait->Reading();
        return wait;
      }
      if (m != "cam_verify")
        return nullptr;
      wait->StartVerify(state, p);
    }
    return wait;
  } catch (...) {
    // Whatever went wrong -- no job, no such setup, a parameter of the
    // wrong type, a setup that does not post -- the handler says it.
    return nullptr;
  }
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
std::unique_ptr<CamParking> ParkCamRequest(ui::AppState&, const std::string&,
                                           const RpcParams&) {
  return nullptr;
}
} // namespace remote

#endif // HAVE_CAM
