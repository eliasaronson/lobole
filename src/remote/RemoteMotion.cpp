// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "remote/RemoteMotion.h"

#include "core/Angles.h"
#include "remote/RemoteHelpers.h"

#include "core/AssemblySolve.h"
#include "core/ContactFollower.h"
#include "core/Conventions.h"
#include "core/FollowerCheck.h"
#include "core/InputHash.h"
#include "core/MassProps.h"
#include "core/MotionStudy.h"
#include "core/Occurrence.h"
#include "core/features/CamProfileFeature.h"
#include "core/io/Serialization.h"
#include "remote/RemotePrint.h"
#include "ui/AppState.h"
#include "ui/AssemblyActions.h"
#include "ui/BackgroundJobs.h"
#include "ui/MotionActions.h"
#include "ui/SequencePlayback.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <span>

using json = nlohmann::json;

namespace remote {
namespace {

// A motion study's from/to speak the JOINT's unit: DEGREES on the wire for a
// rotational dof, millimetres for a sliding one. The wire says degrees so the
// caller never has to know which kind the joint is; storage is radians either
// way, and MotionSample::value is documented "radians or mm" for that reason.
double RotAwareAngle(double wire, bool rot) {
  return rot ? angles::ToRadians(wire) : wire;
}

// describe: one row per `m ==` branch below — extend together.
constexpr MethodDoc MotionMethods[] = {
    {"add_motion_study",
     "Define a motion study on the active assembly: ONE mate's degree of "
     "freedom driven over a range, the assembly solved at every sample, and "
     "every occurrence pair checked for a clash. {driver: mate id, "
     "driver_path? ([occurrence id] of a FLEXIBLE sub-assembly placement: "
     "`driver` is then a mate of that sub-assembly, by its own id — an "
     "engine turning its pump's camshaft), dof?: "
     "rotation|offset (inferred from a revolute or slider; a cylindrical "
     "mate must say which), from, to — DEGREES for a rotation, mm for an "
     "offset — samples? (default 48), check_clash? (default true), "
     "clash_mode?: mesh (default, ~10x faster) | exact (the boolean), "
     "clash_deflection_mm? (mesh only: the tessellation the mesh check "
     "decides on, default 0.2% of each body's bounding diagonal — set it "
     "under the smallest running gap, or a gap finer than the triangles "
     "reads either way), "
     "configuration? (an assembly configuration id the study runs, is "
     "judged and is scrubbed UNDER, whatever is active; 0 = the base, with "
     "nothing held or suppressed; omitted = whichever is active when it "
     "runs — so a study that measures one load or one variant should name "
     "it), max_refine? (how many times a step may be halved where a free "
     "joint jumps between samples, 0..12, default 6: down to a 64th of the "
     "step; docs/assemblies/README.md#branch-jumps), name?}. Returns the "
     "study and, if it cannot run, `why_not`. Nothing "
     "runs until run_motion_study. ONE driver moves the whole mechanism "
     "when the rest follows through mates and RELATIONS (add_relation): a "
     "geared pair turns because the relation propagates, not because two "
     "DOFs were driven. Pick the driver with care where a mechanism has a "
     "SINGULAR configuration — a symmetric linkage that can also move "
     "antisymmetrically passes through one — because the solve continues "
     "from the previous sample and can come out of it on the other branch "
     "(reported as a branch_jump, below). "
     "The run watches every free joint: a step whose joint RATES change "
     "too much is re-solved at half the step, down to max_refine halvings, "
     "so a step too coarse for its seed no longer lands on a mirror branch; "
     "a jump that stays at the finest step is a singular pose, reported as "
     "the sample's `branch_jump`, and the study does not pass.",
     "check_clash, clash_deflection_mm, clash_mode, configuration, dof, "
     "driver, driver_path, from, max_refine, name, samples, to"},
    {"set_motion_study",
     "Edit a study {study, name?, from?, to?, samples?, check_clash?, "
     "clash_mode?, clash_deflection_mm? (mm, null = derived per body "
     "again), configuration? (an id, 0 = the base, null = whichever "
     "is active), max_refine? (0..12)}; a stored "
     "result is dropped, since it described the old definition.",
     "check_clash, clash_deflection_mm, clash_mode, configuration, from, "
     "max_refine, name, samples, study, to"},
    {"remove_motion_study", "Remove a study by id.", "study"},
    {"list_motion_studies",
     "Every study, with whether it has a result and whether one is running.",
     "", remote::NoFlags,
     "studies:{check_clash:bool, clash_mode:string, driver_dof:string, "
     "driver_mate:int, from:number, has_result:bool, id:int, "
     "max_refine:int, name:string, running:bool, samples:int, to:number, "
     "type:string, "
     "unit:string, clean?:bool, collision_checked?:bool, stale?:bool, "
     "clash_deflection_mm?:number, configuration?:int, "
     "why_not?:string}[]"},
    {"run_motion_study",
     "Run it: {study, async?, include_poses?, leave_at?, leave_at_deg?, "
     "leave_at_mm?, recompute?, outlive_client?}. A run whose INPUTS hash "
     "the same as a stored result -- this session's, or the one a saved "
     "project carries -- (the study's definition, "
     "every occurrence and its placement, every mate and its current "
     "value, relations, configurations, and the placed parts' exact "
     "geometry, materials and size bands) is answered from it, `reused: "
     "true`, nothing "
     "computed; `input_hash` names the inputs; `recompute: true` runs "
     "anyway. Synchronous by default and "
     "returns the COLLISION TABLE — per sample: the driver value, whether "
     "it solved, the loop residual, and the clashing pairs (each with "
     "`rigid`, and under an EXACT check its `volume_mm3`, `centroid_mm` and "
     "`bbox_min_mm`/`bbox_max_mm` — the overlap's centre and world bounds "
     "in mm, which is how a pair of names becomes something to act on; a "
     "mesh check knows only THAT, and gives `within_min_mm`/`within_max_mm`, "
     "the region the two bodies' boxes share, rather than a zero that reads "
     "like a measurement). Pairs RIGID to each other over the whole study "
     "(fastened, or carried together) cannot be made to collide by the "
     "motion, so they are checked ONCE, exactly whatever clash_mode says, "
     "at `rigid_checked_at_sample` (the first sample), and skipped after "
     "(`pairs_rigid` counts the skips, `pairs_rigid_checked` the checks); a "
     "clash there is a STANDING clash, marked `rigid: true` and listed in "
     "`rigid_clashes`, and the study is not clean. Plus "
     "`clean`, `seated_contacts` and `appeared` (what TURNING changed: a "
     "mesh check cannot relieve a thread or a press fit, so the pairs "
     "already clashing at the first sample are counted and the rest are "
     "listed). A mesh check calls a pair that meets only on the surface "
     "-- a face seated on a face, no triangle piercing another -- `touching`, "
     "listed per sample {a, b, a_name, b_name}, not a clash, as an exact "
     "check calls it touching. `worst_sample`, "
     "`table` (the same as text, for a report), "
     "`deflection_mm` for a mesh check (the coarsest tessellation it "
     "decided on: the study's clash_deflection_mm when it sets one), and "
     "`pairs_checked`. An exact "
     "check relieves an engaged thread, a ring in its groove and a PRESS "
     "FIT (two coaxial round faces whose size bands allow the overlap, "
     "docs/tolerances/README.md#press-fits); `press_fits` lists each "
     "relieved pair once {a, b, a_name, b_name, fit, interference_mm, "
     "max_interference_mm, volume_mm3}, and a mesh run says it relieves "
     "none (`relieves`) except on rigid pairs. "
     "Every free joint is watched between samples "
     "(docs/assemblies/README.md#branch-jumps): `refined_samples` counts the "
     "samples whose step was re-solved finer (each marked `refined`), and a "
     "sample that still jumped at the finest step carries `branch_jump` "
     "{mate, name, path?, dof, jump_deg|jump_mm (its move over that step), "
     "expected_deg|expected_mm (what its rate gave), at_deg|at_mm, "
     "step_deg|step_mm}: the linkage passes a singular pose there, "
     "`branch_jumps` counts them, and the study is not clean. "
     "include_poses adds, per sample, every occurrence's placement "
     "(`poses`: position mm, orientation quaternion [x,y,z,w]) and every "
     "moving mate's value (`mates`: {mate, name, rotation_deg, offset} — "
     "what the study actually turned and slid, so no angle has to be "
     "recovered from a quaternion). Inside a FLEXIBLE sub-assembly "
     "placement each moving occurrence is a pose of its own keyed by "
     "`path` [placement, occurrence] (no `occurrence` key: that id is the "
     "sub-assembly's), each of its mates carries `path` [placement], and "
     "clashes are checked between its leaves — a plunger against its "
     "barrel — with `a_path`/`b_path` naming which. A study that names a "
     "configuration runs "
     "under it. "
     "{async: true} runs it as a background JOB and answers `job` (its id: "
     "study_status {job} reports it, cancel_study {job} stops it, and "
     "motion_status {study} reads the result when it lands). The job is "
     "cancelled when the connection that started it closes, unless "
     "`outlive_client: true` -- a script that starts a run and exits "
     "without it has stopped the run. A "
     "sample that does not solve is reported unsolved with its residual — "
     "never drawn in the last good pose's place. The run happens on a COPY, "
     "so the assembly is left where it was; `leave_at` (\"last\", "
     "\"start\", or a sample INDEX) poses it at one of the samples "
     "afterwards, which is how anything about the far end of the travel — a "
     "section, a mass property, a measurement — can be asked at all: the "
     "placements and mate values the run recorded there, and ONE solve "
     "(with unchanged inputs the run itself is reused, so asking again with "
     "leave_at costs that solve and nothing more). A pose left this way is "
     "on the run's own path, so the result stays the answer from there: "
     "the next run_motion_study {leave_at} is reused too and the result is "
     "not stale, while a scrub, a drag or any other edit is an input again. "
     "To leave it at a driver "
     "VALUE instead, `leave_at_deg` (a rotation study, degrees) or "
     "`leave_at_mm` (an offset study, millimetres), within the study's "
     "range: the nearest sample's pose and one solve stepping from it, so a "
     "slider-crank stays on the branch the run found. The reply says "
     "`left_at` (the sample), `left_at_value` and `left_solves`. leave_at "
     "with async is refused: its samples do not exist yet. "
     "`trace` [{name, occurrence (or path [placement, occurrence] inside a "
     "flexible placement), point_mm}] follows each point -- given in the "
     "occurrence's own part coordinates, mm -- through every sample: the "
     "reply's `trace` gives each its `points_mm` (world mm, one per "
     "sample, null where the sample did not solve -- never the last good "
     "pose), `min_mm`/`max_mm`/`extent_mm` over the solved samples, and "
     "`unsolved_samples`. A foot's path, a coupler point, a follower's tip "
     "(docs/assemblies/README.md#tracing-a-point). An unknown occurrence, "
     "an empty or repeated name, or an inner occurrence that does not move "
     "is refused naming what there is; trace with async is refused (ask "
     "motion_status {study, trace} once it lands).",
     "study:int>=0, async?:bool, include_poses?:bool, leave_at?:string|int, "
     "leave_at_deg?:number, leave_at_mm?:number, outlive_client?:bool, "
     "recompute?:bool, trace?:object[]"},
    {"motion_status",
     "{study}: whether a run of THIS study is in progress (with its `job` "
     "id and `fraction` while it is), and the stored result if one has "
     "landed (the same shape run_motion_study returns, `trace` included "
     "when asked: {study, trace}).",
     "include_poses, study, trace"},
    {"check_followers",
     "Whether each follower stays on its cam, over a motion study's STORED "
     "run (run_motion_study first; a stale or missing run is refused) "
     "(docs/assemblies/relations.md#the-follower-check). {study, "
     "relations?: [contact relation ids] (omitted = every one), "
     "speed_rpm? (the study driver's speed, rpm), max_pressure_angle_deg? "
     "(the limit; omitted = unchecked)}. Per contact relation: `touching` "
     "(at every sample the follower enters no cut across the shared width "
     "by more than the contact tolerance — `clash_mm`|`clash_deg` the "
     "deepest — and stands where the geometry puts it, `gap_mm`|`gap_deg`; "
     "the samples where the study itself listed the pair as a clash), "
     "`pressure_angle` {max_deg, at_cam_deg, limit_deg?, ok? | unchecked}, "
     "`curvature` {min_cam_convex_mm, min_cam_concave_mm, "
     "follower_radius_mm, bridges_deg}, for a cam cut by a CamProfile "
     "`deviation_mm`|`deviation_deg` (the realised motion against the "
     "designed, a constant mate zero removed; drops' corners left out) with "
     "the profile's `fit_deviation_mm`, and `gravity` from the masses that "
     "move with the follower (materials or printed mass; refused naming an "
     "occurrence with none): `liftoff_rpm` (the driver speed above which "
     "gravity can no longer hold it on; null = never; guide friction not "
     "modelled, so an upper bound), `held` and `not_held_deg` (where its "
     "weight pushes it off at any speed), and per drop {at_cam_deg, "
     "height_mm|height_deg, fall_s, dwell_deg, drop_rpm (the speed it just "
     "lands within its dwell), turn_deg and lands at speed_rpm, jams (a step "
     "met from below: the cam turns against the way it was cut)}; "
     "`holds_at_speed` with a speed.",
     "study:int>=0, relations?:int[], speed_rpm?:number>0, "
     "max_pressure_angle_deg?:number>0",
     remote::NoFlags, "study:int, speed_rpm?:number, relations:object[]"},
    {"set_motion_pose",
     "Scrub: drive the study's mate to {t} (0..1 along the range) and "
     "re-solve, so the viewport and every query show that pose. No undo "
     "step, like the explode slider — a scrub is a look, not an edit — "
     "EXCEPT that a study naming a configuration first activates it (an "
     "undo step), since its poses only exist under it.",
     "study, t", remote::NotInBatch},
    {"export_motion",
     "Write the run as a GLB with a glTF animation {study, path}: one node "
     "per occurrence (and per moving occurrence inside a flexible "
     "placement), translation + rotation keyframes at every sample, "
     "24 frames per second, in METRES and Y-up as glTF defines (one root "
     "node carries both). Refuses without a run, and refuses when any "
     "sample is unsolved.",
     "path, study"},
    {"export_motion_video",
     "Film the study: {study, path, format?, width?, height?, fps?, frames?, "
     "quality?, async?}. It plays as the Motion window's Play does — the "
     "run's poses at 24 samples per second, interpolated between samples — "
     "and each frame is the VIEWPORT's own render of the model (its "
     "materials, shadows, ambient occlusion, outlines and anti-aliasing; no "
     "sketches, gizmos, selection or panels) from the CURRENT camera, at "
     "width x height PIXELS — so set the camera first. It needs no run: "
     "without a fresh one it makes its own, collision-free, and does not "
     "store it. `format`: mp4 (H.264), webm (VP9) or gif (one palette for "
     "the whole animation) — encoded by ffmpeg, which must be on PATH "
     "(ping's features.ffmpeg says) and is REFUSED by name when it is not — "
     "or png, numbered frames written into a FOLDER (`path`), needing "
     "nothing. Omitted, the format is the path's extension; a format and an "
     "extension that disagree are refused. Defaults: width and height the "
     "viewport's (pixels; mp4/webm need both even), `fps` (frames per "
     "SECOND) 60, `frames` as many as Play takes at that rate ((samples - "
     "1) / 24 seconds), `quality` medium (low|medium|high: x264 CRF "
     "23/18/14, VP9 CRF 36/30/24, GIF 64/128/256 colours; not png). The "
     "assembly is not moved. Synchronous by default, answering {path, "
     "format, frames, width, height, fps, bytes, seconds, warnings?}, and "
     "refusing with ffmpeg's own last lines when it fails; {async: true} "
     "runs it as a background job ('motion-video' in background_jobs, "
     "cancellable) and motion_video_status reads the outcome. `speed` and "
     "`highlight` are export_assembly_video's and are refused here.",
     "async, format, fps, frames, height, highlight, path, quality, speed, "
     "study, width",
     remote::NotInBatch | remote::NeedsRenderer},
    {"export_assembly_video",
     "Film the assembly SEQUENCE going together: {path, format?, width?, "
     "height?, fps?, frames?, quality?, speed?, highlight?, async?}, the "
     "request, the job "
     "('motion-video') and the answer of export_motion_video. Each step's "
     "occurrences appear at its turn, still for a moment where they start, "
     "then travel home along the path the sequence CHECK took -- its line, "
     "its way and its distance from clear, turned by the step's turn_deg "
     "or, going on by a thread, a turn a pitch -- and stay home after; a "
     "group moves as one, and a pose step turns and slides what is shown "
     "through the check's own solve of it, later parts going home to the "
     "posed seats. One "
     "speed for the whole film, from the assembly's size (its bounds' "
     "diagonal in 3 s), so its length (`frames`, null in an async answer "
     "until then) is known once the check has run, which it does inside "
     "the job (the timeline Play made is reused while the assembly is "
     "unchanged). A sequence that does not check is REFUSED, naming its "
     "first failing step (or what no step places). The assembly is not "
     "moved; the frames are the viewport's render from the current camera, "
     "width x height PIXELS, `fps` frames per SECOND (60). The step's "
     "moving parts -- travelling home, or turned by a pose -- are drawn in "
     "the selection's colour and go back to their own once home "
     "(`highlight`, default true); `speed` is the pace, a RATIO of the "
     "timeline's own (default 1; 2 films it in half the frames). Parts not "
     "yet put in are hidden, their section caps too, and a section's caps "
     "move with the parts they cut.",
     "async, format, fps, frames, height, highlight, path, quality, speed, "
     "width",
     remote::NotInBatch | remote::NeedsRenderer},
    {"motion_video_status",
     "The video export in progress, if any ({running, fraction}), and the "
     "last one to finish (`last`: the same shape export_motion_video answers, "
     "plus ok and error).",
     ""},
};

// `clash_deflection_mm`: a positive number of mm, or null to derive it per
// body again. Checked against the study's clash_mode as it will be, so
// clash_mode and the deflection can change in one call.
void ReadClashDeflection(const RpcParams& p, MotionStudy& s) {
  if (p.contains("clash_deflection_mm")) {
    const json& v = p.at("clash_deflection_mm");
    if (v.is_null()) {
      s.clashDeflectionMm = 0.0;
    } else if (!v.is_number()) {
      throw std::runtime_error(
          "clash_deflection_mm is a number of millimetres, or null to derive "
          "it from each body's size; got " +
          v.dump());
    } else {
      s.clashDeflectionMm = v.get<double>();
      if (!(s.clashDeflectionMm > 0.0))
        throw std::runtime_error(
            "clash_deflection_mm must be a positive number of millimetres "
            "(null derives it from each body's size); got " +
            v.dump());
    }
  }
  if (const std::string why = MotionStudyDeflectionProblem(s); !why.empty())
    throw std::runtime_error(why);
}

// `max_refine`: how many times a step may be halved where a joint jumps
// (docs/assemblies/README.md#branch-jumps), 0 to kMaxMaxRefine.
void ReadMaxRefine(const RpcParams& p, MotionStudy& s) {
  if (!p.contains("max_refine"))
    return;
  const json& v = p.at("max_refine");
  if (!v.is_number_integer() || v.get<int64_t>() < 0 ||
      v.get<int64_t>() > MotionStudy::kMaxMaxRefine)
    throw std::runtime_error(
        "max_refine " + v.dump() + " is not a number of halvings from 0 to " +
        std::to_string(MotionStudy::kMaxMaxRefine) + " (default " +
        std::to_string(MotionStudy::kDefaultMaxRefine) +
        ": down to a 64th of the step)");
  s.maxRefine = static_cast<int>(v.get<int64_t>());
}

json StudyJson(ui::AppState& state, const AssemblyDocument& asmDoc,
               const MotionStudy& s) {
  json j = DocumentIO::MotionStudyToJson(s);
  const bool rot = s.driverDof == MateDof::Rotation;
  // The document stores the DOF's own unit; the surface speaks degrees for
  // a rotation. Both are shown so nobody has to remember which this is.
  j["unit"] = rot ? "deg" : "mm";
  j["from"] = rot ? angles::ToDegrees(s.from) : s.from;
  j["to"] = rot ? angles::ToDegrees(s.to) : s.to;
  j["max_refine"] = s.maxRefine;
  const std::string why =
      motion::WhyNot(asmDoc, s, ui::AssemblyResolverFor(state));
  if (!why.empty())
    j["why_not"] = why;
  return j;
}

// One clash, in the vocabulary `interference` and `contact_report` use.
// A side inside a flexible placement (v129) also says its path, {root,
// occurrence}: `a`/`b` stay the top-level ids every caller keys on.
void AddSidePaths(json& j, uint64_t a, uint64_t aInner, uint64_t b,
                  uint64_t bInner) {
  if (aInner != 0)
    j["a_path"] = {a, aInner};
  if (bInner != 0)
    j["b_path"] = {b, bInner};
}

json ClashJson(const motion::Clash& c) {
  json j{{"a", c.a},
         {"b", c.b},
         {"a_name", c.aName},
         {"b_name", c.bName},
         {"rigid", c.rigid}};
  AddSidePaths(j, c.a, c.aInner, c.b, c.bInner);
  // An exact check knows how much and where; a mesh one only knows THAT, and
  // says where it COULD be rather than a zero that reads like a measurement.
  if (c.known) {
    j["volume_mm3"] = c.volume;
    AddOverlapSite(j, c.centroid, c.bboxMin, c.bboxMax);
  } else {
    AddOverlapBound(j, c.withinMin, c.withinMax);
  }
  return j;
}

// `trace: [{name, occurrence, path?, point_mm}]`: the points a run follows
// (motion::TracePoint). Read and checked against the assembly BEFORE the run,
// so a typo is refused without costing one.
std::vector<motion::TracePoint> ReadTrace(const AssemblyDocument& asmDoc,
                                          const RpcParams& p) {
  std::vector<motion::TracePoint> out;
  if (!p.contains("trace"))
    return out;
  const json& t = p.at("trace");
  if (!t.is_array())
    throw std::runtime_error(
        "trace is a list of {name, occurrence, point_mm} (or {name, path: "
        "[placement, occurrence], point_mm} inside a flexible placement); "
        "got " +
        t.dump());
  for (size_t i = 0; i < t.size(); i++) {
    const json& e = t[i];
    const std::string at = "trace " + std::to_string(i + 1) + ": ";
    if (!e.is_object())
      throw std::runtime_error(at +
                               "each traced point is {name, occurrence | "
                               "path, point_mm}; got " +
                               e.dump());
    for (const auto& [key, v] : e.items())
      if (key != "name" && key != "occurrence" && key != "path" &&
          key != "point_mm")
        throw std::runtime_error(at + "unknown key '" + key +
                                 "'; accepted: name, occurrence, path, "
                                 "point_mm");
    if (!e.contains("name") || !e.at("name").is_string())
      throw std::runtime_error(at + "name (a string) is required");
    if (!e.contains("point_mm"))
      throw std::runtime_error(at + "point_mm [x, y, z] is required: the "
                                    "point in the occurrence's own (part) "
                                    "coordinates, mm");
    if (e.contains("occurrence") == e.contains("path"))
      throw std::runtime_error(
          at + "name the point's occurrence by `occurrence` (a top-level "
               "id) or `path` ([placement, occurrence] inside a flexible "
               "placement), exactly one of them");
    motion::TracePoint tp;
    tp.name = e.at("name").get<std::string>();
    tp.pointMm = Vec3Param(e.at("point_mm"), at + "point_mm");
    if (e.contains("occurrence")) {
      if (!e.at("occurrence").is_number_unsigned())
        throw std::runtime_error(at + "occurrence is an occurrence id; got " +
                                 e.at("occurrence").dump());
      tp.occurrence = e.at("occurrence").get<uint64_t>();
    } else {
      const json& path = e.at("path");
      if (!path.is_array() || path.size() != 2 ||
          !path[0].is_number_unsigned() || !path[1].is_number_unsigned())
        throw std::runtime_error(at +
                                 "path is [placement, occurrence], two ids; "
                                 "got " +
                                 path.dump());
      tp.occurrence = path[0].get<uint64_t>();
      tp.inner = path[1].get<uint64_t>();
    }
    out.push_back(std::move(tp));
  }
  if (const std::string why = motion::WhyNotTrace(asmDoc, out); !why.empty())
    throw std::runtime_error(why);
  return out;
}

// Each traced path: a point per sample (null where it did not solve) and its
// extent over the solved ones.
json TraceJson(const std::vector<motion::TracePoint>& points,
               const std::vector<motion::TracePath>& paths) {
  json out = json::array();
  const auto vec = [](const glm::dvec3& v) { return json{v.x, v.y, v.z}; };
  for (size_t i = 0; i < paths.size(); i++) {
    const motion::TracePath& t = paths[i];
    const motion::TracePoint& p = points[i];
    json pts = json::array();
    for (const auto& q : t.points)
      pts.push_back(q ? vec(*q) : json(nullptr));
    json j{{"name", t.name},
           {"point_mm", vec(p.pointMm)},
           {"points_mm", std::move(pts)},
           {"unsolved_samples", t.unsolved}};
    if (p.inner != 0)
      j["path"] = {p.occurrence, p.inner};
    else
      j["occurrence"] = p.occurrence;
    if (t.any) {
      j["min_mm"] = vec(t.min);
      j["max_mm"] = vec(t.max);
      j["extent_mm"] = vec(t.max - t.min);
    } else {
      j["min_mm"] = nullptr;
      j["max_mm"] = nullptr;
      j["extent_mm"] = nullptr;
    }
    out.push_back(std::move(j));
  }
  return out;
}

json TraceResultJson(const std::vector<motion::TracePoint>& points,
                     const motion::Result& r) {
  std::vector<motion::TracePath> paths;
  std::string why;
  if (!motion::TracePoints(r, points, &paths, &why))
    throw std::runtime_error(why);
  return TraceJson(points, paths);
}

json ResultJson(const AssemblyDocument& asmDoc, const motion::Result& r,
                const MotionStudy& s, bool includePoses) {
  json samples = json::array();
  const bool rot = s.driverDof == MateDof::Rotation;
  for (size_t i = 0; i < r.samples.size(); i++) {
    const motion::Sample& sm = r.samples[i];
    json clashes = json::array();
    for (const motion::Clash& c : sm.clashes)
      clashes.push_back(ClashJson(c));
    json sj{{"index", i},
            {"t", sm.t},
            {"value", rot ? angles::ToDegrees(sm.value) : sm.value},
            {"solved", sm.solved},
            {"errored_mates", sm.erroredMates},
            {"residual_mm", sm.residualMm},
            {"clashes", clashes}};
    if (sm.refined)
      sj["refined"] = true;
    if (const auto& b = sm.branchJump) {
      const auto shown = [&](double v, bool r) {
        return r ? angles::ToDegrees(v) : v;
      };
      json bj{{"mate", b->mate},
              {"name", b->name},
              {"dof", b->rotation ? "rotation" : "offset"}};
      const char* unit = b->rotation ? "_deg" : "_mm";
      bj[std::string("jump") + unit] = shown(b->jump, b->rotation);
      bj[std::string("expected") + unit] = shown(b->expected, b->rotation);
      bj[rot ? "at_deg" : "at_mm"] = shown(b->atValue, rot);
      bj[rot ? "step_deg" : "step_mm"] = shown(b->stepValue, rot);
      if (!b->path.empty())
        bj["path"] = b->path;
      sj["branch_jump"] = std::move(bj);
    }
    // A mesh check's pairs that met on the surface only: seated, not
    // clashing (docs/inspect/README.md#touching-is-not-a-clash).
    sj["touching"] = json::array();
    for (const motion::Touch& t : sm.touching) {
      json tj{{"a", t.a}, {"b", t.b}, {"a_name", t.aName}, {"b_name", t.bName}};
      AddSidePaths(tj, t.a, t.aInner, t.b, t.bInner);
      sj["touching"].push_back(std::move(tj));
    }
    sj["unknown_contacts"] = json::array();
    for (const auto& c : sm.unknownContacts) {
      json uj{{"a", c.a},
              {"b", c.b},
              {"a_name", c.aName},
              {"b_name", c.bName},
              {"reason", c.reason}};
      AddSidePaths(uj, c.a, c.aInner, c.b, c.bInner);
      sj["unknown_contacts"].push_back(std::move(uj));
    }
    if (includePoses) {
      json poses = json::array();
      for (const auto& [occId, m] : sm.worlds) {
        const glm::dquat q = glm::normalize(glm::quat_cast(glm::dmat3(m)));
        poses.push_back({{"occurrence", occId},
                         {"position", {m[3].x, m[3].y, m[3].z}},
                         {"orientation", {q.x, q.y, q.z, q.w}}});
      }
      // Inside a flexible placement (v129): each moving occurrence by its
      // PATH, and no `occurrence` key — its id is the sub-assembly's and
      // would collide with this document's.
      for (const motion::Sample::LeafWorld& l : sm.leaves) {
        const glm::dmat4& m = l.world;
        const glm::dquat q = glm::normalize(glm::quat_cast(glm::dmat3(m)));
        poses.push_back({{"path", {l.root, l.occurrence}},
                         {"position", {m[3].x, m[3].y, m[3].z}},
                         {"orientation", {q.x, q.y, q.z, q.w}}});
      }
      sj["poses"] = poses;
      json mates = json::array();
      for (const motion::Sample::MateValue& v : sm.mates) {
        json mj{{"mate", v.mate}, {"name", v.name}};
        if (!v.path.empty())
          mj["path"] = v.path; // a mate of that placement's sub-assembly
        if (v.turns)
          mj["rotation_deg"] = angles::ToDegrees(v.rotation);
        if (v.slides)
          mj["offset"] = v.offset;
        mates.push_back(std::move(mj));
      }
      sj["mates"] = std::move(mates);
    }
    samples.push_back(sj);
  }
  // What TURNING changed. A mesh sweep called touching a clash, and at rest
  // a propeller touches its motor, its nut and four screws -- so a study of
  // a working machine was never "clean" and the caller had to work out
  // which contacts were news. A face seated on a face now reads touching
  // (each sample's `touching`), but a mesh check still cannot relieve a
  // thread or a press fit, so: the pairs clashing at the FIRST sample are
  // seated, and anything else that shows up appeared.
  //
  // A RIGID pair's clash is neither: the two never move relative to each
  // other, so it is a standing clash, checked once, and listed on its own.
  // Keyed by both sides' paths: two leaves of one flexible placement share
  // their top-level id.
  using Side = std::pair<uint64_t, uint64_t>;
  const auto pairKey = [](const motion::Clash& c) {
    const Side a{c.a, c.aInner}, b{c.b, c.bInner};
    return std::pair{std::min(a, b), std::max(a, b)};
  };
  std::set<std::pair<Side, Side>> seated;
  if (!r.samples.empty())
    for (const motion::Clash& c : r.samples.front().clashes)
      if (!c.rigid)
        seated.insert(pairKey(c));
  json rigidClashes = json::array();
  for (const motion::Sample& sm : r.samples)
    for (const motion::Clash& c : sm.clashes)
      if (c.rigid)
        rigidClashes.push_back(ClashJson(c));
  std::set<std::pair<Side, Side>> appeared;
  json appearedJson = json::array();
  for (const motion::Sample& sm : r.samples)
    for (const motion::Clash& c : sm.clashes) {
      const auto key = pairKey(c);
      if (c.rigid || seated.count(key) || !appeared.insert(key).second)
        continue;
      json aj{{"a", c.a}, {"b", c.b}, {"a_name", c.aName}, {"b_name", c.bName}};
      AddSidePaths(aj, c.a, c.aInner, c.b, c.bInner);
      appearedJson.push_back(std::move(aj));
    }
  json out{
      {"clean", r.clean && r.checkedClashes && !r.cancelled && r.error.empty()},
      {"seated_contacts", seated.size()},
      {"appeared", appearedJson},
      {"samples", samples},
      {"worst_sample", r.worstSample},
      {"pairs_checked", r.pairsChecked},
      {"pairs_cached", r.pairsCached},
      {"pairs_repeated", r.pairsRepeated},
      {"pairs_executed", r.pairsChecked - r.pairsCached - r.pairsRepeated},
      {"pairs_unknown", r.pairsUnknown},
      {"collision_status", !r.error.empty()    ? "error"
                           : !r.checkedClashes ? "not_checked"
                           : r.cancelled       ? "incomplete"
                           : r.pairsUnknown    ? "unverified"
                           : r.clean           ? "passed"
                                               : "failed"},
      {"pairs_rigid", r.pairsRigid},
      {"pairs_rigid_checked", r.pairsRigidChecked},
      {"rigid_checked_at_sample",
       r.rigidCheckedAt >= 0 ? json(r.rigidCheckedAt) : json(nullptr)},
      {"rigid_clashes", rigidClashes},
      {"seconds", r.seconds},
      {"refined_samples", r.refinedSamples},
      {"branch_jumps", r.branchJumps},
      {"unit", rot ? "deg" : "mm"},
      {"table", motion::Table(r, s)}};
  if (s.clashMode == "mesh" && s.checkClash)
    out["deflection_mm"] = r.deflectionMm;
  // The press fits an exact check relieved, once per pair: not clashes, and
  // listed so a clean run still says what it took to be deliberate
  // (docs/tolerances/README.md#press-fits). A mesh check relieves none.
  if (s.checkClash) {
    json fits = json::array();
    for (const motion::PressFit& f : r.pressFits) {
      json fj{{"a", f.a},
              {"b", f.b},
              {"a_name", f.aName},
              {"b_name", f.bName},
              {"fit", f.fit},
              {"interference_mm", f.interference},
              {"max_interference_mm", f.maxInterference},
              {"volume_mm3", f.volume}};
      AddSidePaths(fj, f.a, f.aInner, f.b, f.bInner);
      fits.push_back(std::move(fj));
    }
    out["press_fits"] = std::move(fits);
    if (s.clashMode == "mesh")
      out["relieves"] = "nothing: a mesh check sees an engaged thread, a ring "
                        "in its groove and a press fit as the overlap they "
                        "are, except on a rigid pair, which is checked exactly";
  }
  if (r.cancelled)
    out["cancelled"] = true;
  if (!r.error.empty())
    out["error"] = r.error;
  if (r.inputHash != 0)
    out["input_hash"] = inputhash::Hex(r.inputHash);

  // The receipt (core/Receipt.h). This handler had the discipline first and
  // alone — `collision_status` has told error / not_checked / incomplete /
  // unverified / passed / failed apart since v87, and `unverified` there is
  // neither a pass nor a failure. The receipt is that vocabulary made common,
  // so a caller reads one field the same way across twelve RPCs instead of
  // learning this one's.
  //
  // Mapping is deliberately total: every state of collision_status lands
  // somewhere, and the four that are not verdicts land on Unverifiable with
  // their reason, never on Pass.
  {
    receipt::Receipt receipt;
    const receipt::Oracle oracle{s.clashMode == "mesh"
                                     ? "designer/motion-mesh-clash"
                                     : "designer/motion-exact-clash",
                                 "1"};
    const std::string claim =
        "no pair comes into contact that was not touching at rest, and no "
        "pair rigid to each other overlaps";
    if (!r.error.empty()) {
      receipt.Unverifiable(claim, oracle, "the study errored: " + r.error);
    } else if (!s.checkClash) {
      receipt.Unverifiable(
          claim, oracle,
          "this study was asked for kinematics only "
          "(check_clash false), so nothing looked for contact");
    } else if (r.cancelled) {
      receipt.Unverifiable(claim, oracle,
                           "cancelled after " + std::to_string(samples.size()) +
                               " of " + std::to_string(s.samples) +
                               " samples — the poses not reached were not "
                               "checked");
    } else if (r.pairsUnknown) {
      receipt.Unverifiable(claim, oracle,
                           std::to_string(r.pairsUnknown) +
                               " exact pair checks could not be verified");
    } else {
      receipt::Claim c(claim, oracle,
                       r.clean ? receipt::Verdict::Pass
                               : receipt::Verdict::Fail);
      // A mesh sweep answers off the tessellations. It is the right question
      // for "are there millimetres" and it is not the B-Rep, so it says so.
      c.basis = receipt::Basis::Verified;
      c.value = static_cast<double>(appearedJson.size());
      c.unit = "new contacts";
      c.note = std::to_string(r.pairsChecked) + " pair checks over " +
               std::to_string(samples.size()) + " samples, " +
               std::to_string(rigidClashes.size()) +
               " standing clashes between rigid pairs (checked once, "
               "exactly)" +
               (s.clashMode == "mesh"
                    ? ", off the tessellations at " +
                          std::to_string(r.deflectionMm) + " mm deflection"
                    : ", exact on the B-Rep");
      receipt.Add(std::move(c));
    }
    AttachReceipt(out, receipt);
  }
  return out;
}

// `leave_at_deg` / `leave_at_mm`: the live assembly left at a driver VALUE
// rather than at a sample. Refused when it names the wrong unit for the
// driver, falls outside the range the run walked, or comes with `leave_at`
// as well — two answers to one question, and neither would be the other.
//
// The checks need only the study, so the handler makes them BEFORE the run
// (CheckLeaveAtValue) as well as here: the run is the expensive half, and a
// refusal of the pose it should end on used to arrive after it.
void CheckLeaveAtValue(const MotionStudy& s, const RpcParams& p);

json LeaveAtValue(ui::AppState& state, const MotionStudy& s,
                  const motion::Result& r, const RpcParams& p) {
  CheckLeaveAtValue(s, p);
  const bool rot = s.driverDof == MateDof::Rotation;
  const char* want = rot ? "leave_at_deg" : "leave_at_mm";
  const double wire = p.at(want).get<double>();
  const double value = RotAwareAngle(wire, rot);
  const int n = static_cast<int>(r.samples.size());
  if (n < 2)
    throw std::runtime_error(std::string(want) +
                             ": the run has fewer than two samples");
  const double t = (value - s.from) / (s.to - s.from);
  const int nearest =
      std::clamp(static_cast<int>(std::lround(t * (n - 1))), 0, n - 1);
  std::string err;
  if (!ui::ApplyMotionSample(state, s.id, r, static_cast<size_t>(nearest),
                             &value, &err))
    throw std::runtime_error(err);
  return json{{"left_at", nearest}, {"left_at_value", wire}};
}

void CheckLeaveAtValue(const MotionStudy& s, const RpcParams& p) {
  const bool rot = s.driverDof == MateDof::Rotation;
  const char* want = rot ? "leave_at_deg" : "leave_at_mm";
  const char* other = rot ? "leave_at_mm" : "leave_at_deg";
  if (p.contains(other))
    throw std::runtime_error(
        std::string(other) + ": this study drives " +
        (rot ? "a rotation, in degrees — use leave_at_deg"
             : "an offset, in millimetres — use leave_at_mm"));
  if (p.contains("leave_at"))
    throw std::runtime_error(
        std::string("leave_at and ") + want +
        " both say where to leave the assembly; pass one — leave_at is a "
        "sample index, \"start\" or \"last\", " +
        want + " a driver value");
  const double wire = p.at(want).get<double>();
  const double value = RotAwareAngle(wire, rot);
  const double lo = std::min(s.from, s.to), hi = std::max(s.from, s.to);
  const double slack = 1e-9 * std::max(1.0, hi - lo);
  if (value < lo - slack || value > hi + slack) {
    const double shownLo = rot ? angles::ToDegrees(lo) : lo;
    const double shownHi = rot ? angles::ToDegrees(hi) : hi;
    char range[128];
    std::snprintf(range, sizeof range, "%.6g..%.6g %s", shownLo, shownHi,
                  rot ? "deg" : "mm");
    throw std::runtime_error(std::string(want) + ": " + p.at(want).dump() +
                             " is outside the study's range " + range +
                             " — the run never walked there, so it has no "
                             "branch to keep; widen the study's from/to");
  }
}

AssemblyDocument& NeedAssembly(ui::AppState& state) {
  if (!state.assembly)
    throw std::runtime_error(
        "the active document is not an assembly — switch tabs with "
        "set_active_part (see list_parts)");
  return *state.assembly;
}

MotionStudy& NeedStudy(AssemblyDocument& asmDoc, const RpcParams& p) {
  const uint64_t id = p.at("study").get<uint64_t>();
  MotionStudy* s = asmDoc.FindMotionStudyById(id);
  if (!s) {
    std::vector<std::string> have;
    for (const MotionStudy& cand : asmDoc.GetMotionStudies())
      have.push_back(std::to_string(cand.id) + " (" + cand.name + ")");
    throw std::runtime_error("no motion study with id " + std::to_string(id) +
                             "; studies here: " + KnownList(have));
  }
  return *s;
}

// ---- check_followers -------------------------------------------------------

glm::dmat4 OccWorld(const Occurrence& o) {
  return glm::translate(glm::dmat4(1.0), o.position) *
         glm::mat4_cast(o.orientation);
}

// An occurrence's mass as built — printed where its part is printed, else
// solid from its materials — in its part's frame: grams, the centre (mm)
// and the inertia about it (g mm^2, part axes).
struct OwnMass {
  bool known = false;
  double g = 0.0;
  glm::dvec3 com{0.0};
  glm::dmat3 inertia{0.0};
  std::string why;
};
OwnMass MassOf(ui::AppState& state, const Occurrence& o) {
  OwnMass out;
  const Document* part =
      o.refKind == OccurrenceRef::Part
          ? ui::PartResolverFor(state)(o.partId, o.configurationId)
          : nullptr;
  if (part == nullptr) {
    out.why = "it is not a part that is loaded";
    return out;
  }
  std::string err;
  const auto mass = Measure::MassOfBodies(part->GetBodies(), &err, o.material);
  std::vector<assembly::PointInertia> bodies;
  bool printed = false;
  size_t i = 0;
  for (const Body& b : part->GetBodies()) {
    if (b.reference)
      continue;
    const Measure::BodyMass* bm =
        mass && i < mass->bodies.size() ? &mass->bodies[i] : nullptr;
    i++;
    const json pr = PrintedMassJson(state, *part, b,
                                    bm && bm->massKnown ? bm->density : 0.0);
    if (pr.is_null())
      continue;
    printed = true;
    if (!pr.contains("mass_g") || pr["mass_g"].is_null()) {
      out.why =
          pr.value("why", std::string("its filament's density is unknown"));
      return out;
    }
    const double g = pr["mass_g"].get<double>();
    const double v = pr["volume_mm3"].get<double>();
    const auto& c = pr["center_of_mass_mm"];
    const auto& in = pr["inertia"];
    glm::dmat3 own(0.0);
    for (int r = 0; r < 3; r++)
      for (int col = 0; col < 3; col++)
        own[col][r] = in[r][col].get<double>() * (v > 0 ? g / v : 0.0);
    bodies.push_back(
        {g, {c[0].get<double>(), c[1].get<double>(), c[2].get<double>()}, own});
  }
  if (!printed) {
    if (!mass || !mass->massKnown) {
      out.why = "its solid mass is unknown (a body with no material)";
      return out;
    }
    size_t k = 0;
    for (const Body& b : part->GetBodies()) {
      if (b.reference)
        continue;
      const Measure::BodyMass& bm = mass->bodies[k++];
      glm::dmat3 own(0.0);
      for (int r = 0; r < 3; r++)
        for (int col = 0; col < 3; col++)
          own[col][r] = bm.inertia[r][col] * bm.density * 1e-6;
      bodies.push_back({bm.massG, bm.centerOfMass, own});
    }
  }
  for (const auto& b : bodies) {
    out.g += b.massG;
    out.com += b.massG * b.centre;
  }
  if (out.g > 0.0)
    out.com /= out.g;
  out.inertia = assembly::InertiaAbout(bodies, out.com);
  out.known = true;
  return out;
}

// The generalised inertia and gravity force on b's DOF of everything that
// moves with it — the follower and what it carries through mate loops —
// from the assembly solved with b pinned and the contact relation left
// out, at a few values of b across its travel.
std::function<follower::MassAt(double)> MassFunction(ui::AppState& state,
                                                     const Relation& r,
                                                     const contact::Model& m,
                                                     json* moving) {
  AssemblyDocument copy = *state.assembly;
  copy.RemoveRelation(r.id);
  const auto parts = ui::PartResolverFor(state);
  const auto asms = ui::AssemblyResolverFor(state);
  double lo = 1e300, hi = -1e300;
  for (const contact::Table::Node& n : m.table.nodes) {
    lo = std::min(lo, n.q);
    hi = std::max(hi, n.q);
  }
  const int grid = hi - lo > 1e-9 ? 9 : 1;
  const double h = m.turn ? 1e-5 : 1e-3;   // b's unit
  const double unit = m.turn ? 1.0 : 1e-3; // to SI
  struct Node {
    double q, M, Q;
  };
  std::vector<Node> nodes;
  std::map<uint64_t, OwnMass> masses;
  std::set<uint64_t> moved;
  const auto poses = [&](double q) {
    Mate* mb = copy.FindMateById(r.mateB);
    assembly::SetMateDof(*mb, r.dofB, q);
    assembly::Solve(copy, parts, asms, {r.mateB, r.dofB, {}}, false);
    std::map<uint64_t, glm::dmat4> out;
    for (const Occurrence& o : copy.GetOccurrences())
      out[o.id] = OccWorld(o);
    return out;
  };
  for (int k = 0; k < grid; k++) {
    const double q = grid == 1 ? lo : lo + (hi - lo) * k / (grid - 1);
    const auto a = poses(q - h), b = poses(q + h);
    double M = 0.0, Q = 0.0;
    for (const auto& [id, wa] : a) {
      const glm::dmat4& wb = b.at(id);
      double diff = 0.0;
      for (int c = 0; c < 4; c++)
        diff = std::max(diff, glm::length(wb[c] - wa[c]));
      if (diff < 1e-12)
        continue;
      moved.insert(id);
      auto it = masses.find(id);
      if (it == masses.end())
        it = masses.emplace(id, MassOf(state, *copy.FindById(id))).first;
      const OwnMass& om = it->second;
      if (!om.known)
        throw std::runtime_error(
            "check_followers: '" + copy.FindById(id)->name +
            "' moves with the follower and its mass is not known (" + om.why +
            "): liftoff depends on it — give it a material");
      const glm::dvec3 ca = glm::dvec3(wa * glm::dvec4(om.com, 1.0));
      const glm::dvec3 cb = glm::dvec3(wb * glm::dvec4(om.com, 1.0));
      const glm::dvec3 v = (cb - ca) * 1e-3 / (2.0 * h * unit); // m per q(SI)
      const glm::dmat3 ra(wa), rb(wb);
      const glm::dmat3 dr = rb * glm::transpose(ra);
      const glm::dvec3 w{dr[1][2] - dr[2][1], dr[2][0] - dr[0][2],
                         dr[0][1] - dr[1][0]}; // 2 sin(angle) * axis
      const glm::dvec3 omega = 0.5 * w / (2.0 * h * unit);
      const double kg = om.g * 1e-3;
      const glm::dmat3 Iw = glm::dmat3(wa) * om.inertia *
                            glm::transpose(glm::dmat3(wa)) * 1e-9; // kg m^2
      M += kg * glm::dot(v, v) + glm::dot(omega, Iw * omega);
      Q += kg * glm::dot(conv::kGravity, v);
    }
    nodes.push_back({q, M, Q});
  }
  if (moving) {
    *moving = json::array();
    for (uint64_t id : moved)
      (*moving).push_back({{"occurrence", id},
                           {"name", copy.FindById(id)->name},
                           {"mass_g", masses.at(id).g}});
  }
  return [nodes, unit](double q) {
    follower::MassAt out;
    if (nodes.size() == 1) {
      out.M = nodes[0].M;
      out.Q = nodes[0].Q;
      return out;
    }
    size_t i = 0;
    while (i + 2 < nodes.size() && q > nodes[i + 1].q)
      i++;
    const Node& a = nodes[i];
    const Node& b = nodes[i + 1];
    const double t = (q - a.q) / (b.q - a.q);
    out.M = a.M + t * (b.M - a.M);
    out.Q = a.Q + t * (b.Q - a.Q);
    out.dM = (b.M - a.M) / ((b.q - a.q) * unit);
    return out;
  };
}

json RangesJson(const std::vector<follower::Range>& rs) {
  json out = json::array();
  for (const follower::Range& r : rs)
    out.push_back({r.fromDeg, r.toDeg});
  return out;
}

} // namespace

std::span<const MethodDoc> MotionMethodDocs() { return MotionMethods; }

json CheckFollowers(ui::AppState& state, uint64_t studyId,
                    const std::vector<uint64_t>& relationIds,
                    std::optional<double> speedRpm,
                    std::optional<double> maxPressureDeg) {
  AssemblyDocument& asmDoc = NeedAssembly(state);
  const MotionStudy* study = asmDoc.FindMotionStudyById(studyId);
  if (!study)
    throw std::runtime_error("check_followers: no motion study with id " +
                             std::to_string(studyId));
  if (study->driverDof != MateDof::Rotation)
    throw std::runtime_error("check_followers: the study's driver slides; a "
                             "speed in rpm needs a driver that turns");
  const motion::Result* run = ui::MotionResultFor(state, studyId);
  if (!run || ui::MotionResultStale(state, studyId))
    throw std::runtime_error(
        "check_followers answers from a stored run, and study " +
        std::to_string(studyId) + " has " +
        (run ? "a stale one (its inputs changed)" : "none") +
        ": run_motion_study first");
  std::vector<const Relation*> rels;
  for (const Relation& r : asmDoc.GetRelations())
    if (r.kind == RelationKind::Contact &&
        (relationIds.empty() ||
         std::find(relationIds.begin(), relationIds.end(), r.id) !=
             relationIds.end()))
      rels.push_back(&r);
  for (uint64_t id : relationIds) {
    const Relation* r = asmDoc.FindRelationById(id);
    if (!r || r->kind != RelationKind::Contact)
      throw std::runtime_error("check_followers: relation " +
                               std::to_string(id) +
                               " is not a contact relation here");
  }
  if (rels.empty())
    throw std::runtime_error(
        "check_followers: this assembly has no contact relation (add_relation "
        "type contact)");
  json out{{"study", studyId}, {"relations", json::array()}};
  if (speedRpm)
    out["speed_rpm"] = *speedRpm;
  for (const Relation* r : rels) {
    if (!r->error.empty() || !r->contactModel)
      throw std::runtime_error("check_followers: relation " +
                               std::to_string(r->id) +
                               " is broken: " + r->error);
    const contact::Model& m = *r->contactModel;
    const bool turn = m.turn;
    const double unit = turn ? angles::kDegPerRad : 1.0;
    const std::string sfx = turn ? "_deg" : "_mm";
    follower::Input in;
    in.model = &m;
    in.speedRpm = speedRpm;
    in.maxPressureDeg = maxPressureDeg;
    // The samples: the cam's angle and the follower's DOF as the run left
    // them, and the driver's ratio to the cam from them.
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (size_t i = 0; i < run->samples.size(); i++) {
      const motion::Sample& smp = run->samples[i];
      if (!smp.solved)
        continue;
      std::optional<double> a, b;
      for (const motion::Sample::MateValue& mv : smp.mates) {
        if (!mv.path.empty())
          continue;
        if (mv.mate == r->mateA)
          a = mv.rotation;
        if (mv.mate == r->mateB)
          b = r->dofB == MateDof::Rotation ? mv.rotation : mv.offset;
      }
      if (!a || !b)
        continue;
      in.samples.push_back({i, smp.value, *a, *b});
      sx += *a;
      sy += smp.value;
      sxx += *a * *a;
      sxy += *a * smp.value;
    }
    if (in.samples.size() < 2)
      throw std::runtime_error("check_followers: the study's run does not "
                               "move relation " +
                               std::to_string(r->id) +
                               "'s cam (fewer than two samples with it)");
    const double n = static_cast<double>(in.samples.size());
    const double den = n * sxx - sx * sx;
    if (std::abs(den) < 1e-12)
      throw std::runtime_error("check_followers: the study does not turn "
                               "relation " +
                               std::to_string(r->id) + "'s cam");
    in.driverPerCam = (n * sxy - sx * sy) / den;
    // The design, when the cam's body is a CamProfile's.
    const Occurrence* camOcc = asmDoc.FindById(m.cam);
    const Document* camPart =
        camOcc ? ui::PartResolverFor(state)(camOcc->partId,
                                            camOcc->configurationId)
               : nullptr;
    // The body's own CamProfile, or the one CamProfile fused into it (a
    // cam added to its hub keeps the hub's body).
    const CamProfileFeature* cp = nullptr;
    int added = 0;
    const CamProfileFeature* addedCp = nullptr;
    if (camPart)
      for (const auto& f : camPart->GetFeatures()) {
        const auto* c = dynamic_cast<const CamProfileFeature*>(f.get());
        if (!c)
          continue;
        if (c->GetId() == m.camBody)
          cp = c;
        else if (!c->newBody) {
          added++;
          addedCp = c;
        }
      }
    if (!cp && added == 1)
      cp = addedCp;
    if (cp && cp->report.valid) {
      follower::Designed d;
      d.motion = cp->report.motion;
      d.follower = cp->report.follower;
      d.ccw = cp->ccw;
      d.baseRadius = cp->report.baseRadius;
      d.fitDeviation = cp->report.fitDeviation;
      d.frameInCamPart = cp->report.frame;
      in.designed = d;
    }
    json moving;
    in.mass = MassFunction(state, *r, m, &moving);
    const follower::Report rep = follower::Check(in);

    // Across the width, at every sample; and the study's own clash list.
    std::string why;
    const auto levels = follower::Levels(m, &why);
    double worstClash = 0.0, worstGap = 0.0;
    json failures = json::array(), studyClashes = json::array();
    bool touching = !levels.empty();
    json perSample = json::array();
    for (const follower::Sample& smp : in.samples) {
      const follower::Across x = follower::AcrossWidth(m, levels, smp.a, smp.b);
      worstClash = std::max(worstClash, x.clash);
      worstGap = std::max(worstGap, x.gap);
      if (!x.ok) {
        touching = false;
        if (failures.size() < 12)
          failures.push_back({{"sample", smp.index},
                              {"clash" + sfx, x.clash * unit},
                              {"gap" + sfx, x.gap * unit},
                              {"across_mm", x.atZ}});
      }
      for (const motion::Clash& c : run->samples[smp.index].clashes)
        if ((c.a == m.cam && c.b == m.follower) ||
            (c.a == m.follower && c.b == m.cam)) {
          studyClashes.push_back(smp.index);
          touching = false;
        }
      json ps{{"sample", smp.index}, {"cam_deg", angles::ToDegrees(smp.a)}};
      for (const follower::SampleOut& so : rep.samples)
        if (so.index == smp.index) {
          ps["pressure_angle_deg"] = so.pressureDeg;
          if (rep.deviationKnown)
            ps["deviation" + sfx] = so.deviation * unit;
        }
      perSample.push_back(std::move(ps));
    }
    json rj{{"relation", r->id},
            {"cam", m.cam},
            {"follower", m.follower},
            {"samples", in.samples.size()},
            {"samples_in_drops", rep.inDrop},
            {"touching", touching},
            {"clash" + sfx, worstClash * unit},
            {"gap" + sfx, worstGap * unit},
            {"tolerance_mm", m.tolerance},
            {"failures", failures},
            {"study_clashes", studyClashes},
            {"per_sample", perSample}};
    if (levels.empty())
      rj["touching_why"] = "the cuts across the width failed: " + why;
    json pa{{"max_deg", rep.maxPressureDeg},
            {"at_cam_deg", rep.maxPressureAtDeg}};
    if (maxPressureDeg) {
      pa["limit_deg"] = *maxPressureDeg;
      pa["ok"] = *rep.pressureOk;
    } else {
      pa["ok"] = "unchecked";
    }
    rj["pressure_angle"] = pa;
    rj["curvature"] = {{"min_cam_convex_mm", rep.minCamConvex},
                       {"min_cam_concave_mm", rep.minCamConcave},
                       {"follower_radius_mm", rep.followerRadius},
                       {"bridges_deg", RangesJson(rep.bridges)}};
    if (rep.deviationKnown) {
      rj["deviation" + sfx] = rep.deviation * unit;
      rj["deviation_at_cam_deg"] = rep.deviationAtDeg;
      rj["fit_deviation_mm"] = rep.fitDeviation;
      rj["mate_zero" + sfx] = rep.mateZero * unit;
    }
    json g{
        {"held", rep.notHeld.empty()},
        {"not_held_deg", RangesJson(rep.notHeld)},
        {"liftoff_rpm", rep.liftoffRpm ? json(*rep.liftoffRpm) : json(nullptr)},
        {"liftoff_cam_rpm",
         rep.liftoffCamRpm ? json(*rep.liftoffCamRpm) : json(nullptr)},
        {"liftoff_at_cam_deg", rep.liftoffAtDeg},
        {"driver_per_cam", in.driverPerCam},
        {"moving", moving}};
    json drops = json::array();
    for (const follower::DropOut& d : rep.drops) {
      json dj{{"jams", d.jams},
              {"at_cam_deg", d.atDeg},
              {"height" + sfx, d.height * unit},
              {"fall_s", d.fallS},
              {"dwell_deg", d.dwellDeg},
              {"drop_rpm", d.dropRpm}};
      if (d.turnDeg) {
        dj["turn_deg"] = *d.turnDeg;
        dj["lands"] = *d.lands;
      }
      drops.push_back(std::move(dj));
    }
    g["drops"] = drops;
    rj["gravity"] = g;
    if (rep.holdsAtSpeed)
      rj["holds_at_speed"] = *rep.holdsAtSpeed;
    out["relations"].push_back(std::move(rj));
  }
  return out;
}

std::optional<json> HandleMotionRpc(ui::AppState& state, const std::string& m,
                                    const RpcParams& p) {
  if (m == "add_motion_study") {
    AssemblyDocument& asmDoc = NeedAssembly(state);
    MotionStudy s;
    const uint64_t driver = p.at("driver").get<uint64_t>();
    // v129: a driver inside a flexible placement, named {driver_path, driver}
    // — the pump's camshaft, turned from the engine.
    if (p.contains("driver_path"))
      s.driverPath = p.at("driver_path").get<std::vector<uint64_t>>();
    std::string pathWhy;
    const Mate* mate =
        s.driverPath.empty()
            ? asmDoc.FindMateById(driver)
            : assembly::MateAtPath(asmDoc, s.driverPath, driver,
                                   ui::AssemblyResolverFor(state), &pathWhy);
    if (!mate && !s.driverPath.empty())
      throw std::runtime_error("driver_path: " + pathWhy);
    if (!mate) {
      std::vector<std::string> have;
      for (const Mate& cand : asmDoc.GetMates())
        if (assembly::MateHasDof(cand.type, MateDof::Rotation) ||
            assembly::MateHasDof(cand.type, MateDof::Offset))
          have.push_back(std::to_string(cand.id) + " (" + cand.name + ")");
      throw std::runtime_error(
          "no mate with id " + std::to_string(driver) +
          "; mates with a degree of freedom: " + KnownList(have));
    }
    const bool canRot = assembly::MateHasDof(mate->type, MateDof::Rotation);
    const bool canSlide = assembly::MateHasDof(mate->type, MateDof::Offset);
    if (p.contains("dof")) {
      s.driverDof =
          DocumentIO::MateDofFromString(p.at("dof").get<std::string>());
    } else if (canRot && canSlide) {
      throw std::runtime_error("mate " + std::to_string(driver) +
                               " is cylindrical — it both turns and slides; "
                               "say dof: rotation or dof: offset");
    } else if (canRot) {
      s.driverDof = MateDof::Rotation;
    } else if (canSlide) {
      s.driverDof = MateDof::Offset;
    } else {
      throw std::runtime_error(
          "mate " + std::to_string(driver) + " (" +
          DocumentIO::MateToJson(*mate).at("mate_type").get<std::string>() +
          ") has no degree of freedom to drive — a revolute or cylindrical "
          "mate turns, a slider or cylindrical mate slides");
    }
    if ((s.driverDof == MateDof::Rotation && !canRot) ||
        (s.driverDof == MateDof::Offset && !canSlide))
      throw std::runtime_error("mate " + std::to_string(driver) + " has no " +
                               DocumentIO::MateDofToString(s.driverDof) +
                               " to drive");
    s.driverMate = driver;
    const bool rot = s.driverDof == MateDof::Rotation;
    s.from = RotAwareAngle(p.at("from").get<double>(), rot);
    s.to = RotAwareAngle(p.at("to").get<double>(), rot);
    if (std::abs(s.to - s.from) < 1e-12)
      throw std::runtime_error("from and to are equal — nothing would move");
    s.samples = p.value("samples", 48);
    if (s.samples < 2)
      throw std::runtime_error("samples must be at least 2");
    s.checkClash = p.value("check_clash", true);
    s.clashMode = p.value("clash_mode", std::string("mesh"));
    if (s.clashMode != "mesh" && s.clashMode != "exact")
      throw std::runtime_error("clash_mode '" + s.clashMode +
                               "' is not one of: mesh, exact");
    ReadClashDeflection(p, s);
    ReadMaxRefine(p, s);
    if (p.contains("configuration")) {
      // null clears it (whichever is active); 0 is the base; else an id.
      if (p.at("configuration").is_null()) {
        s.configuration.reset();
      } else {
        const uint64_t c = p.at("configuration").get<uint64_t>();
        if (c != 0 && asmDoc.FindConfiguration(c) == nullptr) {
          std::vector<std::string> have;
          for (const Configuration& cfg : asmDoc.GetConfigurations())
            have.push_back(std::to_string(cfg.id) + " (" + cfg.name + ")");
          throw std::runtime_error(
              "configuration: no configuration " + std::to_string(c) +
              "; configurations: " + KnownList(have) +
              " (0 = the base, null = whichever is active)");
        }
        s.configuration = c;
      }
    }
    s.name = p.value("name", std::string{});
    if (s.name.empty())
      s.name = "Motion " + std::to_string(asmDoc.GetMotionStudies().size() + 1);
    ui::SnapshotAssembly(state);
    const uint64_t id = asmDoc.AddMotionStudy(std::move(s));
    return StudyJson(state, asmDoc, *asmDoc.FindMotionStudyById(id));
  }
  if (m == "set_motion_study") {
    AssemblyDocument& asmDoc = NeedAssembly(state);
    MotionStudy& live = NeedStudy(asmDoc, p);
    // Edited on a copy and written back only once every field has passed: a
    // refused call changes nothing
    // (docs/scripting/rpc.md#a-refused-call-changes-nothing).
    MotionStudy s = live;
    const bool rot = s.driverDof == MateDof::Rotation;
    if (p.contains("name"))
      s.name = p.at("name").get<std::string>();
    if (p.contains("from"))
      s.from = RotAwareAngle(p.at("from").get<double>(), rot);
    if (p.contains("to"))
      s.to = RotAwareAngle(p.at("to").get<double>(), rot);
    if (p.contains("samples")) {
      s.samples = IntParam(p.at("samples"), "samples");
      if (s.samples < 2)
        throw std::runtime_error("samples must be at least 2");
    }
    if (p.contains("check_clash"))
      s.checkClash = p.at("check_clash").get<bool>();
    if (p.contains("clash_mode")) {
      s.clashMode = p.at("clash_mode").get<std::string>();
      if (s.clashMode != "mesh" && s.clashMode != "exact")
        throw std::runtime_error("clash_mode '" + s.clashMode +
                                 "' is not one of: mesh, exact");
    }
    ReadClashDeflection(p, s);
    ReadMaxRefine(p, s);
    if (p.contains("configuration")) {
      // null clears it (whichever is active); 0 is the base; else an id.
      if (p.at("configuration").is_null()) {
        s.configuration.reset();
      } else {
        const uint64_t c = p.at("configuration").get<uint64_t>();
        if (c != 0 && asmDoc.FindConfiguration(c) == nullptr) {
          std::vector<std::string> have;
          for (const Configuration& cfg : asmDoc.GetConfigurations())
            have.push_back(std::to_string(cfg.id) + " (" + cfg.name + ")");
          throw std::runtime_error(
              "configuration: no configuration " + std::to_string(c) +
              "; configurations: " + KnownList(have) +
              " (0 = the base, null = whichever is active)");
        }
        s.configuration = c;
      }
    }
    ui::SnapshotAssembly(state);
    live = s;
    asmDoc.BumpVersion();
    ui::DropMotionResult(state, live.id); // it described the old definition
    return StudyJson(state, asmDoc, live);
  }
  if (m == "remove_motion_study") {
    AssemblyDocument& asmDoc = NeedAssembly(state);
    const MotionStudy& s = NeedStudy(asmDoc, p);
    const uint64_t id = s.id;
    ui::SnapshotAssembly(state);
    asmDoc.RemoveMotionStudy(id);
    ui::DropMotionResult(state, id);
    return json{{"removed", id}};
  }
  if (m == "list_motion_studies") {
    AssemblyDocument& asmDoc = NeedAssembly(state);
    json out = json::array();
    const ui::BackgroundJob* job = ui::FindBackgroundJob(state, "motion");
    for (const MotionStudy& s : asmDoc.GetMotionStudies()) {
      const bool running = job != nullptr && job->studyId == s.id;
      json j = StudyJson(state, asmDoc, s);
      const motion::Result* r = ui::MotionResultFor(state, s.id);
      j["has_result"] = r != nullptr;
      if (r) {
        j["stale"] = ui::MotionResultStale(state, s.id);
        j["clean"] = r->clean && r->checkedClashes && !j["stale"].get<bool>();
        j["collision_checked"] = r->checkedClashes;
      }
      j["running"] = running;
      out.push_back(j);
    }
    return json{{"studies", out}};
  }
  if (m == "run_motion_study") {
    AssemblyDocument& asmDoc = NeedAssembly(state);
    MotionStudy& s = NeedStudy(asmDoc, p);
    const bool includePoses = p.value("include_poses", false);
    const std::vector<motion::TracePoint> trace = ReadTrace(asmDoc, p);
    const bool atValue =
        p.contains("leave_at_deg") || p.contains("leave_at_mm");
    // Who the run belongs to, and whether a stored answer may stand for it
    // (docs/simulation/README.md#studies-as-jobs).
    ui::JobStart start;
    start.recompute = p.value("recompute", false);
    start.owner = p.value("outlive_client", false) ? 0 : state.rpcConnection;
    // `leave_at`'s shape is checked BEFORE the run, not after it: the run is
    // the expensive half, and a typo in the pose to leave it at used to be
    // refused only once it had finished. (The index's range needs the run's
    // sample count, so that one is still checked after.)
    if (atValue) {
      CheckLeaveAtValue(s, p);
    } else if (p.contains("leave_at")) {
      const json& at = p.at("leave_at");
      const bool word = at.is_string() && (at.get<std::string>() == "last" ||
                                           at.get<std::string>() == "start");
      if (!word && !(at.is_number_integer() && at.get<int64_t>() >= 0))
        throw std::runtime_error(
            "leave_at: \"start\", \"last\", or a sample index from 0 — got " +
            at.dump());
    }
    if (p.value("async", false)) {
      if (!trace.empty())
        throw std::runtime_error(
            "trace needs the run's samples, and an async run has none yet "
            "-- poll motion_status, then motion_status {study, trace}");
      if (p.contains("leave_at") || atValue)
        throw std::runtime_error(
            "leave_at needs the run's samples, and an async run has none yet "
            "-- poll motion_status, then run_motion_study {study, leave_at} "
            "(the same inputs are answered from the stored run, and the pose "
            "is one solve)");
      std::string err;
      if (!ui::StartMotionRun(state, s.id, &err, nullptr, &start))
        throw std::runtime_error(err);
      if (start.reused) {
        json out{{"started", false},
                 {"running", false},
                 {"reused", true},
                 {"kind", "motion"}};
        if (const motion::Result* r = ui::MotionResultFor(state, s.id))
          out["input_hash"] = inputhash::Hex(r->inputHash);
        return out;
      }
      return json{{"started", true},
                  {"running", true},
                  {"job", start.jobId},
                  {"kind", "motion"},
                  {"outlives_client", start.owner == 0}};
    }
    const motion::Result r = ui::RunMotionNow(state, s.id, &start);
    if (!r.error.empty())
      throw std::runtime_error(r.error);
    json out = ResultJson(NeedAssembly(state), r, s, includePoses);
    out["reused"] = start.reused;
    // Traced before anything is left anywhere: a point the run cannot trace
    // is refused with the assembly still where it was.
    if (!trace.empty())
      out["trace"] = TraceResultJson(trace, r);
    // A run happens on a COPY, so the assembly is left exactly where it was
    // -- which means nothing about the far end of the travel can be measured
    // afterwards. `leave_at` poses the live assembly at one of the run's
    // samples instead: the placements and mate values recorded there,
    // written in and solved ONCE, so it arrives on the branch the run found
    // without walking the travel again.
    const uint64_t solvesBefore = assembly::SolveCount();
    if (atValue)
      out.update(LeaveAtValue(state, s, r, p));
    if (p.contains("leave_at") && !atValue) {
      const json& at = p.at("leave_at");
      const int n = static_cast<int>(r.samples.size());
      int idx = 0;
      if (at.is_string()) {
        const std::string w = at.get<std::string>();
        if (w == "last")
          idx = n - 1;
        else if (w == "start")
          idx = 0;
        else
          throw std::runtime_error(
              "leave_at: \"start\", \"last\", or a sample index 0.." +
              std::to_string(n - 1) + " — got " + at.dump());
      } else {
        idx = IntParam(at, "leave_at");
        if (idx < 0 || idx >= n)
          throw std::runtime_error("leave_at: sample " + std::to_string(idx) +
                                   " of a study with " + std::to_string(n) +
                                   " — the range is 0.." +
                                   std::to_string(n - 1));
      }
      std::string err;
      if (!ui::ApplyMotionSample(state, s.id, r, static_cast<size_t>(idx),
                                 nullptr, &err))
        throw std::runtime_error(err);
      out["left_at"] = idx;
    }
    if (p.contains("leave_at") || atValue)
      out["left_solves"] = assembly::SolveCount() - solvesBefore;
    return out;
  }
  if (m == "check_followers") {
    // rpc-params: study, relations, speed_rpm, max_pressure_angle_deg
    std::vector<uint64_t> rels;
    if (p.contains("relations"))
      rels = p.at("relations").get<std::vector<uint64_t>>();
    std::optional<double> speed, limit;
    if (p.contains("speed_rpm"))
      speed = p.at("speed_rpm").get<double>();
    if (p.contains("max_pressure_angle_deg"))
      limit = p.at("max_pressure_angle_deg").get<double>();
    return CheckFollowers(state, p.at("study").get<uint64_t>(), rels, speed,
                          limit);
  }
  if (m == "motion_status") {
    AssemblyDocument& asmDoc = NeedAssembly(state);
    const MotionStudy& s = NeedStudy(asmDoc, p);
    const bool includePoses = p.value("include_poses", false);
    const std::vector<motion::TracePoint> trace = ReadTrace(asmDoc, p);
    // The job running THIS study, if any: its id and how far it is.
    ui::BackgroundJob* job = ui::FindBackgroundJob(state, "motion");
    if (job != nullptr && job->studyId != s.id)
      job = nullptr;
    json out{{"running", job != nullptr}};
    if (job != nullptr) {
      out["job"] = job->id;
      out["fraction"] = job->fraction.load();
    }
    if (const motion::Result* r = ui::MotionResultFor(state, s.id)) {
      out["result"] = ResultJson(asmDoc, *r, s, includePoses);
      if (!trace.empty())
        out["result"]["trace"] = TraceResultJson(trace, *r);
      out["stale"] = ui::MotionResultStale(state, s.id);
      if (out["stale"].get<bool>()) {
        out["result"]["last_clean"] = r->clean;
        out["result"]["clean"] = false;
        out["result"]["collision_status"] = "stale";
      }
      out["has_result"] = true;
    } else {
      out["has_result"] = false;
    }
    return out;
  }
  if (m == "set_motion_pose") {
    AssemblyDocument& asmDoc = NeedAssembly(state);
    const MotionStudy& s = NeedStudy(asmDoc, p);
    const double t = p.at("t").get<double>();
    if (t < 0.0 || t > 1.0)
      throw std::runtime_error("t is 0..1 along the study's range");
    std::string err;
    if (!ui::ApplyMotionPose(state, s.id, t, &err))
      throw std::runtime_error(err);
    const bool rot = s.driverDof == MateDof::Rotation;
    const double value = motion::ValueAt(s, t);
    json out{{"t", t},
             {"value", rot ? angles::ToDegrees(value) : value},
             {"unit", rot ? "deg" : "mm"}};
    int errored = 0;
    for (const Mate& mm : asmDoc.GetMates())
      if (!mm.error.empty())
        errored++;
    out["errored_mates"] = errored;
    return out;
  }
  if (m == "export_motion") {
    AssemblyDocument& asmDoc = NeedAssembly(state);
    const MotionStudy& s = NeedStudy(asmDoc, p);
    const std::string path = p.at("path").get<std::string>();
    std::string err;
    size_t nodes = 0, frames = 0;
    if (!ui::ExportMotionGlb(state, s.id, path, &err, &nodes, &frames))
      throw std::runtime_error(err);
    return json{{"path", path}, {"nodes", nodes}, {"frames", frames}};
  }
  if (m == "export_motion_video" || m == "export_assembly_video") {
    // One request and one job for both (docs/assemblies/README.md
    // #motion-video): a study's run, or the assembly sequence's timeline.
    const bool sequence = m == "export_assembly_video";
    AssemblyDocument& asmDoc = NeedAssembly(state);
    ui::MotionVideoRequest req;
    if (m == "export_motion_video") {
      req.studyId = NeedStudy(asmDoc, p).id;
    }
    req.path = p.at("path").get<std::string>();
    req.format = p.value("format", std::string{});
    if (p.contains("width") || p.contains("height")) {
      if (!p.contains("width") || !p.contains("height"))
        throw std::runtime_error("width and height go together (pixels); "
                                 "pass both, or neither for the viewport's "
                                 "size");
      const int w = IntParam(p.at("width"), "width"),
                h = IntParam(p.at("height"), "height");
      if (w < 2 || h < 2)
        throw std::runtime_error("width and height are pixels, at least 2 "
                                 "each");
      req.width = static_cast<uint32_t>(w);
      req.height = static_cast<uint32_t>(h);
    }
    if (p.contains("fps")) {
      req.fps = p.at("fps").get<double>();
      if (!(req.fps > 0.0))
        throw std::runtime_error("fps must be a positive number of frames "
                                 "per second (omit it for 60)");
    }
    if (p.contains("frames")) {
      req.frames = IntParam(p.at("frames"), "frames");
      if (req.frames < 2)
        throw std::runtime_error("frames must be at least 2");
    }
    req.quality = p.value("quality", std::string{});
    if (!sequence && (p.contains("speed") || p.contains("highlight")))
      throw std::runtime_error("speed and highlight film an assembly "
                               "sequence (export_assembly_video); a study "
                               "plays at its run's 24 samples a second");
    if (sequence && p.contains("speed")) {
      if (!p.at("speed").is_number() || !(p.at("speed").get<double>() > 0.0))
        throw std::runtime_error("speed is a ratio of the timeline's own pace "
                                 "(2 plays it twice as fast): a positive "
                                 "number");
      req.speed = p.at("speed").get<double>();
    }
    if (sequence && p.contains("highlight")) {
      if (!p.at("highlight").is_boolean())
        throw std::runtime_error("highlight is true or false");
      req.highlight = p.at("highlight").get<bool>();
    }
    // Read before the job starts: a mistyped `async` refused AFTER it would
    // leave a video rendering behind an error.
    const bool async = p.value("async", false);
    ui::MotionVideoPlan plan;
    std::string err;
    if (!(sequence ? ui::StartAssemblyVideo(state, req, &err, &plan)
                   : ui::StartMotionVideo(state, req, &err, &plan)))
      throw std::runtime_error(err);
    const auto planJson = [](const ui::MotionVideoPlan& pl) {
      return json{
          {"path", pl.path},
          {"format", pl.format},
          // A sequence's length is known once its check has run.
          {"frames", pl.frames > 0 ? json(pl.frames) : json(nullptr)},
          {"width", pl.width},
          {"height", pl.height},
          {"fps", pl.fps},
          {"quality", pl.format == "png" ? json(nullptr) : json(pl.quality)}};
    };
    if (async) {
      json out = planJson(plan);
      out["started"] = true;
      out["job"] = "motion-video";
      return out;
    }
    ui::JoinBackgroundJobs(state, "motion-video");
    const auto last = state.motionVideoLast;
    if (!last)
      throw std::runtime_error("the export ended without an outcome");
    if (!last->ok)
      throw std::runtime_error(m + ": " + last->error);
    json out = planJson(last->plan);
    out["bytes"] = last->bytes;
    out["seconds"] = last->seconds;
    if (!last->warnings.empty())
      out["warnings"] = last->warnings;
    return out;
  }
  if (m == "motion_video_status") {
    ui::ReapBackgroundJobs(state);
    json out{{"running", false}};
    if (ui::BackgroundJob* job = ui::FindBackgroundJob(state, "motion-video")) {
      out["running"] = true;
      out["fraction"] = job->fraction.load();
    }
    if (const auto last = state.motionVideoLast) {
      json l{{"ok", last->ok},
             {"path", last->plan.path},
             {"format", last->plan.format},
             {"frames", last->framesWritten},
             {"width", last->plan.width},
             {"height", last->plan.height},
             {"fps", last->plan.fps},
             {"bytes", last->bytes},
             {"seconds", last->seconds}};
      if (!last->error.empty())
        l["error"] = last->error;
      if (last->cancelled)
        l["cancelled"] = true;
      if (!last->warnings.empty())
        l["warnings"] = last->warnings;
      out["last"] = std::move(l);
    }
    return out;
  }
  return std::nullopt;
}

} // namespace remote
