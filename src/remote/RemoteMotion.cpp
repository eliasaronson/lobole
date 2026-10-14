// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "remote/RemoteMotion.h"

#include "core/Angles.h"
#include "remote/RemoteHelpers.h"

#include "core/AssemblySolve.h"
#include "core/InputHash.h"
#include "core/MotionStudy.h"
#include "core/Occurrence.h"
#include "core/io/Serialization.h"
#include "ui/AppState.h"
#include "ui/AssemblyActions.h"
#include "ui/BackgroundJobs.h"
#include "ui/MotionActions.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
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
     "it), name?}. Returns the study and, if it cannot run, `why_not`. Nothing "
     "runs until run_motion_study. ONE driver moves the whole mechanism "
     "when the rest follows through mates and RELATIONS (add_relation): a "
     "geared pair turns because the relation propagates, not because two "
     "DOFs were driven. Pick the driver with care where a mechanism has a "
     "SINGULAR configuration — a symmetric linkage that can also move "
     "antisymmetrically passes through one — because the solve continues "
     "from the previous sample and can come out of it on the other branch. "
     "The driver to choose is the DOF that cannot change on the branch you "
     "do not want; the clash check is what makes the jump visible.",
     "check_clash, clash_deflection_mm, clash_mode, configuration, dof, "
     "driver, driver_path, from, name, samples, to"},
    {"set_motion_study",
     "Edit a study {study, name?, from?, to?, samples?, check_clash?, "
     "clash_mode?, clash_deflection_mm? (mm, null = derived per body "
     "again), configuration? (an id, 0 = the base, null = whichever "
     "is active)}; a stored "
     "result is dropped, since it described the old definition.",
     "check_clash, clash_deflection_mm, clash_mode, configuration, from, "
     "name, samples, study, to"},
    {"remove_motion_study", "Remove a study by id.", "study"},
    {"list_motion_studies",
     "Every study, with whether it has a result and whether one is running.",
     "", remote::NoFlags,
     "studies:{check_clash:bool, clash_mode:string, driver_dof:string, "
     "driver_mate:int, from:number, has_result:bool, id:int, "
     "name:string, running:bool, samples:int, to:number, type:string, "
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
     "mesh check calls touching a clash, and at rest a propeller touches "
     "its motor and its screws, so the pairs already in contact at the "
     "first sample are counted and the rest are listed), `worst_sample`, "
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
     "leave_at costs that solve and nothing more). To leave it at a driver "
     "VALUE instead, `leave_at_deg` (a rotation study, degrees) or "
     "`leave_at_mm` (an offset study, millimetres), within the study's "
     "range: the nearest sample's pose and one solve stepping from it, so a "
     "slider-crank stays on the branch the run found. The reply says "
     "`left_at` (the sample), `left_at_value` and `left_solves`. leave_at "
     "with async is refused: its samples do not exist yet.",
     "study:int>=0, async?:bool, include_poses?:bool, leave_at?:string|int, "
     "leave_at_deg?:number, leave_at_mm?:number, outlive_client?:bool, "
     "recompute?:bool"},
    {"motion_status",
     "{study}: whether a run of THIS study is in progress (with its `job` "
     "id and `fraction` while it is), and the stored result if one has "
     "landed (the same shape run_motion_study returns).",
     "include_poses, study"},
    {"set_motion_pose",
     "Scrub: drive the study's mate to {t} (0..1 along the range) and "
     "re-solve, so the viewport and every query show that pose. No undo "
     "step, like the explode slider — a scrub is a look, not an edit — "
     "EXCEPT that a study naming a configuration first activates it (an "
     "undo step), since its poses only exist under it.",
     "study, t"},
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
     "cancellable) and motion_video_status reads the outcome.",
     "async, format, fps, frames, height, path, quality, study, width",
     remote::NotInBatch},
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

json StudyJson(ui::AppState& state, const AssemblyDocument& asmDoc,
               const MotionStudy& s) {
  json j = DocumentIO::MotionStudyToJson(s);
  const bool rot = s.driverDof == MateDof::Rotation;
  // The document stores the DOF's own unit; the surface speaks degrees for
  // a rotation. Both are shown so nobody has to remember which this is.
  j["unit"] = rot ? "deg" : "mm";
  j["from"] = rot ? angles::ToDegrees(s.from) : s.from;
  j["to"] = rot ? angles::ToDegrees(s.to) : s.to;
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
  // What TURNING changed. A mesh sweep calls touching a clash, and at rest a
  // propeller touches its motor, its nut and four screws — so a study of a
  // working machine is never "clean" and the caller has to work out which
  // contacts are news. That is this: the pairs in contact at the FIRST
  // sample are seated, and anything else that shows up appeared.
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

} // namespace

std::span<const MethodDoc> MotionMethodDocs() { return MotionMethods; }

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
  if (m == "motion_status") {
    AssemblyDocument& asmDoc = NeedAssembly(state);
    const MotionStudy& s = NeedStudy(asmDoc, p);
    const bool includePoses = p.value("include_poses", false);
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
  if (m == "export_motion_video") {
    AssemblyDocument& asmDoc = NeedAssembly(state);
    const MotionStudy& s = NeedStudy(asmDoc, p);
    ui::MotionVideoRequest req;
    req.studyId = s.id;
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
    // Read before the job starts: a mistyped `async` refused AFTER it would
    // leave a video rendering behind an error.
    const bool async = p.value("async", false);
    ui::MotionVideoPlan plan;
    std::string err;
    if (!ui::StartMotionVideo(state, req, &err, &plan))
      throw std::runtime_error(err);
    const auto planJson = [](const ui::MotionVideoPlan& pl) {
      return json{
          {"path", pl.path},
          {"format", pl.format},
          {"frames", pl.frames},
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
      throw std::runtime_error("export_motion_video: " + last->error);
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
